#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/signal.h>

#include <map>
#include <list>

#include "trace-packet.h"

volatile bool terminate;

static void sigint(int sig)
{
	fprintf(stderr, "interrupted\n");
	terminate = true;
}

static void analyse()
{
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "error: usage: %s <trace file>\n", argv[0]);
		return 1;
	}
	
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "error: unable to open file: %s\n", strerror(errno));
		return 1;
	}
	
	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		
		fprintf(stderr, "error: unable to stat file: %s\n", strerror(errno));
		return 1;
	}
	
	signal(SIGINT, sigint);
	
	uint64_t approx_total = st.st_size / sizeof(InstructionTracePacket);
	fprintf(stderr, "estimated number of packets: %lu\n", approx_total);
	
	uint64_t nr_packets = 0;
	uint64_t reuse_tracker = 0;
	do {
		uint8_t type;
		int rc = read(fd, &type, sizeof(type));
		if (rc == 0) break;
		
		lseek(fd, -1, SEEK_CUR);
		
		switch (type) {
		case TRACE_PACKET_FRAME_START:
		case TRACE_PACKET_FRAME_END:
		{
			FrameTracePacket ftp;
			rc = read(fd, &ftp, sizeof(ftp));
			if (rc != sizeof(ftp)) {
				fprintf(stderr, "error: frame packet read error\n");
				terminate = true;
				continue;
			}
						
			break;
		}
		
		case TRACE_PACKET_KERNEL_START:
		case TRACE_PACKET_KERNEL_END:
		{
			KernelTracePacket ktp;
			rc = read(fd, &ktp, sizeof(ktp));
			if (rc != sizeof(ktp)) {
				fprintf(stderr, "error: kernel packet read error\n");
				terminate = true;
				continue;
			}
						
			break;
		}
		
		case TRACE_PACKET_INSTRUCTION:
		{
			InstructionTracePacket itp;
			rc = read(fd, &itp, sizeof(itp));
			if (rc != sizeof(itp)) {
				fprintf(stderr, "error: instruction packet read error\n");
				terminate = true;
				continue;
			}
			
			break;
		}
		
		default:
			fprintf(stderr, "error: unknown log packet type: %d\n", type);
				terminate = true;
				continue;
		}
		
		nr_packets++;
		
		if ((nr_packets % 1000000) == 0) {
			fprintf(stderr, "processed %lu packets (approx. %lu%%)\n", nr_packets, (nr_packets * 100) / approx_total);
		}
		
	} while(!terminate);
	
	analyse();
	
	close(fd);
	return 0;
}

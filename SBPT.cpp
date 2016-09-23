#include "pin.H"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <list>
#include <time.h>

static uint64_t now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	
	return tv.tv_sec * 1e6 + tv.tv_usec;
}

struct KernelRoutineDescriptor
{
	KernelRoutineDescriptor() : count(0), executing(false) { }
	
	uint64_t count;
	std::string name;
	
	std::list<uint64_t> runtimes;
	
	bool executing;
	uint64_t start_time;
};

struct FrameDescriptor
{
	std::list<KernelRoutineDescriptor *> kernels;
};

std::list<KernelRoutineDescriptor *> KernelRoutineDescriptors;
std::list<FrameDescriptor *> FrameDescriptors;

static FrameDescriptor *CurrentFrame;

void FrameStart()
{
	CurrentFrame = new FrameDescriptor();
	FrameDescriptors.push_back(CurrentFrame);
}

void FrameEnd()
{
	CurrentFrame = NULL;
}

void KernelRoutineEnter(KernelRoutineDescriptor *descriptor)
{
	if (descriptor->executing) {
		std::cerr << "ASSERTION FAIL: kernel routine " << descriptor->name << " currently executing" << std::endl;
		exit(0);
	}
	
	if (!CurrentFrame) {
		std::cerr << "ASSERTION FAIL: not in a frame";
		exit(0);
	}
	
	CurrentFrame->kernels.push_back(descriptor);
	
	descriptor->executing = true;
	descriptor->count++;
	descriptor->start_time = now();
}

void KernelRoutineExit(KernelRoutineDescriptor *descriptor)
{
	uint64_t stop_time = now();
	
	if (!descriptor->executing) {
		std::cerr << "ASSERTION FAIL: kernel routine not currently executing" << std::endl;
		exit(0);
	}
	
	descriptor->executing = false;
	descriptor->runtimes.push_back(stop_time - descriptor->start_time);
}

void Routine(RTN rtn, void *v)
{
	// std::cerr << "Routine: " << RTN_Name(rtn) << std::endl;
	
	if (RTN_Name(rtn) == "FRAME_START") {
		std::cerr << "Located FRAME_START directive" << std::endl;
		RTN_Open(rtn);
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)FrameStart, IARG_END);
		RTN_Close(rtn);
		return;
	} else if (RTN_Name(rtn) == "FRAME_END") {
		std::cerr << "Located FRAME_END directive" << std::endl;
		RTN_Open(rtn);
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)FrameEnd, IARG_END);
		RTN_Close(rtn);
		return;
	}
	
	SEC sec = RTN_Sec(rtn);
	
	
	// Ignore routines that aren't part of the ".kernel" ELF section
	if (SEC_Name(sec) != ".kernel") return;
	
	std::cerr << "Identified kernel routine: " << RTN_Name(rtn) << std::endl;
	
	KernelRoutineDescriptor *descriptor = new KernelRoutineDescriptor();
	descriptor->name = RTN_Name(rtn);
	KernelRoutineDescriptors.push_back(descriptor);
	
	RTN_Open(rtn);
	RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)KernelRoutineEnter, IARG_PTR, descriptor, IARG_END);
	RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)KernelRoutineExit, IARG_PTR, descriptor, IARG_END);
	RTN_Close(rtn);
}

void Fini(INT32 code, void *v)
{
	std::cerr << std::endl;
	std::cerr << "*** SLAMBench Completed ***" << std::endl;
	
	uint64_t total_executions = 0;
	for (auto descriptor : KernelRoutineDescriptors) {
		total_executions += descriptor->count;
	}

	uint64_t total_runtime = 0;
	for (auto descriptor : KernelRoutineDescriptors) {
		uint64_t kernel_total_runtime = 0;
		for (auto runtime : descriptor->runtimes) {
			kernel_total_runtime += runtime;
		}
		
		total_runtime += kernel_total_runtime;
		
		double runtime_average = (double)kernel_total_runtime / descriptor->runtimes.size();
		
		std::cerr << "Kernel: " << descriptor->name.c_str() << ":" << std::endl
		<< "  Execution Count: " << descriptor->count << " (" << std::setprecision(2) << (((double)descriptor->count / (double)total_executions) * 100.0) << "%) " << std::endl
		<< "    Total Runtime: " << kernel_total_runtime / 1000 << "ms" << std::endl
		<< "  Average Runtime: " << std::setprecision(5) << (runtime_average / 1000) << "ms" << std::endl
		<< std::endl;
		
		delete descriptor;
	}
	
	KernelRoutineDescriptors.clear();
	
	std::cerr << "Total Execution Count: " << total_executions << std::endl
			  << "        Total Runtime: " << (total_runtime / 1000) << "ms" << std::endl;
	
	std::cerr << "Total Frames: " << FrameDescriptors.size();
}

int main(int argc, char *argv[])
{
	PIN_InitSymbols();

	if (PIN_Init(argc, argv)) {
		std::cerr << "This is the SLAMBench pin tool" << std::endl;
		std::cerr << KNOB_BASE::StringKnobSummary();
		std::cerr << std::endl;

		return 1;
	}
	
	RTN_AddInstrumentFunction(Routine, NULL);
	PIN_AddFiniFunction(Fini, NULL);

	PIN_StartProgram();
	return 0;
}

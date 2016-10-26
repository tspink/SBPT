#ifndef TRACE_PACKET_H
#define TRACE_PACKET_H

#define __trace_packed __attribute__((packed))

struct TracePacket
{
	uint8_t Type;
} __trace_packed;

struct InstructionTracePacket : public TracePacket
{
	uint64_t RIP;
	uint32_t Opcode;
} __trace_packed;

struct KernelTracePacket : public TracePacket
{
	uint32_t ID;
} __trace_packed;

struct FrameTracePacket : public TracePacket
{
	uint32_t ID;
} __trace_packed;

#define TRACE_PACKET_KERNEL_START	0
#define TRACE_PACKET_KERNEL_END		1
#define TRACE_PACKET_FRAME_START	2
#define TRACE_PACKET_FRAME_END		3
#define TRACE_PACKET_INSTRUCTION	4

#endif /* TRACE_PACKET_H */


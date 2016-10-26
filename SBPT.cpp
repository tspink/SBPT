#include "pin.H"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <time.h>

KNOB<bool> KnobTraceMemory(KNOB_MODE_WRITEONCE, "pintool", "trace_mem", "0", "Should trace memory");
KNOB<bool> KnobTraceReuse(KNOB_MODE_WRITEONCE, "pintool", "trace_reuse", "0", "Should trace reuses");
KNOB<bool> KnobTraceTimes(KNOB_MODE_WRITEONCE, "pintool", "trace_timing", "0", "Should trace times");
KNOB<bool> KnobTraceKInst(KNOB_MODE_WRITEONCE, "pintool", "trace_kinst", "0", "Should trace kernel instructions");
KNOB<bool> KnobTraceSeq(KNOB_MODE_WRITEONCE, "pintool", "trace_seq", "0", "Should trace instruction sequences");

static uint64_t now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	
	return tv.tv_sec * 1e6 + tv.tv_usec;
}

struct Average
{
	Average() : Value(0), DataPoints(0) { }
	
	void Add(double new_value) {
		if (!DataPoints) {
			Value = new_value;
		} else {
			Value = Value * ((double)(DataPoints - 1) / (double)DataPoints) + (new_value / DataPoints);
		}
		
		DataPoints++;
	}
	
	double Value;
	uint64_t DataPoints;
};

struct MemoryZone
{
	uint64_t TotalReads, TotalWrites;
	std::unordered_map<uint64_t, uint64_t> AddressAccesses, AddressReads, AddressWrites;

	Average AverageReuse, AverageReuseDistance;
	uint64_t MaxReuseDistance;
};

struct MemoryStatistics
{
	MemoryZone StackZone, HeapZone, DataZone;	
};

struct MemoryInstruction
{
	uint64_t RIP;
	uint64_t LastTouch;
};

struct KernelMemoryInstruction
{
	uint64_t LastAddress;
	std::set<int64_t> AddressDifferences;
};

struct KernelDescriptor
{
	KernelDescriptor(int _id, std::string _name) : ID(_id), Name(_name), TotalExecutionCount(0), TotalExecutionTime(0) { }
	
	int ID;
	std::string Name;
	uint64_t TotalExecutionCount;
	uint64_t TotalExecutionTime;
	
	std::unordered_map<uintptr_t, KernelMemoryInstruction *> MemoryInstructions;
};

struct InstructionExecution
{
	uint64_t RIP;
	uint32_t Opcode;
};

struct KernelInvocation
{
	KernelInvocation(KernelDescriptor *descriptor) : Descriptor(descriptor), Duration(0) { }
	
	KernelDescriptor *Descriptor;
	uint64_t Duration;
	
	std::list<InstructionExecution *> Instructions;
};

struct FrameDescriptor
{
	std::list<KernelInvocation *> KernelInvocations;
	uint32_t Index;
	uint64_t Duration;
};

#define VMA_TYPE_DATA  0
#define VMA_TYPE_STACK 1
#define VMA_TYPE_HEAP  2

struct VMA
{
	uint64_t Start, End;
	uint8_t Type;
};

std::list<KernelDescriptor *> KernelDescriptors;
std::list<FrameDescriptor *> FrameDescriptors;
std::map<uint64_t, VMA> VMAs;

static MemoryStatistics MemoryStats;

static FrameDescriptor *CurrentFrame;
static KernelInvocation *CurrentKernel;
static int NextKernelID;

static int CurrentFrameIndex;

#include "trace-packet.h"

FILE *TraceFile;

void FrameStart()
{
	ASSERT(!CurrentFrame, "A frame is already in progress");
	
	CurrentFrame = new FrameDescriptor();
	CurrentFrame->Index = CurrentFrameIndex++;
	CurrentFrame->Duration = now();
	
	if (TraceFile) {
		FrameTracePacket ftp;
		ftp.Type = TRACE_PACKET_FRAME_START;
		fwrite(&ftp, sizeof(ftp), 1, TraceFile);
	}
}

void FrameEnd()
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	
	CurrentFrame->Duration = now() - CurrentFrame->Duration;
	
	FrameDescriptors.push_back(CurrentFrame);
	CurrentFrame = NULL;
	
	if (TraceFile) {
		FrameTracePacket ftp;
		ftp.Type = TRACE_PACKET_FRAME_END;
		fwrite(&ftp, sizeof(ftp), 1, TraceFile);
	}
}

void KernelRoutineEnter(KernelDescriptor *descriptor)
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	ASSERT(!CurrentKernel, "A kernel is already in progress");

	CurrentKernel = new KernelInvocation(descriptor);
	CurrentKernel->Duration = now();
	
	if (TraceFile) {
		KernelTracePacket ktp;
		ktp.Type = TRACE_PACKET_KERNEL_START;
		ktp.ID = CurrentKernel->Descriptor->ID;
		fwrite(&ktp, sizeof(ktp), 1, TraceFile);
	}
}

void KernelRoutineExit(KernelDescriptor *descriptor)
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	ASSERT(CurrentKernel, "A kernel is not in progress");
	
	CurrentKernel->Duration = now() - CurrentKernel->Duration;
	CurrentFrame->KernelInvocations.push_back(CurrentKernel);
	
	if (TraceFile) {
		KernelTracePacket ktp;
		ktp.Type = TRACE_PACKET_KERNEL_END;
		ktp.ID = CurrentKernel->Descriptor->ID;
		fwrite(&ktp, sizeof(ktp), 1, TraceFile);
	}
	
	CurrentKernel->Descriptor->TotalExecutionCount++;
	CurrentKernel->Descriptor->TotalExecutionTime += CurrentKernel->Duration;
	CurrentKernel = NULL;	
}

static uintptr_t ReuseQueue[4096];
static uint64_t ReuseQueueSize, LastTimepoint;

void MemoryAccessCommon(uintptr_t addr, MemoryZone& zone, MemoryInstruction& mi)
{
	mi.LastTouch = zone.TotalReads + zone.TotalWrites;
	
	if (CurrentKernel) {
		auto& kmi = CurrentKernel->Descriptor->MemoryInstructions[(uintptr_t)&mi];		
		if (!kmi) kmi = new KernelMemoryInstruction();

		if (kmi->LastAddress) {
			int64_t delta = (int64_t)addr - (int64_t)kmi->LastAddress;
			kmi->AddressDifferences.insert(delta);
		}
		
		kmi->LastAddress = addr;
	}
		
	if (KnobTraceReuse.Value()) {
		bool found = false;
		unsigned int index;
		for (index = 0; index < ReuseQueueSize; index++) {
			if (ReuseQueue[index] == addr) {
				found = true;
				break;
			}
		}

		if (found) {
			uint64_t distance = ReuseQueueSize - index;
			if (distance > zone.MaxReuseDistance)
				zone.MaxReuseDistance = distance;

			zone.AverageReuseDistance.Add(distance);
			ReuseQueueSize = 0;
		} else {
			ReuseQueue[ReuseQueueSize++] = addr;
			if (ReuseQueueSize >= 4096) ReuseQueueSize = 0;
		}
	}
	
	zone.AddressAccesses[addr]++;
	
	if (((zone.TotalWrites + zone.TotalReads) % 1048576) == 0) {
		uint64_t delta = now() - LastTimepoint;
		std::cerr << "Processed " << std::dec << (zone.TotalWrites + zone.TotalReads) << " accesses (" << (uint64_t)(1000000.0 / (delta / 1e6)) << " APS)" << std::endl;
		LastTimepoint = now();
	}
}

static inline MemoryZone& ClassifyAddress(uintptr_t addr)
{
	auto vma = VMAs.lower_bound(addr);
	if (vma == VMAs.end())
		return MemoryStats.HeapZone;
	
	if (addr < vma->second.End) {
		if (vma->second.Type == VMA_TYPE_STACK)
			return MemoryStats.StackZone;
		else
			return MemoryStats.DataZone;
	} else {
		return MemoryStats.HeapZone;
	}
}

void MemoryReadInstruction(void *rip, uintptr_t addr, MemoryInstruction *mi)
{
	MemoryZone& zone = ClassifyAddress(addr);
	
	zone.TotalReads++;
	zone.AddressReads[addr]++;
	
	MemoryAccessCommon(addr, zone, *mi);
}

void MemoryWriteInstruction(void *rip, uintptr_t addr, MemoryInstruction *mi)
{
	MemoryZone& zone = ClassifyAddress(addr);
	
	zone.TotalWrites++;
	zone.AddressWrites[addr]++;
	
	MemoryAccessCommon(addr, zone, *mi);
}

void InstructionExecuted(VOID *rip, uint32_t opcode)
{
	if (!CurrentKernel) return;
	
	if (TraceFile) {
		InstructionTracePacket itp;
		itp.Type = TRACE_PACKET_INSTRUCTION;
		itp.Opcode = opcode;
		itp.RIP = (uint64_t)rip;
	
		fwrite(&itp, sizeof(InstructionTracePacket), 1, TraceFile);
	}
}

std::map<std::string, std::string> KernelNameMap;

void Routine(RTN rtn, VOID *v)
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
	
	std::string name;
	
	auto friendly = KernelNameMap.find(RTN_Name(rtn));
	if (friendly == KernelNameMap.end()) {
		name = RTN_Name(rtn);
	} else {
		name = friendly->second;
	}
	
	KernelDescriptor *descriptor = new KernelDescriptor(NextKernelID++, name);
	KernelDescriptors.push_back(descriptor);
	
	RTN_Open(rtn);
	RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)KernelRoutineEnter, IARG_PTR, descriptor, IARG_END);
	RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)KernelRoutineExit, IARG_PTR, descriptor, IARG_END);
	RTN_Close(rtn);
}

void Instruction(INS ins, VOID *p)
{
	if (KnobTraceMemory.Value()) {
		unsigned int operand_count = INS_MemoryOperandCount(ins);
		if (operand_count > 0) {
			MemoryInstruction *mi = new MemoryInstruction();
			mi->RIP = INS_Address(ins);
			mi->LastTouch = 0;

			for (unsigned int operand_index = 0; operand_index < operand_count; operand_index++) {
				if (INS_MemoryOperandIsRead(ins, operand_index)) {
					INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryReadInstruction, IARG_INST_PTR, IARG_MEMORYOP_EA, operand_index, IARG_PTR, (VOID *)mi, IARG_END);
				}

				if (INS_MemoryOperandIsWritten(ins, operand_index)) {
					INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryWriteInstruction, IARG_INST_PTR, IARG_MEMORYOP_EA, operand_index, IARG_PTR, (VOID *)mi, IARG_END);
				}
			}
		}
	}
	
	if (KnobTraceKInst.Value()) {
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)InstructionExecuted, IARG_INST_PTR, IARG_PTR, (uint64_t)INS_Opcode(ins), IARG_END);
	}
}

void Fini(INT32 code, void *v)
{
	if (TraceFile)
		fclose(TraceFile);
	
	std::cerr << std::endl;
	std::cerr << "*** SLAMBench Completed ***" << std::endl;
	
	if (KnobTraceMemory.Value()) {
		for (auto descriptor : KernelDescriptors) {
			std::cerr << "Kernel: " << descriptor->Name << std::endl;

			if (descriptor->MemoryInstructions.size() > 0) {
				std::set<int64_t> strides;

				uint64_t nr_one_stride = 0, nr_two_stride = 0;
				for (auto mi : descriptor->MemoryInstructions) {
					for (auto stride : mi.second->AddressDifferences) {
						strides.insert(stride);
					}

					uint64_t unique_strides = mi.second->AddressDifferences.size();
					if (unique_strides == 1) {
						nr_one_stride++;
					} else if (unique_strides == 2) {
						nr_two_stride++;
					}
				}

				std::cerr << "  % of memory instructions with only one unique stride: " << ((nr_one_stride * 100)/descriptor->MemoryInstructions.size()) << std::endl;
				std::cerr << "      % of memory instructions with two unique strides: " << ((nr_two_stride * 100)/descriptor->MemoryInstructions.size()) << std::endl;
				/*std::cerr << " Seen strides:";
				for (auto stride : strides) {
					std::cerr << " " << stride;
				}
				std::cerr << std::endl;*/
			}
		}
	
		std::cerr << "Memory Statistics:" << std::endl;

		MemoryZone& zone1 = MemoryStats.DataZone;
		std::cerr << "*** DATA ***" << std::dec << std::endl;
		std::cerr << "        Total Accesses: Reads=" << zone1.TotalReads << ", Writes=" << zone1.TotalWrites << ", Total=" << (zone1.TotalReads + zone1.TotalWrites) << std::endl;
		std::cerr << "     Distinct Accesses: Reads=" << zone1.AddressReads.size() << ", Writes=" << zone1.AddressWrites.size() << ", Total=" << (zone1.AddressReads.size() + zone1.AddressWrites.size()) << std::endl;

		std::cerr << "Average Reuse Distance: " << zone1.AverageReuseDistance.Value << std::endl;
		std::cerr << "   Max. Reuse Distance: " << zone1.MaxReuseDistance << std::endl;

		for (auto access : zone1.AddressAccesses) {
			zone1.AverageReuse.Add(access.second);
		}

		std::cerr << "         Average Reuse: " << zone1.AverageReuse.Value << std::endl << std::endl;

		MemoryZone& zone2 = MemoryStats.StackZone;
		std::cerr << "*** STACK ***" << std::endl;
		std::cerr << "        Total Accesses: Reads=" << zone2.TotalReads << ", Writes=" << zone2.TotalWrites << ", Total=" << (zone2.TotalReads + zone2.TotalWrites) << std::endl;
		std::cerr << "     Distinct Accesses: Reads=" << zone2.AddressReads.size() << ", Writes=" << zone2.AddressWrites.size() << ", Total=" << (zone2.AddressReads.size() + zone2.AddressWrites.size()) << std::endl;

		std::cerr << "Average Reuse Distance: " << zone2.AverageReuseDistance.Value << std::endl;
		std::cerr << "   Max. Reuse Distance: " << zone2.MaxReuseDistance << std::endl;

		for (auto access : zone2.AddressAccesses) {
			zone2.AverageReuse.Add(access.second);
		}

		std::cerr << "         Average Reuse: " << zone2.AverageReuse.Value << std::endl << std::endl;

		MemoryZone& zone3 = MemoryStats.HeapZone;
		std::cerr << "*** HEAP ***" << std::endl;
		std::cerr << "        Total Accesses: Reads=" << zone3.TotalReads << ", Writes=" << zone3.TotalWrites << ", Total=" << (zone3.TotalReads + zone3.TotalWrites) << std::endl;
		std::cerr << "     Distinct Accesses: Reads=" << zone3.AddressReads.size() << ", Writes=" << zone3.AddressWrites.size() << ", Total=" << (zone3.AddressReads.size() + zone3.AddressWrites.size()) << std::endl;

		std::cerr << "Average Reuse Distance: " << zone3.AverageReuseDistance.Value << std::endl;
		std::cerr << "   Max. Reuse Distance: " << zone3.MaxReuseDistance << std::endl;

		for (auto access : zone3.AddressAccesses) {
			zone3.AverageReuse.Add(access.second);
		}

		std::cerr << "         Average Reuse: " << zone3.AverageReuse.Value << std::endl << std::endl;
	}
	
	if (KnobTraceTimes.Value()) {
		uint64_t all_kernel_executions = 0;
		for (auto descriptor : KernelDescriptors) {
			all_kernel_executions += descriptor->TotalExecutionCount;
		}

		uint64_t all_kernel_runtimes = 0;
		for (auto descriptor : KernelDescriptors) {
			all_kernel_runtimes += descriptor->TotalExecutionTime;
		}

		for (auto descriptor : KernelDescriptors) {
			if (descriptor->TotalExecutionCount == 0) continue;

			all_kernel_runtimes += descriptor->TotalExecutionTime;

			double runtime_average = (double)descriptor->TotalExecutionTime / descriptor->TotalExecutionCount;

			std::cerr << "Kernel: " << descriptor->ID << ": " << descriptor->Name << ":" << std::endl
			<< "  Execution Count: " << descriptor->TotalExecutionCount << " (" << std::setprecision(2) << (((double)descriptor->TotalExecutionCount / (double)all_kernel_executions) * 100.0) << "%) " << std::endl
			<< "    Total Runtime: " << (descriptor->TotalExecutionTime / 1000) << "ms (" << std::setprecision(2) << (((double)descriptor->TotalExecutionTime / all_kernel_runtimes) * 100) << "%)" << std::endl
			<< "  Average Runtime: " << std::setprecision(5) << (runtime_average / 1000) << "ms" << std::endl
			<< std::endl;
		}

		std::cerr << "Total Execution Count: " << all_kernel_executions << std::endl
				  << "        Total Runtime: " << (all_kernel_runtimes / 1000) << "ms" << std::endl;

		std::cerr << std::endl;

		std::cerr << "Total Frames: " << FrameDescriptors.size() << std::endl;

		uint64_t all_frame_times = 0;
		for (auto frame : FrameDescriptors) {
			all_frame_times += frame->Duration;
		}

		std::cerr << "Average Frame Duration: " << (((double)all_frame_times /  FrameDescriptors.size()) / 1000) << "ms" << std::endl;
		std::cerr << "Average Throughput: " << ((double)FrameDescriptors.size() / (all_frame_times / 1e6)) << " FPS" << std::endl;

		int index = 0;
		for (auto frame : FrameDescriptors) {
			/*std::cerr << "FRAME " << std::setw(3) << index++ << ": ";

			std::cerr << std::setw(12) << (frame->Duration / 1000);

			std::cerr << " [";

			bool first = true;
			for (auto inv : frame->KernelInvocations) {
				if (first) first = false;
				else std::cerr << ", ";

				std::cerr << inv->Descriptor->ID << ":" << inv->Duration;
			}

			std::cerr << "]" << std::endl;

			std::cerr << "REL [";
			first = true;
			for (auto inv : frame->KernelInvocations) {
				if (first) first = false;
				else std::cerr << ", ";
				std::cerr << inv->Descriptor->ID << ":" << std::setprecision(2) << (((double)inv->Duration / frame->Duration) * 100);
			}
			std::cerr << "]" << std::endl;*/

			std::cerr << index++ << "," << frame->Duration;

			for (auto inv : frame->KernelInvocations) {
				std::cerr << "," << inv->Duration;
				//std::cerr << "," << inv->Descriptor->ID;
			}

			std::cerr << std::endl;
		}
	}
}

static void FindStack()
{
	FILE *maps = fopen("/proc/self/maps", "rt");
	if (!maps) {
		return;
	}

	uint64_t rsp;
	asm volatile("mov %%rsp, %0" : "=r"(rsp));

	while (!feof(maps)) {
		char buffer[512];
		fgets(buffer, sizeof(buffer) - 1, maps);

		VMA vma;
		sscanf(buffer, "%lx-%lx", &vma.Start, &vma.End);

		if (rsp >= vma.Start && rsp < vma.End) {
			std::cerr << "FOUND STACK: " << std::hex << vma.Start << "--" << vma.End << std::endl;
			vma.Type = VMA_TYPE_STACK;
			VMAs[vma.Start] = vma;
			break;
		}
	}

	fclose(maps);
}

void Image(IMG img, VOID *v)
{
	std::cerr << "IMAGE: " << IMG_Name(img) << std::endl;
	
	for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
		if (!SEC_Mapped(sec)) continue;
		
		std::cerr << "  SECTION: " << SEC_Name(sec) << std::endl;
		std::cerr << "  START:" << SEC_Address(sec) << ", SIZE:" << SEC_Size(sec) << std::endl;
		
		VMA vma;
		vma.Start = SEC_Address(sec);
		vma.End   = vma.Start + SEC_Size(sec);
		vma.Type  = VMA_TYPE_DATA;
		
		VMAs[SEC_Address(sec)] = vma;
	}
	
	FindStack();
}

static void LoadFriendlyNames()
{
	KernelNameMap["_Z21bilateralFilterKernelPfPKf23__device_builtin__uint2S1_fi"] = "Bilateral Filter";
	KernelNameMap["_Z18depth2vertexKernelP24__device_builtin__float3PKf23__device_builtin__uint28sMatrix4"] = "Depth2Vertex";
	KernelNameMap["_Z19vertex2normalKernelP24__device_builtin__float3PKS_23__device_builtin__uint2"] = "Vertex2Normal";
	KernelNameMap["_Z12reduceKernelPfP9TrackData23__device_builtin__uint2S2_"] = "Reduce";
	KernelNameMap["_Z11trackKernelP9TrackDataPK24__device_builtin__float3S3_23__device_builtin__uint2S3_S3_S4_8sMatrix4S5_ff"] = "Track";
	KernelNameMap["_Z15mm2metersKernelPf23__device_builtin__uint2PKtS0_"] = "mm2m";
	KernelNameMap["_Z27halfSampleRobustImageKernelPfPKf23__device_builtin__uint2fi"] = "HalfSampleRobustImage";
	KernelNameMap["_Z15integrateKernel6VolumePKf23__device_builtin__uint28sMatrix4S3_ff"] = "Integrate";
	KernelNameMap["_Z13raycastKernelP24__device_builtin__float3S0_23__device_builtin__uint26Volume8sMatrix4ffff"] = "Raycast";
	KernelNameMap["_Z15checkPoseKernelR8sMatrix4S_PKf23__device_builtin__uint2f"] = "CheckPose";
	KernelNameMap["_Z18renderNormalKernelP24__device_builtin__uchar3PK24__device_builtin__float323__device_builtin__uint2"] = "RenderNormal";
	KernelNameMap["_Z17renderDepthKernelP24__device_builtin__uchar4Pf23__device_builtin__uint2ff"] = "RenderDepth";
	KernelNameMap["_Z17renderTrackKernelP24__device_builtin__uchar4PK9TrackData23__device_builtin__uint2"] = "RenderTrack";
	KernelNameMap["_Z18renderVolumeKernelP24__device_builtin__uchar423__device_builtin__uint26Volume8sMatrix4ffff24__device_builtin__float3S4_"] = "RenderVolume";
	KernelNameMap["_Z16updatePoseKernelR8sMatrix4PKff"] = "UpdatePose";
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
	
	LoadFriendlyNames();
	
	IMG_AddInstrumentFunction(Image, NULL);
	RTN_AddInstrumentFunction(Routine, NULL);	
	INS_AddInstrumentFunction(Instruction, NULL);
	
	if (KnobTraceKInst.Value()) {
		TraceFile = fopen("./trace.bin", "wb");
	}
	
	PIN_AddFiniFunction(Fini, NULL);
			
	PIN_StartProgram();
	return 0;
}

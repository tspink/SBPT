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

struct MemoryInstruction
{
	uint64_t RIP;
};

struct KernelDescriptor
{
	KernelDescriptor(int _id, std::string _name) : ID(_id), Name(_name), TotalExecutionCount(0), TotalExecutionTime(0) { }
	
	int ID;
	std::string Name;
	uint64_t TotalExecutionCount;
	uint64_t TotalExecutionTime;
};

struct KernelInvocation
{
	KernelInvocation(KernelDescriptor *descriptor) : Descriptor(descriptor), Duration(0), MaxReuseDistance(0) { }
	
	KernelDescriptor *Descriptor;
	uint64_t Duration;
	
	Average AverageReuse, AverageReuseDistance;
	uint64_t MaxReuseDistance;
	
	std::map<uint64_t, uint64_t> Addresses;
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

static FrameDescriptor *CurrentFrame;
static KernelInvocation *CurrentKernel;
static int NextKernelID;

static int CurrentFrameIndex;

void FrameStart()
{
	ASSERT(!CurrentFrame, "A frame is already in progress");
	
	CurrentFrame = new FrameDescriptor();
	CurrentFrame->Index = CurrentFrameIndex++;
	CurrentFrame->Duration = now();
}

void FrameEnd()
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	
	CurrentFrame->Duration = now() - CurrentFrame->Duration;
	
	FrameDescriptors.push_back(CurrentFrame);
	CurrentFrame = NULL;
}

void KernelRoutineEnter(KernelDescriptor *descriptor)
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	ASSERT(!CurrentKernel, "A kernel is already in progress");

	CurrentKernel = new KernelInvocation(descriptor);
	CurrentKernel->Duration = now();
}

void KernelRoutineExit(KernelDescriptor *descriptor)
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	ASSERT(CurrentKernel, "A kernel is not in progress");
	
	CurrentKernel->Duration = now() - CurrentKernel->Duration;
	CurrentFrame->KernelInvocations.push_back(CurrentKernel);
	
	CurrentKernel->Descriptor->TotalExecutionCount++;
	CurrentKernel->Descriptor->TotalExecutionTime += CurrentKernel->Duration;
	
	if (CurrentFrame->Index >= 5) {
		uint64_t total_accesses = 0;
		for (const auto& addr : CurrentKernel->Addresses) {
			total_accesses += addr.second;
			CurrentKernel->AverageReuse.Add(addr.second);
		}
		
		/*std::cerr << "*** KERNEL: " << CurrentKernel->Descriptor->Name << std::endl;
		std::cerr << "           Avg. Reuse=" << std::dec << CurrentKernel->AverageReuse.Value;
		std::cerr << "  Avg. Reuse Distance=" << std::dec << CurrentKernel->AverageReuseDistance.Value;
		std::cerr << "  Max. Reuse Distance=" << std::dec << CurrentKernel->MaxReuseDistance << std::endl;*/
		
		std::cerr << CurrentKernel->Descriptor->Name << ","
				<< std::dec << CurrentKernel->Addresses.size() << ","
				<< std::dec << total_accesses << ","
				<< std::dec << CurrentKernel->AverageReuse.Value << ","
				<< std::dec << CurrentKernel->AverageReuseDistance.Value << ","
				<< std::dec << CurrentKernel->MaxReuseDistance << std::endl;
				
	}

	CurrentKernel->Addresses.clear();
	
	CurrentKernel = NULL;	
}

static uintptr_t ReuseQueue[4096];
static uint64_t ReuseQueueSize;

void MemoryAccessCommon(uintptr_t addr, MemoryInstruction& mi)
{
	if (!CurrentFrame) return;
	if (!CurrentKernel) return;
	if (CurrentFrame->Index < 5) return;
	
	CurrentKernel->Addresses[addr]++;
	
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
		if (distance > CurrentKernel->MaxReuseDistance)
			CurrentKernel->MaxReuseDistance = distance;

		CurrentKernel->AverageReuseDistance.Add(distance);
		ReuseQueueSize = 0;
	} else {
		ReuseQueue[ReuseQueueSize++] = addr;
		if (ReuseQueueSize >= 4096) ReuseQueueSize = 0;
	}
}

void MemoryReadInstruction(void *rip, uintptr_t addr, MemoryInstruction *mi)
{
	MemoryAccessCommon(addr, *mi);
}

void MemoryWriteInstruction(void *rip, uintptr_t addr, MemoryInstruction *mi)
{
	MemoryAccessCommon(addr, *mi);
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
	
	std::string name;	
	auto friendly = KernelNameMap.find(RTN_Name(rtn));
	if (friendly == KernelNameMap.end()) {
		name = RTN_Name(rtn);
	} else {
		name = friendly->second;
	}
	
		std::cerr << "Identified kernel routine: " << name << std::endl;
	
	KernelDescriptor *descriptor = new KernelDescriptor(NextKernelID++, name);
	KernelDescriptors.push_back(descriptor);
	
	RTN_Open(rtn);
	RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)KernelRoutineEnter, IARG_PTR, descriptor, IARG_END);
	RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)KernelRoutineExit, IARG_PTR, descriptor, IARG_END);
	RTN_Close(rtn);
}

void Instruction(INS ins, VOID *p)
{
	unsigned int operand_count = INS_MemoryOperandCount(ins);
	if (operand_count > 0) {
		MemoryInstruction *mi = new MemoryInstruction();
		mi->RIP = INS_Address(ins);

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

void Fini(INT32 code, void *v)
{
	std::cerr << std::endl;
	std::cerr << "*** SLAMBench Completed ***" << std::endl;
	
	/*if (KnobTraceMemory.Value()) {
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
				/ *std::cerr << " Seen strides:";
				for (auto stride : strides) {
					std::cerr << " " << stride;
				}
				std::cerr << std::endl;* /
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
	}*/
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
		
	PIN_AddFiniFunction(Fini, NULL);
			
	PIN_StartProgram();
	return 0;
}

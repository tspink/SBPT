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
#include <sys/time.h>

extern "C" {
#include <d4.h>
}

d4cache *mm, *l2, *l1d;

static uint64_t now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	
	return tv.tv_sec * 1e6 + tv.tv_usec;
}

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
	KernelInvocation(KernelDescriptor *descriptor) : Descriptor(descriptor), Duration(0) { }
	
	KernelDescriptor *Descriptor;
	uint64_t Duration;
};

struct FrameDescriptor
{
	std::list<KernelInvocation *> KernelInvocations;
	uint32_t Index;
	uint64_t Duration;
};

std::list<KernelDescriptor *> KernelDescriptors;
std::list<FrameDescriptor *> FrameDescriptors;

static FrameDescriptor *CurrentFrame;
static KernelInvocation *CurrentKernel;
static int NextKernelID;

static int CurrentFrameIndex;

#define SKIP_FRAME 5

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

static void DumpCacheStats()
{
	fprintf(stderr, "l1d: read:  accesses=%lu, hits=%lu, misses=%lu\n", (uint64_t)l1d->fetch[D4XREAD], (uint64_t)l1d->fetch[D4XREAD] - (uint64_t)l1d->miss[D4XREAD], (uint64_t)l1d->miss[D4XREAD]);
	fprintf(stderr, "l1d: write: accesses=%lu, hits=%lu, misses=%lu\n", (uint64_t)l1d->fetch[D4XWRITE], (uint64_t)l1d->fetch[D4XWRITE] - (uint64_t)l1d->miss[D4XWRITE], (uint64_t)l1d->miss[D4XWRITE]);

	fprintf(stderr, "l2:  read:  accesses=%lu, hits=%lu, misses=%lu\n", (uint64_t)l2->fetch[D4XREAD], (uint64_t)l2->fetch[D4XREAD] - (uint64_t)l2->miss[D4XREAD], (uint64_t)l2->miss[D4XREAD]);
	fprintf(stderr, "l2:  write: accesses=%lu, hits=%lu, misses=%lu\n", (uint64_t)l2->fetch[D4XWRITE], (uint64_t)l2->fetch[D4XWRITE] - (uint64_t)l2->miss[D4XWRITE], (uint64_t)l2->miss[D4XWRITE]);
	
	fprintf(stderr, "mem: read:  accesses=%lu, hits=%lu, misses=%lu\n", (uint64_t)mm->fetch[D4XREAD], (uint64_t)mm->fetch[D4XREAD] - (uint64_t)mm->miss[D4XREAD], (uint64_t)mm->miss[D4XREAD]);
	fprintf(stderr, "mem: write: accesses=%lu, hits=%lu, misses=%lu\n", (uint64_t)mm->fetch[D4XWRITE], (uint64_t)mm->fetch[D4XWRITE] - (uint64_t)mm->miss[D4XWRITE], (uint64_t)mm->miss[D4XWRITE]);
}

void KernelRoutineExit(KernelDescriptor *descriptor)
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	ASSERT(CurrentKernel, "A kernel is not in progress");
	
	CurrentKernel->Duration = now() - CurrentKernel->Duration;
	CurrentFrame->KernelInvocations.push_back(CurrentKernel);
	
	CurrentKernel->Descriptor->TotalExecutionCount++;
	CurrentKernel->Descriptor->TotalExecutionTime += CurrentKernel->Duration;
	
	if (CurrentFrame->Index >= SKIP_FRAME) {
		fprintf(stderr, "*** KERNEL: %s\n", CurrentKernel->Descriptor->Name.c_str());
		DumpCacheStats();
		fprintf(stderr, "************\n");
	}

	CurrentKernel = NULL;
}

static void MemoryAccessCommon(uintptr_t addr, bool read)
{
	d4memref memref;
	memref.address = (d4addr)addr;
	memref.size = 4;
	memref.accesstype = read ? D4XREAD : D4XWRITE;

	d4ref(l1d, memref);
}

void MemoryReadInstruction(void *rip, uintptr_t addr)
{
	if (!CurrentKernel) return;
	if (CurrentFrame->Index < SKIP_FRAME) return;

	MemoryAccessCommon(addr, true);
}

void MemoryWriteInstruction(void *rip, uintptr_t addr)
{
	if (!CurrentKernel) return;
	if (CurrentFrame->Index < SKIP_FRAME) return;
	
	MemoryAccessCommon(addr, false);
}

std::map<std::string, std::string> KernelNameMap;

void Routine(RTN rtn, VOID *v)
{
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
		for (unsigned int operand_index = 0; operand_index < operand_count; operand_index++) {
			if (INS_MemoryOperandIsRead(ins, operand_index)) {
				INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryReadInstruction, IARG_INST_PTR, IARG_MEMORYOP_EA, operand_index, IARG_END);
			}

			if (INS_MemoryOperandIsWritten(ins, operand_index)) {
				INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryWriteInstruction, IARG_INST_PTR, IARG_MEMORYOP_EA, operand_index, IARG_END);
			}
		}
	}
}

void Fini(INT32 code, void *v)
{
	std::cerr << std::endl;
	std::cerr << "*** SLAMBench Completed ***" << std::endl;
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

static void InitCache()
{
	mm = d4new(NULL);
	mm->name = (char *)"memory";
	
	l2 = d4new(mm);
	l2->name = (char *)"l2";
	l2->flags = 0;
		
	l2->lg2blocksize = 6;
	l2->lg2subblocksize = 6;
	l2->lg2size = 20;
	l2->assoc = 8;
	
	l2->replacementf = d4rep_lru;
	l2->name_replacement = (char *)"LRU";
	
	l2->prefetchf = d4prefetch_none;
	l2->name_prefetch = (char *)"demand only";
	
	l2->wallocf = d4walloc_never;
	l2->name_walloc = (char *)"never";
	
	l2->wbackf = d4wback_never;
	l2->name_wback = (char *)"never";
	
	l2->prefetch_distance = 6;
	l2->prefetch_abortpercent = 0;
	
	l1d = d4new(l2);
	l1d->name = (char *)"l1d";
	l1d->flags = 0;
		
	l1d->lg2blocksize = 6;
	l1d->lg2subblocksize = 6;
	l1d->lg2size = 15;
	l1d->assoc = 4;
	
	l1d->replacementf = d4rep_random;
	l1d->name_replacement = (char *)"random";
	
	l1d->prefetchf = d4prefetch_none;
	l1d->name_prefetch = (char *)"demand only";
	
	l1d->wallocf = d4walloc_never;
	l1d->name_walloc = (char *)"never";
	
	l1d->wbackf = d4wback_never;
	l1d->name_wback = (char *)"never";
	
	l1d->prefetch_distance = 6;
	l1d->prefetch_abortpercent = 0;
	
	int err = d4setup();
	if (err) {
		fprintf(stderr, "ERROR: %d\n", err);
		_exit(-1);
	}
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
	
	InitCache();
	
	LoadFriendlyNames();
	
	RTN_AddInstrumentFunction(Routine, NULL);	
	INS_AddInstrumentFunction(Instruction, NULL);

	PIN_AddFiniFunction(Fini, NULL);			
	PIN_StartProgram();
	
	return 0;
}

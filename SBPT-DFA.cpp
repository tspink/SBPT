#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <list>
#include <map>
#include <set>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <time.h>

#include "pin.H"

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
	uint64_t LastAddr;
	uint8_t Size;
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
	KernelInvocation(KernelDescriptor *descriptor) : Descriptor(descriptor), Duration(0) { }
	
	KernelInvocation *Previous;
	KernelDescriptor *Descriptor;
	uint64_t Duration;
	
	std::set<uint64_t> AddressesWrittenTo;
	std::set<uint64_t> AddressesReadFrom;
	std::map<KernelInvocation *, uint64_t> RAW;
};

struct FrameDescriptor
{
	FrameDescriptor() : Index(0), Duration(0), LastKI(NULL) { }
	
	std::list<KernelInvocation *> KernelInvocations;
	uint32_t Index;
	uint64_t Duration;
	
	KernelInvocation *LastKI;
};

std::list<KernelDescriptor *> KernelDescriptors;
std::list<FrameDescriptor *> FrameDescriptors;

static FrameDescriptor *CurrentFrame;
static KernelInvocation *CurrentKernel;
static int NextKernelID;

static int CurrentFrameIndex;

/**
 * Called when SLAMBENCH starts a frame
 */
void FrameStart()
{
	ASSERT(!CurrentFrame, "A frame is already in progress");
	
	CurrentFrame = new FrameDescriptor();
	CurrentFrame->Index = CurrentFrameIndex++;
	CurrentFrame->Duration = now();
}

/**
 * Called when SLAMBENCH completes a frame
 */
void FrameEnd()
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	
	CurrentFrame->Duration = now() - CurrentFrame->Duration;
	
	FrameDescriptors.push_back(CurrentFrame);
	CurrentFrame = NULL;
}

/**
 * Called when a kernel routine begins.
 */
void KernelRoutineEnter(KernelDescriptor *descriptor)
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	ASSERT(!CurrentKernel, "A kernel is already in progress");

	CurrentKernel = new KernelInvocation(descriptor);
	CurrentKernel->Duration = now();
	CurrentKernel->Previous = CurrentFrame->LastKI;
}

/**
 * Called when a kernel routine ends.
 */
void KernelRoutineExit(KernelDescriptor *descriptor)
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	ASSERT(CurrentKernel, "A kernel is not in progress");
	
	CurrentKernel->Duration = now() - CurrentKernel->Duration;
	CurrentFrame->KernelInvocations.push_back(CurrentKernel);
	
	CurrentKernel->Descriptor->TotalExecutionCount++;
	CurrentKernel->Descriptor->TotalExecutionTime += CurrentKernel->Duration;
	
	CurrentFrame->LastKI = CurrentKernel;
	CurrentKernel = NULL;
}

bool MemoryAccessCommon(uintptr_t addr, MemoryInstruction& mi)
{
	if (!CurrentFrame) return false;
	if (!CurrentKernel) return false;
	
	return true;
}

void MemoryReadInstruction(void *rip, uintptr_t addr, MemoryInstruction *mi)
{
	if (!MemoryAccessCommon(addr, *mi)) return;
	
	/*if (CurrentKernel->AddressesReadFrom.count(addr) > 0) {
		return;
	}
	
	CurrentKernel->AddressesReadFrom.insert(addr);*/
	
	KernelInvocation *check = CurrentKernel;
	while (check != NULL) {
		if (check->AddressesWrittenTo.count(addr) > 0) {
			CurrentKernel->RAW[check] += mi->Size;
			break;
		}
		
		check = check->Previous;
	}
}

void MemoryWriteInstruction(void *rip, uintptr_t addr, MemoryInstruction *mi)
{
	if (!MemoryAccessCommon(addr, *mi)) return;

	CurrentKernel->AddressesWrittenTo.insert(addr);
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
		MemoryInstruction *mi = new MemoryInstruction();
		mi->RIP = INS_Address(ins);
		mi->LastAddr = 0;

		for (unsigned int operand_index = 0; operand_index < operand_count; operand_index++) {
			mi->Size = INS_MemoryOperandSize(ins, operand_index);
			
			if (INS_MemoryOperandIsRead(ins, operand_index)) {
				INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryReadInstruction,
						IARG_INST_PTR,
						IARG_MEMORYOP_EA, operand_index,
						IARG_PTR, (VOID *)mi,
						IARG_END);
			}

			if (INS_MemoryOperandIsWritten(ins, operand_index)) {
				INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryWriteInstruction,
						IARG_INST_PTR,
						IARG_MEMORYOP_EA, operand_index,
						IARG_PTR, (VOID *)mi,
						IARG_END);
			}
		}
	}
}

static void DumpControlFlow()
{
	for (const auto& frame : FrameDescriptors) {
		std::stringstream cfg_fname, dfg_fname;
		cfg_fname << "frame-" << frame->Index << ".cfg.dot";
		dfg_fname << "frame-" << frame->Index << ".dfg.dot";
		
		std::ofstream cfg(cfg_fname.str().c_str());
		
		cfg << "digraph a { " << std::endl;
		
		for (const auto& kernel : KernelDescriptors) {
			if (kernel->TotalExecutionCount > 0) {
				cfg << "K" << kernel->ID << " [label=\"" << kernel->Name << "\"];" << std::endl;
			} else {
				cfg << "K" << kernel->ID << " [label=\"" << kernel->Name << "\", color=\"red\"];" << std::endl;
			}
		}
		
		cfg << "ZZ [label=\"Frame Start\"];" << std::endl;
		
		const KernelInvocation *last = NULL;
		for (const auto& kernel : frame->KernelInvocations) {
			if (last) {
				cfg << "K" << last->Descriptor->ID << " -> K" << kernel->Descriptor->ID << ";" << std::endl;
			} else {
				cfg << "ZZ -> K" << kernel->Descriptor->ID << std::endl;
			}
			
			last = kernel;
		}

		cfg << "}" << std::endl;
		
		std::ofstream dfg(dfg_fname.str().c_str());
		
		dfg << "digraph a { " << std::endl;
		
		for (const auto& kernel : frame->KernelInvocations) {
			dfg << "K" << (void *)kernel << " [label=\"" << kernel->Descriptor->Name << "\"];" << std::endl;
			
			if (kernel->Previous) {
				dfg << "K" << (void *)kernel->Previous << " -> K" << (void *)kernel << " [color=\"blue\"];" << std::endl;
			}
			
			for (const auto& dep : kernel->RAW) {
				if (dep.second > 1024768) {
					dfg << "K" << (void *)kernel << " -> K" << (void *)dep.first << " [color=\"red\",label=\"" << (uint64_t)(dep.second / 1024768) << "Mb\"];" << std::endl;
				} else if (dep.second > 1024) {
					dfg << "K" << (void *)kernel << " -> K" << (void *)dep.first << " [color=\"red\",label=\"" << (uint64_t)(dep.second / 1024) << "kb\"];" << std::endl;
				} else {
					dfg << "K" << (void *)kernel << " -> K" << (void *)dep.first << " [color=\"red\",label=\"" << (uint64_t)(dep.second) << "b\"];" << std::endl;
				}
			}
		}
		
		dfg << "}" << std::endl;
	}
}

void Fini(INT32 code, void *v)
{
	std::cerr << std::endl;
	std::cerr << "*** SLAMBench Completed ***" << std::endl;
	
	DumpControlFlow();
}

void Image(IMG img, VOID *v)
{
	std::cerr << "IMAGE: " << IMG_Name(img) << std::endl;
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

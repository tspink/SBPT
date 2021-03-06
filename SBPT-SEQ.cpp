#include "pin.H"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <list>
#include <map>
#include <string>
#include <sstream>
#include <set>
#include <unordered_map>
#include <time.h>
#include <sys/time.h>

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

struct SequenceNode
{
	SequenceNode() : Count(0) { }
	
	uint64_t Opcode;
	uint64_t Count;
	
	std::map<uint64_t, SequenceNode> Children;
};

struct KernelInvocation
{
	KernelInvocation(KernelDescriptor *descriptor) : Index(0), Descriptor(descriptor), Duration(0) { }
	
	uint64_t Index;
	
	KernelDescriptor *Descriptor;
	uint64_t Duration;
	
	std::vector<uint64_t> CurrentSequence;
	SequenceNode Root;
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
	CurrentKernel->Index = CurrentFrame->KernelInvocations.size();
	CurrentKernel->Duration = now();
	CurrentKernel->Root.Opcode = 0;
	
	/*for (uint64_t i = 0; i < XED_ICLASS_LAST; i++) {
		DictionaryEntry e;
		e.Sequence.push_back(i);
		
		CurrentKernel->LZW.push_back(e);
	}*/
}

static void DumpTree(std::ofstream& f, const SequenceNode& node, uint64_t total)
{
	f << "P_" << std::hex << (uint64_t)&node << " [label=\"" << OPCODE_StringShort(node.Opcode) << "\"];" << std::endl;
	
	for (const auto& child : node.Children) {
		f << "P_" << std::hex << (uint64_t)&node 
				<< " -> " 
				<< "P_" << std::hex << (uint64_t)&child.second 
				<< " [label=\"" << std::dec << std::setprecision(2) << (((double)child.second.Count * 100.0) / (double)total) << "\"]"
				<< ";" << std::endl;
		DumpTree(f, child.second, total);
	}
}

void KernelRoutineExit(KernelDescriptor *descriptor)
{
	ASSERT(CurrentFrame, "A frame is not in progress");
	ASSERT(CurrentKernel, "A kernel is not in progress");
	
	CurrentKernel->Duration = now() - CurrentKernel->Duration;
	CurrentFrame->KernelInvocations.push_back(CurrentKernel);
	
	if (CurrentFrame->Index >= SKIP_FRAME) {
		//std::cerr << "*** KERNEL " << CurrentKernel->Descriptor->Name << std::endl;
		
		uint64_t total = 0;
		for (auto n : CurrentKernel->Root.Children) {
			total += n.second.Count;
		}
		
		std::stringstream s;
		s << "seq-" << CurrentKernel->Descriptor->Name << "." << std::dec << CurrentKernel->Index << "." << CurrentFrame->Index << ".dot";
		std::ofstream f(s.str().c_str());
		
		f << "digraph a {" << std::endl;
		DumpTree(f, CurrentKernel->Root, total);
		f << "}" << std::endl;
		
		//std::cerr << "*** END ***" << std::endl;
	}
	
	CurrentKernel->Root.Children.clear();
	
	CurrentKernel->Descriptor->TotalExecutionCount++;
	CurrentKernel->Descriptor->TotalExecutionTime += CurrentKernel->Duration;
	CurrentKernel = NULL;
}

void InstructionExecuted(VOID *rip, uint64_t opcode, uint64_t opclass)
{
	if (!CurrentKernel) return;
	if (CurrentFrame->Index < SKIP_FRAME) return;
	
	if (opclass == XED_CATEGORY_UNCOND_BR || opclass == XED_CATEGORY_COND_BR || opclass == XED_CATEGORY_RET) {
		CurrentKernel->CurrentSequence.clear();
		return;
	}
	
	CurrentKernel->CurrentSequence.push_back(opcode);
	
	SequenceNode *current_node = &CurrentKernel->Root;
	
	unsigned int CurrentSymbol;
	for (CurrentSymbol = 0; CurrentSymbol < CurrentKernel->CurrentSequence.size(); CurrentSymbol++) {
		auto child = current_node->Children.find(CurrentKernel->CurrentSequence[CurrentSymbol]);
		if (child != current_node->Children.end()) {
			current_node = &child->second;
		} else {
			break;
		}
	}
	
	while (CurrentSymbol < CurrentKernel->CurrentSequence.size()) {
		uint64_t opcode = CurrentKernel->CurrentSequence[CurrentSymbol];
		current_node->Children[opcode].Opcode = opcode;
		current_node = &(current_node->Children[opcode]);
		CurrentSymbol++;
	}
	
	current_node->Count++;
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
	INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)InstructionExecuted, 
			IARG_INST_PTR,
			IARG_PTR, (uint64_t)INS_Opcode(ins),
			IARG_PTR, (uint64_t)INS_Category(ins),
			IARG_END);
}

void Fini(INT32 code, void *v)
{
	std::cerr << std::endl;
	std::cerr << "*** SLAMBench Completed ***" << std::endl;
}

static void LoadFriendlyNames()
{
	KernelNameMap["_Z21bilateralFilterKernelPfPKf23__device_builtin__uint2S1_fi"] = "BilateralFilter";
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
	
	RTN_AddInstrumentFunction(Routine, NULL);	
	INS_AddInstrumentFunction(Instruction, NULL);

	PIN_AddFiniFunction(Fini, NULL);			
	PIN_StartProgram();
	
	return 0;
}

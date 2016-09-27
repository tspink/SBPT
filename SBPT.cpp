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
	uint64_t Duration;
};

std::list<KernelDescriptor *> KernelDescriptors;
std::list<FrameDescriptor *> FrameDescriptors;

static FrameDescriptor *CurrentFrame;
static KernelInvocation *CurrentKernel;
static int NextKernelID;

void FrameStart()
{
	ASSERT(!CurrentFrame, "A frame is already in progress");
	
	CurrentFrame = new FrameDescriptor();
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
	CurrentKernel = NULL;
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
	
	KernelDescriptor *descriptor = new KernelDescriptor(NextKernelID++, RTN_Name(rtn));
	KernelDescriptors.push_back(descriptor);
	
	RTN_Open(rtn);
	RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)KernelRoutineEnter, IARG_PTR, descriptor, IARG_END);
	RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)KernelRoutineExit, IARG_PTR, descriptor, IARG_END);
	RTN_Close(rtn);
}

void Fini(INT32 code, void *v)
{
	std::cerr << std::endl;
	std::cerr << "*** SLAMBench Completed ***" << std::endl;
	
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

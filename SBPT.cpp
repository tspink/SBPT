#include "pin.H"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <list>

struct KernelRoutineDescriptor
{
	KernelRoutineDescriptor() : count(0) { }
	
	uint64_t count;
	std::string name;
};

std::list<KernelRoutineDescriptor *> KernelRoutineDescriptors;

void KernelRoutineEnter(KernelRoutineDescriptor *descriptor)
{
	descriptor->count++;
}

void Routine(RTN rtn, void *v)
{
	SEC sec = RTN_Sec(rtn);
	
	// Ignore routines that aren't part of the ".kernel" ELF section
	if (SEC_Name(sec) != ".kernel") return;
	
	std::cerr << "Identified kernel routine: " << RTN_Name(rtn) << std::endl;
	
	KernelRoutineDescriptor *descriptor = new KernelRoutineDescriptor();
	descriptor->name = RTN_Name(rtn);
	KernelRoutineDescriptors.push_back(descriptor);
	
	RTN_Open(rtn);
	RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)KernelRoutineEnter, IARG_PTR, descriptor, IARG_END);
	RTN_Close(rtn);
}

void Fini(INT32 code, void *v)
{
	std::cerr << std::endl;
	std::cerr << "*** SLAMBeanch Completed ***" << std::endl;
	
	for (auto descriptor : KernelRoutineDescriptors) {
		std::cerr << "Kernel: " << descriptor->name << ", Count: " << descriptor->count << std::endl;
		delete descriptor;
	}
	
	KernelRoutineDescriptors.clear();
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

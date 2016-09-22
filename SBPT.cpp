#include "pin.H"

#include <iostream>
#include <iomanip>
#include <fstream>

void Routine(RTN rtn, void *v)
{
	SEC sec = RTN_Sec(rtn);
	
	// Ignore routines that aren't part of the ".kernel" ELF section
	if (SEC_Name(sec) != ".kernel") return;
	
	std::cerr << "Identified kernel routine: " << RTN_Name(rtn) << std::endl;
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

	PIN_StartProgram();
	return 0;
}

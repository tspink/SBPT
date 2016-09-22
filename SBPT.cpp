#include "pin.H"

#include <iostream>
#include <iomanip>
#include <fstream>

void Routine(RTN rtn, void *v)
{
	fprintf(stderr, "routine: %s\n", RTN_Name(rtn).c_str());
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

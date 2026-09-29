#include <windows.h>
EXECUTION_STATE WINAPI reviewExecutionState(EXECUTION_STATE);
#define SetThreadExecutionState reviewExecutionState
#include "../src/engine.cpp"

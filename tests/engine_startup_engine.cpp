#include "engine_startup_hooks.h"
#include <objbase.h>
#define CoInitializeEx probe::initialize
#define SetThreadExecutionState probe::executionState
#include "engine_startup_instrumented.cpp"

#include "engine_power_hooks.h"
#include <objbase.h>
#define Encoder PowerEncoder
#define CoInitializeEx probe::initialize
#define SetThreadExecutionState probe::executionState
#include "../src/engine.cpp"

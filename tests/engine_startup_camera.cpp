#include "engine_startup_hooks.h"
// Keep the actual CameraClient constructor and implementation. No helper may
// launch even if a regression reaches past this fixture's invalid camera ID.
#define CreateProcessW probe::forbiddenProcess
#include "../src/camera_host.cpp"

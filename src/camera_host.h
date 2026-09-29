#pragma once
#include "core.h"

namespace lapse {
// Camera drivers run in a private child of the same portable executable.
// start/latest do not wait for driver activation or a new camera frame.
class CameraClient {
public:
    CameraClient();
    ~CameraClient();
    CameraClient(const CameraClient&) = delete;
    CameraClient& operator=(const CameraClient&) = delete;
    bool start(const std::wstring& id, std::wstring& error);
    void stop();
    bool latest(Frame& output, std::wstring& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Call before initializing the GUI; parses GetCommandLineW internally. Returns
// -1 for a normal launch; otherwise returns the helper process exit code.
// The argument is accepted for convenient use from either wWinMain or tests.
int runCameraHost(const wchar_t* commandLine);
}

#pragma once
#include "core.h"

namespace lapse {
struct Monitor { std::wstring name; RECT bounds{}; };
struct CameraDevice { std::wstring name; std::wstring id; };
std::vector<Monitor> enumerateMonitors();
// The caller initializes COM and Media Foundation.
std::vector<CameraDevice> enumerateCameras(std::wstring& error);
bool captureDesktop(const RECT& bounds, int maxWidth, int maxHeight, bool cursor,
                    Frame& output, std::wstring& error);
// Own and use on one worker thread. False + empty error from latest means
// the camera is warming up; latest never waits for a new camera sample.
class Camera {
public:
    Camera();
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    bool start(const std::wstring& id, std::wstring& error);
    void stop();
    bool latest(Frame& output, std::wstring& error);
    // Host transport uses the actual sample receipt tick, including across processes.
    bool latest(Frame& output, std::wstring& error, uint64_t& receivedTick);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}


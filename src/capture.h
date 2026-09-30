#pragma once
#include "core.h"

namespace lapse {
struct Monitor { std::wstring name; RECT bounds{}; std::wstring id; };
struct CameraDevice { std::wstring name; std::wstring id; };
// Source identity belongs to successful non-null media deliveries, never to
// preview reads or shared-memory publications. Media time and receipt ticks
// use different clocks; only receipt ticks define software blend windows.
struct CameraSampleInfo {
    uint64_t epoch = 0, sequence = 0, receivedTick = 0;
    int64_t timestamp100ns = 0;
    uint32_t transferFunction = 0;
    bool timestampValid = false, discontinuity = false;
};
std::vector<Monitor> enumerateMonitors();
// The caller initializes COM and Media Foundation.
std::vector<CameraDevice> enumerateCameras(std::wstring& error);
bool captureDesktop(const RECT& bounds, int maxWidth, int maxHeight, bool cursor,
                    Frame& output, std::wstring& error);
bool captureMonitor(const std::wstring& id, int maxWidth, int maxHeight, bool cursor,
                    Frame& output, std::wstring& error);
// Release only this thread's existing desktop surfaces when capture is idle.
// A cold/repeated call does no allocation; the next demand recreates the cache.
void releaseDesktopCaptureCache() noexcept;
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
    // Returns metadata even while pending. A retained sample at or before the
    // watermark is not converted/copied; ordinary source-staleness rules apply.
    bool latestNewer(Frame& output, std::wstring& error, CameraSampleInfo& info,
                     const CameraSampleInfo& watermark);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}


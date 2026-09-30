#pragma once
#include "core.h"
#include "night.h"
#include "time_skip.h"

namespace lapse {
struct NightWindowResult {
    NightResult exposure;
    uint64_t beginTick = 0, endTick = 0, firstSampleTick = 0, lastSampleTick = 0;
    bool timestampFallback = false;
};
struct CameraObservation {
    TimeSkipDescriptor descriptor;
    uint64_t epoch = 0, sequence = 0, receivedTick = 0;
    int sourceWidth = 0, sourceHeight = 0;
};
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
    // Windows start after helper acceptance and a fresh source watermark.
    // Each full duration is bounded independently of progress responses. A
    // completed result is pinned until cancellation or the next begin request;
    // ordinary preview reads neither consume nor replace it.
    bool beginNight(uint64_t token, uint32_t durationMs, const NightSettings& settings, std::wstring& error);
    bool nightResult(uint64_t token, Frame& output, NightWindowResult& result, std::wstring& error);
    // Invalidates the token immediately, also resets automatic gain history.
    // No allocation or camera-driver call; safe before recording publication.
    void cancelNight() noexcept;
    // Independent small raw-source report. Call at the observation cadence;
    // false with an empty error means pending/duplicate, never evidence of quiet.
    // A token identifies the current recording/source selection, not an image.
    bool observeActivity(uint64_t token, CameraObservation& output, std::wstring& error);
    void cancelActivityObservation() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Call before initializing the GUI; parses GetCommandLineW internally. Returns
// -1 for a normal launch; otherwise returns the helper process exit code.
// The argument is accepted for convenient use from either wWinMain or tests.
int runCameraHost(const wchar_t* commandLine);
}

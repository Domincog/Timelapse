#pragma once
#include "core.h"
#include "night.h"
#include "time_skip.h"
#include <mutex>
#include <thread>
#include <condition_variable>

namespace lapse {
enum class State { Idle, Starting, Recording, Paused, Finishing };
struct Settings {
    RECT monitor{};
    // Resolve the selected display by identity for every requested frame.
    std::wstring monitorId;
    std::wstring cameraId;
    std::vector<Layer> layers = preset(Mode::Desktop);
    // Exact capture cadence; slower capture/encoding skips elapsed slots.
    int intervalMs = 5000;
    int width = 1280, height = 720;
    EncodingQuality encodingQuality = EncodingQuality::Balanced;
    EncodingMode encodingMode = EncodingMode::Compatible;
    std::wstring folder;
    bool preview = true;
    // Capture both selected sources on one schedule into independent videos.
    bool separateFiles = false;
    // Active recording time, excluding initial startup and pauses. Nonpositive
    // values keep recording until the user finishes or a capture/save fails.
    int recordingLimitSeconds = 0;
    // Split at active recording-time boundaries; zero keeps one output set.
    // Pauses/initial preparation are excluded; automatic saving stays active.
    int segmentDurationSeconds = 0;
    // Best-effort room for finalization; query failure also stops admission.
    // Disable only for destinations that cannot report caller-available space.
    bool stopOnLowDiskSpace = true;
    // Camera-only software exposure; all policy is frozen with the session.
    NightSettings night;
    TimeSkipSettings timeSkip;
    // Optional fragmented H.264 MP4; ordinary MP4 remains the default.
    bool recoveryMode = false;
};
struct TimeSkipStatus {
    bool enabled = false;
    TimeSkipReason reason = TimeSkipReason::Off;
    // Planned cadence, not a measurement of achieved capture speed.
    int64_t intervalMs = 0;
    // Oldest required source receipt, in GetTickCount64's time domain.
    uint64_t lastCheckTick = 0;
    bool observationDelayed = false;
    std::array<wchar_t, 256> diagnostic{};
};
struct CameraInputSize {
    // Verified delivered pixels, not the camera's advertised native modes.
    // Zero dimensions mean no current input is available.
    int width = 0, height = 0;
    uint64_t generation = 0;
};
struct Status {
    State state = State::Idle;
    // Session-wide samples submitted to every output, including closed parts.
    // A failed second write can leave one additional desktop frame, reported
    // in the final message. Never resets at a file boundary.
    uint64_t frames = 0;
    double elapsed = 0;
    std::wstring message = L"Choose a source, then record.";
    std::wstring savedPath;
    // Latest finalized output set (at most two paths), including retained
    // .recording.mp4 files. Earlier split parts remain in the session folder.
    // savedPath remains the first entry for existing single-file callers.
    std::vector<std::wstring> savedPaths;
    // Fully published segment sets in a split session; zero when splitting is
    // off. Paired desktop/camera files count as one set. Never resets at rollover.
    uint64_t completedSegments = 0;
    bool error = false;
    // Preview errors do not change the outcome of the last recording attempt.
    bool recordingFailed = false;
    // Facts about the most recently admitted camera blend. The original
    // camera's shutter/exposure is not changed or inferred from these values.
    bool nightEnabled = false, nightWaiting = false;
    uint32_t nightDurationMs = 0;
    NightResult night;
    TimeSkipStatus timeSkip;
    CameraInputSize cameraInput;
    std::shared_ptr<const Frame> preview;
};
class Engine {
public:
    Engine();
    ~Engine();
    void configure(const Settings& settings);
    void refreshSources();
    void record();
    void pause();
    void setPaused(bool paused);
    void finish();
    Status status();
private:
    void run();
    // Called with mutex_ held when a preview selection is retired.
    void retirePreview();
    // Called with mutex_ held. Invalidates in-flight input-size publication.
    void retireCameraInput() noexcept;
    std::mutex mutex_;
    std::condition_variable wake_;
    Settings settings_;
    Status status_;
    uint64_t previewGeneration_ = 0;
    uint64_t cameraInputGeneration_ = 0;
    uint64_t settingsRevision_ = 0;
    bool previewProblem_ = false;
    bool quit_ = false, start_ = false, stop_ = false, pauseRequested_ = false, pauseTarget_ = false, retrySources_ = false;
    std::thread worker_;
};
}

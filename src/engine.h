#pragma once
#include "core.h"
#include "config.h"
#include "night.h"
#include "time_skip.h"
#include "watermark.h"
#include "status_feed.h"
#include <array>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <optional>

namespace lapse {
enum class State { Idle, Starting, Recording, Paused, Finishing, Waiting };
struct Settings {
    RECT monitor{};
    // Resolve the selected display by identity for every requested frame.
    std::wstring monitorId;
    std::wstring cameraId;
    std::vector<Layer> layers = preset(Mode::Desktop);
    // Live capture cadence; slower capture/encoding skips elapsed slots.
    int intervalMs = 5000;
    // Camera input uses a bounded 1080p tier only outside the 1280x720 box;
    // output dimensions (and therefore that request) freeze with the session.
    int width = 1280, height = 720;
    // Saved playback rate, independent of capture cadence. Frozen at Record.
    int outputFps = DefaultOutputFps;
    EncodingQuality encodingQuality = EncodingQuality::Balanced;
    EncodingMode encodingMode = EncodingMode::Compatible;
    // Advanced settings freeze with every output and segment at Record.
    EncodingOptions encodingOptions;
    std::wstring folder;
    bool preview = true;
    // Capture both selected sources on one schedule into independent videos.
    bool separateFiles = false;
    // With a layout, also write each selected source to its own full-frame
    // MP4 (-desktop/-camera) from the same samples. Ignored for separate files
    // and when the layout already shows only that source. Frozen at Record.
    bool alsoSaveDesktop = false, alsoSaveCamera = false;
    // Active recording time, excluding initial startup and pauses. Nonpositive
    // values keep recording until the user finishes or a capture/save fails.
    int recordingLimitSeconds = 0;
    // Split at active recording-time boundaries; zero keeps one output set.
    // Pauses/initial preparation are excluded; automatic saving stays active.
    int segmentDurationSeconds = 0;
    // Optional self-timer before preparation; does not consume recording time.
    // Zero starts immediately; positive whole seconds are bounded to 1..300.
    int startDelaySeconds = 0;
    // Best-effort room for finalization; query failure also stops admission.
    // Disable only for destinations that cannot report caller-available space.
    bool stopOnLowDiskSpace = true;
    // Camera-only software exposure; blend policy is frozen with the session.
    NightSettings night;
    TimeSkipSettings timeSkip;
    // Final-frame overlay; raw source analysis is unaffected. Frozen at Record.
    WatermarkSettings watermark;
    // Where and how the live status line is drawn. Frozen at Record; the
    // status itself changes at any time through Engine::setStatus.
    StatusFeedSettings statusFeed;
    // Optional fragmented H.264 MP4; ordinary MP4 remains the default.
    bool recoveryMode = false;
    // Include the native cursor in desktop video and preview. Frozen with the
    // session; dedicated desktop Quiet observations always omit the cursor.
    bool captureCursor = true;
};
// Every recording writes one to three MP4 outputs from the same admitted
// samples, sharing their capture schedule, pauses, splits and frame count.
enum class OutputKind { Layout, Desktop, Camera };
constexpr size_t MaxRecordingOutputs = 3;
struct OutputPlan {
    std::array<OutputKind, MaxRecordingOutputs> kinds{};
    size_t count = 0;
};
// Separate files keep their desktop/camera pair. Otherwise the layout comes
// first, followed by requested single-source companions that differ from it.
inline OutputPlan outputPlan(const Settings& settings) noexcept {
    OutputPlan plan;
    if (settings.separateFiles) {
        plan.kinds[0] = OutputKind::Desktop; plan.kinds[1] = OutputKind::Camera; plan.count = 2;
        return plan;
    }
    unsigned layout = 0;
    for (const auto& layer : settings.layers) layout |= layer.source == Source::Desktop ? 1u : 2u;
    plan.kinds[plan.count++] = OutputKind::Layout;
    // A companion matching a single-source layout would only duplicate it.
    if (settings.alsoSaveDesktop && layout != 1u) plan.kinds[plan.count++] = OutputKind::Desktop;
    if (settings.alsoSaveCamera && layout != 2u) plan.kinds[plan.count++] = OutputKind::Camera;
    return plan;
}
// Bit 0: desktop, bit 1: camera; every source any planned output needs.
inline unsigned recordingSources(const Settings& settings) noexcept {
    unsigned mask = 0;
    for (const auto& layer : settings.layers) mask |= layer.source == Source::Desktop ? 1u : 2u;
    const auto plan = outputPlan(settings);
    for (size_t i = 0; i < plan.count; ++i)
        if (plan.kinds[i] != OutputKind::Layout) mask |= plan.kinds[i] == OutputKind::Desktop ? 1u : 2u;
    return mask;
}
struct TimeSkipStatus {
    bool enabled = false;
    TimeSkipReason reason = TimeSkipReason::Off;
    // Planned cadence, not a measurement of achieved capture speed.
    int64_t intervalMs = 0;
    // Oldest required source receipt, in GetTickCount64's time domain.
    uint64_t lastCheckTick = 0;
    bool observationDelayed = false;
    // Person-only recording is admitting no frames until a person returns.
    bool suspended = false;
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
    // Authoritative GetTickCount64 deadline, nonzero only while Waiting.
    uint64_t startDeadlineTick = 0;
    std::wstring message = L"Choose a source, then record.";
    std::wstring savedPath;
    // Latest finalized output set (at most three paths), including retained
    // .recording.mp4 files. Earlier split parts remain in the session folder.
    // savedPath remains the first entry for existing single-file callers.
    std::vector<std::wstring> savedPaths;
    // Fully published segment sets in a split session; zero when splitting is
    // off. A set's files (layout and/or sources) count once. Never resets at rollover.
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
    // Cancel a delayed request through its first admission boundary. Called
    // synchronously for suspend/resume; cleanup remains on the worker thread.
    void cancelDelayedStart() noexcept;
    // The user's current status line, shown on the next admitted frame and in
    // the preview. Kind None clears it. Invalid items are ignored.
    void setStatus(const StatusItem& item);
    Status status();
private:
    void run();
    // Called with mutex_ held when a preview selection is retired.
    void retirePreview(bool visualOnly = false);
    // Called with mutex_ held. Invalidates in-flight input-size publication.
    void retireCameraInput() noexcept;
    std::mutex mutex_;
    std::condition_variable wake_;
    Settings settings_;
    Status status_;
    uint64_t previewGeneration_ = 0;
    uint64_t visualPreviewGeneration_ = 0;
    uint64_t cameraInputGeneration_ = 0;
    uint64_t settingsRevision_ = 0;
    bool requestedCursor_ = true;
    int requestedOutputFps_ = DefaultOutputFps;
    std::optional<Settings> delayedSettings_;
    std::wstring delayedCancellationMessage_;
    uint64_t delayedWakeEpoch_ = 0;
    bool delayedStartPending_ = false, delayedCancel_ = false;
    bool previewProblem_ = false;
    // A live-added optional source is missing; recording continues without it.
    bool sourceWarning_ = false;
    // The active session's frozen files, for status text set outside the worker.
    OutputPlan sessionPlan_{};
    StatusItem statusItem_{};
    bool quit_ = false, start_ = false, stop_ = false, pauseRequested_ = false, pauseTarget_ = false, retrySources_ = false;
    std::thread worker_;
};
}

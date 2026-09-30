#pragma once
#include "core.h"
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
    int interval = 5;
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
    // Best-effort room for finalization; query failure also stops admission.
    // Disable only for destinations that cannot report caller-available space.
    bool stopOnLowDiskSpace = true;
};
struct Status {
    State state = State::Idle;
    // Samples submitted to every active output; a failed second write can
    // leave one additional desktop frame, reported in the final message.
    uint64_t frames = 0;
    double elapsed = 0;
    std::wstring message = L"Choose a source, then record.";
    std::wstring savedPath;
    // All finalized playable videos, including retained .recording.mp4 files.
    // savedPath remains the first entry for existing single-file callers.
    std::vector<std::wstring> savedPaths;
    bool error = false;
    // Preview errors do not change the outcome of the last recording attempt.
    bool recordingFailed = false;
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
    std::mutex mutex_;
    std::condition_variable wake_;
    Settings settings_;
    Status status_;
    uint64_t previewGeneration_ = 0;
    uint64_t settingsRevision_ = 0;
    bool previewProblem_ = false;
    bool quit_ = false, start_ = false, stop_ = false, pauseRequested_ = false, pauseTarget_ = false, retrySources_ = false;
    std::thread worker_;
};
}

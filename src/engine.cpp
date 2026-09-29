#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include "encoder.h"
#include <objbase.h>
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace lapse {
using Clock = std::chrono::steady_clock;
namespace {
// Applies only to this worker thread. Releasing the request restores normal
// Windows sleep behavior; no persistent power settings are changed.
class RecordingPower {
public:
    void update(bool recording, bool desktop) {
        EXECUTION_STATE requested = ES_CONTINUOUS;
        if (recording) {
            requested |= ES_SYSTEM_REQUIRED;
            if (desktop) requested |= ES_DISPLAY_REQUIRED;
        }
        if (requested != current_ && SetThreadExecutionState(requested)) current_ = requested;
    }
    ~RecordingPower() { SetThreadExecutionState(ES_CONTINUOUS); }
private:
    EXECUTION_STATE current_ = ES_CONTINUOUS;
};
}
Engine::Engine() : worker_(&Engine::run, this) {}
Engine::~Engine() {
    { std::lock_guard<std::mutex> lock(mutex_); quit_ = true; }
    wake_.notify_one();
    worker_.join();
}
void Engine::configure(const Settings& s) {
    { std::lock_guard<std::mutex> lock(mutex_);
      auto sources = [](const Settings& config) { int mask = 0; for (const auto& layer : config.layers) mask |= layer.source == Source::Desktop ? 1 : 2; return mask; };
      const int mask = sources(s);
      if (mask != sources(settings_) || ((mask & 2) && s.cameraId != settings_.cameraId) ||
          ((mask & 1) && !EqualRect(&s.monitor, &settings_.monitor))) status_.preview.reset();
      settings_ = s; }
    wake_.notify_one();
}
void Engine::refreshSources() {
    { std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state != State::Idle) return;
      retrySources_ = true; status_.error = false; status_.message = L"Ready to record."; }
    wake_.notify_one();
}
void Engine::record() {
    { std::lock_guard<std::mutex> lock(mutex_); if (status_.state != State::Idle) return;
      start_ = true; status_.state = State::Starting; status_.error = false;
      status_.message = L"Preparing recording..."; status_.frames = 0; status_.elapsed = 0; status_.savedPath.clear(); }
    wake_.notify_one();
}
void Engine::pause() { { std::lock_guard<std::mutex> lock(mutex_); togglePause_ = true; } wake_.notify_one(); }
void Engine::finish() { { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; } wake_.notify_one(); }
Status Engine::status() { std::lock_guard<std::mutex> lock(mutex_); return status_; }
void Engine::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        CameraClient camera;
        Encoder encoder;
        RecordingPower power;
        Settings session;
        std::wstring activeCamera, temporary, finalPath, sourceError;
        bool cameraRunning = false, pending = false, writing = false, paused = false, previewProblem = false;
        auto lastPreview = Clock::time_point::min(), nextFrame = Clock::now();
        auto lastTick = Clock::now();
        double elapsed = 0;
        auto publishError = [&](const std::wstring& message, bool reset) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.error = true; status_.message = message;
            previewProblem = false;
            if (reset) status_.state = State::Idle;
        };
        auto publishPreviewError = [&](const std::wstring& message) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.preview.reset();
            // A preview can recover on its next refresh, but it must not
            // replace a recording or resource error that the user needs to see.
            if (!status_.error || previewProblem) {
                status_.error = true;
                status_.message = writing ? L"Preview unavailable: " + message : message;
                previewProblem = true;
            }
        };
        auto closeRecording = [&](const std::wstring& reason) {
            { std::lock_guard<std::mutex> lock(mutex_); status_.state = State::Finishing; status_.message = L"Finishing MP4..."; }
            std::wstring error;
            const bool finalized = writing && encoder.finish(error);
            bool renamed = false, retainedPartial = false;
            if (finalized) {
                renamed = MoveFileExW(temporary.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
                if (!renamed) {
                    const DWORD renameError = GetLastError();
                    error = L"Video finished, but could not rename it. Finished video remains at: " + temporary + L". " + errorText(HRESULT_FROM_WIN32(renameError));
                }
            } else if (writing && encoder.frames() > 0) {
                const DWORD attributes = GetFileAttributesW(temporary.c_str());
                retainedPartial = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
            }
            const bool ok = !writing || renamed;
            std::lock_guard<std::mutex> lock(mutex_);
            status_.state = State::Idle;
            status_.error = !reason.empty() || !ok;
            // Finalize and rename are separate outcomes: a rename collision
            // leaves a complete playable movie under the temporary filename.
            if (finalized) status_.savedPath = renamed ? finalPath : temporary;
            status_.message = !ok ? finalized ? error
                    : L"Could not finish video: " + error + (retainedPartial ? L" Partial file: " + temporary : L"")
                : !reason.empty() ? reason + (writing ? L" Captured frames were saved." : L"")
                : writing ? L"Saved: " + finalPath : L"Recording cancelled.";
            writing = pending = paused = false; previewProblem = false;
            power.update(false, false);
        };
        for (;;) {
            bool previewOnlyWork = false;
            try {
                Settings cfg;
                bool quit, start, stop, toggle, retry;
                { std::unique_lock<std::mutex> lock(mutex_);
                  wake_.wait_for(lock, std::chrono::milliseconds(50));
                  cfg = settings_; quit = quit_; start = start_; stop = stop_; toggle = togglePause_; retry = retrySources_;
                  start_ = stop_ = togglePause_ = retrySources_ = false; }
                const auto now = Clock::now();
                if (writing && !paused) elapsed += std::chrono::duration<double>(now - lastTick).count();
                lastTick = now;
                if (quit) { if (writing || pending) closeRecording(L""); break; }
                if (retry) { camera.stop(); cameraRunning = false; activeCamera.clear(); previewProblem = false; }
                if (FAILED(com)) { publishError(L"Windows media initialization failed: " + errorText(com), true); continue; }
                if (start) {
                    session = cfg; pending = true; elapsed = 0; previewProblem = false;
                    nextFrame = now;
                }
                if (stop && (pending || writing)) { closeRecording(L""); continue; }
                if (toggle && writing) {
                    paused = !paused;
                    if (!paused) nextFrame = now;
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.state = paused ? State::Paused : State::Recording;
                    if (!previewProblem)
                        status_.message = paused ? L"Paused. Resume when you are ready." : L"Recording. You can adjust the collage live.";
                }
                if (writing || pending) {
                    session.layers = cfg.layers; session.preview = cfg.preview;
                    cfg = session;
                }
                bool needCamera = false, needDesktop = false;
                for (auto& layer : cfg.layers) { needCamera |= layer.source == Source::Camera; needDesktop |= layer.source == Source::Desktop; }
                power.update(writing && !paused, needDesktop);
                const bool active = pending || (writing && !paused) || cfg.preview;
                if ((!needCamera || !active || activeCamera != cfg.cameraId) && cameraRunning) {
                    camera.stop(); cameraRunning = false; activeCamera.clear();
                }
                if (!needCamera) {
                    // Switching away and back is an explicit source retry,
                    // including when a failed camera has already been stopped.
                    activeCamera.clear(); sourceError.clear();
                }
                // Re-open only on a change of device or a new recording attempt after failure.
                if (needCamera && active && !cameraRunning && (activeCamera != cfg.cameraId || start)) {
                    activeCamera = cfg.cameraId;
                    sourceError.clear();
                    if (!cfg.cameraId.empty()) cameraRunning = camera.start(cfg.cameraId, sourceError);
                    else sourceError = L"No camera found. Connect a camera and refresh sources.";
                }
                const bool captureDue = pending || (writing && !paused && now >= nextFrame);
                const bool previewDue = cfg.preview && (lastPreview == Clock::time_point::min() || now - lastPreview >= std::chrono::milliseconds(writing ? 1000 : 500));
                if (!captureDue && !previewDue) {
                    if (writing) { std::lock_guard<std::mutex> lock(mutex_); status_.elapsed = elapsed; }
                    continue;
                }
                previewOnlyWork = !captureDue;
                lastPreview = now;
                Frame desktop, webcam, composed;
                std::wstring error;
                bool ready = true, cameraWaiting = false;
                if (needCamera) {
                    ready = cameraRunning && camera.latest(webcam, error);
                    cameraWaiting = cameraRunning && !ready && error.empty();
                    if (!cameraRunning) error = sourceError.empty() ? L"No camera available." : sourceError;
                    else if (!ready && !error.empty()) {
                        // A stale/disconnected reader cannot recover by polling
                        // its old sample. Keep the device id as the failed-attempt
                        // marker so idle preview does not repeatedly reopen it;
                        // a new Record action can retry the same camera.
                        camera.stop(); cameraRunning = false; sourceError = error;
                    }
                }
                int width = captureDue ? cfg.width : 640, height = captureDue ? cfg.height : 360;
                if (needDesktop && ready) ready = captureDesktop(cfg.monitor, width, height, true, desktop, error);
                if (ready) ready = compose(needDesktop ? &desktop : nullptr, needCamera ? &webcam : nullptr, cfg.layers, width, height, composed, error);
                if (!ready) {
                    // Also allow warmup after resuming with preview disabled:
                    // that pause releases the camera and resume opens it again.
                    // An empty result is a transient wait (including shared
                    // frame contention). The client owns activation/staleness
                    // deadlines and returns a concrete error when they expire.
                    if (cameraWaiting) continue;
                    if (error.empty()) error = L"The capture source did not provide a frame. Check its connection or select another source.";
                    // A preview refresh is not a requested video frame. Keep
                    // the encoder open through transient preview failures,
                    // including while paused; a due capture must still fail
                    // visibly and finalize all frames already written.
                    if (captureDue) closeRecording(L"Capture stopped: " + error);
                    else publishPreviewError(error);
                    continue;
                }
                if (pending) {
                    std::error_code ec;
                    std::filesystem::create_directories(cfg.folder, ec);
                    if (ec || cfg.folder.empty()) { closeRecording(L"Cannot create the save folder. Choose another folder."); continue; }
                    SYSTEMTIME t; GetLocalTime(&t);
                    wchar_t name[100];
                    swprintf_s(name, L"Timelapse-%04u%02u%02u-%02u%02u%02u-%03u-%lu", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
                    auto base = (std::filesystem::path(cfg.folder) / name).wstring();
                    temporary = base + L".recording.mp4"; finalPath = base + L".mp4";
                    if (!encoder.open(temporary, cfg.width, cfg.height, 30, error)) { closeRecording(L"Cannot start recording: " + error); continue; }
                    writing = true; pending = false;
                    power.update(true, needDesktop);
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.state = State::Recording; status_.error = false; status_.message = L"Recording. You can adjust the collage live.";
                }
                if (captureDue && writing && !paused) {
                    if (!encoder.write(composed, error)) { closeRecording(L"Recording stopped: " + error); continue; }
                    const auto interval = std::chrono::seconds(std::clamp(cfg.interval, 1, 3600));
                    do { nextFrame += interval; } while (nextFrame <= Clock::now());
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.frames = encoder.frames(); status_.elapsed = elapsed;
                }
                { std::lock_guard<std::mutex> lock(mutex_);
                  status_.preview = std::make_shared<Frame>(std::move(composed));
                  if (previewProblem) {
                      status_.error = false;
                      status_.message = writing
                          ? paused ? L"Paused. Resume when you are ready." : L"Recording. You can adjust the collage live."
                          : L"Ready to record.";
                      previewProblem = false;
                  } }
            } catch (const std::exception&) {
                if (previewOnlyWork) {
                    // Preview buffers are disposable. Do not close a healthy
                    // recording or camera when only this refresh ran out of
                    // resources. Due-frame allocation failures below still
                    // finalize the recording and retain their error message.
                    try { publishPreviewError(L"The preview could not allocate a resource. It will retry."); } catch (...) {}
                    std::unique_lock<std::mutex> lock(mutex_);
                    wake_.wait_for(lock, std::chrono::seconds(1), [&] { return quit_ || stop_ || togglePause_; });
                    continue;
                }
                // Recover this iteration, leaving the worker available for a
                // new recording. Finalize first so captured frames can survive
                // allocation/path failures as a playable video.
                try {
                    if (writing || pending) closeRecording(L"Recording stopped because a resource could not be allocated. You can try recording again.");
                    else publishError(L"Capture could not allocate a resource. You can try recording again.", true);
                } catch (...) {
                    // Reporting or naming the completed recording may itself
                    // allocate. Still release the encoder and leave a retryable
                    // state; the .recording.mp4 file may contain saved frames.
                    try { std::wstring ignored; encoder.finish(ignored); } catch (...) {}
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.state = State::Idle; status_.error = true;
                    try { status_.message = L"Capture stopped. Check the save folder and try recording again."; } catch (...) {}
                }
                camera.stop(); cameraRunning = false;
                writing = pending = paused = false; previewProblem = false;
                power.update(false, false);
                std::unique_lock<std::mutex> lock(mutex_);
                status_.preview.reset();
                // Persistent resource exhaustion must not become a hot retry
                // loop. Shutdown can wake this delay immediately.
                wake_.wait_for(lock, std::chrono::seconds(1), [&] { return quit_; });
            }
        }
        camera.stop();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}
}

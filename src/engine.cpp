#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include "encoder.h"
#include <objbase.h>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <optional>

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
    void saving() {
        // A paused writer still needs system availability while saving. Keep an
        // active desktop request unchanged; saving alone need not wake a display.
        update(true, (current_ & ES_DISPLAY_REQUIRED) != 0);
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
      if (!s.preview || mask != sources(settings_) || ((mask & 2) && s.cameraId != settings_.cameraId) ||
          ((mask & 1) && (CompareStringOrdinal(s.monitorId.c_str(), -1, settings_.monitorId.c_str(), -1, TRUE) != CSTR_EQUAL ||
              !EqualRect(&s.monitor, &settings_.monitor)))) retirePreview();
      settings_ = s; }
    wake_.notify_one();
}
void Engine::retirePreview() {
    ++previewGeneration_;
    status_.preview.reset();
    if (!previewProblem_) return;
    previewProblem_ = false;
    status_.error = false;
    status_.message = status_.state == State::Recording ? L"Recording. You can adjust the collage live."
        : status_.state == State::Paused ? L"Paused. Resume when you are ready."
        : status_.state == State::Starting ? L"Preparing recording..."
        : status_.state == State::Finishing ? L"Finishing MP4..." : L"Ready to record.";
}
void Engine::refreshSources() {
    { std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state != State::Idle) return;
      retirePreview();
      retrySources_ = true; previewProblem_ = false;
      status_.error = false; status_.message = L"Ready to record."; }
    wake_.notify_one();
}
void Engine::record() {
    { std::lock_guard<std::mutex> lock(mutex_); if (status_.state != State::Idle) return;
      ++previewGeneration_; previewProblem_ = false;
      stop_ = pauseRequested_ = pauseTarget_ = false;
      start_ = true; status_.state = State::Starting; status_.error = false; status_.recordingFailed = false;
      status_.message = L"Preparing recording..."; status_.frames = 0; status_.elapsed = 0; status_.savedPath.clear(); }
    wake_.notify_one();
}
void Engine::pause() {
    { std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state != State::Recording && status_.state != State::Paused) return;
      pauseRequested_ = true; pauseTarget_ = status_.state != State::Paused; }
    wake_.notify_one();
}
void Engine::setPaused(bool paused) {
    { std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state != State::Recording && status_.state != State::Paused) return;
      pauseRequested_ = true; pauseTarget_ = paused; }
    wake_.notify_one();
}
void Engine::finish() {
    { std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state != State::Starting && status_.state != State::Recording && status_.state != State::Paused) return;
      stop_ = true; }
    wake_.notify_one();
}
Status Engine::status() { std::lock_guard<std::mutex> lock(mutex_); return status_; }
void Engine::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        // Construct allocating resources inside the guarded operation that
        // needs them, so construction failure cannot escape this worker.
        std::optional<CameraClient> camera;
        std::optional<Encoder> encoder;
        RecordingPower power;
        std::optional<Settings> session;
        // Capture and encode storage belongs only to the worker. Retain its
        // allocation across samples instead of allocating several megabytes on
        // every refresh. Published preview frames never share this storage.
        Frame desktop, webcam, composed;
        std::shared_ptr<Frame> previewBuffer;
        auto preparePreviewBuffer = [&]() -> Frame& {
            // Keep at most one retired frame for reuse. A UI snapshot can keep
            // an older frame alive, so recycle only with exclusive ownership.
            if (!previewBuffer || previewBuffer.use_count() != 1) previewBuffer = std::make_shared<Frame>();
            return *previewBuffer;
        };
        std::wstring activeCamera, temporary, finalPath, temporaryIO, finalPathIO, sourceError;
        bool cameraRunning = false, pending = false, writing = false, paused = false;
        uint64_t previewGeneration = 0, cameraAttemptGeneration = 0;
        auto lastPreview = Clock::time_point::min(), nextFrame = Clock::now();
        auto lastTick = Clock::now();
        double elapsed = 0;
        auto publishError = [&](const std::wstring& message, bool reset) {
            std::lock_guard<std::mutex> lock(mutex_);
            // An unconsumed Record command owns the next attempt's status.
            if (start_) return;
            if (reset && status_.state != State::Idle) status_.recordingFailed = true;
            status_.error = true; status_.message = message;
            previewProblem_ = false;
            if (reset) status_.state = State::Idle;
        };
        auto publishPreviewError = [&](const std::wstring& message) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (start_ || previewGeneration != previewGeneration_) return;
            status_.preview.reset();
            // A preview can recover on its next refresh, but it must not
            // replace a recording or resource error that the user needs to see.
            if (!status_.error || previewProblem_) {
                status_.error = true;
                status_.message = writing ? L"Preview unavailable: " + message : message;
                previewProblem_ = true;
            }
        };
        auto closeRecording = [&](const std::wstring& reason) {
            if (writing) power.saving();
            { std::lock_guard<std::mutex> lock(mutex_); status_.state = State::Finishing; status_.message = L"Finishing MP4..."; }
            std::wstring error;
            const bool finalized = writing && encoder->finishForPublication(error);
            bool ok = !writing;
            std::wstring savedPath, message;
            if (finalized) {
                // Prepare every allocating success update before publishing the
                // file. Once renamed, status publication below cannot allocate.
                savedPath = finalPath;
                message = reason.empty() ? L"Saved: " + finalPath : reason + L" Captured frames were saved.";
                const DWORD renameError = encoder->publish(finalPathIO);
                ok = renameError == ERROR_SUCCESS;
                if (!ok) {
                    savedPath = temporary;
                    message = L"Video finished, but could not rename it. Finished video remains at: " + temporary + L". " + errorText(HRESULT_FROM_WIN32(renameError));
                }
            } else if (writing) {
                const DWORD attributes = encoder->frames() > 0 ? GetFileAttributesW(temporaryIO.c_str()) : INVALID_FILE_ATTRIBUTES;
                const bool retainedPartial = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
                message = L"Could not finish video: " + error + (retainedPartial ? L" Partial file: " + temporary : L"");
            } else message = reason.empty() ? L"Recording cancelled." : reason;
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = pauseRequested_ = pauseTarget_ = false;
            status_.state = State::Idle;
            status_.error = !reason.empty() || !ok;
            status_.recordingFailed = status_.error;
            // Finalize and rename remain separate outcomes: a failed rename
            // advertises the complete playable movie at its temporary filename.
            if (finalized) status_.savedPath.swap(savedPath);
            status_.message.swap(message);
            if (encoder) encoder->releasePublication();
            writing = pending = paused = false; previewProblem_ = false;
            desktop = {}; webcam = {}; composed = {};
            power.update(false, false);
        };
        auto waitAfterFailure = [&](std::unique_lock<std::mutex>& lock) {
            // Commands already queued at failure remain pending, but must not
            // turn persistent allocation failure into an immediate retry loop.
            const auto generation = previewGeneration_;
            const bool start = start_, stop = stop_, pauseRequested = pauseRequested_, pauseTarget = pauseTarget_, retry = retrySources_;
            wake_.wait_for(lock, std::chrono::seconds(1), [&] {
                return quit_ || previewGeneration_ != generation || start_ != start ||
                    stop_ != stop || pauseRequested_ != pauseRequested || pauseTarget_ != pauseTarget || retrySources_ != retry;
            });
        };
        for (;;) {
            bool previewOnlyWork = false;
            try {
                std::optional<Settings> snapshot;
                bool quit, start = false, stop = false, pauseRequested = false, pauseTarget = false, retry = false;
                { std::unique_lock<std::mutex> lock(mutex_);
                  wake_.wait_for(lock, std::chrono::milliseconds(50));
                  // Shutdown needs no settings allocation. Capture operation
                  // ownership before copying, and consume commands only after
                  // their settings snapshot has succeeded.
                  quit = quit_;
                  previewGeneration = previewGeneration_;
                  previewOnlyWork = !start_ && !stop_ && !quit_ && !pending &&
                      (!writing || paused || Clock::now() < nextFrame);
                  if (!quit) {
                      snapshot.emplace(settings_);
                      start = start_; stop = stop_; pauseRequested = pauseRequested_; pauseTarget = pauseTarget_; retry = retrySources_;
                      start_ = stop_ = pauseRequested_ = retrySources_ = false;
                  } }
                const auto now = Clock::now();
                if (writing && !paused) elapsed += std::chrono::duration<double>(now - lastTick).count();
                lastTick = now;
                if (quit) { if (writing || pending) { previewOnlyWork = false; closeRecording(L""); } break; }
                Settings& cfg = *snapshot;
                if (retry) { if (camera) camera->stop(); cameraRunning = false; activeCamera.clear(); }
                if (FAILED(com)) { publishError(L"Windows media initialization failed: " + errorText(com), true); continue; }
                if (start) {
                    previewOnlyWork = false;
                    session.emplace(cfg); pending = true; elapsed = 0;
                    nextFrame = now;
                }
                if (stop && (pending || writing)) { previewOnlyWork = false; closeRecording(L""); continue; }
                if (pauseRequested && writing && paused != pauseTarget) {
                    paused = pauseTarget;
                    const bool desktopNeeded = std::any_of(cfg.layers.begin(), cfg.layers.end(),
                        [](const Layer& layer) { return layer.source == Source::Desktop; });
                    power.update(!paused, desktopNeeded);
                    if (!paused) nextFrame = now;
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.state = paused ? State::Paused : State::Recording;
                    if (!previewProblem_)
                        status_.message = paused ? L"Paused. Resume when you are ready." : L"Recording. You can adjust the collage live.";
                }
                const bool captureDue = pending || (writing && !paused && now >= nextFrame);
                previewOnlyWork = !captureDue;
                if (writing || pending) {
                    session->layers = cfg.layers; session->preview = cfg.preview;
                    cfg = *session;
                }
                bool needCamera = false, needDesktop = false;
                for (auto& layer : cfg.layers) { needCamera |= layer.source == Source::Camera; needDesktop |= layer.source == Source::Desktop; }
                power.update(writing && !paused, needDesktop);
                const bool active = pending || (writing && !paused) || cfg.preview;
                if (!cfg.preview) previewBuffer.reset();
                if (!active) { desktop = {}; webcam = {}; composed = {}; }
                else {
                    if (!needDesktop) desktop = {};
                    if (!needCamera) webcam = {};
                }
                if ((!needCamera || !active || activeCamera != cfg.cameraId) && cameraRunning) {
                    camera->stop(); cameraRunning = false; activeCamera.clear();
                }
                if (!needCamera) {
                    // Switching away and back is an explicit source retry,
                    // including when a failed camera has already been stopped.
                    activeCamera.clear(); sourceError.clear();
                }
                // Retry a retired failed attempt once under the new source generation.
                // Healthy readers remain open; current-generation failures stay latched.
                if (needCamera && active && !cameraRunning && (activeCamera != cfg.cameraId || cameraAttemptGeneration != previewGeneration || start)) {
                    activeCamera = cfg.cameraId;
                    cameraAttemptGeneration = previewGeneration;
                    sourceError.clear();
                    if (!cfg.cameraId.empty()) {
                        try {
                            if (!camera) camera.emplace();
                            cameraRunning = camera->start(cfg.cameraId, sourceError);
                        }
                        catch (...) {
                            // A startup exception may leave partial client state.
                            // Permit the promised preview retry after cleanup.
                            if (camera) camera->stop();
                            cameraRunning = false;
                            activeCamera.clear(); sourceError.clear();
                            throw;
                        }
                    }
                    else sourceError = L"No camera found. Connect a camera and refresh sources.";
                }
                const bool previewDue = cfg.preview && (lastPreview == Clock::time_point::min() || now - lastPreview >= std::chrono::milliseconds(writing ? 1000 : 500));
                if (!captureDue && !previewDue) {
                    if (writing) { std::lock_guard<std::mutex> lock(mutex_); status_.elapsed = elapsed; }
                    continue;
                }
                lastPreview = now;
                std::wstring error;
                bool ready = true, cameraWaiting = false;
                if (needCamera) {
                    ready = cameraRunning && camera->latest(webcam, error);
                    cameraWaiting = cameraRunning && !ready && error.empty();
                    if (!cameraRunning) error = sourceError.empty() ? L"No camera available." : sourceError;
                    else if (!ready && !error.empty()) {
                        // A stale/disconnected reader cannot recover by polling
                        // its old sample. Keep the device id as the failed-attempt
                        // marker so idle preview does not repeatedly reopen it;
                        // a new Record action can retry the same camera.
                        camera->stop(); cameraRunning = false;
                        cameraAttemptGeneration = previewGeneration;
                        sourceError = error;
                    }
                }
                int width = captureDue ? cfg.width : 640, height = captureDue ? cfg.height : 360;
                if (needDesktop && ready) ready = captureMonitor(cfg.monitorId, width, height, true, desktop, error);
                if (ready) ready = compose(needDesktop ? &desktop : nullptr, needCamera ? &webcam : nullptr, cfg.layers,
                    width, height, captureDue ? composed : preparePreviewBuffer(), error);
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
                    std::filesystem::create_directories(fileIOPath(cfg.folder), ec);
                    if (ec || cfg.folder.empty()) { closeRecording(L"Cannot create the save folder. Choose another folder."); continue; }
                    SYSTEMTIME t; GetLocalTime(&t);
                    wchar_t name[100];
                    swprintf_s(name, L"Timelapse-%04u%02u%02u-%02u%02u%02u-%03u-%lu", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
                    auto base = (std::filesystem::path(cfg.folder) / name).wstring();
                    temporary = base + L".recording.mp4"; finalPath = base + L".mp4";
                    // Cache I/O spellings before opening: successful save
                    // publication must not allocate after the file is renamed.
                    temporaryIO = fileIOPath(temporary); finalPathIO = fileIOPath(finalPath);
                    if (!encoder) encoder.emplace();
                    if (!encoder->open(temporaryIO, cfg.width, cfg.height, 30, error, cfg.encodingQuality)) { closeRecording(L"Cannot start recording: " + error); continue; }
                    writing = true; pending = false;
                    power.update(true, needDesktop);
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.state = State::Recording; status_.error = false; status_.message = L"Recording. You can adjust the collage live.";
                }
                if (captureDue && writing && !paused) {
                    if (!encoder->write(composed, error)) { closeRecording(L"Recording stopped: " + error); continue; }
                    const auto interval = std::chrono::seconds(std::clamp(cfg.interval, 1, 3600));
                    do { nextFrame += interval; } while (nextFrame <= Clock::now());
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.frames = encoder->frames(); status_.elapsed = elapsed;
                }
                if (cfg.preview) {
                    // Encoding uses the full-size composition; the UI needs
                    // only a small preview, even on a recording sample. Once
                    // the video frame is written, failures here are disposable
                    // preview failures and must not truncate that recording.
                    previewOnlyWork = true;
                    if (captureDue && !compose(needDesktop ? &desktop : nullptr, needCamera ? &webcam : nullptr,
                            cfg.layers, 640, 360, preparePreviewBuffer(), error)) {
                        publishPreviewError(error);
                        continue;
                    }
                }
                { std::lock_guard<std::mutex> lock(mutex_);
                  // A selection change, Refresh, or Record may have retired
                  // this disposable preview while capture was in flight.
                  if (previewGeneration != previewGeneration_) continue;
                  if (cfg.preview && settings_.preview) {
                      auto previous = std::move(status_.preview);
                      status_.preview = previewBuffer;
                      previewBuffer = std::const_pointer_cast<Frame>(previous);
                  }
                  if (previewProblem_) {
                      status_.error = false;
                      status_.message = writing
                          ? paused ? L"Paused. Resume when you are ready." : L"Recording. You can adjust the collage live."
                          : L"Ready to record.";
                      previewProblem_ = false;
                  } }
            } catch (const std::exception&) {
                if (previewOnlyWork) {
                    // Preview buffers are disposable. Do not close a healthy
                    // recording or camera when only this refresh ran out of
                    // resources. Due-frame allocation failures below still
                    // finalize the recording and retain their error message.
                    try { publishPreviewError(L"The preview could not allocate a resource. It will retry."); } catch (...) {}
                    std::unique_lock<std::mutex> lock(mutex_);
                    waitAfterFailure(lock);
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
                    try { if (encoder) { std::wstring ignored; encoder->finish(ignored); } } catch (...) {}
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!start_) {
                        previewProblem_ = false;
                        status_.state = State::Idle; status_.error = true;
                        status_.recordingFailed = true;
                        try { status_.message = L"Capture stopped. Check the save folder and try recording again."; } catch (...) {}
                    }
                }
                if (camera) camera->stop();
                cameraRunning = false;
                writing = pending = paused = false;
                desktop = {}; webcam = {}; composed = {}; previewBuffer.reset();
                power.update(false, false);
                std::unique_lock<std::mutex> lock(mutex_);
                previewProblem_ = false;
                status_.preview.reset();
                waitAfterFailure(lock);
            }
        }
        if (camera) camera->stop();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}
}

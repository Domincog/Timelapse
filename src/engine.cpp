#include "engine.h"
#include "config.h"
#include "capture.h"
#include "camera_host.h"
#include "encoder.h"
#include <objbase.h>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <optional>
#include <array>

namespace lapse {
using Clock = std::chrono::steady_clock;
namespace {
const wchar_t* recordingMessage(bool separateFiles) {
    return separateFiles ? L"Recording desktop and camera to separate files." : L"Recording. You can adjust the collage live.";
}
bool usesNightCamera(const Settings& settings) {
    return settings.night.enabled && (settings.separateFiles || std::any_of(settings.layers.begin(), settings.layers.end(),
        [](const Layer& layer) { return layer.source == Source::Camera; }));
}
int nightWindowDuration(const Settings& settings, int suggested) {
    const int maximum = std::min(NightMaxDurationMs, settings.intervalMs);
    return settings.night.durationMs > 0 ? settings.night.durationMs : std::clamp(suggested, NightMinDurationMs, maximum);
}
bool validateNightCapture(const Settings& settings, std::wstring& error) {
    if (settings.intervalMs < NightMinDurationMs) {
        error = L"Night camera needs a capture interval of at least 1 second, including Auto blend duration.";
        return false;
    }
    if (!validNightSettings(settings.night) || settings.night.durationMs > settings.intervalMs) {
        error = L"Night camera needs a valid brightness and a blend duration no longer than Capture every.";
        return false;
    }
    return true;
}
// Leave room for buffered samples and MP4 finalization instead of waiting for
// the writer to fail on a full volume. This is a conservative policy, not a
// reservation: another process can consume the remaining space after a check.
constexpr ULONGLONG RecordingReservePerVideo = 64ULL * 1024 * 1024;
std::wstring recordingDirectoryIO(const std::wstring& absoluteFolder) {
    auto result = fileIOPath(absoluteFolder);
    // UNC share roots require a trailing separator for GetDiskFreeSpaceEx.
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') result += L'\\';
    return result;
}
struct RecordingSpace {
    ULONGLONG available = 0;
    DWORD failure = ERROR_SUCCESS;
    bool enough(bool separateFiles) const { return failure == ERROR_SUCCESS && available > RecordingReservePerVideo * (separateFiles ? 2 : 1); }
};
RecordingSpace queryRecordingSpace(const std::wstring& folderIO) {
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(folderIO.c_str(), &available, nullptr, nullptr)) {
        const DWORD failure = GetLastError();
        return {0, failure ? failure : ERROR_GEN_FAILURE};
    }
    // Caller-available bytes account for quotas; total volume free space can
    // be much larger. Check the combined reserve before admitting either write.
    return {available.QuadPart, ERROR_SUCCESS};
}
std::wstring recordingSpaceProblem(const RecordingSpace& space, bool separateFiles) {
    if (space.failure != ERROR_SUCCESS)
        return L"Cannot check available space in the save folder. " + errorText(HRESULT_FROM_WIN32(space.failure));
    return std::wstring(L"The save folder needs more than ") + (separateFiles ? L"128" : L"64") +
        L" MiB available to leave room for finishing the video. Free space or choose another folder.";
}
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
      auto sources = [](const Settings& config) { int mask = config.separateFiles ? 3 : 0; for (const auto& layer : config.layers) mask |= layer.source == Source::Desktop ? 1 : 2; return mask; };
      const int mask = sources(s);
      if (!s.preview || (status_.state == State::Idle && (s.width != settings_.width || s.height != settings_.height)) ||
          mask != sources(settings_) || ((mask & 2) && s.cameraId != settings_.cameraId) ||
          ((mask & 1) && (CompareStringOrdinal(s.monitorId.c_str(), -1, settings_.monitorId.c_str(), -1, TRUE) != CSTR_EQUAL ||
              !EqualRect(&s.monitor, &settings_.monitor)))) retirePreview();
      settings_ = s;
      ++settingsRevision_; }
    wake_.notify_one();
}
void Engine::retirePreview() {
    ++previewGeneration_;
    status_.preview.reset();
    if (!previewProblem_) return;
    previewProblem_ = false;
    status_.error = false;
    status_.message = status_.state == State::Recording ? recordingMessage(settings_.separateFiles)
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
      status_.message = L"Preparing recording..."; status_.frames = 0; status_.elapsed = 0; status_.savedPath.clear(); status_.savedPaths.clear();
      status_.nightEnabled = usesNightCamera(settings_); status_.nightWaiting = status_.nightEnabled;
      status_.nightDurationMs = 0; status_.night = {}; }
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
        std::optional<Encoder> cameraEncoder;
        RecordingPower power;
        std::optional<Settings> session;
        // Capture and encode storage belongs only to the worker. Retain its
        // allocation across samples instead of allocating several megabytes on
        // every refresh. Published preview frames never share this storage.
        Frame desktop, webcam, composed, cameraComposed;
        std::vector<Layer> desktopLayers, cameraLayers;
        std::shared_ptr<Frame> previewBuffer;
        auto preparePreviewBuffer = [&]() -> Frame& {
            // Keep at most one retired frame for reuse. A UI snapshot can keep
            // an older frame alive, so recycle only with exclusive ownership.
            if (!previewBuffer || previewBuffer.use_count() != 1) previewBuffer = std::make_shared<Frame>();
            return *previewBuffer;
        };
        std::wstring activeCamera, temporary, finalPath, temporaryIO, finalPathIO, sourceError, recordingFolder, recordingFolderIO;
        std::wstring cameraTemporary, cameraFinalPath, cameraTemporaryIO, cameraFinalPathIO;
        bool cameraRunning = false, pending = false, writing = false, paused = false;
        bool cameraWriting = false;
        bool nightMode = false, nightQueued = false, nightFrameReady = false;
        uint64_t nightToken = 0;
        int nightDurationMs = NightMinDurationMs, suggestedNightDurationMs = NightInitialDurationMs;
        auto nightStartAt = Clock::now(), nightPollAt = Clock::time_point::max(), nightExpectedAt = Clock::time_point::max();
        NightWindowResult completedNight;
        auto cancelNight = [&](bool clearFrame = true) {
            if (camera && nightMode) camera->cancelNight();
            nightQueued = false;
            if (clearFrame) nightFrameReady = false;
            nightPollAt = nightExpectedAt = Clock::time_point::max();
        };
        uint64_t previewGeneration = 0, cameraAttemptGeneration = 0;
        uint64_t settingsRevision = UINT64_MAX;
        auto lastPreview = Clock::time_point::min(), nextFrame = Clock::now();
        auto retryFrameAt = Clock::time_point::min();
        auto lastTick = Clock::now();
        Clock::duration activeDuration{};
        double elapsed = 0;
        bool recordingLimitReached = false;
        auto advanceElapsed = [&](Clock::time_point now) {
            if (writing && !paused) activeDuration += now - lastTick;
            lastTick = now;
            elapsed = std::chrono::duration<double>(activeDuration).count();
        };
        auto recordingDeadline = [&] {
            return writing && !paused && session && session->recordingLimitSeconds > 0
                ? lastTick + (std::chrono::seconds(session->recordingLimitSeconds) - activeDuration)
                : Clock::time_point::max();
        };
        auto limitExpired = [&] {
            return writing && !paused && session && session->recordingLimitSeconds > 0 &&
                activeDuration >= std::chrono::seconds(session->recordingLimitSeconds);
        };
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
            cancelNight(); nightMode = false;
            if (writing) power.saving();
            bool keepCamera;
            { std::lock_guard<std::mutex> lock(mutex_); status_.state = State::Finishing; status_.nightWaiting = false; status_.message = L"Finishing MP4..."; keepCamera = settings_.preview; }
            // Hidden idle workers have no next tick on which to release the
            // device. Finish/cancel must stop it before returning to that wait.
            if (!keepCamera && cameraRunning) { camera->stop(); cameraRunning = false; activeCamera.clear(); }
            if (session && session->separateFiles) {
                std::array<std::wstring, 2> errors;
                const std::array<bool, 2> opened{writing, cameraWriting};
                const std::array<Encoder*, 2> writers{encoder ? &*encoder : nullptr, cameraEncoder ? &*cameraEncoder : nullptr};
                const std::array<const std::wstring*, 2> temporaryNames{&temporary, &cameraTemporary};
                const std::array<const std::wstring*, 2> finalNames{&finalPath, &cameraFinalPath};
                const std::array<const std::wstring*, 2> temporaryNamesIO{&temporaryIO, &cameraTemporaryIO};
                const std::array<const std::wstring*, 2> finalNamesIO{&finalPathIO, &cameraFinalPathIO};
                std::array<bool, 2> finalized{}, partial{};
                std::array<uint64_t, 2> frameCounts{};
                for (size_t i = 0; i < writers.size(); ++i) if (opened[i]) {
                    frameCounts[i] = writers[i]->frames();
                    finalized[i] = writers[i]->finishForPublication(errors[i]);
                    const DWORD attributes = !finalized[i] && frameCounts[i] ? GetFileAttributesW(temporaryNamesIO[i]->c_str()) : INVALID_FILE_ATTRIBUTES;
                    partial[i] = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
                }
                struct Report { std::wstring first, message; std::vector<std::wstring> paths; bool error = false; };
                std::array<Report, 4> reports;
                // Prepare every possible publication result before the first
                // rename. A later writer's allocation failure can then report
                // the first saved movie and the second retained movie without
                // allocating or losing the identity-bound ownership guards.
                for (size_t mask = 0; mask < reports.size(); ++mask) {
                    auto& report = reports[mask];
                    report.error = !reason.empty();
                    report.message = reason.empty() && recordingLimitReached ? L"Recording time limit reached." : reason;
                    for (size_t i = 0; i < writers.size(); ++i) {
                        if (!report.message.empty()) report.message += L" ";
                        report.message += i == 0 ? L"Desktop: " : L"Camera: ";
                        if (finalized[i]) {
                            const bool renamed = (mask & (size_t(1) << i)) != 0;
                            const auto& path = *(renamed ? finalNames[i] : temporaryNames[i]);
                            report.paths.push_back(path);
                            report.message += renamed ? L"saved " : L"could not publish the final filename; finished video remains at ";
                            report.message += path + L" (" + std::to_wstring(frameCounts[i]) + L" frames).";
                            report.error |= !renamed;
                        } else if (frameCounts[i]) {
                            report.error = true;
                            report.message += L"could not finish video: " + errors[i];
                            if (partial[i]) report.message += L" Partial file: " + *temporaryNames[i];
                        } else {
                            report.message += L"no frames saved.";
                            report.error |= writing && !reason.empty();
                        }
                    }
                    if (!report.paths.empty()) report.first = report.paths.front();
                }
                size_t publicationMask = 0;
                for (size_t i = 0; i < writers.size(); ++i) if (finalized[i]) {
                    try {
                        if (writers[i]->publish(*finalNamesIO[i]) == ERROR_SUCCESS) publicationMask |= size_t(1) << i;
                    } catch (const std::exception&) {
                        // publish() allocates only before its native rename.
                        // Both finalized files are still accurately described
                        // by the precomputed retained-file result for this bit.
                    }
                }
                auto& report = reports[publicationMask];
                std::lock_guard<std::mutex> lock(mutex_);
                stop_ = pauseRequested_ = pauseTarget_ = false;
                status_.state = State::Idle; status_.elapsed = elapsed;
                status_.frames = std::min(frameCounts[0], frameCounts[1]);
                status_.error = status_.recordingFailed = report.error;
                status_.savedPath.swap(report.first); status_.savedPaths.swap(report.paths); status_.message.swap(report.message);
                if (encoder) encoder->releasePublication();
                if (cameraEncoder) cameraEncoder->releasePublication();
                writing = cameraWriting = pending = paused = false; previewProblem_ = false;
                recordingLimitReached = false;
                desktop = {}; webcam = {}; composed = {}; cameraComposed = {};
                power.update(false, false);
                return;
            }
            std::wstring error;
            const bool finalized = writing && encoder->finishForPublication(error);
            bool ok = !writing;
            std::wstring savedPath, message;
            std::vector<std::wstring> savedPaths;
            const std::wstring failureContext = reason.empty()
                ? (recordingLimitReached ? L"Recording time limit reached. " : L"") : reason + L" ";
            if (finalized) {
                // Prepare every allocating success update before publishing the
                // file. Once renamed, status publication below cannot allocate.
                savedPath = finalPath;
                savedPaths.push_back(finalPath);
                message = reason.empty() ? (recordingLimitReached ? L"Recording time limit reached. Saved: " : L"Saved: ") + finalPath
                    : reason + L" Captured frames were saved.";
                const DWORD renameError = encoder->publish(finalPathIO);
                ok = renameError == ERROR_SUCCESS;
                if (!ok) {
                    savedPath = temporary;
                    savedPaths.front() = temporary;
                    message = failureContext + L"Video finished, but could not rename it. Finished video remains at: " + temporary + L". " + errorText(HRESULT_FROM_WIN32(renameError));
                }
            } else if (writing) {
                const DWORD attributes = encoder->frames() > 0 ? GetFileAttributesW(temporaryIO.c_str()) : INVALID_FILE_ATTRIBUTES;
                const bool retainedPartial = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
                message = failureContext + L"Could not finish video: " + error + (retainedPartial ? L" Partial file: " + temporary : L"");
            } else message = reason.empty() ? L"Recording cancelled." : reason;
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = pauseRequested_ = pauseTarget_ = false;
            status_.state = State::Idle;
            status_.elapsed = elapsed;
            status_.error = !reason.empty() || !ok;
            status_.recordingFailed = status_.error;
            // Finalize and rename remain separate outcomes: a failed rename
            // advertises the complete playable movie at its temporary filename.
            if (finalized) { status_.savedPath.swap(savedPath); status_.savedPaths.swap(savedPaths); }
            status_.message.swap(message);
            if (encoder) encoder->releasePublication();
            writing = pending = paused = false; previewProblem_ = false;
            recordingLimitReached = false;
            desktop = {}; webcam = {}; composed = {};
            power.update(false, false);
        };
        auto waitAfterFailure = [&](std::unique_lock<std::mutex>& lock) {
            // Commands already queued at failure remain pending, but must not
            // turn persistent allocation failure into an immediate retry loop.
            const auto generation = previewGeneration_;
            const auto revision = settingsRevision_;
            const bool start = start_, stop = stop_, pauseRequested = pauseRequested_, pauseTarget = pauseTarget_, retry = retrySources_;
            wake_.wait_until(lock, std::min(Clock::now() + std::chrono::seconds(1), recordingDeadline()), [&] {
                return quit_ || previewGeneration_ != generation || settingsRevision_ != revision || start_ != start ||
                    stop_ != stop || pauseRequested_ != pauseRequested || pauseTarget_ != pauseTarget || retrySources_ != retry;
            });
        };
        for (;;) {
            bool previewOnlyWork = false;
            try {
                std::optional<Settings> snapshot;
                bool quit, start = false, stop = false, pauseRequested = false, pauseTarget = false, retry = false;
                { std::unique_lock<std::mutex> lock(mutex_);
                  // Sleep until useful work is due. Configuration revisions
                  // also catch notifications sent while capture was in flight.
                  // Hidden idle/paused sessions need no periodic worker tick.
                  auto deadline = Clock::time_point::max();
                  if (settings_.preview) {
                      deadline = lastPreview == Clock::time_point::min() ? Clock::now()
                          : lastPreview + std::chrono::milliseconds(writing ? 1000 : 500);
                  }
                  if (pending || (writing && !paused)) {
                      const auto due = nightMode ? (nightQueued ? nightPollAt : nightStartAt) : nextFrame;
                      deadline = std::min(deadline, std::max(due, retryFrameAt));
                  }
                  // The displayed recording clock uses whole seconds. Commands
                  // wake immediately and publish their precise elapsed time.
                  if (writing && !paused) deadline = std::min(deadline, lastTick + std::chrono::seconds(1));
                  deadline = std::min(deadline, recordingDeadline());
                  if (FAILED(com)) deadline = Clock::now() + std::chrono::seconds(1);
                  auto commanded = [&] {
                      return quit_ || start_ || stop_ || pauseRequested_ || retrySources_ || settingsRevision_ != settingsRevision;
                  };
                  if (deadline == Clock::time_point::max()) wake_.wait(lock, commanded);
                  else wake_.wait_until(lock, deadline, commanded);
                  // Shutdown needs no settings allocation. Capture operation
                  // ownership before copying, and consume commands only after
                  // their settings snapshot has succeeded.
                  quit = quit_;
                  previewGeneration = previewGeneration_;
                  const auto requiredWork = nightMode ? (nightQueued ? nightPollAt : nightStartAt) : nextFrame;
                  previewOnlyWork = !start_ && !stop_ && !quit_ && !pending &&
                      (!writing || paused || Clock::now() < requiredWork) && Clock::now() < recordingDeadline();
                  if (!quit) {
                      snapshot.emplace(settings_);
                      settingsRevision = settingsRevision_;
                      start = start_; stop = stop_; pauseRequested = pauseRequested_; pauseTarget = pauseTarget_; retry = retrySources_;
                      start_ = stop_ = pauseRequested_ = retrySources_ = false;
                  } }
                const auto now = Clock::now();
                advanceElapsed(now);
                // Preview deadlines can coincide with every clock deadline.
                // Publish time even when this wake also refreshes the image.
                if (writing) { std::lock_guard<std::mutex> lock(mutex_); status_.elapsed = elapsed; }
                if (quit) { if (writing || pending) { previewOnlyWork = false; closeRecording(L""); } break; }
                Settings& cfg = *snapshot;
                if (retry) { if (camera) camera->stop(); cameraRunning = false; activeCamera.clear(); }
                if (FAILED(com)) { publishError(L"Windows media initialization failed: " + errorText(com), true); continue; }
                if (start) {
                    previewOnlyWork = false;
                    session.emplace(cfg); pending = true; elapsed = 0;
                    activeDuration = Clock::duration::zero(); recordingLimitReached = false;
                    cancelNight(); nightMode = usesNightCamera(cfg);
                    suggestedNightDurationMs = NightInitialDurationMs;
                    nightStartAt = now;
                    std::wstring validationError;
                    if (!validateCaptureInterval(cfg.intervalMs, validationError) ||
                        !validateVideoSize(cfg.width, cfg.height, validationError) ||
                        (nightMode && !validateNightCapture(cfg, validationError))) {
                        closeRecording(validationError); continue;
                    }
                    if (cfg.separateFiles) { desktopLayers = preset(Mode::Desktop); cameraLayers = preset(Mode::Camera); }
                    nextFrame = now;
                    retryFrameAt = Clock::time_point::min();
                }
                if (stop && (pending || writing)) { previewOnlyWork = false; closeRecording(L""); continue; }
                // The time limit takes precedence over a sample due at exactly
                // the same instant, and remains active during source retries.
                if (limitExpired()) {
                    previewOnlyWork = false; recordingLimitReached = true;
                    closeRecording(L""); continue;
                }
                if (pauseRequested && writing && paused != pauseTarget) {
                    // Retain a completed processed preview while dropping only
                    // in-flight integration. A stopped/changed camera below
                    // still clears it together with its capture storage.
                    cancelNight(false);
                    paused = pauseTarget;
                    const bool desktopNeeded = (session && session->separateFiles) || std::any_of(cfg.layers.begin(), cfg.layers.end(),
                        [](const Layer& layer) { return layer.source == Source::Desktop; });
                    power.update(!paused, desktopNeeded);
                    if (!paused) { nextFrame = now; nightStartAt = now; retryFrameAt = Clock::time_point::min(); }
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.state = paused ? State::Paused : State::Recording;
                    status_.nightWaiting = nightMode && !paused;
                    status_.elapsed = elapsed;
                    if (!previewProblem_)
                        status_.message = paused ? L"Paused. Resume when you are ready." : recordingMessage(session && session->separateFiles);
                }
                bool captureDue = pending || (writing && !paused && now >= nextFrame);
                previewOnlyWork = !captureDue;
                if (writing || pending) {
                    if (!session->separateFiles) session->layers = cfg.layers;
                    session->preview = cfg.preview;
                    cfg = *session;
                }
                bool needCamera = cfg.separateFiles, needDesktop = cfg.separateFiles;
                for (auto& layer : cfg.layers) { needCamera |= layer.source == Source::Camera; needDesktop |= layer.source == Source::Desktop; }
                const bool useNight = (pending || writing) && cfg.night.enabled && needCamera;
                if (useNight != nightMode) {
                    cancelNight(); nightMode = useNight; nightStartAt = now;
                    std::wstring validationError;
                    if (useNight && !validateNightCapture(cfg, validationError)) {
                        previewOnlyWork = false;
                        closeRecording(validationError); continue;
                    }
                    std::lock_guard<std::mutex> lock(mutex_); status_.nightEnabled = useNight; status_.nightWaiting = useNight && !paused;
                }
                power.update(writing && !paused, needDesktop);
                const bool active = pending || (writing && !paused) || cfg.preview;
                if (!cfg.preview) previewBuffer.reset();
                if (!active) { desktop = {}; webcam = {}; composed = {}; cameraComposed = {}; }
                else {
                    if (!needDesktop) desktop = {};
                    if (!needCamera) webcam = {};
                }
                if ((!needCamera || !active || activeCamera != cfg.cameraId) && cameraRunning) {
                    cancelNight();
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
                bool nightCompleted = false, firstNightPreview = false;
                if (nightMode && !paused) {
                    // The helper owns the integration window and intermediate
                    // pixels. Parent wakes only for commands, preview, the
                    // recording clock, or this bounded completion observation.
                    if (cameraRunning && !nightQueued && now >= nightStartAt && now >= retryFrameAt) {
                        previewOnlyWork = false;
                        nightDurationMs = nightWindowDuration(cfg, suggestedNightDurationMs);
                        if (++nightToken == 0) ++nightToken;
                        std::wstring nightError;
                        const bool begun = camera->beginNight(nightToken, static_cast<uint32_t>(nightDurationMs), cfg.night, nightError);
                        advanceElapsed(Clock::now());
                        if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); continue; }
                        if (begun) {
                            nightQueued = true;
                            nightExpectedAt = Clock::now() + std::chrono::milliseconds(nightDurationMs);
                            nightPollAt = std::min(nightExpectedAt, Clock::now() + std::chrono::seconds(1));
                            retryFrameAt = Clock::time_point::min();
                            std::lock_guard<std::mutex> lock(mutex_); status_.nightWaiting = true;
                        } else if (!nightError.empty()) { closeRecording(L"Night camera stopped: " + nightError); continue; }
                        else retryFrameAt = Clock::now() + std::chrono::milliseconds(50);
                    }
                    if (cameraRunning && nightQueued && Clock::now() >= nightPollAt) {
                        previewOnlyWork = false;
                        std::wstring nightError;
                        const bool complete = camera->nightResult(nightToken, webcam, completedNight, nightError);
                        advanceElapsed(Clock::now());
                        if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); continue; }
                        if (complete) {
                            firstNightPreview = !nightFrameReady;
                            nightQueued = false; nightFrameReady = nightCompleted = true;
                            retryFrameAt = Clock::time_point::min();
                        } else if (!nightError.empty()) { closeRecording(L"Night camera stopped: " + nightError); continue; }
                        else {
                            const auto observed = Clock::now();
                            nightPollAt = observed >= nightExpectedAt ? observed + std::chrono::milliseconds(50)
                                : std::min(nightExpectedAt, observed + std::chrono::seconds(1));
                        }
                    }
                    captureDue = nightCompleted;
                    if (!cameraRunning) { closeRecording(sourceError.empty() ? L"No camera available for night recording." : sourceError); continue; }
                }
                if (!captureDue && !previewDue) {
                    continue;
                }
                previewOnlyWork = !captureDue;
                // Recording cadence can be much faster than preview cadence.
                // Only a requested preview attempt advances its clock. The
                // first processed Night window must promptly replace raw video.
                const bool refreshPreview = previewDue || (cfg.preview && firstNightPreview);
                if (refreshPreview) lastPreview = now;
                std::wstring error;
                bool ready = true, cameraWaiting = false;
                if (needCamera && !(nightMode && nightFrameReady)) {
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
                const auto previewSize = previewDimensions(cfg.width, cfg.height);
                const int width = captureDue ? cfg.width : previewSize.first, height = captureDue ? cfg.height : previewSize.second;
                if (needDesktop && ready) ready = captureMonitor(cfg.monitorId, width, height, true, desktop, error);
                if (ready && captureDue && cfg.separateFiles) {
                    ready = compose(&desktop, nullptr, desktopLayers, width, height, composed, error) &&
                        compose(nullptr, &webcam, cameraLayers, width, height, cameraComposed, error);
                } else if (ready) ready = compose(needDesktop ? &desktop : nullptr, needCamera ? &webcam : nullptr, cfg.layers,
                    width, height, captureDue ? composed : preparePreviewBuffer(), error);
                if (!ready) {
                    // Also allow warmup after resuming with preview disabled:
                    // that pause releases the camera and resume opens it again.
                    // An empty result is a transient wait (including shared
                    // frame contention). The client owns activation/staleness
                    // deadlines and returns a concrete error when they expire.
                    if (cameraWaiting) {
                        // Only a requested recording sample needs a prompt
                        // retry. Ordinary preview warmup keeps its own cadence.
                        if (captureDue) retryFrameAt = Clock::now() + std::chrono::milliseconds(50);
                        continue;
                    }
                    if (error.empty()) error = L"The capture source did not provide a frame. Check its connection or select another source.";
                    // A preview refresh is not a requested video frame. Keep
                    // the encoder open through transient preview failures,
                    // including while paused; a due capture must still fail
                    // visibly and finalize all frames already written.
                    if (captureDue) closeRecording(L"Capture stopped: " + error);
                    else publishPreviewError(error);
                    continue;
                }
                if (pending && captureDue) {
                    std::error_code ec;
                    if (cfg.folder.empty()) { closeRecording(L"Cannot create the save folder. Choose another folder."); continue; }
                    // Resolve relative folders once. Both writers, publication
                    // destinations and space checks use this session directory.
                    const auto folder = std::filesystem::absolute(std::filesystem::path(cfg.folder), ec);
                    if (ec) { closeRecording(L"Cannot resolve the save folder. Choose another folder."); continue; }
                    recordingFolder = folder.lexically_normal().wstring();
                    recordingFolderIO = recordingDirectoryIO(recordingFolder);
                    std::filesystem::create_directories(recordingFolderIO, ec);
                    if (ec) { closeRecording(L"Cannot create the save folder. Choose another folder."); continue; }
                    if (cfg.stopOnLowDiskSpace) {
                        const auto space = queryRecordingSpace(recordingFolderIO);
                        if (!space.enough(cfg.separateFiles)) {
                            closeRecording(L"Cannot start recording: " + recordingSpaceProblem(space, cfg.separateFiles) + L" Save folder: " + recordingFolder); continue;
                        }
                    }
                    SYSTEMTIME t; GetLocalTime(&t);
                    wchar_t name[100];
                    swprintf_s(name, L"Timelapse-%04u%02u%02u-%02u%02u%02u-%03u-%lu", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
                    auto base = (std::filesystem::path(recordingFolder) / name).wstring();
                    const auto desktopBase = cfg.separateFiles ? base + L"-desktop" : base;
                    temporary = desktopBase + L".recording.mp4"; finalPath = desktopBase + L".mp4";
                    // Cache I/O spellings before opening: successful save
                    // publication must not allocate after the file is renamed.
                    const auto ioBase = recordingFolderIO + name;
                    const auto desktopIOBase = cfg.separateFiles ? ioBase + L"-desktop" : ioBase;
                    temporaryIO = desktopIOBase + L".recording.mp4"; finalPathIO = desktopIOBase + L".mp4";
                    if (cfg.separateFiles) {
                        cameraTemporary = base + L"-camera.recording.mp4"; cameraFinalPath = base + L"-camera.mp4";
                        cameraTemporaryIO = ioBase + L"-camera.recording.mp4"; cameraFinalPathIO = ioBase + L"-camera.mp4";
                    }
                    if (!encoder) encoder.emplace();
                    if (!encoder->open(temporaryIO, cfg.width, cfg.height, 30, error, cfg.encodingQuality, cfg.encodingMode)) {
                        closeRecording((cfg.separateFiles ? L"Cannot start desktop recording: " : L"Cannot start recording: ") + error); continue;
                    }
                    writing = true;
                    if (cfg.separateFiles) {
                        if (!cameraEncoder) cameraEncoder.emplace();
                        if (!cameraEncoder->open(cameraTemporaryIO, cfg.width, cfg.height, 30, error, cfg.encodingQuality, cfg.encodingMode)) {
                            closeRecording(L"Cannot start camera recording: " + error); continue;
                        }
                        cameraWriting = true;
                    }
                    pending = false;
                    // Initial source warmup and opening either writer are
                    // preparation, not active recording time. Anchor sampling
                    // and the optional deadline to the first admitted frame.
                    lastTick = Clock::now(); nextFrame = lastTick;
                    power.update(true, needDesktop);
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.state = State::Recording; status_.error = false; status_.message = recordingMessage(cfg.separateFiles);
                }
                if (captureDue && writing && !paused) {
                    const bool firstSample = encoder->frames() == 0;
                    if (!firstSample) advanceElapsed(Clock::now());
                    // Preserve successful time-limit completion if capture
                    // already used the remaining budget, without querying disk.
                    if (!firstSample && limitExpired()) {
                        recordingLimitReached = true; closeRecording(L""); continue;
                    }
                    // Capture and writer startup can take time or consume
                    // space. Recheck after both finish, before either sample
                    // is submitted, and fail closed if the query is unavailable.
                    // Read the result without allocating diagnostics. An expired
                    // deadline wins even when a slow storage query fails.
                    const auto space = cfg.stopOnLowDiskSpace ? queryRecordingSpace(recordingFolderIO) : RecordingSpace{};
                    if (firstSample) {
                        // The first space check after writer opening is still
                        // startup. Start active time only when admission ends.
                        activeDuration = Clock::duration::zero(); elapsed = 0;
                        lastTick = Clock::now(); nextFrame = lastTick;
                    } else advanceElapsed(Clock::now());
                    // A slow storage query may also cross the time limit.
                    // Admit both separate writes together, or neither of them.
                    if (limitExpired()) {
                        recordingLimitReached = true; closeRecording(L""); continue;
                    }
                    if (cfg.stopOnLowDiskSpace && !space.enough(cfg.separateFiles)) {
                        closeRecording(L"Recording stopped: " + recordingSpaceProblem(space, cfg.separateFiles) + L" Save folder: " + recordingFolder); continue;
                    }
                    if (!encoder->write(composed, error)) {
                        closeRecording((cfg.separateFiles ? L"Desktop recording stopped: " : L"Recording stopped: ") + error); continue;
                    }
                    if (cfg.separateFiles && !cameraEncoder->write(cameraComposed, error)) {
                        closeRecording(L"Camera recording stopped: " + error); continue;
                    }
                    const auto interval = std::chrono::milliseconds(cfg.intervalMs);
                    if (nightMode) {
                        suggestedNightDurationMs = completedNight.exposure.suggestedDurationMs;
                        const auto duration = std::chrono::milliseconds(nightWindowDuration(cfg, suggestedNightDurationMs));
                        // Keep complete windows when a start/encode runs late.
                        // No catch-up burst or millisecond-late whole-slot skip.
                        nextFrame = std::max(nextFrame + interval, Clock::now() + duration);
                        nightStartAt = nextFrame - duration;
                    } else {
                        nextFrame += interval;
                        const auto completedAt = Clock::now();
                        // Skip elapsed slots in constant time even for a short
                        // cadence after a slow operation or long system sleep.
                        if (nextFrame <= completedAt)
                            nextFrame += interval * ((completedAt - nextFrame) / interval + 1);
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.frames = encoder->frames(); status_.elapsed = elapsed;
                    if (nightMode) {
                        status_.night = completedNight.exposure;
                        status_.nightDurationMs = static_cast<uint32_t>(completedNight.endTick - completedNight.beginTick);
                        status_.nightWaiting = false;
                    }
                }
                if (refreshPreview) {
                    // Encoding uses the full-size composition; the UI needs
                    // only a small preview, even on a recording sample. Once
                    // the video frame is written, failures here are disposable
                    // preview failures and must not truncate that recording.
                    previewOnlyWork = true;
                    if (captureDue && !compose(needDesktop ? &desktop : nullptr, needCamera ? &webcam : nullptr,
                            cfg.layers, previewSize.first, previewSize.second, preparePreviewBuffer(), error)) {
                        publishPreviewError(error);
                        continue;
                    }
                }
                // A successful video admission is not evidence that a failed
                // disposable preview recovered, nor does it own a new buffer.
                if (!refreshPreview) continue;
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
                          ? paused ? L"Paused. Resume when you are ready." : recordingMessage(cfg.separateFiles)
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
                    try { if (cameraEncoder) { std::wstring ignored; cameraEncoder->finish(ignored); } } catch (...) {}
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!start_) {
                        previewProblem_ = false;
                        status_.state = State::Idle; status_.error = true;
                        status_.nightWaiting = false;
                        status_.recordingFailed = true;
                        try { status_.message = L"Capture stopped. Check the save folder and try recording again."; } catch (...) {}
                    }
                }
                if (camera) camera->stop();
                cameraRunning = false;
                nightMode = nightQueued = nightFrameReady = false;
                writing = cameraWriting = pending = paused = false;
                desktop = {}; webcam = {}; composed = {}; cameraComposed = {}; previewBuffer.reset();
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

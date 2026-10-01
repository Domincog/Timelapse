#include "engine.h"
#include "config.h"
#include "capture.h"
#include "camera_host.h"
#include "encoder.h"
#include "person_client.h"
#include <objbase.h>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <optional>
#include <array>
#include <limits>
#include <stdexcept>
#include <cstring>

namespace lapse {
using Clock = std::chrono::steady_clock;
namespace {
uint64_t segmentFrameTotal(uint64_t closed, uint64_t current) {
    if (current > UINT64_MAX - closed) throw std::overflow_error("Recording frame count exhausted");
    return closed + current;
}
Clock::duration nextSegmentCut(Clock::duration active, int seconds) {
    const auto span = std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(seconds));
    const auto bucket = active / span;
    if (bucket >= Clock::duration::max() / span) throw std::overflow_error("Recording segment clock exhausted");
    return span * (bucket + 1);
}
const wchar_t* recordingMessage(bool separateFiles) {
    return separateFiles ? L"Recording desktop and camera to separate files." : L"Recording. You can adjust the collage live.";
}
bool usesNightCamera(const Settings& settings) {
    return settings.night.enabled && (settings.separateFiles || std::any_of(settings.layers.begin(), settings.layers.end(),
        [](const Layer& layer) { return layer.source == Source::Camera; }));
}
bool usesPersonChecks(TimeSkipMode mode) noexcept {
    return mode == TimeSkipMode::NoPerson || mode == TimeSkipMode::NoPersonWithinSchedule;
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
bool validateRecordingSettings(Settings& settings, std::wstring& error) {
    if (settings.startDelaySeconds < 0 || settings.startDelaySeconds > 300)
        error = L"Delay next recording must be between 0 and 300 whole seconds.";
    else if (settings.segmentDurationSeconds < 0)
        error = L"Split files every must be Off or a positive whole number of seconds.";
    return error.empty() && validateCaptureInterval(settings.intervalMs, error) &&
        validateOutputFps(settings.outputFps, error) &&
        validateVideoSize(settings.width, settings.height, error) &&
        validateEncodingMode(settings.encodingMode, settings.recoveryMode, error) &&
        (!usesNightCamera(settings) || validateNightCapture(settings, error)) &&
        normalizeTimeSkipSettings(settings.timeSkip, error) && validateWatermarkSettings(settings.watermark, error);
}
bool queryDelayedWakeEpoch(uint64_t& epoch, std::wstring& error) {
    // No binding or query on the default immediate path. Own the system-only
    // module for each of the three bounded self-timer checkpoints.
    struct Module {
        HMODULE value;
        ~Module() { if (value) FreeLibrary(value); }
    } module{LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    using Query = LONG (WINAPI*)(POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID, ULONG);
    Query query{};
    const auto address = module.value ? GetProcAddress(module.value, "CallNtPowerInformation") : nullptr;
    static_assert(sizeof(query) == sizeof(address));
    std::memcpy(&query, &address, sizeof(query));
    if (!query || query(LastWakeTime, nullptr, 0, &epoch, sizeof(epoch)) != 0) {
        error = L"Cannot safely delay recording because Windows wake information is unavailable. Choose None to record immediately.";
        return false;
    }
    return true;
}
bool sameLayers(const std::vector<Layer>& a, const std::vector<Layer>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].source != b[i].source || a[i].rect.x != b[i].rect.x || a[i].rect.y != b[i].rect.y ||
            a[i].rect.w != b[i].rect.w || a[i].rect.h != b[i].rect.h) return false;
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
      const bool cameraTierChanged = status_.state == State::Idle && (mask & 2) &&
          cameraResolutionForOutput(s.width, s.height) != cameraResolutionForOutput(settings_.width, settings_.height);
      if (!sameWatermarkSettings(s.watermark, settings_.watermark) ||
          (status_.state == State::Idle && s.watermark.enabled && s.watermark.showSpeed && s.outputFps != settings_.outputFps) ||
          (status_.state == State::Idle && (mask & 1) && s.captureCursor != settings_.captureCursor)) retirePreview(true);
      if (mask != sources(settings_) || s.cameraId != settings_.cameraId || !s.preview || cameraTierChanged) retireCameraInput();
      if (!s.preview || (status_.state == State::Idle && (s.width != settings_.width || s.height != settings_.height)) ||
          mask != sources(settings_) || ((mask & 2) && s.cameraId != settings_.cameraId) ||
          ((mask & 1) && (CompareStringOrdinal(s.monitorId.c_str(), -1, settings_.monitorId.c_str(), -1, TRUE) != CSTR_EQUAL ||
              !EqualRect(&s.monitor, &settings_.monitor)))) retirePreview();
      settings_ = s;
      ++settingsRevision_; }
    wake_.notify_one();
}
void Engine::retireCameraInput() noexcept {
    if (++cameraInputGeneration_ == 0) ++cameraInputGeneration_;
    status_.cameraInput = {0, 0, cameraInputGeneration_};
}
void Engine::retirePreview(bool visualOnly) {
    // Visual policy edits invalidate pixels without retiring camera source evidence
    // or turning a failed camera attempt into an implicit device retry.
    if (visualOnly) ++visualPreviewGeneration_;
    else ++previewGeneration_;
    status_.preview.reset();
    if (!previewProblem_) return;
    previewProblem_ = false;
    status_.error = false;
    status_.message = status_.state == State::Recording ? recordingMessage(settings_.separateFiles)
        : status_.state == State::Paused ? L"Paused. Resume when you are ready."
        : status_.state == State::Starting ? L"Preparing recording..."
        : status_.state == State::Waiting ? L"Waiting to start recording..."
        : status_.state == State::Finishing ? L"Finishing MP4..." : L"Ready to record.";
}
void Engine::refreshSources() {
    { std::lock_guard<std::mutex> lock(mutex_);
      if (status_.state != State::Idle) return;
      // Source retry must not discard a recording's result or recovery path.
      // A later preview error remains independently retryable, even when the
      // prior recording still has saved paths.
      const bool retainOutcome = !previewProblem_ && (status_.recordingFailed ||
          !status_.savedPath.empty() || !status_.savedPaths.empty());
      retireCameraInput();
      retirePreview();
      retrySources_ = true; previewProblem_ = false;
      if (!retainOutcome) { status_.error = false; status_.message = L"Ready to record."; } }
    wake_.notify_one();
}
void Engine::record() {
    { std::unique_lock<std::mutex> lock(mutex_); if (status_.state != State::Idle) return;
      std::optional<Settings> request;
      std::wstring rejection;
      uint64_t wakeEpoch = 0;
      if (settings_.startDelaySeconds != 0) {
          // Build and check the request before replacing the previous result.
          request.emplace(settings_);
          lock.unlock();
          if (validateRecordingSettings(*request, rejection)) queryDelayedWakeEpoch(wakeEpoch, rejection);
          lock.lock();
          if (status_.state != State::Idle) return;
      }
      const bool delayed = request && rejection.empty();
      const int delay = request ? request->startDelaySeconds : 0;
      std::wstring message;
      if (request) message = !rejection.empty() ? std::move(rejection) : L"Waiting to start recording...";
      std::wstring cancellation;
      if (delayed) cancellation = L"Delayed start cancelled.";
      ++previewGeneration_; previewProblem_ = false;
      stop_ = pauseRequested_ = pauseTarget_ = false;
      requestedCursor_ = request ? request->captureCursor : settings_.captureCursor;
      requestedOutputFps_ = request ? request->outputFps : settings_.outputFps;
      const bool rejected = request && !delayed;
      delayedSettings_ = delayed ? std::move(request) : std::nullopt;
      delayedCancellationMessage_ = std::move(cancellation);
      delayedWakeEpoch_ = wakeEpoch; delayedStartPending_ = delayed; delayedCancel_ = false;
      start_ = !rejected; status_.state = rejected ? State::Idle : delayed ? State::Waiting : State::Starting;
      status_.error = status_.recordingFailed = rejected;
      status_.startDeadlineTick = delayed ? GetTickCount64() + uint64_t(delay) * 1000 : 0;
      if (request || rejected) status_.message = std::move(message); else status_.message = L"Preparing recording...";
      status_.frames = 0; status_.elapsed = 0; status_.savedPath.clear(); status_.savedPaths.clear();
      status_.completedSegments = 0;
      status_.nightEnabled = !delayed && !rejected && usesNightCamera(settings_); status_.nightWaiting = status_.nightEnabled;
      status_.nightDurationMs = 0; status_.night = {};
      status_.timeSkip = {}; }
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
      if (status_.state != State::Waiting && status_.state != State::Starting && status_.state != State::Recording && status_.state != State::Paused) return;
      stop_ = true; }
    wake_.notify_one();
}
void Engine::cancelDelayedStart() noexcept {
    { std::lock_guard<std::mutex> lock(mutex_);
      if (!delayedStartPending_) return;
      delayedCancel_ = true; stop_ = true; }
    wake_.notify_one();
}
Status Engine::status() { std::lock_guard<std::mutex> lock(mutex_); return status_; }
void Engine::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        // Construct allocating resources inside the guarded operation that
        // needs them, so construction failure cannot escape this worker.
        std::optional<CameraClient> camera;
        std::optional<PersonClient> personClient;
        std::unique_ptr<CameraPersonInput> personInput;
        std::optional<Encoder> encoder;
        std::optional<Encoder> cameraEncoder;
        RecordingPower power;
        std::optional<Settings> session;
        std::optional<Settings> delayedRequest;
        uint64_t delayedDeadline = 0, delayedEpoch = 0;
        bool delayedOrigin = false;
        bool delayedLayersFrozen = false;
        WatermarkRenderer watermark;
        WatermarkSettings preparedWatermark;
        int watermarkWidth = 0, watermarkHeight = 0;
        bool watermarkPrepared = false, haveWatermarkContext = false;
        WatermarkContext lastWatermarkContext;
        int64_t incomingIntervalMs = 5000;
        auto resetWatermark = [&]() noexcept {
            watermark.reset(); watermarkPrepared = haveWatermarkContext = false;
        };
        auto prepareWatermark = [&](const Settings& cfg, std::wstring& error) {
            if (!cfg.watermark.enabled) {
                if (watermarkPrepared) { watermark.reset(); watermarkPrepared = false; }
                return true;
            }
            if (watermarkPrepared && watermarkWidth == cfg.width && watermarkHeight == cfg.height &&
                sameWatermarkSettings(preparedWatermark, cfg.watermark)) return true;
            watermarkPrepared = false;
            if (!watermark.prepare(cfg.watermark, cfg.width, cfg.height, error)) return false;
            preparedWatermark = cfg.watermark; watermarkWidth = cfg.width; watermarkHeight = cfg.height;
            watermarkPrepared = true;
            return true;
        };
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
        CameraResolution activeCameraResolution = CameraResolution::Standard720;
        std::wstring cameraTemporary, cameraFinalPath, cameraTemporaryIO, cameraFinalPathIO;
        bool cameraRunning = false, pending = false, writing = false, paused = false;
        bool cameraWriting = false, segmentWriting = false, sessionStarted = false, segmentFailed = false;
        uint64_t segmentOrdinal = 0, completedSegments = 0;
        std::array<uint64_t, 2> closedFrames{};
        Clock::duration nextSegmentBoundary{};
        std::wstring sessionStem;
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
        uint64_t previewGeneration = 0, visualPreviewGeneration = 0, cameraAttemptGeneration = 0, cameraInputGeneration = 0;
        auto clearCameraInput = [&]() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cameraInputGeneration != cameraInputGeneration_) return;
            retireCameraInput(); cameraInputGeneration = cameraInputGeneration_;
        };
        auto publishCameraInput = [&](const Frame& frame) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cameraInputGeneration != cameraInputGeneration_ || previewGeneration != previewGeneration_ ||
                activeCamera != settings_.cameraId) return;
            if (frame.valid()) status_.cameraInput = {frame.width, frame.height, cameraInputGeneration};
            else { retireCameraInput(); cameraInputGeneration = cameraInputGeneration_; }
        };
        uint64_t settingsRevision = UINT64_MAX;
        auto lastPreview = Clock::time_point::min(), nextFrame = Clock::now();
        auto retryFrameAt = Clock::time_point::min();
        auto lastTick = Clock::now();
        Clock::duration activeDuration{};
        TimeSkipController timeSkip;
        TimeSkipDecision skipDecision;
        TimeSkipStatus skipStatus;
        bool skipping = false, observing = false, skipControllerValid = false;
        bool personMode = false, personStarted = false, personFault = false;
        bool personCameraPending = false, personInputReady = false, personInferencePending = false;
        person::Source personSubmitted{}, personLastSource{};
        std::array<wchar_t, 256> personFailure{};
        unsigned observedSources = 0;
        uint64_t observationToken = 0, desktopSequence = 0, observationStarted = 0;
        std::array<uint64_t, 2> observationTicks{};
        std::array<std::array<wchar_t, 256>, 2> observationProblems{};
        auto nextObservation = Clock::time_point::max(), lastAdmission = Clock::time_point::min();
        auto nextPersonPoll = Clock::time_point::max();
        Frame observationDesktop;
        auto activeMilliseconds = [&] {
            const auto duration = activeDuration + (writing && !paused ? Clock::now() - lastTick : Clock::duration::zero());
            return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        };
        auto cancelObservation = [&] {
            if (observing && camera) camera->cancelActivityObservation();
            if (personClient) personClient->cancel();
            personClient.reset(); personInput.reset();
            personStarted = personCameraPending = personInputReady = personInferencePending = false;
            personSubmitted = {}; personLastSource = {};
            nextPersonPoll = Clock::time_point::max();
            nextObservation = Clock::time_point::max();
        };
        auto resetSkipping = [&](const Settings& cfg, unsigned sourceMask) {
            cancelObservation();
            incomingIntervalMs = cfg.intervalMs;
            skipping = cfg.timeSkip.mode != TimeSkipMode::Off;
            personMode = usesPersonChecks(cfg.timeSkip.mode);
            observing = skipping && cfg.timeSkip.mode != TimeSkipMode::Manual;
            observedSources = personMode ? sourceMask & 2u : sourceMask;
            skipStatus = {}; skipStatus.enabled = skipping;
            skipStatus.intervalMs = skipping ? cfg.intervalMs : 0;
            observationTicks = {}; observationProblems = {};
            observationStarted = GetTickCount64();
            if (++observationToken == 0) ++observationToken;
            desktopSequence = 0;
            if (skipping) {
                skipControllerValid = timeSkip.reset(cfg.timeSkip, cfg.intervalMs, observedSources, cfg.outputFps);
                if (!skipControllerValid) observing = false;
                skipDecision = timeSkip.inspect(activeMilliseconds());
                skipStatus.reason = skipDecision.reason;
                if (observing && !paused) nextObservation = Clock::now();
            } else skipDecision = {};
        };
        auto returnToBase = [&] {
            if (!writing || paused || !session || lastAdmission == Clock::time_point::min()) return;
            const auto earliest = std::max(lastAdmission + std::chrono::milliseconds(session->intervalMs), Clock::now());
            if (nightMode) {
                // Never truncate an accepted integration window. If it has not
                // started, bring forward a complete base-bounded window.
                if (!nightQueued) {
                    incomingIntervalMs = session->intervalMs;
                    const auto duration = std::chrono::milliseconds(nightWindowDuration(*session, suggestedNightDurationMs));
                    nextFrame = std::min(nextFrame, std::max(earliest, Clock::now() + duration));
                    nightStartAt = nextFrame - duration;
                }
            } else { nextFrame = std::min(nextFrame, earliest); incomingIntervalMs = session->intervalMs; }
        };
        auto inspectSkipping = [&] {
            if (!skipping) return;
            if (!skipControllerValid) {
                skipDecision = {}; skipDecision.intervalMs = session ? session->intervalMs : 1000;
                skipStatus.intervalMs = skipDecision.intervalMs; skipStatus.reason = TimeSkipReason::Unavailable;
                const wchar_t* missing = personMode ? L"Person checks need a selected camera; using normal cadence."
                    : L"Activity observation needs a selected source; using normal cadence.";
                skipStatus.diagnostic = {};
                std::copy_n(missing, std::min(wcslen(missing), skipStatus.diagnostic.size() - 1), skipStatus.diagnostic.begin());
                std::lock_guard<std::mutex> lock(mutex_); status_.timeSkip = skipStatus;
                return;
            }
            const uint64_t tick = GetTickCount64();
            uint64_t oldest = UINT64_MAX;
            bool complete = true;
            skipStatus.diagnostic = {};
            if (observing) for (unsigned source = 0; source < 2; ++source) if (observedSources & (1u << source)) {
                const auto receipt = observationTicks[source];
                const auto age = tick >= receipt ? tick - receipt : UINT64_MAX;
                if (!receipt || age > 3000) {
                    complete = false;
                    if (receipt || tick - observationStarted > 3000) {
                        if (personMode) timeSkip.personUnavailable(); else timeSkip.unavailable(source);
                    }
                }
                oldest = std::min(oldest, receipt);
                if (!skipStatus.diagnostic[0] && observationProblems[source][0]) skipStatus.diagnostic = observationProblems[source];
            }
            skipStatus.lastCheckTick = observing && oldest != UINT64_MAX ? oldest : 0;
            skipStatus.observationDelayed = observing && ((!complete && tick - observationStarted > 1500) ||
                (complete && oldest != UINT64_MAX && tick - oldest > 1500));
            skipDecision = timeSkip.inspect(activeMilliseconds());
            if (skipDecision.returnToBase) returnToBase();
            skipStatus.reason = skipDecision.reason;
            skipStatus.intervalMs = skipDecision.intervalMs;
            if (skipStatus.observationDelayed && !skipStatus.diagnostic[0]) {
                const wchar_t* delayed = personMode ? L"Person checks are delayed. Normal cadence resumes when source images become stale."
                    : L"Activity checks are delayed; unseen activity may be missed. Normal cadence resumes when observations become stale.";
                std::copy_n(delayed, std::min(wcslen(delayed), skipStatus.diagnostic.size() - 1), skipStatus.diagnostic.begin());
            }
            std::lock_guard<std::mutex> lock(mutex_); status_.timeSkip = skipStatus;
        };
        auto personProblem = [&](const wchar_t* text, bool hardFailure) {
            timeSkip.personUnavailable();
            auto& message = observationProblems[1]; message = {};
            std::copy_n(text, std::min(wcslen(text), message.size() - 1), message.begin());
            if (hardFailure) {
                // A broken/missing detector never restarts itself within this
                // recording, including after pause, layout or source resets.
                personFault = true; personFailure = message; cancelObservation();
            }
        };
        auto validPersonSource = [&](const person::Source& source, uint64_t tick) {
            return source.sessionToken == observationToken && source.cameraEpoch && source.sequence &&
                source.receivedTick && source.receivedTick >= observationStarted && source.receivedTick <= tick &&
                tick - source.receivedTick <= person::SourceFreshnessMs && person::validGeometry(source);
        };
        auto observePeople = [&] {
            if (!observing || !writing || paused || !sessionStarted) return;
            if (personFault) {
                nextObservation = nextPersonPoll = Clock::time_point::max();
                personProblem(personFailure[0] ? personFailure.data() :
                    L"Person checks stopped for this recording; using normal cadence. Start a new recording to retry.", false);
                inspectSkipping(); return;
            }
            const auto now = Clock::now();
            const bool requestDue = now >= nextObservation;
            if (!requestDue && now < nextPersonPoll) return;
            if (requestDue) nextObservation = now + std::chrono::milliseconds(TimeSkipObservationMs);
            nextPersonPoll = Clock::time_point::max();
            try {
                if (!personStarted) {
                    std::wstring error;
                    if (!personClient) personClient.emplace();
                    if (!personClient->start(observationToken, error)) {
                        personProblem(error.empty() ? L"The optional person detector could not start; using normal cadence." : error.c_str(), true);
                        inspectSkipping(); return;
                    }
                    personStarted = true;
                    // The detector also rejects source samples retained from
                    // before its own asynchronous startup request.
                    observationStarted = std::max(observationStarted, GetTickCount64());
                }
                PersonCheckResult result;
                const auto state = personClient->poll(result);
                if (state == PersonPoll::Unavailable) {
                    const auto* detail = personClient->diagnostic();
                    personProblem(detail && detail[0] ? detail : L"The optional person detector is unavailable; using normal cadence.", true);
                    inspectSkipping(); return;
                }
                bool detectorReady = state == PersonPoll::Ready;
                if (state == PersonPoll::Complete) {
                    const auto tick = GetTickCount64();
                    if (!personInferencePending || !person::sameSource(result.source, personSubmitted)) {
                        personProblem(L"The optional person detector returned the wrong source identity; using normal cadence.", true);
                        inspectSkipping(); return;
                    }
                    personInferencePending = false; detectorReady = true;
                    if (!validPersonSource(result.source, tick)) {
                        personProblem(L"The person-check source image is stale or invalid; using normal cadence.", false);
                    } else {
                        PersonPresence presence = PersonPresence::Unknown;
                        if (result.output.verdict == person::Verdict::Present) presence = PersonPresence::Present;
                        else if (result.output.verdict == person::Verdict::QualifiedAbsent) presence = PersonPresence::QualifiedAbsent;
                        const auto age = tick - result.source.receivedTick;
                        const int64_t observedAt = std::max<int64_t>(0, activeMilliseconds() - static_cast<int64_t>(age));
                        if (timeSkip.observePerson({presence, result.source.cameraEpoch, result.source.sequence, observedAt})) {
                            observationTicks[1] = result.source.receivedTick;
                            observationProblems[1] = {}; personLastSource = result.source;
                            if (presence == PersonPresence::Unknown) {
                                personProblem(result.output.reason == person::Reason::InsufficientDetail
                                    ? L"Too little image detail for absence checks; using normal cadence."
                                    : L"The person check is uncertain; using normal cadence.", false);
                            }
                        } else personProblem(L"The person check repeated or regressed its source; using normal cadence.", false);
                    }
                }
                // Poll replies independently of the one-second request cadence.
                // Pending reads must not request another camera conversion.
                if (detectorReady && !personInferencePending && !personInputReady && (requestDue || personCameraPending)) {
                    if (!cameraRunning) personProblem(L"The selected camera is unavailable for person checks; using normal cadence.", false);
                    else {
                        if (!personInput) personInput = std::make_unique<CameraPersonInput>();
                        std::wstring error;
                        personCameraPending = true;
                        if (camera->personInput(observationToken, *personInput, error, requestDue)) {
                            personCameraPending = false;
                            const auto& source = personInput->source;
                            const bool newer = !personLastSource.cameraEpoch ||
                                (source.receivedTick >= personLastSource.receivedTick && source.cameraEpoch >= personLastSource.cameraEpoch &&
                                 (source.cameraEpoch != personLastSource.cameraEpoch || source.sequence > personLastSource.sequence));
                            if (validPersonSource(source, GetTickCount64()) && newer) {
                                if (personLastSource.cameraEpoch &&
                                    (source.cameraEpoch != personLastSource.cameraEpoch ||
                                     source.sourceWidth != personLastSource.sourceWidth || source.sourceHeight != personLastSource.sourceHeight ||
                                     source.width != personLastSource.width || source.height != personLastSource.height)) {
                                    // Changed source geometry/epoch cannot carry
                                    // an old absence dwell into the new image.
                                    timeSkip.personUnavailable(); observationTicks[1] = 0;
                                }
                                personInputReady = true;
                            }
                            else personProblem(L"The camera person-check image is stale or repeated; using normal cadence.", false);
                        } else if (!error.empty()) {
                            personCameraPending = false;
                            personProblem(error.c_str(), false);
                        }
                    }
                }
                if (detectorReady && personInputReady) {
                    if (!validPersonSource(personInput->source, GetTickCount64())) {
                        personInputReady = false;
                        personProblem(L"The camera person-check image became stale before submission; using normal cadence.", false);
                    } else if (personClient->submit(*personInput)) {
                        personSubmitted = personInput->source; personInputReady = false; personInferencePending = true;
                    }
                }
                if (state == PersonPoll::Pending || personInferencePending || personCameraPending || personInputReady)
                    nextPersonPoll = Clock::now() + std::chrono::milliseconds(50);
            } catch (...) {
                personProblem(L"Person checks could not allocate a resource; using normal cadence.", true);
            }
            inspectSkipping();
        };
        auto observeSources = [&](const Settings& cfg) {
            if (personMode) { observePeople(); return; }
            if (!observing || !writing || paused || Clock::now() < nextObservation) return;
            nextObservation = Clock::now() + std::chrono::milliseconds(TimeSkipObservationMs);
            for (unsigned source = 0; source < 2; ++source) if (observedSources & (1u << source)) {
                auto problem = [&](const wchar_t* message) {
                    timeSkip.unavailable(source);
                    auto& text = observationProblems[source]; text = {};
                    std::copy_n(message, std::min(wcslen(message), text.size() - 1), text.begin());
                };
                // Optional observation work cannot convert an allocation or
                // source-query failure into a capture/save failure.
                try {
                    std::wstring error;
                    if (source == 0) {
                        // Keep sampling geometry and cursor policy stable.
                        // Alternating preview/full-frame resampling can create
                        // false motion even when the desktop is unchanged.
                        if (!captureMonitor(cfg.monitorId, TimeSkipWidth, TimeSkipHeight, false, observationDesktop, error)) {
                            problem(error.empty() ? L"Desktop activity observation is unavailable." : error.c_str()); continue;
                        }
                        TimeSkipDescriptor descriptor;
                        if (!describeTimeSkipFrame(observationDesktop, descriptor)) { problem(L"Desktop activity observation is invalid."); continue; }
                        const auto tick = GetTickCount64();
                        if (timeSkip.observe(0, descriptor, observationToken, ++desktopSequence, activeMilliseconds())) {
                            observationTicks[0] = tick; observationProblems[0] = {};
                        }
                    } else {
                        CameraObservation observation;
                        if (!cameraRunning) { problem(L"Camera activity observation is unavailable."); continue; }
                        if (!camera->observeActivity(observationToken, observation, error)) {
                            if (!error.empty()) problem(error.c_str());
                            continue;
                        }
                        const auto tick = GetTickCount64();
                        if (!observation.receivedTick || observation.receivedTick < observationStarted || observation.receivedTick > tick || tick - observation.receivedTick > 3000 ||
                            !observation.epoch || !observation.sequence || observation.sourceWidth <= 0 || observation.sourceHeight <= 0) {
                            problem(L"Camera activity observation is stale or invalid."); continue;
                        }
                        const int64_t observedAt = std::max<int64_t>(0, activeMilliseconds() - static_cast<int64_t>(tick - observation.receivedTick));
                        if (timeSkip.observe(1, observation.descriptor, observation.epoch, observation.sequence, observedAt)) {
                            observationTicks[1] = observation.receivedTick; observationProblems[1] = {};
                        }
                    }
                } catch (...) { problem(L"Activity observation could not allocate a resource; using normal cadence."); }
            }
            inspectSkipping();
        };
        double elapsed = 0;
        bool recordingLimitReached = false;
        bool terminalClockFrozen = false;
        auto advanceElapsed = [&](Clock::time_point now) {
            if (writing && !paused && (!delayedOrigin || sessionStarted)) activeDuration += now - lastTick;
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
        auto segmentDeadline = [&] {
            return writing && !paused && session && session->segmentDurationSeconds > 0 && segmentWriting && encoder->frames()
                ? lastTick + (nextSegmentBoundary - activeDuration) : Clock::time_point::max();
        };
        auto segmentExpired = [&] {
            return writing && !paused && session && session->segmentDurationSeconds > 0 && segmentWriting &&
                encoder->frames() && activeDuration >= nextSegmentBoundary;
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
            if (start_ || previewGeneration != previewGeneration_ || visualPreviewGeneration != visualPreviewGeneration_) return;
            status_.preview.reset();
            // A preview can recover on its next refresh, but it must not
            // replace a recording or resource error that the user needs to see.
            if (!status_.error || previewProblem_) {
                status_.error = true;
                status_.message = writing ? L"Preview unavailable: " + message : message;
                previewProblem_ = true;
            }
        };
        auto finishSegment = [&](const std::wstring& reason, bool terminal) -> bool {
            if (session && session->segmentDurationSeconds > 0) {
                const size_t count = session->separateFiles ? 2 : 1;
                const std::array<bool, 2> opened{segmentWriting, cameraWriting};
                const std::array<Encoder*, 2> writers{encoder ? &*encoder : nullptr, cameraEncoder ? &*cameraEncoder : nullptr};
                const std::array<const std::wstring*, 2> temps{&temporary, &cameraTemporary}, finals{&finalPath, &cameraFinalPath};
                const std::array<const std::wstring*, 2> tempIO{&temporaryIO, &cameraTemporaryIO}, finalIO{&finalPathIO, &cameraFinalPathIO};
                std::array<std::wstring, 2> errors;
                std::array<bool, 2> finalized{}, retained{};
                std::array<uint64_t, 2> frames{}, totals = closedFrames;
                bool hasCurrent = false;
                for (size_t i = 0; i < count; ++i) if (opened[i]) {
                    hasCurrent = true;
                    frames[i] = writers[i]->frames();
                    totals[i] = segmentFrameTotal(closedFrames[i], frames[i]);
                    finalized[i] = writers[i]->finishForPublication(errors[i]);
                    const DWORD attrs = !finalized[i] ? GetFileAttributesW(tempIO[i]->c_str()) : INVALID_FILE_ATTRIBUTES;
                    retained[i] = attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
                }
                // Automatic saving consumes active time, while a terminal save
                // freezes elapsed at the observed terminal cutoff.
                if (!terminal && sessionStarted) {
                    advanceElapsed(Clock::now());
                    if (limitExpired()) recordingLimitReached = true;
                }
                if (!hasCurrent && segmentFailed) return false; // Preserve the committed failing part's exact report.
                std::wstring previousFiles;
                if (completedSegments) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (const auto& path : status_.savedPaths) previousFiles += L" Latest saved: " + path + L".";
                }
                struct Report { std::wstring first, message; std::vector<std::wstring> paths; bool error = false; uint64_t completed = 0; };
                std::array<Report, 4> reports;
                const size_t masks = size_t(1) << count;
                for (size_t mask = 0; mask < masks; ++mask) {
                    auto& report = reports[mask];
                    report.error = !reason.empty();
                    report.message = reason;
                    if (recordingLimitReached) report.message += (report.message.empty() ? L"" : L" ") + std::wstring(L"Recording time limit reached.");
                    bool complete = hasCurrent;
                    for (size_t i = 0; i < count; ++i) {
                        complete &= finalized[i] && (mask & (size_t(1) << i)) != 0;
                        if (!opened[i]) continue;
                        if (!report.message.empty()) report.message += L" ";
                        if (count == 2) report.message += i == 0 ? L"Desktop: " : L"Camera: ";
                        if (finalized[i]) {
                            const bool renamed = (mask & (size_t(1) << i)) != 0;
                            const auto& path = *(renamed ? finals[i] : temps[i]);
                            report.paths.push_back(path);
                            report.message += renamed ? L"Saved: " : L"Could not publish the final filename; finished video remains at: ";
                            report.message += path + L" (" + std::to_wstring(frames[i]) + L" frames).";
                            report.error |= !renamed;
                        } else if (frames[i] || retained[i]) {
                            report.error = true;
                            report.message += L"Could not finish video: " + errors[i];
                            if (retained[i]) report.message += L" Partial file: " + *temps[i];
                        } else report.message += L"No frames saved in this part.";
                    }
                    if (complete && completedSegments == UINT64_MAX) throw std::overflow_error("Recording segment count exhausted");
                    report.completed = completedSegments + (complete ? 1 : 0);
                    if (!report.message.empty()) report.message += L" ";
                    report.message += std::to_wstring(report.completed) + L" complete segment set(s) saved";
                    if (!sessionStem.empty()) report.message += L" under " + (std::filesystem::path(recordingFolder) / sessionStem).wstring();
                    report.message += L".";
                    if (report.paths.empty()) report.message += previousFiles;
                    if (!report.paths.empty()) report.first = report.paths.front();
                }
                // Reports are ready before either rename; a later publish may
                // allocate before its own rename. The status commit below does
                // not allocate, even when that later publication throws.
                size_t published = 0;
                for (size_t i = 0; i < count; ++i) if (finalized[i]) {
                    try { if (writers[i]->publish(*finalIO[i]) == ERROR_SUCCESS) published |= size_t(1) << i; }
                    catch (const std::exception&) {}
                }
                auto& report = reports[published];
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.elapsed = elapsed;
                    status_.frames = count == 2 ? std::min(totals[0], totals[1]) : totals[0];
                    status_.completedSegments = report.completed;
                    status_.error = status_.recordingFailed = report.error;
                    if (!report.paths.empty()) { status_.savedPath.swap(report.first); status_.savedPaths.swap(report.paths); }
                    status_.message.swap(report.message);
                    previewProblem_ = false;
                }
                closedFrames = totals; completedSegments = report.completed; segmentFailed = report.error;
                for (size_t i = 0; i < count; ++i) if (writers[i]) writers[i]->releasePublication();
                segmentWriting = cameraWriting = false;
                return !segmentFailed;
            }
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
                writing = segmentWriting = cameraWriting = pending = paused = false; previewProblem_ = false;
                recordingLimitReached = false;
                desktop = {}; webcam = {}; composed = {}; cameraComposed = {};
                power.update(false, false);
                return !report.error;
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
            writing = segmentWriting = pending = paused = false; previewProblem_ = false;
            recordingLimitReached = false;
            desktop = {}; webcam = {}; composed = {};
            power.update(false, false);
            return !status_.error;
        };
        auto closeRecording = [&](const std::wstring& reason) {
            delayedRequest.reset(); delayedDeadline = 0; delayedOrigin = false; delayedLayersFrozen = false;
            { std::lock_guard<std::mutex> lock(mutex_);
              delayedSettings_.reset(); delayedStartPending_ = delayedCancel_ = false; status_.startDeadlineTick = 0; }
            // Snapshot the observed stop/failure time once, before helper or
            // encoder cleanup. A reporting exception can retry this operation;
            // that retry must not count the preceding terminal save as active.
            if (!terminalClockFrozen) {
                if (sessionStarted) advanceElapsed(Clock::now());
                else { activeDuration = Clock::duration::zero(); elapsed = 0; }
                terminalClockFrozen = true;
            }
            releaseDesktopCaptureCache();
            resetWatermark();
            cancelNight(); nightMode = false;
            cancelObservation(); skipping = observing = false; observationDesktop = {};
            if (writing) power.saving();
            bool keepCamera;
            { std::lock_guard<std::mutex> lock(mutex_);
              status_.state = State::Finishing; status_.nightWaiting = false;
              // A recovery marker can fail after the writer accepted a sample.
              // Publish its count before reporting can allocate or retry; a
              // writerless new attempt must not reuse the prior encoder count.
              if (!session || (!session->separateFiles && session->segmentDurationSeconds <= 0))
                  status_.frames = writing && encoder ? encoder->frames() : 0;
              if (!segmentFailed) status_.message = L"Finishing MP4...";
              keepCamera = settings_.preview; }
            if (!keepCamera && cameraRunning) { clearCameraInput(); camera->stop(); cameraRunning = false; activeCamera.clear(); }
            finishSegment(reason, true);
            // Legacy Off reporting already commits Idle and all session state
            // together. Do not overwrite a Record queued after that commit.
            if (!session || session->segmentDurationSeconds <= 0) return;
            { std::lock_guard<std::mutex> lock(mutex_);
              stop_ = pauseRequested_ = pauseTarget_ = false; status_.state = State::Idle; status_.elapsed = elapsed; }
            writing = segmentWriting = cameraWriting = pending = paused = false;
            recordingLimitReached = false;
            desktop = {}; webcam = {}; composed = {}; cameraComposed = {};
            power.update(false, false);
        };
        auto endDelayedRequest = [&](const std::wstring& problem, bool failed) {
            std::wstring message = problem.empty() ? L"Delayed start cancelled." : problem;
            // The delayed request has not admitted any frame. Finish each
            // owned empty writer, retaining typed cleanup failures; ordinary
            // finish's false result alone also denotes successful empty discard.
            if (writing && !sessionStarted) {
                std::wstring cleanup;
                bool discarded = true;
                const std::array<Encoder*, 2> writers{segmentWriting && encoder ? &*encoder : nullptr,
                    cameraWriting && cameraEncoder ? &*cameraEncoder : nullptr};
                for (auto* writer : writers) if (writer) {
                    std::wstring error;
                    writer->finish(error);
                    if (!writer->emptyOutputDiscarded()) {
                        discarded = false;
                        if (!cleanup.empty()) cleanup += L" ";
                        cleanup += error;
                    }
                    writer->releasePublication();
                }
                if (!discarded) { failed = true; message += L" Empty output cleanup failed: " + cleanup; }
                writing = segmentWriting = cameraWriting = false;
            }
            if (pending || delayedOrigin) {
                cancelNight(); nightMode = false;
                cancelObservation(); skipping = observing = false; observationDesktop = {};
                resetWatermark(); releaseDesktopCaptureCache();
                pending = paused = false; sessionStarted = false; delayedLayersFrozen = false;
                desktop = {}; webcam = {}; composed = {}; cameraComposed = {};
            }
            bool keepCamera;
            { std::lock_guard<std::mutex> lock(mutex_); keepCamera = settings_.preview; }
            if (!keepCamera && cameraRunning) { clearCameraInput(); camera->stop(); cameraRunning = false; activeCamera.clear(); }
            {
                delayedRequest.reset(); delayedDeadline = 0; delayedOrigin = false;
                power.update(false, false);
                std::lock_guard<std::mutex> lock(mutex_);
                if (start_) return;
                delayedSettings_.reset(); delayedStartPending_ = delayedCancel_ = false;
                stop_ = pauseRequested_ = pauseTarget_ = false;
                status_.state = State::Idle; status_.startDeadlineTick = 0;
                status_.frames = 0; status_.elapsed = 0; status_.completedSegments = 0;
                status_.nightEnabled = status_.nightWaiting = false; status_.timeSkip = {};
                status_.error = status_.recordingFailed = failed; previewProblem_ = false;
                status_.message.swap(message);
            }
        };
        auto waitingDeadline = [&] {
            if (!delayedDeadline) return Clock::time_point::max();
            const auto tick = GetTickCount64();
            return Clock::now() + std::chrono::milliseconds(delayedDeadline > tick ? delayedDeadline - tick : 0);
        };
        auto waitAfterFailure = [&](std::unique_lock<std::mutex>& lock) {
            if (stop_ && delayedStartPending_ && status_.state == State::Waiting) return;
            // Commands already queued at failure remain pending, but must not
            // turn persistent allocation failure into an immediate retry loop.
            const auto generation = previewGeneration_;
            const auto revision = settingsRevision_;
            const bool start = start_, stop = stop_, pauseRequested = pauseRequested_, pauseTarget = pauseTarget_, retry = retrySources_;
            wake_.wait_until(lock, std::min({Clock::now() + std::chrono::seconds(1), recordingDeadline(), segmentDeadline(), waitingDeadline()}), [&] {
                return quit_ || previewGeneration_ != generation || settingsRevision_ != revision || start_ != start ||
                    stop_ != stop || pauseRequested_ != pauseRequested || pauseTarget_ != pauseTarget || retrySources_ != retry;
            });
        };
        for (;;) {
            bool previewOnlyWork = false;
            bool readingCameraInput = false;
            try {
                std::optional<Settings> snapshot;
                bool quit, start = false, stop = false, pauseRequested = false, pauseTarget = false, retry = false;
                bool cancelledWaiting = false, cancelledPreview = false;
                uint64_t requestDeadline = 0, requestEpoch = 0;
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
                  if (writing && !paused && skipping) {
                      if (observing) {
                          deadline = std::min(deadline, nextObservation);
                          if (personMode) deadline = std::min(deadline, nextPersonPoll);
                      }
                      if (skipDecision.nextBoundaryMs > 0)
                          deadline = std::min(deadline, lastTick + (std::chrono::milliseconds(skipDecision.nextBoundaryMs) - activeDuration));
                  }
                  deadline = std::min({deadline, recordingDeadline(), segmentDeadline()});
                  if (status_.state == State::Waiting) {
                      const auto tick = GetTickCount64();
                      deadline = std::min(deadline, Clock::now() + std::chrono::milliseconds(
                          status_.startDeadlineTick > tick ? status_.startDeadlineTick - tick : 0));
                  }
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
                  if (!quit && stop_ && delayedStartPending_ && status_.state == State::Waiting && !pending && !writing) {
                      // Retire an armed request even when Settings copies keep
                      // failing. Cancellation must not require an allocation.
                      delayedSettings_.reset(); delayedRequest.reset(); delayedDeadline = 0;
                      delayedOrigin = delayedLayersFrozen = delayedStartPending_ = delayedCancel_ = false;
                      start_ = stop_ = pauseRequested_ = pauseTarget_ = false;
                      status_.state = State::Idle; status_.startDeadlineTick = 0;
                      status_.error = status_.recordingFailed = previewProblem_ = false;
                      status_.nightEnabled = status_.nightWaiting = false;
                      status_.message.swap(delayedCancellationMessage_);
                      power.update(false, false);
                      cancelledWaiting = true; cancelledPreview = settings_.preview;
                      settingsRevision = settingsRevision_;
                  }
                  previewGeneration = previewGeneration_;
                  visualPreviewGeneration = visualPreviewGeneration_;
                  cameraInputGeneration = cameraInputGeneration_;
                  const auto requiredWork = nightMode ? (nightQueued ? nightPollAt : nightStartAt) : nextFrame;
                  previewOnlyWork = !start_ && !stop_ && !quit_ && !pending &&
                      (!writing || paused || Clock::now() < requiredWork) && Clock::now() < recordingDeadline() && Clock::now() < segmentDeadline() &&
                      (!delayedDeadline || GetTickCount64() < delayedDeadline);
                  if (!quit && !cancelledWaiting) {
                      if (start_ && delayedSettings_) snapshot.emplace(*delayedSettings_);
                      else snapshot.emplace(settings_);
                      snapshot->preview = settings_.preview;
                      settingsRevision = settingsRevision_;
                      start = start_; stop = stop_; pauseRequested = pauseRequested_; pauseTarget = pauseTarget_; retry = retrySources_;
                      // Record fixes cursor and playback FPS at acceptance even if an idle
                      // capture delays this worker and next-session edits arrive.
                      if (start) {
                          snapshot->captureCursor = requestedCursor_;
                          snapshot->outputFps = requestedOutputFps_;
                      }
                      if (start && delayedSettings_) {
                          requestDeadline = status_.startDeadlineTick; requestEpoch = delayedWakeEpoch_;
                          delayedSettings_.reset();
                      }
                      start_ = stop_ = pauseRequested_ = retrySources_ = false;
                  } }
                if (cancelledWaiting) {
                    if (!cancelledPreview) {
                        releaseDesktopCaptureCache(); resetWatermark();
                        desktop = {}; webcam = {}; composed = {}; cameraComposed = {}; previewBuffer.reset();
                        if (cameraRunning) { clearCameraInput(); camera->stop(); cameraRunning = false; activeCamera.clear(); }
                    }
                    continue;
                }
                const auto now = Clock::now();
                advanceElapsed(now);
                // Preview deadlines can coincide with every clock deadline.
                // Publish time even when this wake also refreshes the image.
                if (writing) { std::lock_guard<std::mutex> lock(mutex_); status_.elapsed = elapsed; }
                if (quit) { if (writing || pending) { previewOnlyWork = false; closeRecording(L""); } break; }
                Settings& cfg = *snapshot;
                bool resetSkipRequested = false;
                if (start && requestDeadline) {
                    delayedRequest.emplace(std::move(cfg)); delayedDeadline = requestDeadline; delayedEpoch = requestEpoch;
                    delayedOrigin = true; sessionStarted = false; start = false;
                    cfg = *delayedRequest;
                }
                if (stop && delayedOrigin && !sessionStarted) { endDelayedRequest(L"", false); continue; }
                if (delayedRequest && GetTickCount64() >= delayedDeadline) {
                    previewOnlyWork = false;
                    uint64_t epoch = 0; std::wstring error;
                    const bool known = queryDelayedWakeEpoch(epoch, error);
                    bool cancelled;
                    { std::lock_guard<std::mutex> lock(mutex_); cancelled = quit_ || stop_ || delayedCancel_; }
                    if (cancelled || !known || epoch != delayedEpoch) {
                        endDelayedRequest(cancelled ? L"" : !known ? error : epoch != delayedEpoch
                            ? L"Delayed start cancelled because Windows resumed." : L"", !known && !cancelled);
                        continue;
                    }
                    const bool preview = cfg.preview;
                    cfg = std::move(*delayedRequest); cfg.preview = preview;
                    delayedRequest.reset(); delayedDeadline = 0; start = true;
                    { std::lock_guard<std::mutex> lock(mutex_);
                      delayedLayersFrozen = true;
                      status_.state = State::Starting; status_.startDeadlineTick = 0;
                      status_.nightEnabled = usesNightCamera(cfg); status_.nightWaiting = status_.nightEnabled;
                      if (!previewProblem_) status_.message = L"Preparing recording..."; }
                }
                if (retry) { clearCameraInput(); if (camera) camera->stop(); cameraRunning = false; activeCamera.clear(); }
                if (FAILED(com)) {
                    const auto error = L"Windows media initialization failed: " + errorText(com);
                    if (delayedOrigin) endDelayedRequest(error, true); else publishError(error, true);
                    continue;
                }
                if (start) {
                    previewOnlyWork = false;
                    personFault = false; personFailure = {};
                    session.emplace(cfg); pending = true; elapsed = 0;
                    resetWatermark(); incomingIntervalMs = cfg.intervalMs;
                    segmentWriting = cameraWriting = sessionStarted = segmentFailed = false;
                    closedFrames = {}; segmentOrdinal = completedSegments = 0; sessionStem.clear();
                    activeDuration = Clock::duration::zero(); recordingLimitReached = false; terminalClockFrozen = false;
                    cancelNight(); nightMode = usesNightCamera(cfg);
                    suggestedNightDurationMs = NightInitialDurationMs;
                    nightStartAt = now;
                    std::wstring validationError;
                    if (!validateRecordingSettings(cfg, validationError) ||
                        !prepareWatermark(cfg, validationError)) {
                        closeRecording(validationError); continue;
                    }
                    session->timeSkip = cfg.timeSkip;
                    resetSkipRequested = true; lastAdmission = Clock::time_point::min();
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
                    cancelObservation();
                    paused = pauseTarget;
                    if (paused) releaseDesktopCaptureCache();
                    resetSkipRequested = skipping;
                    const bool desktopNeeded = (session && session->separateFiles) || std::any_of(cfg.layers.begin(), cfg.layers.end(),
                        [](const Layer& layer) { return layer.source == Source::Desktop; });
                    power.update(!paused, desktopNeeded);
                    if (!paused) { nextFrame = now; nightStartAt = now; retryFrameAt = Clock::time_point::min(); incomingIntervalMs = cfg.intervalMs; }
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.state = paused ? State::Paused : State::Recording;
                    status_.nightWaiting = nightMode && !paused;
                    status_.elapsed = elapsed;
                    if (!previewProblem_)
                        status_.message = paused ? L"Paused. Resume when you are ready." : recordingMessage(session && session->separateFiles);
                }
                bool captureDue = pending || (writing && !paused && now >= nextFrame);
                // An overdue part still owns required work while applying the
                // consumed command/configuration changes below. If those fail,
                // do not discard their reset intent as a disposable preview.
                previewOnlyWork = !captureDue && !segmentExpired();
                if (writing || pending) {
                    if (!session->separateFiles && !delayedLayersFrozen) {
                        if (skipping && !sameLayers(session->layers, cfg.layers)) resetSkipRequested = true;
                        session->layers = cfg.layers;
                    }
                    session->preview = cfg.preview;
                    cfg = *session;
                }
                bool needCamera = cfg.separateFiles, needDesktop = cfg.separateFiles;
                for (auto& layer : cfg.layers) { needCamera |= layer.source == Source::Camera; needDesktop |= layer.source == Source::Desktop; }
                // cfg is the frozen recording snapshot while a session exists.
                // Idle previews request detail only when the output needs it.
                const auto cameraResolution = cameraResolutionForOutput(cfg.width, cfg.height);
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
                if (resetSkipRequested) {
                    resetSkipping(cfg, (needDesktop ? 1u : 0u) | (needCamera ? 2u : 0u));
                    if (skipping && writing && !paused && lastAdmission != Clock::time_point::min()) {
                        nextFrame = std::max(lastAdmission + std::chrono::milliseconds(cfg.intervalMs), Clock::now());
                        if (nightMode && !nightQueued) {
                            const auto duration = std::chrono::milliseconds(nightWindowDuration(cfg, suggestedNightDurationMs));
                            nextFrame = std::max(nextFrame, Clock::now() + duration);
                            nightStartAt = nextFrame - duration;
                        }
                    }
                    std::lock_guard<std::mutex> lock(mutex_); status_.timeSkip = skipStatus;
                }
                if (segmentExpired()) {
                    previewOnlyWork = false;
                    if (!finishSegment(L"", false) || recordingLimitReached) closeRecording(L"");
                    // Apply a consumed Resume/source reset before this early
                    // return, then preserve commands queued during saving.
                    // Ordinary rollover does not reset observation or Night.
                    continue;
                }
                if (skipping && writing && !paused) {
                    inspectSkipping();
                    captureDue = Clock::now() >= nextFrame;
                }
                const bool holdingDelay = delayedOrigin && !sessionStarted;
                power.update(holdingDelay || (writing && !paused), !holdingDelay && needDesktop);
                const bool active = pending || (writing && !paused) || cfg.preview;
                // An idle hidden/disabled overlay owns no reusable GDI cache.
                // Keep paused session resources and its last context intact:
                // a hidden resume must still stamp before its first write.
                if (watermarkPrepared && (!cfg.watermark.enabled || (!writing && !pending && !active))) resetWatermark();
                if (!cfg.preview) previewBuffer.reset();
                if (!active || !needDesktop) releaseDesktopCaptureCache();
                if (!active) { desktop = {}; webcam = {}; composed = {}; cameraComposed = {}; }
                else {
                    if (!needDesktop) desktop = {};
                    if (!needCamera) webcam = {};
                }
                if ((!needCamera || !active || activeCamera != cfg.cameraId || activeCameraResolution != cameraResolution) && cameraRunning) {
                    cancelNight();
                    clearCameraInput();
                    camera->stop(); cameraRunning = false; activeCamera.clear();
                }
                if (!needCamera) {
                    // Switching away and back is an explicit source retry,
                    // including when a failed camera has already been stopped.
                    activeCamera.clear(); sourceError.clear();
                }
                // Retry a retired failed attempt once under the new source generation.
                // Healthy readers remain open; current-generation failures stay latched.
                if (needCamera && active && !cameraRunning && (activeCamera != cfg.cameraId || activeCameraResolution != cameraResolution ||
                        cameraAttemptGeneration != cameraInputGeneration || start)) {
                    if (skipping && writing) {
                        // A restarted helper can reuse its local epoch values;
                        // do not compare its first report with the old reader.
                        resetSkipping(cfg, (needDesktop ? 1u : 0u) | 2u);
                        returnToBase(); inspectSkipping();
                    }
                    activeCamera = cfg.cameraId;
                    activeCameraResolution = cameraResolution;
                    cameraAttemptGeneration = cameraInputGeneration;
                    sourceError.clear();
                    if (!cfg.cameraId.empty()) {
                        try {
                            if (!camera) camera.emplace();
                            cameraRunning = camera->start(cfg.cameraId, sourceError, cameraResolution);
                        }
                        catch (...) {
                            // A startup exception may leave partial client state.
                            // Permit the promised preview retry after cleanup.
                            clearCameraInput();
                            if (camera) camera->stop();
                            cameraRunning = false;
                            activeCamera.clear(); sourceError.clear();
                            throw;
                        }
                    }
                    else sourceError = L"No camera found. Connect a camera and refresh sources.";
                }
                const bool previewDue = cfg.preview && (lastPreview == Clock::time_point::min() || now - lastPreview >= std::chrono::milliseconds(writing ? 1000 : 500));
                // Observation alone asks for a small, consistently sampled
                // image; it never composes a preview, queries disk or writes.
                if (observing && writing && !paused) {
                    observeSources(cfg);
                    advanceElapsed(Clock::now());
                    if (limitExpired()) { previewOnlyWork = false; recordingLimitReached = true; closeRecording(L""); continue; }
                    if (!nightMode) captureDue = Clock::now() >= nextFrame;
                }
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
                        } else if (!nightError.empty()) { clearCameraInput(); closeRecording(L"Night camera stopped: " + nightError); continue; }
                        else retryFrameAt = Clock::now() + std::chrono::milliseconds(50);
                    }
                    if (cameraRunning && nightQueued && Clock::now() >= nightPollAt) {
                        previewOnlyWork = false;
                        std::wstring nightError;
                        readingCameraInput = true;
                        const bool complete = camera->nightResult(nightToken, webcam, completedNight, nightError);
                        readingCameraInput = false;
                        if (complete) publishCameraInput(webcam);
                        advanceElapsed(Clock::now());
                        if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); continue; }
                        if (complete) {
                            firstNightPreview = !nightFrameReady;
                            nightQueued = false; nightFrameReady = nightCompleted = true;
                            retryFrameAt = Clock::time_point::min();
                        } else if (!nightError.empty()) { clearCameraInput(); closeRecording(L"Night camera stopped: " + nightError); continue; }
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
                    readingCameraInput = true;
                    ready = cameraRunning && camera->latest(webcam, error);
                    readingCameraInput = false;
                    if (ready) publishCameraInput(webcam);
                    cameraWaiting = cameraRunning && !ready && error.empty();
                    if (!cameraRunning) error = sourceError.empty() ? L"No camera available." : sourceError;
                    else if (!ready && !error.empty()) {
                        // A stale/disconnected reader cannot recover by polling
                        // its old sample. Keep the device id as the failed-attempt
                        // marker so idle preview does not repeatedly reopen it;
                        // a new Record action can retry the same camera.
                        clearCameraInput();
                        camera->stop(); cameraRunning = false;
                        cameraAttemptGeneration = cameraInputGeneration;
                        sourceError = error;
                        if (skipping && writing) {
                            resetSkipping(cfg, (needDesktop ? 1u : 0u) | 2u);
                            returnToBase(); inspectSkipping();
                        }
                    }
                }
                const auto previewSize = previewDimensions(cfg.width, cfg.height);
                const int width = captureDue ? cfg.width : previewSize.first, height = captureDue ? cfg.height : previewSize.second;
                if (needDesktop && ready) ready = captureMonitor(cfg.monitorId, width, height, cfg.captureCursor, desktop, error);
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
                if (captureDue && (pending || (writing && !paused))) {
                    // Hold this prepared frame/pair through a rollover. A
                    // command discards only unadmitted pixels at a checkpoint;
                    // no preview or source refresh can overwrite them here.
                    bool admit = true;
                    Clock::time_point admittedAt;
                    auto commandPending = [&] {
                        std::lock_guard<std::mutex> lock(mutex_);
                        return quit_ || stop_ || pauseRequested_;
                    };
                    for (;;) {
                        if (sessionStarted) advanceElapsed(Clock::now());
                        if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); admit = false; break; }
                        if (commandPending()) { admit = false; break; }
                        if (segmentExpired()) {
                            if (!finishSegment(L"", false)) { closeRecording(L""); admit = false; break; }
                            // A zero-frame successor cannot roll over, so this
                            // loop retires at most one nonempty old segment.
                            continue;
                        }
                        if (!segmentWriting) {
                            if (pending) {
                                std::error_code ec;
                                if (cfg.folder.empty()) { closeRecording(L"Cannot create the save folder. Choose another folder."); admit = false; break; }
                                const auto folder = std::filesystem::absolute(std::filesystem::path(cfg.folder), ec);
                                if (ec) { closeRecording(L"Cannot resolve the save folder. Choose another folder."); admit = false; break; }
                                recordingFolder = folder.lexically_normal().wstring();
                                recordingFolderIO = recordingDirectoryIO(recordingFolder);
                                std::filesystem::create_directories(recordingFolderIO, ec);
                                if (ec) { closeRecording(L"Cannot create the save folder. Choose another folder."); admit = false; break; }
                                SYSTEMTIME t; GetLocalTime(&t);
                                wchar_t name[100];
                                swprintf_s(name, L"Timelapse-%04u%02u%02u-%02u%02u%02u-%03u-%lu", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
                                sessionStem = name;
                            }
                            const auto space = cfg.stopOnLowDiskSpace ? queryRecordingSpace(recordingFolderIO) : RecordingSpace{};
                            if (sessionStarted) advanceElapsed(Clock::now());
                            if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); admit = false; break; }
                            if (commandPending()) { admit = false; break; }
                            if (cfg.stopOnLowDiskSpace && !space.enough(cfg.separateFiles)) {
                                closeRecording(L"Cannot start recording: " + recordingSpaceProblem(space, cfg.separateFiles) + L" Save folder: " + recordingFolder); admit = false; break;
                            }
                            auto name = sessionStem;
                            if (cfg.segmentDurationSeconds > 0) {
                                if (segmentOrdinal == UINT64_MAX) throw std::overflow_error("Recording part names exhausted");
                                wchar_t part[40]; swprintf_s(part, L"-part-%06llu", static_cast<unsigned long long>(segmentOrdinal + 1));
                                name += part;
                            }
                            const auto base = (std::filesystem::path(recordingFolder) / name).wstring();
                            const auto desktopBase = cfg.separateFiles ? base + L"-desktop" : base;
                            temporary = desktopBase + L".recording.mp4"; finalPath = desktopBase + L".mp4";
                            const auto ioBase = recordingFolderIO + name;
                            const auto desktopIOBase = cfg.separateFiles ? ioBase + L"-desktop" : ioBase;
                            temporaryIO = desktopIOBase + L".recording.mp4"; finalPathIO = desktopIOBase + L".mp4";
                            if (cfg.separateFiles) {
                                cameraTemporary = base + L"-camera.recording.mp4"; cameraFinalPath = base + L"-camera.mp4";
                                cameraTemporaryIO = ioBase + L"-camera.recording.mp4"; cameraFinalPathIO = ioBase + L"-camera.mp4";
                            }
                            if (!encoder) encoder.emplace();
                            if (!encoder->open(temporaryIO, cfg.width, cfg.height, cfg.outputFps, error, cfg.encodingQuality, cfg.encodingMode, cfg.recoveryMode)) {
                                closeRecording((cfg.separateFiles ? L"Cannot start desktop recording: " : L"Cannot start recording: ") + error); admit = false; break;
                            }
                            writing = segmentWriting = true;
                            if (cfg.separateFiles) {
                                if (sessionStarted) advanceElapsed(Clock::now());
                                if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); admit = false; break; }
                                if ((cfg.segmentDurationSeconds > 0 || (delayedOrigin && !sessionStarted)) && commandPending()) {
                                    // Do not launch the second codec after a
                                    // queued command. Retire this owned empty
                                    // prospective part before Pause is serviced.
                                    if (delayedOrigin && !sessionStarted) endDelayedRequest(L"", false);
                                    else if (!finishSegment(L"", false)) closeRecording(L"");
                                    admit = false; break;
                                }
                                if (!cameraEncoder) cameraEncoder.emplace();
                                if (!cameraEncoder->open(cameraTemporaryIO, cfg.width, cfg.height, cfg.outputFps, error, cfg.encodingQuality, cfg.encodingMode, cfg.recoveryMode)) {
                                    closeRecording(L"Cannot start camera recording: " + error); admit = false; break;
                                }
                                cameraWriting = true;
                            }
                            pending = false;
                            if (!sessionStarted) { lastTick = Clock::now(); nextFrame = lastTick; }
                            power.update(true, !(delayedOrigin && !sessionStarted) && needDesktop);
                            { std::lock_guard<std::mutex> lock(mutex_);
                              status_.state = delayedOrigin ? State::Starting : State::Recording;
                              status_.error = false;
                              status_.message = delayedOrigin ? L"Preparing recording..." : recordingMessage(cfg.separateFiles); }
                        }
                        if (sessionStarted) advanceElapsed(Clock::now());
                        if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); admit = false; break; }
                        if (commandPending()) { admit = false; break; }
                        const auto space = cfg.stopOnLowDiskSpace ? queryRecordingSpace(recordingFolderIO) : RecordingSpace{};
                        if (!sessionStarted) {
                            activeDuration = Clock::duration::zero(); elapsed = 0;
                            lastTick = Clock::now(); nextFrame = lastTick;
                            if (observing) { observationStarted = GetTickCount64(); nextObservation = lastTick; }
                        } else advanceElapsed(Clock::now());
                        if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); admit = false; break; }
                        if (commandPending()) { admit = false; break; }
                        if (cfg.stopOnLowDiskSpace && !space.enough(cfg.separateFiles)) {
                            closeRecording(L"Recording stopped: " + recordingSpaceProblem(space, cfg.separateFiles) + L" Save folder: " + recordingFolder); admit = false; break;
                        }
                        // Expire existing evidence without new observation work
                        // after a slow automatic save/open. Both writes share
                        // the following admission instant and active bucket.
                        if (skipping) inspectSkipping();
                        if (commandPending()) { admit = false; break; }
                        admittedAt = Clock::now();
                        if (delayedOrigin && !sessionStarted) lastTick = admittedAt;
                        advanceElapsed(admittedAt);
                        if (limitExpired()) { recordingLimitReached = true; closeRecording(L""); admit = false; break; }
                        if (segmentExpired()) continue;
                        break;
                    }
                    if (!admit) continue;
                    if (cfg.segmentDurationSeconds > 0 && !encoder->frames()) {
                        nextSegmentBoundary = nextSegmentCut(activeDuration, cfg.segmentDurationSeconds);
                        ++segmentOrdinal;
                    }
                    if (!delayedOrigin) sessionStarted = true;
                    const auto admittedActiveMs = std::chrono::duration_cast<std::chrono::milliseconds>(activeDuration).count();
                    if (cfg.watermark.enabled) {
                        // Both completed compositions share one admission context
                        // and finish rendering before either writer accepts pixels.
                        WatermarkContext context;
                        context.activeMs = admittedActiveMs;
                        context.targetIntervalMs = incomingIntervalMs;
                        context.outputFps = cfg.outputFps;
                        GetLocalTime(&context.recordedLocal);
                        if (!watermark.apply(composed, context, error) ||
                            (cfg.separateFiles && !watermark.apply(cameraComposed, context, error))) {
                            closeRecording(L"Watermark stopped recording: " + error); continue;
                        }
                        lastWatermarkContext = context; haveWatermarkContext = true;
                    }
                    if (delayedOrigin) {
                        // Source, codec and both overlays remain preparation.
                        // Recheck Windows after those potentially slow steps,
                        // then let cancellation and paired admission race only
                        // at this one allocation-free mutex commit.
                        if (commandPending()) continue;
                        std::wstring recordingText = recordingMessage(cfg.separateFiles), wakeError;
                        uint64_t epoch = 0;
                        const bool known = queryDelayedWakeEpoch(epoch, wakeError);
                        bool cancelled;
                        { std::lock_guard<std::mutex> lock(mutex_);
                          cancelled = quit_ || stop_ || delayedCancel_;
                          if (!cancelled && known && epoch == delayedEpoch) {
                              delayedStartPending_ = delayedOrigin = delayedLayersFrozen = false;
                              sessionStarted = true; admittedAt = lastTick = nextFrame = Clock::now();
                              activeDuration = Clock::duration::zero(); elapsed = 0;
                              status_.state = State::Recording; status_.error = false; status_.message.swap(recordingText);
                          } }
                        if (cancelled || !known || epoch != delayedEpoch) {
                            endDelayedRequest(cancelled ? L"" : !known ? wakeError : L"Delayed start cancelled because Windows resumed.", !known && !cancelled);
                            continue;
                        }
                        power.update(true, needDesktop);
                    }
                    if (!encoder->write(composed, error)) {
                        closeRecording((cfg.separateFiles ? L"Desktop recording stopped: " : L"Recording stopped: ") + error); continue;
                    }
                    if (cfg.separateFiles && !cameraEncoder->write(cameraComposed, error)) {
                        closeRecording(L"Camera recording stopped: " + error); continue;
                    }
                    lastAdmission = admittedAt;
                    const auto interval = std::chrono::milliseconds(skipping ? timeSkip.onFrame(admittedActiveMs) : cfg.intervalMs);
                    incomingIntervalMs = interval.count();
                    if (skipping) nextFrame = std::max(nextFrame, lastAdmission);
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
                    if (skipping) inspectSkipping();
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.frames = cfg.separateFiles ? std::min(segmentFrameTotal(closedFrames[0], encoder->frames()),
                        segmentFrameTotal(closedFrames[1], cameraEncoder->frames())) : segmentFrameTotal(closedFrames[0], encoder->frames());
                    status_.elapsed = elapsed;
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
                    if (!prepareWatermark(cfg, error)) { publishPreviewError(error); continue; }
                    if (cfg.watermark.enabled) {
                        WatermarkContext context = lastWatermarkContext;
                        if (!(writing || pending) || !haveWatermarkContext) {
                            context.activeMs = 0; context.targetIntervalMs = cfg.intervalMs;
                            context.outputFps = cfg.outputFps;
                            GetLocalTime(&context.recordedLocal);
                        }
                        if (!watermark.apply(*previewBuffer, context, error)) { publishPreviewError(error); continue; }
                    }
                }
                // A successful video admission is not evidence that a failed
                // disposable preview recovered, nor does it own a new buffer.
                if (!refreshPreview) continue;
                { std::lock_guard<std::mutex> lock(mutex_);
                  // A selection change, Refresh, or Record may have retired
                  // this disposable preview while capture was in flight.
                  if (previewGeneration != previewGeneration_ || visualPreviewGeneration != visualPreviewGeneration_) continue;
                  if (cfg.preview && settings_.preview) {
                      auto previous = std::move(status_.preview);
                      status_.preview = previewBuffer;
                      previewBuffer = std::const_pointer_cast<Frame>(previous);
                  }
                  if (previewProblem_) {
                      status_.error = false;
                      status_.message = writing
                          ? paused ? L"Paused. Resume when you are ready." : recordingMessage(cfg.separateFiles)
                          : delayedRequest ? L"Waiting to start recording..." : L"Ready to record.";
                      previewProblem_ = false;
                  } }
            } catch (const std::exception&) {
                if (readingCameraInput) clearCameraInput();
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
                    if (delayedOrigin) endDelayedRequest(L"Delayed start stopped because a resource could not be allocated. You can try recording again.", true);
                    else if (writing || pending) closeRecording(L"Recording stopped because a resource could not be allocated. You can try recording again.");
                    else publishError(L"Capture could not allocate a resource. You can try recording again.", true);
                } catch (...) {
                    // Reporting or naming the completed recording may itself
                    // allocate. Still release the encoder and leave a retryable
                    // state; the .recording.mp4 file may contain saved frames.
                    try { if (encoder) { std::wstring ignored; encoder->finish(ignored); } } catch (...) {}
                    try { if (cameraEncoder) { std::wstring ignored; cameraEncoder->finish(ignored); } } catch (...) {}
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!start_) {
                        delayedSettings_.reset(); delayedStartPending_ = delayedCancel_ = false; status_.startDeadlineTick = 0;
                        previewProblem_ = false;
                        status_.state = State::Idle; status_.error = true;
                        if (terminalClockFrozen && (writing || pending)) status_.elapsed = elapsed;
                        status_.nightWaiting = false;
                        status_.recordingFailed = true;
                        try { status_.message = L"Capture stopped. Check the save folder and try recording again."; } catch (...) {}
                    }
                }
                releaseDesktopCaptureCache();
                resetWatermark();
                clearCameraInput();
                if (camera) camera->stop();
                cameraRunning = false;
                nightMode = nightQueued = nightFrameReady = false;
                cancelObservation(); skipping = observing = false; observationDesktop = {};
                writing = segmentWriting = cameraWriting = pending = paused = false;
                delayedRequest.reset(); delayedDeadline = 0; delayedOrigin = false;
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

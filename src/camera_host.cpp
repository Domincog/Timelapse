#include "camera_host.h"
#include "capture.h"
#include "person_pixels.h"
#include <mfapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <new>
#include <limits>
#include <optional>

namespace lapse {
namespace {
constexpr uint32_t protocolMagic = 0x4C43414D;
constexpr uint32_t protocolVersion = 5;
constexpr uint32_t maxWidth = 1280, maxHeight = 720;
constexpr size_t maxPixels = size_t(maxWidth) * maxHeight * 4;
constexpr size_t idCapacity = 4096;
constexpr wchar_t mappingPrefix[] = L"Local\\Timelapse.Camera.";
enum class HostState : uint32_t { Starting = 1, Ready, Failed, Stopped };
enum class PixelRequest : uint32_t { Preview, Night };
enum class NightState : uint32_t { Idle, Waiting, Integrating, Complete, Failed };
enum class ObservationState : uint32_t { Pending, Ready, Duplicate, Failed };
enum class ObservationKind : uint32_t { Activity, Person };

struct SharedFrame {
    uint32_t magic, version;
    HostState state;
    uint32_t width, height, bytes;
    uint64_t generation, receivedTick;
    uint64_t requested, completed;
    PixelRequest requestedKind, completedKind;
    uint64_t requestedToken, completedToken;
    // Completion also acknowledges a no-frame response. Bind the pixel slot
    // separately to the exact request that actually supplied these bytes.
    uint64_t publishedRequest, publishedToken;
    PixelRequest publishedKind;
    uint64_t nightCommand, nightToken;
    alignas(8) volatile LONG64 nightCancelled;
    uint32_t nightDuration;
    NightSettings nightSettings;
    uint64_t nightStateCommand;
    uint64_t nightSourceTick;
    uint64_t nightCompletedTick;
    NightState nightState;
    NightWindowResult nightResult;
    wchar_t nightError[512];
    // Independent of the full-pixel slot: acknowledgements without fresh
    // source bytes must never relabel an earlier descriptor as a new image.
    uint64_t observationRequested, observationCompleted;
    uint64_t observationToken, observationCompletedToken;
    uint64_t observationPublished, observationPublishedToken;
    alignas(8) volatile LONG64 observationCancelled;
    ObservationState observationState;
    CameraObservation observation;
    ObservationKind observationKind, observationCompletedKind, observationPublishedKind;
    person::Source personSource;
    wchar_t observationError[256];
    wchar_t id[idCapacity];
    wchar_t error[512];
    uint8_t pixels[maxPixels];
};

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE next = nullptr) { if (value) CloseHandle(value); value = next; }
};
struct View {
    SharedFrame* value = nullptr;
    ~View() { reset(); }
    void reset(SharedFrame* next = nullptr) { if (value) UnmapViewOfFile(value); value = next; }
};
struct PersonView {
    uint8_t* value = nullptr;
    ~PersonView() { reset(); }
    void reset(uint8_t* next = nullptr) { if (value) UnmapViewOfFile(value); value = next; }
};
struct SharedLock {
    HANDLE mutex;
    DWORD result;
    explicit SharedLock(HANDLE handle, DWORD timeout = 10) : mutex(handle), result(WaitForSingleObject(handle, timeout)) {}
    ~SharedLock() { if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) ReleaseMutex(mutex); }
    bool acquired() const { return result == WAIT_OBJECT_0; }
};

bool fail(std::wstring& error, const wchar_t* message, DWORD code = GetLastError()) {
    error = message;
    if (code) error += L" " + errorText(HRESULT_FROM_WIN32(code));
    return false;
}

void setMessage(SharedFrame& shared, HostState state, const std::wstring& error) {
    shared.state = state;
    const size_t count = std::min(error.size(), std::size(shared.error) - 1);
    std::wmemcpy(shared.error, error.data(), count);
    shared.error[count] = L'\0';
}

bool validHeader(const SharedFrame& shared) {
    return shared.magic == protocolMagic && shared.version == protocolVersion;
}
uint64_t cancelledNight(SharedFrame& shared) noexcept {
    return static_cast<uint64_t>(InterlockedCompareExchange64(&shared.nightCancelled, 0, 0));
}
uint64_t cancelledObservation(SharedFrame& shared) noexcept {
    return static_cast<uint64_t>(InterlockedCompareExchange64(&shared.observationCancelled, 0, 0));
}

std::wstring modulePath() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size()) return {};
    path.resize(count);
    return path;
}

int hostMain(const wchar_t* mappingName) {
    const std::wstring name = mappingName;
    if (name.compare(0, std::size(mappingPrefix) - 1, mappingPrefix) != 0 || name.size() > 128) return 2;
    Handle mapping, mutex, stop, request, response, personMapping;
    PersonView personView;
    mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    mutex.value = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".mutex").c_str());
    stop.value = OpenEventW(SYNCHRONIZE, FALSE, (name + L".stop").c_str());
    request.value = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + L".request").c_str());
    response.value = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L".response").c_str());
    if (!mapping.value || !mutex.value || !stop.value || !request.value || !response.value) return 3;
    View view;
    view.value = static_cast<SharedFrame*>(MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedFrame)));
    if (!view.value) return 4;
    std::wstring id;
    {
        SharedLock lock(mutex.value, 100);
        if (!lock.acquired() || !validHeader(*view.value)) return 5;
        const wchar_t* end = static_cast<const wchar_t*>(std::wmemchr(view.value->id, L'\0', std::size(view.value->id)));
        if (!end || end == view.value->id) return 6;
        id.assign(view.value->id, size_t(end - view.value->id));
    }
    auto publishError = [&](const std::wstring& error) {
        SharedLock lock(mutex.value, 100);
        if (lock.acquired()) setMessage(*view.value, HostState::Failed, error);
    };
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { publishError(L"Windows could not initialize camera capture."); return 7; }
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) {
        publishError(L"Windows Media Foundation is unavailable."); CoUninitialize(); return 8;
    }
    int result = 0;
    try {
        Camera camera;
        std::wstring error;
        if (WaitForSingleObject(stop.value, 0) == WAIT_OBJECT_0) result = 0;
        else if (!camera.start(id, error)) { publishError(error); result = 9; }
        else {
            Frame frame, nightFrame, observationFrame;
            std::unique_ptr<CameraPersonInput> personInput;
            std::unique_ptr<person_pixels::Cache> personCache;
            std::unique_ptr<NightAccumulator> accumulator;
            const Frame* completedNight = nullptr;
            CameraSampleInfo watermark, seen;
            NightSettings nightSettings;
            NightWindowResult nightResult;
            NightState nightState = NightState::Idle;
            bool nightDelivered = false;
            uint64_t nightCommand = 0, nightToken = 0, nextNight = 0, nightSourceTick = 0, nightCompletedTick = 0;
            uint32_t nightDuration = 0, duplicateTimes = 0;
            int64_t lastTimestamp = 0;
            bool trustedTimestamp = false;
            std::wstring nightError;
            CameraSampleInfo observationSeen;
            uint64_t observationToken = 0, observationLastRequest = 0;
            const HANDLE events[] = {stop.value, request.value};
            auto cancel = [&] {
                // Explicit cancellation releases the optional work buffers and
                // gain history. Next ordinary preview has baseline memory/cost.
                accumulator.reset(); nightFrame = {}; completedNight = nullptr;
                nightState = NightState::Idle; nightResult = {}; nightCompletedTick = 0; nightError.clear();
                nightDelivered = false;
            };
            auto nightFailed = [&](const wchar_t* message) {
                nightState = NightState::Failed; nightError = message; completedNight = nullptr;
                if (accumulator) accumulator->resetExposure();
            };
            for (;;) {
                const bool accumulating = nightState == NightState::Waiting || nightState == NightState::Integrating;
                const uint64_t before = GetTickCount64();
                const DWORD delay = accumulating ? DWORD(nextNight > before ? std::min<uint64_t>(nextNight - before, NightCadenceMs) : 0) : INFINITE;
                const DWORD wake = WaitForMultipleObjects(2, events, FALSE, delay);
                if (wake == WAIT_OBJECT_0) break;
                if (wake != WAIT_TIMEOUT && wake != WAIT_OBJECT_0 + 1) { result = 12; break; }
                // Cancellation never waits for the sharing mutex. Even while
                // it is held, the helper stops sampling and drops partial work.
                if (nightCommand && cancelledNight(*view.value) >= nightCommand && nightState != NightState::Idle) cancel();
                if (observationLastRequest && cancelledObservation(*view.value) >= observationLastRequest) {
                    observationFrame = {}; observationSeen = {}; observationToken = observationLastRequest = 0;
                }
                uint64_t requested = 0, requestedToken = 0;
                uint64_t observationRequested = 0, requestedObservationToken = 0;
                ObservationKind requestedObservationKind = ObservationKind::Activity;
                PixelRequest requestedKind = PixelRequest::Preview;
                {
                    SharedLock lock(mutex.value);
                    if (lock.result == WAIT_ABANDONED || lock.result == WAIT_FAILED) { result = 12; break; }
                    if (!lock.acquired()) {
                        if (WaitForSingleObject(stop.value, 50) == WAIT_OBJECT_0) break;
                        SetEvent(request.value); continue;
                    }
                    if (!validHeader(*view.value)) { result = 12; break; }
                    if (view.value->nightCommand != nightCommand) {
                        nightCommand = view.value->nightCommand;
                        nightToken = view.value->nightToken;
                        nightDuration = view.value->nightDuration;
                        nightSettings = view.value->nightSettings;
                        completedNight = nullptr; nightResult = {}; nightCompletedTick = 0; nightError.clear();
                        nightDelivered = false;
                        watermark = {}; seen = {}; nightSourceTick = 0; duplicateTimes = 0; trustedTimestamp = false;
                        nightState = NightState::Waiting; nextNight = GetTickCount64();
                        if (cancelledNight(*view.value) >= nightCommand) cancel();
                        else if (!nightToken || nightDuration < NightMinDurationMs || nightDuration > NightMaxDurationMs ||
                                 !nightSettings.enabled || !validNightSettings(nightSettings))
                            nightFailed(L"The camera received invalid night blend settings.");
                    }
                    requested = view.value->requested;
                    requestedKind = view.value->requestedKind;
                    requestedToken = view.value->requestedToken;
                    if (requested == view.value->completed) requested = 0;
                    if (view.value->observationRequested != view.value->observationCompleted &&
                        view.value->observationRequested > cancelledObservation(*view.value)) {
                        observationRequested = observationLastRequest = view.value->observationRequested;
                        requestedObservationToken = view.value->observationToken;
                        requestedObservationKind = view.value->observationKind;
                        if (observationToken != requestedObservationToken) {
                            observationToken = requestedObservationToken; observationSeen = {};
                        }
                    }
                }
                const Frame* rawObservation = nullptr;
                CameraSampleInfo rawInfo;
                const uint64_t sampleAt = GetTickCount64();
                if ((nightState == NightState::Waiting || nightState == NightState::Integrating) && sampleAt >= nextNight) {
                    CameraSampleInfo info;
                    const bool fresh = camera.latestNewer(nightFrame, error, info, seen);
                    if (fresh) { rawObservation = &nightFrame; rawInfo = info; }
                    nightSourceTick = info.receivedTick;
                    if (!error.empty()) { publishError(error); result = 10; break; }
                    const uint64_t copiedAt = GetTickCount64();
                    if (info.receivedTick > copiedAt) nightFailed(L"The camera returned invalid source freshness data.");
                    else if (info.receivedTick && copiedAt - info.receivedTick > 3000)
                        nightFailed(L"No fresh camera frame arrived for 3 seconds. Check the camera connection and try again.");
                    const bool inWindow = fresh && info.receivedTick >= nightResult.beginTick && info.receivedTick < nightResult.endTick;
                    // Facts from a fresh post-end sample cannot invalidate
                    // already admitted light. A format-only observation has no
                    // event time, so retain conservative epoch rejection.
                    const bool inspectEpoch = !fresh || inWindow;
                    if (nightState == NightState::Integrating && inspectEpoch && info.epoch && info.epoch != watermark.epoch)
                        nightFailed(L"The camera format or source timeline changed during the night blend. Try recording again.");
                    if (fresh && (nightState == NightState::Waiting || (nightState == NightState::Integrating && inWindow)) &&
                        (!nightFrame.valid() || nightFrame.width > int(maxWidth) || nightFrame.height > int(maxHeight)))
                        nightFailed(L"The camera returned an unsupported night blend frame size.");
                    if (fresh && nightState == NightState::Waiting) {
                        if (!accumulator) accumulator = std::make_unique<NightAccumulator>();
                        if (!accumulator->prepare(nightFrame.width, nightFrame.height, error) || !accumulator->begin(nightSettings)) {
                            nightFailed(L"The camera could not prepare the night blend.");
                        } else {
                            // This first fresh sample is only the watermark.
                            // No pre-request measurement enters the new window.
                            watermark = info; seen = info;
                            trustedTimestamp = info.timestampValid; lastTimestamp = info.timestamp100ns;
                            nightResult.timestampFallback = !trustedTimestamp;
                            nightResult.beginTick = GetTickCount64();
                            nightResult.endTick = nightResult.beginTick + nightDuration;
                            nightState = NightState::Integrating;
                        }
                    } else if (fresh && nightState == NightState::Integrating && info.sequence > watermark.sequence &&
                               info.receivedTick >= nightResult.beginTick && info.receivedTick < nightResult.endTick) {
                        bool accept = true;
                        if (!nightResult.timestampFallback) {
                            if (!info.timestampValid || info.timestamp100ns < lastTimestamp) nightResult.timestampFallback = true;
                            else if (trustedTimestamp && info.timestamp100ns == lastTimestamp) {
                                // A zero timestamp is valid. Only repeated new
                                // source deliveries establish an unusable clock.
                                if (++duplicateTimes >= 3) nightResult.timestampFallback = true;
                                else accept = false;
                            } else { duplicateTimes = 0; lastTimestamp = info.timestamp100ns; trustedTimestamp = true; }
                        }
                        if (accept) {
                            const auto added = accumulator->add(nightFrame, info.sequence);
                            if (added == NightAdd::Added) {
                                if (!nightResult.firstSampleTick) nightResult.firstSampleTick = info.receivedTick;
                                nightResult.lastSampleTick = info.receivedTick;
                            } else if (added == NightAdd::Invalid || added == NightAdd::Full)
                                nightFailed(L"The camera night blend exceeded its supported sample bounds.");
                        }
                        seen = info;
                    } else if (fresh) seen = info;
                    const uint64_t after = GetTickCount64();
                    nextNight = after + NightCadenceMs;
                    if (nightState == NightState::Integrating) {
                        nextNight = std::min(nextNight, nightResult.endTick);
                        if (after >= nightResult.endTick) {
                            completedNight = accumulator->finish(nightResult.exposure);
                            nightCompletedTick = GetTickCount64();
                            if (nightSourceTick && nightCompletedTick - nightSourceTick > 3000)
                                nightFailed(L"No fresh camera frame arrived for 3 seconds. Check the camera connection and try again.");
                            else if (completedNight && nightResult.exposure.samples) {
                                // Aim for at least two actual source deliveries
                                // on the next Auto window, even when bright slow
                                // cameras would otherwise request only a second.
                                const uint64_t samples = nightResult.exposure.samples;
                                const uint64_t twoSamples = (2ULL * nightDuration + samples - 1) / samples;
                                const uint64_t rounded = ((twoSamples + 999) / 1000) * 1000;
                                nightResult.exposure.suggestedDurationMs = std::clamp(
                                    std::max(nightResult.exposure.suggestedDurationMs, int(std::min<uint64_t>(rounded, NightMaxDurationMs))),
                                    NightMinDurationMs, NightMaxDurationMs);
                                nightState = NightState::Complete;
                            } else nightFailed(L"No distinct camera samples arrived during the night blend. Choose a longer blend and capture interval, or reconnect the camera.");
                        }
                    }
                    if (nightCommand && cancelledNight(*view.value) >= nightCommand) { cancel(); rawObservation = nullptr; }
                }
                uint64_t receivedTick = 0;
                const Frame* publication = nullptr;
                if (requested && requestedKind == PixelRequest::Preview) {
                    if (observationRequested) {
                        CameraSampleInfo info;
                        if (camera.latestNewer(frame, error, info, {})) {
                            publication = &frame; receivedTick = info.receivedTick;
                            rawObservation = &frame; rawInfo = info;
                        }
                    } else if (camera.latest(frame, error, receivedTick)) publication = &frame;
                    if (!publication && !error.empty()) { publishError(error); result = 10; break; }
                } else if (requested && requestedKind == PixelRequest::Night && requestedToken == nightToken &&
                           nightState == NightState::Complete && cancelledNight(*view.value) < nightCommand) {
                    publication = completedNight; receivedTick = nightResult.lastSampleTick;
                }
                if (publication && (!publication->valid() || publication->width > int(maxWidth) || publication->height > int(maxHeight))) {
                    publishError(L"The camera returned an unsupported frame size."); result = 11; break;
                }
                std::optional<CameraObservation> observation;
                ObservationState observationState = ObservationState::Pending;
                std::wstring observationError;
                const wchar_t* observationFallback = nullptr;
                const bool deliveringNight = publication && requestedKind == PixelRequest::Night;
                const bool nightOwnsRawRead = nightState == NightState::Waiting || nightState == NightState::Integrating ||
                    (nightState == NightState::Complete && !nightDelivered);
                const bool deferObservation = deliveringNight || (nightOwnsRawRead && !rawObservation);
                if (observationRequested && !deferObservation && cancelledObservation(*view.value) < observationRequested) {
                    // Optional observation errors are isolated from recording.
                    // Reuse this iteration's raw conversion (never a processed
                    // Night result); otherwise convert only a distinct sample.
                    try {
                        observation.emplace();
                        bool fresh = rawObservation && (rawInfo.epoch != observationSeen.epoch || rawInfo.sequence > observationSeen.sequence);
                        if (!fresh && !nightOwnsRawRead) {
                            fresh = camera.latestNewer(observationFrame, observationError, rawInfo, observationSeen);
                            rawObservation = fresh ? &observationFrame : nullptr;
                        }
                        const uint64_t now = GetTickCount64();
                        if (!observationError.empty()) observationState = ObservationState::Failed;
                        else if (!fresh) observationState = ObservationState::Duplicate;
                        else if (!rawInfo.epoch || !rawInfo.sequence || !rawInfo.receivedTick || rawInfo.receivedTick > now ||
                                 now - rawInfo.receivedTick > 3000 || !rawObservation->valid() ||
                                 rawObservation->width > int(maxWidth) || rawObservation->height > int(maxHeight) ||
                                 requestedObservationKind > ObservationKind::Person) {
                            observationState = ObservationState::Failed;
                            observationError = L"The camera activity check returned invalid or stale source data.";
                        } else {
                            observation->epoch = rawInfo.epoch; observation->sequence = rawInfo.sequence;
                            observation->receivedTick = rawInfo.receivedTick;
                            observation->sourceWidth = rawObservation->width; observation->sourceHeight = rawObservation->height;
                            observationState = ObservationState::Ready;
                            if (requestedObservationKind == ObservationKind::Activity) {
                                if (!describeTimeSkipFrame(*rawObservation, observation->descriptor)) {
                                    observationState = ObservationState::Failed;
                                    observationError = L"The camera activity image is invalid.";
                                }
                            } else {
                                if (!personView.value) {
                                    if (!personMapping.value) personMapping.value = OpenFileMappingW(FILE_MAP_WRITE, FALSE, (name + L".person").c_str());
                                    if (personMapping.value) personView.reset(static_cast<uint8_t*>(MapViewOfFile(
                                        personMapping.value, FILE_MAP_WRITE, 0, 0, person::MaxBgrBytes)));
                                }
                                if (!personInput) personInput = std::make_unique<CameraPersonInput>();
                                if (!personCache) personCache = std::make_unique<person_pixels::Cache>();
                                person_pixels::Geometry geometry;
                                if (!personView.value || !person_pixels::prepare(rawObservation->pixels.data(), rawObservation->pixels.size(),
                                        rawObservation->width, rawObservation->height, personInput->bgr.data(), personInput->bgr.size(), geometry, *personCache)) {
                                    observationState = ObservationState::Failed;
                                    observationError = L"The camera person-check image could not be prepared.";
                                } else {
                                    personInput->source = {requestedObservationToken, rawInfo.epoch, rawInfo.sequence, rawInfo.receivedTick,
                                        uint32_t(rawObservation->width), uint32_t(rawObservation->height), uint32_t(geometry.width), uint32_t(geometry.height)};
                                }
                            }
                        }
                    } catch (...) {
                        observationState = ObservationState::Failed;
                        observationFallback = L"The camera activity check is unavailable.";
                    }
                }
                SharedLock lock(mutex.value);
                if (lock.result == WAIT_ABANDONED || lock.result == WAIT_FAILED) { result = 12; break; }
                if (!lock.acquired()) {
                    if (WaitForSingleObject(stop.value, 50) == WAIT_OBJECT_0) break;
                    SetEvent(request.value); continue;
                }
                if (view.value->nightCommand == nightCommand) {
                    view.value->nightStateCommand = nightCommand;
                    view.value->nightSourceTick = nightSourceTick;
                    view.value->nightCompletedTick = nightCompletedTick;
                    view.value->nightState = nightState;
                    view.value->nightResult = nightResult;
                    const size_t count = std::min(nightError.size(), std::size(view.value->nightError) - 1);
                    std::wmemcpy(view.value->nightError, nightError.data(), count); view.value->nightError[count] = L'\0';
                }
                if (observationRequested && !deferObservation && view.value->observationRequested == observationRequested &&
                    view.value->observationToken == requestedObservationToken && view.value->observationKind == requestedObservationKind &&
                    cancelledObservation(*view.value) < observationRequested) {
                    if (observationState == ObservationState::Ready) {
                        view.value->observation = *observation;
                        if (requestedObservationKind == ObservationKind::Person) {
                            const auto& source = personInput->source;
                            std::memcpy(personView.value, personInput->bgr.data(), size_t(source.width) * source.height * 3);
                            view.value->personSource = source;
                        }
                        view.value->observationPublished = observationRequested;
                        view.value->observationPublishedToken = requestedObservationToken;
                        view.value->observationPublishedKind = requestedObservationKind;
                        observationSeen = rawInfo;
                    }
                    view.value->observationState = observationState;
                    view.value->observationCompleted = observationRequested;
                    view.value->observationCompletedToken = requestedObservationToken;
                    view.value->observationCompletedKind = requestedObservationKind;
                    const wchar_t* detail = observationFallback ? observationFallback : observationError.c_str();
                    const size_t count = std::min(std::wcslen(detail), std::size(view.value->observationError) - 1);
                    std::wmemcpy(view.value->observationError, detail, count);
                    view.value->observationError[count] = L'\0';
                    SetEvent(response.value);
                }
                // Odd means publication is in progress; only an even generation
                // with a valid size can ever be copied by the client.
                if (publication) {
                    ++view.value->generation;
                    view.value->width = uint32_t(publication->width); view.value->height = uint32_t(publication->height);
                    view.value->bytes = static_cast<uint32_t>(publication->pixels.size());
                    std::memcpy(view.value->pixels, publication->pixels.data(), publication->pixels.size());
                    view.value->receivedTick = receivedTick;
                    view.value->publishedRequest = requested;
                    view.value->publishedKind = requestedKind;
                    view.value->publishedToken = requestedToken;
                    setMessage(*view.value, HostState::Ready, L"");
                    ++view.value->generation;
                    if (deliveringNight) nightDelivered = true;
                }
                if (requested) {
                    view.value->completed = requested;
                    view.value->completedKind = requestedKind;
                    view.value->completedToken = requestedToken;
                    SetEvent(response.value);
                }
                // Publish an already completed blend before optional driver
                // conversion. During integration, leave the report pending for
                // the next existing raw Night sample instead of adding a read
                // or a busy-loop wake that could delay the exposure window.
                if (observationRequested && deferObservation && deliveringNight) SetEvent(request.value);
            }
            // A stuck driver shutdown affects only this helper. The parent owns
            // a kill-on-close job and bounds graceful shutdown to 500 ms.
            camera.stop();
        }
        if (!result) {
            SharedLock lock(mutex.value);
            if (lock.acquired()) setMessage(*view.value, HostState::Stopped, L"");
        }
    } catch (const std::bad_alloc&) {
        publishError(L"Not enough memory to capture the camera."); result = 13;
    } catch (...) {
        publishError(L"The camera stopped unexpectedly. Reconnect it and try again."); result = 14;
    }
    MFShutdown(); CoUninitialize();
    return result;
}
}

struct CameraClient::Impl {
    Handle mapping, mutex, stopEvent, requestEvent, responseEvent, process, job, personMapping;
    View view;
    PersonView personView;
    std::wstring mappingName;
    uint64_t started = 0, contentionSince = 0, generation = 0;
    uint64_t request = 0, requestedTick = 0, requestedGeneration = 0;
    PixelRequest requestedKind = PixelRequest::Preview;
    uint64_t nightCommand = 0, nightToken = 0, nightRequestedTick = 0, nightDeadline = 0;
    uint64_t observationRequest = 0, observationLastRequest = 0, observationToken = 0, observationDeadline = 0;
    uint64_t observationEpoch = 0, observationSequence = 0;
    ObservationKind observationKind = ObservationKind::Activity;
    bool delivered = false, contended = false, activationFailed = false;
};

CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() { stop(); }

bool CameraClient::start(const std::wstring& id, std::wstring& error) {
    stop(); error.clear();
    if (id.empty() || id.size() >= idCapacity || id.find(L'\0') != std::wstring::npos) {
        error = L"Select a valid camera first."; return false;
    }
    try {
        GUID guid{};
        HRESULT hr = CoCreateGuid(&guid);
        if (FAILED(hr)) { error = L"Windows could not prepare camera capture."; return false; }
        wchar_t token[40]{};
        StringFromGUID2(guid, token, static_cast<int>(std::size(token)));
        const std::wstring name = std::wstring(mappingPrefix) + token;
        impl_->mappingName = name;
        impl_->mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedFrame), name.c_str());
        if (!impl_->mapping.value || GetLastError() == ERROR_ALREADY_EXISTS) {
            fail(error, L"Windows could not reserve the camera frame."); stop(); return false;
        }
        impl_->mutex.value = CreateMutexW(nullptr, FALSE, (name + L".mutex").c_str());
        if (!impl_->mutex.value || GetLastError() == ERROR_ALREADY_EXISTS) {
            fail(error, L"Windows could not prepare camera sharing."); stop(); return false;
        }
        impl_->stopEvent.value = CreateEventW(nullptr, TRUE, FALSE, (name + L".stop").c_str());
        if (!impl_->stopEvent.value || GetLastError() == ERROR_ALREADY_EXISTS) {
            fail(error, L"Windows could not prepare camera shutdown."); stop(); return false;
        }
        impl_->requestEvent.value = CreateEventW(nullptr, FALSE, FALSE, (name + L".request").c_str());
        if (!impl_->requestEvent.value || GetLastError() == ERROR_ALREADY_EXISTS) {
            fail(error, L"Windows could not prepare camera requests."); stop(); return false;
        }
        impl_->responseEvent.value = CreateEventW(nullptr, FALSE, FALSE, (name + L".response").c_str());
        if (!impl_->responseEvent.value || GetLastError() == ERROR_ALREADY_EXISTS) {
            fail(error, L"Windows could not prepare camera responses."); stop(); return false;
        }
        impl_->view.value = static_cast<SharedFrame*>(MapViewOfFile(impl_->mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedFrame)));
        if (!impl_->view.value) { fail(error, L"Windows could not access the camera frame."); stop(); return false; }
        // Newly created page-file backed mappings are zero initialized by Windows.
        auto& shared = *impl_->view.value;
        shared.magic = protocolMagic; shared.version = protocolVersion; shared.state = HostState::Starting;
        std::wmemcpy(shared.id, id.c_str(), id.size() + 1);
        impl_->job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!impl_->job.value || !SetInformationJobObject(impl_->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            fail(error, L"Windows could not isolate the camera driver."); stop(); return false;
        }
        const std::wstring executable = modulePath();
        if (executable.empty()) { error = L"Cannot locate the application for camera capture."; stop(); return false; }
        // Only the random mapping token is on the command line, never the device ID.
        std::wstring command = L"\"" + executable + L"\" --camera-host \"" + name + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
            fail(error, L"Windows could not start camera capture."); stop(); return false;
        }
        impl_->process.value = process.hProcess;
        Handle thread; thread.value = process.hThread;
        if (!AssignProcessToJobObject(impl_->job.value, impl_->process.value)) {
            fail(error, L"Windows could not isolate the camera driver.");
            TerminateProcess(impl_->process.value, ERROR_PROCESS_ABORTED); stop(); return false;
        }
        impl_->started = GetTickCount64();
        shared.requested = impl_->request = 1;
        impl_->requestedTick = impl_->started;
        SetEvent(impl_->requestEvent.value);
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
            fail(error, L"Windows could not start the camera helper."); stop(); return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        stop(); error = L"Not enough memory to start camera capture."; return false;
    }
}

void CameraClient::stop() {
    if (impl_->stopEvent.value) SetEvent(impl_->stopEvent.value);
    if (impl_->process.value && WaitForSingleObject(impl_->process.value, 500) == WAIT_TIMEOUT)
        TerminateProcess(impl_->process.value, ERROR_TIMEOUT);
    // The unnamed job also guarantees cleanup after a crash or forced app exit.
    impl_->job.reset(); impl_->process.reset();
    impl_->view.reset(); impl_->mapping.reset(); impl_->mutex.reset(); impl_->stopEvent.reset();
    impl_->personView.reset(); impl_->personMapping.reset(); impl_->mappingName.clear();
    impl_->requestEvent.reset(); impl_->responseEvent.reset();
    impl_->started = impl_->contentionSince = impl_->generation = 0;
    impl_->request = impl_->requestedTick = impl_->requestedGeneration = 0;
    impl_->nightCommand = impl_->nightToken = impl_->nightRequestedTick = impl_->nightDeadline = 0;
    impl_->observationRequest = impl_->observationLastRequest = impl_->observationToken = impl_->observationDeadline = 0;
    impl_->observationEpoch = impl_->observationSequence = 0;
    impl_->requestedKind = PixelRequest::Preview;
    impl_->delivered = impl_->contended = impl_->activationFailed = false;
}

bool CameraClient::latest(Frame& output, std::wstring& error) {
    error.clear();
    if (!impl_->process.value || !impl_->view.value) { error = L"The camera is not running."; return false; }
    auto failActivation = [&] {
        impl_->activationFailed = true;
        error = L"The camera did not start within 8 seconds. Check Windows camera access and reconnect it.";
        return false;
    };
    if (impl_->activationFailed) return failActivation();
    bool expiredRefresh = false;
    auto read = [&](bool& waiting) {
        waiting = false;
        uint64_t now = GetTickCount64();
        SharedLock lock(impl_->mutex.value);
        if (lock.result == WAIT_ABANDONED) { error = L"The camera helper stopped while sharing a frame. Try again."; return false; }
        if (lock.result == WAIT_FAILED) return fail(error, L"Windows could not read the camera frame.");
        if (!lock.acquired()) {
            if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0) {
                error = L"The camera helper stopped unexpectedly. Reconnect the camera and try again."; return false;
            }
            // Brief sharing contention says nothing about source freshness.
            // An intentionally old publication is refreshed after acquisition.
            if (!impl_->contended) {
                impl_->contended = true;
                impl_->contentionSince = now;
            } else if (now - impl_->contentionSince > (impl_->delivered ? 3000 : 8000)) {
                error = L"Camera frame sharing stayed unavailable. Reconnect the camera and try again.";
            }
            return false;
        }
        now = GetTickCount64();
        impl_->contended = false; impl_->contentionSince = 0;
        auto& shared = *impl_->view.value;
        if (!validHeader(shared)) { error = L"The camera helper returned invalid data. Try again."; return false; }
        if (shared.completed > shared.requested || impl_->request > shared.requested) {
            error = L"The camera helper returned an invalid response. Try again."; return false;
        }
        if (shared.state == HostState::Failed) {
            const wchar_t* end = static_cast<const wchar_t*>(std::wmemchr(shared.error, L'\0', std::size(shared.error)));
            error = end ? std::wstring(shared.error, size_t(end - shared.error)) : L"The camera helper returned an invalid error.";
            if (error.empty()) error = L"The camera stopped unexpectedly. Try again.";
            return false;
        }
        if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0 || shared.state == HostState::Stopped) {
            error = L"The camera helper stopped unexpectedly. Reconnect the camera and try again."; return false;
        }
        auto requestFrame = [&] {
            impl_->request = ++shared.requested;
            shared.requestedKind = impl_->requestedKind = PixelRequest::Preview;
            shared.requestedToken = 0;
            impl_->requestedTick = now;
            impl_->requestedGeneration = shared.generation;
            expiredRefresh = !impl_->delivered && now - impl_->started > 8000;
            if (!ResetEvent(impl_->responseEvent.value) || !SetEvent(impl_->requestEvent.value))
                return fail(error, L"Windows could not request a camera frame.");
            return true;
        };
        // A completed night result is read only by nightResult. Preview must
        // not steal its typed response from the single shared pixel slot.
        if (impl_->request && impl_->requestedKind == PixelRequest::Night) return false;
        if (!impl_->request && !requestFrame()) return false;
        if (shared.completed < impl_->request) {
            waiting = true;
            // Warmup may require several requests. None may move the original
            // activation deadline; only an already delivered camera gets a
            // fresh per-request deadline.
            const uint64_t began = impl_->delivered ? impl_->requestedTick : impl_->started;
            if (now - began > (impl_->delivered ? 3000 : 8000)) {
                if (!impl_->delivered) return failActivation();
                error = L"The camera did not answer a frame request for 3 seconds. Reconnect it and try again.";
            }
            return false;
        }
        if (shared.completedKind != PixelRequest::Preview || shared.completedToken) {
            error = L"The camera helper returned the wrong preview response. Try again."; return false;
        }
        const uint64_t responseRequest = impl_->request;
        impl_->request = 0;
        if (shared.state == HostState::Starting) {
            if (now - impl_->started > 8000) {
                // The eager warmup response can sit uncollected while healthy
                // samples arrive. Give an old completed response one bounded
                // fresh request, without resetting the activation deadline.
                if (impl_->requestedTick - impl_->started <= 8000) {
                    waiting = requestFrame();
                    return false;
                }
                return failActivation();
            }
            return false;
        }
        if (shared.generation == impl_->requestedGeneration || shared.publishedRequest != responseRequest ||
            shared.publishedKind != PixelRequest::Preview || shared.publishedToken) {
            if (now - shared.receivedTick > 3000)
                error = L"No fresh camera frame arrived for 3 seconds. Check the camera connection and try again.";
            return false;
        }
        const uint64_t frameNow = GetTickCount64();
        // A consumer can wait minutes before collecting the eager startup frame or
        // an outstanding response. Refresh that old response once; only a sample
        // fetched for the current request can establish that the device is stale.
        if (shared.receivedTick <= frameNow && frameNow - shared.receivedTick > 3000 &&
            frameNow - impl_->requestedTick > 3000) {
            waiting = requestFrame();
            return false;
        }
        if (shared.state != HostState::Ready || !shared.width || !shared.height || shared.width > maxWidth || shared.height > maxHeight ||
            uint64_t(shared.width) * shared.height * 4 != shared.bytes || !shared.generation || (shared.generation & 1) ||
            shared.generation < impl_->generation || shared.receivedTick > frameNow) {
            error = L"The camera helper returned an invalid frame. Try again."; return false;
        }
        if (frameNow - shared.receivedTick > 3000) {
            error = L"No fresh camera frame arrived for 3 seconds. Check the camera connection and try again."; return false;
        }
        try {
            output.pixels.resize(shared.bytes);
            std::memcpy(output.pixels.data(), shared.pixels, shared.bytes);
            output.width = int(shared.width); output.height = int(shared.height);
            impl_->delivered = true; impl_->generation = shared.generation;
            return true;
        } catch (const std::bad_alloc&) { error = L"Not enough memory to read the camera frame."; return false; }
    };
    bool waiting = false;
    if (read(waiting)) return true;
    if (!waiting || !error.empty()) return false;
    // The helper copies an already-arrived sample, so a short bounded wait
    // usually completes this request without costing another preview period.
    // Slow or stuck drivers remain entirely outside the application's process.
    // A first consumer collecting an expired warmup response gets one slightly
    // longer grace for scheduling the helper; ordinary requests stay at 10 ms.
    const DWORD response = WaitForSingleObject(impl_->responseEvent.value, expiredRefresh ? 50 : 10);
    if (response == WAIT_OBJECT_0 && read(waiting)) return true;
    if (response == WAIT_FAILED) return fail(error, L"Windows could not receive the camera frame.");
    // A late collected warmup response gets only the bounded wait above. A
    // stuck fresh request must not turn that grace into another eight seconds.
    if (!impl_->delivered && error.empty() && GetTickCount64() - impl_->started > 8000)
        return failActivation();
    return false;
}

bool CameraClient::beginNight(uint64_t token, uint32_t durationMs, const NightSettings& settings, std::wstring& error) {
    error.clear();
    if (!token || durationMs < NightMinDurationMs || durationMs > NightMaxDurationMs || !settings.enabled || !validNightSettings(settings)) {
        error = L"Choose a supported camera night blend duration and brightness."; return false;
    }
    if (!impl_->process.value || !impl_->view.value) { error = L"The camera is not running."; return false; }
    if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0) {
        error = L"The camera helper stopped unexpectedly. Reconnect the camera and try again."; return false;
    }
    if (impl_->nightToken == token) return true;
    const uint64_t now = GetTickCount64();
    SharedLock lock(impl_->mutex.value);
    if (!lock.acquired()) {
        if (lock.result == WAIT_ABANDONED || lock.result == WAIT_FAILED) {
            error = L"Windows could not request the camera night blend."; return false;
        }
        if (!impl_->contended) { impl_->contended = true; impl_->contentionSince = now; }
        else if (now - impl_->contentionSince > (impl_->delivered ? 3000 : 8000))
            error = L"Camera frame sharing stayed unavailable. Reconnect the camera and try again.";
        return false;
    }
    impl_->contended = false; impl_->contentionSince = 0;
    auto& shared = *impl_->view.value;
    if (!validHeader(shared) || shared.nightCommand == std::numeric_limits<uint64_t>::max()) {
        error = L"The camera helper returned invalid data. Try again."; return false;
    }
    impl_->nightCommand = ++shared.nightCommand;
    impl_->nightToken = shared.nightToken = token;
    shared.nightDuration = durationMs; shared.nightSettings = settings;
    impl_->nightRequestedTick = now;
    impl_->nightDeadline = now + durationMs + (impl_->delivered ? 3000 : 8000);
    // A newer begin owns the result channel immediately, even when an older
    // pixel response is still in transit. Preview commands remain independent.
    if (impl_->requestedKind == PixelRequest::Night) impl_->request = 0;
    if (!SetEvent(impl_->requestEvent.value)) {
        cancelNight(); return fail(error, L"Windows could not request the camera night blend.");
    }
    return true;
}

void CameraClient::cancelNight() noexcept {
    if (impl_->view.value && impl_->nightCommand) {
        InterlockedExchange64(&impl_->view.value->nightCancelled, static_cast<LONG64>(impl_->nightCommand));
        if (impl_->requestEvent.value) SetEvent(impl_->requestEvent.value);
    }
    impl_->nightToken = impl_->nightRequestedTick = impl_->nightDeadline = 0;
    if (impl_->requestedKind == PixelRequest::Night) {
        impl_->request = 0; impl_->requestedKind = PixelRequest::Preview;
    }
}

bool CameraClient::nightResult(uint64_t token, Frame& output, NightWindowResult& result, std::wstring& error) {
    error.clear();
    if (!token || token != impl_->nightToken) { error = L"The camera night blend request was cancelled or replaced."; return false; }
    if (!impl_->process.value || !impl_->view.value) { error = L"The camera is not running."; return false; }
    auto read = [&](bool& waiting) {
        waiting = false;
        uint64_t now = GetTickCount64();
        SharedLock lock(impl_->mutex.value);
        if (!lock.acquired()) {
            if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0)
                error = L"The camera helper stopped unexpectedly. Reconnect the camera and try again.";
            else if (lock.result != WAIT_TIMEOUT) error = L"Windows could not read the camera night blend.";
            else if (now > impl_->nightDeadline) error = L"The camera night blend was not delivered within its deadline. Reconnect it and try again.";
            else if (!impl_->contended) { impl_->contended = true; impl_->contentionSince = now; }
            else if (now - impl_->contentionSince > (impl_->delivered ? 3000 : 8000))
                error = L"Camera frame sharing stayed unavailable. Reconnect the camera and try again.";
            return false;
        }
        // A completion can be published while this short mutex wait is in
        // progress. Validate its timestamps against time after acquisition.
        now = GetTickCount64();
        impl_->contended = false; impl_->contentionSince = 0;
        auto& shared = *impl_->view.value;
        if (!validHeader(shared) || shared.completed > shared.requested || impl_->request > shared.requested) {
            error = L"The camera helper returned invalid night blend data. Try again."; return false;
        }
        if (shared.state == HostState::Failed || shared.state == HostState::Stopped) {
            const wchar_t* end = static_cast<const wchar_t*>(std::wmemchr(shared.error, L'\0', std::size(shared.error)));
            error = end && end != shared.error ? std::wstring(shared.error, size_t(end - shared.error))
                : L"The camera helper stopped unexpectedly. Reconnect the camera and try again.";
            return false;
        }
        if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0) {
            error = L"The camera helper stopped unexpectedly. Reconnect the camera and try again."; return false;
        }
        const bool current = shared.nightStateCommand == impl_->nightCommand && shared.nightToken == token;
        if (current && shared.nightState == NightState::Failed) {
            const wchar_t* end = static_cast<const wchar_t*>(std::wmemchr(shared.nightError, L'\0', std::size(shared.nightError)));
            error = end && end != shared.nightError ? std::wstring(shared.nightError, size_t(end - shared.nightError))
                : L"The camera could not complete its night blend.";
            return false;
        }
        if (!current || shared.nightState != NightState::Complete) {
            if (current && shared.nightSourceTick > now) error = L"The camera helper returned invalid source freshness data.";
            else if (current && shared.nightSourceTick && now - shared.nightSourceTick > 3000)
                error = L"No fresh camera frame arrived for 3 seconds. Check the camera connection and try again.";
            else if ((!current || !shared.nightSourceTick) && now - impl_->nightRequestedTick > (impl_->delivered ? 3000 : 8000))
                error = L"The camera did not prepare a fresh source for the night blend within its deadline.";
            else if (now > impl_->nightDeadline) error = L"The camera night blend was not delivered within its deadline. Reconnect it and try again.";
            return false;
        }
        if (now > impl_->nightDeadline) {
            error = L"The camera night blend was not delivered within its deadline. Reconnect it and try again."; return false;
        }
        const auto& finished = shared.nightResult;
        if (!finished.exposure.samples || finished.exposure.samples > NightMaxSamples ||
            finished.beginTick < impl_->nightRequestedTick || finished.endTick > impl_->nightDeadline || finished.endTick > now ||
            finished.endTick - finished.beginTick != shared.nightDuration || shared.nightCompletedTick < finished.endTick ||
            shared.nightCompletedTick > now || shared.nightCompletedTick > impl_->nightDeadline ||
            finished.firstSampleTick < finished.beginTick || finished.lastSampleTick < finished.firstSampleTick || finished.lastSampleTick >= finished.endTick) {
            error = L"The camera helper returned an invalid night blend window. Try again."; return false;
        }
        // A prior preview response can be superseded; the reverse is prohibited
        // in latest(), so frequent preview requests cannot starve result delivery.
        if (!impl_->request || impl_->requestedKind != PixelRequest::Night) {
            impl_->request = ++shared.requested;
            shared.requestedKind = impl_->requestedKind = PixelRequest::Night;
            shared.requestedToken = token;
            impl_->requestedTick = now; impl_->requestedGeneration = shared.generation;
            if (!ResetEvent(impl_->responseEvent.value) || !SetEvent(impl_->requestEvent.value))
                return fail(error, L"Windows could not request the camera night result.");
        }
        if (shared.completed < impl_->request) {
            waiting = true;
            if (now - impl_->requestedTick > 3000) error = L"The camera did not answer the night result request for 3 seconds.";
            return false;
        }
        if (shared.completedKind != PixelRequest::Night || shared.completedToken != token ||
            shared.publishedRequest != impl_->request || shared.publishedKind != PixelRequest::Night || shared.publishedToken != token ||
            !shared.width || !shared.height || shared.width > maxWidth || shared.height > maxHeight ||
            uint64_t(shared.width) * shared.height * 4 != shared.bytes || !shared.generation || (shared.generation & 1) ||
            shared.generation <= impl_->requestedGeneration || shared.generation < impl_->generation) {
            error = L"The camera helper returned an invalid night result. Try again."; return false;
        }
        try {
            output.pixels.resize(shared.bytes);
            std::memcpy(output.pixels.data(), shared.pixels, shared.bytes);
            output.width = int(shared.width); output.height = int(shared.height);
            if (GetTickCount64() > impl_->nightDeadline) {
                error = L"The camera night blend was not delivered within its deadline. Reconnect it and try again."; return false;
            }
            result = finished; impl_->request = 0; impl_->requestedKind = PixelRequest::Preview;
            impl_->delivered = true; impl_->generation = shared.generation;
            return true;
        } catch (const std::bad_alloc&) { error = L"Not enough memory to read the camera night result."; return false; }
    };
    bool waiting = false;
    if (read(waiting)) return true;
    if (!waiting || !error.empty()) return false;
    const DWORD response = WaitForSingleObject(impl_->responseEvent.value, 10);
    if (response == WAIT_OBJECT_0 && read(waiting)) return true;
    if (response == WAIT_FAILED) return fail(error, L"Windows could not receive the camera night result.");
    return false;
}
void CameraClient::cancelActivityObservation() noexcept {
    if (impl_->view.value && impl_->observationLastRequest) {
        InterlockedExchange64(&impl_->view.value->observationCancelled, static_cast<LONG64>(impl_->observationLastRequest));
        if (impl_->requestEvent.value) SetEvent(impl_->requestEvent.value);
    }
    impl_->observationRequest = impl_->observationLastRequest = impl_->observationToken = impl_->observationDeadline = 0;
    impl_->observationEpoch = impl_->observationSequence = 0;
}

bool CameraClient::observeActivity(uint64_t token, CameraObservation& output, std::wstring& error) {
    return observe(token, &output, nullptr, error, true);
}
bool CameraClient::personInput(uint64_t token, CameraPersonInput& output, std::wstring& error, bool requestNew) {
    return observe(token, nullptr, &output, error, requestNew);
}
bool CameraClient::observe(uint64_t token, CameraObservation* activity, CameraPersonInput* personOutput, std::wstring& error, bool requestNew) {
    error.clear();
    if (!token) { error = L"The camera activity check needs a valid session."; return false; }
    if (!impl_->process.value || !impl_->view.value) { error = L"The camera is not running."; return false; }
    if (!requestNew && (!impl_->observationRequest || impl_->observationToken != token || impl_->observationKind != ObservationKind::Person)) return false;
    const ObservationKind kind = personOutput ? ObservationKind::Person : ObservationKind::Activity;
    if (impl_->observationToken != token || impl_->observationKind != kind) {
        cancelActivityObservation(); impl_->observationToken = token; impl_->observationKind = kind;
    }
    if (personOutput && !impl_->personView.value) {
        if (!impl_->personMapping.value) {
            impl_->personMapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                static_cast<DWORD>(person::MaxBgrBytes), (impl_->mappingName + L".person").c_str());
            if (!impl_->personMapping.value || GetLastError() == ERROR_ALREADY_EXISTS) {
                impl_->personMapping.reset();
                error = L"Windows could not reserve the optional person-check image."; return false;
            }
        }
        impl_->personView.reset(static_cast<uint8_t*>(MapViewOfFile(impl_->personMapping.value, FILE_MAP_READ, 0, 0, person::MaxBgrBytes)));
        if (!impl_->personView.value) { error = L"Windows could not access the optional person-check image."; return false; }
    }
    if (!impl_->observationDeadline)
        impl_->observationDeadline = GetTickCount64() + (impl_->delivered ? 3000 : 8000);
    auto read = [&](bool& waiting) {
        waiting = false;
        SharedLock lock(impl_->mutex.value, personOutput ? 0 : 10);
        const uint64_t now = GetTickCount64();
        if (now > impl_->observationDeadline) {
            error = L"The camera activity check was not delivered within its deadline."; return false;
        }
        if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0) {
            error = L"The camera helper stopped before its activity check."; return false;
        }
        if (!lock.acquired()) {
            if (lock.result != WAIT_TIMEOUT) error = L"Windows could not read the camera activity check.";
            return false;
        }
        auto& shared = *impl_->view.value;
        if (!validHeader(shared) || shared.observationCompleted > shared.observationRequested ||
            impl_->observationRequest > shared.observationRequested) {
            error = L"The camera helper returned invalid activity data."; return false;
        }
        if (shared.state == HostState::Failed || shared.state == HostState::Stopped) {
            error = L"The camera activity check is unavailable because the camera stopped."; return false;
        }
        if (!impl_->observationRequest) {
            if (shared.observationRequested >= uint64_t((std::numeric_limits<LONG64>::max)())) {
                error = L"The camera activity request limit was reached."; return false;
            }
            impl_->observationRequest = impl_->observationLastRequest = ++shared.observationRequested;
            shared.observationToken = token;
            shared.observationKind = kind;
            if (!SetEvent(impl_->requestEvent.value)) return fail(error, L"Windows could not request the camera activity check.");
            waiting = true; return false;
        }
        if (shared.observationRequested != impl_->observationRequest || shared.observationToken != token || shared.observationKind != kind) {
            error = L"The camera activity request was replaced."; return false;
        }
        if (shared.observationCompleted != impl_->observationRequest) { waiting = true; return false; }
        if (shared.observationCompletedToken != token || shared.observationCompletedKind != kind) {
            error = L"The camera activity response has an invalid session."; return false;
        }
        if (shared.observationState == ObservationState::Failed) {
            const wchar_t* end = static_cast<const wchar_t*>(std::wmemchr(shared.observationError, L'\0', std::size(shared.observationError)));
            error = end && end != shared.observationError ? std::wstring(shared.observationError, size_t(end - shared.observationError))
                : L"The camera activity check is unavailable.";
            return false;
        }
        if (shared.observationState == ObservationState::Duplicate || shared.observationState == ObservationState::Pending) {
            // Retry the SAME request on the caller's next cadence. A retained
            // sample/acknowledgement cannot extend the original deadline.
            if (!requestNew) return false;
            shared.observationCompleted = 0;
            if (!SetEvent(impl_->requestEvent.value)) return fail(error, L"Windows could not retry the camera activity check.");
            return false;
        }
        const auto& report = shared.observation;
        if (shared.observationState != ObservationState::Ready || shared.observationPublished != impl_->observationRequest ||
            shared.observationPublishedToken != token || shared.observationPublishedKind != kind || !report.epoch || !report.sequence || !report.receivedTick ||
            report.receivedTick > now || now - report.receivedTick > 3000 ||
            report.sourceWidth <= 0 || report.sourceWidth > int(maxWidth) || report.sourceHeight <= 0 || report.sourceHeight > int(maxHeight) ||
            (report.epoch == impl_->observationEpoch && report.sequence <= impl_->observationSequence)) {
            error = L"The camera activity response is invalid, repeated or stale."; return false;
        }
        if (personOutput) {
            const auto& source = shared.personSource;
            if (!person::validGeometry(source) || source.sessionToken != token || source.cameraEpoch != report.epoch ||
                source.sequence != report.sequence || source.receivedTick != report.receivedTick ||
                source.sourceWidth != uint32_t(report.sourceWidth) || source.sourceHeight != uint32_t(report.sourceHeight)) {
                error = L"The camera person-check image has invalid source data."; return false;
            }
            std::memcpy(personOutput->bgr.data(), impl_->personView.value, size_t(source.width) * source.height * 3);
            personOutput->source = source;
        } else *activity = report;
        impl_->observationEpoch = report.epoch; impl_->observationSequence = report.sequence;
        impl_->observationRequest = impl_->observationDeadline = 0;
        return true;
    };
    bool waiting = false;
    if (read(waiting)) return true;
    if (waiting && error.empty() && !personOutput) {
        // Shared response signals are only hints; provenance is checked under
        // the mutex, so a Preview/Night acknowledgement cannot supply this data.
        const DWORD response = WaitForSingleObject(impl_->responseEvent.value, 10);
        if (response == WAIT_OBJECT_0 && read(waiting)) return true;
        if (response == WAIT_FAILED) fail(error, L"Windows could not receive the camera activity check.");
    }
    if (!error.empty()) cancelActivityObservation();
    return false;
}

int runCameraHost(const wchar_t*) {
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return -1;
    int result = -1;
    if (count >= 2 && std::wcscmp(args[1], L"--camera-host") == 0) {
        try { result = count == 3 ? hostMain(args[2]) : 2; }
        catch (...) { result = 15; }
    }
    LocalFree(args);
    return result;
}
}

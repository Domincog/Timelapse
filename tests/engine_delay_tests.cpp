// Actual worker/CV and MP4 encoder, generated sources and intercepted power.
// No camera/display, system power change, optional process or app is launched.
#include "engine.h"
#include "encoder.h"
#include "camera_host.h"
#include "person_client.h"
#include <mfapi.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace delay_probe {
using namespace std::chrono_literals;
std::atomic<unsigned> captures{0}, previews{0}, cameraStarts{0}, nightStarts{0}, personStarts{0};
std::atomic<unsigned> opens{0}, writes{0}, queries{0}, loads{0}, frees{0}, diskQueries{0};
std::atomic<unsigned> renders{0}, gateRender{0};
std::atomic<unsigned> failQuery{0}, gateQuery{0}, gateOpen{0}, allocationFailures{0};
std::atomic<uint64_t> epoch{1}, firstOpen{0}, firstWrite{0}, firstNight{0};
std::atomic<bool> failBinding{false}, failDelete{false}, failWorkerAllocations{false};
std::atomic<bool> protocolOkay{true}, observedCursor{false};
std::atomic<DWORD> workerThread{0};
std::atomic<EXECUTION_STATE> power{ES_CONTINUOUS};
thread_local bool failCallerAllocation = false;
struct Gate {
    std::atomic<bool> armed{false}, reached{false}, released{false};
    void reset() { armed = reached = released = false; }
    void enter() {
        if (!armed.exchange(false)) return;
        reached = true;
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (!released) {
            if (std::chrono::steady_clock::now() >= end) ExitProcess(78);
            std::this_thread::sleep_for(1ms);
        }
    }
} captureGate, openGate, queryGate, renderGate;
struct Release { ~Release() { failWorkerAllocations = false; captureGate.released = openGate.released = queryGate.released = renderGate.released = true; } };
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class Predicate> void until(Predicate predicate, const char* text) {
    const auto end = std::chrono::steady_clock::now() + 6s;
    while (!predicate()) { if (std::chrono::steady_clock::now() >= end) throw std::runtime_error(text); std::this_thread::sleep_for(2ms); }
}
void reset() {
    captures = previews = cameraStarts = nightStarts = personStarts = 0;
    opens = writes = queries = loads = frees = diskQueries = 0;
    renders = gateRender = 0;
    failQuery = gateQuery = gateOpen = allocationFailures = 0;
    epoch = 1; firstOpen = firstWrite = firstNight = 0;
    failBinding = failDelete = failWorkerAllocations = false; protocolOkay = true; observedCursor = false;
    workerThread = 0; power = ES_CONTINUOUS; failCallerAllocation = false;
    captureGate.reset(); openGate.reset(); queryGate.reset(); renderGate.reset();
}
void pixels(lapse::Frame& frame, int width, int height) {
    frame.width = width; frame.height = height; frame.pixels.assign(size_t(width) * height * 4, 96);
}
}
void* operator new(size_t size) {
    if (delay_probe::failCallerAllocation || (delay_probe::failWorkerAllocations && GetCurrentThreadId() == delay_probe::workerThread)) {
        delay_probe::failCallerAllocation = false; ++delay_probe::allocationFailures; throw std::bad_alloc();
    }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
BOOL WINAPI delayFileInformation(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID value, DWORD bytes) {
    if (kind == FileDispositionInfo && delay_probe::failDelete) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return SetFileInformationByHandle(file, kind, value, bytes);
}
#define SetFileInformationByHandle delayFileInformation
#include "../src/encoder.cpp"
#undef SetFileInformationByHandle

namespace lapse {
class DelayEncoder {
    Encoder real_;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error, EncodingQuality quality, EncodingMode mode, bool recovery, const EncodingOptions& options = {}) {
        const bool okay = real_.open(path, width, height, fps, error, quality, mode, recovery, options);
        if (okay) {
            const auto number = ++delay_probe::opens;
            uint64_t empty = 0; delay_probe::firstOpen.compare_exchange_strong(empty, GetTickCount64());
            if (number == delay_probe::gateOpen) delay_probe::openGate.enter();
        }
        return okay;
    }
    bool write(const Frame& frame, std::wstring& error) {
        ++delay_probe::writes; uint64_t empty = 0; delay_probe::firstWrite.compare_exchange_strong(empty, GetTickCount64());
        return real_.write(frame, error);
    }
    bool finish(std::wstring& error) { return real_.finish(error); }
    bool finishForPublication(std::wstring& error) { return real_.finishForPublication(error); }
    bool emptyOutputDiscarded() const noexcept { return real_.emptyOutputDiscarded(); }
    DWORD publish(const std::wstring& path) { return real_.publish(path); }
    void releasePublication() noexcept { real_.releasePublication(); }
    uint64_t frames() const { return real_.frames(); }
};
class DelayWatermarkRenderer {
    WatermarkRenderer real_;
public:
    bool prepare(const WatermarkSettings& settings, int width, int height, std::wstring& error) { return real_.prepare(settings, width, height, error); }
    bool apply(Frame& frame, const WatermarkContext& context, std::wstring& error) {
        const bool okay = real_.apply(frame, context, error);
        if (++delay_probe::renders == delay_probe::gateRender) delay_probe::renderGate.enter();
        return okay;
    }
    void reset() noexcept { real_.reset(); }
};
}
LONG WINAPI delayPowerQuery(POWER_INFORMATION_LEVEL level, PVOID input, ULONG inputBytes, PVOID output, ULONG outputBytes) {
    using namespace delay_probe;
    protocolOkay = protocolOkay && level == LastWakeTime && !input && !inputBytes && output && outputBytes == sizeof(uint64_t);
    const auto number = ++queries;
    if (number == gateQuery) queryGate.enter();
    if (number == failQuery) return LONG(0xc0000001UL);
    *static_cast<uint64_t*>(output) = epoch;
    return 0;
}
HMODULE WINAPI delayLoadLibrary(LPCWSTR name, HANDLE file, DWORD flags) {
    ++delay_probe::loads;
    delay_probe::protocolOkay = delay_probe::protocolOkay && name && std::wcscmp(name, L"powrprof.dll") == 0 && !file && flags == LOAD_LIBRARY_SEARCH_SYSTEM32;
    return delay_probe::failBinding ? nullptr : reinterpret_cast<HMODULE>(uintptr_t(1));
}
FARPROC WINAPI delayGetProcAddress(HMODULE module, LPCSTR name) {
    delay_probe::protocolOkay = delay_probe::protocolOkay && module == reinterpret_cast<HMODULE>(uintptr_t(1)) && std::strcmp(name, "CallNtPowerInformation") == 0;
    auto query = &delayPowerQuery; FARPROC result{};
    static_assert(sizeof(result) == sizeof(query)); std::memcpy(&result, &query, sizeof(result)); return result;
}
BOOL WINAPI delayFreeLibrary(HMODULE module) {
    delay_probe::protocolOkay = delay_probe::protocolOkay && module == reinterpret_cast<HMODULE>(uintptr_t(1)); ++delay_probe::frees; return TRUE;
}
BOOL WINAPI delayDisk(LPCWSTR, PULARGE_INTEGER available, PULARGE_INTEGER, PULARGE_INTEGER) {
    ++delay_probe::diskQueries; available->QuadPart = 1024ULL * 1024 * 1024; return TRUE;
}
EXECUTION_STATE WINAPI delayPower(EXECUTION_STATE value) { delay_probe::power = value; return ES_CONTINUOUS; }
#define Encoder DelayEncoder
#define WatermarkRenderer DelayWatermarkRenderer
#define LoadLibraryExW delayLoadLibrary
#define GetProcAddress delayGetProcAddress
#define FreeLibrary delayFreeLibrary
#define GetDiskFreeSpaceExW delayDisk
#define SetThreadExecutionState delayPower
#include "../src/engine.cpp"
#undef SetThreadExecutionState
#undef GetDiskFreeSpaceExW
#undef FreeLibrary
#undef GetProcAddress
#undef LoadLibraryExW
#undef Encoder
#undef WatermarkRenderer
#include "engine_segment_decode.h"

namespace lapse {
struct CameraClient::Impl { bool running = false; uint64_t token = 0, begin = 0, end = 0; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring& id, std::wstring& error, CameraResolution) {
    ++delay_probe::cameraStarts; error.clear(); impl_->running = id == L"camera"; return impl_->running;
}
void CameraClient::stop() { impl_->running = false; }
bool CameraClient::latest(Frame& output, std::wstring& error) { error.clear(); if (!impl_->running) return false; delay_probe::pixels(output, 320, 240); return true; }
bool CameraClient::beginNight(uint64_t token, uint32_t duration, const NightSettings&, std::wstring& error) {
    error.clear(); ++delay_probe::nightStarts; impl_->token = token; impl_->begin = GetTickCount64(); impl_->end = impl_->begin + duration;
    uint64_t empty = 0; delay_probe::firstNight.compare_exchange_strong(empty, impl_->begin); return true;
}
bool CameraClient::nightResult(uint64_t token, Frame& output, NightWindowResult& result, std::wstring& error) {
    error.clear(); if (token != impl_->token || GetTickCount64() < impl_->end) return false;
    delay_probe::pixels(output, 320, 240); result.beginTick = result.firstSampleTick = impl_->begin;
    result.endTick = result.lastSampleTick = impl_->end; result.exposure.samples = 5; return true;
}
void CameraClient::cancelNight() noexcept { impl_->token = 0; }
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) { error.clear(); return false; }
bool CameraClient::personInput(uint64_t, CameraPersonInput&, std::wstring& error, bool) { error.clear(); return false; }
void CameraClient::cancelActivityObservation() noexcept {}
bool captureMonitor(const std::wstring& id, int width, int height, bool cursor, Frame& output, std::wstring& error) {
    delay_probe::workerThread = GetCurrentThreadId(); error.clear();
    if (id != L"display") { error = L"Synthetic display unavailable."; return false; }
    if (width == 320 && height == 240) { ++delay_probe::captures; delay_probe::observedCursor = cursor; }
    else ++delay_probe::previews;
    delay_probe::pixels(output, width, height); delay_probe::captureGate.enter(); return true;
}
void releaseDesktopCaptureCache() noexcept {}
struct PersonClient::Impl {};
PersonClient::PersonClient() : impl_(std::make_unique<Impl>()) {}
PersonClient::~PersonClient() = default;
bool PersonClient::start(uint64_t, std::wstring& error) { ++delay_probe::personStarts; error = L"Synthetic detector unavailable."; return false; }
void PersonClient::cancel() noexcept {}
PersonPoll PersonClient::poll(PersonCheckResult&) noexcept { return PersonPoll::Unavailable; }
bool PersonClient::submit(const CameraPersonInput&) noexcept { return false; }
const wchar_t* PersonClient::diagnostic() const noexcept { return L"Synthetic detector unavailable."; }
}
namespace delay_probe {
lapse::Settings config(const std::filesystem::path& path) {
    lapse::Settings value; value.monitorId = L"display"; value.cameraId = L"camera"; value.width = 320; value.height = 240;
    value.folder = path.wstring(); value.preview = false; value.intervalMs = 1000; value.startDelaySeconds = 1;
    value.recordingLimitSeconds = 1; return value;
}
template<class Predicate> lapse::Status await(lapse::Engine& engine, Predicate predicate) {
    lapse::Status result; until([&] { result = engine.status(); return predicate(result); }, "Engine status timed out"); return result;
}
lapse::Status idle(lapse::Engine& engine) { return await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }); }
void noOutput(const lapse::Status& status, bool error = false) {
    require(status.state == lapse::State::Idle && status.error == error && status.recordingFailed == error && !status.frames && status.elapsed == 0 &&
        status.savedPath.empty() && status.savedPaths.empty() && !status.completedSegments && !status.startDeadlineTick, "Cancelled/rejected attempt retained session output");
}
size_t files(const std::filesystem::path& path) {
    size_t result = 0; if (std::filesystem::exists(path)) for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) if (entry.is_regular_file()) ++result;
    return result;
}
void success(const std::filesystem::path& root, bool paired, bool night) {
    reset(); lapse::Engine engine; auto cfg = config(root / (night ? L"night" : paired ? L"paired" : L"single"));
    cfg.separateFiles = paired;
    if (night) { cfg.layers = lapse::preset(lapse::Mode::Camera); cfg.night.enabled = true; cfg.night.durationMs = 1000; }
    engine.configure(cfg); engine.record(); const auto waiting = engine.status();
    require(waiting.state == lapse::State::Waiting && waiting.startDeadlineTick > GetTickCount64(), "Record did not arm authoritative deadline");
    std::this_thread::sleep_for(100ms);
    require(!captures && !cameraStarts && !nightStarts && !personStarts && !opens && !writes && !diskQueries && !std::filesystem::exists(cfg.folder), "Waiting performed required source/output work");
    require(power == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED), "Waiting did not hold system-only power");
    const auto saved = idle(engine);
    require(!saved.error && saved.frames == 1 && saved.savedPaths.size() == (paired ? 2u : 1u) && saved.elapsed >= 1 && saved.elapsed < 1.5, "Delay consumed active limit or broke paired result");
    require(firstOpen >= waiting.startDeadlineTick && queries == 3 && loads == frees && protocolOkay, "Promotion/admission query or no-earlier deadline failed");
    for (const auto& path : saved.savedPaths) require(split_test::decodedFrames(path, 1) == 1, "Delayed output failed real decoding");
    if (night) require(firstNight >= waiting.startDeadlineTick && firstWrite >= firstNight + 1000, "Night was integrated before countdown or shortened");
    require(power == ES_CONTINUOUS, "Terminal delayed recording retained power request");
}
void cancelWaiting(const std::filesystem::path& root, bool suspend) {
    reset(); lapse::Engine engine; auto cfg = config(root / (suspend ? L"suspend-wait" : L"finish-wait")); cfg.startDelaySeconds = 5;
    cfg.layers = lapse::preset(lapse::Mode::Camera); cfg.night.enabled = true; cfg.timeSkip.mode = lapse::TimeSkipMode::NoPerson;
    engine.configure(cfg); engine.record(); await(engine, [](const auto& s) { return s.state == lapse::State::Waiting; });
    if (suspend) engine.cancelDelayedStart(); else engine.finish();
    noOutput(idle(engine)); require(!opens && !writes && !cameraStarts && !nightStarts && !personStarts && !diskQueries && !files(cfg.folder), "Cancelled timer performed recording work");
    engine.cancelDelayedStart(); engine.finish(); noOutput(engine.status());
}
void cancelPreparing(const std::filesystem::path& root, unsigned openNumber, bool cleanupFailure) {
    reset(); lapse::Engine engine; Release release; auto cfg = config(root / (L"cancel-open-" + std::to_wstring(openNumber) + (cleanupFailure ? L"-fault" : L"")));
    cfg.separateFiles = openNumber > 0; gateOpen = openNumber ? openNumber : 1; openGate.armed = true;
    engine.configure(cfg); engine.record(); until([] { return openGate.reached.load(); }, "First open was not reached");
    failDelete = cleanupFailure;
    if (openNumber) engine.cancelDelayedStart(); else engine.finish();
    openGate.released = true;
    const auto stopped = idle(engine); noOutput(stopped, cleanupFailure);
    require(!writes && opens == (openNumber ? openNumber : 1), "Cancel allowed second open or paired first write");
    require(files(cfg.folder) == (cleanupFailure ? size_t(opens.load()) : 0u), "Empty disposal changed owned-file result");
    if (cleanupFailure) require(stopped.message.find(L"cleanup") != std::wstring::npos && stopped.message.find(L".recording.mp4") != std::wstring::npos, "Cleanup failure lost retained empty filename");
    failDelete = false;
}
void powerCheckpoint(const std::filesystem::path& root, unsigned checkpoint, bool failure, bool cancelBlocked = false) {
    reset(); lapse::Engine engine; Release release; auto cfg = config(root / (L"power-" + std::to_wstring(checkpoint) + (failure ? L"-error" : L"-changed") + (cancelBlocked ? L"-cancel" : L"")));
    if (failure) failQuery = checkpoint;
    if (checkpoint == 3) { gateOpen = 1; openGate.armed = true; }
    if (cancelBlocked) { gateQuery = checkpoint; queryGate.armed = true; }
    engine.configure(cfg); engine.record();
    if (checkpoint == 3) { until([] { return openGate.reached.load(); }, "Power check preparation gate missing"); if (!failure) ++epoch; openGate.released = true; }
    else if (!failure) ++epoch;
    if (cancelBlocked) { until([] { return queryGate.reached.load(); }, "Power query gate missing"); engine.cancelDelayedStart(); require(engine.status().state != lapse::State::Idle, "Status could not be sampled while query blocked"); queryGate.released = true; }
    const auto stopped = idle(engine); noOutput(stopped, failure && !cancelBlocked);
    require(!writes && !files(cfg.folder) && queries == checkpoint && loads == frees && protocolOkay, "Wake check failed closed or leaked binding");
}
void frozenAndOff(const std::filesystem::path& root) {
    reset(); lapse::Engine engine; auto cfg = config(root / L"frozen"); cfg.captureCursor = false;
    engine.configure(cfg); engine.record(); auto next = cfg;
    next.folder = (root / L"next").wstring(); next.width = 640; next.height = 480; next.layers = lapse::preset(lapse::Mode::Camera);
    next.cameraId = L"unavailable"; next.captureCursor = true; next.recordingLimitSeconds = 0; next.startDelaySeconds = 0;
    engine.configure(next); const auto saved = idle(engine);
    require(!saved.error && saved.frames == 1 && !observedCursor && !cameraStarts && !std::filesystem::exists(next.folder), "Waiting configure changed accepted recording");
    require(split_test::decodedFrames(saved.savedPath, 1) == 1, "Frozen output was invalid");
    cfg.startDelaySeconds = 0; cfg.folder = (root / L"off").wstring(); engine.configure(cfg);
    const auto before = queries.load(); engine.record(); engine.cancelDelayedStart(); const auto direct = idle(engine);
    require(!direct.error && direct.frames == 1 && queries == before, "Off gained native queries or delayed cancellation");
}
void allocationCancellation(const std::filesystem::path& root) {
    reset(); lapse::Engine engine; Release release; auto cfg = config(root / L"allocation"); cfg.preview = true; cfg.startDelaySeconds = 5;
    captureGate.armed = true; engine.configure(cfg); until([] { return captureGate.reached.load(); }, "Idle preview gate missing");
    engine.record(); cfg.preview = false; engine.configure(cfg); engine.finish(); failWorkerAllocations = true; captureGate.released = true;
    const auto stopped = idle(engine); noOutput(stopped);
    require(!opens && !writes && !files(cfg.folder) && !stopped.startDeadlineTick, "Allocation failure kept cancelled request armed");
    const auto failures = allocationFailures.load();
    std::this_thread::sleep_for(100ms); const auto settled = engine.status();
    noOutput(settled);
    require(settled.message == stopped.message && allocationFailures == failures && power == ES_CONTINUOUS,
        "Hidden cancelled timer retried snapshots or lost its benign result under persistent allocation failure");
    failWorkerAllocations = false;
    cfg.startDelaySeconds = 0; cfg.folder = (root / L"allocation-control").wstring(); engine.configure(cfg); engine.record(); const auto prior = idle(engine);
    require(!prior.error && prior.frames == 1, "Control recording failed");
    cfg.startDelaySeconds = 1; engine.configure(cfg); failCallerAllocation = true;
    bool threw = false; try { engine.record(); } catch (const std::bad_alloc&) { threw = true; }
    failCallerAllocation = false; const auto unchanged = engine.status();
    require(threw && unchanged.state == lapse::State::Idle && unchanged.frames == prior.frames && unchanged.message == prior.message && unchanged.savedPaths == prior.savedPaths, "Unaccepted allocation failure replaced previous outcome");
}
void emptyPredicate(const std::filesystem::path& root) {
    reset(); std::filesystem::create_directories(root / L"predicate");
    for (int fault = 0; fault < 2; ++fault) {
        lapse::Encoder encoder; std::wstring error;
        require(!encoder.emptyOutputDiscarded(), "Unused encoder claimed terminal cleanup");
        const auto path = root / L"predicate" / (fault ? L"fault.mp4" : L"clean.mp4");
        require(encoder.open(path.wstring(), 320, 240, 30, error), "Empty predicate open failed");
        require(!encoder.emptyOutputDiscarded(), "Open encoder claimed terminal cleanup");
        failDelete = fault != 0; require(!encoder.finish(error), "Empty writer unexpectedly reported video success");
        require(encoder.emptyOutputDiscarded() == (fault == 0) && std::filesystem::exists(path) == (fault != 0), "Typed empty cleanup contradicted owned file");
        failDelete = false;
    }
}
void renderCancellation(const std::filesystem::path& root, bool resumed) {
    reset(); lapse::Engine engine; Release release; auto cfg = config(root / (resumed ? L"render-resume" : L"render-cancel"));
    cfg.separateFiles = true; cfg.watermark.enabled = true; gateRender = 2; renderGate.armed = true;
    engine.configure(cfg); engine.record(); until([] { return renderGate.reached.load(); }, "Paired watermark preparation gate missing");
    require(engine.status().state == lapse::State::Starting && queries == 2 && !writes,
        "Delayed admission happened before both watermark overlays");
    if (resumed) ++epoch; else engine.cancelDelayedStart();
    renderGate.released = true; noOutput(idle(engine));
    require(!writes && !files(cfg.folder) && queries == (resumed ? 3u : 2u), "Suspend during rendering admitted a paired frame");
}
void rejectionAndDestruction(const std::filesystem::path& root) {
    reset(); auto cfg = config(root / L"rejected");
    { lapse::Engine engine;
      for (int delay : {-1, 301}) { cfg.startDelaySeconds = delay; engine.configure(cfg); engine.record(); noOutput(engine.status(), true); }
      require(!loads && !opens, "Invalid delay touched native query or encoder");
      cfg.startDelaySeconds = 1; failBinding = true; engine.configure(cfg); engine.record(); noOutput(engine.status(), true);
      require(loads == 1 && !queries && !opens, "Missing power binding did not fail closed"); }
    reset();
    { lapse::Engine engine; cfg.startDelaySeconds = 300; engine.configure(cfg); engine.record(); }
    require(!opens && !writes && !captures && !cameraStarts && !files(cfg.folder) && power == ES_CONTINUOUS,
        "Destruction allowed an armed timer to run or retained power");
}
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-delay-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        delay_probe::success(root, false, false); delay_probe::success(root, true, false); delay_probe::success(root, false, true);
        delay_probe::cancelWaiting(root, false); delay_probe::cancelWaiting(root, true);
        delay_probe::cancelPreparing(root, 0, false); delay_probe::cancelPreparing(root, 1, false); delay_probe::cancelPreparing(root, 2, false); delay_probe::cancelPreparing(root, 1, true);
        delay_probe::powerCheckpoint(root, 1, true); delay_probe::powerCheckpoint(root, 2, true); delay_probe::powerCheckpoint(root, 3, true);
        delay_probe::powerCheckpoint(root, 2, false); delay_probe::powerCheckpoint(root, 3, false); delay_probe::powerCheckpoint(root, 3, true, true);
        delay_probe::frozenAndOff(root); delay_probe::allocationCancellation(root); delay_probe::emptyPredicate(root);
        delay_probe::renderCancellation(root, false); delay_probe::renderCancellation(root, true); delay_probe::rejectionAndDestruction(root);
        std::filesystem::remove_all(root); std::cout << "Delayed recording: 21 synthetic case groups passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::wcerr << L"Evidence retained at " << root.wstring() << L'\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

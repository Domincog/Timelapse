// Real engine, compositor, and encoder with synthetic capture APIs. Each case
// blocks an old preview completion and the next capture so configuration races
// can be inspected without a camera, desktop access, or timing-dependent sleeps.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;
constexpr uint8_t oldMarker = 65, newMarker = 201;
enum class Completion { Frame, Error, Exception };
enum class Change { Source, Monitor, CameraId, RoundTrip, PreviewToggle, SameSource, Refresh, Record, CameraStart,
    CameraRoundTrip, CameraPreviewToggle, CameraStartRoundTrip, CameraStartToggle };

struct CaptureGate {
    std::mutex mutex;
    std::condition_variable changed;
    int calls = 0;
    bool releaseOld = false, releaseNew = false, timedOut = false;
    bool secondIsWarmup = false, blockCameraStart = false;
    Completion completion = Completion::Frame;
    std::wstring firstSource, secondSource;

    void reset(Completion result, bool warmup, bool startup) {
        std::lock_guard<std::mutex> lock(mutex);
        calls = 0; releaseOld = releaseNew = timedOut = false;
        completion = result; firstSource.clear(); secondSource.clear();
        secondIsWarmup = warmup; blockCameraStart = startup;
    }
    bool awaitCall(int count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return calls >= count; });
    }
    void release(bool all) {
        { std::lock_guard<std::mutex> lock(mutex);
          releaseOld = true; if (all) releaseNew = true; }
        changed.notify_all();
    }
    bool capture(const std::wstring& source, lapse::Frame& frame, std::wstring& error) {
        std::unique_lock<std::mutex> lock(mutex);
        const int ordinal = ++calls;
        if (ordinal == 1) firstSource = source;
        if (ordinal == 2) secondSource = source;
        changed.notify_all();
        if (ordinal <= 2 && !changed.wait_for(lock, 5s, [&] {
                return ordinal == 1 ? releaseOld : releaseNew;
            })) {
            timedOut = true; error = L"Synthetic preview barrier timed out."; return false;
        }
        const auto result = completion;
        const bool warmup = secondIsWarmup && ordinal == 2;
        lock.unlock();
        if (ordinal == 1) {
            if (result == Completion::Exception) throw std::bad_alloc();
            if (result == Completion::Error) { error = L"Old source failed after reconfiguration."; return false; }
        }
        // A pending recording is cancelled before this warmup result is
        // released, so the test never reaches folder creation or the encoder.
        if (warmup) { error.clear(); return false; }
        frame.width = 64; frame.height = 36;
        frame.pixels.assign(size_t(frame.width) * frame.height * 4,
                            ordinal == 1 ? oldMarker : newMarker);
        error.clear(); return true;
    }
} gate;

struct ReleaseBarriers {
    ~ReleaseBarriers() { gate.release(true); }
};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool hasMarker(const lapse::Status& status, uint8_t marker) {
    return status.preview && status.preview->valid() && status.preview->pixels[0] == marker;
}
bool awaitHealthyPreview(lapse::Engine& engine) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = engine.status();
        if (hasMarker(status, newMarker) && !status.error) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
const char* name(Change change) {
    switch (change) {
    case Change::Source: return "source switch";
    case Change::Monitor: return "monitor switch";
    case Change::CameraId: return "camera ID switch";
    case Change::RoundTrip: return "A to B to A";
    case Change::PreviewToggle: return "preview off then on";
    case Change::SameSource: return "same-source control";
    case Change::Refresh: return "source refresh";
    case Change::Record: return "accepted Record";
    case Change::CameraStart: return "camera startup source switch";
    case Change::CameraRoundTrip: return "camera to desktop to camera";
    case Change::CameraPreviewToggle: return "camera preview off then on";
    case Change::CameraStartRoundTrip: return "camera startup round trip";
    case Change::CameraStartToggle: return "camera startup preview off then on";
    }
    return "unknown change";
}
const char* name(Completion completion) {
    switch (completion) {
    case Completion::Frame: return "old frame";
    case Completion::Error: return "old error";
    case Completion::Exception: return "old exception";
    }
    return "unknown completion";
}

void runCase(Change change, Completion completion) {
    const bool startup = change == Change::CameraStart || change == Change::CameraStartRoundTrip ||
        change == Change::CameraStartToggle;
    gate.reset(completion, change == Change::Record, startup);
    lapse::Settings original;
    original.monitor = {0, 0, 640, 360};
    original.monitorId = L"synthetic-display-0";
    original.cameraId = L"old-camera";
    const bool camera = change == Change::CameraId || change == Change::Refresh ||
        change == Change::Record || startup || change == Change::CameraRoundTrip || change == Change::CameraPreviewToggle;
    original.layers = lapse::preset(camera ? lapse::Mode::Camera : lapse::Mode::Desktop);
    lapse::Engine engine;
    ReleaseBarriers releaseOnExit; // Releases gates before engine joins worker.
    engine.configure(original);
    require(gate.awaitCall(1), "Old capture did not reach its barrier.");

    auto updated = original;
    if (change == Change::Source || change == Change::RoundTrip)
        updated.layers = lapse::preset(lapse::Mode::Camera);
    if (change == Change::CameraRoundTrip || change == Change::CameraStartRoundTrip)
        updated.layers = lapse::preset(lapse::Mode::Desktop);
    if (change == Change::Monitor) {
        updated.monitor = {640, 0, 1280, 360};
        updated.monitorId = L"synthetic-display-640";
    }
    if (change == Change::CameraId) updated.cameraId = L"new-camera";
    if (change == Change::CameraStart) updated.layers = lapse::preset(lapse::Mode::Desktop);
    if (change == Change::PreviewToggle || change == Change::CameraPreviewToggle || change == Change::CameraStartToggle)
        updated.preview = false;
    engine.configure(updated);
    if (change == Change::RoundTrip || change == Change::PreviewToggle || change == Change::CameraRoundTrip ||
        change == Change::CameraPreviewToggle || change == Change::CameraStartRoundTrip || change == Change::CameraStartToggle)
        engine.configure(original);
    if (change == Change::Refresh) engine.refreshSources();
    if (change == Change::Record) engine.record();
    const auto afterConfigure = engine.status();
    require(!afterConfigure.preview && !afterConfigure.error, "Unexpected status before releasing old capture.");
    if (change == Change::Refresh)
        require(afterConfigure.message == L"Ready to record.", "Refresh did not publish its current message.");
    if (change == Change::Record)
        require(afterConfigure.state == lapse::State::Starting && afterConfigure.message == L"Preparing recording...",
                "Record was not accepted before old capture completion.");

    gate.release(false);
    // The worker reaches this barrier only after handling the previous result.
    // No new frame or error can replace the observed state until we release it.
    require(gate.awaitCall(2), "Next capture did not reach its barrier.");
    const auto between = engine.status();
    const std::wstring first = startup ? L"start:old-camera"
        : camera ? L"camera:old-camera" : L"desktop:0";
    const std::wstring second = change == Change::Source ? L"camera:old-camera"
        : change == Change::Monitor ? L"desktop:640"
        : change == Change::CameraId ? L"camera:new-camera"
        : change == Change::Refresh || change == Change::Record || change == Change::CameraRoundTrip ||
          change == Change::CameraPreviewToggle ? L"camera:old-camera"
        : change == Change::CameraStartRoundTrip || change == Change::CameraStartToggle ? L"start:old-camera" : L"desktop:0";
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        require(!gate.timedOut, "A capture barrier timed out.");
        require(gate.firstSource == first && gate.secondSource == second,
                "The capture configurations do not match this test case.");
    }
    const bool accepted = change == Change::SameSource;
    const bool previewCorrect = accepted ? hasMarker(between, oldMarker) : !between.preview;
    const auto expectedState = change == Change::Record ? lapse::State::Starting : lapse::State::Idle;
    const bool statusCorrect = !between.error && between.message == afterConfigure.message &&
        between.state == expectedState && between.frames == 0 && between.savedPath.empty();
    if (change == Change::Record) engine.finish();
    gate.release(true);
    require(awaitHealthyPreview(engine), "The next healthy preview did not appear.");
    const auto healthy = engine.status();
    require(healthy.state == lapse::State::Idle && healthy.frames == 0 && healthy.savedPath.empty(),
            "The preview test unexpectedly recorded a frame or remained active.");
    require(previewCorrect, accepted ? "A valid same-source frame was discarded." : "An invalidated frame was republished.");
    require(statusCorrect, "An invalidated preview error or exception changed current status.");
}
}

namespace retryCases {
using namespace std::chrono_literals;
// Set before Engine construction and reset only after its worker is joined.
bool enabled = false;
struct Mode { Mode() { enabled = true; } ~Mode() { enabled = false; } };
constexpr uint8_t cameraMarker = 137, desktopMarker = 83;
constexpr const wchar_t* startError = L"Synthetic current camera start failure.";
constexpr const wchar_t* readError = L"Synthetic current camera read failure.";
enum class Operation { Open, Read };

struct Controls {
    struct Gate { bool blocked = false, entered = false, fail = false; } openGate, readGate;
    struct Counts { int opens, reads, closes; } counts{};
    std::mutex mutex;
    std::condition_variable changed;
    bool timedOut = false;

    Gate& gate(Operation operation) { return operation == Operation::Open ? openGate : readGate; }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        openGate = {}; readGate = {}; counts = {}; timedOut = false;
    }
    void arm(Operation operation, bool fail) {
        std::lock_guard<std::mutex> lock(mutex);
        gate(operation) = {true, false, fail};
    }
    bool awaitEntry(Operation operation) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return gate(operation).entered; });
    }
    void release(Operation operation) {
        { std::lock_guard<std::mutex> lock(mutex); gate(operation).blocked = false; }
        changed.notify_all();
    }
    void releaseAll() {
        { std::lock_guard<std::mutex> lock(mutex); openGate.blocked = readGate.blocked = false; }
        changed.notify_all();
    }
    bool enter(Operation operation) {
        std::unique_lock<std::mutex> lock(mutex);
        if (operation == Operation::Open) ++counts.opens;
        else ++counts.reads;
        auto& selected = gate(operation);
        const bool fail = selected.blocked && selected.fail;
        const bool wait = selected.blocked;
        if (wait) selected.entered = true;
        changed.notify_all();
        if (wait && !changed.wait_for(lock, 5s, [&] { return !selected.blocked; })) {
            timedOut = true;
            throw std::runtime_error("Synthetic camera control gate timed out.");
        }
        return fail;
    }
    void closed() {
        { std::lock_guard<std::mutex> lock(mutex); ++counts.closes; }
        changed.notify_all();
    }
    Counts snapshot() { std::lock_guard<std::mutex> lock(mutex); return counts; }
    bool noNewOpen(int expected) {
        std::unique_lock<std::mutex> lock(mutex);
        // Open decisions run before the 500ms preview cadence, on 50ms worker
        // ticks. Wait on the open event across several ticks without a sleep.
        return !changed.wait_for(lock, 350ms, [&] { return counts.opens != expected || timedOut; });
    }
    bool healthyGates() { std::lock_guard<std::mutex> lock(mutex); return !timedOut; }
} controls;

struct ReleaseGates { ~ReleaseGates() { controls.releaseAll(); } };
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> bool await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}
void frame(lapse::Frame& output, uint8_t marker) {
    output.width = 64; output.height = 36;
    output.pixels.assign(size_t(output.width) * output.height * 4, marker);
}
bool cameraPreview(const lapse::Status& status) {
    return status.preview && status.preview->valid() && status.preview->pixels[0] == cameraMarker;
}
lapse::Settings cameraSettings(bool preview = true) {
    lapse::Settings settings;
    settings.cameraId = L"synthetic-camera-A";
    settings.monitor = {0, 0, 640, 360};
    settings.monitorId = L"synthetic-display-0";
    settings.layers = lapse::preset(lapse::Mode::Camera);
    settings.preview = preview;
    // Deliberately empty: even an unexpected successful pending frame cannot
    // create a recording. Every Record test fails before reaching this check.
    settings.folder.clear();
    return settings;
}

void healthyReaderThenCurrentFailure() {
    Mode mode;
    controls.reset();
    lapse::Engine engine;
    ReleaseGates release; // Unblocks operations before the engine destructor joins.
    const auto initial = cameraSettings();
    engine.configure(initial);
    require(await([&] { const auto s = engine.status(); return cameraPreview(s) && !s.error; }),
            "Initial camera preview did not appear.");
    controls.arm(Operation::Read, false);
    require(controls.awaitEntry(Operation::Read), "Healthy camera read did not reach its gate.");
    const auto before = controls.snapshot();
    auto different = initial; different.cameraId = L"synthetic-camera-B";
    engine.configure(different); engine.configure(initial);
    require(!engine.status().preview, "Camera round trip did not retire the old image.");
    controls.release(Operation::Read);
    require(await([&] { const auto s = engine.status(); return cameraPreview(s) && !s.error; }),
            "Healthy camera did not publish in the new generation.");
    const auto healthy = controls.snapshot();
    require(healthy.opens == before.opens && healthy.closes == before.closes,
            "A healthy reader restarted merely because its preview generation changed.");

    controls.arm(Operation::Read, true);
    require(controls.awaitEntry(Operation::Read), "Current-generation read failure did not reach its gate.");
    controls.release(Operation::Read);
    require(await([&] { const auto s = engine.status(); return s.error && !s.preview &&
            s.message.find(readError) != std::wstring::npos; }), "Current read failure was not reported.");
    auto geometryOnly = initial; geometryOnly.layers[0].rect = {.1, .1, .8, .8}; geometryOnly.interval = 7;
    engine.configure(geometryOnly); engine.configure(initial);
    require(controls.noNewOpen(before.opens), "Current-generation failure reopened without an explicit retry.");
    require(controls.snapshot().closes == before.closes + 1 && engine.status().error,
            "Current failed reader was not closed and left latched.");
    engine.refreshSources();
    require(await([&] { const auto s = engine.status(); return cameraPreview(s) && !s.error; }),
            "Refresh did not recover the latched read failure.");
    require(controls.snapshot().opens == before.opens + 1 && controls.healthyGates(),
            "Read-failure recovery reopened more than once or timed out.");
}

void currentStartFailureNeedsRefresh() {
    Mode mode;
    controls.reset(); controls.arm(Operation::Open, true);
    lapse::Engine engine;
    ReleaseGates release;
    const auto initial = cameraSettings();
    engine.configure(initial);
    require(controls.awaitEntry(Operation::Open), "Current start failure did not reach its gate.");
    controls.release(Operation::Open);
    require(await([&] { const auto s = engine.status(); return s.error && !s.preview &&
            s.message.find(startError) != std::wstring::npos; }), "Current start failure was not reported.");
    auto geometryOnly = initial; geometryOnly.layers[0].rect = {.1, .1, .8, .8};
    engine.configure(geometryOnly); engine.configure(initial);
    require(controls.noNewOpen(1), "Same-generation start failure reopened without Refresh.");
    require(controls.snapshot().reads == 0 && engine.status().state == lapse::State::Idle,
            "Failed startup attempted a read or changed the recording lifecycle.");
    engine.refreshSources();
    require(await([&] { const auto s = engine.status(); return cameraPreview(s) && !s.error; }),
            "Refresh did not recover the current start failure.");
    require(controls.snapshot().opens == 2 && controls.healthyGates(),
            "Start-failure recovery reopened more than once or timed out.");
}

void pendingRecordingFailureStaysTerminal() {
    Mode mode;
    controls.reset(); controls.arm(Operation::Read, true);
    lapse::Engine engine;
    ReleaseGates release;
    auto initial = cameraSettings(false);
    engine.configure(initial); engine.record();
    require(controls.awaitEntry(Operation::Read), "Pending recording did not reach the failing read.");
    require(engine.status().state == lapse::State::Starting, "Pending recording was not in Starting.");
    // Retire this preview generation while the genuine due capture is blocked.
    // Its recording failure must remain terminal even though its preview is old.
    engine.configure(initial);
    controls.release(Operation::Read);
    require(await([&] { const auto s = engine.status(); return s.state == lapse::State::Idle && s.error; }),
            "Pending capture failure did not terminate the recording attempt.");
    const auto failed = engine.status();
    require(failed.message == std::wstring(L"Capture stopped: ") + readError && failed.frames == 0 &&
            failed.savedPath.empty() && !failed.preview,
            "Genuine capture failure was suppressed or reached recording output.");

    // Native configure(preview=false) retires this generation even though the
    // preview was already disabled. Keep the same camera id throughout so the
    // failed-attempt generation marker is what permits the next open.
    engine.configure(initial);
    initial.preview = true;
    engine.configure(initial);
    require(await([&] { return cameraPreview(engine.status()); }),
            "Source invalidation did not allow a healthy preview after terminal failure.");
    const auto recovered = engine.status();
    require(recovered.state == lapse::State::Idle && recovered.error && recovered.message == failed.message &&
            recovered.frames == 0 && recovered.savedPath.empty() && controls.healthyGates(),
            "Healthy preview erased the terminal recording error or restarted recording.");
}
}

namespace lapse {
struct CameraClient::Impl { std::wstring id; bool controlledRunning = false; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() { if (retryCases::enabled) stop(); }
bool CameraClient::start(const std::wstring& id, std::wstring& error) {
    if (retryCases::enabled) {
        stop(); error.clear();
        if (retryCases::controls.enter(retryCases::Operation::Open)) { error = retryCases::startError; return false; }
        impl_->id = id; impl_->controlledRunning = true; return true;
    }
    impl_->id = id; error.clear();
    // Test configuration is immutable until this engine has been destroyed.
    if (gate.blockCameraStart) {
        Frame unused;
        return gate.capture(L"start:" + id, unused, error);
    }
    return true;
}
void CameraClient::stop() {
    if (retryCases::enabled && impl_->controlledRunning) {
        impl_->controlledRunning = false; retryCases::controls.closed();
    }
    impl_->id.clear();
}
bool CameraClient::latest(Frame& output, std::wstring& error) {
    if (retryCases::enabled) {
        error.clear();
        if (!impl_->controlledRunning) { error = L"Synthetic camera was closed."; return false; }
        if (retryCases::controls.enter(retryCases::Operation::Read)) { error = retryCases::readError; return false; }
        retryCases::frame(output, retryCases::cameraMarker); return true;
    }
    return gate.capture(L"camera:" + impl_->id, output, error);
}
bool captureMonitor(const std::wstring& id, int, int, bool, Frame& output, std::wstring& error) {
    // The worker can wake before the test's initial configure() under load.
    // Do not let its unconfigured default display consume either real barrier.
    if (id != L"synthetic-display-0" && id != L"synthetic-display-640") {
        error = L"The synthetic display has not been configured."; return false;
    }
    if (retryCases::enabled) { retryCases::frame(output, retryCases::desktopMarker); error.clear(); return true; }
    return gate.capture(id == L"synthetic-display-0" ? L"desktop:0" : L"desktop:640", output, error);
}
}

int main() {
    std::cout << std::unitbuf;
    int failures = 0, cases = 0;
    auto test = [&](Change change, Completion completion) {
        ++cases;
        try {
            runCase(change, completion);
            std::cout << "PASS " << name(change) << ": " << name(completion) << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name(change) << ": " << name(completion) << ": " << error.what() << '\n';
        }
    };
    for (auto change : {Change::Source, Change::Monitor, Change::CameraId, Change::RoundTrip, Change::PreviewToggle,
                       Change::Refresh, Change::Record})
        for (auto completion : {Completion::Frame, Completion::Error, Completion::Exception}) test(change, completion);
    test(Change::SameSource, Completion::Frame);
    test(Change::CameraStart, Completion::Error);
    test(Change::CameraStart, Completion::Exception);
    for (auto change : {Change::CameraRoundTrip, Change::CameraPreviewToggle, Change::CameraStartRoundTrip, Change::CameraStartToggle})
        test(change, Completion::Error);
    const std::pair<const char*, void(*)()> retryTests[] = {
        {"healthy reader survives invalidation and latches its later current failure", retryCases::healthyReaderThenCurrentFailure},
        {"same-generation startup failure stays latched until Refresh", retryCases::currentStartFailureNeedsRefresh},
        {"due recording failure stays terminal across in-flight source invalidation", retryCases::pendingRecordingFailureStaysTerminal}
    };
    for (const auto& retryTest : retryTests) {
        ++cases;
        try { retryTest.second(); std::cout << "PASS " << retryTest.first << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << retryTest.first << ": " << error.what() << '\n'; }
    }
    std::cout << "Engine preview publication: " << (cases - failures) << '/' << cases << " passed.\n";
    return failures ? 1 : 0;
}

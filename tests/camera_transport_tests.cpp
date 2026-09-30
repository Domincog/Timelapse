#include "camera_host.h"
#include "capture.h"
#include <objbase.h>
#include <shellapi.h>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool failFrameAllocation = false;
thread_local std::size_t failedFrameBytes = 0;
}
void* operator new(std::size_t bytes) {
    if (failFrameAllocation) {
        failFrameAllocation = false;
        failedFrameBytes = bytes;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
constexpr wchar_t controlEnvironment[] = L"TIMELAPSE_TEST_TRANSPORT_CONTROL";
struct TestHandle {
    HANDLE value = nullptr;
    ~TestHandle() { if (value) CloseHandle(value); }
    TestHandle() = default;
    TestHandle(const TestHandle&) = delete;
    TestHandle& operator=(const TestHandle&) = delete;
};
struct TestView {
    void* value = nullptr;
    ~TestView() { if (value) UnmapViewOfFile(value); }
};
struct TestControl { volatile LONG ready, latestCalls; DWORD processId; wchar_t transportName[128]; };
// Inspect only the header of this fixture's own mapping; actual transport code
// performs all publication and CameraClient consumption.
// Protocol 5 adds person observation data after the existing request/pixel
// metadata. This fixed prefix through completed retains its original layout.
struct SharedHeader {
    uint32_t magic, version, state, width, height, bytes;
    uint64_t generation, receivedTick;
    uint64_t requested, completed;
};
bool exposeTransport(std::wstring& error, TestHandle& mapping, TestView& view) {
    wchar_t controlName[256]{};
    if (!GetEnvironmentVariableW(controlEnvironment, controlName, 256)) return true;
    mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, controlName);
    if (mapping.value) view.value = MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(TestControl));
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    const bool valid = view.value && args && count == 3 && std::wcscmp(args[1], L"--camera-host") == 0 && std::wcslen(args[2]) < 128;
    if (valid) {
        auto& control = *static_cast<TestControl*>(view.value);
        std::wmemcpy(control.transportName, args[2], std::wcslen(args[2]) + 1);
        control.processId = GetCurrentProcessId();
        InterlockedExchange(&control.ready, 1);
    }
    if (args) LocalFree(args);
    if (!valid) error = L"Cannot expose this synthetic helper's transport.";
    return valid;
}
}

// A synthetic device substitutes only the hardware layer. The real helper
// process, shared memory, frame validation, and lifecycle code remain in use.
namespace lapse {
struct Camera::Impl {
    TestHandle controlMapping;
    TestView controlView;
    std::wstring mode;
    bool running = false;
    uint64_t started = 0;
    uint32_t sequence = 0;
};
Camera::Camera() : impl_(std::make_unique<Impl>()) {}
Camera::~Camera() { stop(); }
bool Camera::start(const std::wstring& id, std::wstring& error) {
    stop(); error.clear();
    if (id != L"transport-live" && id != L"transport-large" && id != L"transport-frozen" && id != L"transport-disconnect" &&
        id != L"transport-starting" && id != L"transport-hang-read" && id != L"transport-warm-live" && id != L"transport-warm-stall") {
        error = L"Unknown synthetic camera."; return false;
    }
    if (!exposeTransport(error, impl_->controlMapping, impl_->controlView)) return false;
    impl_->mode = id; impl_->running = true; impl_->started = GetTickCount64(); impl_->sequence = 0;
    return true;
}
void Camera::stop() { impl_->running = false; }
bool Camera::latest(Frame& output, std::wstring& error) {
    uint64_t tick = 0;
    return latest(output, error, tick);
}
bool Camera::latest(Frame& output, std::wstring& error, uint64_t& tick) {
    if (impl_->controlView.value) InterlockedIncrement(&static_cast<TestControl*>(impl_->controlView.value)->latestCalls);
    error.clear();
    if (!impl_->running) { error = L"Synthetic camera is closed."; return false; }
    if (impl_->mode == L"transport-starting") return false;
    if (impl_->mode == L"transport-warm-live" || impl_->mode == L"transport-warm-stall") {
        if (!impl_->sequence) { ++impl_->sequence; return false; }
        if (impl_->mode == L"transport-warm-stall") Sleep(INFINITE);
    }
    if (impl_->mode == L"transport-hang-read" && impl_->sequence) Sleep(INFINITE);
    if (impl_->mode == L"transport-disconnect" && GetTickCount64() - impl_->started >= 500) {
        error = L"Synthetic camera disconnected."; return false;
    }
    tick = impl_->mode == L"transport-frozen" ? impl_->started : GetTickCount64();
    output.width = impl_->mode == L"transport-large" ? 1280 : 64;
    output.height = impl_->mode == L"transport-large" ? 720 : 36;
    output.pixels.resize(size_t(output.width) * output.height * 4);
    const uint8_t sequence = static_cast<uint8_t>(++impl_->sequence);
    for (int y = 0; y < output.height; ++y) {
        for (int x = 0; x < output.width; ++x) {
            const size_t index = (size_t(y) * output.width + x) * 4;
            output.pixels[index] = static_cast<uint8_t>(x * 3);
            output.pixels[index + 1] = static_cast<uint8_t>(y * 7);
            output.pixels[index + 2] = sequence;
            output.pixels[index + 3] = 255;
        }
    }
    return true;
}
bool Camera::latestNewer(Frame& output, std::wstring& error, CameraSampleInfo& info, const CameraSampleInfo&) {
    uint64_t tick = 0;
    if (!latest(output, error, tick)) return false;
    info.epoch = 1; info.sequence = impl_->sequence; info.receivedTick = tick;
    info.timestamp100ns = int64_t(tick) * 10000; info.timestampValid = true;
    return true;
}
}

namespace {
using Clock = std::chrono::steady_clock;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
long long elapsed(Clock::time_point from) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count();
}
lapse::Frame firstFrame(lapse::CameraClient& camera) {
    lapse::Frame frame;
    std::wstring error;
    const auto began = Clock::now();
    while (elapsed(began) < 3000) {
        if (camera.latest(frame, error)) return frame;
        require(error.empty(), "unexpected failure before first synthetic frame");
        Sleep(10);
    }
    throw std::runtime_error("synthetic frame did not cross process boundary");
}
void verifyPixels(const lapse::Frame& frame) {
    require(frame.valid() && frame.width == 64 && frame.height == 36, "transported frame dimensions");
    const uint8_t sequence = frame.pixels[2];
    require(sequence != 0, "frame sequence is nonzero");
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const size_t index = (size_t(y) * frame.width + x) * 4;
            require(frame.pixels[index] == uint8_t(x * 3) && frame.pixels[index + 1] == uint8_t(y * 7) &&
                    frame.pixels[index + 2] == sequence && frame.pixels[index + 3] == 255,
                    "BGRA rows and frame generation remain intact across IPC");
        }
    }
}
void stopBounded(lapse::CameraClient& camera) {
    const auto began = Clock::now(); camera.stop(); camera.stop();
    require(elapsed(began) < 1000, "synthetic helper shutdown bounded");
}

struct TestEnvironment {
    std::wstring previous;
    bool existed = false;
    explicit TestEnvironment(const std::wstring& name) {
        const DWORD size = GetEnvironmentVariableW(controlEnvironment, nullptr, 0);
        if (size) {
            existed = true; previous.resize(size);
            previous.resize(GetEnvironmentVariableW(controlEnvironment, previous.data(), size));
        }
        require(SetEnvironmentVariableW(controlEnvironment, name.c_str()) != FALSE, "set private transport control");
    }
    ~TestEnvironment() { SetEnvironmentVariableW(controlEnvironment, existed ? previous.c_str() : nullptr); }
};
class TransportObserver {
public:
    explicit TransportObserver(lapse::CameraClient& camera, const wchar_t* mode = L"transport-live") {
        GUID guid{}; require(SUCCEEDED(CoCreateGuid(&guid)), "create transport control token");
        wchar_t token[40]{}; StringFromGUID2(guid, token, 40);
        const std::wstring name = L"Local\\Timelapse.TransportTest." + std::wstring(token);
        controlMapping_.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(TestControl), name.c_str());
        require(controlMapping_.value && GetLastError() != ERROR_ALREADY_EXISTS, "create owned transport control");
        controlView_.value = MapViewOfFile(controlMapping_.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(TestControl));
        require(controlView_.value != nullptr, "map owned transport control");
        std::wstring error;
        { TestEnvironment environment(name); require(camera.start(mode, error), "launch observed synthetic helper"); }
        auto& control = *static_cast<TestControl*>(controlView_.value);
        const auto began = Clock::now();
        while (!InterlockedCompareExchange(&control.ready, 0, 0) && elapsed(began) < 3000) Sleep(10);
        require(InterlockedCompareExchange(&control.ready, 0, 0) != 0, "synthetic helper exposes its owned token");
        process.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, control.processId);
        require(process.value != nullptr, "observe only this synthetic helper process");
        const std::wstring transportName = control.transportName;
        mapping_.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, transportName.c_str());
        mutex.value = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (transportName + L".mutex").c_str());
        stopEvent.value = OpenEventW(EVENT_MODIFY_STATE, FALSE, (transportName + L".stop").c_str());
        require(mapping_.value && mutex.value && stopEvent.value, "open only owned transport objects");
        view_.value = MapViewOfFile(mapping_.value, FILE_MAP_READ, 0, 0, sizeof(SharedHeader));
        require(view_.value != nullptr, "inspect owned shared header read-only");
    }
    SharedHeader observe() {
        require(WaitForSingleObject(mutex.value, 1000) == WAIT_OBJECT_0, "inspect shared header under actual mutex");
        const SharedHeader result = *static_cast<const SharedHeader*>(view_.value);
        ReleaseMutex(mutex.value);
        require(result.magic == 0x4C43414D && result.version == 5, "expected shared protocol");
        return result;
    }
    LONG copies() {
        return InterlockedCompareExchange(&static_cast<TestControl*>(controlView_.value)->latestCalls, 0, 0);
    }
    void corruptDimensions() {
        TestView writable;
        writable.value = MapViewOfFile(mapping_.value, FILE_MAP_WRITE, 0, 0, sizeof(SharedHeader));
        require(writable.value != nullptr, "map only owned synthetic frame for corruption");
        require(WaitForSingleObject(mutex.value, 1000) == WAIT_OBJECT_0, "corrupt owned frame under sharing mutex");
        static_cast<SharedHeader*>(writable.value)->width = 1281;
        ReleaseMutex(mutex.value);
    }
    SharedHeader waitFresh() {
        const auto began = Clock::now();
        while (elapsed(began) < 1500) {
            const auto frame = observe();
            if (frame.state == 2 && frame.generation && !(frame.generation & 1) && GetTickCount64() - frame.receivedTick < 500) return frame;
            Sleep(10);
        }
        throw std::runtime_error("producer did not publish a fresh shared frame");
    }
    void waitWarmupResponse() {
        const auto began = Clock::now();
        while (elapsed(began) < 1500) {
            const auto response = observe();
            if (response.state == 1 && response.requested == 1 && response.completed == 1 && !response.generation) return;
            Sleep(10);
        }
        throw std::runtime_error("eager empty warmup response was not completed");
    }
    TestHandle mutex, process, stopEvent;
private:
    TestHandle controlMapping_, mapping_;
    TestView controlView_, view_;
};
class MutexHold {
public:
    explicit MutexHold(HANDLE mutex) {
        ready_.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release_.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(ready_.value && release_.value, "create owned mutex gates");
        thread_ = std::thread([this, mutex] {
            const DWORD result = WaitForSingleObject(mutex, 1000);
            acquired_ = result == WAIT_OBJECT_0;
            SetEvent(ready_.value);
            if (acquired_) { WaitForSingleObject(release_.value, 11000); ReleaseMutex(mutex); }
            else if (result == WAIT_ABANDONED) ReleaseMutex(mutex);
        });
    }
    ~MutexHold() { release(); }
    void wait() { require(WaitForSingleObject(ready_.value, 1500) == WAIT_OBJECT_0 && acquired_, "owned thread holds real transport mutex"); }
    void release() { SetEvent(release_.value); if (thread_.joinable()) thread_.join(); }
private:
    TestHandle ready_, release_;
    std::thread thread_;
    bool acquired_ = false;
};
void expectTransientContention(lapse::CameraClient& camera) {
    lapse::Frame frame;
    std::wstring error;
    const auto began = Clock::now();
    require(!camera.latest(frame, error) && error.empty(), "brief sharing contention is transient");
    require(elapsed(began) < 250, "contended read remains bounded");
}

void freshProducerContention() {
    lapse::CameraClient camera;
    TransportObserver observer(camera);
    verifyPixels(firstFrame(camera));
    const uint64_t firstGeneration = observer.waitFresh().generation;
    const LONG firstCopies = observer.copies();
    Sleep(4100);
    require(observer.observe().generation == firstGeneration, "helper copied pixels without a consumer request");
    require(observer.copies() == firstCopies, "helper called Camera::latest without a consumer request");
    MutexHold hold(observer.mutex.value); hold.wait();
    expectTransientContention(camera);
    hold.release();
    verifyPixels(firstFrame(camera));
    require(observer.waitFresh().generation > firstGeneration, "fresh on-demand frame did not replace the old publication");
    require(observer.copies() == firstCopies + 1, "one demand performed more than one camera pixel copy");
    stopBounded(camera);
    std::cout << "fresh_producer_contention passed\n";
}

void contentionEpisodeReset() {
    lapse::CameraClient camera;
    TransportObserver observer(camera);
    verifyPixels(firstFrame(camera));
    const auto firstBegan = Clock::now();
    {
        MutexHold hold(observer.mutex.value); hold.wait();
        expectTransientContention(camera);
        Sleep(1600);
        expectTransientContention(camera);
    }
    verifyPixels(firstFrame(camera)); // Successful acquisition resets the episode.
    MutexHold hold(observer.mutex.value); hold.wait();
    const auto secondBegan = Clock::now();
    expectTransientContention(camera);
    Sleep(1600);
    require(elapsed(firstBegan) > 3000, "first episode would already have expired");
    expectTransientContention(camera);
    lapse::Frame frame;
    std::wstring error;
    while (elapsed(secondBegan) < 4000) {
        require(!camera.latest(frame, error), "held sharing mutex cannot deliver");
        if (!error.empty()) break;
        Sleep(25);
    }
    require(error.find(L"sharing") != std::wstring::npos, "persistent contention reports unavailable sharing");
    require(elapsed(secondBegan) >= 2900 && elapsed(secondBegan) < 3800, "delivered contention episode bounded at three seconds");
    hold.release();
    // The producer needs a new request after contention, rather than treating
    // an intentionally unrefreshed shared image as a stale capture source.
    const bool ready = camera.latest(frame, error);
    require(ready || error.empty(), "healthy demand request inherited an old shared-frame age");
    verifyPixels(firstFrame(camera)); observer.waitFresh();
    stopBounded(camera);
    std::cout << "contention_episode_reset passed\n";
}

void initialContentionBound() {
    lapse::CameraClient camera, starting, delayed;
    {
        TransportObserver observer(camera), warming(starting, L"transport-starting");
        TransportObserver unconsumed(delayed);
        observer.waitFresh(); // Deliberately do not consume a frame through CameraClient.
        unconsumed.waitFresh();
        lapse::Frame frame;
        std::wstring error;
        require(!starting.latest(frame, error) && error.empty(), "actual shared Starting state initially waits");
        MutexHold hold(observer.mutex.value); hold.wait();
        const auto began = Clock::now();
        expectTransientContention(camera);
        while (elapsed(began) < 9000) {
            require(!camera.latest(frame, error), "initial held mutex cannot deliver");
            if (!error.empty()) break;
            Sleep(25);
        }
        require(error.find(L"sharing") != std::wstring::npos, "initial contention reports unavailable sharing");
        require(elapsed(began) >= 7900 && elapsed(began) < 8800, "initial contention bounded at eight seconds");
        require(!starting.latest(frame, error) && error.find(L"8 seconds") != std::wstring::npos,
                "acquired Starting state preserves original activation deadline");
        require(unconsumed.observe().generation != 0, "initial unconsumed publication remains available");
        { MutexHold late(unconsumed.mutex.value); late.wait(); expectTransientContention(delayed); }
        verifyPixels(firstFrame(delayed));
        hold.release();
        // Leave the first client's eight-second episode expired: stop/start
        // must reset it before any later successful client acquisition.
        camera.stop(); starting.stop(); delayed.stop();
    }
    // Reuse the same client after stopping during an expired contention episode.
    TransportObserver restarted(camera);
    restarted.waitFresh();
    { MutexHold hold(restarted.mutex.value); hold.wait(); expectTransientContention(camera); }
    verifyPixels(firstFrame(camera));
    stopBounded(camera);
    std::cout << "initial_contention_bound passed\n";
}

void exitedHelperContention() {
    lapse::CameraClient camera;
    TransportObserver observer(camera);
    observer.waitFresh();
    MutexHold hold(observer.mutex.value); hold.wait();
    require(SetEvent(observer.stopEvent.value) != FALSE, "stop only owned synthetic helper through its normal protocol");
    require(WaitForSingleObject(observer.process.value, 1000) == WAIT_OBJECT_0, "owned helper has actually exited");
    lapse::Frame frame;
    std::wstring error;
    const auto began = Clock::now();
    require(!camera.latest(frame, error) && error.find(L"helper stopped unexpectedly") != std::wstring::npos,
            "known helper exit is terminal despite unavailable mutex");
    require(elapsed(began) < 250, "known helper exit does not receive contention grace");
    hold.release(); stopBounded(camera);
    std::cout << "exited_helper_contention passed\n";
}

void frameTransfer() {
    lapse::CameraClient camera;
    std::wstring error;
    require(camera.start(L"transport-live", error), "start synthetic helper");
    auto frame = firstFrame(camera);
    verifyPixels(frame);
    const uint8_t firstSequence = frame.pixels[2];
    const auto began = Clock::now();
    while (elapsed(began) < 1500) {
        if (camera.latest(frame, error) && frame.pixels[2] != firstSequence) break;
        require(error.empty(), "live frame update must not fail");
        Sleep(10);
    }
    verifyPixels(frame);
    require(frame.pixels[2] != firstSequence, "later camera sample replaces shared frame");
    stopBounded(camera);
    std::cout << "frame_transfer passed\n";
}

void staleFrame() {
    lapse::CameraClient camera;
    std::wstring error;
    require(camera.start(L"transport-frozen", error), "start frozen synthetic helper");
    auto frame = firstFrame(camera);
    verifyPixels(frame);
    const auto began = Clock::now();
    while (elapsed(began) < 4500) {
        if (!camera.latest(frame, error) && !error.empty()) break;
        Sleep(20);
    }
    require(error.find(L"3 seconds") != std::wstring::npos, "stale capture timestamp is rejected");
    require(elapsed(began) < 4000, "stale deadline bounded independently of helper heartbeat");
    stopBounded(camera);
    std::cout << "stale_frame passed\n";
}

void driverError() {
    lapse::CameraClient camera;
    std::wstring error;
    require(camera.start(L"transport-disconnect", error), "start disconnecting synthetic helper");
    auto frame = firstFrame(camera);
    const auto began = Clock::now();
    while (elapsed(began) < 2000) {
        if (!camera.latest(frame, error) && !error.empty()) break;
        Sleep(20);
    }
    require(error == L"Synthetic camera disconnected.", "driver error crosses IPC unchanged");
    stopBounded(camera);
    std::cout << "driver_error passed\n";
}

void delayedConsumer() {
    lapse::CameraClient camera;
    std::wstring error;
    require(camera.start(L"transport-live", error), "start delayed-consumer helper");
    Sleep(8200);
    auto frame = firstFrame(camera);
    verifyPixels(frame);
    stopBounded(camera);
    std::cout << "delayed_consumer passed\n";
}

void unresponsiveRequest() {
    lapse::CameraClient camera;
    std::wstring error;
    require(camera.start(L"transport-hang-read", error), "start owned request-stall helper");
    auto frame = firstFrame(camera);
    verifyPixels(frame);
    const auto original = frame.pixels;
    const auto began = Clock::now();
    long long slowest = 0;
    do {
        const auto reading = Clock::now();
        require(!camera.latest(frame, error), "stuck request returned a frame");
        require(frame.width == 64 && frame.height == 36 && frame.pixels == original,
                "transient or failed request overwrote the caller's frame");
        slowest = std::max(slowest, elapsed(reading));
        if (!error.empty()) break;
        Sleep(20);
    } while (elapsed(began) < 4500);
    require(error.find(L"frame request") != std::wstring::npos && elapsed(began) >= 2900 && elapsed(began) < 4000,
            "delivered camera request did not fail at its three-second bound");
    require(slowest < 250, "request wait blocked the parent on its stuck helper");
    stopBounded(camera);
    std::cout << "unresponsive_request passed\n";
}

void failedDeliveryKeepsPreviousFrame(bool allocationFailure) {
    lapse::CameraClient camera;
    TransportObserver observer(camera, allocationFailure ? L"transport-large" : L"transport-live");
    observer.waitFresh(); // Keep its initial response unconsumed by the client.
    lapse::Frame frame;
    frame.width = 64; frame.height = 36;
    frame.pixels.assign(64 * 36 * 4, 83);
    const auto original = frame.pixels;
    if (!allocationFailure) observer.corruptDimensions();
    std::wstring error;
    failedFrameBytes = 0;
    failFrameAllocation = allocationFailure;
    const bool delivered = camera.latest(frame, error);
    failFrameAllocation = false;
    require(!delivered && error.find(allocationFailure ? L"memory" : L"invalid frame") != std::wstring::npos,
            "owned malformed/allocation fixture did not hit the actual client delivery failure");
    // MSVC's vector allocator includes alignment bookkeeping for large buffers.
    require(!allocationFailure || (failedFrameBytes >= 1280 * 720 * 4 && failedFrameBytes <= 1280 * 720 * 4 + 64),
            "allocation fault did not target the actual delivered pixel buffer");
    require(frame.width == 64 && frame.height == 36 && frame.pixels == original,
            "failed client delivery changed its previous frame");
    require(firstFrame(camera).valid(), "client did not recover on a new demand after delivery failure");
    stopBounded(camera);
    std::cout << (allocationFailure ? "allocation_keeps_frame" : "malformed_keeps_frame") << " passed\n";
}

void warmupActivationDeadline() {
    lapse::CameraClient stalled, lateHealthy, lateStalled, lateEmpty;
    const auto began = Clock::now();
    TransportObserver original(stalled, L"transport-warm-stall");
    TransportObserver healthy(lateHealthy, L"transport-warm-live");
    TransportObserver delayed(lateStalled, L"transport-warm-stall");
    TransportObserver empty(lateEmpty, L"transport-starting");
    original.waitWarmupResponse(); healthy.waitWarmupResponse(); delayed.waitWarmupResponse(); empty.waitWarmupResponse();
    const auto warmed = Clock::now();
    // Start a new request well after activation. Its stall must not reset the
    // original eight-second deadline, even though the eager read completed.
    Sleep(3000);
    lapse::Frame frame;
    std::wstring error;
    require(!stalled.latest(frame, error) && error.empty(), "warmup response was not transient");
    const auto requested = Clock::now();
    do {
        require(!stalled.latest(frame, error), "stalled warmup request returned a frame");
        if (!error.empty()) break;
        Sleep(20);
    } while (elapsed(began) < 10000);
    require(error.find(L"8 seconds") != std::wstring::npos && elapsed(began) >= 7800 && elapsed(began) < 9300 &&
            elapsed(requested) < 6300,
            "warmup request reset the original eight-second activation deadline");
    std::cout << "warmup_stall_keeps_activation_deadline passed\n";

    // Neither late consumer has inspected its empty startup response. A healthy
    // source now answers one fresh request; a stuck source gets only bounded
    // response grace and cannot acquire another eight seconds of activation.
    while (elapsed(warmed) < 8100) Sleep(10);
    const auto healthyStart = Clock::now();
    require(lateHealthy.latest(frame, error) && error.empty(), "late empty startup response hid a now-healthy source");
    require(elapsed(healthyStart) < 250 && healthy.copies() == 2, "late warmup refresh was not one bounded fresh request");
    verifyPixels(frame);
    std::cout << "late_warmup_refreshes_once passed\n";
    const auto lateStart = Clock::now();
    require(!lateStalled.latest(frame, error) && error.find(L"8 seconds") != std::wstring::npos && elapsed(lateStart) < 250,
            "late stalled warmup received a new activation deadline");
    require(!lateStalled.latest(frame, error) && error.find(L"8 seconds") != std::wstring::npos,
            "repeated late warmup request erased the terminal deadline");
    require(delayed.copies() <= 2, "expired warmup repeatedly refreshed its stalled helper");
    std::cout << "late_warmup_stall_is_bounded passed\n";
    require(!lateEmpty.latest(frame, error) && error.find(L"8 seconds") != std::wstring::npos,
            "late empty response received a new activation deadline");
    const auto emptyCopies = empty.copies();
    require(emptyCopies == 2, "late empty response was not refreshed exactly once");
    for (int i = 0; i < 3; ++i)
        require(!lateEmpty.latest(frame, error) && error.find(L"8 seconds") != std::wstring::npos,
                "repeated late empty response erased its terminal deadline");
    require(empty.copies() == emptyCopies, "expired empty responses kept requesting camera copies");
    std::cout << "late_empty_warmup_refreshes_once passed\n";
    stopBounded(stalled); stopBounded(lateHealthy); stopBounded(lateStalled); stopBounded(lateEmpty);
}

void handlesReleased() {
    DWORD before = 0, after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "count baseline handles");
    for (int i = 0; i < 3; ++i) {
        lapse::CameraClient camera;
        std::wstring error;
        require(camera.start(L"transport-live", error), "restart synthetic helper");
        verifyPixels(firstFrame(camera));
        stopBounded(camera);
    }
    require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "count released handles");
    require(after == before, "mapping, mutex, event, process and job handles released");
    std::cout << "handles_released passed count=" << before << "\n";
}

void demandBenchmark() {
    lapse::CameraClient camera;
    TransportObserver observer(camera, L"transport-large");
    require(firstFrame(camera).valid(), "large owned camera frame missing");
    std::cout << "consumer_period_ms,wall_seconds,helper_cpu_ms,helper_cycles,pixel_reads,bytes_per_read\n";
    auto cpu = [&] {
        FILETIME created{}, exited{}, kernel{}, user{};
        require(GetProcessTimes(observer.process.value, &created, &exited, &kernel, &user) != FALSE, "read owned helper CPU");
        ULARGE_INTEGER k{}, u{}; k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime; return k.QuadPart + u.QuadPart;
    };
    auto cycles = [&] { ULONG64 count = 0; require(QueryProcessCycleTime(observer.process.value, &count) != FALSE,
        "read owned helper CPU cycles"); return count; };
    for (const DWORD period : {0u, 1000u, 5000u}) {
        const auto beforeCpu = cpu(), beforeCycles = cycles();
        const auto beforeCopies = observer.copies();
        const auto began = Clock::now();
        const int duration = period == 5000 ? 11000 : 8000;
        if (!period) Sleep(duration);
        else while (elapsed(began) < duration) { Sleep(period); require(firstFrame(camera).valid(), "large demand response missing"); }
        std::cout << period << ',' << elapsed(began) / 1000.0 << ',' << (cpu() - beforeCpu) / 10000.0 << ','
            << cycles() - beforeCycles << ',' << observer.copies() - beforeCopies << ',' << 1280 * 720 * 4 << '\n';
    }
    stopBounded(camera);
}
}

int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    const int hostResult = lapse::runCameraHost(nullptr);
    if (hostResult >= 0) return hostResult;
    try {
        if (argc == 2 && std::wcscmp(argv[1], L"--demand-benchmark") == 0) { demandBenchmark(); return 0; }
        if (argc == 2 && std::wcscmp(argv[1], L"--warmup-only") == 0) { warmupActivationDeadline(); return 0; }
        const bool contentionOnly = argc == 2 && std::wcscmp(argv[1], L"--contention-only") == 0;
        if (!contentionOnly) {
            frameTransfer(); staleFrame(); driverError(); delayedConsumer(); handlesReleased(); unresponsiveRequest();
            failedDeliveryKeepsPreviousFrame(false); failedDeliveryKeepsPreviousFrame(true);
            warmupActivationDeadline();
        }
        freshProducerContention(); contentionEpisodeReset(); initialContentionBound(); exitedHelperContention();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n'; return 1;
    }
}

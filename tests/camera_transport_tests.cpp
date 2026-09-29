#include "camera_host.h"
#include "capture.h"
#include <objbase.h>
#include <shellapi.h>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <thread>

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
struct TestControl { volatile LONG ready; DWORD processId; wchar_t transportName[128]; };
// Inspect only the header of this fixture's own mapping; actual transport code
// performs all publication and CameraClient consumption.
struct SharedHeader {
    uint32_t magic, version, state, width, height, bytes;
    uint64_t generation, receivedTick;
};
bool exposeTransport(std::wstring& error) {
    wchar_t controlName[256]{};
    if (!GetEnvironmentVariableW(controlEnvironment, controlName, 256)) return true;
    TestHandle mapping;
    mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, controlName);
    TestView view;
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
    std::wstring mode;
    bool running = false;
    uint64_t started = 0;
    uint32_t sequence = 0;
};
Camera::Camera() : impl_(std::make_unique<Impl>()) {}
Camera::~Camera() { stop(); }
bool Camera::start(const std::wstring& id, std::wstring& error) {
    stop(); error.clear();
    if (id != L"transport-live" && id != L"transport-frozen" && id != L"transport-disconnect" && id != L"transport-starting") {
        error = L"Unknown synthetic camera."; return false;
    }
    if (!exposeTransport(error)) return false;
    impl_->mode = id; impl_->running = true; impl_->started = GetTickCount64(); impl_->sequence = 0;
    return true;
}
void Camera::stop() { impl_->running = false; }
bool Camera::latest(Frame& output, std::wstring& error) {
    uint64_t tick = 0;
    return latest(output, error, tick);
}
bool Camera::latest(Frame& output, std::wstring& error, uint64_t& tick) {
    error.clear();
    if (!impl_->running) { error = L"Synthetic camera is closed."; return false; }
    if (impl_->mode == L"transport-starting") return false;
    if (impl_->mode == L"transport-disconnect" && GetTickCount64() - impl_->started >= 500) {
        error = L"Synthetic camera disconnected."; return false;
    }
    tick = impl_->mode == L"transport-frozen" ? impl_->started : GetTickCount64();
    output.width = 64; output.height = 36;
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
        process.value = OpenProcess(SYNCHRONIZE, FALSE, control.processId);
        require(process.value != nullptr, "observe only this synthetic helper process");
        const std::wstring transportName = control.transportName;
        mapping_.value = OpenFileMappingW(FILE_MAP_READ, FALSE, transportName.c_str());
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
        require(result.magic == 0x4C43414D && result.version == 1, "expected shared protocol");
        return result;
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
    const auto began = Clock::now();
    while (elapsed(began) < 3300) {
        observer.waitFresh(); Sleep(100);
    }
    require(observer.waitFresh().generation > firstGeneration, "producer stays fresh during skipped client reads");
    MutexHold hold(observer.mutex.value); hold.wait();
    expectTransientContention(camera);
    hold.release();
    verifyPixels(firstFrame(camera));
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
    // The producer was blocked for three seconds: acquisition must now enforce
    // the real shared-frame age or deliver an already refreshed frame.
    const bool ready = camera.latest(frame, error);
    require(ready || error.find(L"3 seconds") != std::wstring::npos, "acquisition preserves authoritative stale-frame validation");
    observer.waitFresh(); verifyPixels(firstFrame(camera));
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
        unconsumed.waitFresh();
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
}

int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    const int hostResult = lapse::runCameraHost(nullptr);
    if (hostResult >= 0) return hostResult;
    try {
        const bool contentionOnly = argc == 2 && std::wcscmp(argv[1], L"--contention-only") == 0;
        if (!contentionOnly) { frameTransfer(); staleFrame(); driverError(); delayedConsumer(); handlesReleased(); }
        freshProducerContention(); contentionEpisodeReset(); initialContentionBound(); exitedHelperContention();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n'; return 1;
    }
}

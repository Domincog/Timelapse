#include "camera_host.h"
#include "capture.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

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
    if (id != L"transport-live" && id != L"transport-frozen" && id != L"transport-disconnect") {
        error = L"Unknown synthetic camera."; return false;
    }
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

int wmain() {
    std::cout << std::unitbuf;
    const int hostResult = lapse::runCameraHost(nullptr);
    if (hostResult >= 0) return hostResult;
    try {
        frameTransfer(); staleFrame(); driverError(); delayedConsumer(); handlesReleased();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n'; return 1;
    }
}

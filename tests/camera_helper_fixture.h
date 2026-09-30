#pragma once
// Real helper process/protocol and image processing; synthetic camera sources
// only. No physical camera, desktop, persistent settings or quota is touched.
#include "camera_host.h"
#include "capture.h"
#include "../src/camera_host.cpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
constexpr wchar_t controlEnvironment[] = L"TIMELAPSE_TEST_NIGHT_CONTROL";
struct Control {
    volatile LONG ready, calls, copies, previewGap, previewGate, previewReached, previewRelease;
    volatile LONG freeze, future, throwCopy, readGate, readReached, readRelease;
    volatile LONG differentBufferReads;
    alignas(8) volatile LONG64 lastFrameAddress, expectedFrameAddress;
    volatile LONG boundaryArmed, boundaryDelivered;
    alignas(8) volatile LONG64 windowEnd, boundaryReceived;
    DWORD processId; wchar_t name[128];
};
struct TestHandle {
    HANDLE value = nullptr;
    ~TestHandle() { if (value) CloseHandle(value); }
};
struct TestView {
    void* value = nullptr;
    ~TestView() { if (value) UnmapViewOfFile(value); }
};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void pixels(lapse::Frame& frame, uint8_t value = 32, bool large = false) {
    frame.width = large ? 1280 : 64; frame.height = large ? 720 : 36;
    frame.pixels.resize(size_t(frame.width) * frame.height * 4);
    for (size_t i = 0; i < frame.pixels.size(); i += 4) {
        frame.pixels[i] = frame.pixels[i + 1] = frame.pixels[i + 2] = value; frame.pixels[i + 3] = 255;
    }
}
}
namespace lapse {
struct Camera::Impl {
    TestHandle mapping;
    TestView view;
    std::wstring mode;
    uint64_t started = 0;
    bool active = false;
};
Camera::Camera() : impl_(std::make_unique<Impl>()) {}
Camera::~Camera() { stop(); }
bool Camera::start(const std::wstring& id, std::wstring& error) {
    error.clear();
    if (id.rfind(L"night-", 0) != 0) { error = L"Unknown synthetic night source."; return false; }
    wchar_t control[256]{};
    if (!GetEnvironmentVariableW(controlEnvironment, control, 256)) { error = L"Missing owned test mapping."; return false; }
    impl_->mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, control);
    if (impl_->mapping.value) impl_->view.value = MapViewOfFile(impl_->mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Control));
    if (!impl_->view.value) { error = L"Cannot open owned test mapping."; return false; }
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool valid = argv && argc == 3 && std::wcslen(argv[2]) < 128;
    if (valid) std::wmemcpy(static_cast<Control*>(impl_->view.value)->name, argv[2], std::wcslen(argv[2]) + 1);
    if (argv) LocalFree(argv);
    if (!valid) { error = L"Invalid synthetic helper launch."; return false; }
    impl_->mode = id; impl_->started = GetTickCount64(); impl_->active = true;
    static_cast<Control*>(impl_->view.value)->processId = GetCurrentProcessId();
    InterlockedExchange(&static_cast<Control*>(impl_->view.value)->ready, 1);
    return true;
}
void Camera::stop() { impl_->active = false; }
bool Camera::latest(Frame& output, std::wstring& error) { uint64_t tick = 0; return latest(output, error, tick); }
bool Camera::latest(Frame& output, std::wstring& error, uint64_t& tick) {
    if (impl_->view.value) {
        auto& control = *static_cast<Control*>(impl_->view.value);
        if (InterlockedCompareExchange(&control.previewGate, 0, 0)) {
            InterlockedExchange(&control.previewReached, 1);
            const uint64_t until = GetTickCount64() + 2000;
            while (!InterlockedCompareExchange(&control.previewRelease, 0, 0) && GetTickCount64() < until) Sleep(1);
        }
        if (InterlockedCompareExchange(&control.previewGap, 0, 0)) { error.clear(); return false; }
    }
    CameraSampleInfo info;
    const bool ready = latestNewer(output, error, info, {});
    if (ready) tick = info.receivedTick;
    return ready;
}
bool Camera::latestNewer(Frame& output, std::wstring& error, CameraSampleInfo& info, const CameraSampleInfo& watermark) {
    error.clear();
    if (!impl_->active) { error = L"Synthetic night source is closed."; return false; }
    auto& control = *static_cast<Control*>(impl_->view.value); InterlockedIncrement(&control.calls);
    const LONG64 address = static_cast<LONG64>(reinterpret_cast<uintptr_t>(&output));
    InterlockedExchange64(&control.lastFrameAddress, address);
    const LONG64 expected = InterlockedCompareExchange64(&control.expectedFrameAddress, 0, 0);
    if (expected && address != expected) InterlockedIncrement(&control.differentBufferReads);
    uint64_t now = GetTickCount64();
    bool boundary = false;
    if ((impl_->mode == L"night-post-end-epoch" || impl_->mode == L"night-post-end-size" || impl_->mode == L"night-late-null-epoch") && watermark.sequence &&
        InterlockedCompareExchange(&control.boundaryArmed, 0, 0)) {
        const uint64_t end = static_cast<uint64_t>(InterlockedCompareExchange64(&control.windowEnd, 0, 0));
        if (now + 2 * NightCadenceMs >= end) {
            while (now <= end) { Sleep(DWORD(std::min<uint64_t>(end + 2 - now, 50))); now = GetTickCount64(); }
            boundary = true; InterlockedExchange64(&control.boundaryReceived, static_cast<LONG64>(now));
            InterlockedExchange(&control.boundaryDelivered, 1);
        }
    }
    const uint64_t age = now - impl_->started;
    if (impl_->mode == L"night-warm" && age < 1200) return false;
    if (impl_->mode == L"night-hang" && watermark.sequence) Sleep(INFINITE);
    info.epoch = impl_->mode == L"night-epoch" && age >= 500 ? 2 : 1;
    const uint64_t period = impl_->mode == L"night-slow" ? 2000 : 50;
    info.sequence = impl_->mode == L"night-duplicate" ? 1 : age / period + 1;
    if (control.freeze) info.sequence = 1;
    info.receivedTick = impl_->started + (info.sequence - 1) * period;
    if (control.future) info.receivedTick = now + 5000;
    if (boundary) { info.receivedTick = now; if (impl_->mode == L"night-post-end-epoch") info.epoch = 2; }
    if (boundary && impl_->mode == L"night-late-null-epoch") {
        info = watermark; info.epoch = 2; return false;
    }
    info.timestamp100ns = int64_t(info.sequence - 1) * int64_t(period) * 10000;
    info.timestampValid = true;
    if (impl_->mode == L"night-constant-time") info.timestamp100ns = 0;
    if (impl_->mode == L"night-regressing-time" && age >= 500) info.timestamp100ns = -int64_t(age) * 10000;
    if (now - info.receivedTick > 3000) { error = L"No fresh camera frame arrived for 3 seconds."; return false; }
    if (info.epoch == watermark.epoch && info.sequence <= watermark.sequence) return false;
    const bool inWindowSize = impl_->mode == L"night-in-window-size" && watermark.sequence && age >= 500;
    if ((impl_->mode == L"night-epoch" && watermark.sequence && info.epoch != watermark.epoch) || inWindowSize) {
        InterlockedExchange64(&control.boundaryReceived, static_cast<LONG64>(info.receivedTick));
        InterlockedExchange(&control.boundaryDelivered, 1);
    }
    if (control.readGate) {
        InterlockedExchange(&control.readReached, 1);
        const uint64_t until = GetTickCount64() + 5000;
        while (!control.readRelease && GetTickCount64() < until) Sleep(1);
    }
    if (control.throwCopy) throw std::bad_alloc();
    if (impl_->mode == L"night-slow-copy" && watermark.sequence) Sleep(3300);
    InterlockedIncrement(&control.copies); pixels(output, impl_->mode == L"night-slow" ? 128 : 32, impl_->mode == L"night-720p");
    // Only a subsequent Night read gets the fault; ordinary preview can still
    // fetch a healthy frame. Model late conversion/invalid raw metadata after
    // the initial source check, without a physical source or multi-second wait.
    if (watermark.sequence && impl_->mode == L"night-reuse-stale") info.receivedTick = now - 3100;
    if (watermark.sequence && impl_->mode == L"night-reuse-future") info.receivedTick = now + 5000;
    if (watermark.sequence && impl_->mode == L"night-reuse-zero") info.receivedTick = 0;
    if (watermark.sequence && impl_->mode == L"night-reuse-shape") {
        output.width = 1290; output.height = 36; output.pixels.assign(size_t(1290) * 36 * 4, 32);
    }
    if ((boundary && impl_->mode == L"night-post-end-size") || inWindowSize) {
        output.width = 1290; output.height = 36; output.pixels.assign(size_t(output.width) * output.height * 4, 32);
    }
    return true;
}
}
namespace {
using namespace std::chrono_literals;
struct Harness {
    lapse::CameraClient client;
    TestHandle mapping, sharedMapping, mutex;
    TestView view, sharedView;
    Harness(const wchar_t* mode) {
        GUID guid{}; require(SUCCEEDED(CoCreateGuid(&guid)), "create owned control identity");
        wchar_t token[40]{}; StringFromGUID2(guid, token, 40);
        const std::wstring name = L"Local\\Timelapse.NightTest." + std::wstring(token);
        mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Control), name.c_str());
        require(mapping.value && GetLastError() != ERROR_ALREADY_EXISTS, "create private control mapping");
        view.value = MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Control));
        require(view.value != nullptr, "map private control");
        const DWORD length = GetEnvironmentVariableW(controlEnvironment, nullptr, 0);
        std::wstring previous(length, L'\0');
        if (length) previous.resize(GetEnvironmentVariableW(controlEnvironment, previous.data(), length));
        require(SetEnvironmentVariableW(controlEnvironment, name.c_str()) != FALSE, "set child-only synthetic control");
        std::wstring error; const bool launched = client.start(mode, error);
        SetEnvironmentVariableW(controlEnvironment, length ? previous.c_str() : nullptr);
        require(launched, "launch owned synthetic helper");
        const uint64_t until = GetTickCount64() + 3000;
        while (!InterlockedCompareExchange(&control().ready, 0, 0) && GetTickCount64() < until) Sleep(5);
        require(control().ready, "synthetic helper exposed its owned transport");
        const std::wstring shared = control().name;
        sharedMapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, shared.c_str());
        mutex.value = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (shared + L".mutex").c_str());
        sharedView.value = MapViewOfFile(sharedMapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(lapse::SharedFrame));
        require(sharedMapping.value && mutex.value && sharedView.value, "inspect only owned helper transport");
    }
    ~Harness() { client.stop(); }
    Control& control() { return *static_cast<Control*>(view.value); }
    template<class Action> void inspect(Action action) {
        require(WaitForSingleObject(mutex.value, 1000) == WAIT_OBJECT_0, "acquire owned transport mutex");
        try { action(*static_cast<lapse::SharedFrame*>(sharedView.value)); }
        catch (...) { ReleaseMutex(mutex.value); throw; }
        ReleaseMutex(mutex.value);
    }
    void first(uint8_t expected = 32) {
        lapse::Frame frame; std::wstring error;
        const uint64_t until = GetTickCount64() + 3000;
        while (GetTickCount64() < until) {
            if (client.latest(frame, error)) { require(frame.valid() && frame.pixels[0] == expected, "preview must be ordinary source pixels"); return; }
            require(error.empty(), "synthetic preview failed"); Sleep(10);
        }
        throw std::runtime_error("synthetic source did not warm up");
    }
    void begin(uint64_t token, uint32_t duration = 1000) {
        lapse::NightSettings settings; settings.enabled = true;
        std::wstring error;
        require(client.beginNight(token, duration, settings, error) && error.empty(), "begin bounded software window");
    }
    lapse::NightWindowResult completed(uint64_t token, lapse::Frame& output, bool preview = false, uint32_t waitMs = 6000) {
        lapse::NightWindowResult result; lapse::Frame ordinary; std::wstring error;
        const uint64_t until = GetTickCount64() + waitMs;
        while (GetTickCount64() < until) {
            if (client.nightResult(token, output, result, error)) return result;
            if (!error.empty()) { std::wcerr << error << L'\n'; throw std::runtime_error("night result failed"); }
            if (preview && client.latest(ordinary, error)) require(ordinary.pixels[0] == 32, "blend pixels leaked into ordinary preview");
            require(error.empty(), "preview failed during accumulation"); Sleep(20);
        }
        throw std::runtime_error("night result did not complete");
    }
    std::wstring failed(uint64_t token, uint32_t waitMs = 6000) {
        lapse::Frame frame; lapse::NightWindowResult result; std::wstring error;
        const uint64_t until = GetTickCount64() + waitMs;
        while (GetTickCount64() < until) {
            require(!client.nightResult(token, frame, result, error), "invalid window produced a result");
            if (!error.empty()) return error;
            Sleep(20);
        }
        throw std::runtime_error("invalid window never reached its bounded error");
    }
};
}

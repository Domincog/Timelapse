#include "camera_host.h"
#include "capture.h"
#include <mfapi.h>
#include <algorithm>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
constexpr wchar_t testEnvironment[] = L"TIMELAPSE_TEST_CAMERA_HANG_MAPPING";
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
long long elapsed(Clock::time_point from) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count();
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
struct OwnedProcess {
    HANDLE value = nullptr;
    ~OwnedProcess() {
        if (value) { if (WaitForSingleObject(value, 0) == WAIT_TIMEOUT) TerminateProcess(value, ERROR_PROCESS_ABORTED); CloseHandle(value); }
    }
};
struct Control {
    std::wstring name;
    Handle mapping;
    volatile LONG* childPid = nullptr;
    Control() {
        GUID guid{}; require(SUCCEEDED(CoCreateGuid(&guid)), "create test GUID");
        wchar_t token[40]{}; StringFromGUID2(guid, token, 40);
        name = L"Local\\Timelapse.CameraTest." + std::wstring(token);
        mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), name.c_str());
        require(mapping.value != nullptr, "create test mapping");
        childPid = static_cast<volatile LONG*>(MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LONG)));
        require(childPid != nullptr, "map test control");
    }
    ~Control() { if (childPid) UnmapViewOfFile(const_cast<LONG*>(childPid)); }
    DWORD waitPid() {
        const auto began = Clock::now();
        while (!InterlockedCompareExchange(childPid, 0, 0) && elapsed(began) < 3000) Sleep(10);
        return static_cast<DWORD>(InterlockedCompareExchange(childPid, 0, 0));
    }
};
struct Environment {
    std::wstring previous;
    bool existed = false;
    explicit Environment(const std::wstring& value) {
        const DWORD length = GetEnvironmentVariableW(testEnvironment, nullptr, 0);
        if (length) {
            existed = true; previous.resize(length);
            previous.resize(GetEnvironmentVariableW(testEnvironment, previous.data(), length));
        }
        require(SetEnvironmentVariableW(testEnvironment, value.c_str()) != FALSE, "set test environment");
    }
    ~Environment() { SetEnvironmentVariableW(testEnvironment, existed ? previous.c_str() : nullptr); }
};

// This behavior exists only in the test executable. It simulates a driver
// stuck before Media Foundation activation can return; production has no hook.
bool simulateHungHelper(int argc, wchar_t** argv) {
    if (argc < 2 || std::wcscmp(argv[1], L"--camera-host") != 0) return false;
    wchar_t mappingName[256]{};
    if (!GetEnvironmentVariableW(testEnvironment, mappingName, 256)) return false;
    Handle mapping;
    mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName);
    if (!mapping.value) return false;
    auto* pid = static_cast<volatile LONG*>(MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LONG)));
    if (!pid) return false;
    InterlockedExchange(pid, static_cast<LONG>(GetCurrentProcessId()));
    Sleep(INFINITE);
    return true;
}

void invalidDevice() {
    lapse::CameraClient camera;
    lapse::Frame frame;
    std::wstring error;
    require(!camera.latest(frame, error) && !error.empty(), "closed camera reports error");
    require(!camera.start(L"", error) && !error.empty(), "empty camera ID rejected");
    require(!camera.start(std::wstring(4096, L'x'), error) && !error.empty(), "oversized camera ID rejected");
    auto began = Clock::now();
    require(camera.start(L"Timelapse.NonexistentCamera.7652", error), "launch nonexistent camera helper");
    require(elapsed(began) < 1500, "camera start must return without waiting for driver");
    bool gotFrame = false;
    while (elapsed(began) < 10000) {
        gotFrame = camera.latest(frame, error);
        if (gotFrame || !error.empty()) break;
        Sleep(20);
    }
    require(!gotFrame && !error.empty(), "invalid camera returns error within startup deadline");
    began = Clock::now(); camera.stop(); camera.stop();
    require(elapsed(began) < 1000, "repeated camera stop bounded");
    std::cout << "invalid_device passed\n";
}

void hungActivation() {
    Control control;
    lapse::CameraClient camera;
    std::wstring error;
    const auto began = Clock::now();
    {
        Environment environment(control.name);
        require(camera.start(L"simulated-hung-camera", error), "launch hung helper");
    }
    require(elapsed(began) < 1500, "hung camera start is asynchronous");
    const DWORD pid = control.waitPid(); require(pid != 0, "hung helper reached test point");
    Handle child; child.value = OpenProcess(SYNCHRONIZE, FALSE, pid);
    require(child.value != nullptr, "observe owned helper process");
    lapse::Frame frame;
    long long slowestRead = 0;
    while (elapsed(began) < 10000) {
        const auto reading = Clock::now();
        require(!camera.latest(frame, error), "hung helper has no frame");
        slowestRead = (std::max)(slowestRead, elapsed(reading));
        if (!error.empty()) break;
        Sleep(20);
    }
    require(!error.empty(), "hung activation reports timeout");
    require(slowestRead < 250, "latest does not wait for driver");
    require(elapsed(began) >= 7800 && elapsed(began) < 10000, "activation timeout is eight seconds");
    const auto stopping = Clock::now(); camera.stop();
    const auto stopMs = elapsed(stopping);
    require(stopMs < 1000, "hung camera stop is bounded");
    require(WaitForSingleObject(child.value, 1000) == WAIT_OBJECT_0, "stopped helper process actually exits");
    std::cout << "hung_activation passed latest_max_ms=" << slowestRead << " stop_ms=" << stopMs << "\n";
}

void orphanCleanup() {
    Control control;
    wchar_t executable[32768]{};
    require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "find test executable");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --test-orphan-parent";
    PROCESS_INFORMATION info{};
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    {
        Environment environment(control.name);
        require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &info) != FALSE, "launch orphan controller");
    }
    OwnedProcess controller; controller.value = info.hProcess;
    Handle thread; thread.value = info.hThread;
    const DWORD pid = control.waitPid(); require(pid != 0, "orphan helper reached test point");
    Handle child; child.value = OpenProcess(SYNCHRONIZE, FALSE, pid);
    require(child.value != nullptr, "observe orphan helper");
    require(TerminateProcess(controller.value, ERROR_PROCESS_ABORTED) != FALSE, "simulate controller crash");
    require(WaitForSingleObject(controller.value, 1000) == WAIT_OBJECT_0, "controller exited");
    require(WaitForSingleObject(child.value, 1500) == WAIT_OBJECT_0, "job closes orphan helper after parent crash");
    std::cout << "orphan_cleanup passed\n";
}

void hardwareCheck() {
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "initialize COM");
    require(SUCCEEDED(MFStartup(MF_VERSION)), "initialize MF");
    std::wstring error;
    const auto cameras = lapse::enumerateCameras(error);
    require(error.empty(), "enumerate hardware cameras");
    if (cameras.empty()) std::cout << "hardware_camera unavailable\n";
    else {
        lapse::CameraClient camera;
        const auto began = Clock::now();
        require(camera.start(cameras.front().id, error), "launch hardware helper");
        const auto startMs = elapsed(began);
        lapse::Frame frame;
        bool gotFrame = false;
        while (elapsed(began) < 10000) {
            gotFrame = camera.latest(frame, error);
            if (gotFrame || !error.empty()) break;
            Sleep(20);
        }
        require(gotFrame || !error.empty(), "hardware must produce frame or bounded failure");
        const auto stopping = Clock::now(); camera.stop();
        require(elapsed(stopping) < 1000, "hardware stop is bounded");
        std::cout << "hardware_start_ms=" << startMs << " frame=" << gotFrame << " stop_ms=" << elapsed(stopping) << "\n";
        if (!error.empty()) std::wcout << L"hardware_result=" << error << L"\n";
    }
    MFShutdown(); CoUninitialize();
}
}

int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    if (simulateHungHelper(argc, argv)) return 0;
    const int hostResult = lapse::runCameraHost(nullptr);
    if (hostResult >= 0) return hostResult;
    if (argc >= 2 && std::wcscmp(argv[1], L"--test-orphan-parent") == 0) {
        lapse::CameraClient camera;
        std::wstring error;
        if (!camera.start(L"simulated-hung-camera", error)) return 30;
        Sleep(INFINITE);
        return 0;
    }
    try {
        if (argc >= 2 && std::wcscmp(argv[1], L"--hardware") == 0) hardwareCheck();
        else { invalidDevice(); hungActivation(); orphanCleanup(); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}

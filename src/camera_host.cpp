#include "camera_host.h"
#include "capture.h"
#include <mfapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <new>

namespace lapse {
namespace {
constexpr uint32_t protocolMagic = 0x4C43414D;
constexpr uint32_t protocolVersion = 1;
constexpr uint32_t maxWidth = 1280, maxHeight = 720;
constexpr size_t maxPixels = size_t(maxWidth) * maxHeight * 4;
constexpr size_t idCapacity = 4096;
constexpr wchar_t mappingPrefix[] = L"Local\\Timelapse.Camera.";
enum class HostState : uint32_t { Starting = 1, Ready, Failed, Stopped };

struct SharedFrame {
    uint32_t magic, version;
    HostState state;
    uint32_t width, height, bytes;
    uint64_t generation, receivedTick;
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
    Handle mapping, mutex, stop;
    mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    mutex.value = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".mutex").c_str());
    stop.value = OpenEventW(SYNCHRONIZE, FALSE, (name + L".stop").c_str());
    if (!mapping.value || !mutex.value || !stop.value) return 3;
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
            Frame frame;
            uint64_t publishedTick = 0;
            while (WaitForSingleObject(stop.value, 100) == WAIT_TIMEOUT) {
                uint64_t receivedTick = 0;
                if (!camera.latest(frame, error, receivedTick)) {
                    if (!error.empty()) { publishError(error); result = 10; break; }
                    continue;
                }
                if (receivedTick == publishedTick) continue;
                if (!frame.valid() || frame.width > int(maxWidth) || frame.height > int(maxHeight)) {
                    publishError(L"The camera returned an unsupported frame size."); result = 11; break;
                }
                SharedLock lock(mutex.value);
                if (lock.result == WAIT_ABANDONED || lock.result == WAIT_FAILED) { result = 12; break; }
                if (!lock.acquired()) continue;
                // Odd means publication is in progress; only an even generation
                // with a valid size can ever be copied by the client.
                ++view.value->generation;
                view.value->width = uint32_t(frame.width); view.value->height = uint32_t(frame.height);
                view.value->bytes = static_cast<uint32_t>(frame.pixels.size());
                std::memcpy(view.value->pixels, frame.pixels.data(), frame.pixels.size());
                view.value->receivedTick = receivedTick;
                setMessage(*view.value, HostState::Ready, L"");
                ++view.value->generation;
                publishedTick = receivedTick;
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
    Handle mapping, mutex, stopEvent, process, job;
    View view;
    uint64_t started = 0, contentionSince = 0, generation = 0;
    bool delivered = false, contended = false;
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
    impl_->started = impl_->contentionSince = impl_->generation = 0;
    impl_->delivered = impl_->contended = false;
}

bool CameraClient::latest(Frame& output, std::wstring& error) {
    error.clear();
    if (!impl_->process.value || !impl_->view.value) { error = L"The camera is not running."; return false; }
    const uint64_t now = GetTickCount64();
    SharedLock lock(impl_->mutex.value);
    if (lock.result == WAIT_ABANDONED) { error = L"The camera helper stopped while sharing a frame. Try again."; return false; }
    if (lock.result == WAIT_FAILED) return fail(error, L"Windows could not read the camera frame.");
    if (!lock.acquired()) {
        if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0) {
            error = L"The camera helper stopped unexpectedly. Reconnect the camera and try again."; return false;
        }
        // A slow consumer can miss many healthy publications. A brief mutex
        // collision cannot establish that the producer's latest frame is stale.
        if (!impl_->contended) {
            impl_->contended = true;
            impl_->contentionSince = now;
        } else if (now - impl_->contentionSince > (impl_->delivered ? 3000 : 8000)) {
            error = L"Camera frame sharing stayed unavailable. Reconnect the camera and try again.";
        }
        return false;
    }
    impl_->contended = false; impl_->contentionSince = 0;
    const auto& shared = *impl_->view.value;
    if (!validHeader(shared)) { error = L"The camera helper returned invalid data. Try again."; return false; }
    if (shared.state == HostState::Failed) {
        const wchar_t* end = static_cast<const wchar_t*>(std::wmemchr(shared.error, L'\0', std::size(shared.error)));
        error = end ? std::wstring(shared.error, end) : L"The camera helper returned an invalid error.";
        if (error.empty()) error = L"The camera stopped unexpectedly. Try again.";
        return false;
    }
    if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0 || shared.state == HostState::Stopped) {
        error = L"The camera helper stopped unexpectedly. Reconnect the camera and try again."; return false;
    }
    if (shared.state == HostState::Starting) {
        if (now - impl_->started > 8000)
            error = L"The camera did not start within 8 seconds. Check Windows camera access and reconnect it.";
        return false;
    }
    const uint64_t frameNow = GetTickCount64();
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

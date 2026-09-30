#include "model.h"
#include "resources.h"
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <memory>
#include <string>
#include <fcntl.h>
#include <io.h>

namespace {
using namespace lapse::person;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct View {
    Shared* value = nullptr;
    ~View() { if (value) UnmapViewOfFile(value); }
};
struct Lock {
    HANDLE handle = nullptr;
    DWORD result = WAIT_FAILED;
    explicit Lock(HANDLE mutex) {
        result = WaitForSingleObject(mutex, 50);
        if (result == WAIT_OBJECT_0) handle = mutex;
        else if (result == WAIT_ABANDONED) ReleaseMutex(mutex);
    }
    ~Lock() { if (handle) ReleaseMutex(handle); }
    explicit operator bool() const { return handle != nullptr; }
};
bool running(HANDLE stop, DWORD delay = 0) { return WaitForSingleObject(stop, delay) == WAIT_TIMEOUT; }
bool fresh(const Source& source) {
    const uint64_t now = GetTickCount64();
    return source.receivedTick && source.receivedTick <= now && now - source.receivedTick <= SourceFreshnessMs;
}
bool validName(const wchar_t* name) {
    constexpr wchar_t prefix[] = L"Local\\Timelapse.Person.";
    const size_t length = std::wcslen(name), prefixLength = std::size(prefix) - 1;
    if (length <= prefixLength || length > 120 || std::wcsncmp(name, prefix, prefixLength)) return false;
    for (size_t at = prefixLength; at < length; ++at) {
        const wchar_t ch = name[at];
        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
              (ch >= L'A' && ch <= L'F') || ch == L'-' || ch == L'{' || ch == L'}')) return false;
    }
    return true;
}
int host(const wchar_t* name) {
    if (!validName(name)) return 2;
    Handle mapping, mutex, stop, request; View view;
    mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    mutex.value = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (std::wstring(name) + L".mutex").c_str());
    stop.value = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(name) + L".stop").c_str());
    request.value = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(name) + L".request").c_str());
    if (!mapping.value || !mutex.value || !stop.value || !request.value) return 3;
    view.value = static_cast<Shared*>(MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
    if (!view.value) return 4;
    auto& shared = *view.value;
    for (;;) {
        if (!running(stop.value)) return 0;
        Lock lock(mutex.value);
        if (lock) {
            if (!validHeader(shared) || shared.phase != Phase::Starting || shared.requested || shared.published) return 5;
            break;
        }
        if (lock.result != WAIT_TIMEOUT) return 6;
        if (!running(stop.value, 10)) return 0;
    }
    Detector detector;
    const bool initialized = detector.initialize(); // Never hold IPC mutex during runtime work.
    for (;;) {
        if (!running(stop.value)) return 0;
        Lock lock(mutex.value);
        if (lock) {
            if (!validHeader(shared) || shared.phase != Phase::Starting || shared.requested || shared.published) return 7;
            shared.phase = initialized ? Phase::Ready : Phase::Failed;
            break;
        }
        if (lock.result != WAIT_TIMEOUT) return 8;
        if (!running(stop.value, 10)) return 0;
    }
    if (!initialized) return 9;
    // One bounded reusable private copy; the runtime can never read a changing
    // shared frame. The optional parent's process watchdog owns hard deadlines.
    auto input = std::make_unique<Input>();
    HANDLE events[] = {stop.value, request.value};
    uint64_t serviced = 0; Source last{};
    for (;;) {
        const DWORD wake = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wake == WAIT_OBJECT_0) return 0;
        if (wake != WAIT_OBJECT_0 + 1) return 10;
        Source source{}; uint64_t id = 0; uint32_t bytes = 0;
        // A consumed auto-reset request is retained across transient contention.
        for (;;) {
            if (!running(stop.value)) return 0;
            Lock lock(mutex.value);
            if (lock) {
                if (!validHeader(shared) || shared.phase != Phase::Ready || shared.requested < serviced ||
                    shared.published != serviced) return 11;
                id = shared.requested; source = shared.inputSource; bytes = shared.inputBytes;
                if (bytes <= input->size()) std::memcpy(input->data(), shared.input.data(), bytes);
                break;
            }
            if (lock.result != WAIT_TIMEOUT) return 12;
            if (!running(stop.value, 10)) return 0;
        }
        if (!id || id == serviced) continue; // Extra event wakes do not re-infer.
        if (serviced == UINT64_MAX || id != serviced + 1) return 13;
        Output output{0, 0, Verdict::Unknown, Reason::InvalidInput};
        const bool sourceValid = source.sessionToken && source.cameraEpoch && source.sequence && validGeometry(source) &&
            bytes == size_t(source.width) * source.height * 3 && bytes <= input->size() &&
            !(source.sessionToken == last.sessionToken && source.cameraEpoch == last.cameraEpoch && source.sequence <= last.sequence);
        if (sourceValid && fresh(source)) detector.infer(input->data(), bytes, source, output);
        else if (sourceValid) output.reason = Reason::StaleSource;
        // Completion is retained privately across publication contention; no
        // second invocation and no reset of the parent's original deadline.
        for (;;) {
            if (!running(stop.value)) return 0;
            Lock lock(mutex.value);
            if (lock) {
                if (!validHeader(shared) || shared.phase != Phase::Ready || shared.requested != id ||
                    shared.published != serviced || shared.inputBytes != bytes || !sameSource(shared.inputSource, source)) return 14;
                if (sourceValid && !fresh(source)) { output.verdict = Verdict::Unknown; output.reason = Reason::StaleSource; }
                shared.outputSource = source; shared.output = output;
                shared.published = id; serviced = id; last = source;
                break;
            }
            if (lock.result != WAIT_TIMEOUT) return 15;
            if (!running(stop.value, 10)) return 0;
        }
    }
}
int licenses() {
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 20;
    for (const int id : {NoticeResource, NanoLicenseResource, NcnnLicenseResource}) {
        const auto text = resource(id);
        if (!text.data || !text.bytes || std::fwrite(text.data, 1, text.bytes, stdout) != text.bytes) return 20;
        if (std::fputc('\n', stdout) == EOF) return 20;
    }
    return std::fflush(stdout) == 0 ? 0 : 20;
}
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        if (argc == 2 && !std::wcscmp(argv[1], L"--licenses")) return licenses();
        if (argc == 3 && !std::wcscmp(argv[1], L"--person-host")) return host(argv[2]);
        return 1;
    } catch (...) { return 30; }
}

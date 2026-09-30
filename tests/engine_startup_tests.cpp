// Real Engine, Settings, CameraClient and Encoder construction. Only physical
// capture/process and thread power APIs are replaced. Allocation failure is
// thrown by global operator new while the real production operation is scoped.
#include "engine.h"
#include "capture.h"
#include "engine_startup_hooks.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;
namespace {
using Stage = probe::Stage;
thread_local Stage stage = Stage::None;
thread_local bool queuedStart = false, allWorkerAllocationsFail = false;
std::atomic<Stage> target{Stage::None};
std::atomic<bool> persistent{false}, onlyQueued{false}, gateFault{false}, failAllAfterClose{false};
std::atomic<unsigned> injections{0}, captures{0}, constructors[static_cast<size_t>(Stage::Count)]{};
std::atomic<size_t> failedBytes{0};
HANDLE startupEntered, startupRelease, faultEntered, faultRelease, completed;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT hr, const char* message) { require(SUCCEEDED(hr), message); }
void boundedWait(HANDLE event) {
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) ExitProcess(78);
}
void reset() {
    target = Stage::None; persistent = onlyQueued = gateFault = failAllAfterClose = false;
    injections = captures = 0; failedBytes = 0;
    for (auto& n : constructors) n = 0;
    ResetEvent(startupEntered); ResetEvent(startupRelease); ResetEvent(faultEntered); ResetEvent(faultRelease);
}
void arm(Stage at, bool repeatedly = false, bool onlyWithRecord = false, bool gated = false) {
    persistent = repeatedly; onlyQueued = onlyWithRecord; gateFault = gated; target = at;
}
struct ReleaseGates { ~ReleaseGates() { SetEvent(startupRelease); SetEvent(faultRelease); } };
void allocation(size_t size) {
    const Stage selected = target.load();
    bool fail = allWorkerAllocationsFail;
    if (!fail && selected != Stage::None && selected == stage && (!onlyQueued || queuedStart)) {
        if (persistent) fail = true;
        else { Stage expected = selected; fail = target.compare_exchange_strong(expected, Stage::None); }
    }
    if (!fail) return;
    failedBytes = size; ++injections;
    SetEvent(faultEntered);
    if (gateFault.exchange(false)) boundedWait(faultRelease);
    throw std::bad_alloc();
}
template<class Predicate> lapse::Status await(lapse::Engine& engine, Predicate predicate, bool preserveStart = false) {
    const auto until = std::chrono::steady_clock::now() + 6s;
    do {
        auto status = engine.status();
        if (preserveStart) require(status.state != lapse::State::Idle && !status.error && !status.recordingFailed,
            "An unconsumed Record lost its Starting state or inherited an allocation error");
        if (predicate(status)) return status;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error("Timed out waiting for engine state");
}
lapse::Settings settings(const std::filesystem::path& directory, bool camera = false) {
    // Keep this test ID inline so Settings faults still reach the layer allocation.
    lapse::Settings s; s.monitorId = L"test";
    s.monitor = {0, 0, 320, 240}; s.width = 320; s.height = 240; s.intervalMs = 1000;
    s.layers = lapse::preset(camera ? lapse::Mode::Camera : lapse::Mode::Desktop);
    // A real CameraClient rejects this invalid ID before IPC or a helper launch.
    // Constructor allocation remains actual and unchanged.
    if (camera) s.cameraId.assign(4096, L'A');
    s.folder = directory.wstring(); s.preview = camera;
    return s;
}
void pattern(lapse::Frame& out) {
    out.width = 320; out.height = 240; out.pixels.assign(320 * 240 * 4, 0);
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) {
        auto* p = out.pixels.data() + (size_t(y) * 320 + x) * 4;
        if (y < 120 && x < 160) p[2] = 255;
        else if (y < 120) p[1] = 255;
        else if (x < 160) p[0] = p[1] = p[2] = 255;
        p[3] = 255;
    }
}
void verify(const std::filesystem::path& file, unsigned frames) {
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader), "Cannot reopen recovered MP4");
    checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Deselect failed");
    checked(reader->SetStreamSelection(stream, TRUE), "Select failed");
    ComPtr<IMFMediaType> type;
    checked(reader->GetNativeMediaType(stream, 0, &type), "Missing native format");
    GUID subtype{}; checked(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Missing codec");
    UINT32 w = 0, h = 0; checked(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h), "Missing size");
    require(subtype == MFVideoFormat_H264 && w == 320 && h == 240, "Recovered codec or dimensions wrong");
    PROPVARIANT duration; PropVariantInit(&duration);
    checked(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration), "Missing duration");
    const bool validDuration = duration.vt == VT_UI8 && std::llabs(static_cast<long long>(duration.uhVal.QuadPart) - frames * 10000000LL / 30) < 20000;
    PropVariantClear(&duration); require(validDuration, "Recovered duration wrong");
    type.Reset(); checked(MFCreateMediaType(&type), "Decoder type failed");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Decoder major failed");
    checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Decoder subtype failed");
    checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "NV12 decode unavailable");
    unsigned count = 0; bool eos = false;
    for (unsigned reads = 0; reads < frames + 100; ++reads) {
        DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
        checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Recovered decode failed");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Recovered stream error");
        if (sample) {
            require(count < frames && std::llabs(timestamp - count * 10000000LL / 30) <= 1, "Recovered frame count/timing wrong");
            ComPtr<IMFMediaBuffer> buffer; ComPtr<IMF2DBuffer> plane;
            checked(sample->ConvertToContiguousBuffer(&buffer), "Missing decoded buffer");
            BYTE* bytes = nullptr; LONG stride = 320; DWORD length = 0;
            const bool twoD = SUCCEEDED(buffer.As(&plane));
            if (twoD) checked(plane->Lock2D(&bytes, &stride), "Cannot lock plane");
            else checked(buffer->Lock(&bytes, nullptr, &length), "Cannot lock buffer");
            bool valid = stride >= 320 && (twoD || length >= 320 * 240 * 3 / 2);
            const int points[][5] = {{80,60,63,102,240},{240,60,173,42,26},{80,180,235,128,128},{240,180,16,128,128}};
            if (valid) for (const auto& point : points) {
                const auto* uv = bytes + stride * 240 + (point[1] / 2) * stride + (point[0] & ~1);
                valid &= std::abs(int(bytes[point[1] * stride + point[0]]) - point[2]) <= 12 &&
                    std::abs(int(uv[0]) - point[3]) <= 12 && std::abs(int(uv[1]) - point[4]) <= 12;
            }
            if (twoD) plane->Unlock2D(); else buffer->Unlock();
            require(valid, "Recovered color/layout wrong"); ++count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { eos = true; break; }
    }
    require(eos && count == frames, "Recovered decoded frame count wrong");
    std::cout << "  decoded=" << count << " bytes=" << std::filesystem::file_size(file) << '\n';
}
void finishGood(lapse::Engine& engine, bool preserveStart = false) {
    const auto captured = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 2; }, preserveStart);
    engine.finish();
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(!saved.error && !saved.recordingFailed && saved.frames == captured.frames && !saved.savedPath.empty(), "Recovered recording failed to publish");
    verify(saved.savedPath, static_cast<unsigned>(saved.frames));
    require(DeleteFileW(saved.savedPath.c_str()) != FALSE, "Could not remove owned output");
}
void ownedFault(Stage at, const std::filesystem::path& directory) {
    reset(); const auto config = settings(directory);
    lapse::Engine engine; ReleaseGates release;
    boundedWait(startupEntered); engine.configure(config); arm(at); engine.record(); SetEvent(startupRelease);
    const auto failed = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    require(failed.recordingFailed, "Failed recording startup did not retain its terminal outcome");
    require(injections == 1 && constructors[static_cast<size_t>(at)] == 1, "Actual constructor fault missed");
    require(failed.frames == 0 && failed.savedPath.empty() &&
        (!std::filesystem::exists(directory) || std::filesystem::is_empty(directory)), "Constructor failure created media");
    if (at == Stage::Session) require(captures == 0 && constructors[static_cast<size_t>(Stage::Encoder)] == 0, "Session fault reached capture/encoder");
    std::cout << "  actual constructor allocation bytes=" << failedBytes << '\n';
    engine.record(); finishGood(engine, true);
    require(injections == 1, "Constructor fault repeated unexpectedly");
}
void cameraFault(int transition, const std::filesystem::path& directory) {
    reset(); auto config = settings(directory, true);
    lapse::Engine engine; ReleaseGates release;
    boundedWait(startupEntered); engine.configure(config); arm(Stage::Camera, false, false, transition != 0); SetEvent(startupRelease);
    boundedWait(faultEntered);
    if (transition == 0) {
        const auto failed = await(engine, [](const auto& s) { return s.error; });
        require(failed.state == lapse::State::Idle && !failed.recordingFailed && !failed.preview && failed.message.find(L"allocate") != std::wstring::npos,
            "Camera constructor failure was not a recoverable preview error");
        await(engine, [](const auto& s) { return s.error && s.message == L"Select a valid camera first."; });
        require(constructors[static_cast<size_t>(Stage::Camera)] == 2, "Camera constructor did not retry once");
    }
    config = settings(directory); config.preview = transition != 2;
    engine.configure(config);
    if (transition == 2) engine.record();
    SetEvent(faultRelease);
    if (transition != 2) {
        await(engine, [](const auto& s) { return !s.error && s.preview && s.preview->valid(); });
        config.preview = false; engine.configure(config); engine.record();
    }
    finishGood(engine, true);
    require(injections == 1 && failedBytes > 0, "Did not fail actual CameraClient Impl allocation");
    require(constructors[static_cast<size_t>(Stage::Camera)] == (transition == 0 ? 2u : 1u), "Stale constructor failure reopened retired camera");
}
void queuedSnapshot(const std::filesystem::path& directory) {
    reset(); const auto config = settings(directory);
    lapse::Engine engine; ReleaseGates release;
    boundedWait(startupEntered); engine.configure(config); arm(Stage::Snapshot, false, true); engine.record(); SetEvent(startupRelease);
    finishGood(engine, true);
    require(injections == 1 && failedBytes == sizeof(lapse::Layer), "Snapshot copy fault did not hit actual layer allocation");
    require(constructors[static_cast<size_t>(Stage::Session)] == 1 && constructors[static_cast<size_t>(Stage::Encoder)] == 1,
        "Queued command was duplicated");
}
void persistentSnapshot(bool record, const std::filesystem::path& directory) {
    reset(); const auto config = settings(directory);
    auto engine = std::make_unique<lapse::Engine>(); ReleaseGates release;
    boundedWait(startupEntered); engine->configure(config); arm(Stage::Snapshot, true, record);
    if (record) engine->record(); else engine->refreshSources();
    SetEvent(startupRelease); boundedWait(faultEntered);
    const auto until = std::chrono::steady_clock::now() + 2250ms;
    while (std::chrono::steady_clock::now() < until) {
        const auto status = engine->status();
        if (record) require(status.state == lapse::State::Starting && !status.error && status.frames == 0,
            "Persistent snapshot failure overwrote queued Record");
        std::this_thread::sleep_for(10ms);
    }
    require(injections >= 2 && injections <= 4, "Existing queued command caused unbounded retry");
    require(captures == 0 && constructors[static_cast<size_t>(Stage::Camera)] == 0 && constructors[static_cast<size_t>(Stage::Encoder)] == 0,
        "Failed snapshots reached capture/resources");
    const auto before = injections.load();
    const auto quitAt = GetTickCount64();
    engine.reset(); // Fault remains armed through Quit and thread join.
    const auto quitMs = GetTickCount64() - quitAt;
    require(quitMs < 500 && injections == before, "Quit allocated another snapshot or did not join promptly");
    std::cout << "  persistent failures=" << before << " over2250ms quitMs=" << quitMs << '\n';
    target = Stage::None;
}
void quitDuringPersistentAllocation(const std::filesystem::path& directory) {
    reset(); const auto config = settings(directory);
    auto engine = std::make_unique<lapse::Engine>(); ReleaseGates release;
    boundedWait(startupEntered); engine->configure(config); engine->record(); SetEvent(startupRelease);
    const auto recording = await(*engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
    failAllAfterClose = true;
    const auto quitAt = GetTickCount64(); engine.reset();
    const auto quitMs = GetTickCount64() - quitAt;
    require(injections >= 1 && injections <= 12 && quitMs < 1000, "Persistent finalization allocation failure blocked Quit");
    unsigned retained = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        require(item.path().extension() == L".mp4", "Unexpected retained test artifact");
        verify(item.path(), static_cast<unsigned>(recording.frames));
        require(DeleteFileW(item.path().c_str()) != FALSE, "Cannot delete retained test recording"); ++retained;
    }
    require(retained == 1, "Quit did not retain its finalized movie");
    std::cout << "  persistent finalization failures=" << injections << " quitMs=" << quitMs << '\n';
}
void disabledIdle() {
    reset();
    {
        lapse::Engine engine; ReleaseGates release; boundedWait(startupEntered);
        lapse::Settings cfg; cfg.preview = false; engine.configure(cfg); SetEvent(startupRelease);
        std::this_thread::sleep_for(120ms);
    }
    require(constructors[static_cast<size_t>(Stage::Camera)] == 0 && constructors[static_cast<size_t>(Stage::Encoder)] == 0 &&
        constructors[static_cast<size_t>(Stage::Session)] == 0 && captures == 0, "Disabled idle allocated optional resources");
}
}

void* operator new(size_t size) { allocation(size); if (void* p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc(); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace probe {
Scope::Scope(Stage next, bool queued) noexcept : previous(stage), priorQueued(queuedStart) {
    stage = next; queuedStart = queued; ++constructors[static_cast<size_t>(next)];
    if (next == Stage::Close && failAllAfterClose) allWorkerAllocationsFail = true;
}
Scope::~Scope() { stage = previous; queuedStart = priorQueued; }
HRESULT WINAPI initialize(LPVOID reserved, DWORD flags) {
    const auto result = CoInitializeEx(reserved, flags);
    allWorkerAllocationsFail = false; stage = Stage::None;
    SetEvent(startupEntered); boundedWait(startupRelease); return result;
}
EXECUTION_STATE WINAPI executionState(EXECUTION_STATE flags) { return flags; }
BOOL WINAPI forbiddenProcess(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) { ExitProcess(75); }
}
namespace lapse {
struct Camera::Impl {};
Camera::Camera() { ExitProcess(76); }
Camera::~Camera() = default;
bool Camera::start(const std::wstring&, std::wstring&) { ExitProcess(76); }
void Camera::stop() { ExitProcess(76); }
bool Camera::latest(Frame&, std::wstring&) { ExitProcess(76); }
bool Camera::latest(Frame&, std::wstring&, uint64_t&) { ExitProcess(76); }
bool Camera::latestNewer(Frame&, std::wstring&, CameraSampleInfo&, const CameraSampleInfo&) { ExitProcess(76); }
bool captureMonitor(const std::wstring& id, int, int, bool, Frame& out, std::wstring& error) {
    if (id != L"test") { error = L"Unknown synthetic display."; return false; }
    ++captures; pattern(out); error.clear(); return true;
}
}

int main() {
    std::cout << std::unitbuf;
    startupEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr); startupRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    faultEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr); faultRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!startupEntered || !startupRelease || !faultEntered || !faultRelease || !completed) return 80;
    std::thread watchdog([] { if (WaitForSingleObject(completed, 45000) != WAIT_OBJECT_0) ExitProcess(79); });
    const auto directory = std::filesystem::current_path() / (L"engine-startup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0, passed = 0;
    const auto test = [&](const char* name, auto work) { work(); ++passed; std::cout << "PASS " << name << '\n'; };
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        checked(com, "COM startup failed"); checked(MFStartup(MF_VERSION), "MF startup failed");
        test("disabled idle and shutdown create no optional resources", disabledIdle);
        test("actual CameraClient constructor failure retries current preview", [&] { cameraFault(0, directory); });
        test("actual CameraClient stale constructor failure respects changed source", [&] { cameraFault(1, directory); });
        test("actual CameraClient old preview failure preserves queued Record", [&] { cameraFault(2, directory); });
        test("actual session copy failure is terminal and next Record decodes", [&] { ownedFault(Stage::Session, directory); });
        test("actual Encoder constructor failure is terminal and next Record decodes", [&] { ownedFault(Stage::Encoder, directory); });
        test("actual Settings snapshot failure retains queued Record", [&] { queuedSnapshot(directory); });
        test("persistent snapshot failure with queued Record is bounded and Quit stays allocation-free", [&] { persistentSnapshot(true, directory); });
        test("persistent idle snapshot with queued Refresh is bounded and Quit stays allocation-free", [&] { persistentSnapshot(false, directory); });
        test("Quit survives persistent allocation failure during real finalization", [&] { quitDuringPersistentAllocation(directory); });
        std::cout << passed << "/10 actual-construction and shutdown cases passed.\n";
        require(!std::filesystem::exists(directory) || std::filesystem::is_empty(directory), "Owned media remained");
        std::filesystem::remove(directory);
    } catch (const std::exception& error) { std::cerr << "FAIL after " << passed << ": " << error.what() << " injections=" << injections << '\n'; result = 1; }
    MFShutdown(); if (SUCCEEDED(com)) CoUninitialize();
    SetEvent(completed); watchdog.join();
    for (HANDLE handle : {startupEntered, startupRelease, faultEntered, faultRelease, completed}) CloseHandle(handle);
    return result;
}

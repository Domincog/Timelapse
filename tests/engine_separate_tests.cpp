// Synthetic camera/desktop, real MP4 encoding and owned-file publication.
// Faults are injected only at the individual writer boundary; no devices or
// desktop pixels are captured and no system power setting is changed.
#include "engine.h"
#include "encoder.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
enum class Fault { None, OpenDesktop, OpenCamera, WriteDesktop, WriteCamera, FinishDesktop, FinishCamera, SecondPublicationAllocation, LastPublicationAllocation };
std::atomic<Fault> fault{Fault::None};
std::atomic<bool> disconnectCamera{false}, disconnectDesktop{false};
std::atomic<bool> cameraWarming{false}, allocationFreeCommit{false};
std::atomic<unsigned> cameraStarts{0}, cameraStops{0}, captures{0}, allocationFailures{0};
std::atomic<unsigned> recoveryOpens{0};
thread_local bool failAllocation = false;
}
void* operator new(std::size_t size) {
    if (failAllocation) { failAllocation = false; ++allocationFailures; throw std::bad_alloc(); }
    if (auto* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
namespace lapse {
class SeparateEncoder {
    Encoder real_;
    bool camera_ = false;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error, EncodingQuality quality, EncodingMode mode, bool recoveryMode) {
        if (recoveryMode) ++recoveryOpens;
        camera_ = path.find(L"-camera.recording.mp4") != std::wstring::npos;
        if (fault == (camera_ ? Fault::OpenCamera : Fault::OpenDesktop)) { error = L"Injected open failure."; return false; }
        return real_.open(path, width, height, fps, error, quality, mode, recoveryMode);
    }
    bool write(const Frame& frame, std::wstring& error) {
        if (fault == (camera_ ? Fault::WriteCamera : Fault::WriteDesktop)) { error = L"Injected write failure."; return false; }
        return real_.write(frame, error);
    }
    bool finish(std::wstring& error) { return real_.finish(error); }
    bool finishForPublication(std::wstring& error) {
        const bool result = real_.finishForPublication(error);
        if (result && fault == (camera_ ? Fault::FinishCamera : Fault::FinishDesktop)) { error = L"Injected finalization failure."; return false; }
        return result;
    }
    DWORD publish(const std::wstring& path) {
        const DWORD result = real_.publish(path);
        if (!camera_ && result == ERROR_SUCCESS && fault == Fault::SecondPublicationAllocation) failAllocation = true;
        if (camera_ && result == ERROR_SUCCESS && fault == Fault::LastPublicationAllocation) failAllocation = true;
        return result;
    }
    void releasePublication() noexcept { real_.releasePublication(); }
    uint64_t frames() const { return real_.frames(); }
};
}
EXECUTION_STATE WINAPI separateExecutionState(EXECUTION_STATE flags) {
    if (flags == ES_CONTINUOUS && fault == Fault::LastPublicationAllocation && failAllocation) {
        allocationFreeCommit = true; failAllocation = false;
    }
    return ES_CONTINUOUS;
}
#define Encoder SeparateEncoder
#define SetThreadExecutionState separateExecutionState
#include "../src/engine.cpp"
#undef SetThreadExecutionState
#undef Encoder

namespace {
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }
template<class Predicate> lapse::Status await(lapse::Engine& engine, Predicate predicate, int milliseconds = 7000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        auto status = engine.status();
        if (predicate(status)) return status;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < until);
    std::wcerr << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for separate recording");
}
void pattern(lapse::Frame& frame, int width, int height, bool camera) {
    frame.width = width; frame.height = height;
    frame.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    for (size_t i = 0; i < frame.pixels.size(); i += 4) {
        frame.pixels[i + (camera ? 2 : 0)] = 255;
        frame.pixels[i + 3] = 255;
    }
}
lapse::Settings settings(const std::filesystem::path& folder) {
    lapse::Settings result;
    result.separateFiles = true;
    // Deliberately only one layer: separate mode must still acquire both full
    // sources and must not encode this camera-only preview into the desktop.
    result.layers = lapse::preset(lapse::Mode::Camera);
    result.monitorId = L"synthetic-display"; result.cameraId = L"synthetic-camera";
    result.width = 320; result.height = 240; result.intervalMs = 60000;
    result.preview = false; result.folder = folder.wstring();
    return result;
}
void resetFaults() { fault = Fault::None; disconnectCamera = disconnectDesktop = cameraWarming = allocationFreeCommit = false; allocationFailures = 0; }
std::vector<std::filesystem::path> files(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> result;
    if (std::filesystem::exists(folder)) for (const auto& item : std::filesystem::directory_iterator(folder)) result.push_back(item.path());
    return result;
}
std::filesystem::path temporary(const std::filesystem::path& folder, bool camera) {
    const auto suffix = camera ? L"-camera.recording.mp4" : L"-desktop.recording.mp4";
    for (const auto& file : files(folder)) if (file.filename().wstring().find(suffix) != std::wstring::npos) return file;
    throw std::runtime_error("Missing temporary stream");
}
std::filesystem::path destination(const std::filesystem::path& temporaryPath) {
    auto value = temporaryPath.wstring(); value.replace(value.rfind(L".recording.mp4"), 14, L".mp4"); return value;
}
uint64_t identity(const std::filesystem::path& file) {
    HANDLE handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Cannot inspect owned output");
    BY_HANDLE_FILE_INFORMATION info{}; const BOOL result = GetFileInformationByHandle(handle, &info); CloseHandle(handle);
    require(result != FALSE, "Cannot identify output"); return (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}
void protectedFile(const std::filesystem::path& file) {
    require(!DeleteFileW(file.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION, "Active output could be deleted");
    const auto stolen = file.wstring() + L".stolen";
    require(!MoveFileExW(file.c_str(), stolen.c_str(), 0) && GetLastError() == ERROR_SHARING_VIOLATION, "Active output could be replaced");
}
void verify(const std::filesystem::path& path, unsigned frames, bool camera,
            bool recoveryMode = false, std::vector<LONGLONG>* timestamps = nullptr) {
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Cannot reopen output MP4");
    checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Deselect streams failed");
    checked(reader->SetStreamSelection(stream, TRUE), "Select video failed");
    ComPtr<IMFMediaType> type; checked(reader->GetNativeMediaType(stream, 0, &type), "Missing native media type");
    UINT32 width = 0, height = 0; checked(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "Missing dimensions");
    require(width == 320 && height == 240, "Session dimensions changed or stream has collage size");
    PROPVARIANT duration; PropVariantInit(&duration);
    checked(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration), "Missing duration");
    const bool validDuration = duration.vt == VT_UI8 && std::llabs(static_cast<long long>(duration.uhVal.QuadPart) - frames * 10000000LL / 30) < 20000;
    PropVariantClear(&duration); require(validDuration, "Separate stream duration is incorrect");
    type.Reset(); checked(MFCreateMediaType(&type), "Cannot create decoded type");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Cannot set decoded major type");
    checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Cannot set decoded pixel type");
    checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Cannot request NV12 decode");
    unsigned decoded = 0; bool ended = false;
    for (unsigned attempt = 0; attempt < frames + 100; ++attempt) {
        DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
        checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Cannot decode output");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Decoded output contains an error");
        if (sample) {
            // The native fragmented sink uses a different media timescale.
            // Keep the ordinary MP4 tolerance unchanged, and compare paired
            // recovery timestamps exactly in verifyPaths below.
            require(decoded < frames && std::llabs(timestamp - decoded * 10000000LL / 30) <= (recoveryMode ? 334 : 1), "Streams lost frame synchronization");
            if (timestamps) timestamps->push_back(timestamp);
            ComPtr<IMFMediaBuffer> buffer; ComPtr<IMF2DBuffer> plane;
            checked(sample->ConvertToContiguousBuffer(&buffer), "No decoded pixels");
            BYTE* pixels = nullptr; LONG stride = 320; DWORD length = 0;
            const bool twoD = SUCCEEDED(buffer.As(&plane));
            if (twoD) checked(plane->Lock2D(&pixels, &stride), "Cannot lock decoded plane");
            else checked(buffer->Lock(&pixels, nullptr, &length), "Cannot lock decoded pixels");
            bool correct = stride >= 320 && (twoD || length >= 320 * 240 * 3 / 2);
            const int expectedY = camera ? 63 : 32;
            const int expectedU = camera ? 102 : 240;
            const int expectedV = camera ? 240 : 118;
            if (correct) for (const auto& point : {POINT{20,20}, POINT{160,120}, POINT{300,220}}) {
                const BYTE* uv = pixels + stride * 240 + (point.y / 2) * stride + (point.x & ~1);
                correct &= std::abs(int(pixels[point.y * stride + point.x]) - expectedY) <= 10 &&
                    std::abs(int(uv[0]) - expectedU) <= 10 && std::abs(int(uv[1]) - expectedV) <= 10;
            }
            if (twoD) plane->Unlock2D(); else buffer->Unlock();
            require(correct, "Separate output contains wrong source or composited/letterboxed content"); ++decoded;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && decoded == frames, "Separate output lost frames");
}
void verifyPaths(const lapse::Status& status, unsigned desktopFrames, unsigned cameraFrames, bool recoveryMode = false) {
    require(!status.savedPaths.empty() && status.savedPath == status.savedPaths.front(), "Compatibility path is not first saved path");
    std::vector<LONGLONG> firstTimestamps;
    for (const auto& path : status.savedPaths) {
        const bool camera = path.find(L"-camera") != std::wstring::npos;
        std::vector<LONGLONG> timestamps;
        verify(path, camera ? cameraFrames : desktopFrames, camera, recoveryMode, &timestamps);
        if (desktopFrames == cameraFrames) {
            if (firstTimestamps.empty()) firstTimestamps = timestamps;
            else require(timestamps == firstTimestamps, "Paired output timestamps differ");
        }
        require(status.message.find(path) != std::wstring::npos, "Outcome did not name each playable video");
    }
}
void lifecycle(const std::filesystem::path& root, bool recoveryMode = false) {
    resetFaults(); const auto folder = root / (recoveryMode ? L"recovery-lifecycle" : L"lifecycle"); auto config = settings(folder);
    config.recoveryMode = recoveryMode;
    const unsigned recoveryBefore = recoveryOpens;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    require(recoveryOpens == recoveryBefore + (recoveryMode ? 2u : 0u), "Recovery option did not reach both writers");
    const auto desktop = temporary(folder, false), camera = temporary(folder, true);
    const uint64_t desktopIdentity = identity(desktop), cameraIdentity = identity(camera);
    protectedFile(desktop); protectedFile(camera);
    auto desktopBase = desktop.filename().wstring(), cameraBase = camera.filename().wstring();
    desktopBase.resize(desktopBase.size() - 22); cameraBase.resize(cameraBase.size() - 21);
    require(desktopBase == cameraBase, "Separate names do not share a session stem");
    const unsigned stopsBefore = cameraStops;
    engine.setPaused(true);
    const auto paused = await(engine, [&](const auto& s) { return s.state == lapse::State::Paused && cameraStops > stopsBefore; });
    const auto capturesBefore = captures.load();
    std::this_thread::sleep_for(250ms);
    require(engine.status().frames == 1 && captures == capturesBefore && engine.status().elapsed == paused.elapsed,
        "Paused hidden recording kept capturing or changed elapsed time");
    // Simulate unrelated UI settings changing while hidden. Sources, mode and
    // resolution belong to the active session; only visibility remains live.
    config.separateFiles = false; config.cameraId = L"invalid"; config.monitorId = L"invalid";
    config.width = 640; config.height = 360; config.layers = lapse::preset(lapse::Mode::Desktop);
    config.encodingMode = lapse::EncodingMode::HardwareHEVC; config.recoveryMode = !recoveryMode; engine.configure(config);
    engine.setPaused(false);
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 2; });
    engine.finish(); const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(!saved.error && !saved.recordingFailed && saved.frames == 2 && saved.savedPaths.size() == 2, "Paired recording did not complete");
    require(identity(saved.savedPaths[0]) == desktopIdentity && identity(saved.savedPaths[1]) == cameraIdentity, "Paired publication lost file ownership");
    verifyPaths(saved, 2, 2, recoveryMode);
    for (const auto& path : saved.savedPaths) require(MoveFileExW(path.c_str(), (path + L".moved").c_str(), 0) != FALSE, "Publication guard was not released");
    std::cout << "PASS recovery=" << recoveryMode << " paired sources, matching timestamps, full-frame content, pause/resume, frozen session, owned publication.\n";
}
void singleRecoveryAndInvalidMode(const std::filesystem::path& root) {
    resetFaults(); const auto folder = root / L"single-recovery"; auto config = settings(folder);
    config.separateFiles = false; config.layers = lapse::preset(lapse::Mode::Desktop);
    config.recoveryMode = true; config.encodingMode = lapse::EncodingMode::Efficient;
    const unsigned recoveryBefore = recoveryOpens;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.finish(); const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(!saved.error && saved.savedPaths.size() == 1 && recoveryOpens == recoveryBefore + 1,
        "Single-stream recovery mode did not reach its writer");
    verify(saved.savedPath, 1, false, true);
    config.folder = (root / L"invalid-recovery-hevc").wstring(); config.encodingMode = lapse::EncodingMode::HardwareHEVC;
    const unsigned capturesBefore = captures, opensBefore = recoveryOpens;
    engine.configure(config); engine.record();
    const auto rejected = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    require(rejected.recordingFailed && rejected.frames == 0 && rejected.savedPaths.empty() &&
        rejected.message.find(L"H.264") != std::wstring::npos && !std::filesystem::exists(config.folder) &&
        captures == capturesBefore && recoveryOpens == opensBefore,
        "Unsupported recovery encoder was not rejected before capture or output creation");
    std::cout << "PASS single recovery and explicit HEVC admission rejection before side effects.\n";
}
void failure(const std::filesystem::path& root, Fault selected, bool failFirstWrite = false) {
    resetFaults(); const auto folder = root / (L"fault-" + std::to_wstring(static_cast<int>(selected)) + (failFirstWrite ? L"-first" : L""));
    lapse::Engine engine; auto config = settings(folder); engine.configure(config);
    const bool openFailure = selected == Fault::OpenCamera || selected == Fault::OpenDesktop;
    if (openFailure || failFirstWrite) fault = selected;
    engine.record();
    if (!openFailure && !failFirstWrite) {
        await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
        fault = selected;
        if (selected == Fault::WriteCamera || selected == Fault::WriteDesktop) {
            engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; }); engine.setPaused(false);
        } else engine.finish();
    }
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    require(saved.recordingFailed, "Partial outcome was presented as successful recording");
    if (openFailure) require(saved.savedPaths.empty() && saved.frames == 0 && files(folder).empty(), "Failed paired startup retained empty files or stale paths");
    else if (selected == Fault::WriteCamera) {
        require(saved.savedPaths.size() == (failFirstWrite ? 1u : 2u) && saved.frames == (failFirstWrite ? 0u : 1u), "Partial write hid complete-pair count");
        verifyPaths(saved, failFirstWrite ? 1 : 2, 1);
        require(saved.message.find(failFirstWrite ? L"(1 frames)" : L"(2 frames)") != std::wstring::npos, "Partial write did not explain extra desktop frame");
    } else if (selected == Fault::WriteDesktop) {
        require(saved.savedPaths.size() == (failFirstWrite ? 0u : 2u), "Desktop write failure lost prior videos");
        if (!failFirstWrite) verifyPaths(saved, 1, 1);
        else require(files(folder).empty(), "First desktop write failure retained empty outputs");
    } else if (selected == Fault::FinishCamera || selected == Fault::FinishDesktop) {
        require(saved.savedPaths.size() == 1 && saved.message.find(L"Partial file:") != std::wstring::npos, "Finalization failure hid surviving output or partial file");
        verifyPaths(saved, 1, 1);
    } else {
        require(allocationFailures == 1 && saved.savedPaths.size() == 2 && saved.savedPaths[1].find(L".recording.mp4") != std::wstring::npos,
            "Second publication allocation failure lost the saved or retained output");
        verifyPaths(saved, 1, 1);
    }
    resetFaults(); engine.record();
    require(engine.status().savedPaths.empty(), "Retry retained paths from prior attempt");
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.finish(); const auto recovered = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(!recovered.error && recovered.savedPaths.size() == 2, "Dual writer did not recover after failure"); verifyPaths(recovered, 1, 1);
    std::cout << "PASS fault=" << int(selected) << " first=" << failFirstWrite << " partial preservation and retry.\n";
}
void collision(const std::filesystem::path& root, bool cameraCollision) {
    resetFaults(); const auto folder = root / (cameraCollision ? L"camera-collision" : L"desktop-collision");
    lapse::Engine engine; engine.configure(settings(folder)); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    const auto pending = temporary(folder, cameraCollision), final = destination(pending);
    const auto ownedIdentity = identity(pending);
    { std::ofstream sentinel(final, std::ios::binary); sentinel << "existing output"; require(bool(sentinel), "Cannot create collision fixture"); }
    engine.finish(); const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(saved.error && saved.recordingFailed && saved.savedPaths.size() == 2, "One collision discarded the other completed output");
    require(saved.savedPaths[cameraCollision ? 1 : 0] == pending.wstring() && identity(pending) == ownedIdentity, "Collision advertised the wrong owned file");
    std::ifstream sentinel(final, std::ios::binary); std::string text; std::getline(sentinel, text); require(text == "existing output", "Collision overwrote another file");
    verifyPaths(saved, 1, 1); std::cout << "PASS collision camera=" << cameraCollision << " preserves both outputs and destination.\n";
}
void sourceFailure(const std::filesystem::path& root, bool cameraFailure) {
    resetFaults(); const auto folder = root / (cameraFailure ? L"camera-disconnect" : L"desktop-disconnect");
    lapse::Engine engine; engine.configure(settings(folder)); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    (cameraFailure ? disconnectCamera : disconnectDesktop) = true; engine.setPaused(false);
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    require(saved.savedPaths.size() == 2 && saved.frames == 1 && saved.recordingFailed, "Source failure did not finalize both prior streams");
    verifyPaths(saved, 1, 1); resetFaults(); std::cout << "PASS source disconnect camera=" << cameraFailure << " stops and saves both.\n";
}
void shutdown(const std::filesystem::path& root) {
    resetFaults(); const auto folder = root / L"shutdown";
    { lapse::Engine engine; engine.configure(settings(folder)); engine.record(); await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; }); }
    const auto saved = files(folder); require(saved.size() == 2, "Shutdown lost paired outputs");
    for (const auto& path : saved) { require(path.wstring().find(L".recording.mp4") == std::wstring::npos, "Shutdown did not publish paired output"); verify(path, 1, path.wstring().find(L"-camera") != std::wstring::npos); }
    std::cout << "PASS shutdown finalizes and publishes both streams.\n";
}
void intervalAndCommit(const std::filesystem::path& root) {
    resetFaults(); const auto folder = root / L"interval"; auto config = settings(folder); config.intervalMs = 1000;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 3; });
    fault = Fault::LastPublicationAllocation; engine.finish();
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && allocationFreeCommit.load(); });
    require(!saved.error && saved.frames == 3 && saved.savedPaths.size() == 2 && allocationFailures == 0,
        "Paired publication allocated after the last rename or changed scheduled frame count");
    verifyPaths(saved, 3, 3); resetFaults();
    std::cout << "PASS shared interval, three matching timestamps, allocation-free final publication commit.\n";
}
void warmupAndInvalidSource(const std::filesystem::path& root) {
    resetFaults(); const auto folder = root / L"warmup"; auto config = settings(folder);
    lapse::Engine engine; engine.configure(config); cameraWarming = true;
    const unsigned startsBefore = cameraStarts; engine.record();
    await(engine, [&](const auto& s) { return s.state == lapse::State::Starting && cameraStarts > startsBefore; });
    engine.finish(); const auto cancelled = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(!cancelled.error && !cancelled.recordingFailed && cancelled.savedPaths.empty() && cancelled.frames == 0 && files(folder).empty(),
        "Cancelling paired source warmup created an output or recording failure");
    cameraWarming = false; config.cameraId = L"invalid"; engine.configure(config); engine.record();
    const auto failed = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    require(failed.recordingFailed && failed.savedPaths.empty() && failed.frames == 0 && files(folder).empty(), "Invalid camera started desktop-only output");
    std::cout << "PASS cancel during source warmup and unavailable camera do not start partial sessions.\n";
}
void hardwareSmoke(const std::filesystem::path& root) {
    // Optional manual smoke: public CTest remains independent of GPU/HEVC
    // availability. Preserve outputs for an external decoder when Windows has
    // no HEVC decoder. Production Encoder still rejects software fallback.
    for (const auto mode : {lapse::EncodingMode::HardwareH264, lapse::EncodingMode::HardwareHEVC}) {
        resetFaults(); const auto folder = root / (mode == lapse::EncodingMode::HardwareH264 ? L"hardware-h264" : L"hardware-hevc");
        auto config = settings(folder); config.encodingMode = mode;
        lapse::Engine engine; engine.configure(config); engine.record();
        const auto started = await(engine, [](const auto& s) { return (s.state == lapse::State::Recording && s.frames == 1) || (s.state == lapse::State::Idle && s.error); });
        if (started.error) {
            require(started.frames == 0 && started.savedPaths.empty() && files(folder).empty() && started.recordingFailed &&
                started.message.find(L"Cannot start") != std::wstring::npos, "Hardware failure was not an explicit, clean startup rejection");
            std::wcout << L"UNAVAILABLE " << folder.filename().wstring() << L": " << started.message << L'\n';
            continue;
        }
        engine.finish(); const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
        require(!saved.error && saved.savedPaths.size() == 2 && saved.frames == 1, "Hardware pair did not finish both streams");
        for (const auto& path : saved.savedPaths) std::wcout << L"HARDWARE_OUTPUT " << path << L'\n';
    }
}
}
namespace lapse {
bool CameraClient::beginNight(uint64_t, uint32_t, const NightSettings&, std::wstring& error) {
    error = L"Unexpected night request in ordinary-mode fixture."; return false;
}
bool CameraClient::nightResult(uint64_t, Frame&, NightWindowResult&, std::wstring& error) {
    error = L"Unexpected night result in ordinary-mode fixture."; return false;
}
void CameraClient::cancelNight() noexcept {}
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) {
    error = L"Unexpected activity observer in an Off-mode fixture."; return false;
}
void CameraClient::cancelActivityObservation() noexcept {}
struct CameraClient::Impl { bool active = false; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring& id, std::wstring& error) {
    if (id != L"synthetic-camera") { error = L"Unknown synthetic camera."; return false; }
    error.clear(); impl_->active = true; ++cameraStarts; return true;
}
void CameraClient::stop() { if (impl_->active) ++cameraStops; impl_->active = false; }
bool CameraClient::latest(Frame& output, std::wstring& error) {
    if (!impl_->active || disconnectCamera) { error = L"Synthetic camera disconnected."; return false; }
    if (cameraWarming) { error.clear(); return false; }
    error.clear(); pattern(output, 320, 240, true); return true;
}
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    if (id != L"synthetic-display" || disconnectDesktop) { error = L"Synthetic display disconnected."; return false; }
    error.clear(); ++captures; pattern(output, width, height, false); return true;
}
}
int main(int argc, char** argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-separate-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        if (argc == 2 && std::string(argv[1]) == "--hardware-smoke") {
            hardwareSmoke(root);
            std::wcout << L"Hardware artifacts kept at " << root.wstring() << L'\n';
            MFShutdown(); CoUninitialize(); return 0;
        }
        lifecycle(root);
        lifecycle(root, true); singleRecoveryAndInvalidMode(root);
        for (const auto selected : {Fault::OpenDesktop, Fault::OpenCamera, Fault::WriteDesktop, Fault::WriteCamera,
            Fault::FinishDesktop, Fault::FinishCamera, Fault::SecondPublicationAllocation}) failure(root, selected);
        failure(root, Fault::WriteDesktop, true); failure(root, Fault::WriteCamera, true);
        collision(root, false); collision(root, true); sourceFailure(root, false); sourceFailure(root, true); shutdown(root);
        intervalAndCommit(root); warmupAndInvalidSource(root);
        std::filesystem::remove_all(root);
        std::cout << "Separate engine: 19 synthetic real-encoder case groups passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts kept at " << root.wstring() << L'\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

#include "engine_person_camera_stub.h"

// This fixture owns no native desktop capture surface.
namespace lapse { void releaseDesktopCaptureCache() noexcept {} }

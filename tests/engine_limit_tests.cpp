// Timed sessions with real MP4 encoding/decoding and synthetic sources. No
// desktop/camera devices or system power requests are used by this fixture.
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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<bool> cameraWarmup{false};
std::atomic<unsigned> cameraStarts{0}, cameraStops{0};
std::atomic<int> desktopDelayMs{0}, cameraOpenDelayMs{0};
std::atomic<int> previewAllocationDelayMs{0}, finalizeDelayMs{0};
std::atomic<unsigned> previewAllocationFaults{0};
}
namespace lapse {
class LimitEncoder {
    Encoder real_;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error, EncodingQuality quality, EncodingMode mode) {
        if (path.find(L"-camera.recording.mp4") != std::wstring::npos)
            std::this_thread::sleep_for(std::chrono::milliseconds(cameraOpenDelayMs.exchange(0)));
        return real_.open(path, width, height, fps, error, quality, mode);
    }
    bool write(const Frame& frame, std::wstring& error) { return real_.write(frame, error); }
    bool finish(std::wstring& error) { return real_.finish(error); }
    bool finishForPublication(std::wstring& error) {
        std::this_thread::sleep_for(std::chrono::milliseconds(finalizeDelayMs.exchange(0)));
        return real_.finishForPublication(error);
    }
    DWORD publish(const std::wstring& path) { return real_.publish(path); }
    void releasePublication() noexcept { real_.releasePublication(); }
    uint64_t frames() const { return real_.frames(); }
};
}
EXECUTION_STATE WINAPI limitExecutionState(EXECUTION_STATE) { return ES_CONTINUOUS; }
#define Encoder LimitEncoder
#define SetThreadExecutionState limitExecutionState
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
        auto status = engine.status(); if (predicate(status)) return status;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < until);
    std::wcerr << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for recording limit");
}
lapse::Settings settings(const std::filesystem::path& directory, lapse::Mode mode = lapse::Mode::Desktop, bool separate = false) {
    lapse::Settings result;
    result.monitorId = L"synthetic-display"; result.cameraId = L"synthetic-camera";
    result.width = 320; result.height = 240; result.interval = 1;
    result.preview = false; result.folder = directory.wstring();
    result.layers = lapse::preset(mode); result.separateFiles = separate; result.recordingLimitSeconds = 1;
    return result;
}
void pixels(lapse::Frame& frame, int width, int height) {
    frame.width = width; frame.height = height; frame.pixels.assign(static_cast<size_t>(width) * height * 4, 96);
}
void verify(const std::wstring& path, uint64_t expectedFrames) {
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Cannot reopen timed MP4");
    checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Cannot deselect streams");
    checked(reader->SetStreamSelection(stream, TRUE), "Cannot select recorded video");
    ComPtr<IMFMediaType> type; checked(MFCreateMediaType(&type), "Cannot create decoded type");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Cannot set decoded major type");
    checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Cannot set decoded pixel type");
    checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Cannot decode timed MP4");
    uint64_t frames = 0; bool ended = false;
    for (uint64_t attempt = 0; attempt < expectedFrames + 100; ++attempt) {
        DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
        checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Timed MP4 decode failed");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Timed MP4 has a stream error");
        if (sample) {
            require(frames < expectedFrames && std::llabs(timestamp - static_cast<LONGLONG>(frames) * 10000000 / 30) <= 1,
                "Timed MP4 has an extra frame or incorrect timestamp");
            ComPtr<IMFMediaBuffer> buffer; checked(sample->ConvertToContiguousBuffer(&buffer), "Decoded frame has no pixels");
            DWORD length = 0; checked(buffer->GetCurrentLength(&length), "Decoded frame size unavailable");
            require(length >= 320 * 240 * 3 / 2, "Decoded frame is incomplete"); ++frames;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && frames == expectedFrames, "Timed MP4 frame count mismatch");
}
void verifySaved(const lapse::Status& status, uint64_t expectedFrames, size_t files) {
    require(status.state == lapse::State::Idle && !status.error && !status.recordingFailed, "Automatic completion was treated as a failure");
    require(status.frames == expectedFrames && status.savedPaths.size() == files && status.savedPath == status.savedPaths.front(),
        "Automatic completion lost output paths or admitted a late frame");
    require(status.message.find(L"Recording time limit reached.") != std::wstring::npos, "Automatic completion did not explain why recording stopped");
    require(status.elapsed >= 1 && status.elapsed < 3, "Timed session reported incorrect active time");
    for (const auto& path : status.savedPaths) verify(path, expectedFrames);
}
void sourceMode(const std::filesystem::path& root, lapse::Mode mode, bool separate) {
    const auto name = L"source-" + std::to_wstring(static_cast<int>(mode)) + (separate ? L"-separate" : L"");
    auto config = settings(root / name, mode, separate);
    if (mode == lapse::Mode::Camera) config.interval = 60; // Deadline must wake a hidden sparse session.
    if (mode == lapse::Mode::Overlay) config.preview = true;
    const auto stopsBefore = cameraStops.load();
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    if (mode == lapse::Mode::Desktop && !separate) {
        config.recordingLimitSeconds = 0; engine.configure(config); // The active session remains bounded.
    }
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 3000);
    verifySaved(saved, 1, separate ? 2 : 1);
    if (!config.preview && (mode == lapse::Mode::Camera || separate))
        require(cameraStops > stopsBefore, "Hidden timed completion left its camera active");
    if (config.preview) {
        std::this_thread::sleep_for(150ms);
        require(engine.status().message == saved.message && !engine.status().recordingFailed, "Idle preview erased successful automatic completion");
    }
    std::wcout << L"PASS " << name << L": boundary excludes a second frame, frozen limit, successful output.\n";
}
void startupExcluded(const std::filesystem::path& root, bool encoderDelay) {
    auto config = settings(root / (encoderDelay ? L"writer-startup" : L"camera-warmup"), lapse::Mode::Camera, encoderDelay);
    config.interval = 60;
    cameraWarmup = !encoderDelay; cameraOpenDelayMs = encoderDelay ? 1200 : 0;
    const unsigned startsBefore = cameraStarts;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [&](const auto& s) { return s.state == lapse::State::Starting && cameraStarts > startsBefore; });
    if (!encoderDelay) {
        std::this_thread::sleep_for(1200ms);
        const auto warming = engine.status();
        require(warming.state == lapse::State::Starting && warming.elapsed == 0 && warming.frames == 0,
            "Initial source warmup consumed the recording limit");
        cameraWarmup = false;
    }
    const auto started = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    require(started.elapsed < .2, "Opening the source or second encoder counted as active recording");
    const auto activeAt = std::chrono::steady_clock::now();
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 3000);
    require(std::chrono::steady_clock::now() - activeAt >= 800ms, "Startup shortened the active recording budget");
    verifySaved(saved, 1, encoderDelay ? 2 : 1);
    std::cout << "PASS startup excluded, encoder_delay=" << encoderDelay << ".\n";
}
void pauseExcluded(const std::filesystem::path& root) {
    auto config = settings(root / L"pause", lapse::Mode::Camera, true); config.interval = 60;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    std::this_thread::sleep_for(200ms); engine.setPaused(true);
    const auto paused = await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    std::this_thread::sleep_for(1200ms);
    require(engine.status().state == lapse::State::Paused && engine.status().elapsed == paused.elapsed,
        "Pause consumed the recording budget or triggered automatic stop");
    engine.setPaused(false);
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 3000);
    verifySaved(saved, 2, 2);
    std::cout << "PASS pause excluded, resumed pair admitted within remaining budget.\n";
}
void slowCapture(const std::filesystem::path& root) {
    auto config = settings(root / L"slow-capture", lapse::Mode::Desktop, true); config.recordingLimitSeconds = 2;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    desktopDelayMs = 1400;
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 5000);
    verifySaved(saved, 1, 2);
    require(saved.elapsed >= 2 && desktopDelayMs == 0, "Slow acquisition did not exercise the deadline crossing");
    std::cout << "PASS capture crossing deadline does not admit either late video frame.\n";
}
void unlimitedAndManualFinish(const std::filesystem::path& root, int limit) {
    auto config = settings(root / (limit ? L"negative-unlimited" : L"zero-unlimited"), lapse::Mode::Camera);
    config.recordingLimitSeconds = limit; config.interval = 60;
    const unsigned stopsBefore = cameraStops;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    config.recordingLimitSeconds = 1; engine.configure(config);
    std::this_thread::sleep_for(1200ms);
    require(engine.status().state == lapse::State::Recording, "Live settings changed the unlimited session limit");
    engine.finish(); const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(!saved.error && !saved.recordingFailed && saved.frames == 1 && saved.savedPaths.size() == 1 && cameraStops > stopsBefore,
        "Manual hidden-camera Finish failed or retained the active device");
    require(saved.message.find(L"time limit") == std::wstring::npos, "Manual finish was mislabeled as automatic completion");
    verify(saved.savedPath, 1);
    std::cout << "PASS unlimited=" << limit << ", frozen setting, manual Finish releases hidden camera.\n";
}
void collision(const std::filesystem::path& root) {
    const auto directory = root / L"collision"; auto config = settings(directory, lapse::Mode::Desktop, true); config.interval = 60;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    std::wstring temporary;
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.path().wstring().find(L"-desktop.recording.mp4") != std::wstring::npos) temporary = item.path().wstring();
    require(!temporary.empty(), "Cannot locate owned desktop temporary file");
    auto final = temporary; final.replace(final.rfind(L".recording.mp4"), 14, L".mp4");
    { std::ofstream sentinel(final, std::ios::binary); sentinel << "existing output"; require(bool(sentinel), "Cannot create owned collision fixture"); }
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 3000);
    require(saved.error && saved.recordingFailed && saved.savedPaths.size() == 2 && saved.savedPath == temporary,
        "Automatic completion hid a publication failure or discarded a completed stream");
    require(saved.message.find(L"Recording time limit reached.") != std::wstring::npos && saved.message.find(temporary) != std::wstring::npos,
        "Automatic save failure omitted its cause or recovery path");
    for (const auto& path : saved.savedPaths) verify(path, 1);
    std::ifstream sentinel(final, std::ios::binary); std::string text; std::getline(sentinel, text);
    require(text == "existing output", "Automatic publication replaced an existing file");
    std::cout << "PASS automatic publication collision preserves both movies and reports failure.\n";
}
void previewFailureDeadline(const std::filesystem::path& root) {
    auto config = settings(root / L"preview-allocation"); config.interval = 60; config.preview = true; config.recordingLimitSeconds = 2;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    previewAllocationDelayMs = 800;
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 4000);
    verifySaved(saved, 1, 1);
    require(previewAllocationFaults == 1 && saved.elapsed < 2.5, "Preview allocation recovery delayed the recording limit by its full cooldown");
    std::cout << "PASS preview allocation cooldown respects the earlier recording deadline.\n";
}
void finalizationExcluded(const std::filesystem::path& root) {
    auto config = settings(root / L"slow-finalization"); config.interval = 60; config.preview = true;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    const auto activeAt = std::chrono::steady_clock::now(); finalizeDelayMs = 800;
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 4000);
    verifySaved(saved, 1, 1);
    require(std::chrono::steady_clock::now() - activeAt >= 1600ms && saved.elapsed < 1.5,
        "Finalization delay counted as active recording time");
    std::this_thread::sleep_for(600ms);
    require(engine.status().elapsed == saved.elapsed && engine.status().message == saved.message,
        "Idle preview changed completed elapsed time or completion reason");
    std::cout << "PASS slow finalization and later preview do not extend completed elapsed time.\n";
}
}
namespace lapse {
struct CameraClient::Impl { bool active = false; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring& id, std::wstring& error) {
    if (id != L"synthetic-camera") { error = L"Invalid synthetic camera."; return false; }
    error.clear(); impl_->active = true; ++cameraStarts; return true;
}
void CameraClient::stop() { if (impl_->active) ++cameraStops; impl_->active = false; }
bool CameraClient::latest(Frame& output, std::wstring& error) {
    error.clear(); if (!impl_->active) { error = L"Synthetic camera is stopped."; return false; }
    if (cameraWarmup) return false;
    pixels(output, 320, 240); return true;
}
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    if (id != L"synthetic-display") { error = L"Invalid synthetic display."; return false; }
    if (width == 640) if (const int delay = previewAllocationDelayMs.exchange(0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay)); ++previewAllocationFaults; throw std::bad_alloc();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(desktopDelayMs.exchange(0)));
    error.clear(); pixels(output, width, height); return true;
}
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-limit-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        sourceMode(root, lapse::Mode::Desktop, false); sourceMode(root, lapse::Mode::Camera, false);
        sourceMode(root, lapse::Mode::Overlay, false); sourceMode(root, lapse::Mode::Desktop, true);
        startupExcluded(root, false); startupExcluded(root, true); pauseExcluded(root); slowCapture(root);
        unlimitedAndManualFinish(root, 0); unlimitedAndManualFinish(root, -1); collision(root);
        previewFailureDeadline(root); finalizationExcluded(root);
        std::filesystem::remove_all(root);
        std::cout << "Recording limits: 13 synthetic real-encoder cases passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts kept at " << root.wstring() << L'\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

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
std::atomic<bool> finalizeFailure{false};
std::atomic<int> captureFailureDelayMs{0}, writeFailureDelayMs{0}, writeFailureSide{0};
std::atomic<bool> cameraOpenFailure{false};
std::atomic<unsigned> finalizeThrows{0};
std::atomic<uint64_t> firstAdmissionTick{0}, failureObservedTick{0};
std::atomic<unsigned> finalizeCalls{0};
}
namespace lapse {
class LimitEncoder {
    Encoder real_;
    bool camera_ = false;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error, EncodingQuality quality, EncodingMode mode, bool recoveryMode) {
        camera_ = path.find(L"-camera.recording.mp4") != std::wstring::npos;
        if (camera_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cameraOpenDelayMs.exchange(0)));
            if (cameraOpenFailure.exchange(false)) { error = L"Injected initial camera writer failure."; return false; }
        }
        return real_.open(path, width, height, fps, error, quality, mode, recoveryMode);
    }
    bool write(const Frame& frame, std::wstring& error) {
        uint64_t empty = 0; firstAdmissionTick.compare_exchange_strong(empty, GetTickCount64());
        if (writeFailureSide == (camera_ ? 2 : 1)) if (const int delay = writeFailureDelayMs.exchange(0)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            failureObservedTick = GetTickCount64(); error = L"Injected delayed writer failure."; return false;
        }
        return real_.write(frame, error);
    }
    bool finish(std::wstring& error) { return real_.finish(error); }
    bool finishForPublication(std::wstring& error) {
        ++finalizeCalls;
        std::this_thread::sleep_for(std::chrono::milliseconds(finalizeDelayMs.exchange(0)));
        const bool finished = real_.finishForPublication(error);
        if (finished && finalizeThrows) { --finalizeThrows; throw std::bad_alloc(); }
        if (finished && finalizeFailure.exchange(false)) {
            error = L"Injected MP4 finalization failure.";
            return false;
        }
        return finished;
    }
    DWORD publish(const std::wstring& path) { return real_.publish(path); }
    void releasePublication() noexcept { real_.releasePublication(); }
    bool emptyOutputDiscarded() const noexcept { return real_.emptyOutputDiscarded(); }
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
    result.width = 320; result.height = 240; result.intervalMs = 1000;
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
    if (mode == lapse::Mode::Camera) config.intervalMs = 60000; // Deadline must wake a hidden sparse session.
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
    config.intervalMs = 60000;
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
    auto config = settings(root / L"pause", lapse::Mode::Camera, true); config.intervalMs = 60000;
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
    config.recordingLimitSeconds = limit; config.intervalMs = 60000;
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
void collision(const std::filesystem::path& root, bool separate = true) {
    const auto directory = root / (separate ? L"collision-separate" : L"collision-single");
    auto config = settings(directory, lapse::Mode::Desktop, separate); config.intervalMs = 60000;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    std::wstring temporary;
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.path().wstring().find(separate ? L"-desktop.recording.mp4" : L".recording.mp4") != std::wstring::npos) temporary = item.path().wstring();
    require(!temporary.empty(), "Cannot locate owned desktop temporary file");
    auto final = temporary; final.replace(final.rfind(L".recording.mp4"), 14, L".mp4");
    { std::ofstream sentinel(final, std::ios::binary); sentinel << "existing output"; require(bool(sentinel), "Cannot create owned collision fixture"); }
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 3000);
    require(saved.error && saved.recordingFailed && saved.frames == 1 && saved.savedPaths.size() == (separate ? 2u : 1u) && saved.savedPath == temporary,
        "Automatic completion hid a publication failure or discarded a completed stream");
    for (const auto& path : saved.savedPaths) verify(path, 1);
    std::ifstream sentinel(final, std::ios::binary); std::string text; std::getline(sentinel, text);
    require(text == "existing output", "Automatic publication replaced an existing file");
    require(saved.message.find(temporary) != std::wstring::npos, "Publication collision omitted the retained movie path");
    std::wcout << L"Collision outcome: " << saved.message << L'\n';
    require(saved.message.find(L"Recording time limit reached.") != std::wstring::npos,
        "Automatic save failure omitted its stop reason after retained movie decoded successfully");
    std::cout << "PASS automatic publication collision preserves movie(s) and stop reason; separate=" << separate << ".\n";
}
void singleFinalizationFailure(const std::filesystem::path& root) {
    const auto directory = root / L"finalization-single";
    auto config = settings(directory); config.intervalMs = 60000;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    std::wstring temporary;
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.path().wstring().find(L".recording.mp4") != std::wstring::npos) temporary = item.path().wstring();
    require(!temporary.empty(), "Cannot locate finalization fixture temporary file");
    finalizeFailure = true;
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 3000);
    require(saved.error && saved.recordingFailed && saved.frames == 1 && saved.savedPaths.empty() && saved.savedPath.empty(),
        "Finalization failure was reported as a published movie");
    require(!finalizeFailure && saved.message.find(L"Injected MP4 finalization failure.") != std::wstring::npos &&
        saved.message.find(temporary) != std::wstring::npos, "Finalization failure lost its diagnostic or partial path");
    // The seam reports failure after actual finalization, so this retained
    // fixture is decodable. Real failed finalization need not be playable.
    verify(temporary, 1);
    std::wcout << L"Finalization outcome: " << saved.message << L'\n';
    require(saved.message.find(L"Recording time limit reached.") != std::wstring::npos,
        "Automatic finalization failure omitted its stop reason after retained fixture decoded successfully");
    std::cout << "PASS single-file finalization error preserves stop reason, diagnostic and retained path.\n";
}
void previewFailureDeadline(const std::filesystem::path& root) {
    auto config = settings(root / L"preview-allocation"); config.intervalMs = 60000; config.preview = true; config.recordingLimitSeconds = 2;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    previewAllocationDelayMs = 800;
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 4000);
    verifySaved(saved, 1, 1);
    require(previewAllocationFaults == 1 && saved.elapsed < 2.5, "Preview allocation recovery delayed the recording limit by its full cooldown");
    std::cout << "PASS preview allocation cooldown respects the earlier recording deadline.\n";
}
void finalizationExcluded(const std::filesystem::path& root) {
    auto config = settings(root / L"slow-finalization"); config.intervalMs = 60000; config.preview = true;
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
void resetTerminalFaults() {
    captureFailureDelayMs = writeFailureDelayMs = writeFailureSide = 0;
    cameraOpenFailure = false; finalizeThrows = 0;
    firstAdmissionTick = failureObservedTick = 0; finalizeCalls = 0;
}
void delayedFailureElapsed(const std::filesystem::path& root, int variant) {
    resetTerminalFaults();
    const bool splitPair = variant == 1 || variant == 3;
    const bool writeFailure = variant >= 2;
    auto cfg = settings(root / (L"failure-elapsed-" + std::to_wstring(variant)), lapse::Mode::Desktop, splitPair);
    cfg.intervalMs = splitPair ? 1200 : 100; cfg.recordingLimitSeconds = variant == 0 ? 1 : 0;
    cfg.segmentDurationSeconds = splitPair ? 1 : 0;
    lapse::Engine engine; engine.configure(cfg); engine.record();
    await(engine, [](const auto& s) { return s.frames == 1; });
    if (writeFailure) { writeFailureSide = splitPair ? 2 : 1; writeFailureDelayMs = 600; }
    else captureFailureDelayMs = variant == 0 ? 1100 : 600;
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; }, 5000);
    require(result.recordingFailed && result.error && result.frames == 1 &&
        result.message.find(writeFailure ? L"Injected delayed writer failure." : L"Injected delayed capture failure.") != std::wstring::npos,
        "Elapsed correction changed the real failure or accepted frame count");
    require(firstAdmissionTick && failureObservedTick > firstAdmissionTick, "Delayed operation timing was not observed");
    const double expected = double(failureObservedTick - firstAdmissionTick) / 1000;
    require(result.elapsed >= expected - .08 && result.elapsed < expected + .15,
        "Terminal elapsed omitted the delayed failing operation or included saving");
    if (variant == 0) require(result.elapsed >= 1 && result.message.find(L"time limit reached") == std::wstring::npos,
        "Elapsed correction masked a real capture error with a new limit policy");
    require(result.completedSegments == (splitPair ? 1u : 0u), "Failure changed earlier completed-segment accounting");
    unsigned files = 0;
    for (const auto& item : std::filesystem::directory_iterator(cfg.folder)) if (item.path().extension() == L".mp4") {
        require(item.path().wstring().find(L".recording.mp4") == std::wstring::npos, "Delayed failure left an unfinished output");
        verify(item.path().wstring(), 1); ++files;
    }
    require(files == (variant == 3 ? 3u : splitPair ? 2u : 1u), "Delayed paired failure lost prior or accepted current output");
    std::cout << "PASS delayed capture/writer terminal elapsed variant=" << variant << ", observed=" << expected << ", reported=" << result.elapsed << ".\n";
}
void terminalClockControls(const std::filesystem::path& root) {
    resetTerminalFaults();
    auto cfg = settings(root / L"failed-startup-clock", lapse::Mode::Desktop, true);
    cameraOpenDelayMs = 600; cameraOpenFailure = true;
    { lapse::Engine engine; engine.configure(cfg); engine.record();
      const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
      require(result.recordingFailed && result.elapsed == 0 && result.frames == 0 && !firstAdmissionTick,
          "Failed initial writer preparation counted as active time"); }
    resetTerminalFaults(); cfg = settings(root / L"failed-first-admission-clock"); cfg.recordingLimitSeconds = 0;
    writeFailureSide = 1; writeFailureDelayMs = 600;
    { lapse::Engine engine; engine.configure(cfg); engine.record();
      const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
      const double expected = double(failureObservedTick - firstAdmissionTick) / 1000;
      require(result.recordingFailed && result.frames == 0 && result.savedPaths.empty() && expected >= .55 &&
          result.elapsed >= expected - .08 && result.elapsed < expected + .15,
          "A slow failed first admitted write was mistaken for pre-admission startup"); }
    resetTerminalFaults(); cfg = settings(root / L"paused-terminal-clock"); cfg.intervalMs = 60000; cfg.recordingLimitSeconds = 0;
    { lapse::Engine engine; engine.configure(cfg); engine.record(); await(engine, [](const auto& s) { return s.frames == 1; });
      std::this_thread::sleep_for(120ms); engine.setPaused(true);
      const auto paused = await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
      std::this_thread::sleep_for(400ms); finalizeDelayMs = 500; engine.finish();
      const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
      require(!result.error && result.elapsed == paused.elapsed, "Paused wait or terminal saving extended elapsed time"); verify(result.savedPath, 1); }
    resetTerminalFaults(); cfg = settings(root / L"terminal-retry-clock"); cfg.intervalMs = 100; cfg.recordingLimitSeconds = 0;
    { lapse::Engine engine; engine.configure(cfg); engine.record(); await(engine, [](const auto& s) { return s.frames == 1; });
      finalizeDelayMs = 700; finalizeThrows = 1;
      const auto requested = GetTickCount64(); engine.finish();
      const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
      const double cutoff = double(requested - firstAdmissionTick) / 1000;
      require(result.recordingFailed && finalizeCalls >= 2 && result.elapsed >= cutoff - .05 && result.elapsed < cutoff + .15 &&
          GetTickCount64() - requested >= 650, "Exception-driven close retry counted terminal finalization as active");
      verify(result.savedPath, result.frames);
      resetTerminalFaults(); engine.record(); await(engine, [](const auto& s) { return s.frames == 1; });
      writeFailureSide = 1; writeFailureDelayMs = 600;
      const auto second = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
      const double expected = double(failureObservedTick - firstAdmissionTick) / 1000;
      require(second.recordingFailed && second.elapsed >= expected - .08 && second.elapsed < expected + .15,
          "Next Record retained the previous session's terminal clock latch"); verify(second.savedPath, 1); }
    resetTerminalFaults(); cfg = settings(root / L"terminal-report-fallback"); cfg.intervalMs = 100; cfg.recordingLimitSeconds = 0;
    { lapse::Engine engine; engine.configure(cfg); engine.record(); await(engine, [](const auto& s) { return s.frames == 1; });
      finalizeDelayMs = 500; finalizeThrows = 2; captureFailureDelayMs = 600;
      const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
      const double expected = double(failureObservedTick - firstAdmissionTick) / 1000;
      require(result.recordingFailed && finalizeCalls >= 2 && finalizeThrows == 0 && result.savedPath.empty() &&
          result.elapsed >= expected - .08 && result.elapsed < expected + .15,
          "Persistent terminal-report failure lost the frozen active elapsed time");
      unsigned retained = 0;
      for (const auto& item : std::filesystem::directory_iterator(cfg.folder)) if (item.path().extension() == L".mp4") {
          verify(item.path().wstring(), 1); ++retained;
      }
      require(retained == 1, "Persistent reporting failure lost the previously accepted movie"); }
    std::cout << "PASS initial preparation, paused Finish, terminal retry, and next Record clock controls.\n";
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
bool CameraClient::start(const std::wstring& id, std::wstring& error, CameraResolution) {
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
    if (width == 480 && height == 360) if (const int delay = previewAllocationDelayMs.exchange(0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay)); ++previewAllocationFaults; throw std::bad_alloc();
    }
    if (const int delay = captureFailureDelayMs.exchange(0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        failureObservedTick = GetTickCount64(); error = L"Injected delayed capture failure."; return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(desktopDelayMs.exchange(0)));
    error.clear(); pixels(output, width, height); return true;
}
}
int main(int argc, char** argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-limit-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        const std::string focused = argc > 1 ? argv[1] : "";
        if (focused == "single-collision") collision(root, false);
        else if (focused == "single-finalization") singleFinalizationFailure(root);
        else if (focused == "elapsed") { for (int i = 0; i < 4; ++i) delayedFailureElapsed(root, i); terminalClockControls(root); }
        else {
        sourceMode(root, lapse::Mode::Desktop, false); sourceMode(root, lapse::Mode::Camera, false);
        sourceMode(root, lapse::Mode::Overlay, false); sourceMode(root, lapse::Mode::Desktop, true);
        startupExcluded(root, false); startupExcluded(root, true); pauseExcluded(root); slowCapture(root);
        unlimitedAndManualFinish(root, 0); unlimitedAndManualFinish(root, -1); collision(root);
        previewFailureDeadline(root); finalizationExcluded(root);
        collision(root, false); singleFinalizationFailure(root);
        for (int i = 0; i < 4; ++i) delayedFailureElapsed(root, i); terminalClockControls(root);
        }
        std::filesystem::remove_all(root);
        std::cout << "Recording limits: " << (focused.empty() ? "20 synthetic real-encoder case groups" : "focused outcome case") << " passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts kept at " << root.wstring() << L'\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

#include "engine_person_camera_stub.h"

// This fixture owns no native desktop capture surface.
namespace lapse { void releaseDesktopCaptureCache() noexcept {} }

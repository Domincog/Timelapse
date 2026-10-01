// Custom setting admission, scheduling and geometry with synthetic sources,
// inert power requests and real owned MP4 encoding/decoding. No devices open.
#include "engine.h"
#include "config.h"
#include "encoder.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
struct Write { uint64_t tick; int width, height; bool secondary; };
std::mutex observationsMutex;
std::vector<Write> writes;
std::atomic<unsigned> cameraStarts{0}, captures{0}, queries{0}, opens{0};
std::atomic<unsigned> previewCompositions{0};
std::atomic<int> slowWriteMs{0};
std::atomic<uint64_t> delayedWriteFinished{0};
std::atomic<bool> holdCapture{false}, captureEntered{false}, captureReleased{false}, captureTimedOut{false};
void reset() {
    std::lock_guard<std::mutex> lock(observationsMutex); writes.clear();
    cameraStarts = captures = queries = opens = previewCompositions = 0; slowWriteMs = 0; delayedWriteFinished = 0;
    holdCapture = captureEntered = captureReleased = captureTimedOut = false;
}
std::vector<Write> observations() { std::lock_guard<std::mutex> lock(observationsMutex); return writes; }
void pixels(lapse::Frame& frame, int width, int height, uint8_t shade) {
    frame.width = width; frame.height = height; frame.pixels.resize(size_t(width) * height * 4);
    for (size_t offset = 0; offset < frame.pixels.size(); offset += 4) {
        frame.pixels[offset] = frame.pixels[offset + 1] = frame.pixels[offset + 2] = shade;
        frame.pixels[offset + 3] = 255;
    }
}
}
namespace lapse {
bool customCompose(const Frame* desktop, const Frame* camera, const std::vector<Layer>& layers,
                   int width, int height, Frame& output, std::wstring& error) {
    if (width == 480 && height == 360) ++previewCompositions;
    return compose(desktop, camera, layers, width, height, output, error);
}
class CustomEncoder {
    Encoder real_;
    bool secondary_ = false;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error, EncodingQuality q, EncodingMode mode, bool recoveryMode) {
        ++opens; secondary_ = path.find(L"-camera.recording.mp4") != std::wstring::npos;
        return real_.open(path, width, height, fps, error, q, mode, recoveryMode);
    }
    bool write(const Frame& frame, std::wstring& error) {
        { std::lock_guard<std::mutex> lock(observationsMutex); writes.push_back({GetTickCount64(), frame.width, frame.height, secondary_}); }
        const int delay = slowWriteMs.exchange(0);
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        const bool result = real_.write(frame, error);
        if (delay) delayedWriteFinished = GetTickCount64();
        return result;
    }
    bool finish(std::wstring& error) { return real_.finish(error); }
    bool finishForPublication(std::wstring& error) { return real_.finishForPublication(error); }
    DWORD publish(const std::wstring& path) { return real_.publish(path); }
    void releasePublication() noexcept { real_.releasePublication(); }
    bool emptyOutputDiscarded() const noexcept { return real_.emptyOutputDiscarded(); }
    uint64_t frames() const { return real_.frames(); }
};
}
EXECUTION_STATE WINAPI customExecutionState(EXECUTION_STATE) { return ES_CONTINUOUS; }
BOOL WINAPI customDiskSpace(LPCWSTR, PULARGE_INTEGER available, PULARGE_INTEGER, PULARGE_INTEGER) {
    ++queries; available->QuadPart = 1024ULL * 1024 * 1024; return TRUE;
}
#define Encoder CustomEncoder
#define SetThreadExecutionState customExecutionState
#define GetDiskFreeSpaceExW customDiskSpace
#define compose customCompose
#include "../src/engine.cpp"
#undef compose
#undef GetDiskFreeSpaceExW
#undef SetThreadExecutionState
#undef Encoder

namespace {
using namespace lapse;
using Microsoft::WRL::ComPtr;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }
template<class Predicate> Status await(Engine& engine, Predicate predicate, int timeoutMs = 3500) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        const auto value = engine.status(); if (predicate(value)) return value;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < until);
    std::wcerr << L"Last custom-settings status: " << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for custom-settings contract");
}
Settings settings(const std::filesystem::path& folder) {
    Settings result; result.monitorId = L"custom-desktop"; result.cameraId = L"custom-camera";
    result.width = 320; result.height = 240; result.intervalMs = 60000;
    result.preview = false; result.folder = folder.wstring(); return result;
}
Status finish(Engine& engine) {
    engine.finish(); return await(engine, [](const auto& value) { return value.state == State::Idle; });
}
void decode(const Status& result, int width, int height, unsigned count, bool failure = false,
            int fps = DefaultOutputFps, bool recovery = false) {
    require(result.error == failure && result.recordingFailed == failure && result.savedPaths.size() == count && result.frames > 0,
        "Custom recording did not save the expected outputs");
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    for (const auto& path : result.savedPaths) {
        ComPtr<IMFSourceReader> reader; checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Cannot open custom MP4");
        ComPtr<IMFMediaType> type; checked(reader->GetCurrentMediaType(stream, &type), "Cannot inspect custom MP4");
        UINT32 actualWidth = 0, actualHeight = 0;
        checked(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &actualWidth, &actualHeight), "Missing custom geometry");
        require(actualWidth == UINT32(width) && actualHeight == UINT32(height), "Encoded custom dimensions changed");
        UINT32 rate = 0, denominator = 0;
        checked(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &rate, &denominator), "Missing playback rate");
        // The Windows reader reports 10000000/169491 for 59fps, deriving
        // the rate from a quantized 100ns frame duration. Bound that duration
        // to one tick rather than requiring an identical rational spelling.
        const uint64_t reported = uint64_t(denominator) * 10000000 * fps;
        const uint64_t expected = uint64_t(rate) * 10000000;
        const uint64_t difference = reported > expected ? reported - expected : expected - reported;
        require(rate && denominator && difference <= uint64_t(rate) * fps,
            "Encoded playback FPS exceeds native metadata quantization");
        if (!recovery) {
            PROPVARIANT duration{};
            checked(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&duration), "Missing MP4 duration");
            const bool valid = duration.vt == VT_UI8 &&
                std::llabs(int64_t(duration.uhVal.QuadPart) - int64_t(result.frames) * 10000000 / fps) < 20000;
            PropVariantClear(&duration);
            require(valid, "MP4 duration does not match admitted frames / frozen playback FPS");
        }
        checked(MFCreateMediaType(&type), "Cannot create decoded type");
        checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Cannot select video");
        checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Cannot select decoded pixels");
        checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Cannot decode custom MP4");
        uint64_t samples = 0; bool ended = false;
        for (uint64_t attempt = 0; attempt < result.frames + 100; ++attempt) {
            DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
            checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Cannot read custom MP4");
            require(!(flags & MF_SOURCE_READERF_ERROR), "Custom MP4 stream failed");
            if (sample) {
                const int64_t tolerance = recovery ? (fps == DefaultOutputFps ? 334 : 1000) : 1;
                require(samples < result.frames && std::llabs(timestamp - LONGLONG(samples) * 10000000 / fps) <= tolerance,
                    "Custom capture changed configured playback timestamps");
                if (!recovery) {
                    LONGLONG duration = 0;
                    checked(sample->GetSampleDuration(&duration), "Decoded playback sample has no duration");
                    const auto expectedDuration = LONGLONG(samples + 1) * 10000000 / fps - LONGLONG(samples) * 10000000 / fps;
                    require(std::llabs(duration - expectedDuration) <= 1, "Playback sample duration accumulated rounding drift");
                }
                ++samples;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
        }
        require(ended && samples == result.frames, "Custom MP4 frame count changed");
    }
}
void invalidAdmission(const std::filesystem::path& root) {
    std::vector<Settings> cases;
    for (int interval : {-1, 0, 99, MaxCaptureIntervalMs + 1, INT_MAX}) {
        auto value = settings(L""); value.intervalMs = interval; cases.push_back(value);
    }
    for (int fps : {INT_MIN,-1,0,MaxOutputFps+1,INT_MAX}) {
        auto value = settings(L""); value.outputFps = fps; cases.push_back(value);
    }
    for (const auto size : {std::pair<int, int>{0, 240}, {46, 240}, {49, 240}, {320, 47}, {4098, 48}, {4096, 2162}}) {
        auto value = settings(L""); value.width = size.first; value.height = size.second; cases.push_back(value);
    }
    for (const auto duration : {0, 1000}) {
        auto value = settings(L""); value.intervalMs = 999; value.night.enabled = true; value.night.durationMs = duration; cases.push_back(value);
    }
    auto tooLong = settings(L""); tooLong.intervalMs = 1499; tooLong.night.enabled = true; tooLong.night.durationMs = 1500; cases.push_back(tooLong);
    unsigned index = 0;
    for (auto value : cases) {
        reset(); value.folder = (root / (L"invalid-" + std::to_wstring(index++))).wstring(); value.separateFiles = true;
        Engine engine; engine.configure(value); engine.record();
        const auto result = await(engine, [](const auto& status) { return status.state == State::Idle; });
        require(result.recordingFailed && result.error && !result.message.empty() && result.savedPaths.empty(), "Invalid custom value was accepted");
        require(cameraStarts == 0 && captures == 0 && queries == 0 && opens == 0 && !std::filesystem::exists(value.folder),
            "Invalid custom value reached a source, query or output file");
    }
    std::cout << "PASS invalid custom interval/geometry/Night settings fail before source and output work.\n";
}
void fractionalFrozen(const std::filesystem::path& root) {
    reset(); auto config = settings(root / L"fractional-portrait"); config.intervalMs = 137;
    config.width = 240; config.height = 320; config.preview = true;
    Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& value) { return value.frames >= 1 && value.preview; });
    config.intervalMs = 1000; config.width = 384; config.height = 384; engine.configure(config);
    const auto running = await(engine, [](const auto& value) { return value.frames >= 5 && value.preview; }, 1800);
    require(running.preview->width == 270 && running.preview->height == 360 && running.preview->pixels.capacity() <= 640 * 360 * 4,
        "Active portrait preview did not retain its bounded frozen aspect");
    const auto saved = finish(engine); const auto observed = observations();
    require(observed.size() >= 5 && observed[4].tick - observed[0].tick >= 450 && observed[4].tick - observed[0].tick < 1400,
        "Exact 137ms interval was rounded, changed live, or burst ahead");
    for (const auto& write : observed) require(write.width == 240 && write.height == 320, "Active output dimensions changed live");
    decode(saved, 240, 320, 1);
    std::cout << "PASS exact fractional cadence, frozen interval/portrait size and bounded matching preview.\n";
}
void boundaryGeometry(const std::filesystem::path& root) {
    for (const bool separate : {false, true}) {
        reset(); auto config = settings(root / (separate ? L"square-pair" : L"minimum-size"));
        config.width = config.height = separate ? 384 : MinVideoDimension; config.separateFiles = separate; config.preview = true;
        Engine engine; engine.configure(config); engine.record();
        const auto running = await(engine, [](const auto& value) { return value.frames == 1 && value.preview; });
        require(running.preview->width == 360 && running.preview->height == 360, "Square output has a widescreen preview");
        decode(finish(engine), config.width, config.height, separate ? 2 : 1);
    }
    std::cout << "PASS minimum 48px and square paired output preserve exact encoded dimensions.\n";
}
void playbackRates(const std::filesystem::path& root) {
    for (int fps : {MinOutputFps,24,59,60,MaxOutputFps}) for (bool recovery : {false,true}) {
        reset(); auto config = settings(root / (L"fps-" + std::to_wstring(fps) + (recovery ? L"-recovery" : L"")));
        config.outputFps = fps; config.intervalMs = 137; config.separateFiles = true; config.recoveryMode = recovery;
        Engine engine; engine.configure(config); engine.record();
        await(engine, [](const auto& value) { return value.frames >= 1; });
        auto changed = config; changed.outputFps = fps == 60 ? 24 : 60; engine.configure(changed);
        await(engine, [](const auto& value) { return value.frames >= 5; }, 2200);
        decode(finish(engine), config.width, config.height, 2, false, fps, recovery);
        const auto observed = observations();
        require(observed.size() >= 10 && observed[8].tick - observed[0].tick >= 400,
            "Playback FPS shortened the independent capture cadence");
    }
    std::cout << "PASS 1/24/59/60/120 FPS are encoded and frozen in paired ordinary/recovery files with independent capture cadence.\n";
}
void slowWork(const std::filesystem::path& root) {
    reset(); auto config = settings(root / L"slow-work"); config.intervalMs = MinCaptureIntervalMs;
    // Stored Night options do not constrain a desktop-only 100ms recording.
    config.night.enabled = true; config.night.durationMs = 123;
    Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& value) { return value.frames == 1; }); slowWriteMs = 350;
    await(engine, [](const auto& value) { return value.frames >= 5; });
    const auto saved = finish(engine); const auto observed = observations();
    require(delayedWriteFinished != 0 && observed.size() >= 5 && observed[2].tick >= delayedWriteFinished,
        "Slow work fixture did not delay frame submission");
    require(observed[3].tick - observed[2].tick >= 60 && observed[4].tick - observed[3].tick >= 60,
        "Elapsed capture slots caused a catch-up burst");
    require(observed[4].tick - observed[0].tick >= 600 && !saved.nightEnabled,
        "Slow encoder work shortened the schedule or activated desktop Night");
    decode(saved, 320, 240, 1);
    std::cout << "PASS 100ms admission skips slow-work slots without catch-up or desktop-only Night constraints.\n";
}
void liveNightValidation(const std::filesystem::path& root) {
    reset(); auto config = settings(root / L"live-night-validation"); config.intervalMs = 500;
    config.night.enabled = true; config.night.durationMs = 0;
    Engine engine; engine.configure(config); engine.record();
    const auto initial = await(engine, [](const auto& value) { return value.frames == 1; });
    config.layers = preset(Mode::Camera); engine.configure(config);
    const auto saved = await(engine, [](const auto& value) { return value.state == State::Idle; });
    require(saved.frames == initial.frames && cameraStarts == 0 && saved.message.find(L"at least 1 second") != std::wstring::npos,
        "Adding a camera live bypassed the frozen subsecond Night restriction");
    decode(saved, 320, 240, 1, true);
    std::cout << "PASS live camera addition revalidates frozen Night cadence before camera activation and preserves saved frames.\n";
}
void previewCadence(const std::filesystem::path& root) {
    for (const bool visible : {true, false}) {
        reset(); auto config = settings(root / (visible ? L"fast-visible" : L"fast-hidden"));
        config.intervalMs = MinCaptureIntervalMs; config.preview = visible;
        Engine engine; engine.configure(config); engine.record();
        await(engine, [](const auto& value) { return value.frames >= 14; }, 2500);
        const auto compositions = previewCompositions.load();
        require(visible ? (compositions >= 2 && compositions <= 3) : compositions == 0,
            "100ms capture starved preview, composed it for every video frame, or composed while hidden");
        const auto saved = finish(engine); decode(saved, 320, 240, 1);
    }
    std::cout << "PASS 100ms capture keeps visible preview near 1Hz and hidden preview composition at zero.\n";
}
void longInterval(const std::filesystem::path& root) {
    reset(); auto config = settings(root / L"day-interval"); config.intervalMs = MaxCaptureIntervalMs; config.recordingLimitSeconds = INT_MAX;
    Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& value) { return value.frames == 1; });
    engine.setPaused(true); await(engine, [](const auto& value) { return value.state == State::Paused; }, 700);
    std::this_thread::sleep_for(120ms); require(engine.status().frames == 1, "Paused 24h session captured another sample");
    engine.setPaused(false); await(engine, [](const auto& value) { return value.frames == 2; }, 700);
    decode(finish(engine), 320, 240, 1);
    reset(); config.folder = (root / L"day-interval-limit").wstring(); config.recordingLimitSeconds = 1;
    Engine limited; limited.configure(config); limited.record();
    const auto saved = await(limited, [](const auto& value) { return value.state == State::Idle; }, 3000);
    require(saved.frames == 1 && saved.message.find(L"time limit") != std::wstring::npos && saved.elapsed >= 1,
        "Custom stop deadline slept until the 24h capture interval");
    decode(saved, 320, 240, 1);
    std::cout << "PASS 24h interval and INT_MAX stop duration remain responsive; finite limit wins between samples.\n";
}
void idleAspectRetirement() {
    reset(); auto config = settings(L""); config.preview = true;
    Engine engine; engine.configure(config);
    struct Release { ~Release() { captureReleased = true; } } release;
    await(engine, [](const auto& value) { return value.preview && value.preview->width == 480; });
    holdCapture = true;
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (!captureEntered && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    require(captureEntered, "Old aspect preview did not reach its capture gate");
    config.width = config.height = 384; engine.configure(config);
    require(!engine.status().preview, "Idle size change did not retire the previous aspect");
    captureReleased = true;
    await(engine, [](const auto& value) {
        require(!value.preview || (value.preview->width == 360 && value.preview->height == 360), "Retired in-flight aspect was republished");
        return bool(value.preview);
    });
    require(!captureTimedOut, "Preview fixture gate timed out");
    std::cout << "PASS idle aspect change retires both published and in-flight old preview.\n";
}
void playbackAcceptance(const std::filesystem::path& root) {
    reset(); auto config = settings(root / L"fps-acceptance"); config.preview = true; config.outputFps = 24;
    Engine engine; engine.configure(config);
    struct Release { ~Release() { captureReleased = true; } } release;
    await(engine, [](const auto& value) { return bool(value.preview); });
    holdCapture = true;
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (!captureEntered && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    require(captureEntered, "Idle capture did not reach the acceptance barrier");
    engine.record(); require(engine.status().state == State::Starting, "Record request was not accepted at the barrier");
    auto changed = config; changed.outputFps = 120; engine.configure(changed);
    captureReleased = true;
    await(engine, [](const auto& value) { return value.frames == 1; });
    decode(finish(engine), config.width, config.height, 1, false, 24);
    require(!captureTimedOut, "Playback acceptance barrier timed out");
    std::cout << "PASS Record freezes playback FPS before worker preparation even when an idle capture delays startup.\n";
}
}
namespace lapse {
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) {
    error = L"Unexpected activity observer in an Off-mode fixture."; return false;
}
void CameraClient::cancelActivityObservation() noexcept {}
struct CameraClient::Impl { bool active = false; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring& id, std::wstring& error, CameraResolution) {
    error.clear(); if (id != L"custom-camera") { error = L"Unknown synthetic camera."; return false; }
    ++cameraStarts; impl_->active = true; return true;
}
void CameraClient::stop() { impl_->active = false; }
bool CameraClient::latest(Frame& output, std::wstring& error) {
    error.clear(); if (!impl_->active) { error = L"Synthetic camera is stopped."; return false; }
    pixels(output, 320, 240, 160); return true;
}
bool CameraClient::beginNight(uint64_t, uint32_t, const NightSettings&, std::wstring& error) {
    error = L"Unexpected synthetic Night start."; return false;
}
bool CameraClient::nightResult(uint64_t, Frame&, NightWindowResult&, std::wstring& error) {
    error = L"Unexpected synthetic Night read."; return false;
}
void CameraClient::cancelNight() noexcept {}
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    error.clear(); if (id != L"custom-desktop") { error = L"Unknown synthetic desktop."; return false; }
    ++captures;
    if (holdCapture.exchange(false)) {
        captureEntered = true; const auto until = std::chrono::steady_clock::now() + 2s;
        while (!captureReleased && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
        if (!captureReleased) captureTimedOut = true;
    }
    pixels(output, width, height, 80); return true;
}
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-custom-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        invalidAdmission(root); fractionalFrozen(root); boundaryGeometry(root); playbackRates(root); slowWork(root); liveNightValidation(root);
        previewCadence(root); longInterval(root); idleAspectRetirement(); playbackAcceptance(root);
        std::filesystem::remove_all(root); std::cout << "All synthetic custom settings engine contracts passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts retained at " << root.wstring() << L'\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

#include "engine_person_camera_stub.h"

// This fixture owns no native desktop capture surface.
namespace lapse { void releaseDesktopCaptureCache() noexcept {} }

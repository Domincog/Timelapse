// Real generated MP4s, synthetic sources only: no display, camera, or user file.
#include "engine.h"
#include "encoder.h"
#include "capture.h"
#include "camera_host.h"
#include "engine_segment_decode.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
struct Part { std::wstring temporary; std::vector<unsigned> grey; };
std::mutex journalMutex;
std::vector<Part> journal;
std::atomic<unsigned> desktopCalls{}, cameraCalls{}, openCalls{};
std::atomic<int> captureDelay{}, writeDelay{}, finalizeDelay{}, openDelay{};
std::atomic<unsigned> delayedOpen{};
void pixels(lapse::Frame& frame, int width, int height, unsigned grey) {
    frame.width = width; frame.height = height; frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < frame.pixels.size(); i += 4) {
        frame.pixels[i] = frame.pixels[i + 1] = frame.pixels[i + 2] = static_cast<uint8_t>(grey);
        frame.pixels[i + 3] = 255;
    }
}
}
namespace lapse {
class SegmentEncoder {
    Encoder real_;
    size_t index_ = 0;
public:
    bool open(const std::wstring& path, int w, int h, int fps, std::wstring& error,
              EncodingQuality quality, EncodingMode mode, bool recovery) {
        const auto call = ++openCalls;
        if (call == delayedOpen) std::this_thread::sleep_for(std::chrono::milliseconds(openDelay.exchange(0)));
        if (!real_.open(path, w, h, fps, error, quality, mode, recovery)) return false;
        std::lock_guard<std::mutex> lock(journalMutex);
        index_ = journal.size(); journal.push_back({path, {}}); return true;
    }
    bool write(const Frame& frame, std::wstring& error) {
        std::this_thread::sleep_for(std::chrono::milliseconds(writeDelay.exchange(0)));
        const bool ok = real_.write(frame, error);
        if (ok) { std::lock_guard<std::mutex> lock(journalMutex); journal[index_].grey.push_back(frame.pixels[0]); }
        return ok;
    }
    bool finish(std::wstring& error) { return real_.finish(error); }
    bool finishForPublication(std::wstring& error) {
        std::this_thread::sleep_for(std::chrono::milliseconds(finalizeDelay.exchange(0)));
        return real_.finishForPublication(error);
    }
    DWORD publish(const std::wstring& path) { return real_.publish(path); }
    void releasePublication() noexcept { real_.releasePublication(); }
    bool emptyOutputDiscarded() const noexcept { return real_.emptyOutputDiscarded(); }
    uint64_t frames() const { return real_.frames(); }
};
}
EXECUTION_STATE WINAPI segmentPower(EXECUTION_STATE) { return ES_CONTINUOUS; }
#define Encoder SegmentEncoder
#define SetThreadExecutionState segmentPower
#include "../src/engine.cpp"
#undef SetThreadExecutionState
#undef Encoder

namespace {
using split_test::require;
template<class Predicate> lapse::Status await(lapse::Engine& engine, Predicate predicate, int ms = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        auto result = engine.status();
        if (predicate(result)) return result;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < end);
    std::wcerr << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for split recording");
}
lapse::Settings settings(const std::filesystem::path& root, int interval = 100, int limit = 3) {
    { std::lock_guard<std::mutex> lock(journalMutex); journal.clear(); }
    desktopCalls = cameraCalls = openCalls = 0;
    captureDelay = writeDelay = finalizeDelay = openDelay = 0; delayedOpen = 0;
    lapse::Settings cfg;
    cfg.monitorId = L"generated-desktop"; cfg.cameraId = L"generated-camera";
    cfg.width = 320; cfg.height = 240; cfg.preview = false; cfg.folder = root.wstring();
    cfg.intervalMs = interval; cfg.recordingLimitSeconds = limit; cfg.segmentDurationSeconds = 1;
    return cfg;
}
void verifyPixels(const std::wstring& path, const std::vector<unsigned>& expected, bool recovery) {
    using Microsoft::WRL::ComPtr;
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    require(SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)), "Cannot open generated segment");
    ComPtr<IMFMediaType> type;
    require(SUCCEEDED(MFCreateMediaType(&type)) && SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
        SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) &&
        SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, type.Get())), "Cannot set segment decoder");
    size_t index = 0; bool ended = false;
    for (size_t attempt = 0; attempt < expected.size() + 100; ++attempt) {
        ComPtr<IMFSample> sample; DWORD flags = 0; LONGLONG timestamp = 0;
        require(SUCCEEDED(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample)) &&
            !(flags & MF_SOURCE_READERF_ERROR), "Generated segment decode failed");
        if (sample) {
            require(index < expected.size() && std::llabs(timestamp - static_cast<LONGLONG>(index) * 10000000 / 30) <= (recovery ? 334 : 1),
                "Segment timestamp did not restart from zero");
            ComPtr<IMFMediaBuffer> buffer;
            require(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)), "Decoded sample has no pixels");
            BYTE* bytes = nullptr; DWORD length = 0;
            require(SUCCEEDED(buffer->Lock(&bytes, nullptr, &length)), "Cannot inspect decoded pixels");
            const int actual = length ? bytes[0] : -999; buffer->Unlock();
            const int luma = 16 + (219 * static_cast<int>(expected[index]) + 127) / 255;
            require(length >= 320 * 240 * 3 / 2 && std::abs(actual - luma) <= 5, "Segment lost generated pixel order/content");
            ++index;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && index == expected.size(), "Segment changed accepted frame count");
}
void verify(const lapse::Settings& cfg, const lapse::Status& saved) {
    split_test::verify(cfg.folder, saved, cfg.separateFiles, cfg.recoveryMode);
    std::lock_guard<std::mutex> lock(journalMutex);
    size_t nonempty = 0;
    for (const auto& part : journal) {
        auto path = part.temporary;
        if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4); // MF's URL reader uses the ordinary local spelling.
        const auto extension = path.rfind(L".recording.mp4"); require(extension != std::wstring::npos, "Unexpected encoder pathname");
        if (part.grey.empty()) { require(!std::filesystem::exists(path), "Empty prospective part was retained"); continue; }
        path.replace(extension, 14, L".mp4");
        verifyPixels(path, part.grey, cfg.recoveryMode); ++nonempty;
    }
    require(nonempty == saved.completedSegments * (cfg.separateFiles ? 2u : 1u), "Published set count disagrees with writer journal");
}
void regular(const std::filesystem::path& root, lapse::Mode mode, bool paired, bool recovery) {
    auto cfg = settings(root); cfg.layers = lapse::preset(mode); cfg.separateFiles = paired; cfg.recoveryMode = recovery;
    lapse::Engine engine; engine.configure(cfg); engine.record();
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    verify(cfg, saved);
    require(saved.completedSegments == 3 && saved.frames >= 20 && saved.elapsed >= 3 && saved.elapsed < 4,
        "Regular splitting changed the overall deadline or reset cumulative frames");
    require(saved.message.find(L"Recording time limit reached.") != std::wstring::npos, "Terminal split lost time-limit context");
    std::cout << "PASS generated regular/composited/paired/recovery segments.\n";
}
void sparse(const std::filesystem::path& root) {
    auto cfg = settings(root, 3500, 5);
    lapse::Engine engine; engine.configure(cfg); engine.record();
    await(engine, [](const auto& s) { return s.completedSegments == 1; });
    require(engine.status().state == lapse::State::Recording && desktopCalls == 1 && openCalls == 1,
        "Sparse boundary captured/opened a new empty part or ended the session");
    auto changed = cfg; changed.segmentDurationSeconds = 0; changed.folder += L"-changed"; engine.configure(changed);
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    verify(cfg, saved);
    require(saved.completedSegments == 2 && saved.frames == 2 && desktopCalls == 2 && openCalls == 2,
        "Sparse gaps created empty windows or changed frozen split settings");
    require(saved.savedPath.find(L"-part-000002.mp4") != std::wstring::npos && !std::filesystem::exists(changed.folder),
        "Sparse ordinal followed elapsed buckets or changed output directory");
    std::cout << "PASS sparse active boundaries, lazy writers, skipped windows, frozen folder and duration.\n";
}
void pause(const std::filesystem::path& root) {
    auto cfg = settings(root, 3000, 2);
    lapse::Engine engine; engine.configure(cfg); engine.record();
    await(engine, [](const auto& s) { return s.frames == 1; });
    std::this_thread::sleep_for(200ms); engine.setPaused(true);
    const auto paused = await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    std::this_thread::sleep_for(1200ms);
    require(engine.status().elapsed == paused.elapsed && !engine.status().completedSegments, "Paused wall time split a file");
    engine.setPaused(false);
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    verify(cfg, saved);
    require(saved.frames == 2 && saved.completedSegments == 1 && openCalls == 1, "Resume created a new split clock or empty successor");
    std::cout << "PASS pause exclusion and Finish with no open writer.\n";
}
void exact(const std::filesystem::path& root) {
    auto cfg = settings(root, 1000, 2);
    lapse::Engine engine; engine.configure(cfg); engine.record();
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    verify(cfg, saved);
    require(saved.frames == 2 && saved.completedSegments == 2 && openCalls == 2, "Equal stop/split/capture boundary admitted a third sample");
    std::cout << "PASS half-open admission windows and terminal-limit precedence.\n";
}
void slow(const std::filesystem::path& root, int variant) {
    auto cfg = settings(root, variant == 2 ? 1200 : 60000, variant == 0 ? 3 : 2);
    if (variant == 0) cfg.intervalMs = 600;
    if (variant == 3) { cfg.separateFiles = true; writeDelay = 1300; }
    if (variant == 2) { delayedOpen = 2; openDelay = 1200; }
    lapse::Engine engine; engine.configure(cfg); engine.record();
    await(engine, [](const auto& s) { return s.frames >= 1; });
    if (variant == 0) captureDelay = 1500;
    if (variant == 1) finalizeDelay = 1200;
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    verify(cfg, saved);
    if (variant == 0) {
        require(saved.completedSegments == 2, "Prepared late capture was backdated into the retired part");
        std::lock_guard<std::mutex> lock(journalMutex); require(journal.front().grey.size() == 1, "Late frame stayed in the first part");
    } else {
        require(saved.completedSegments == 1 && saved.frames == 1, "Slow automatic save/open/write extended the session or split a pair");
        if (variant == 1 || variant == 2) require(saved.elapsed >= 2.1, "Automatic save/open time was excluded from active time");
        if (variant == 2) require(openCalls == 2, "Slow successor-open control was not exercised");
    }
    std::cout << "PASS slow capture/save/open/paired-write boundary variant " << variant << ".\n";
}
void validationAndOff(const std::filesystem::path& root) {
    auto cfg = settings(root / L"negative"); cfg.segmentDurationSeconds = -1;
    { lapse::Engine engine; engine.configure(cfg); engine.record();
      const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
      require(result.recordingFailed && !desktopCalls && !cameraCalls && !openCalls && !std::filesystem::exists(cfg.folder),
          "Negative splitting was not rejected before source/file work"); }
    cfg = settings(root / L"off", 60000, 1); cfg.segmentDurationSeconds = 0;
    { lapse::Engine engine; engine.configure(cfg); engine.record();
      const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
      require(!saved.error && saved.frames == 1 && saved.completedSegments == 0 && saved.savedPath.find(L"-part-") == std::wstring::npos,
          "Default Off changed its filename or segment status");
      require(split_test::decodedFrames(saved.savedPath, 1) == 1, "Off output did not decode"); }
    require(lapse::segmentFrameTotal(UINT64_MAX - 2, 2) == UINT64_MAX, "Checked frame total rejected its endpoint");
    bool overflow = false; try { (void)lapse::segmentFrameTotal(UINT64_MAX, 1); } catch (const std::overflow_error&) { overflow = true; }
    require(overflow, "Frame count wrapped");
    require(lapse::nextSegmentCut(std::chrono::seconds(INT_MAX), 1) == std::chrono::seconds(int64_t(INT_MAX) + 1),
        "Large sparse time failed constant-time bucket arithmetic");
    overflow = false; try { (void)lapse::nextSegmentCut(lapse::Clock::duration::max(), 1); } catch (const std::overflow_error&) { overflow = true; }
    require(overflow, "Segment deadline wrapped");
    std::cout << "PASS validation, Off compatibility, checked counters and large clock gaps.\n";
}
}
namespace lapse {
struct CameraClient::Impl { bool active = false; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring&, std::wstring& error, CameraResolution) { error.clear(); impl_->active = true; return true; }
void CameraClient::stop() { impl_->active = false; }
bool CameraClient::latest(Frame& frame, std::wstring& error) {
    error.clear(); if (!impl_->active) return false;
    pixels(frame, 320, 240, 160 + (++cameraCalls % 20) * 2); return true;
}
bool CameraClient::beginNight(uint64_t, uint32_t, const NightSettings&, std::wstring& error) { error = L"Unexpected Night request"; return false; }
bool CameraClient::nightResult(uint64_t, Frame&, NightWindowResult&, std::wstring& error) { error = L"Unexpected Night result"; return false; }
void CameraClient::cancelNight() noexcept {}
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) { error = L"Unexpected observation"; return false; }
void CameraClient::cancelActivityObservation() noexcept {}
bool captureMonitor(const std::wstring&, int width, int height, bool, Frame& frame, std::wstring& error) {
    std::this_thread::sleep_for(std::chrono::milliseconds(captureDelay.exchange(0)));
    error.clear(); pixels(frame, width, height, 36 + (++desktopCalls % 40) * 4); return true;
}
void releaseDesktopCaptureCache() noexcept {}
}
#include "engine_person_camera_stub.h"
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-segments-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        validationAndOff(root);
        regular(root / L"single", lapse::Mode::Desktop, false, false);
        regular(root / L"overlay", lapse::Mode::Overlay, false, false);
        regular(root / L"paired-recovery", lapse::Mode::Desktop, true, true);
        sparse(root / L"sparse"); pause(root / L"pause"); exact(root / L"exact");
        for (int variant = 0; variant < 4; ++variant) slow(root / (L"slow-" + std::to_wstring(variant)), variant);
        std::filesystem::remove_all(root);
        std::cout << "All generated segment lifecycle cases passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts retained at " << root.wstring() << L'\n'; result = 1;
    }
    MFShutdown(); CoUninitialize(); return result;
}

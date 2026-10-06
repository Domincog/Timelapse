// Synthetic sources, actual compositor/watermark/MP4 writer, and bounded
// renderer fault seams. No camera, desktop capture or power request is used.
#include "engine.h"
#include "encoder.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
enum class Kind { Prepare, ApplyFull, ApplyPreview, Open, Write };
struct Event {
    Kind kind;
    lapse::WatermarkContext context{};
    lapse::WatermarkSettings settings{};
    uint64_t before = 0, after = 0;
    bool camera = false;
};
std::mutex journalMutex;
std::vector<Event> journal;
std::atomic<unsigned> fullCalls{0}, previewCalls{0}, prepareCalls{0}, opens{0}, cameraStarts{0}, cameraReads{0}, desktopReads{0};
std::atomic<unsigned> failFullAt{0};
std::atomic<unsigned> watermarkResets{0};
std::atomic<bool> failPrepare{false}, failPreview{false}, blockPreview{false}, previewEntered{false}, releasePreview{false};
std::atomic<bool> blockSecondRead{false}, secondReadEntered{false}, releaseSecondRead{false};
std::atomic<int> fullDelayMs{0};
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
uint64_t pixelsHash(const lapse::Frame& frame) {
    uint64_t result = 1469598103934665603ULL;
    for (const auto value : frame.pixels) { result ^= value; result *= 1099511628211ULL; }
    return result;
}
void add(Event event) { std::lock_guard<std::mutex> lock(journalMutex); journal.push_back(event); }
std::vector<Event> events(Kind kind) {
    std::lock_guard<std::mutex> lock(journalMutex);
    std::vector<Event> result;
    for (const auto& event : journal) if (event.kind == kind) result.push_back(event);
    return result;
}
bool sameContext(const lapse::WatermarkContext& a, const lapse::WatermarkContext& b) {
    return a.activeMs == b.activeMs && a.targetIntervalMs == b.targetIntervalMs &&
        a.outputFps == b.outputFps &&
        std::memcmp(&a.recordedLocal, &b.recordedLocal, sizeof(SYSTEMTIME)) == 0;
}
void waitFlag(const std::atomic<bool>& flag) {
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!flag) { require(std::chrono::steady_clock::now() < until, "Synthetic barrier timed out"); std::this_thread::sleep_for(1ms); }
}
void reset() {
    { std::lock_guard<std::mutex> lock(journalMutex); journal.clear(); }
    fullCalls = previewCalls = prepareCalls = opens = cameraStarts = cameraReads = desktopReads = 0;
    failFullAt = 0; failPrepare = failPreview = blockPreview = previewEntered = releasePreview = false;
    watermarkResets = 0;
    blockSecondRead = secondReadEntered = releaseSecondRead = false; fullDelayMs = 0;
}
struct ReleaseBarriers { ~ReleaseBarriers() { releasePreview = releaseSecondRead = true; } };
void solid(lapse::Frame& frame, int width, int height, uint8_t level) {
    frame.width = width; frame.height = height;
    frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < frame.pixels.size(); i += 4) {
        frame.pixels[i] = frame.pixels[i + 1] = frame.pixels[i + 2] = level; frame.pixels[i + 3] = 255;
    }
}
}
namespace lapse {
class ObservedWatermark {
    WatermarkRenderer real_;
    WatermarkSettings settings_;
    int width_ = 0, height_ = 0;
public:
    bool prepare(const WatermarkSettings& settings, int width, int height, std::wstring& error) {
        ++prepareCalls; add({Kind::Prepare, {}, settings});
        if (failPrepare) { error = L"Synthetic watermark preparation failure."; return false; }
        settings_ = settings; width_ = width; height_ = height;
        return real_.prepare(settings, width, height, error);
    }
    bool apply(Frame& frame, const WatermarkContext& context, std::wstring& error) {
        const bool full = frame.width == width_ && frame.height == height_;
        const unsigned ordinal = full ? ++fullCalls : ++previewCalls;
        if (!full && blockPreview.exchange(false)) { previewEntered = true; waitFlag(releasePreview); }
        if ((full && ordinal == failFullAt) || (!full && failPreview)) {
            error = L"Synthetic watermark rendering failure."; return false;
        }
        if (full) std::this_thread::sleep_for(std::chrono::milliseconds(fullDelayMs.exchange(0)));
        const auto before = pixelsHash(frame);
        if (!real_.apply(frame, context, error)) return false;
        add({full ? Kind::ApplyFull : Kind::ApplyPreview, context, settings_, before, pixelsHash(frame)});
        return true;
    }
    void reset() noexcept { real_.reset(); ++watermarkResets; }
};
class ObservedEncoder {
    Encoder real_;
    bool camera_ = false;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error,
              EncodingQuality quality, EncodingMode mode, bool recovery, const EncodingOptions& options = {}) {
        ++opens; camera_ = path.find(L"-camera.recording.mp4") != std::wstring::npos;
        add({Kind::Open, {}, {}, 0, 0, camera_});
        return real_.open(path, width, height, fps, error, quality, mode, recovery, options);
    }
    bool write(const Frame& frame, std::wstring& error) {
        add({Kind::Write, {}, {}, 0, pixelsHash(frame), camera_});
        return real_.write(frame, error);
    }
    bool finish(std::wstring& error) { return real_.finish(error); }
    bool finishForPublication(std::wstring& error) { return real_.finishForPublication(error); }
    DWORD publish(const std::wstring& path) { return real_.publish(path); }
    void releasePublication() noexcept { real_.releasePublication(); }
    bool emptyOutputDiscarded() const noexcept { return real_.emptyOutputDiscarded(); }
    uint64_t frames() const { return real_.frames(); }
};
}
EXECUTION_STATE WINAPI watermarkExecutionState(EXECUTION_STATE) { return ES_CONTINUOUS; }
#define WatermarkRenderer ObservedWatermark
#define Encoder ObservedEncoder
#define SetThreadExecutionState watermarkExecutionState
#include "../src/engine.cpp"
#undef SetThreadExecutionState
#undef Encoder
#undef WatermarkRenderer

namespace {
using namespace lapse;
template<class Predicate> Status await(Engine& engine, Predicate predicate, int timeoutMs = 6000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do { const auto status = engine.status(); if (predicate(status)) return status; std::this_thread::sleep_for(5ms); }
    while (std::chrono::steady_clock::now() < until);
    std::wcerr << engine.status().message << L'\n'; throw std::runtime_error("Watermark engine condition timed out");
}
Settings config(const std::filesystem::path& folder, bool pair = false) {
    Settings settings;
    settings.monitorId = L"synthetic-display"; settings.cameraId = L"synthetic-camera";
    settings.layers = preset(Mode::Desktop); settings.separateFiles = pair;
    settings.width = 640; settings.height = 480; settings.intervalMs = 200;
    settings.preview = false; settings.folder = folder.wstring(); settings.watermark.enabled = true;
    return settings;
}
Status finish(Engine& engine) {
    engine.finish(); return await(engine, [](const Status& status) { return status.state == State::Idle; });
}
void off(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"off", true); settings.watermark.enabled = false;
    Engine engine; engine.configure(settings); engine.record();
    await(engine, [](const Status& status) { return status.frames >= 2; });
    const auto result = finish(engine);
    require(!result.error && result.savedPaths.size() == 2 && !prepareCalls && !fullCalls && !previewCalls,
        "Disabled watermark did work or changed paired recording");
    Frame reference; solid(reference, 640, 480, 60);
    for (const auto& event : events(Kind::Write)) if (!event.camera) require(event.after == pixelsHash(reference), "Off altered desktop pixels");
    std::cout << "PASS Off performs no prepare/render and preserves pixels.\n";
}
// Watermark changes join a running recording. A change that cannot be
// prepared leaves the working watermark in place and is retried on the next
// configuration; turning it off stops stamping.
void liveChanges(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"live"); settings.watermark.enabled = false;
    Engine engine; engine.configure(settings); engine.record();
    await(engine, [](const Status& status) { return status.frames >= 2; });
    require(!prepareCalls && !fullCalls, "An Off watermark prepared or stamped before it was turned on");
    settings.watermark.enabled = true; engine.configure(settings);
    await(engine, [](const Status&) { return fullCalls >= 2; });
    failPrepare = true; auto refused = settings; refused.watermark.x = 0; engine.configure(refused);
    const auto stamped = fullCalls.load(); await(engine, [&](const Status&) { return fullCalls >= stamped + 3; });
    require(engine.status().state == State::Recording && !engine.status().error && events(Kind::ApplyFull).back().settings.x != 0,
        "A watermark change that could not be prepared stopped the recording or replaced the working one");
    failPrepare = false; engine.configure(refused);
    await(engine, [](const Status&) { const auto full = events(Kind::ApplyFull); return !full.empty() && full.back().settings.x == 0; });
    settings.watermark.enabled = false; engine.configure(settings);
    std::this_thread::sleep_for(450ms); const auto stopped = fullCalls.load(); const auto frames = engine.status().frames;
    await(engine, [&](const Status& status) { return status.frames >= frames + 2; });
    require(fullCalls == stopped, "A watermark turned off live kept stamping");
    const auto result = finish(engine);
    require(!result.error && result.savedPaths.size() == 1, "Live watermark changes failed the recording");
    std::cout << "PASS live watermark on, refused change kept the working one, retry and off.\n";
}
void paired(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"paired", true); settings.watermark.timeKind = WatermarkTimeKind::RecordedLocal;
    settings.outputFps = 24;
    Engine engine; engine.configure(settings); engine.record();
    await(engine, [](const Status& status) { return status.frames >= 2; });
    const auto result = finish(engine); require(!result.error && result.savedPaths.size() == 2, "Paired watermark recording failed");
    const auto renders = events(Kind::ApplyFull), writes = events(Kind::Write);
    require(renders.size() == writes.size() && renders.size() >= 4 && prepareCalls == 1, "Paired stamp count or preflight reuse wrong");
    require(renders.front().context.activeMs == 0 && renders.front().context.targetIntervalMs == settings.intervalMs &&
        renders.front().context.outputFps == settings.outputFps,
        "First frame includes startup time or wrong target");
    for (size_t i = 0; i < renders.size(); i += 2) {
        require(sameContext(renders[i].context, renders[i + 1].context), "Pair did not share immutable admission context");
        require(renders[i].context.recordedLocal.wYear >= 2026, "Local admission clock missing");
        require(renders[i].before != renders[i].after && renders[i + 1].before != renders[i + 1].after, "Actual renderer did not alter output");
        require(writes[i].after == renders[i].after && writes[i + 1].after == renders[i + 1].after, "Writer received unstamped or different pixels");
    }
    std::lock_guard<std::mutex> lock(journalMutex);
    require(journal.front().kind == Kind::Prepare, "Writer opened before watermark preflight");
    for (size_t i = 0; i < journal.size(); ++i) if (journal[i].kind == Kind::Write && !journal[i].camera)
        require(i >= 2 && journal[i - 1].kind == Kind::ApplyFull && journal[i - 2].kind == Kind::ApplyFull,
            "First writer accepted pixels before both renders finished");
    std::cout << "PASS one preflight and immutable paired admission/render ordering.\n";
}
void faults(const std::filesystem::path& root, unsigned selected) {
    reset(); auto settings = config(root / (L"fault-" + std::to_wstring(selected)), true);
    if (!selected) failPrepare = true; else failFullAt = selected;
    Engine engine; engine.configure(settings); engine.record();
    const auto result = await(engine, [](const Status& status) { return status.state == State::Idle; });
    require(result.error && result.recordingFailed && result.message.find(L"watermark") != std::wstring::npos, "Requested watermark failure was hidden");
    if (!selected) require(!opens && !std::filesystem::exists(settings.folder), "Failed preflight created writers/folder");
    else {
        const unsigned frames = selected == 4 ? 1 : 0;
        require(result.frames == frames && events(Kind::Write).size() == frames * 2, "Render failure admitted half or unstamped pair");
        size_t count = 0; for (const auto& ignored : std::filesystem::directory_iterator(settings.folder)) { (void)ignored; ++count; }
        require(count == frames * 2 && result.savedPaths.size() == frames * 2, "Render failure lost prior outputs or retained empty writers");
    }
    std::cout << "PASS preparation/paired render failure " << selected << " preserves prior work.\n";
}
void previewFailure(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"preview-fault"); settings.preview = true; failPreview = true;
    Engine engine; engine.configure(settings); engine.record();
    const auto failed = await(engine, [](const Status& status) { return status.frames >= 2 && status.error; });
    require(failed.state == State::Recording && !failed.recordingFailed && !failed.preview, "Disposable watermark preview failure stopped recording");
    failPreview = false;
    await(engine, [](const Status& status) { return status.preview && !status.error; });
    require(!finish(engine).error, "Recovered preview corrupted final outcome");
    std::cout << "PASS preview rendering failure remains disposable and recovers.\n";
}
void pausedSession(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"pause"); settings.preview = true;
    settings.outputFps = 59;
    Engine engine; engine.configure(settings); engine.record();
    await(engine, [](const Status& status) { return status.frames >= 2; });
    engine.setPaused(true); const auto paused = await(engine, [](const Status& status) { return status.state == State::Paused; });
    const auto last = events(Kind::ApplyFull).back(); const auto count = events(Kind::ApplyFull).size();
    settings.watermark.x = 0; settings.watermark.timeKind = WatermarkTimeKind::RecordedLocal; settings.watermark.showSpeed = false;
    settings.outputFps = 120;
    settings.intervalMs = 350;
    engine.configure(settings);
    await(engine, [&](const Status&) { const auto previews = events(Kind::ApplyPreview); return !previews.empty() && sameContext(previews.back().context, last.context); });
    std::this_thread::sleep_for(350ms);
    require(engine.status().elapsed == paused.elapsed && events(Kind::ApplyFull).size() == count, "Pause advanced watermark/session clock");
    // A live watermark edit shows at once over the paused frame's context.
    await(engine, [&](const Status&) { const auto previews = events(Kind::ApplyPreview); return !previews.empty() && previews.back().settings.x == 0; });
    const auto preview = events(Kind::ApplyPreview).back();
    require(sameContext(preview.context, last.context) && !preview.settings.showSpeed && preview.settings.timeKind == WatermarkTimeKind::RecordedLocal,
        "Paused preview changed its frozen context or missed the live watermark edit");
    const auto resets = watermarkResets.load(); settings.preview = false; engine.configure(settings);
    std::this_thread::sleep_for(100ms);
    require(watermarkResets == resets, "Hidden pause discarded its prepared recording renderer");
    engine.setPaused(false); await(engine, [&](const Status&) { return events(Kind::ApplyFull).size() > count; });
    const auto resumed = events(Kind::ApplyFull)[count];
    require(resumed.before != resumed.after && resumed.context.targetIntervalMs == 350 && resumed.context.outputFps == 59 &&
        resumed.context.activeMs - last.context.activeMs < 300 && resumed.settings.x == 0 && !resumed.settings.showSpeed,
        "Resume target, active time or live watermark edit is wrong");
    require(!finish(engine).error, "Pause/resume recording failed");
    std::cout << "PASS paused context stays fixed; live watermark edit shows at once; resume stamps the edited interval at frozen playback FPS.\n";
}
void manualRate(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"manual"); settings.intervalMs = 100;
    settings.outputFps = 24;
    settings.timeSkip.mode = TimeSkipMode::Manual; settings.timeSkip.rangeCount = 1;
    settings.timeSkip.ranges[0] = {0, 60}; settings.timeSkip.multiplier = 4; settings.timeSkip.rampFrames = 15;
    Engine engine; engine.configure(settings); engine.record();
    await(engine, [](const Status& status) { return status.frames >= 9; }); require(!finish(engine).error, "Manual recording failed");
    TimeSkipController reference; require(reference.reset(settings.timeSkip, 100, 1, settings.outputFps), "Reference controller invalid");
    int64_t incoming = 100; bool accelerated = false;
    for (const auto& event : events(Kind::ApplyFull)) {
        require(event.context.targetIntervalMs == incoming, "Watermark displays following interval instead of incoming target");
        accelerated |= incoming > 100; reference.inspect(event.context.activeMs); incoming = reference.onFrame(event.context.activeMs);
    }
    require(accelerated, "Ramp case did not exercise changing intervals");
    std::cout << "PASS target uses incoming ramp interval, not next-frame policy.\n";
}
void rollover(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"segments", true); settings.segmentDurationSeconds = 1; settings.recordingLimitSeconds = 2;
    Engine engine; engine.configure(settings); engine.record();
    const auto result = await(engine, [](const Status& status) { return status.state == State::Idle; });
    require(!result.error && result.completedSegments == 2 && opens == 4 && prepareCalls == 1, "Rollover reset watermark resources/session");
    int64_t previous = -1;
    for (const auto& event : events(Kind::ApplyFull)) { require(event.context.activeMs >= previous, "Elapsed watermark reset at split"); previous = event.context.activeMs; }
    require(previous >= 1000, "No second segment watermark exercised");
    std::cout << "PASS split parts retain cumulative time and one prepared renderer.\n";
}
void night(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"night", true); settings.layers = preset(Mode::Camera);
    settings.intervalMs = 1000; settings.night.enabled = true; settings.night.durationMs = 1000; settings.preview = true;
    Engine engine; engine.configure(settings); engine.record();
    await(engine, [](const Status& status) { return status.frames >= 1 && status.preview; });
    const auto result = finish(engine); require(!result.error && result.frames == 1, "Night watermark recording failed");
    const auto renders = events(Kind::ApplyFull); Frame processed; solid(processed, 640, 480, 180);
    require(renders.size() == 2 && renders[1].before == pixelsHash(processed), "Watermark stamped raw instead of completed Night pixels");
    require(sameContext(renders[0].context, renders[1].context), "Night pair contexts differ");
    std::cout << "PASS Night stamps completed exposure and preserves raw inputs.\n";
}
void slowRender(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"slow-render", true); settings.recordingLimitSeconds = 1; fullDelayMs = 1100;
    Engine engine; engine.configure(settings); engine.record();
    const auto result = await(engine, [](const Status& status) { return status.state == State::Idle; });
    require(!result.error && result.frames == 1 && events(Kind::Write).size() == 2, "Deadline split a previously admitted rendered pair");
    std::cout << "PASS admitted pair completes when rendering crosses time limit.\n";
}
void generation(const std::filesystem::path& root, bool oldFailure) {
    reset(); auto settings = config(root / (oldFailure ? L"generation-error" : L"generation"));
    settings.layers = preset(Mode::Camera); settings.preview = true;
    blockPreview = true; blockSecondRead = true; failPreview = oldFailure;
    Engine engine; ReleaseBarriers release; engine.configure(settings);
    waitFlag(previewEntered); const auto input = engine.status().cameraInput;
    require(input.width == 320 && input.generation, "Delivered camera metadata missing before overlay edit");
    settings.watermark.x = 0; engine.configure(settings); releasePreview = true;
    waitFlag(secondReadEntered);
    const auto stale = engine.status();
    require(!stale.preview && !stale.error && stale.cameraInput.generation == input.generation && stale.cameraInput.width == input.width && cameraStarts == 1,
        "Overlay edit published stale result/error or retired camera provenance");
    failPreview = false; releaseSecondRead = true;
    await(engine, [](const Status& status) { return status.preview && !status.error; });
    const auto latest = events(Kind::ApplyPreview).back();
    require(latest.settings.x == 0 && latest.context.activeMs == 0 && latest.context.targetIntervalMs == settings.intervalMs && cameraStarts == 1,
        "Idle preview did not use new illustrative watermark without camera restart");
    std::cout << "PASS overlay generation rejects old " << (oldFailure ? "error" : "pixels") << " without camera provenance/retry changes.\n";
}
void cacheRetirement(const std::filesystem::path& root, bool hidden) {
    reset(); auto settings = config(root / (hidden ? L"idle-hide" : L"idle-off")); settings.preview = true;
    if (!hidden) settings.layers = preset(Mode::Camera);
    Engine engine; engine.configure(settings);
    const auto initial = await(engine, [](const Status& status) { return status.preview != nullptr; });
    require(prepareCalls == 1, "Idle watermark was not prepared once");
    const auto resets = watermarkResets.load();
    if (hidden) settings.preview = false; else settings.watermark.enabled = false;
    engine.configure(settings);
    await(engine, [&](const Status&) { return watermarkResets > resets; });
    if (!hidden) require(engine.status().cameraInput.generation == initial.cameraInput.generation && cameraStarts == 1,
        "Disabling watermark retired camera metadata or retried capture");
    if (hidden) settings.preview = true; else settings.watermark.enabled = true;
    engine.configure(settings);
    const auto restored = await(engine, [](const Status& status) { return status.preview && prepareCalls >= 2; });
    require(prepareCalls == 2 && events(Kind::ApplyPreview).back().before != events(Kind::ApplyPreview).back().after,
        "Visible watermark did not reprepare after cache retirement");
    if (!hidden) require(restored.cameraInput.generation == initial.cameraInput.generation && cameraStarts == 1,
        "Reenabling watermark changed camera provenance or reopened it");
    std::cout << "PASS idle " << (hidden ? "hide" : "Off") << " releases cache and visible preview prepares again.\n";
}
}
namespace lapse {
struct CameraClient::Impl { bool active = false; uint64_t token = 0, begin = 0; uint32_t duration = 0; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring&, std::wstring& error, CameraResolution) { ++cameraStarts; impl_->active = true; error.clear(); return true; }
void CameraClient::stop() { impl_->active = false; }
bool CameraClient::latest(Frame& frame, std::wstring& error) {
    error.clear(); const auto ordinal = ++cameraReads;
    if (ordinal == 2 && blockSecondRead) { secondReadEntered = true; waitFlag(releaseSecondRead); }
    if (!impl_->active) { error = L"Synthetic camera stopped."; return false; }
    solid(frame, 320, 240, 100); return true;
}
bool CameraClient::beginNight(uint64_t token, uint32_t duration, const NightSettings&, std::wstring& error) {
    error.clear(); impl_->token = token; impl_->begin = GetTickCount64(); impl_->duration = duration; return true;
}
bool CameraClient::nightResult(uint64_t token, Frame& frame, NightWindowResult& result, std::wstring& error) {
    error.clear(); if (token != impl_->token || GetTickCount64() < impl_->begin + impl_->duration) return false;
    result = {}; result.beginTick = impl_->begin; result.endTick = impl_->begin + impl_->duration;
    result.exposure.samples = 5; result.exposure.suggestedDurationMs = 1000;
    solid(frame, 320, 240, 180); return true;
}
void CameraClient::cancelNight() noexcept { impl_->token = 0; }
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) { error = L"Unexpected camera observation."; return false; }
void CameraClient::cancelActivityObservation() noexcept {}
bool captureMonitor(const std::wstring&, int width, int height, bool, Frame& frame, std::wstring& error) {
    ++desktopReads; error.clear(); solid(frame, width, height, 60); return true;
}
void releaseDesktopCaptureCache() noexcept {}
}
#include "engine_person_camera_stub.h"

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-watermark-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        off(root); liveChanges(root); paired(root); faults(root, 0); faults(root, 2); faults(root, 4); previewFailure(root);
        pausedSession(root); manualRate(root); rollover(root); night(root); slowRender(root); generation(root, false); generation(root, true);
        cacheRetirement(root, true); cacheRetirement(root, false);
        std::filesystem::remove_all(root); std::cout << "Watermark engine: all 16 synthetic scenarios passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts retained at " << root.wstring() << L'\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

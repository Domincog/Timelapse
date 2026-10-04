// Synthetic sources, actual compositor/status renderer/MP4 writer and an
// observing renderer seam. No camera, desktop capture or power request is used.
#include "engine.h"
#include "encoder.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
struct Applied { bool full = false; uint64_t index = 0; std::wstring top, second; bool topStruck = false, secondStruck = false; uint64_t before = 0, after = 0; };
struct Written { uint64_t hash = 0; bool camera = false; };
std::mutex journalMutex;
std::vector<Applied> applied;
std::vector<Written> writtenFrames;
std::vector<lapse::StatusFeedSettings> preparedSettings;
std::atomic<unsigned> failApplyAt{0}, fullApplies{0}, cameraStarts{0};
std::atomic<int> outputWidth{640};
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
uint64_t pixelsHash(const lapse::Frame& frame) {
    uint64_t result = 1469598103934665603ULL;
    for (const auto value : frame.pixels) { result ^= value; result *= 1099511628211ULL; }
    return result;
}
void reset() {
    std::lock_guard<std::mutex> lock(journalMutex);
    applied.clear(); writtenFrames.clear(); preparedSettings.clear(); failApplyAt = 0; fullApplies = 0; cameraStarts = 0;
}
void solid(lapse::Frame& frame, int width, int height, uint8_t level) {
    frame.width = width; frame.height = height;
    frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < frame.pixels.size(); i += 4) {
        frame.pixels[i] = frame.pixels[i + 1] = frame.pixels[i + 2] = level; frame.pixels[i + 3] = 255;
    }
}
}
namespace lapse {
class ObservedStatus {
    StatusFeedRenderer real_;
public:
    bool prepare(const StatusFeedSettings& settings, int width, int height, std::wstring& error) {
        { std::lock_guard<std::mutex> lock(journalMutex); preparedSettings.push_back(settings); }
        return real_.prepare(settings, width, height, error);
    }
    bool prepared() const noexcept { return real_.prepared(); }
    bool fits() const noexcept { return real_.fits(); }
    bool apply(Frame& frame, const StatusFeed& feed, uint64_t index, std::wstring& error) {
        const bool full = frame.width == outputWidth;
        if (full && ++fullApplies == failApplyAt) { error = L"Synthetic status rendering failure."; return false; }
        Applied event; event.full = full; event.index = index; event.before = pixelsHash(frame);
        StatusFeed::Rows rows{}; const size_t count = feed.layout(index, rows);
        if (count) { event.top = rows[0].view.label.data(); event.topStruck = rows[0].strike > 0; }
        if (count > 1) { event.second = rows[1].view.label.data(); event.secondStruck = rows[1].strike > 0; }
        if (!real_.apply(frame, feed, index, error)) return false;
        event.after = pixelsHash(frame);
        std::lock_guard<std::mutex> lock(journalMutex); applied.push_back(event);
        return true;
    }
    void reset() noexcept { real_.reset(); }
    RECT lastBounds() const noexcept { return real_.lastBounds(); }
};
class ObservedEncoder {
    Encoder real_;
    bool camera_ = false;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error,
              EncodingQuality quality, EncodingMode mode, bool recovery) {
        camera_ = path.find(L"-camera.recording.mp4") != std::wstring::npos;
        return real_.open(path, width, height, fps, error, quality, mode, recovery);
    }
    bool write(const Frame& frame, std::wstring& error) {
        { std::lock_guard<std::mutex> lock(journalMutex); writtenFrames.push_back({pixelsHash(frame), camera_}); }
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
EXECUTION_STATE WINAPI statusExecutionState(EXECUTION_STATE) { return ES_CONTINUOUS; }
#define StatusFeedRenderer ObservedStatus
#define Encoder ObservedEncoder
#define SetThreadExecutionState statusExecutionState
#include "../src/engine.cpp"
#undef SetThreadExecutionState
#undef Encoder
#undef StatusFeedRenderer

namespace {
using namespace lapse;
template<class Predicate> Status await(Engine& engine, Predicate predicate, int timeoutMs = 8000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do { const auto status = engine.status(); if (predicate(status)) return status; std::this_thread::sleep_for(5ms); }
    while (std::chrono::steady_clock::now() < until);
    std::wcerr << engine.status().message << L'\n'; throw std::runtime_error("Status engine condition timed out");
}
template<class Predicate> void awaitJournal(Predicate predicate, int timeoutMs = 8000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do { { std::lock_guard<std::mutex> lock(journalMutex); if (predicate()) return; } std::this_thread::sleep_for(5ms); }
    while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error("Status journal condition timed out");
}
Settings config(const std::filesystem::path& folder, bool pair = false) {
    Settings settings;
    settings.monitorId = L"synthetic-display"; settings.cameraId = L"synthetic-camera";
    settings.layers = preset(Mode::Desktop); settings.separateFiles = pair;
    settings.width = 640; settings.height = 480; settings.intervalMs = 100;
    settings.preview = false; settings.folder = folder.wstring();
    return settings;
}
StatusItem item(uint64_t sequence, StatusKind kind, const wchar_t* text, int64_t duration = 0) {
    StatusItem value; value.sequence = sequence; value.kind = kind; setStatusText(value, text);
    value.startTick = GetTickCount64(); value.durationMs = duration; return value;
}
uint64_t plainHash() { Frame frame; solid(frame, 640, 480, 60); return pixelsHash(frame); }
Status finish(Engine& engine) {
    engine.finish(); return await(engine, [](const Status& status) { return status.state == State::Idle; });
}
size_t writes() { std::lock_guard<std::mutex> lock(journalMutex); return writtenFrames.size(); }
void untouched(const std::filesystem::path& root) {
    reset(); Engine engine; engine.configure(config(root / L"none")); engine.record();
    await(engine, [](const Status& status) { return status.frames >= 3; });
    const auto result = finish(engine);
    std::lock_guard<std::mutex> lock(journalMutex);
    require(!result.error && preparedSettings.empty() && applied.empty(), "No status still prepared or drew an overlay");
    for (const auto& event : writtenFrames) require(event.hash == plainHash(), "No status altered recorded pixels");
    std::cout << "PASS no status prepares nothing and leaves recordings unchanged.\n";
}
void liveChanges(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"live"); settings.statusFeed.corner = StatusCorner::BottomRight;
    Engine engine; engine.configure(settings); engine.record();
    await(engine, [](const Status& status) { return status.frames >= 2; });
    const size_t plain = writes();
    engine.setStatus(item(1, StatusKind::Stopwatch, L"Shower"));
    awaitJournal([] { size_t lit = 0; for (const auto& event : applied) lit += event.full && event.top == L"Shower" && event.after != event.before; return lit >= 3; });
    // Live appearance edits stay frozen for the session.
    settings.statusFeed.corner = StatusCorner::TopLeft; engine.configure(settings);
    StatusItem invalid; invalid.sequence = 9; invalid.kind = StatusKind::Note; engine.setStatus(invalid);
    engine.setStatus(item(2, StatusKind::Timer, L"Work on Essay", 3600000));
    awaitJournal([] { for (const auto& event : applied) if (event.full && event.top == L"Work on Essay" && event.second == L"Shower" && event.secondStruck) return true; return false; });
    engine.setStatus({});
    awaitJournal([] { return !applied.empty() && applied.back().top == L"Work on Essay" && applied.back().topStruck; });
    const auto result = finish(engine);
    std::lock_guard<std::mutex> lock(journalMutex);
    require(!result.error && !preparedSettings.empty(), "Live status recording failed");
    for (const auto& value : preparedSettings) require(value.corner == StatusCorner::BottomRight, "Live appearance edit reached a frozen session");
    for (size_t i = 0; i < plain && i < writtenFrames.size(); ++i) require(writtenFrames[i].hash == plainHash(), "Frames before the first status were altered");
    uint64_t previous = 0; bool first = true;
    for (const auto& event : applied) if (event.full) {
        require(first || event.index == previous + 1, "Status frames were not consecutive admitted frames");
        previous = event.index; first = false;
        require(event.top != L"" || event.after == event.before, "Feed without rows changed pixels");
    }
    size_t stamped = 0;
    for (const auto& event : writtenFrames) stamped += event.hash != plainHash();
    require(stamped >= 4, "Status overlay did not reach the writer");
    std::filesystem::path log;
    for (const auto& entry : std::filesystem::directory_iterator(settings.folder))
        if (entry.path().filename().wstring().find(L"-status.txt") != std::wstring::npos) log = entry.path();
    require(!log.empty(), "No status log was saved beside the video");
    std::ifstream input(log, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(bytes.rfind("\xEF\xBB\xBF", 0) == 0, "Status log is not UTF-8 with a byte order mark");
    const auto at = [&](const char* text) { return bytes.find(text); };
    require(at("0:00 Start") != std::string::npos && at("Shower (stopwatch)") != std::string::npos && at("Work on Essay (timer)") != std::string::npos &&
        at("(no status)") != std::string::npos && at("Shower") < at("Work on Essay") && at("Work on Essay") < at("(no status)"), "Status log lines are missing or out of order");
    std::cout << "PASS live status changes reach the next frames, strike the old line and keep frozen appearance.\n";
}
void bothFiles(const std::filesystem::path& root) {
    reset(); Engine engine; engine.configure(config(root / L"pair", true));
    engine.setStatus(item(3, StatusKind::Note, L"First task done!!"));
    engine.record();
    await(engine, [](const Status& status) { return status.frames >= 4; });
    const auto result = finish(engine);
    std::lock_guard<std::mutex> lock(journalMutex);
    require(!result.error && result.savedPaths.size() == 2, "Paired status recording failed");
    size_t shown = 0; std::vector<unsigned> perFrame;
    for (const auto& event : applied) if (event.full) {
        if (perFrame.size() <= event.index) perFrame.resize(size_t(event.index) + 1);
        ++perFrame[size_t(event.index)]; shown += event.top == L"First task done!!";
    }
    require(shown >= 6, "Paired outputs did not show the status");
    for (const auto count : perFrame) require(count == 2, "Each admitted frame must stamp both paired files");
    std::cout << "PASS a status set before Record appears in every output file.\n";
}
void failure(const std::filesystem::path& root) {
    reset(); failApplyAt = 3; Engine engine; engine.configure(config(root / L"fault"));
    engine.setStatus(item(4, StatusKind::Note, L"Breaks"));
    engine.record();
    const auto result = await(engine, [](const Status& status) { return status.state == State::Idle; });
    require(result.error && result.recordingFailed && result.message.find(L"Status overlay") != std::wstring::npos, "Status rendering failure was hidden");
    require(result.frames == 2 && writes() == 2, "Status failure lost prior frames or admitted an unstamped one");
    std::cout << "PASS a status rendering failure stops cleanly and keeps earlier frames.\n";
}
void preview(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"preview"); settings.preview = true;
    Engine engine; engine.configure(settings);
    await(engine, [](const Status& status) { return status.preview != nullptr; });
    { std::lock_guard<std::mutex> lock(journalMutex); require(applied.empty() && preparedSettings.empty(), "Idle preview drew a status before one was set"); }
    engine.setStatus(item(5, StatusKind::Timer, L"Plan", 600000));
    awaitJournal([] { for (const auto& event : applied) if (!event.full && event.top == L"Plan" && event.after != event.before) return true; return false; });
    engine.setStatus({});
    std::this_thread::sleep_for(1200ms);
    size_t before; { std::lock_guard<std::mutex> lock(journalMutex); before = applied.size(); }
    std::this_thread::sleep_for(1200ms);
    { std::lock_guard<std::mutex> lock(journalMutex); require(applied.size() == before, "Cleared idle status kept drawing on the preview"); }
    std::cout << "PASS idle preview shows a status at once without recording.\n";
}
void splitLogs(const std::filesystem::path& root) {
    reset(); auto settings = config(root / L"split"); settings.segmentDurationSeconds = 1;
    Engine engine; engine.configure(settings); engine.setStatus(item(6, StatusKind::Note, L"Planning"));
    engine.record();
    await(engine, [](const Status& status) { return status.completedSegments >= 2; }, 12000);
    finish(engine);
    size_t logs = 0, videos = 0;
    for (const auto& entry : std::filesystem::directory_iterator(settings.folder)) {
        const auto name = entry.path().filename().wstring();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, L".mp4") == 0) ++videos;
        if (name.find(L"-status.txt") == std::wstring::npos) continue;
        ++logs;
        require(name.find(L"-part-") != std::wstring::npos, "Split status log is not named after its part");
        std::ifstream input(entry.path(), std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(bytes.find("\r\n\r\n0:00 Planning\r\n") != std::string::npos && bytes.find("Start") == std::string::npos, "A part's log does not open with the current status at 0:00");
    }
    require(logs >= 2 && logs == videos, "Every saved part needs its own status log");
    std::cout << "PASS split parts each save a status log that restarts at 0:00.\n";
}
}
namespace lapse {
struct CameraClient::Impl { bool active = false; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring&, std::wstring& error, CameraResolution) { ++cameraStarts; impl_->active = true; error.clear(); return true; }
void CameraClient::stop() { impl_->active = false; }
bool CameraClient::latest(Frame& frame, std::wstring& error) {
    error.clear(); if (!impl_->active) { error = L"Synthetic camera stopped."; return false; }
    solid(frame, 320, 240, 100); return true;
}
bool CameraClient::beginNight(uint64_t, uint32_t, const NightSettings&, std::wstring& error) { error = L"Unexpected Night request."; return false; }
bool CameraClient::nightResult(uint64_t, Frame&, NightWindowResult&, std::wstring& error) { error = L"Unexpected Night result."; return false; }
void CameraClient::cancelNight() noexcept {}
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) { error = L"Unexpected camera observation."; return false; }
void CameraClient::cancelActivityObservation() noexcept {}
bool captureMonitor(const std::wstring&, int width, int height, bool, Frame& frame, std::wstring& error) {
    error.clear(); solid(frame, width, height, 60); return true;
}
void releaseDesktopCaptureCache() noexcept {}
}
#include "engine_person_camera_stub.h"

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-status-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        untouched(root); liveChanges(root); bothFiles(root); failure(root); preview(root); splitLogs(root);
        std::filesystem::remove_all(root); std::cout << "Status engine: all 6 synthetic scenarios passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts retained at " << root.wstring() << L'\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

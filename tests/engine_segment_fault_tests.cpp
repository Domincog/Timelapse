// Synthetic sources and an identity-guarded fake encoder. These cases test
// engine transaction/failure semantics; real media decoding belongs to the
// companion segment suite. No capture device, real app or installer is used.
#include "engine.h"
#include "encoder.h"
#include "capture.h"
#include "camera_host.h"
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
enum class Fault { None, Open, Write, AcceptedWrite, Finalize, Publish, ThrowPublish, ReportAllocation, CommitAllocation, OpenCollision, PublishCollision, EmptyCleanup, NextAllocation, DiskLow, DiskError };
enum class Operation { None, Open, Write, Finalize };
struct Record {
    std::wstring temporary, final;
    std::atomic<uint64_t> frames{0};
    std::atomic<bool> created{false}, finalized{false}, published{false}, emptyDeleted{false}, guarded{false}, cleanupFailed{false};
    int side = 0, part = 0, width = 0, height = 0;
    lapse::EncodingMode mode = lapse::EncodingMode::Compatible;
    bool recovery = false;
};
std::array<Record, 64> records;
std::atomic<unsigned> recordsUsed{0}, faultHits{0}, allocationsFailed{0}, publishCalls{0}, captures{0};
std::atomic<unsigned> openedBySide[2]{};
std::atomic<Fault> fault{Fault::None};
std::atomic<int> faultPart{2}, faultSide{1};
std::atomic<bool> allGuardsHeld{true}, allocationFreeCommit{false}, commitArmed{false};
thread_local int allocationCountdown = 0;
std::wstring collisionPath;
constexpr char sentinel[] = "Existing unrelated output must survive.";
void createCollision(const std::wstring& path) {
    collisionPath = path;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create owned collision sentinel");
    DWORD written = 0;
    const bool okay = WriteFile(file, sentinel, sizeof(sentinel), &written, nullptr) && written == sizeof(sentinel);
    CloseHandle(file);
    if (!okay) throw std::runtime_error("Cannot write owned collision sentinel");
}
struct Gate {
    std::atomic<Operation> operation{Operation::None};
    std::atomic<int> part{1}, side{0};
    std::atomic<bool> reached{false}, released{false}, timedOut{false};
    void enter(Operation at, int p, int s) {
        if (p != part || s != side) return;
        auto expected = at;
        if (!operation.compare_exchange_strong(expected, Operation::None)) return;
        reached = true;
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (!released) {
            if (std::chrono::steady_clock::now() >= until) { timedOut = true; break; }
            std::this_thread::sleep_for(1ms);
        }
    }
    void reset() { operation = Operation::None; reached = released = timedOut = false; part = 1; side = 0; }
} gate;
struct ReleaseGate { ~ReleaseGate() { gate.released = true; } };
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool hit(Fault at, int part, int side) {
    if (part != faultPart || side != faultSide) return false;
    auto expected = at;
    if (!fault.compare_exchange_strong(expected, Fault::None)) return false;
    ++faultHits; return true;
}
void reset(Fault selected = Fault::None, int part = 2, int side = 1) {
    allocationCountdown = 0; recordsUsed = faultHits = allocationsFailed = publishCalls = captures = 0;
    openedBySide[0] = openedBySide[1] = 0;
    fault = selected; faultPart = part; faultSide = side;
    allGuardsHeld = true; allocationFreeCommit = commitArmed = false; gate.reset();
    collisionPath.clear();
    for (auto& r : records) {
        r.temporary.clear(); r.final.clear(); r.frames = 0;
        r.created = r.finalized = r.published = r.emptyDeleted = r.guarded = r.cleanupFailed = false;
        r.part = r.side = r.width = r.height = 0;
    }
}
}
void* operator new(size_t bytes) {
    if (allocationCountdown > 0 && --allocationCountdown == 0) { ++allocationsFailed; throw std::bad_alloc(); }
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace lapse {
class SegmentFaultEncoder {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    Record* record_ = nullptr;
    bool writing_ = false, finished_ = false, finishOkay_ = false, publicationAttempted_ = false;
    DWORD publication_ = ERROR_INVALID_STATE;
    void release(bool empty) noexcept {
        if (file_ == INVALID_HANDLE_VALUE) return;
        if (empty) {
            FILE_DISPOSITION_INFO info{TRUE};
            const bool removed = SetFileInformationByHandle(file_, FileDispositionInfo, &info, sizeof(info)) != FALSE;
            if (record_) record_->emptyDeleted = removed;
        }
        CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
        if (record_) record_->guarded = false;
    }
    bool close(std::wstring& error, bool retain) {
        error.clear();
        if (!record_) return true;
        if (!finished_) {
            gate.enter(Operation::Finalize, record_->part, record_->side);
            writing_ = false; finished_ = true;
            finishOkay_ = record_->frames != 0 && !hit(Fault::Finalize, record_->part, record_->side);
            record_->finalized = finishOkay_;
        }
        if (!record_->frames) {
            if (hit(Fault::EmptyCleanup, record_->part, record_->side)) { record_->cleanupFailed = true; release(false); }
            else release(true);
            error = record_->cleanupFailed ? L"No frames recorded; injected owned-file cleanup failure." : L"No video frames were recorded.";
            return false;
        }
        if (!retain) release(false);
        if (!finishOkay_) error = L"Injected finalization failure.";
        if (retain && finishOkay_ && hit(Fault::ReportAllocation, record_->part, record_->side)) allocationCountdown = 1;
        return finishOkay_;
    }
public:
    ~SegmentFaultEncoder() { release(record_ && record_->frames == 0); }
    bool open(const std::wstring& path, int width, int height, int, std::wstring& error,
              EncodingQuality, EncodingMode mode, bool recovery) {
        error.clear();
        if (file_ != INVALID_HANDLE_VALUE || writing_) { error = L"Guard retained across reopen."; return false; }
        const int side = path.find(L"-camera.recording.mp4") == std::wstring::npos ? 0 : 1;
        const int part = static_cast<int>(++openedBySide[side]);
        const unsigned index = recordsUsed.fetch_add(1);
        require(index < records.size(), "Fixture record journal exceeded fixed capacity");
        record_ = &records[index]; record_->temporary = path;
        record_->side = side; record_->part = part; record_->width = width; record_->height = height;
        record_->mode = mode; record_->recovery = recovery;
        finished_ = finishOkay_ = publicationAttempted_ = false; publication_ = ERROR_INVALID_STATE;
        gate.enter(Operation::Open, part, side);
        if (hit(Fault::Open, part, side)) { error = L"Injected successor open failure."; return false; }
        if (hit(Fault::OpenCollision, part, side)) createCollision(path);
        file_ = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) { error = L"Fake output already exists or cannot be created."; return false; }
        record_->created = record_->guarded = true; writing_ = true;
        return true;
    }
    bool write(const Frame& frame, std::wstring& error) {
        error.clear(); require(record_ && writing_ && frame.valid(), "Invalid fake write");
        gate.enter(Operation::Write, record_->part, record_->side);
        if (hit(Fault::Write, record_->part, record_->side)) { error = L"Injected write failure."; return false; }
        const char value = static_cast<char>(record_->side + 1);
        DWORD written = 0;
        if (!WriteFile(file_, &value, 1, &written, nullptr) || written != 1) { error = L"Owned fixture write failed."; return false; }
        ++record_->frames;
        if (hit(Fault::AcceptedWrite, record_->part, record_->side)) { error = L"Injected marker failure after accepted frame."; return false; }
        return true;
    }
    bool finish(std::wstring& error) { return close(error, false); }
    bool finishForPublication(std::wstring& error) { return close(error, true); }
    DWORD publish(const std::wstring& destination) {
        if (publicationAttempted_) return publication_;
        require(record_ && finishOkay_ && file_ != INVALID_HANDLE_VALUE, "Invalid fake publication");
        ++publishCalls;
        for (unsigned n = 0; n < recordsUsed; ++n) if (records[n].part == record_->part && records[n].created && records[n].frames)
            allGuardsHeld = allGuardsHeld && records[n].guarded;
        if (hit(Fault::ThrowPublish, record_->part, record_->side)) throw std::bad_alloc();
        record_->final = destination;
        if (hit(Fault::PublishCollision, record_->part, record_->side)) createCollision(destination);
        if (hit(Fault::Publish, record_->part, record_->side)) { publicationAttempted_ = true; return publication_ = ERROR_ACCESS_DENIED; }
        const DWORD nameBytes = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
        std::vector<BYTE> storage(sizeof(FILE_RENAME_INFO) + nameBytes, 0);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->ReplaceIfExists = FALSE; rename->FileNameLength = nameBytes;
        std::memcpy(rename->FileName, destination.data(), nameBytes);
        publication_ = SetFileInformationByHandle(file_, FileRenameInfo, rename, static_cast<DWORD>(storage.size())) ? ERROR_SUCCESS : GetLastError();
        publicationAttempted_ = true;
        if (publication_ == ERROR_SUCCESS) {
            record_->published = true;
            if (hit(Fault::CommitAllocation, record_->part, record_->side)) { commitArmed = true; allocationCountdown = 1; }
        }
        return publication_;
    }
    void releasePublication() noexcept {
        if (commitArmed.exchange(false)) { allocationFreeCommit = allocationCountdown == 1; allocationCountdown = 0; }
        release(false);
        if (record_ && hit(Fault::NextAllocation, record_->part, record_->side)) allocationCountdown = 1;
    }
    uint64_t frames() const { return record_ ? record_->frames.load() : 0; }
};
}
BOOL WINAPI segmentDiskSpace(LPCWSTR, PULARGE_INTEGER available, PULARGE_INTEGER, PULARGE_INTEGER) {
    if (records[0].published && records[1].published) {
        if (recordsUsed == 2 && hit(Fault::DiskLow, 2, 1)) {
            if (available) available->QuadPart = 0;
            return TRUE;
        }
        if (recordsUsed == 4 && hit(Fault::DiskError, 2, 1)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    }
    if (available) available->QuadPart = 1024ULL * 1024 * 1024;
    return TRUE;
}
EXECUTION_STATE WINAPI segmentPower(EXECUTION_STATE) { return ES_CONTINUOUS; }
#define Encoder SegmentFaultEncoder
#define GetDiskFreeSpaceExW segmentDiskSpace
#define SetThreadExecutionState segmentPower
#include "../src/engine.cpp"
#undef SetThreadExecutionState
#undef GetDiskFreeSpaceExW
#undef Encoder

namespace {
template<class Predicate> lapse::Status await(lapse::Engine& engine, Predicate predicate, int timeoutMs = 6500) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        auto status = engine.status();
        require(status.savedPaths.size() <= 2, "Unbounded published-path history");
        if (predicate(status)) return status;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < until);
    std::wcerr << L"Last status: " << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for segment fault state");
}
lapse::Settings settings(const std::filesystem::path& directory, bool pair = true) {
    lapse::Settings result;
    result.monitorId = L"owned-desktop"; result.cameraId = L"owned-camera";
    result.width = 64; result.height = 48; result.intervalMs = 100;
    result.preview = false; result.folder = directory.wstring(); result.separateFiles = pair;
    result.segmentDurationSeconds = 1;
    return result;
}
void pixels(lapse::Frame& output, int width, int height) {
    output.width = width; output.height = height; output.pixels.assign(size_t(width) * height * 4, 96);
}
void verifyOutcomes(const lapse::Status& status, bool pair) {
    require(status.savedPaths.size() <= 2 && (status.savedPaths.empty() || status.savedPath == status.savedPaths.front()), "Latest path compatibility failed");
    uint64_t totals[2]{};
    for (unsigned n = 0; n < recordsUsed; ++n) {
        const auto& r = records[n]; totals[r.side] += r.frames;
        if (!r.created) continue;
        if (!r.frames && r.cleanupFailed) require(!r.emptyDeleted && std::filesystem::exists(r.temporary), "Failed empty cleanup lost its retained path");
        else if (!r.frames) require(r.emptyDeleted && !std::filesystem::exists(r.temporary), "Owned zero-frame successor was retained");
        else if (r.published) require(std::filesystem::exists(r.final) && !std::filesystem::exists(r.temporary), "Published path truth lost");
        else require(std::filesystem::exists(r.temporary), "Unpublished partial was lost");
    }
    require(status.frames == (pair ? std::min(totals[0], totals[1]) : totals[0]), "Session frame totals lost accepted frames at rollover/failure");
    for (const auto& path : status.savedPaths) require(std::filesystem::exists(path), "Status advertises a nonexistent path");
    require(allGuardsHeld, "A paired file guard was released before both publication attempts");
}
void pairedFault(const std::filesystem::path& root, Fault selected, int side, const wchar_t* label) {
    reset(selected, 2, side);
    lapse::Status saved;
    {
        lapse::Engine engine; ReleaseGate release; engine.configure(settings(root / label)); engine.record();
        await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
        saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
        require(saved.error && saved.recordingFailed && saved.completedSegments >= 1, "Failure lost earlier completed parts or was reported as success");
    }
    require(faultHits == 1, "Requested segmented fault was not injected once");
    verifyOutcomes(saved, true);
    if (selected == Fault::Open || selected == Fault::Write || selected == Fault::Finalize || selected == Fault::Publish || selected == Fault::ThrowPublish)
        require(saved.completedSegments == 1, "Incomplete pair counted as a completed segment set");
    if (selected == Fault::ThrowPublish) {
        require(records[2].published && !records[3].published, "Second publish throw did not preserve first rename");
        require(saved.savedPaths.size() == 2 && std::filesystem::equivalent(saved.savedPaths[0], records[2].final) && std::filesystem::equivalent(saved.savedPaths[1], records[3].temporary),
                "Throwing second publish lost exact first-final/second-temporary path outcomes");
    }
    if (selected == Fault::OpenCollision || selected == Fault::PublishCollision) {
        std::ifstream input(std::filesystem::path(collisionPath), std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        require(bytes == std::string(sentinel, sizeof(sentinel)) && saved.completedSegments == 1, "Unrelated collision output was replaced, deleted or counted as saved");
    }
    std::wcout << L"PASS " << label << L"; prior=" << saved.completedSegments << L", frames=" << saved.frames << L'\n';
}
void allocationCommit(const std::filesystem::path& root) {
    reset(Fault::CommitAllocation, 1, 1);
    lapse::Status saved;
    {
        auto cfg = settings(root / L"publication-commit"); cfg.intervalMs = 60000;
        lapse::Engine engine; ReleaseGate release; engine.configure(cfg); engine.record();
        await(engine, [](const auto& s) { return s.completedSegments == 1; });
        engine.finish(); saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    }
    require(faultHits == 1 && allocationFreeCommit && allocationsFailed == 0 && !saved.error && saved.completedSegments == 1,
            "Publication-to-status commit allocated or empty Finish forgot earlier success");
    verifyOutcomes(saved, true);
    require(recordsUsed == 2, "Sparse boundary created an empty successor");
    std::cout << "PASS guarded allocation-free publication commit and no empty successor.\n";
}
void commandDuringFinalize(const std::filesystem::path& root, bool pause) {
    reset(); gate.operation = Operation::Finalize;
    auto cfg = settings(root / (pause ? L"pause-during-finalize" : L"finish-during-finalize"));
    lapse::Status saved;
    {
        lapse::Engine engine; ReleaseGate release; engine.configure(cfg); engine.record();
        await(engine, [](const auto&) { return gate.reached.load(); });
        if (pause) engine.setPaused(true); else engine.finish();
        gate.released = true;
        if (pause) {
            const auto paused = await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
            const unsigned openings = recordsUsed;
            std::this_thread::sleep_for(150ms);
            require(engine.status().frames == paused.frames && recordsUsed == openings, "Pause admitted a successor frame");
            engine.finish();
        }
        saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    }
    require(!gate.timedOut && !saved.error && saved.completedSegments == 1 && recordsUsed == 2,
            "Rollover lost queued command or opened a successor before servicing it");
    verifyOutcomes(saved, true);
    std::cout << "PASS queued " << (pause ? "Pause" : "Finish") << " during native finalization.\n";
}
void frozenPaths(const std::filesystem::path& root) {
    reset(); auto cfg = settings(root / L"frozen-original"); cfg.intervalMs = 1500;
    lapse::Status saved;
    {
        lapse::Engine engine; ReleaseGate release; engine.configure(cfg); engine.record();
        await(engine, [](const auto& s) { return s.completedSegments == 1; });
        auto changed = cfg; changed.folder = (root / L"wrong-live-folder").wstring();
        changed.width = 128; changed.height = 96; changed.segmentDurationSeconds = 0;
        changed.encodingMode = lapse::EncodingMode::Efficient;
        engine.configure(changed);
        await(engine, [](const auto& s) { return s.completedSegments == 2; });
        engine.finish(); saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    }
    require(!saved.error && saved.completedSegments == 2 && recordsUsed == 4 && !std::filesystem::exists(root / L"wrong-live-folder"),
            "Live settings changed split cadence/output directory or created an empty successor");
    for (unsigned n = 0; n < recordsUsed; ++n) require(records[n].width == 64 && records[n].height == 48 && records[n].mode == lapse::EncodingMode::Compatible,
        "Successor did not retain frozen encoder settings");
    verifyOutcomes(saved, true);
    std::cout << "PASS frozen paths/geometry/mode/duration across successive parts.\n";
}
void commandDuringOpen(const std::filesystem::path& root, bool pause, int side) {
    reset(); gate.operation = Operation::Open; gate.part = 2; gate.side = side;
    auto cfg = settings(root / (std::wstring(pause ? L"pause-during-open-" : L"finish-during-open-") + std::to_wstring(side)));
    lapse::Status saved;
    {
        lapse::Engine engine; ReleaseGate release; engine.configure(cfg); engine.record();
        await(engine, [](const auto&) { return gate.reached.load(); });
        if (pause) engine.setPaused(true); else engine.finish();
        gate.released = true;
        if (pause) {
            await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
            require(records[2].frames == 0 && records[3].frames == 0, "Pause during successor open admitted an unrequested pair");
            engine.finish();
        }
        saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    }
    require(!gate.timedOut && !saved.error && saved.completedSegments == 1 && recordsUsed == unsigned(3 + side),
        "Slow successor open lost command or earlier published success");
    require(records[2].frames == 0 && records[3].frames == 0, "Finish during successor open admitted a new pair");
    verifyOutcomes(saved, true);
    std::cout << "PASS queued " << (pause ? "Pause" : "Finish") << " during successor open " << side << " and owned empty cleanup.\n";
}
void emptyCleanupFailure(const std::filesystem::path& root) {
    reset(Fault::EmptyCleanup, 2, 0); gate.operation = Operation::Open; gate.part = 2; gate.side = 1;
    lapse::Status saved;
    {
        lapse::Engine engine; ReleaseGate release; engine.configure(settings(root / L"empty-cleanup-failure")); engine.record();
        await(engine, [](const auto&) { return gate.reached.load(); });
        engine.finish(); gate.released = true;
        saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    }
    require(!gate.timedOut && faultHits == 1 && saved.error && saved.recordingFailed && saved.completedSegments == 1 &&
        saved.message.find(L"cleanup failure") != std::wstring::npos && saved.message.find(L"Partial file:") != std::wstring::npos,
        "Empty cleanup failure hid retained output or earlier completed set");
    verifyOutcomes(saved, true);
    std::cout << "PASS failed zero-frame owned cleanup reports retained partial and earlier success.\n";
}
void allocationAfterCommit(const std::filesystem::path& root) {
    reset(Fault::NextAllocation, 1, 1);
    lapse::Status saved;
    {
        lapse::Engine engine; ReleaseGate release; engine.configure(settings(root / L"allocation-after-commit")); engine.record();
        await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
        saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    }
    require(faultHits == 1 && allocationsFailed == 1 && saved.error && saved.recordingFailed && saved.completedSegments == 1 &&
        saved.savedPaths.size() == 2 && recordsUsed == 2, "Next allocation lost previously committed paths or admitted a successor");
    verifyOutcomes(saved, true);
    std::cout << "PASS allocation failure after released publication preserves both completed paths.\n";
}
void limitDuringFinalize(const std::filesystem::path& root) {
    reset(); gate.operation = Operation::Finalize;
    auto cfg = settings(root / L"limit-during-finalize"); cfg.recordingLimitSeconds = 2;
    lapse::Status saved;
    {
        lapse::Engine engine; ReleaseGate release; engine.configure(cfg); engine.record();
        await(engine, [](const auto&) { return gate.reached.load(); });
        std::this_thread::sleep_for(1200ms); gate.released = true;
        saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    }
    require(!gate.timedOut && !saved.error && saved.completedSegments == 1 && recordsUsed == 2 && saved.elapsed >= 2 &&
        saved.message.find(L"time limit reached") != std::wstring::npos, "Automatic finalization reset/paused the active deadline or opened a successor");
    verifyOutcomes(saved, true);
    std::cout << "PASS active Stop-after elapsed during slow automatic finalization.\n";
}
void checkedArithmetic() {
    require(lapse::segmentFrameTotal(UINT64_MAX - 7, 7) == UINT64_MAX, "Valid cumulative frame limit rejected");
    bool overflow = false;
    try { (void)lapse::segmentFrameTotal(UINT64_MAX - 7, 8); } catch (const std::overflow_error&) { overflow = true; }
    require(overflow, "Cumulative accepted-frame overflow wrapped");
    using Duration = lapse::Clock::duration;
    const auto seconds = [](int64_t value) { return std::chrono::duration_cast<Duration>(std::chrono::seconds(value)); };
    require(lapse::nextSegmentCut(seconds(0), 1) == seconds(1) && lapse::nextSegmentCut(seconds(1), 1) == seconds(2), "Half-open segment boundary rounding failed");
    // Two billion elapsed empty windows must be handled by division, not an
    // iteration per window or opening any output files.
    require(lapse::nextSegmentCut(seconds(2000000000), 1) == seconds(2000000001), "Large sparse segment jump failed");
    require(lapse::nextSegmentCut(seconds(0), INT_MAX) == seconds(INT_MAX), "Largest valid split duration rejected");
    overflow = false;
    try { (void)lapse::nextSegmentCut(Duration::max(), 1); } catch (const std::overflow_error&) { overflow = true; }
    require(overflow && recordsUsed == 0, "Next-boundary exhaustion wrapped or arithmetic opened a file");
    std::cout << "PASS checked totals, half-open boundary and constant-time large sparse jump.\n";
}
}
namespace lapse {
struct CameraClient::Impl { bool running = false; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring& id, std::wstring& error, CameraResolution) { error.clear(); impl_->running = id == L"owned-camera"; return impl_->running; }
void CameraClient::stop() { impl_->running = false; }
bool CameraClient::latest(Frame& output, std::wstring& error) { error.clear(); if (!impl_->running) return false; pixels(output, 64, 48); return true; }
bool CameraClient::beginNight(uint64_t, uint32_t, const NightSettings&, std::wstring& error) { error = L"Unexpected Night request."; return false; }
bool CameraClient::nightResult(uint64_t, Frame&, NightWindowResult&, std::wstring& error) { error = L"Unexpected Night result."; return false; }
void CameraClient::cancelNight() noexcept {}
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) { error = L"Unexpected observation."; return false; }
void CameraClient::cancelActivityObservation() noexcept {}
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    error.clear(); if (id != L"owned-desktop") return false; ++captures; pixels(output, width, height); return true;
}
void releaseDesktopCaptureCache() noexcept {}
}
#include "engine_person_camera_stub.h"
int main(int argc, char** argv) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const auto root = std::filesystem::current_path() / (L"engine-segment-fault-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        const std::string focused = argc > 1 ? argv[1] : "";
        if (focused.empty() || focused == "matrix") {
            pairedFault(root, Fault::Finalize, 0, L"first-finalize"); pairedFault(root, Fault::Finalize, 1, L"second-finalize");
            pairedFault(root, Fault::Publish, 0, L"first-publish"); pairedFault(root, Fault::Publish, 1, L"second-publish");
            pairedFault(root, Fault::ThrowPublish, 1, L"throw-second-publish");
            pairedFault(root, Fault::Open, 0, L"first-successor-open"); pairedFault(root, Fault::Open, 1, L"second-successor-open");
            pairedFault(root, Fault::Write, 0, L"first-successor-write"); pairedFault(root, Fault::Write, 1, L"second-successor-write");
            pairedFault(root, Fault::AcceptedWrite, 1, L"accepted-marker-failure");
            pairedFault(root, Fault::OpenCollision, 1, L"successor-existing-temporary");
            pairedFault(root, Fault::PublishCollision, 1, L"successor-existing-final");
            pairedFault(root, Fault::DiskLow, 1, L"successor-low-space-before-open");
            pairedFault(root, Fault::DiskError, 1, L"successor-space-error-before-write");
        }
        if (focused.empty() || focused == "allocation") { pairedFault(root, Fault::ReportAllocation, 1, L"report-allocation"); allocationCommit(root); allocationAfterCommit(root); }
        if (focused.empty() || focused == "commands") {
            commandDuringFinalize(root, false); commandDuringFinalize(root, true); frozenPaths(root);
            commandDuringOpen(root, false, 0); commandDuringOpen(root, true, 0);
            commandDuringOpen(root, false, 1); commandDuringOpen(root, true, 1); limitDuringFinalize(root); emptyCleanupFailure(root);
        }
        if (focused.empty() || focused == "bounds") { reset(); checkedArithmetic(); }
        std::filesystem::remove_all(root);
        std::cout << "Segment fault checks passed.\n";
    } catch (const std::exception& error) {
        gate.released = true; allocationCountdown = 0;
        std::cerr << "FAIL: " << error.what() << '\n'; std::wcerr << L"Artifacts: " << root.wstring() << L'\n'; result = 1;
    }
    CoUninitialize(); return result;
}

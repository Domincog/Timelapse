// Synthetic sources, inert power requests, and real MP4 encoding/decoding.
// Disk availability is injected at the Win32 boundary; this fixture never
// fills a volume, changes quotas, or activates a physical capture device.
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
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr ULONGLONG MiB = 1024ULL * 1024;
constexpr ULONGLONG reserve = 64 * MiB;
struct Query {
    std::wstring folder;
    ULONGLONG available;
    unsigned opened, primaryFrames, secondaryFrames;
    bool success, directoryExists, callerOutput;
};
std::mutex queriesMutex;
std::vector<Query> queries;
std::atomic<ULONGLONG> availableBytes{1024 * MiB};
std::atomic<bool> queryWorks{true}, orderingFailed{false}, paired{false}, expectQueries{true};
std::atomic<unsigned> queryCalls{0}, openedWriters{0}, primaryWrites{0}, secondaryWrites{0};
std::atomic<unsigned> primaryQuery{0}, cameraStarts{0}, cameraStops{0}, captures{0};
std::atomic<unsigned> queryGateAt{0};
enum class FinishFault { None, Primary, Secondary };
std::atomic<FinishFault> finishFault{FinishFault::None};

// Bounded gates make the race under test deterministic without leaving a
// worker hung when an assertion fails. Release guards cover exception paths.
struct Gate {
    std::atomic<bool> armed{false}, reached{false}, released{false}, timedOut{false};
    void reset() { armed = reached = released = timedOut = false; }
    void enter() {
        if (!armed.exchange(false)) return;
        reached = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!released) {
            if (std::chrono::steady_clock::now() >= until) { timedOut = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
} openGate, captureGate, queryGate;
struct ReleaseGates {
    ~ReleaseGates() { openGate.released = captureGate.released = queryGate.released = true; }
};
}

BOOL WINAPI spaceGetDiskFreeSpaceExW(LPCWSTR directory, PULARGE_INTEGER callerAvailable,
                                    PULARGE_INTEGER totalBytes, PULARGE_INTEGER totalFree) {
    const auto call = ++queryCalls;
    if (call == queryGateAt) queryGate.enter();
    const auto available = availableBytes.load();
    const bool success = queryWorks.load();
    const DWORD attributes = directory ? GetFileAttributesW(directory) : INVALID_FILE_ATTRIBUTES;
    {
        std::lock_guard<std::mutex> lock(queriesMutex);
        queries.push_back({directory ? directory : L"", available, openedWriters.load(),
            primaryWrites.load(), secondaryWrites.load(), success,
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            callerAvailable != nullptr});
    }
    // A quota-limited caller can have no available bytes on a volume with a
    // terabyte free. Supplying both values catches use of the wrong result.
    if (callerAvailable) callerAvailable->QuadPart = available;
    if (totalBytes) totalBytes->QuadPart = 2ULL * 1024 * 1024 * MiB;
    if (totalFree) totalFree->QuadPart = 1024ULL * 1024 * MiB;
    if (!success) SetLastError(ERROR_ACCESS_DENIED);
    return success ? TRUE : FALSE;
}

namespace lapse {
class SpaceEncoder {
    Encoder real_;
    bool secondary_ = false;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error,
              EncodingQuality quality, EncodingMode mode) {
        secondary_ = path.find(L"-camera.recording.mp4") != std::wstring::npos;
        if (expectQueries && !queryCalls) orderingFailed = true;
        const bool result = real_.open(path, width, height, fps, error, quality, mode);
        if (result) {
            ++openedWriters;
            if (!paired || secondary_) openGate.enter();
        }
        return result;
    }
    bool write(const Frame& frame, std::wstring& error) {
        const auto currentQuery = queryCalls.load();
        if (secondary_) {
            // Both writers must share one admission decision. A new query
            // between the writes would allow an unmatched desktop sample.
            if (expectQueries && (currentQuery != primaryQuery || primaryWrites != secondaryWrites + 1)) orderingFailed = true;
        } else {
            if (expectQueries && currentQuery <= primaryQuery) orderingFailed = true;
            primaryQuery = currentQuery;
        }
        const bool result = real_.write(frame, error);
        if (result) { if (secondary_) ++secondaryWrites; else ++primaryWrites; }
        return result;
    }
    bool finish(std::wstring& error) { return real_.finish(error); }
    bool finishForPublication(std::wstring& error) {
        const bool result = real_.finishForPublication(error);
        if (result && finishFault == (secondary_ ? FinishFault::Secondary : FinishFault::Primary)) {
            error = L"Injected finalization failure."; return false;
        }
        return result;
    }
    DWORD publish(const std::wstring& path) { return real_.publish(path); }
    void releasePublication() noexcept { real_.releasePublication(); }
    uint64_t frames() const { return real_.frames(); }
};
}
EXECUTION_STATE WINAPI spaceExecutionState(EXECUTION_STATE) { return ES_CONTINUOUS; }
#define GetDiskFreeSpaceExW spaceGetDiskFreeSpaceExW
#define SetThreadExecutionState spaceExecutionState
#define Encoder SpaceEncoder
#include "../src/engine.cpp"
#undef Encoder
#undef SetThreadExecutionState
#undef GetDiskFreeSpaceExW

namespace {
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }
template<class Predicate> lapse::Status await(lapse::Engine& engine, Predicate predicate, int milliseconds = 3500) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        const auto status = engine.status();
        if (predicate(status)) return status;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < until);
    std::wcerr << L"Last engine status: " << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for disk-space contract");
}
void awaitGate(Gate& gate) {
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (!gate.reached && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    require(gate.reached && !gate.timedOut, "Deterministic worker gate was not reached");
}
void reset(bool separate) {
    openGate.reset(); captureGate.reset(); queryGate.reset(); queryGateAt = 0;
    availableBytes = 1024 * MiB; queryWorks = true; orderingFailed = false; paired = separate; expectQueries = true;
    queryCalls = openedWriters = primaryWrites = secondaryWrites = primaryQuery = 0;
    cameraStarts = cameraStops = captures = 0; finishFault = FinishFault::None;
    std::lock_guard<std::mutex> lock(queriesMutex); queries.clear();
}
lapse::Settings settings(const std::filesystem::path& folder, bool separate = false,
                         lapse::Mode mode = lapse::Mode::Desktop) {
    lapse::Settings result;
    result.monitorId = L"synthetic-display"; result.cameraId = L"synthetic-camera";
    result.width = 320; result.height = 240; result.intervalMs = 60000;
    result.folder = folder.wstring(); result.preview = false;
    result.layers = lapse::preset(mode); result.separateFiles = separate;
    return result;
}
std::wstring expectedQueryPath(const std::wstring& folder) {
    const DWORD required = GetFullPathNameW(folder.c_str(), 0, nullptr, nullptr);
    require(required != 0, "Cannot resolve expected absolute save directory");
    std::wstring absolute(required, L'\0');
    const DWORD count = GetFullPathNameW(folder.c_str(), required, absolute.data(), nullptr);
    require(count != 0 && count < required, "Cannot normalize expected save directory");
    absolute.resize(count);
    auto result = lapse::fileIOPath(absolute);
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') result += L'\\';
    return result;
}
void checkQueries(const std::wstring& folder, unsigned expectedCount = 0) {
    std::lock_guard<std::mutex> lock(queriesMutex);
    require(!queries.empty() && (!expectedCount || queries.size() == expectedCount), "Missing or redundant disk-space queries");
    for (const auto& query : queries) {
        require(query.folder == expectedQueryPath(folder), "Space query did not use the session directory's Windows I/O path");
        require(!query.folder.empty() && (query.folder.back() == L'\\' || query.folder.back() == L'/'), "Space query directory lacks trailing slash");
        require(query.directoryExists, "Space queried before creating the destination directory");
        require(query.callerOutput, "Space query omitted the bytes available to the current caller");
    }
    require(queries.front().opened == 0 && queries.front().primaryFrames == 0 && queries.front().secondaryFrames == 0,
        "First space query came after opening or writing an output");
    require(!orderingFailed, "A writer opened or admitted a sample without the shared space check");
}
std::vector<std::filesystem::path> files(const std::wstring& folder) {
    const auto io = lapse::fileIOPath(folder);
    std::vector<std::filesystem::path> result;
    if (std::filesystem::exists(io)) for (const auto& entry : std::filesystem::directory_iterator(io))
        if (entry.is_regular_file()) result.push_back(entry.path());
    return result;
}
std::wstring temporary(const std::wstring& folder, bool secondary) {
    const auto suffix = secondary ? L"-camera.recording.mp4" : (paired ? L"-desktop.recording.mp4" : L".recording.mp4");
    for (const auto& path : files(folder)) if (path.filename().wstring().find(suffix) != std::wstring::npos)
        return (std::filesystem::path(folder) / path.filename()).wstring();
    throw std::runtime_error("Missing owned temporary movie");
}
std::wstring finalName(std::wstring path) {
    const auto suffix = path.rfind(L".recording.mp4"); require(suffix != std::wstring::npos, "Invalid temporary filename");
    path.replace(suffix, 14, L".mp4"); return path;
}
void verifyDecoded(const std::wstring& path, uint64_t expectedFrames) {
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    const auto io = lapse::fileIOPath(path);
    ComPtr<IMFByteStream> file;
    checked(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
        MF_FILEFLAGS_NONE, io.c_str(), &file), "Cannot reopen retained MP4");
    checked(MFCreateSourceReaderFromByteStream(file.Get(), nullptr, &reader), "Cannot read retained MP4");
    checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Cannot deselect streams");
    checked(reader->SetStreamSelection(stream, TRUE), "Cannot select recorded video");
    ComPtr<IMFMediaType> type; checked(reader->GetNativeMediaType(stream, 0, &type), "No recorded media type");
    UINT32 width = 0, height = 0; checked(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "No MP4 dimensions");
    require(width == 320 && height == 240, "MP4 dimensions differ from the session");
    type.Reset(); checked(MFCreateMediaType(&type), "Cannot create decoded type");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Cannot set decoded major type");
    checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Cannot set decoded pixel type");
    checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Cannot decode retained MP4");
    uint64_t frames = 0; bool ended = false;
    for (uint64_t attempt = 0; attempt < expectedFrames + 100; ++attempt) {
        DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
        checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Retained MP4 decode failed");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Retained MP4 has a stream error");
        if (sample) {
            require(frames < expectedFrames && std::llabs(timestamp - static_cast<LONGLONG>(frames) * 10000000 / 30) <= 1,
                "Retained MP4 has an extra frame or incorrect timestamp");
            ComPtr<IMFMediaBuffer> buffer; checked(sample->ConvertToContiguousBuffer(&buffer), "Decoded frame has no pixels");
            DWORD length = 0; checked(buffer->GetCurrentLength(&length), "Decoded frame size unavailable");
            require(length >= 320 * 240 * 3 / 2, "Decoded frame is incomplete"); ++frames;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && frames == expectedFrames, "Retained MP4 frame count differs from admitted samples");
}
void failed(const lapse::Status& status, const wchar_t* prefix, bool queryFailure, unsigned expectedFrames) {
    require(status.state == lapse::State::Idle && status.error && status.recordingFailed && status.frames == expectedFrames,
        "Space failure did not stop with an explicit failed recording status");
    require(status.message.rfind(prefix, 0) == 0, "Space stop lost its startup/active recording reason");
    require(status.message.find(queryFailure ? L"Cannot check available space" : L"needs more than") != std::wstring::npos,
        "Space failure did not distinguish a failed query from the reserve threshold");
    if (queryFailure) require(status.message.find(lapse::errorText(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))) != std::wstring::npos,
        "Failed query omitted the captured Win32 diagnostic");
    require(status.savedPaths.empty() ? status.savedPath.empty() : status.savedPath == status.savedPaths.front(),
        "First retained output path is inconsistent");
}
void saved(const lapse::Status& status, unsigned frames, size_t outputs) {
    require(status.state == lapse::State::Idle && !status.error && !status.recordingFailed && status.frames == frames,
        "Enough caller-available space prevented a normal recording");
    require(status.savedPaths.size() == outputs && status.savedPath == status.savedPaths.front(), "Normal save lost an output path");
    for (const auto& path : status.savedPaths) verifyDecoded(path, frames);
}
void startupRefusal(const std::filesystem::path& root, bool separate, ULONGLONG available, bool queryFailure = false) {
    reset(separate);
    auto config = settings(root / (L"startup-" + std::to_wstring(separate) + L"-" + std::to_wstring(available) +
        (queryFailure ? L"-query" : L"")), separate);
    availableBytes = available; queryWorks = !queryFailure;
    lapse::Engine engine; engine.configure(config); engine.record();
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    failed(result, L"Cannot start recording: ", queryFailure, 0);
    require(openedWriters == 0 && primaryWrites == 0 && secondaryWrites == 0 && result.savedPaths.empty() && files(config.folder).empty(),
        "Refused startup created a writer, empty MP4, or stale saved path");
    checkQueries(config.folder, 1);
    require(result.message.find(config.folder) != std::wstring::npos, "Startup space error omitted the selected folder");
    if (separate) require(cameraStops == cameraStarts, "Refused hidden startup retained its camera");
    if (!separate && available == 0 && !queryFailure) {
        availableBytes = 1024 * MiB; engine.record();
        await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
        engine.finish(); const auto retry = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
        saved(retry, 1, 1); checkQueries(config.folder, 3);
    }
    std::cout << "PASS startup separate=" << separate << " available=" << available << " query_failure=" << queryFailure << ".\n";
}
void sufficient(const std::filesystem::path& root, bool separate, lapse::Mode mode, bool large = false) {
    reset(separate); auto config = settings(root / (L"enough-" + std::to_wstring(separate) + L"-" + std::to_wstring(static_cast<int>(mode)) + (large ? L"-large" : L"")), separate, mode);
    availableBytes = large ? (1ULL << 32) : reserve * (separate ? 2 : 1) + 1;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    require(openedWriters == (separate ? 2u : 1u), "Did not open the expected output count");
    // Finishing must remain possible after availability is lost; a preflight
    // check here would discard exactly the frames the reserve protects.
    queryWorks = false; engine.finish();
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    saved(result, 1, separate ? 2 : 1); checkQueries(config.folder, 2);
    std::cout << "PASS threshold+1 separate=" << separate << " source=" << static_cast<int>(mode) << ".\n";
}
void firstAdmission(const std::filesystem::path& root, bool separate, bool queryFailure) {
    reset(separate); auto config = settings(root / (L"first-" + std::to_wstring(separate) + L"-" + std::to_wstring(queryFailure)), separate);
    openGate.armed = true; ReleaseGates release;
    lapse::Engine engine; engine.configure(config); engine.record(); awaitGate(openGate);
    require(openedWriters == (separate ? 2u : 1u) && primaryWrites == 0 && secondaryWrites == 0 && queryCalls == 1,
        "Writer gate did not lie between preflight and first sample admission");
    availableBytes = reserve * (separate ? 2 : 1); queryWorks = !queryFailure; openGate.released = true;
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    failed(result, L"Recording stopped: ", queryFailure, 0);
    require(primaryWrites == 0 && secondaryWrites == 0 && result.savedPaths.empty() && files(config.folder).empty(),
        "A first sample crossed the space check after opening the writer(s)");
    checkQueries(config.folder, 2); require(!openGate.timedOut, "Encoder-startup gate timed out");
    std::cout << "PASS first admission separate=" << separate << " query_failure=" << queryFailure << ".\n";
}
void laterAdmission(const std::filesystem::path& root, bool separate, bool queryFailure) {
    reset(separate); auto config = settings(root / (L"later-" + std::to_wstring(separate) + L"-" + std::to_wstring(queryFailure)), separate);
    ReleaseGates release; lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    // The output directory is part of the frozen session, even if live UI
    // settings change while this recording is paused.
    const auto originalFolder = config.folder;
    config.folder = (root / L"live-folder-must-be-ignored").wstring();
    config.stopOnLowDiskSpace = false; engine.configure(config);
    captureGate.armed = true; engine.setPaused(false); awaitGate(captureGate);
    require(primaryWrites == 1 && secondaryWrites == (separate ? 1u : 0u) && queryCalls == 2,
        "Space queried before a slow requested capture completed");
    availableBytes = reserve * (separate ? 2 : 1); queryWorks = !queryFailure; captureGate.released = true;
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    failed(result, L"Recording stopped: ", queryFailure, 1);
    require(primaryWrites == 1 && secondaryWrites == (separate ? 1u : 0u), "Space loss admitted an unmatched or late sample");
    require(result.savedPaths.size() == (separate ? 2u : 1u), "Space loss failed to finalize previously admitted outputs");
    for (const auto& path : result.savedPaths) verifyDecoded(path, 1);
    checkQueries(originalFolder, 3); require(!captureGate.timedOut, "Slow-capture gate timed out");
    if (separate) require(cameraStarts == cameraStops, "Stopped hidden recording retained its camera");
    const auto count = queryCalls.load(); const auto message = result.message; std::this_thread::sleep_for(100ms);
    require(queryCalls == count && engine.status().message == message && engine.status().recordingFailed,
        "Hidden idle work retried the query or erased the stop reason");
    std::cout << "PASS later admission separate=" << separate << " query_failure=" << queryFailure << ".\n";
}
void quietQueries(const std::filesystem::path& root) {
    reset(true); auto config = settings(root / L"quiet", true);
    lapse::Engine engine; engine.configure(config); std::this_thread::sleep_for(100ms);
    require(queryCalls == 0 && captures == 0, "Hidden idle worker queried space or captured");
    config.preview = true; engine.configure(config);
    await(engine, [](const auto& s) { return s.preview != nullptr; });
    const auto idleCaptures = captures.load(); std::this_thread::sleep_for(600ms);
    require(captures > idleCaptures && queryCalls == 0, "Idle preview queried disk space");
    engine.record(); await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    const auto admittedQueries = queryCalls.load(), recordedCaptures = captures.load();
    std::this_thread::sleep_for(1100ms);
    require(captures > recordedCaptures && queryCalls == admittedQueries && engine.status().frames == 1,
        "Recording preview or elapsed-clock wake queried space without admitting a sample");
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    const auto pausedCaptures = captures.load(); std::this_thread::sleep_for(1100ms);
    require(captures > pausedCaptures && queryCalls == admittedQueries, "Paused preview queried disk space");
    config.preview = false; engine.configure(config);
    std::this_thread::sleep_for(50ms); const auto hiddenCaptures = captures.load();
    availableBytes = 0; std::this_thread::sleep_for(150ms);
    require(captures == hiddenCaptures && queryCalls == admittedQueries && engine.status().state == lapse::State::Paused,
        "Hidden pause polled space or failed before a requested sample");
    engine.setPaused(false);
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    failed(result, L"Recording stopped: ", false, 1);
    require(result.savedPaths.size() == 2 && primaryWrites == 1 && secondaryWrites == 1, "Resume admitted a low-space pair");
    for (const auto& path : result.savedPaths) verifyDecoded(path, 1);
    checkQueries(config.folder, 3);
    std::cout << "PASS hidden idle/pause and all preview-only/clock work omit space queries; resume checks both outputs.\n";
}
void disabledGuard(const std::filesystem::path& root, bool separate) {
    reset(separate); auto config = settings(root / (L"disabled-" + std::to_wstring(separate)), separate);
    config.stopOnLowDiskSpace = false; expectQueries = false;
    queryWorks = false; availableBytes = 0;
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    config.stopOnLowDiskSpace = true; engine.configure(config); engine.setPaused(false);
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 2; });
    engine.finish(); const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    saved(result, 2, separate ? 2 : 1);
    require(queryCalls == 0 && !orderingFailed, "Disabled session performed a disk-space query after live settings changed");
    std::cout << "PASS disabled guard is frozen and omits every query, separate=" << separate << ".\n";
}
void automaticResult(const lapse::Status& result, bool separate) {
    saved(result, 1, separate ? 2 : 1);
    require(result.message.rfind(L"Recording time limit reached.", 0) == 0 && result.elapsed >= 1 && result.elapsed < 3,
        "Automatic completion lost priority to a storage failure or included startup time");
    require(primaryWrites == 1 && secondaryWrites == (separate ? 1u : 0u), "An expired deadline admitted an extra sample");
}
void slowInitialQuery(const std::filesystem::path& root) {
    reset(true); auto config = settings(root / L"slow-initial-query", true); config.recordingLimitSeconds = 1;
    queryGateAt = 2; queryGate.armed = true;
    lapse::Engine engine; ReleaseGates release; engine.configure(config); engine.record(); awaitGate(queryGate);
    require(openedWriters == 2 && primaryWrites == 0 && secondaryWrites == 0,
        "First admission query did not follow opening both writers");
    std::this_thread::sleep_for(1200ms); const auto admittedAt = std::chrono::steady_clock::now(); queryGate.released = true;
    const auto first = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    require(first.elapsed < 0.5, "Initial disk-space query consumed the recording duration");
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    automaticResult(result, true); checkQueries(config.folder, 2);
    require(std::chrono::steady_clock::now() - admittedAt >= 950ms && !queryGate.timedOut,
        "Slow initial query shortened the usable recording duration");
    std::cout << "PASS slow first admission query is excluded from active recording time.\n";
}
void slowQueryDeadline(const std::filesystem::path& root, bool separate, bool queryFailure) {
    reset(separate); auto config = settings(root / (L"slow-query-" + std::to_wstring(separate) + L"-" + std::to_wstring(queryFailure)), separate);
    config.recordingLimitSeconds = 1;
    lapse::Engine engine; ReleaseGates release; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    queryGateAt = 3; queryGate.armed = true; engine.setPaused(false); awaitGate(queryGate);
    std::this_thread::sleep_for(1200ms); availableBytes = 0; queryWorks = !queryFailure; queryGate.released = true;
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    automaticResult(result, separate); checkQueries(config.folder, 3);
    require(!queryGate.timedOut, "Slow storage query gate timed out");
    std::cout << "PASS time limit wins after slow query, separate=" << separate << " query_failure=" << queryFailure << ".\n";
}
void slowCaptureDeadline(const std::filesystem::path& root) {
    reset(true); auto config = settings(root / L"slow-capture-deadline", true); config.recordingLimitSeconds = 1;
    lapse::Engine engine; ReleaseGates release; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    captureGate.armed = true; engine.setPaused(false); awaitGate(captureGate);
    std::this_thread::sleep_for(1200ms); availableBytes = 0; queryWorks = false; captureGate.released = true;
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    automaticResult(result, true); checkQueries(config.folder, 2);
    require(!captureGate.timedOut, "Slow capture gate timed out");
    std::cout << "PASS capture crossing the deadline finishes before another disk-space query.\n";
}
void changedWorkingDirectory(const std::filesystem::path& root) {
    reset(true);
    struct RestoreDirectory {
        std::filesystem::path original = std::filesystem::current_path();
        ~RestoreDirectory() { std::error_code error; std::filesystem::current_path(original, error); }
    } restore;
    auto config = settings(root.filename() / L"relative-cwd", true);
    const auto originalFolder = std::filesystem::absolute(config.folder).lexically_normal().wstring();
    const auto laterDirectory = root / L"later-cwd"; std::filesystem::create_directories(laterDirectory);
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    std::filesystem::current_path(laterDirectory); engine.setPaused(false);
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 2; });
    engine.finish(); const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    saved(result, 2, 2); checkQueries(originalFolder, 3);
    for (const auto& path : result.savedPaths) require(std::filesystem::path(path).parent_path() == originalFolder,
        "Changing the process working directory redirected output publication");
    require(std::filesystem::is_empty(laterDirectory), "An active session created output in the later working directory");
    std::cout << "PASS relative session folder remains absolute through a process working-directory change.\n";
}
void pathCase(const std::filesystem::path& folder, bool trailingSlash = false) {
    reset(false); auto config = settings(folder);
    if (trailingSlash) config.folder += L"/";
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.finish(); const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    saved(result, 1, 1); checkQueries(config.folder, 2);
    std::cout << "PASS directory query spelling, length=" << config.folder.size() << " trailing_input_slash=" << trailingSlash << ".\n";
}
void publicationFailure(const std::filesystem::path& root, bool separate, bool finalization) {
    reset(separate); auto config = settings(root / (L"publication-" + std::to_wstring(separate) + L"-" + std::to_wstring(finalization)), separate);
    lapse::Engine engine; engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    const auto owned = temporary(config.folder, false);
    const auto destination = finalName(owned);
    if (finalization) finishFault = FinishFault::Primary;
    else { std::ofstream sentinel(destination, std::ios::binary); sentinel << "existing output"; require(bool(sentinel), "Cannot create collision fixture"); }
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    availableBytes = 0; engine.setPaused(false);
    const auto result = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    failed(result, L"Recording stopped: ", false, 1);
    require(result.message.find(owned) != std::wstring::npos, "Failed publication/finalization lost its retained output path");
    require(result.savedPaths.size() == (finalization ? (separate ? 1u : 0u) : (separate ? 2u : 1u)),
        "Low-space close lost the independently finalized playable outputs");
    // A wrapper finalization fault happens after real finalization. The engine
    // correctly calls this a partial file, and its owned physical movie must
    // remain recoverable while the other output is independently published.
    if (finalization) require(result.message.find(L"Injected finalization failure.") != std::wstring::npos &&
        result.message.find(L"Partial file:") != std::wstring::npos, "Finalize fault did not report its error and partial file");
    else {
        std::ifstream sentinel(destination, std::ios::binary); std::string contents; std::getline(sentinel, contents);
        require(contents == "existing output", "Low-space close replaced a colliding output");
        require(result.savedPaths.front() == owned, "Collision advertised a foreign destination instead of the owned movie");
    }
    verifyDecoded(owned, 1);
    for (const auto& path : result.savedPaths) {
        verifyDecoded(path, 1); require(result.message.find(path) != std::wstring::npos, "Failure message omitted a surviving playable output");
    }
    require(primaryWrites == 1 && secondaryWrites == (separate ? 1u : 0u), "Failure publication admitted a low-space sample");
    checkQueries(config.folder, 3);
    std::cout << "PASS low-space close separate=" << separate << " finalization_failure=" << finalization << ".\n";
}
void pixels(lapse::Frame& frame, int width, int height, bool camera) {
    frame.width = width; frame.height = height; frame.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    for (size_t i = 0; i < frame.pixels.size(); i += 4) { frame.pixels[i + (camera ? 2 : 0)] = 255; frame.pixels[i + 3] = 255; }
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
    if (id != L"synthetic-camera") { error = L"Invalid synthetic camera."; return false; }
    error.clear(); impl_->active = true; ++cameraStarts; return true;
}
void CameraClient::stop() { if (impl_->active) ++cameraStops; impl_->active = false; }
bool CameraClient::latest(Frame& output, std::wstring& error) {
    error.clear(); if (!impl_->active) { error = L"Synthetic camera is stopped."; return false; }
    pixels(output, 320, 240, true); return true;
}
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    if (id != L"synthetic-display") { error = L"Invalid synthetic display."; return false; }
    ++captures; captureGate.enter(); error.clear(); pixels(output, width, height, false); return true;
}
}
int main(int argc, char** argv) {
    const std::string selection = argc > 1 ? argv[1] : "all";
    const bool all = selection == "all";
    if (!all && selection != "startup" && selection != "admission" && selection != "quiet" && selection != "paths" && selection != "publication" && selection != "deadline") {
        std::cerr << "Usage: engine_space_tests [all|startup|admission|quiet|paths|publication|deadline]\n"; return 2;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-space-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        if (all || selection == "startup") {
            startupRefusal(root, false, 0); startupRefusal(root, false, reserve - 1); startupRefusal(root, false, reserve);
            startupRefusal(root, true, reserve + 1); startupRefusal(root, true, reserve * 2);
            startupRefusal(root, false, 1024 * MiB, true); startupRefusal(root, true, 1024 * MiB, true);
            sufficient(root, false, lapse::Mode::Desktop); sufficient(root, false, lapse::Mode::Camera);
            sufficient(root, false, lapse::Mode::Overlay); sufficient(root, true, lapse::Mode::Camera);
            sufficient(root, false, lapse::Mode::Desktop, true);
            disabledGuard(root, false); disabledGuard(root, true);
        }
        if (all || selection == "admission") for (bool separate : {false, true}) for (bool queryFailure : {false, true}) {
            firstAdmission(root, separate, queryFailure); laterAdmission(root, separate, queryFailure);
        }
        if (all || selection == "quiet") quietQueries(root);
        if (all || selection == "deadline") {
            slowInitialQuery(root); slowCaptureDeadline(root);
            slowQueryDeadline(root, false, false); slowQueryDeadline(root, true, true);
        }
        if (all || selection == "paths") {
            require(lapse::recordingDirectoryIO(L"\\\\server\\share") == L"\\\\?\\UNC\\server\\share\\",
                "UNC share-root query spelling lacks its required trailing separator");
            pathCase(root / L"\u7a7a\u9593-\u00e9-\u0416");
            pathCase(root.filename() / L"relative");
            auto longFolder = root; while (longFolder.wstring().size() < 300) longFolder /= std::wstring(60, L'x');
            pathCase(longFolder, true);
            changedWorkingDirectory(root);
        }
        if (all || selection == "publication") for (bool separate : {false, true}) for (bool finalization : {false, true})
            publicationFailure(root, separate, finalization);
        std::filesystem::remove_all(lapse::fileIOPath(root.wstring()));
        std::cout << "Disk-space contract: " << selection << " passed with real final MP4 decoding.\n";
    } catch (const std::exception& error) {
        openGate.released = captureGate.released = queryGate.released = true;
        std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts kept at " << root.wstring() << L'\n'; result = 1;
    }
    MFShutdown(); CoUninitialize(); return result;
}

#include "engine_person_camera_stub.h"

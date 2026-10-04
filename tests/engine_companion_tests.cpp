// A collage plus full-frame desktop/camera companion files from the same
// samples. Synthetic sources, real MP4 encoding/decoding and owned publication;
// faults are injected only at the individual writer boundary.
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
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
enum class Fault { None, OpenCamera, WriteCamera, WriteDesktop };
std::atomic<Fault> fault{Fault::None};
std::atomic<unsigned> cameraStarts{0}, cameraStops{0}, captures{0}, opens{0};
std::atomic<bool> cameraFails{false};
}
namespace lapse {
class CompanionEncoder {
    Encoder real_;
    enum class Kind { Layout, Desktop, Camera } kind_ = Kind::Layout;
public:
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error, EncodingQuality quality, EncodingMode mode, bool recoveryMode, const EncodingOptions& options = {}) {
        kind_ = path.find(L"-camera.recording.mp4") != std::wstring::npos ? Kind::Camera
            : path.find(L"-desktop.recording.mp4") != std::wstring::npos ? Kind::Desktop : Kind::Layout;
        ++opens;
        if (kind_ == Kind::Camera && fault == Fault::OpenCamera) { error = L"Injected open failure."; return false; }
        return real_.open(path, width, height, fps, error, quality, mode, recoveryMode, options);
    }
    bool write(const Frame& frame, std::wstring& error) {
        if ((kind_ == Kind::Camera && fault == Fault::WriteCamera) || (kind_ == Kind::Desktop && fault == Fault::WriteDesktop)) {
            error = L"Injected write failure."; return false;
        }
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
#define Encoder CompanionEncoder
#include "../src/engine.cpp"
#undef Encoder

namespace {
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }
template<class Predicate> lapse::Status await(lapse::Engine& engine, Predicate predicate, int milliseconds = 9000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        auto status = engine.status();
        if (predicate(status)) return status;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < until);
    std::wcerr << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for companion recording");
}
// Desktop is pure blue and the camera pure red, so every decoded sample names
// its source: BT.709 limited range gives Y/U/V near 32/240/118 and 63/102/240.
enum class Color { Desktop, Camera };
void pattern(lapse::Frame& frame, int width, int height, bool camera) {
    frame.width = width; frame.height = height;
    frame.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    for (size_t i = 0; i < frame.pixels.size(); i += 4) { frame.pixels[i + (camera ? 2 : 0)] = 255; frame.pixels[i + 3] = 255; }
}
struct Decoded { std::vector<LONGLONG> timestamps; std::vector<std::vector<Color>> samples; };
Decoded decode(const std::wstring& path, const std::vector<POINT>& points) {
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Cannot reopen output MP4");
    checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Deselect streams failed");
    checked(reader->SetStreamSelection(stream, TRUE), "Select video failed");
    ComPtr<IMFMediaType> type; checked(reader->GetNativeMediaType(stream, 0, &type), "Missing native media type");
    UINT32 width = 0, height = 0; checked(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "Missing dimensions");
    require(width == 320 && height == 240, "Companion output has the wrong size");
    type.Reset(); checked(MFCreateMediaType(&type), "Cannot create decoded type");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Cannot set decoded major type");
    checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Cannot set decoded pixel type");
    checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Cannot request NV12 decode");
    Decoded result;
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
        checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Cannot decode output");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Decoded output contains an error");
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer; ComPtr<IMF2DBuffer> plane;
            checked(sample->ConvertToContiguousBuffer(&buffer), "No decoded pixels");
            BYTE* pixels = nullptr; LONG stride = 320; DWORD length = 0;
            const bool twoD = SUCCEEDED(buffer.As(&plane));
            if (twoD) checked(plane->Lock2D(&pixels, &stride), "Cannot lock decoded plane");
            else checked(buffer->Lock(&pixels, nullptr, &length), "Cannot lock decoded pixels");
            std::vector<Color> colors; bool known = stride >= 320;
            for (const auto& point : points) {
                const int y = pixels[point.y * stride + point.x];
                const BYTE* uv = pixels + stride * 240 + (point.y / 2) * stride + (point.x & ~1);
                const bool blue = std::abs(y - 32) <= 10 && std::abs(int(uv[0]) - 240) <= 10 && std::abs(int(uv[1]) - 118) <= 10;
                const bool red = std::abs(y - 63) <= 10 && std::abs(int(uv[0]) - 102) <= 10 && std::abs(int(uv[1]) - 240) <= 10;
                known &= blue || red;
                colors.push_back(red ? Color::Camera : Color::Desktop);
            }
            if (twoD) plane->Unlock2D(); else buffer->Unlock();
            require(known, "Decoded companion sample is neither the desktop nor the camera source");
            result.timestamps.push_back(timestamp); result.samples.push_back(std::move(colors));
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    return result;
}
lapse::Settings settings(const std::filesystem::path& folder, lapse::Mode mode = lapse::Mode::Overlay) {
    lapse::Settings result;
    result.layers = lapse::preset(mode);
    result.alsoSaveDesktop = result.alsoSaveCamera = true;
    result.monitorId = L"synthetic-display"; result.cameraId = L"synthetic-camera";
    result.width = 320; result.height = 240; result.intervalMs = 60000;
    result.preview = false; result.folder = folder.wstring();
    return result;
}
std::vector<std::filesystem::path> files(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> result;
    if (std::filesystem::exists(folder)) for (const auto& item : std::filesystem::directory_iterator(folder)) result.push_back(item.path());
    return result;
}
bool endsWith(const std::wstring& value, const std::wstring& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
lapse::Status finish(lapse::Engine& engine) {
    engine.finish();
    return await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
}
void plans() {
    using lapse::OutputKind;
    lapse::Settings value; value.layers = lapse::preset(lapse::Mode::Overlay);
    auto plan = lapse::outputPlan(value);
    require(plan.count == 1 && plan.kinds[0] == OutputKind::Layout && lapse::recordingSources(value) == 3u, "Plain collage plan changed");
    value.alsoSaveDesktop = value.alsoSaveCamera = true; plan = lapse::outputPlan(value);
    require(plan.count == 3 && plan.kinds[0] == OutputKind::Layout && plan.kinds[1] == OutputKind::Desktop && plan.kinds[2] == OutputKind::Camera,
        "Collage companions are not ordered layout, desktop, camera");
    value.separateFiles = true; plan = lapse::outputPlan(value);
    require(plan.count == 2 && plan.kinds[0] == OutputKind::Desktop && plan.kinds[1] == OutputKind::Camera, "Separate files gained a combined output");
    value.separateFiles = false; value.layers = lapse::preset(lapse::Mode::Desktop); plan = lapse::outputPlan(value);
    require(plan.count == 2 && plan.kinds[1] == OutputKind::Camera && lapse::recordingSources(value) == 3u,
        "A desktop layout duplicated its desktop companion or lost the camera companion");
    value.alsoSaveDesktop = false; value.layers = lapse::preset(lapse::Mode::Camera); plan = lapse::outputPlan(value);
    require(plan.count == 1 && lapse::recordingSources(value) == 2u, "A camera layout duplicated its camera companion");
    // Disk headroom scales with every planned output.
    const lapse::RecordingSpace space{192ULL * 1024 * 1024, ERROR_SUCCESS};
    require(space.enough(2) && !space.enough(3), "Three outputs did not reserve 64 MiB each");
    require(lapse::recordingSpaceProblem(space, 3).find(L"192 MiB") != std::wstring::npos &&
        lapse::recordingSpaceProblem(space, 2).find(L"128 MiB") != std::wstring::npos &&
        lapse::recordingSpaceProblem(space, 1).find(L"64 MiB available to leave room for finishing the video.") != std::wstring::npos,
        "Disk-space messages do not name the per-plan reserve");
    std::cout << "PASS output plans: ordering, separate pair, duplicate suppression, sources and disk reserve.\n";
}
void threeFiles(const std::filesystem::path& root) {
    fault = Fault::None; const auto folder = root / L"three";
    lapse::Engine engine; auto config = settings(folder); engine.configure(config);
    const unsigned opensBefore = opens; engine.record();
    const auto recording = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    require(recording.message == L"Recording the combined video plus separate desktop and camera files." && opens == opensBefore + 3,
        "Three-file recording did not open three writers or describe them");
    std::wstring stem;
    for (const auto& file : files(folder)) {
        const auto name = file.filename().wstring();
        require(endsWith(name, L".recording.mp4"), "Unexpected file while recording");
        if (!endsWith(name, L"-desktop.recording.mp4") && !endsWith(name, L"-camera.recording.mp4")) stem = name.substr(0, name.size() - 14);
    }
    require(files(folder).size() == 3 && !stem.empty(), "Recording did not own exactly one combined and two companion files");
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    // A live collage edit changes only the combined video: move the camera
    // to the upper-left corner before the second sample.
    config.layers[1].rect = {0, 0, 0.3, 0.3}; engine.configure(config);
    engine.setPaused(false);
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 2; });
    const auto saved = finish(engine);
    require(!saved.error && !saved.recordingFailed && saved.frames == 2 && saved.savedPaths.size() == 3 && saved.savedPath == saved.savedPaths[0],
        "Three-file recording did not save every output");
    require(saved.savedPaths[0] == (folder / (stem + L".mp4")).wstring() && saved.savedPaths[1] == (folder / (stem + L"-desktop.mp4")).wstring() &&
        saved.savedPaths[2] == (folder / (stem + L"-camera.mp4")).wstring(), "Companion files do not share the combined video's session stem");
    for (const wchar_t* label : {L"Combined: saved ", L"Desktop: saved ", L"Camera: saved "})
        require(saved.message.find(label) != std::wstring::npos, "Outcome did not label each saved file");
    for (const auto& path : saved.savedPaths) require(saved.message.find(path) != std::wstring::npos, "Outcome did not name each saved file");
    const std::vector<POINT> points{{40, 30}, {160, 120}, {266, 199}};
    const auto combined = decode(saved.savedPaths[0], points), desktop = decode(saved.savedPaths[1], points), camera = decode(saved.savedPaths[2], points);
    require(combined.samples.size() == 2 && desktop.samples.size() == 2 && camera.samples.size() == 2, "An output lost a frame");
    require(combined.timestamps == desktop.timestamps && desktop.timestamps == camera.timestamps, "Output timestamps differ");
    using C = Color;
    require(combined.samples[0] == std::vector<C>{C::Desktop, C::Desktop, C::Camera} &&
        combined.samples[1] == std::vector<C>{C::Camera, C::Desktop, C::Desktop}, "Combined video did not follow the live collage");
    for (const auto& sample : desktop.samples) require(sample == std::vector<C>(3, C::Desktop), "Desktop companion is not the full desktop");
    for (const auto& sample : camera.samples) require(sample == std::vector<C>(3, C::Camera), "Camera companion is not the full camera");
    std::cout << "PASS three files: shared stem and timestamps, live collage edits only in the combined video, full-frame companions.\n";
}
void singleCompanion(const std::filesystem::path& root) {
    fault = Fault::None; const auto folder = root / L"single";
    lapse::Engine engine; auto config = settings(folder, lapse::Mode::SideBySide); config.alsoSaveDesktop = false;
    engine.configure(config); engine.record();
    const auto recording = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    require(recording.message == L"Recording the combined video plus a separate camera file.", "Single companion was not described");
    const auto saved = finish(engine);
    require(!saved.error && saved.savedPaths.size() == 2 && endsWith(saved.savedPaths[1], L"-camera.mp4") &&
        !endsWith(saved.savedPaths[0], L"-desktop.mp4") && files(folder).size() == 2, "Camera-only companion did not save exactly two files");
    const auto combined = decode(saved.savedPaths[0], {{80, 120}, {240, 120}}), camera = decode(saved.savedPaths[1], {{80, 120}, {240, 120}});
    require(combined.samples.size() == 1 && combined.samples[0] == std::vector<Color>{Color::Desktop, Color::Camera} &&
        camera.samples[0] == std::vector<Color>(2, Color::Camera), "Side-by-side combined video or camera companion has the wrong content");
    std::cout << "PASS side by side plus camera companion: two labelled files, correct content.\n";
}
void duplicateCompanion(const std::filesystem::path& root) {
    fault = Fault::None;
    lapse::Engine engine;
    auto config = settings(root / L"camera-layout", lapse::Mode::Camera); config.alsoSaveDesktop = false;
    engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    auto saved = finish(engine);
    require(!saved.error && saved.savedPaths.size() == 1 && !endsWith(saved.savedPaths[0], L"-camera.mp4") && files(config.folder).size() == 1,
        "A camera layout recorded a duplicate camera companion");
    config = settings(root / L"desktop-layout", lapse::Mode::Desktop);
    engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    saved = finish(engine);
    require(!saved.error && saved.savedPaths.size() == 2 && endsWith(saved.savedPaths[1], L"-camera.mp4") && files(config.folder).size() == 2,
        "A desktop layout did not keep only its distinct camera companion");
    require(decode(saved.savedPaths[0], {{160, 120}}).samples[0][0] == Color::Desktop &&
        decode(saved.savedPaths[1], {{160, 120}}).samples[0][0] == Color::Camera, "Deduplicated outputs have the wrong content");
    std::cout << "PASS single-source layouts never duplicate their own source.\n";
}
void splitSets(const std::filesystem::path& root) {
    fault = Fault::None; const auto folder = root / L"split";
    lapse::Engine engine; auto config = settings(folder); config.segmentDurationSeconds = 1;
    engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.frames == 1 && s.completedSegments == 1; });
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    engine.setPaused(false); await(engine, [](const auto& s) { return s.frames == 2; });
    const auto saved = finish(engine);
    require(!saved.error && saved.frames == 2 && saved.completedSegments == 2 && saved.savedPaths.size() == 3, "Split companions did not count whole sets");
    const auto parts = files(folder);
    require(parts.size() == 6, "Two split sets did not publish six files");
    unsigned desktopParts = 0, cameraParts = 0;
    for (const auto& path : parts) {
        const auto name = path.filename().wstring();
        require(name.find(L"-part-00000") != std::wstring::npos && !endsWith(name, L".recording.mp4"), "Split part was not published with its part number");
        desktopParts += endsWith(name, L"-desktop.mp4"); cameraParts += endsWith(name, L"-camera.mp4");
        require(decode(path.wstring(), {{160, 120}}).samples.size() == 1, "Split part lost its frame");
    }
    require(desktopParts == 2 && cameraParts == 2, "Each split part did not keep its companions");
    std::cout << "PASS split parts keep matching combined, desktop and camera files.\n";
}
void faults(const std::filesystem::path& root) {
    // A companion write failure stops the session and still saves every file.
    fault = Fault::None; auto folder = root / L"write-camera";
    lapse::Engine engine; engine.configure(settings(folder)); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    fault = Fault::WriteCamera; engine.setPaused(false);
    auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    require(saved.recordingFailed && saved.frames == 1 && saved.savedPaths.size() == 3 &&
        saved.message.find(L"Camera recording stopped: Injected write failure.") != std::wstring::npos &&
        saved.message.find(L"(2 frames)") != std::wstring::npos && saved.message.find(L"(1 frames)") != std::wstring::npos,
        "Companion write failure lost a file or hid the differing frame counts");
    require(decode(saved.savedPaths[0], {{160, 120}}).samples.size() == 2 && decode(saved.savedPaths[2], {{160, 120}}).samples.size() == 1,
        "Saved files after a companion write failure have the wrong lengths");
    // A failed third writer leaves no empty files; a retry records normally.
    fault = Fault::OpenCamera; folder = root / L"open-camera";
    auto config = settings(folder); engine.configure(config); engine.record();
    saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
    require(saved.recordingFailed && saved.frames == 0 && saved.savedPaths.empty() && files(folder).empty() &&
        saved.message.find(L"Cannot start camera recording: Injected open failure.") != std::wstring::npos,
        "Companion open failure kept files or did not name the failed output");
    fault = Fault::None; engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    saved = finish(engine);
    require(!saved.error && saved.savedPaths.size() == 3 && files(folder).size() == 3, "Retry after a companion failure did not save three files");
    std::cout << "PASS companion write/open failures preserve saved frames, leave no empty files, and allow retry.\n";
}
// Apply a live layout while paused so the next sample (admitted on resume)
// deterministically uses it.
lapse::Status switchLayout(lapse::Engine& engine, lapse::Settings& config, lapse::Mode mode, uint64_t frames, bool expectFrame = true) {
    engine.setPaused(true); await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
    config.layers = lapse::preset(mode); engine.configure(config); engine.setPaused(false);
    if (!expectFrame) return await(engine, [](const auto& s) { return s.state == lapse::State::Recording; });
    return await(engine, [&](const auto& s) { return s.state == lapse::State::Recording && s.frames == frames + 1; });
}
void liveSources(const std::filesystem::path& root) {
    fault = Fault::None; cameraFails = false; const auto folder = root / L"live";
    lapse::Engine engine; auto config = settings(folder, lapse::Mode::Desktop); config.alsoSaveDesktop = config.alsoSaveCamera = false;
    const unsigned startsBefore = cameraStarts;
    engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    require(cameraStarts == startsBefore, "A desktop-only recording started the camera before it was added");
    const auto overlay = switchLayout(engine, config, lapse::Mode::Overlay, 1);
    require(!overlay.error && cameraStarts > startsBefore, "Adding the camera overlay mid-recording did not start the camera");
    switchLayout(engine, config, lapse::Mode::Camera, 2);
    const auto saved = finish(engine);
    require(!saved.error && !saved.recordingFailed && saved.frames == 3 && saved.savedPaths.size() == 1, "Live source changes did not save one video");
    const auto decoded = decode(saved.savedPaths[0], {{40, 30}, {160, 120}, {266, 199}});
    using C = Color;
    require(decoded.samples.size() == 3 && decoded.samples[0] == std::vector<C>(3, C::Desktop) &&
        decoded.samples[1] == std::vector<C>{C::Desktop, C::Desktop, C::Camera} && decoded.samples[2] == std::vector<C>(3, C::Camera),
        "One video did not show desktop, then the camera overlay, then the camera");
    std::cout << "PASS live source changes: desktop, added camera overlay, then camera in one video.\n";
}
void optionalCameraFailure(const std::filesystem::path& root) {
    fault = Fault::None; cameraFails = true; const auto folder = root / L"live-failure";
    lapse::Engine engine; auto config = settings(folder, lapse::Mode::Desktop); config.alsoSaveDesktop = config.alsoSaveCamera = false;
    engine.configure(config); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    // A camera that cannot start after a live change is left out, not fatal.
    auto status = switchLayout(engine, config, lapse::Mode::Overlay, 1);
    status = await(engine, [](const auto& s) { return s.error; });
    require(status.state == lapse::State::Recording && !status.recordingFailed &&
        status.message.find(L"Camera unavailable; recording continues without it. Synthetic camera is in use by another app.") == 0,
        "A failed live-added camera stopped recording or was not explained");
    // With nothing drawable, samples are skipped while recording continues.
    switchLayout(engine, config, lapse::Mode::Camera, 2, false);
    status = await(engine, [](const auto& s) { return s.error && s.message.find(L"Camera unavailable") == 0; });
    std::this_thread::sleep_for(400ms);
    status = engine.status();
    require(status.state == lapse::State::Recording && status.frames == 2 && !status.recordingFailed, "A camera-only layout without a camera admitted a frame or stopped");
    status = switchLayout(engine, config, lapse::Mode::Desktop, 2);
    require(!status.error && status.message == L"Recording. You can adjust the collage live.", "Leaving the missing camera did not clear its warning");
    cameraFails = false;
    status = switchLayout(engine, config, lapse::Mode::Overlay, 3);
    require(!status.error, "Switching back did not retry the camera");
    const auto saved = finish(engine);
    require(!saved.error && !saved.recordingFailed && saved.frames == 4, "An optional camera failure changed the saved outcome");
    const auto decoded = decode(saved.savedPaths[0], {{40, 30}, {160, 120}, {266, 199}});
    using C = Color;
    require(decoded.samples.size() == 4 && decoded.samples[1] == std::vector<C>(3, C::Desktop) && decoded.samples[2] == std::vector<C>(3, C::Desktop) &&
        decoded.samples[3] == std::vector<C>{C::Desktop, C::Desktop, C::Camera}, "Omitted or retried camera content is wrong");
    std::cout << "PASS a failed live-added camera is omitted with a warning, skips camera-only samples, and retries when switched back.\n";
}
}
namespace lapse {
bool CameraClient::beginNight(uint64_t, uint32_t, const NightSettings&, std::wstring& error) {
    error = L"Unexpected night request in companion fixture."; return false;
}
bool CameraClient::nightResult(uint64_t, Frame&, NightWindowResult&, std::wstring& error) {
    error = L"Unexpected night result in companion fixture."; return false;
}
void CameraClient::cancelNight() noexcept {}
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) {
    error = L"Unexpected activity observer in companion fixture."; return false;
}
void CameraClient::cancelActivityObservation() noexcept {}
struct CameraClient::Impl { bool active = false; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring& id, std::wstring& error, CameraResolution) {
    if (id != L"synthetic-camera") { error = L"Unknown synthetic camera."; return false; }
    if (cameraFails) { error = L"Synthetic camera is in use by another app."; return false; }
    error.clear(); impl_->active = true; ++cameraStarts; return true;
}
void CameraClient::stop() { if (impl_->active) ++cameraStops; impl_->active = false; }
bool CameraClient::latest(Frame& output, std::wstring& error) {
    if (!impl_->active) { error = L"Synthetic camera stopped."; return false; }
    error.clear(); pattern(output, 320, 240, true); return true;
}
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    if (id != L"synthetic-display") { error = L"Synthetic display disconnected."; return false; }
    error.clear(); ++captures; pattern(output, width, height, false); return true;
}
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto root = std::filesystem::current_path() / (L"engine-companion-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        plans(); threeFiles(root); singleCompanion(root); duplicateCompanion(root); splitSets(root); faults(root);
        liveSources(root); optionalCameraFailure(root);
        std::filesystem::remove_all(root);
        std::cout << "Companion engine: 8 synthetic real-encoder case groups passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; std::wcerr << L"Artifacts kept at " << root.wstring() << L'\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

#include "engine_person_camera_stub.h"

// This fixture owns no native desktop capture surface.
namespace lapse { void releaseDesktopCaptureCache() noexcept {} }

// Link with engine.cpp, core.cpp and encoder.cpp, without capture.cpp or the
// camera host implementation. These controlled sources exercise worker recovery
// and camera lifecycle without depending on a physical device or desktop.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<int> starts{0};
std::atomic<int> stops{0};
std::atomic<bool> failNext{false};
std::atomic<bool> throwNext{false};
std::atomic<bool> transientNext{false};
std::atomic<bool> failDesktopPreview{false};
std::atomic<bool> throwDesktopPreview{false};
std::atomic<bool> failDesktopCapture{false};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate>
lapse::Status await(lapse::Engine& engine, Predicate predicate, int timeoutMs = 7000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        auto status = engine.status();
        if (predicate(status)) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    std::wcerr << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for engine recovery");
}
lapse::Frame pixels() {
    return {320, 240, std::vector<uint8_t>(320 * 240 * 4, 96)};
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
struct CameraClient::Impl { bool active = false; int warmup = 0; };
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring&, std::wstring& error) {
    error.clear(); impl_->active = true; impl_->warmup = 4; ++starts; return true;
}
void CameraClient::stop() {
    if (impl_->active) ++stops;
    impl_->active = false;
}
bool CameraClient::latest(Frame& output, std::wstring& error) {
    error.clear();
    if (throwNext.exchange(false)) throw std::bad_alloc();
    if (transientNext.exchange(false)) return false;
    if (failNext.exchange(false)) { error = L"Injected camera disconnect."; return false; }
    if (!impl_->active) { error = L"Test camera is stopped."; return false; }
    if (impl_->warmup > 0) { --impl_->warmup; return false; }
    output = pixels(); return true;
}
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    if (id != L"synthetic-display") { error = L"Unknown synthetic display."; return false; }
    error.clear();
    if (width == 480 && height == 360) {
        if (throwDesktopPreview.exchange(false)) throw std::bad_alloc();
        if (failDesktopPreview) { error = L"Injected desktop preview failure."; return false; }
    } else if (failDesktopCapture) { error = L"Injected desktop sample failure."; return false; }
    output = pixels(); return true;
}
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    int result = 0;
    const auto directory = std::filesystem::current_path() /
        (L"engine-recovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        {
            lapse::Settings settings; settings.monitorId = L"synthetic-display";
            settings.layers = lapse::preset(lapse::Mode::Camera);
            settings.cameraId = L"controlled-test-camera";
            settings.width = 320; settings.height = 240; settings.intervalMs = 1000;
            settings.folder = directory.wstring(); settings.preview = false;
            lapse::Engine engine;
            engine.configure(settings); engine.record();
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
            require(starts == 1, "Initial camera opened more than once during warmup");

            engine.pause();
            const auto paused = await(engine, [](const auto& s) { return s.state == lapse::State::Paused && stops >= 1; });
            require(stops >= 1, "Pausing with preview off did not release camera");
            engine.pause();
            await(engine, [&](const auto& s) { return s.state == lapse::State::Recording && s.frames > paused.frames; });
            require(starts == 2, "Resume did not reopen the camera");

            failNext = true;
            auto failed = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
            require(stops >= 2, "Fatal camera error did not release failed reader");
            require(!failed.savedPath.empty() && std::filesystem::exists(failed.savedPath), "Camera failure did not save captured frames");
            const auto beforeRetry = starts.load();
            engine.record();
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
            require(starts == beforeRetry + 1, "Record did not retry the same failed camera");

            const auto priorPath = failed.savedPath;
            throwNext = true;
            failed = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
            require(!failed.savedPath.empty() && failed.savedPath != priorPath && std::filesystem::exists(failed.savedPath),
                    "Resource exception did not finalize the open video");
            const auto resourceError = failed.message;
            settings.layers = lapse::preset(lapse::Mode::Desktop);
            settings.preview = true;
            engine.configure(settings);
            await(engine, [](const auto& s) { return bool(s.preview); });
            require(engine.status().error && engine.status().message == resourceError,
                    "Successful idle preview erased the resource failure");

            settings.layers = lapse::preset(lapse::Mode::Camera); settings.preview = false;
            engine.configure(settings); engine.record();
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
            require(starts == beforeRetry + 2, "Worker could not record after an exception");
            engine.finish();
            const auto finished = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
            require(!finished.error && std::filesystem::exists(finished.savedPath), "Recovered recording did not save");

            // An idle preview failure must also recover after switching to
            // another source and back, without requiring a recording first.
            settings.preview = true;
            engine.configure(settings); failNext = true;
            await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
            const auto previewStarts = starts.load();
            settings.layers = lapse::preset(lapse::Mode::Desktop);
            engine.configure(settings);
            await(engine, [](const auto& s) { return !s.error && bool(s.preview); });
            settings.layers = lapse::preset(lapse::Mode::Camera);
            engine.configure(settings);
            await(engine, [&](const auto& s) { return !s.error && bool(s.preview) && starts > previewStarts; });
            failNext = true;
            await(engine, [](const auto& s) { return s.error; });
            const auto refreshStarts = starts.load();
            engine.refreshSources();
            await(engine, [&](const auto& s) { return !s.error && bool(s.preview) && starts > refreshStarts; });
            require(starts == refreshStarts + 1, "Refresh did not reopen exactly one camera helper");
            engine.record();
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
            // A brief shared-memory contention after the startup grace period
            // must not terminate an otherwise healthy recording.
            std::this_thread::sleep_for(std::chrono::milliseconds(8100));
            const auto beforeTransient = engine.status().frames;
            transientNext = true;
            await(engine, [&](const auto& s) { return s.frames > beforeTransient && !transientNext.load(); });
            require(engine.status().state == lapse::State::Recording, "Transient camera wait ended a healthy recording");
            engine.finish();
            await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
        }
        {
            // At a long sample interval, desktop preview work happens between
            // due video frames. Its failure must not truncate a good recording
            // or close the healthy camera used by the same overlay.
            lapse::Settings settings; settings.monitorId = L"synthetic-display";
            settings.layers = lapse::preset(lapse::Mode::Overlay);
            settings.cameraId = L"controlled-test-camera";
            settings.width = 320; settings.height = 240; settings.intervalMs = 30000;
            settings.folder = directory.wstring(); settings.preview = true;
            lapse::Engine engine;
            engine.configure(settings); engine.record();
            const auto initial = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
            const int initialStarts = starts.load(), initialStops = stops.load();

            failDesktopPreview = true;
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.error && !s.preview; });
            require(starts == initialStarts && stops == initialStops, "Desktop preview failure disturbed healthy camera");
            failDesktopPreview = false;
            auto recovered = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && !s.error && bool(s.preview); });
            require(recovered.frames == initial.frames && recovered.message.find(L"Recording.") == 0,
                    "Active preview recovery did not preserve samples and restore recording message");

            failDesktopPreview = true;
            const auto failedPreview = await(engine, [](const auto& s) { return s.error && !s.preview; });
            engine.pause();
            auto paused = await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
            require(paused.error && paused.message == failedPreview.message, "Pause hid the unresolved preview error");
            failDesktopPreview = false;
            recovered = await(engine, [](const auto& s) { return s.state == lapse::State::Paused && !s.error && bool(s.preview); });
            require(recovered.frames == initial.frames && recovered.message.find(L"Paused.") == 0,
                    "Paused preview recovery did not restore paused message");

            failDesktopPreview = true;
            await(engine, [](const auto& s) { return s.state == lapse::State::Paused && s.error && !s.preview; });
            engine.pause();
            // A full-size resume sample succeeds independently of the broken
            // low-resolution preview; admission alone must not clear its error.
            recovered = await(engine, [&](const auto& s) { return s.state == lapse::State::Recording && s.frames > initial.frames; });
            require(recovered.error, "Successful resume sample falsely cleared the preview error without refreshing it");
            failDesktopPreview = false;
            recovered = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && !s.error && bool(s.preview); });
            require(recovered.message.find(L"Recording.") == 0,
                    "Successful preview refresh did not restore the recording message");

            throwDesktopPreview = true;
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.error && !s.preview; });
            require(starts == initialStarts && stops == initialStops, "Preview allocation failure closed the healthy camera");
            recovered = await(engine, [](const auto& s) { return s.state == lapse::State::Recording && !s.error && bool(s.preview); });
            require(recovered.frames == initial.frames + 1, "Preview allocation failure changed the captured frame count");

            // A real requested sample failure is still fatal. Resume makes a
            // sample due immediately, so this also exercises that distinction
            // without sleeping for the configured capture interval.
            engine.pause();
            await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
            failDesktopCapture = true;
            engine.pause();
            const auto failedSample = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
            failDesktopCapture = false;
            require(failedSample.frames == recovered.frames && !failedSample.savedPath.empty() && std::filesystem::exists(failedSample.savedPath),
                    "Due desktop sample failure did not stop and save captured frames");
            require(failedSample.message.find(L"Capture stopped: Injected desktop sample failure.") == 0,
                    "Due desktop failure was misreported as a preview issue");
            await(engine, [&](const auto& s) { return bool(s.preview) && s.preview != failedSample.preview; });
            require(engine.status().error && engine.status().message == failedSample.message,
                    "Preview recovery erased the due-frame capture error");
            require(starts == initialStarts && stops == initialStops, "Desktop failures closed a healthy overlay camera");
        }
        std::filesystem::remove_all(directory);
        std::cout << "Engine camera retry, resume warmup, preview isolation, exception recovery, saved frames and sticky error checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Artifacts kept at " << directory.wstring() << L'\n';
        result = 1;
    }
    MFShutdown(); CoUninitialize();
    return result;
}

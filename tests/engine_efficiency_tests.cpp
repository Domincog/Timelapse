// Controlled sources exercise preview memory ownership and capture reuse while
// retaining the real compositor, worker and MP4 encoder.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<unsigned> shade{72};
std::atomic<unsigned> desktopCaptures{0};
std::atomic<unsigned> fullCaptures{0};
std::atomic<unsigned> reusedFullBuffers{0};
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
    throw std::runtime_error("Timed out waiting for engine efficiency check");
}
void boundedPreview(const lapse::Status& status) {
    require(status.preview && status.preview->valid(), "No valid preview was published");
    require(status.preview->width == 640 && status.preview->height == 360,
            "Published preview is not bounded to 640 by 360");
    require(status.preview->pixels.capacity() <= 640 * 360 * 4,
            "Published preview retained recording-sized storage");
}
}

namespace lapse {
struct CameraClient::Impl {};
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring&, std::wstring& error) {
    error = L"Unexpected camera request in desktop test."; return false;
}
void CameraClient::stop() {}
bool CameraClient::latest(Frame&, std::wstring&) { return false; }
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    if (id != L"synthetic-display") { error = L"Unknown synthetic display."; return false; }
    error.clear();
    const size_t bytes = size_t(width) * height * 4;
    if (width == 1920 && height == 1080) {
        if (output.pixels.capacity() >= bytes) ++reusedFullBuffers;
        ++fullCaptures;
    }
    output.width = width; output.height = height;
    output.pixels.resize(bytes);
    std::fill(output.pixels.begin(), output.pixels.end(), static_cast<uint8_t>(shade.load()));
    ++desktopCaptures;
    return true;
}
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    int result = 0;
    const auto directory = std::filesystem::current_path() /
        (L"engine-efficiency-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        {
            lapse::Settings settings; settings.monitorId = L"synthetic-display";
            settings.width = 1920; settings.height = 1080;
            settings.interval = 1; settings.folder = directory.wstring();
            lapse::Engine engine;
            engine.configure(settings);
            auto first = await(engine, [](const auto& s) { return bool(s.preview); });
            boundedPreview(first);
            const auto original = first.preview->pixels;
            shade = 184;
            auto changed = await(engine, [&](const auto& s) {
                return s.preview && s.preview != first.preview && s.preview->pixels[0] == 184;
            });
            boundedPreview(changed);
            require(first.preview->pixels == original, "Refreshing preview mutated a retained UI snapshot");

            engine.record();
            auto recording = await(engine, [](const auto& s) {
                return s.state == lapse::State::Recording && s.frames >= 2 && bool(s.preview);
            });
            boundedPreview(recording);
            require(fullCaptures >= 2 && reusedFullBuffers >= 1,
                    "Recording reallocated the desktop capture buffer between samples");
            require(first.preview->pixels == original, "Recording mutated a retained idle preview snapshot");

            // Configure clears the published preview immediately. Future due
            // recording frames must keep it clear while still being encoded.
            settings.preview = false;
            engine.configure(settings);
            require(!engine.status().preview, "Disabling preview retained its published frame");
            auto hidden = await(engine, [&](const auto& s) { return s.frames > recording.frames; });
            require(!hidden.preview && !hidden.error, "Hidden recording published a preview or stopped");
            require(first.preview->pixels == original, "Hidden recording changed an older UI snapshot");

            engine.finish();
            auto finished = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
            require(!finished.error && !finished.preview && !finished.savedPath.empty() &&
                    std::filesystem::file_size(finished.savedPath) > 0,
                    "Hidden recording did not save successfully without a preview");
            const auto capturesAtRest = desktopCaptures.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(650));
            require(desktopCaptures == capturesAtRest, "Idle engine captured frames while preview was disabled");

            settings.preview = true;
            engine.configure(settings);
            auto restored = await(engine, [](const auto& s) { return bool(s.preview); });
            boundedPreview(restored);
            require(first.preview->pixels == original, "Restoring preview mutated an older UI snapshot");
        }
        std::filesystem::remove_all(directory);
        std::cout << "Bounded previews, immutable snapshots, reusable capture buffers and hidden recording checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Artifacts kept at " << directory.wstring() << L'\n';
        result = 1;
    }
    MFShutdown(); CoUninitialize();
    return result;
}

// Controlled capture sources with the real encoder and filesystem. A small
// existing destination forces a real rename failure without filling a disk or
// changing permissions, and the retained movie must still decode completely.
#include "engine.h"
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
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
std::atomic<int> cameraPolls{0};
constexpr char sentinel[] = "An existing video must never be replaced.";

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<class Predicate>
lapse::Status await(lapse::Engine& engine, Predicate predicate, int timeoutMs = 7000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        auto status = engine.status();
        if (predicate(status)) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    std::wcerr << L"Last engine status: " << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for storage recovery");
}

void checked(HRESULT result, const char* reason) {
    if (FAILED(result)) {
        std::wcerr << lapse::errorText(result) << L'\n';
        throw std::runtime_error(reason);
    }
}

std::filesystem::path recordingPath(const std::filesystem::path& directory) {
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().wstring();
        if (name.find(L".recording.mp4") == std::wstring::npos) continue;
        require(found.empty(), "More than one pending movie before collision test");
        found = entry.path();
    }
    require(!found.empty(), "The active recording has no temporary movie");
    return found;
}

void writeSentinel(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Could not create rename collision fixture");
    DWORD written = 0;
    const bool ok = WriteFile(file, sentinel, sizeof(sentinel) - 1, &written, nullptr) != FALSE;
    CloseHandle(file);
    require(ok && written == sizeof(sentinel) - 1, "Could not write rename collision fixture");
}

void verifySentinel(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "The existing destination disappeared");
    const std::string actual((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(actual == sentinel, "A recording overwrote the existing destination");
}

void verifyVideo(const std::filesystem::path& path, uint64_t expectedFrames) {
    require(expectedFrames > 0, "Cannot verify an empty recorded movie");
    require(std::filesystem::file_size(path) > 0, "Recorded movie is empty");
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Could not open retained MP4");
    constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE),
            "Could not deselect source streams");
    checked(reader->SetStreamSelection(videoStream, TRUE), "Could not select recorded video");
    ComPtr<IMFMediaType> decodedType;
    checked(MFCreateMediaType(&decodedType), "Could not create decoded video type");
    checked(decodedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Could not configure decoder");
    checked(decodedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Could not request decoded pixels");
    checked(reader->SetCurrentMediaType(videoStream, nullptr, decodedType.Get()),
            "Retained movie has no decodable video stream");
    uint64_t frames = 0;
    bool ended = false;
    for (uint64_t attempt = 0; attempt < expectedFrames + 100; ++attempt) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        checked(reader->ReadSample(videoStream, 0, nullptr, &flags, nullptr, &sample),
                "Retained movie contains an unreadable frame");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Retained movie reported a decode error");
        if (sample) {
            ComPtr<IMFMediaBuffer> pixels;
            checked(sample->ConvertToContiguousBuffer(&pixels), "Decoded frame has no pixels");
            DWORD bytes = 0;
            checked(pixels->GetCurrentLength(&bytes), "Could not read decoded frame size");
            require(bytes >= 320 * 180 * 3 / 2, "Decoded movie frame is incomplete");
            ++frames;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && frames == expectedFrames, "Retained movie lost recorded frames");
}
}

namespace lapse {
struct CameraClient::Impl {};
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring&, std::wstring& error) {
    error.clear(); return true;
}
void CameraClient::stop() {}
bool CameraClient::latest(Frame&, std::wstring& error) {
    // Never deliver the first camera frame so cancellation is deterministic.
    error.clear(); ++cameraPolls; return false;
}
bool captureDesktop(const RECT&, int width, int height, bool, Frame& output, std::wstring& error) {
    error.clear();
    output = {width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4, 96)};
    return true;
}
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    int result = 0;
    const auto directory = std::filesystem::current_path() /
        (L"engine-storage-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        require(std::filesystem::create_directory(directory), "Could not create isolated storage test directory");
        {
            lapse::Settings settings;
            settings.layers = lapse::preset(lapse::Mode::Desktop);
            settings.width = 320; settings.height = 180; settings.interval = 60;
            settings.folder = directory.wstring(); settings.preview = false;
            lapse::Engine engine;
            engine.configure(settings); engine.record();
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
            engine.pause();
            const auto paused = await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
            const auto temporary = recordingPath(directory);
            const auto temporaryName = temporary.wstring();
            const auto suffix = temporaryName.rfind(L".recording.mp4");
            require(suffix != std::wstring::npos, "Unexpected temporary filename");
            const std::filesystem::path destination(temporaryName.substr(0, suffix) + L".mp4");
            writeSentinel(destination);

            engine.finish();
            const auto failure = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
            require(failure.error, "A real destination collision was reported as success");
            require(failure.savedPath == temporary.wstring(), "Rename failure did not identify the retained playable movie");
            require(failure.message.find(temporary.wstring()) != std::wstring::npos,
                    "Rename failure did not tell the user where the movie was retained");
            require(failure.message.find(L"Partial file:") == std::wstring::npos,
                    "A finalized movie was incorrectly described as a partial file");
            verifySentinel(destination);
            verifyVideo(temporary, paused.frames);

            settings.preview = true; engine.configure(settings);
            await(engine, [&](const auto& s) { return s.preview && s.preview != failure.preview; });
            const auto preview = engine.status();
            require(preview.error && preview.message == failure.message && preview.savedPath == failure.savedPath,
                    "Successful idle preview erased the save error or retained path");

            settings.preview = false; engine.configure(settings); engine.record();
            require(engine.status().savedPath.empty(), "A new recording retained the previous movie's saved path");
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
            engine.finish();
            const auto recovered = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
            require(!recovered.error && !recovered.savedPath.empty(), "Recording did not recover after rename failure");
            require(recovered.savedPath != temporary.wstring() && recovered.savedPath != destination.wstring(),
                    "Recovered recording reused a preserved movie's path");
            verifyVideo(recovered.savedPath, recovered.frames);
            verifyVideo(temporary, paused.frames);
            verifySentinel(destination);

            settings.layers = lapse::preset(lapse::Mode::Camera);
            settings.cameraId = L"controlled-warmup-camera";
            engine.configure(settings); engine.record();
            await(engine, [](const auto& s) { return s.state == lapse::State::Starting && cameraPolls > 0; });
            engine.finish();
            const auto cancelled = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
            require(!cancelled.error && cancelled.frames == 0 && cancelled.savedPath.empty(),
                    "Cancelling before the first frame reported a prior movie or a save error");
            require(recordingPath(directory) == temporary, "Cancellation left an empty temporary movie");

            settings.layers = lapse::preset(lapse::Mode::Desktop);
            settings.folder = destination.wstring(); // A file cannot be a save directory.
            engine.configure(settings); engine.record();
            const auto rejected = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
            require(rejected.frames == 0 && rejected.savedPath.empty(), "A rejected save directory reported a stale movie");
            require(recordingPath(directory) == temporary, "Failed recording start left an empty temporary movie");
            verifySentinel(destination);
        }
        std::filesystem::remove_all(directory);
        std::cout << "Engine storage: real rename collision, retained MP4 decoding, protected destination, sticky error, retry and cancellation passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Artifacts kept at " << directory.wstring() << L'\n';
        result = 1;
    }
    MFShutdown(); CoUninitialize();
    return result;
}

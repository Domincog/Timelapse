#include "encoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <psapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
constexpr int width = 1920, height = 1080, fps = 30, frameCount = 600;
constexpr int warmupFrames = 120;
constexpr uint64_t mib = 1024 * 1024;
constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD allStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD mediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void checked(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::wcerr << lapse::errorText(hr) << L'\n';
        throw std::runtime_error(message);
    }
}
void completed(bool value, const std::wstring& error) {
    if (!value) {
        std::wcerr << error << L'\n';
        throw std::runtime_error("Recording operation failed");
    }
}
void withinDeadline(ULONGLONG start) {
    require(GetTickCount64() - start < 80000, "Recording soak exceeded its 80-second work budget");
}

struct Resources {
    uint64_t privateBytes = 0;
    DWORD handles = 0;
};
Resources resources() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    require(K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE,
            "Could not read process memory usage");
    Resources result;
    result.privateBytes = memory.PrivateUsage;
    require(GetProcessHandleCount(GetCurrentProcess(), &result.handles) != FALSE,
            "Could not read process handle count");
    return result;
}
struct ResourceMonitor {
    Resources baseline, peak;
    void sample(int frames) {
        const auto current = resources();
        peak.privateBytes = std::max(peak.privateBytes, current.privateBytes);
        peak.handles = std::max(peak.handles, current.handles);
        if (frames == warmupFrames) baseline = current;
        // Private committed bytes avoid working-set changes caused by unrelated
        // system memory pressure. These deliberately generous limits tolerate
        // codec startup/caches while catching accumulated frames or handles.
        if (current.privateBytes > 512 * mib || (frames >= warmupFrames &&
            (current.privateBytes > baseline.privateBytes + 128 * mib || current.handles > baseline.handles + 32))) {
            std::cerr << "Resource budget at frame " << frames << ": private memory "
                      << current.privateBytes / mib << " MiB, handles " << current.handles << '\n';
        }
        require(current.privateBytes <= 512 * mib, "Recording private memory exceeded 512 MiB");
        if (frames >= warmupFrames) {
            require(current.privateBytes <= baseline.privateBytes + 128 * mib,
                    "Recording memory grew by more than 128 MiB after warmup");
            require(current.handles <= baseline.handles + 32,
                    "Recording handles grew by more than 32 after warmup");
        }
    }
};

void fill(lapse::Frame& frame, uint8_t blue, uint8_t green, uint8_t red) {
    for (size_t i = 0; i < frame.pixels.size(); i += 4) {
        frame.pixels[i] = blue;
        frame.pixels[i + 1] = green;
        frame.pixels[i + 2] = red;
        frame.pixels[i + 3] = 255;
    }
}
int layoutAt(int index) { return (index / 24) % 5; }
std::vector<lapse::Layer> layersAt(int index) {
    using lapse::Mode;
    using lapse::Source;
    switch (layoutAt(index)) {
    case 0: return lapse::preset(Mode::Desktop);
    case 1: return lapse::preset(Mode::Camera);
    case 2: return lapse::preset(Mode::SideBySide);
    case 3: return {{Source::Desktop, {}}, {Source::Camera, {0, 0, 0.5, 0.5}}};
    default: return {{Source::Desktop, {}}, {Source::Camera, {0.75, 0.75, 0.25, 0.25}}};
    }
}

struct Yuv { int y, u, v; };
struct LockedPixels {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* data = nullptr;
    LONG stride = width;
    explicit LockedPixels(IMFSample* sample) {
        checked(sample->ConvertToContiguousBuffer(&buffer), "No decoded pixels");
        if (SUCCEEDED(buffer.As(&buffer2d))) {
            checked(buffer2d->Lock2D(&data, &stride), "Could not lock decoded 2D pixels");
        } else {
            DWORD length = 0;
            checked(buffer->Lock(&data, nullptr, &length), "Could not lock decoded pixels");
            if (length < width * height * 3 / 2) {
                buffer->Unlock();
                data = nullptr;
                throw std::runtime_error("Decoded buffer is too short");
            }
        }
    }
    ~LockedPixels() {
        if (!data) return;
        if (buffer2d) buffer2d->Unlock2D();
        else buffer->Unlock();
    }
    void color(int x, int y, Yuv expected, int index) const {
        const int actualY = data[y * stride + x];
        const BYTE* chroma = data + height * stride + (y / 2) * stride + (x & ~1);
        if (std::abs(actualY - expected.y) > 12 || std::abs(chroma[0] - expected.u) > 12 ||
            std::abs(chroma[1] - expected.v) > 12) {
            std::cerr << "Frame " << index << ", position " << x << ',' << y << ": YUV "
                      << actualY << ',' << int(chroma[0]) << ',' << int(chroma[1]) << '\n';
            throw std::runtime_error("Decoded collage colors or placement are wrong");
        }
    }
};

void verifyColors(IMFSample* sample, int index) {
    // Independently specified limited-range BT.709 reference colors. Sources
    // change halfway through the recording; no expected output frame is cached.
    const Yuv desktop = index < frameCount / 2 ? Yuv{63, 102, 240} : Yuv{32, 240, 118};
    const Yuv camera = index < frameCount / 2 ? Yuv{173, 42, 26} : Yuv{235, 128, 128};
    const Yuv dark{33, 128, 128};
    LockedPixels pixels(sample);
    require(pixels.stride >= width, "Decoded NV12 must be top-down");
    switch (layoutAt(index)) {
    case 0:
    case 1:
        pixels.color(480, 270, layoutAt(index) == 0 ? desktop : camera, index);
        pixels.color(1440, 810, layoutAt(index) == 0 ? desktop : camera, index);
        break;
    case 2:
        pixels.color(480, 540, desktop, index);
        pixels.color(1440, 540, camera, index);
        pixels.color(480, 120, dark, index);
        pixels.color(1440, 960, dark, index);
        break;
    case 3:
        pixels.color(480, 270, camera, index);
        pixels.color(1440, 270, desktop, index);
        pixels.color(480, 810, desktop, index);
        pixels.color(1680, 945, desktop, index);
        break;
    default:
        pixels.color(1680, 945, camera, index);
        pixels.color(480, 270, desktop, index);
        pixels.color(1200, 700, desktop, index);
        pixels.color(480, 810, desktop, index);
        break;
    }
}

void verifyVideo(const std::filesystem::path& path, ULONGLONG start) {
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Could not open recorded MP4");
    checked(reader->SetStreamSelection(allStreams, FALSE), "Could not deselect other streams");
    checked(reader->SetStreamSelection(videoStream, TRUE), "Could not select video");
    ComPtr<IMFMediaType> native;
    checked(reader->GetNativeMediaType(videoStream, 0, &native), "No encoded video type");
    GUID subtype{};
    checked(native->GetGUID(MF_MT_SUBTYPE, &subtype), "No encoded video subtype");
    require(subtype == MFVideoFormat_H264, "Recorded video is not H.264");
    UINT32 encodedWidth = 0, encodedHeight = 0;
    checked(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &encodedWidth, &encodedHeight), "No video dimensions");
    require(encodedWidth == width && encodedHeight == height, "Recorded video is not 1920 by 1080");
    PROPVARIANT duration;
    PropVariantInit(&duration);
    checked(reader->GetPresentationAttribute(mediaSource, MF_PD_DURATION, &duration), "No MP4 duration");
    const int64_t expectedDuration = int64_t(frameCount) * 10000000 / fps;
    const bool validDuration = duration.vt == VT_UI8 &&
        std::llabs(int64_t(duration.uhVal.QuadPart) - expectedDuration) < 20000;
    PropVariantClear(&duration);
    require(validDuration, "MP4 duration does not match 600 frames at 30 fps");

    ComPtr<IMFMediaType> output;
    checked(MFCreateMediaType(&output), "Could not allocate decoder type");
    checked(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Could not set video major type");
    checked(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Could not select NV12 decoder output");
    checked(reader->SetCurrentMediaType(videoStream, nullptr, output.Get()), "Could not decode recorded H.264");
    int count = 0;
    bool ended = false;
    for (int attempt = 0; attempt < frameCount + 100; ++attempt) {
        withinDeadline(start);
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        checked(reader->ReadSample(videoStream, 0, nullptr, &flags, &timestamp, &sample), "MP4 decode failed");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Decoder signaled a stream error");
        if (sample) {
            require(count < frameCount, "Decoder produced extra frames");
            require(std::llabs(timestamp - int64_t(count) * 10000000 / fps) <= 1,
                    "Recorded frame timestamps drifted or reordered");
            verifyColors(sample.Get(), count);
            ++count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && count == frameCount, "Recorded MP4 lost frames");
}

void run(const std::filesystem::path& directory) {
    const auto start = GetTickCount64();
    const auto path = directory / L"1080p-soak.mp4";
    lapse::Frame desktop{320, 180, std::vector<uint8_t>(320 * 180 * 4)};
    lapse::Frame camera{160, 90, std::vector<uint8_t>(160 * 90 * 4)};
    lapse::Frame composed;
    fill(desktop, 0, 0, 255);
    fill(camera, 0, 255, 0);
    std::wstring error;
    lapse::Encoder encoder;
    ResourceMonitor monitor;
    completed(encoder.open(path.wstring(), width, height, fps, error), error);
    monitor.sample(0);
    for (int index = 0; index < frameCount; ++index) {
        withinDeadline(start);
        if (index == frameCount / 2) {
            fill(desktop, 255, 0, 0);
            fill(camera, 255, 255, 255);
        }
        completed(lapse::compose(&desktop, &camera, layersAt(index), width, height, composed, error), error);
        completed(encoder.write(composed, error), error);
        if ((index + 1) % 30 == 0) monitor.sample(index + 1);
    }
    require(encoder.frames() == frameCount, "Encoder did not accept all 600 frames");
    completed(encoder.finish(error), error);
    monitor.sample(frameCount);
    withinDeadline(start);
    const auto size = std::filesystem::file_size(path);
    require(size > 1000 && size <= 40 * mib, "Soak MP4 is empty or exceeds its 40 MiB size budget");
    const auto encodeMilliseconds = GetTickCount64() - start;
    verifyVideo(path, start);
    withinDeadline(start);
    std::cout << "1080p recording soak: 600 decoded frames, 20 seconds, five changing layouts, "
                 "timestamps and colors passed.\n"
              << "Private memory after warmup: " << monitor.baseline.privateBytes / mib
              << " MiB; peak: " << monitor.peak.privateBytes / mib
              << " MiB. Handles after warmup: " << monitor.baseline.handles
              << "; peak: " << monitor.peak.handles
              << ". MP4: " << size << " bytes. Encode: " << encodeMilliseconds
              << " ms; total: " << GetTickCount64() - start << " ms.\n";
}
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) { CoUninitialize(); return 1; }
    int result = 0;
    const auto directory = std::filesystem::current_path() /
        (L"recording-soak-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        require(std::filesystem::create_directory(directory), "Could not create an isolated soak directory");
        run(directory);
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nArtifacts kept at " << directory.string() << '\n';
        result = 1;
    }
    MFShutdown();
    CoUninitialize();
    return result;
}

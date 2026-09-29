#include "encoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
namespace {
constexpr int width = 320, height = 240, fps = 30, frameCount = 61;
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
void encoded(bool value, const std::wstring& error) {
    if (!value) {
        std::wcerr << error << L'\n';
        throw std::runtime_error("Encoder operation failed");
    }
}

lapse::Frame pattern(int index) {
    lapse::Frame frame{width, height, std::vector<uint8_t>(width * height * 4)};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* pixel = frame.pixels.data() + (size_t(y) * width + x) * 4;
            if (y < height / 2 && x < width / 2) pixel[index % 2 ? 0 : 2] = 255;
            else if (y < height / 2) pixel[1] = 255;
            else if (x < width / 2) pixel[0] = pixel[1] = pixel[2] = 255;
            pixel[3] = 255;
        }
    }
    return frame;
}

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
                buffer->Unlock(); data = nullptr;
                throw std::runtime_error("Decoded buffer is too short");
            }
        }
    }
    ~LockedPixels() {
        if (!data) return;
        if (buffer2d) buffer2d->Unlock2D();
        else buffer->Unlock();
    }
    void color(int x, int y, int expectedY, int expectedU, int expectedV) const {
        const int actualY = data[y * stride + x];
        const BYTE* chroma = data + height * stride + (y / 2) * stride + (x & ~1);
        if (std::abs(actualY - expectedY) > 12 || std::abs(chroma[0] - expectedU) > 12 || std::abs(chroma[1] - expectedV) > 12) {
            std::cerr << "Color at " << x << ',' << y << ": YUV " << actualY << ',' << int(chroma[0]) << ',' << int(chroma[1]) << '\n';
            throw std::runtime_error("Decoded color or orientation is wrong");
        }
    }
};

void verifyVideo(const std::filesystem::path& path, int expectedFrames = frameCount) {
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Could not open encoded video");
    checked(reader->SetStreamSelection(allStreams, FALSE), "Could not deselect other streams");
    checked(reader->SetStreamSelection(videoStream, TRUE), "Could not select video");
    ComPtr<IMFMediaType> native;
    checked(reader->GetNativeMediaType(videoStream, 0, &native), "Could not read native video type");
    GUID subtype{};
    checked(native->GetGUID(MF_MT_SUBTYPE, &subtype), "No encoded video subtype");
    require(subtype == MFVideoFormat_H264, "Output is not H.264");
    UINT32 encodedWidth = 0, encodedHeight = 0;
    checked(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &encodedWidth, &encodedHeight), "No encoded dimensions");
    require(encodedWidth == width && encodedHeight == height, "Wrong encoded dimensions");
    PROPVARIANT duration;
    PropVariantInit(&duration);
    checked(reader->GetPresentationAttribute(mediaSource, MF_PD_DURATION, &duration), "No MP4 duration");
    const int64_t expectedDuration = int64_t(expectedFrames) * 10000000 / fps;
    const bool validDuration = duration.vt == VT_UI8 && std::llabs(int64_t(duration.uhVal.QuadPart) - expectedDuration) < 20000;
    PropVariantClear(&duration);
    require(validDuration, "MP4 duration differs from frame count / playback rate");

    ComPtr<IMFMediaType> output;
    checked(MFCreateMediaType(&output), "Could not allocate decoder type");
    checked(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Could not set video major type");
    checked(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Could not set NV12 decoder output");
    checked(reader->SetCurrentMediaType(videoStream, nullptr, output.Get()), "Could not decode H.264 to NV12");
    int count = 0;
    int attempts = 0;
    bool ended = false;
    while (++attempts < expectedFrames + 100) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        checked(reader->ReadSample(videoStream, 0, nullptr, &flags, &timestamp, &sample), "Decode failed");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Decoder signaled a stream error");
        if (sample) {
            require(count < expectedFrames, "Decoder produced unexpected extra frames");
            if (std::llabs(timestamp - int64_t(count) * 10000000 / fps) > 1) {
                std::cerr << "Frame " << count << " timestamp " << timestamp << " expected " << int64_t(count) * 10000000 / fps << '\n';
                throw std::runtime_error("Frame timestamps have drifted or reordered");
            }
            LockedPixels pixels(sample.Get());
            require(pixels.stride >= width, "NV12 must be top-down");
            // Independently specified limited-range BT.709 reference colors.
            if (count % 2) pixels.color(80, 60, 32, 240, 118); // blue
            else pixels.color(80, 60, 63, 102, 240);          // red
            pixels.color(240, 60, 173, 42, 26);             // green
            pixels.color(80, 180, 235, 128, 128);           // white
            pixels.color(240, 180, 16, 128, 128);            // black
            ++count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && count == expectedFrames, "Decoded frame count is wrong");
}

void run(const std::filesystem::path& directory) {
    const auto path = directory / L"roundtrip.mp4";
    std::wstring error;
    lapse::Encoder encoder;
    require(encoder.finish(error) && encoder.finish(error), "Finish before open should be harmless");
    require(!encoder.write(pattern(0), error) && !error.empty(), "Write before open must fail");
    require(!encoder.open(L"", width, height, fps, error), "Empty path accepted");
    require(!encoder.open(std::wstring(L"bad\0path.mp4", 12), width, height, fps, error), "Embedded NUL accepted");
    require(!encoder.open(path.wstring(), 321, height, fps, error), "Odd width accepted");
    require(!encoder.open(path.wstring(), width, -1, fps, error), "Negative height accepted");
    require(!encoder.open(path.wstring(), 8192, height, fps, error), "Oversize dimensions accepted");
    require(!encoder.open(path.wstring(), width, height, 0, error), "Zero playback rate accepted");
    require(!encoder.open(path.wstring(), width, height, 121, error), "Excessive playback rate accepted");
    require(!std::filesystem::exists(path), "Invalid settings created an output file");
    encoded(encoder.open(path.wstring(), width, height, fps, error), error);
    require(!encoder.open((directory / L"second.mp4").wstring(), width, height, fps, error), "Open while recording must fail");
    auto badFrame = pattern(0); badFrame.pixels.pop_back();
    require(!encoder.write(badFrame, error) && encoder.frames() == 0, "Malformed frame was counted");
    for (int i = 0; i < frameCount; ++i) encoded(encoder.write(pattern(i), error), error);
    require(encoder.frames() == frameCount, "Written frame count is wrong");
    encoded(encoder.finish(error), error);
    encoded(encoder.finish(error), error);
    require(!encoder.write(pattern(0), error), "Write after finish must fail");
    const auto size = std::filesystem::file_size(path);
    require(size > 1000, "MP4 output is unexpectedly empty");
    require(!encoder.open(path.wstring(), width, height, fps, error), "Existing video was overwritten");
    require(std::filesystem::file_size(path) == size, "Existing video changed after refused overwrite");
    verifyVideo(path);

    const auto emptyPath = directory / L"empty.mp4";
    encoded(encoder.open(emptyPath.wstring(), width, height, fps, error), error);
    require(!encoder.finish(error) && !error.empty(), "Empty recording should report no captured frames");
    const auto emptyError = error;
    require(!encoder.finish(error) && error == emptyError, "Repeated failed finish must be stable");
    require(!std::filesystem::exists(emptyPath), "Empty recording left a broken video");
    const auto onePath = directory / L"one-frame.mp4";
    encoded(encoder.open(onePath.wstring(), width, height, fps, error), error);
    encoded(encoder.write(pattern(0), error), error);
    encoded(encoder.finish(error), error);
    require(encoder.frames() == 1, "Reopened encoder did not reset its frame count");
    verifyVideo(onePath, 1);
}
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) { CoUninitialize(); return 1; }
    int result = 0;
    // CTest runs in the build directory. Keep all artifacts beneath it and use
    // a unique directory so parallel runs cannot remove each other's files.
    const auto directory = std::filesystem::current_path() / (L"encoder-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        std::filesystem::create_directory(directory);
        run(directory);
        std::filesystem::remove_all(directory);
        std::cout << "Encoder roundtrip: 61 H.264 frames, timestamps, duration, colors, orientation and lifecycle passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nArtifacts kept at " << directory.string() << '\n';
        result = 1;
    }
    MFShutdown();
    CoUninitialize();
    return result;
}

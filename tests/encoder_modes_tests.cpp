#include "encoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cmath>

HRESULT WINAPI modeTestCreateWriter(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**);
// Make a real sink writer select software for one explicit hardware request.
// This exercises fallback rejection even on a CI machine with no GPU encoder.
#define MFCreateSinkWriterFromURL modeTestCreateWriter
#include "../src/encoder.cpp"
#undef MFCreateSinkWriterFromURL
namespace { bool forceSoftware = false; }
HRESULT WINAPI modeTestCreateWriter(LPCWSTR url, IMFByteStream* bytes, IMFAttributes* attributes, IMFSinkWriter** writer) {
    if (forceSoftware) attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    return MFCreateSinkWriterFromURL(url, bytes, attributes, writer);
}
using Microsoft::WRL::ComPtr;
namespace {
constexpr int width = 320, height = 240, fps = 30;
constexpr DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD media = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::wcerr << lapse::errorText(hr) << L'\n'; throw std::runtime_error(message); }
}
void encoded(bool value, const std::wstring& error) {
    if (!value) { std::wcerr << error << L'\n'; throw std::runtime_error("Encoder mode operation failed"); }
}
bool available(REFGUID subtype) {
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, subtype}; IMFActivate** list = nullptr; UINT32 count = 0;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &output, &list, &count), "Hardware encoder enumeration failed");
    for (UINT32 i = 0; i < count; ++i) list[i]->Release();
    CoTaskMemFree(list); return count != 0;
}
lapse::Frame pattern(int frameIndex) {
    lapse::Frame f{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        auto* p = &f.pixels[(size_t(y) * width + x) * 4];
        if (x < width / 2 && y < height / 2) p[frameIndex % 2 ? 0 : 2] = 255;
        else if (y < height / 2) p[1] = 255;
        else if (x < width / 2) p[0] = p[1] = p[2] = 255;
        p[3] = 255;
    }
    return f;
}
void verify(const std::filesystem::path& path, int count, bool hevc, int playbackFps = fps) {
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Open encoded mode output");
    ComPtr<IMFMediaType> native; check(reader->GetNativeMediaType(video, 0, &native), "Read native type");
    GUID subtype{}; check(native->GetGUID(MF_MT_SUBTYPE, &subtype), "Read codec");
    require(subtype == (hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264), "Requested codec was silently substituted");
    UINT32 w = 0, h = 0; check(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h), "Read size");
    if (w != width || h != height) {
        // HEVC can expose coded CTU padding (320x256 for visible 320x240).
        MFVideoArea area{}; UINT32 bytes = 0;
        HRESULT aperture = native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
            reinterpret_cast<BYTE*>(&area), sizeof(area), &bytes);
        if (FAILED(aperture)) aperture = native->GetBlob(MF_MT_GEOMETRIC_APERTURE,
            reinterpret_cast<BYTE*>(&area), sizeof(area), &bytes);
        require(SUCCEEDED(aperture) && bytes == sizeof(area) && w >= width && h >= height &&
            w < width + 64 && h < height + 64 && area.OffsetX.value == 0 && area.OffsetX.fract == 0 &&
            area.OffsetY.value == 0 && area.OffsetY.fract == 0 && area.Area.cx == width && area.Area.cy == height,
            "Wrong encoded dimensions or display crop");
    }
    PROPVARIANT duration{}; check(reader->GetPresentationAttribute(media, MF_PD_DURATION, &duration), "Read duration");
    // The MP4 muxer uses fps*1000 ticks/sec and can round the final sample
    // down by one container tick. Frame presentation times still match exactly.
    const bool exactDuration = duration.vt == VT_UI8 &&
        std::llabs(static_cast<LONGLONG>(duration.uhVal.QuadPart) - LONGLONG(count) * 10000000 / playbackFps) <= 10000000 / (playbackFps * 1000) + 2;
    PropVariantClear(&duration); require(exactDuration, "Mode output duration differs from captured frame count");
    // Compressed HEVC samples are readable even when the optional Windows HEVC
    // decoder is absent. Local independent FFmpeg QA covers decoded HEVC pixels.
    if (!hevc) {
        ComPtr<IMFMediaType> decoded; check(MFCreateMediaType(&decoded), "Create decoded type");
        check(decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Video type");
        check(decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "NV12 type");
        check(reader->SetCurrentMediaType(video, nullptr, decoded.Get()), "Decode H.264 mode");
    }
    int seen = 0; bool ended = false;
    for (int attempt = 0; attempt < count + 100; ++attempt) {
        DWORD flags = 0; LONGLONG time = 0; ComPtr<IMFSample> sample;
        check(reader->ReadSample(video, 0, nullptr, &flags, &time, &sample), "Read mode frame");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Frame reader error");
        if (sample) {
            require(seen < count && std::llabs(time - LONGLONG(seen) * 10000000 / playbackFps) <= 1,
                "Lost, shifted, or reordered mode frame");
            if (!hevc) {
                ComPtr<IMFMediaBuffer> buffer; check(sample->ConvertToContiguousBuffer(&buffer), "Get decoded frame");
                ComPtr<IMF2DBuffer> buffer2d; BYTE* data = nullptr; LONG stride = width; DWORD length = 0;
                if (SUCCEEDED(buffer.As(&buffer2d))) check(buffer2d->Lock2D(&data, &stride), "Lock2D");
                else check(buffer->Lock(&data, nullptr, &length), "Lock decoded frame");
                const bool valid = stride >= width && (buffer2d || length >= DWORD(width * height * 3 / 2));
                bool colors = valid;
                if (valid) {
                    const int redOrBlue = seen % 2 ? 32 : 63;
                    colors = std::abs(int(data[60 * stride + 80]) - redOrBlue) <= 12 &&
                        std::abs(int(data[60 * stride + 240]) - 173) <= 12 &&
                        std::abs(int(data[180 * stride + 80]) - 235) <= 12 &&
                        std::abs(int(data[180 * stride + 240]) - 16) <= 12;
                }
                if (buffer2d) buffer2d->Unlock2D(); else buffer->Unlock();
                require(colors, "New mode changed decoded luma colors or orientation");
            }
            ++seen;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && seen == count, "Mode output lost short-recording frames");
}
void exercise(const std::filesystem::path& directory, lapse::EncodingMode mode) {
    const bool hevc = mode == lapse::EncodingMode::HardwareHEVC;
    if ((mode == lapse::EncodingMode::HardwareH264 || hevc) && !available(hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264)) {
        std::cout << "Hardware mode " << int(mode) << " absent (independent MFT enumeration); skipped.\n"; return;
    }
    lapse::Encoder encoder; std::wstring error;
    for (auto quality : {lapse::EncodingQuality::Compact, lapse::EncodingQuality::Balanced, lapse::EncodingQuality::Detail}) {
        for (int count : {0, 1, 2, 31}) {
            const auto name = std::to_wstring(int(mode)) + L"-" + std::to_wstring(int(quality)) + L"-" + std::to_wstring(count);
            const auto path = directory / (name + L".recording.mp4"), published = directory / (name + L".mp4");
            encoded(encoder.open(path.wstring(), width, height, fps, error, quality, mode), error);
            require(!MoveFileExW(path.c_str(), published.c_str(), 0) && GetLastError() == ERROR_SHARING_VIOLATION,
                "Encoding mode weakened active output ownership");
            auto invalid = pattern(0); invalid.pixels.pop_back();
            require(!encoder.write(invalid, error) && encoder.frames() == 0, "Malformed mode input counted");
            for (int i = 0; i < count; ++i) encoded(encoder.write(pattern(i), error), error);
            if (!count) {
                require(!encoder.finishForPublication(error) && error == L"No video frames were recorded." &&
                    !std::filesystem::exists(path), "Empty mode output was not removed");
                const auto previous = error;
                require(!encoder.finish(error) && error == previous, "Empty mode terminal result changed");
            } else {
                encoded(encoder.finishForPublication(error), error);
                require(encoder.frames() == uint64_t(count), "Mode frame counter incorrect");
                require(encoder.publish(published.wstring()) == ERROR_SUCCESS, "Publish mode output");
                encoder.releasePublication(); encoded(encoder.finish(error), error);
                verify(published, count, hevc);
                const auto size = std::filesystem::file_size(published);
                require(!encoder.open(published.wstring(), width, height, fps, error, quality, mode) &&
                    std::filesystem::file_size(published) == size, "Mode overwritten existing video");
            }
        }
    }
    for (int rate : {24, 60}) {
        const auto path = directory / (L"rate-" + std::to_wstring(int(mode)) + L"-" + std::to_wstring(rate) + L".mp4");
        encoded(encoder.open(path.wstring(), width, height, rate, error, lapse::EncodingQuality::Balanced, mode), error);
        for (int i = 0; i < 3; ++i) encoded(encoder.write(pattern(i), error), error);
        encoded(encoder.finish(error), error); verify(path, 3, hevc, rate);
    }
    std::cout << "Mode " << int(mode) << ": all qualities, 0/1/2/31 frames, timestamps, duration, publication and collisions passed.\n";
}
void rejection(const std::filesystem::path& directory) {
    lapse::Encoder encoder; std::wstring error; const auto path = directory / L"rejected.mp4";
    for (int invalid : {-1, 5})
        require(!encoder.open(path.wstring(), width, height, fps, error, lapse::EncodingQuality::Balanced,
            static_cast<lapse::EncodingMode>(invalid)) && !std::filesystem::exists(path), "Invalid mode created a file");
    require(!encoder.open(path.wstring(), width, height, fps, error, static_cast<lapse::EncodingQuality>(99),
        lapse::EncodingMode::Efficient) && !std::filesystem::exists(path), "Invalid efficient quality created a file");
    forceSoftware = true;
    const bool opened = encoder.open(path.wstring(), width, height, fps, error,
        lapse::EncodingQuality::Balanced, lapse::EncodingMode::HardwareH264);
    forceSoftware = false;
    require(!opened && error.find(L"hardware encoder is unavailable") != std::wstring::npos &&
        !std::filesystem::exists(path), "Explicit hardware silently fell back or leaked output");
    encoded(encoder.open(path.wstring(), width, height, fps, error, lapse::EncodingQuality::Balanced,
        lapse::EncodingMode::Efficient), error);
    encoded(encoder.write(pattern(0), error), error); encoded(encoder.finish(error), error); verify(path, 1, false);
}
}
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto directory = std::filesystem::current_path() /
        (L"encoder-modes-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        require(std::filesystem::create_directory(directory), "Create mode test directory");
        rejection(directory);
        for (auto mode : {lapse::EncodingMode::Compatible, lapse::EncodingMode::Efficient,
            lapse::EncodingMode::HardwareH264, lapse::EncodingMode::HardwareHEVC, lapse::EncodingMode::QualityH264}) exercise(directory, mode);
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) { std::cerr << error.what() << "\nArtifacts kept at " << directory.string() << '\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

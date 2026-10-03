#include "encoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <cmath>
#include <array>

using Microsoft::WRL::ComPtr;
namespace {
constexpr int width = 1280, height = 720, fps = 30, frames = 45;
// AV1 Smaller file must be at least this many times smaller than Compatible
// Balanced. Measured minimum on this corpus: 2.5 (textured motion).
constexpr uint64_t av1CompactSizeRatio = 2;
constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void checked(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::wcerr << lapse::errorText(hr) << L'\n'; throw std::runtime_error(message); }
}
void encoded(bool ok, const std::wstring& error) {
    if (!ok) { std::wcerr << error << L'\n'; throw std::runtime_error("Encoding failed"); }
}

// Repeatable screen-like detail, panning textured gradients, and hard scene
// changes. This is a regression corpus, not a claim about all real footage.
lapse::Frame pattern(int scene, int index) {
    lapse::Frame frame{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        auto* p = frame.pixels.data() + (size_t(y) * width + x) * 4;
        if (scene == 0) {
            int value = y < 45 ? 45 : x < 220 ? 224 : 248;
            if (x > 240 && y > 70 && y % 23 < 13 && x % 11 < 7 &&
                ((x / 11 * 17 + y / 23 * 31 + (y > 530 ? index / 3 : 0)) >> (x % 5)) % 3 == 0) value = 36;
            if (x > 700 + index * 3 && x < 880 + index * 3 && y > 170 && y < 430) value = 108 + (x + y) % 90;
            p[0] = p[1] = p[2] = static_cast<uint8_t>(value);
        } else {
            const int shift = index * (scene == 1 ? 9 : 23);
            const uint32_t seed = uint32_t((x + shift) / 3) * 2654435761u ^ uint32_t(y / 3) * 2246822519u;
            const int texture = int((seed >> 25) & 31) - 16;
            const int cut = scene == 2 ? (index / 9) * 47 : 0;
            p[0] = static_cast<uint8_t>(40 + ((x + shift + cut) / 5 + y / 7) % 165 + texture);
            p[1] = static_cast<uint8_t>(40 + ((x + shift) / 9 + y / 3 + cut) % 165 + texture);
            p[2] = static_cast<uint8_t>(40 + ((x + shift) / 4 + y / 11 + cut) % 165 + texture);
        }
        p[3] = 255;
    }
    return frame;
}

double verify(const std::filesystem::path& path, int scene) {
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Could not open benchmark video");
    ComPtr<IMFMediaType> type;
    checked(MFCreateMediaType(&type), "Could not create decoder type");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Could not select video");
    checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Could not select NV12");
    checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Could not decode benchmark video");
    ComPtr<IMFMediaType> actual;
    checked(reader->GetCurrentMediaType(stream, &actual), "Could not inspect decoded format");
    UINT32 decodedWidth = 0, decodedHeight = 0;
    checked(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &decodedWidth, &decodedHeight), "No decoded dimensions");
    require(decodedWidth >= width && decodedHeight >= height, "Decoded image is too small");
    const LONG defaultStride = static_cast<LONG>(MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, decodedWidth));
    uint64_t squaredError = 0;
    int count = 0;
    bool ended = false;
    for (int attempt = 0; attempt < frames + 100; ++attempt) {
        DWORD flags = 0; LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Benchmark decode failed");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Decoder error flag");
        if (sample) {
            require(count < frames, "Extra decoded frames");
            require(std::llabs(timestamp - int64_t(count) * 10000000 / fps) <= 1, "Frame timestamp drift or reordering");
            const auto expected = pattern(scene, count);
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMF2DBuffer> buffer2d;
            checked(sample->ConvertToContiguousBuffer(&buffer), "Could not get decoded pixels");
            BYTE* data = nullptr; LONG stride = defaultStride;
            if (SUCCEEDED(buffer.As(&buffer2d))) checked(buffer2d->Lock2D(&data, &stride), "Could not lock decoded image");
            else {
                DWORD length = 0;
                checked(buffer->Lock(&data, nullptr, &length), "Could not lock decoded buffer");
                if (stride < width || length < size_t(stride) * height) {
                    buffer->Unlock(); throw std::runtime_error("Decoded buffer has invalid dimensions");
                }
            }
            if (stride < width) {
                if (buffer2d) buffer2d->Unlock2D(); else buffer->Unlock();
                throw std::runtime_error("Decoded image is not top-down");
            }
            if (count == 0) std::cerr << "Decode stride " << stride << ", dimensions " << decodedWidth << 'x' << decodedHeight
                << ", 2D " << bool(buffer2d) << ", Y corners " << int(data[0]) << ',' << int(data[100 * stride + 100]) << ','
                << int(data[500 * stride + 500]) << '\n';
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const auto* p = expected.pixels.data() + (size_t(y) * width + x) * 4;
                const int luma = 16 + int(std::lround(0.182586 * p[2] + 0.614231 * p[1] + 0.062007 * p[0]));
                const int difference = int(data[size_t(y) * stride + x]) - luma;
                squaredError += uint64_t(difference * difference);
            }
            if (buffer2d) buffer2d->Unlock2D(); else buffer->Unlock();
            ++count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && count == frames, "Benchmark video lost frames");
    const double mse = double(squaredError) / (double(width) * height * frames);
    return mse == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}
struct Result { uint64_t bytes; double psnr; ULONGLONG milliseconds; };
#ifndef TIMELAPSE_BASELINE
// The Windows AV1 decoder is an optional Store extension; encoding never uses it.
bool av1Decoder() {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_AV1}; IMFActivate** list = nullptr; UINT32 count = 0;
    checked(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL & ~MFT_ENUM_FLAG_FIELDOFUSE, &input, nullptr, &list, &count),
        "AV1 decoder enumeration failed");
    for (UINT32 i = 0; i < count; ++i) list[i]->Release();
    CoTaskMemFree(list); return count != 0;
}
#endif
#ifdef TIMELAPSE_BASELINE
Result run(const std::filesystem::path& path, int scene, int quality) {
    lapse::Encoder encoder;
    std::wstring error;
    const auto start = GetTickCount64();
    (void)quality;
    encoded(encoder.open(path.wstring(), width, height, fps, error), error);
#else
Result run(const std::filesystem::path& path, int scene, int quality, lapse::EncodingMode mode = lapse::EncodingMode::Compatible) {
    lapse::Encoder encoder;
    std::wstring error;
    const auto start = GetTickCount64();
    encoded(encoder.open(path.wstring(), width, height, fps, error, static_cast<lapse::EncodingQuality>(quality), mode), error);
#endif
    for (int i = 0; i < frames; ++i) encoded(encoder.write(pattern(scene, i), error), error);
    encoded(encoder.finish(error), error);
    const auto elapsed = GetTickCount64() - start;
    return {std::filesystem::file_size(path), verify(path, scene), elapsed};
}
}
int main(int argc, char**) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    const auto directory = std::filesystem::current_path() /
        (L"encoding-quality-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int result = 0;
    try {
        require(std::filesystem::create_directory(directory), "Could not create benchmark directory");
        std::cout << "scene,quality,bytes,luma_psnr_db,encode_ms\n" << std::fixed << std::setprecision(3);
        std::array<std::array<Result, 3>, 3> compatible{};
        for (int scene = 0; scene < 3; ++scene) {
#ifdef TIMELAPSE_BASELINE
            constexpr int qualities = 1;
#else
            constexpr int qualities = 3;
#endif
            auto& results = compatible[size_t(scene)];
            for (int quality = 0; quality < qualities; ++quality) {
                const auto path = directory / (std::to_wstring(scene) + L"-" + std::to_wstring(quality) + L".mp4");
                results[quality] = run(path, scene, quality);
                const auto& r = results[quality];
                std::cout << scene << ',' << quality << ',' << r.bytes << ',' << r.psnr << ',' << r.milliseconds << '\n';
                require(r.psnr > 27, "Encoded image quality fell below the corpus floor");
            }
#ifndef TIMELAPSE_BASELINE
            require(argc > 1 || (results[0].bytes < results[1].bytes && results[1].bytes < results[2].bytes),
                    "Quality presets did not produce increasing file sizes");
            require(argc > 1 || (results[0].psnr < results[1].psnr && results[1].psnr < results[2].psnr),
                    "Quality presets did not preserve increasing image detail");
#endif
        }
#ifndef TIMELAPSE_BASELINE
        if (av1Decoder()) {
            std::cout << "av1_scene,quality,bytes,luma_psnr_db,encode_ms\n";
            for (int scene = 0; scene < 3; ++scene) {
                std::array<Result, 3> av1{};
                for (int quality = 0; quality < 3; ++quality) {
                    const auto path = directory / (L"av1-" + std::to_wstring(scene) + L"-" + std::to_wstring(quality) + L".mp4");
                    av1[size_t(quality)] = run(path, scene, quality, lapse::EncodingMode::SoftwareAV1);
                    const auto& r = av1[size_t(quality)];
                    std::cout << scene << ',' << quality << ',' << r.bytes << ',' << r.psnr << ',' << r.milliseconds << '\n';
                    require(r.psnr > 27, "AV1 image quality fell below the corpus floor");
                }
                // Near-lossless flat graphics can make More detail no larger
                // than Balanced; Smaller file must still be the smallest.
                require(argc > 1 || (av1[0].bytes < av1[1].bytes && av1[0].bytes < av1[2].bytes),
                        "AV1 Smaller file preset did not produce the smallest file");
                require(argc > 1 || (av1[0].psnr < av1[1].psnr && av1[1].psnr < av1[2].psnr),
                        "AV1 quality presets did not preserve increasing image detail");
                // The mode's purpose, against default Compatible H.264 Balanced:
                // AV1 Balanced is smaller with more detail, and even Smaller
                // file is several times smaller while keeping more detail.
                const auto& reference = compatible[size_t(scene)][1];
                require(argc > 1 || (av1[1].bytes < reference.bytes && av1[1].psnr > reference.psnr &&
                        av1[0].bytes * av1CompactSizeRatio < reference.bytes && av1[0].psnr > reference.psnr),
                        "AV1 lost its size or detail advantage over Compatible H.264");
            }
        } else std::cout << "SKIP AV1 quality: the optional Windows AV1 decoder is not installed.\n";
#endif
        if (argc > 1) std::cout << "Artifacts kept at " << directory.string() << '\n';
        else std::filesystem::remove_all(directory);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nArtifacts kept at " << directory.string() << '\n'; result = 1;
    }
    MFShutdown(); CoUninitialize(); return result;
}

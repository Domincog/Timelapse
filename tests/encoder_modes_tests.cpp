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
HRESULT WINAPI modeTestCreateSample(IMFSample**);
// Make a real sink writer select software for one explicit hardware request.
// This exercises fallback rejection even on a CI machine with no GPU encoder.
// The sample hook fails the AV1 drain after the codec has accepted input.
#define MFCreateSinkWriterFromURL modeTestCreateWriter
#define MFCreateSample modeTestCreateSample
#include "../src/encoder.cpp"
#undef MFCreateSample
#undef MFCreateSinkWriterFromURL
namespace { bool forceSoftware = false, failSample = false; }
HRESULT WINAPI modeTestCreateWriter(LPCWSTR url, IMFByteStream* bytes, IMFAttributes* attributes, IMFSinkWriter** writer) {
    if (forceSoftware) attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    return MFCreateSinkWriterFromURL(url, bytes, attributes, writer);
}
HRESULT WINAPI modeTestCreateSample(IMFSample** sample) { return failSample ? E_OUTOFMEMORY : MFCreateSample(sample); }
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
// The Windows AV1 decoder is an optional Store extension; encoding never uses it.
bool av1Decoder() {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_AV1}; IMFActivate** list = nullptr; UINT32 count = 0;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL & ~MFT_ENUM_FLAG_FIELDOFUSE,
        &input, nullptr, &list, &count), "AV1 decoder enumeration failed");
    for (UINT32 i = 0; i < count; ++i) list[i]->Release();
    CoTaskMemFree(list); return count != 0;
}
GUID codec(lapse::EncodingMode mode) {
    return mode == lapse::EncodingMode::HardwareHEVC ? MFVideoFormat_HEVC :
        mode == lapse::EncodingMode::SoftwareAV1 ? MFVideoFormat_AV1 : MFVideoFormat_H264;
}
uint32_t be32(const std::vector<uint8_t>& d, size_t at) {
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
}
// Finds the first box of a type along the path to the video sample entry.
bool findBox(const std::vector<uint8_t>& d, size_t begin, size_t end, const char* type, size_t& payload, size_t& bytes) {
    for (size_t at = begin; at + 8 <= end;) {
        uint64_t length = be32(d, at); size_t header = 8;
        if (length == 1) { if (at + 16 > end) return false; length = uint64_t(be32(d, at + 8)) << 32 | be32(d, at + 12); header = 16; }
        else if (!length) length = end - at;
        if (length < header || length > end - at) return false;
        const std::string kind(reinterpret_cast<const char*>(&d[at + 4]), 4);
        if (kind == type) { payload = at + header; bytes = size_t(length) - header; return true; }
        size_t children = 0;
        if (kind == "moov" || kind == "trak" || kind == "mdia" || kind == "minf" || kind == "stbl") children = header;
        else if (kind == "stsd") children = header + 8;
        else if (kind == "av01") children = header + 78;
        if (children && children <= length && findBox(d, at + children, at + size_t(length), type, payload, bytes)) return true;
        at += size_t(length);
    }
    return false;
}
struct Obu { int type; size_t offset, length; };
// Splits a sample or av1C configOBUs into OBUs; every OBU must carry a size.
std::vector<Obu> obus(const uint8_t* data, size_t size) {
    std::vector<Obu> result;
    for (size_t at = 0; at < size;) {
        require(!(data[at] & 0x80) && (data[at] & 0x02), "AV1 OBU lacks a size field or sets the forbidden bit");
        size_t cursor = at + ((data[at] & 0x04) ? 2 : 1); uint64_t payload = 0;
        for (int byte = 0; ; ++byte) {
            require(byte < 8 && cursor < size, "Truncated AV1 OBU size");
            payload |= uint64_t(data[cursor] & 0x7f) << (7 * byte);
            if (!(data[cursor++] & 0x80)) break;
        }
        require(payload <= size - cursor, "AV1 OBU exceeds its sample");
        result.push_back({(data[at] >> 3) & 0x0f, at, cursor - at + size_t(payload)});
        at = cursor + size_t(payload);
    }
    return result;
}
struct Av1Sample { std::vector<uint8_t> bytes; bool cleanPoint; };
std::vector<Av1Sample> compressedSamples(const std::filesystem::path& path) {
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Open AV1 output for compressed samples");
    std::vector<Av1Sample> samples;
    for (;;) {
        DWORD flags = 0; LONGLONG time = 0; ComPtr<IMFSample> sample;
        check(reader->ReadSample(video, 0, nullptr, &flags, &time, &sample), "Read compressed AV1 sample");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Compressed AV1 reader error");
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer; check(sample->ConvertToContiguousBuffer(&buffer), "Get AV1 sample bytes");
            BYTE* data = nullptr; DWORD length = 0; check(buffer->Lock(&data, nullptr, &length), "Lock AV1 sample");
            Av1Sample copy{std::vector<uint8_t>(data, data + length), MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE};
            buffer->Unlock(); samples.push_back(std::move(copy));
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return samples;
    }
}
// Checks the ISO BMFF AV1 binding: av1C carries exactly the sequence header,
// colr signals BT.709 limited range, samples exclude temporal delimiters and
// padding, and sync samples are exactly the expected keyframes.
void verifyAv1(const std::filesystem::path& path, int count, const std::vector<int>& keyFrames) {
    std::ifstream in(path, std::ios::binary); require(bool(in), "Read AV1 MP4");
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t at = 0, bytes = 0;
    require(findBox(file, 0, file.size(), "av1C", at, bytes) && bytes > 4, "AV1 MP4 lacks av1C");
    require(file[at] == 0x81 && file[at + 1] >> 5 == 0 && (file[at + 2] & 0x7c) == 0x0c &&
        (file[at + 3] & 0xe0) == 0 && ((file[at + 3] & 0x10) || !(file[at + 3] & 0x0f)),
        "av1C does not describe 8-bit 4:2:0 Main profile with a valid presentation delay");
    const std::vector<uint8_t> sequence(file.begin() + at + 4, file.begin() + at + bytes);
    const auto config = obus(sequence.data(), sequence.size());
    require(config.size() == 1 && config[0].type == 1, "av1C configOBUs must be exactly one sequence header");
    const uint8_t nclx[] = {'n','c','l','x',0,1,0,1,0,1,0};
    require(findBox(file, 0, file.size(), "colr", at, bytes) && bytes == sizeof(nclx) &&
        std::equal(nclx, nclx + sizeof(nclx), file.begin() + at), "AV1 MP4 lacks BT.709 limited-range colr");
    const auto samples = compressedSamples(path);
    require(int(samples.size()) == count, "Compressed AV1 sample count differs");
    std::vector<int> sync;
    for (int i = 0; i < count; ++i) {
        const auto& sample = samples[size_t(i)];
        bool header = false;
        for (const auto& obu : obus(sample.bytes.data(), sample.bytes.size())) {
            require(obu.type != 2 && obu.type != 15, "AV1 sample contains a temporal delimiter or padding OBU");
            if (obu.type == 1) header = std::equal(sequence.begin(), sequence.end(), sample.bytes.begin() + obu.offset,
                sample.bytes.begin() + obu.offset + obu.length) && obu.length == sequence.size();
        }
        if (sample.cleanPoint) { sync.push_back(i); require(header, "AV1 keyframe lacks the av1C sequence header"); }
    }
    require(sync == keyFrames, "AV1 sync samples differ from the keyframe interval");
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
void verify(const std::filesystem::path& path, int count, lapse::EncodingMode mode, int playbackFps = fps, bool checkPattern = true) {
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Open encoded mode output");
    ComPtr<IMFMediaType> native; check(reader->GetNativeMediaType(video, 0, &native), "Read native type");
    GUID subtype{}; check(native->GetGUID(MF_MT_SUBTYPE, &subtype), "Read codec");
    require(subtype == codec(mode), "Requested codec was silently substituted");
    // Compressed HEVC samples are readable even when the optional Windows HEVC
    // decoder is absent. Local independent FFmpeg QA covers decoded HEVC pixels.
    // AV1 pixels are checked whenever the optional Windows AV1 decoder exists.
    const bool decode = checkPattern && mode != lapse::EncodingMode::HardwareHEVC &&
        (mode != lapse::EncodingMode::SoftwareAV1 || av1Decoder());
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
    if (decode) {
        ComPtr<IMFMediaType> decoded; check(MFCreateMediaType(&decoded), "Create decoded type");
        check(decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Video type");
        check(decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "NV12 type");
        check(reader->SetCurrentMediaType(video, nullptr, decoded.Get()), "Decode mode output");
    }
    int seen = 0; bool ended = false;
    for (int attempt = 0; attempt < count + 100; ++attempt) {
        DWORD flags = 0; LONGLONG time = 0; ComPtr<IMFSample> sample;
        check(reader->ReadSample(video, 0, nullptr, &flags, &time, &sample), "Read mode frame");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Frame reader error");
        if (sample) {
            require(seen < count && std::llabs(time - LONGLONG(seen) * 10000000 / playbackFps) <= 1,
                "Lost, shifted, or reordered mode frame");
            if (decode) {
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
    for (auto quality : {lapse::EncodingQuality::ExtraSmall, lapse::EncodingQuality::Compact,
                         lapse::EncodingQuality::Balanced, lapse::EncodingQuality::Detail}) {
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
                verify(published, count, mode);
                if (mode == lapse::EncodingMode::SoftwareAV1) verifyAv1(published, count, {0});
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
        encoded(encoder.finish(error), error); verify(path, 3, mode, rate);
    }
    std::cout << "Mode " << int(mode) << ": all qualities, 0/1/2/31 frames, timestamps, duration, publication and collisions passed.\n";
}
void rejection(const std::filesystem::path& directory) {
    lapse::Encoder encoder; std::wstring error; const auto path = directory / L"rejected.mp4";
    for (int invalid : {-1, 6})
        require(!encoder.open(path.wstring(), width, height, fps, error, lapse::EncodingQuality::Balanced,
            static_cast<lapse::EncodingMode>(invalid)) && !std::filesystem::exists(path), "Invalid mode created a file");
    require(!encoder.open(path.wstring(), width, height, fps, error, static_cast<lapse::EncodingQuality>(99),
        lapse::EncodingMode::Efficient) && !std::filesystem::exists(path), "Invalid efficient quality created a file");
    forceSoftware = true;
    const bool opened = encoder.open(path.wstring(), width, height, fps, error,
        lapse::EncodingQuality::Balanced, lapse::EncodingMode::HardwareH264);
    forceSoftware = false;
    require(!opened && error.find(L"hardware encoder is unavailable") != std::wstring::npos &&
        error.find(L"320 x 240") != std::wstring::npos && error.find(L"smaller video size") != std::wstring::npos &&
        !std::filesystem::exists(path), "Explicit hardware silently fell back or leaked output");
    encoded(encoder.open(path.wstring(), width, height, fps, error, lapse::EncodingQuality::Balanced,
        lapse::EncodingMode::Efficient), error);
    encoded(encoder.write(pattern(0), error), error); encoded(encoder.finish(error), error); verify(path, 1, lapse::EncodingMode::Efficient);
}
// Textured, moving content exercises inter prediction and encoder threads.
lapse::Frame texture(int frameIndex) {
    lapse::Frame f{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        uint32_t seed = uint32_t(x + frameIndex * 3) * 2654435761u ^ uint32_t(y) * 2246822519u;
        seed ^= seed >> 15;
        auto* p = &f.pixels[(size_t(y) * width + x) * 4];
        p[0] = uint8_t(seed); p[1] = uint8_t((x + y + frameIndex * 5) & 255); p[2] = uint8_t(seed >> 8); p[3] = 255;
    }
    return f;
}
void av1Specifics(const std::filesystem::path& directory) {
    const auto av1 = lapse::EncodingMode::SoftwareAV1;
    const auto balanced = lapse::EncodingQuality::Balanced;
    lapse::Encoder encoder; std::wstring error;
    // Keyframes are ten playback seconds apart: every tenth frame at 1 fps.
    const auto spaced = directory / L"av1-keyframes.mp4";
    encoded(encoder.open(spaced.wstring(), width, height, 1, error, balanced, av1), error);
    for (int i = 0; i < 25; ++i) encoded(encoder.write(pattern(i), error), error);
    encoded(encoder.finish(error), error);
    verify(spaced, 25, av1, 1); verifyAv1(spaced, 25, {0, 10, 20});
    // Identical input produces identical compressed samples on every run.
    std::vector<std::vector<Av1Sample>> runs;
    for (int run = 0; run < 2; ++run) {
        const auto path = directory / (L"av1-repeat-" + std::to_wstring(run) + L".mp4");
        encoded(encoder.open(path.wstring(), width, height, fps, error, balanced, av1), error);
        for (int i = 0; i < 31; ++i) encoded(encoder.write(texture(i), error), error);
        encoded(encoder.finish(error), error); verifyAv1(path, 31, {0});
        runs.push_back(compressedSamples(path));
    }
    for (size_t i = 0; i < runs[0].size(); ++i)
        require(runs[0][i].bytes == runs[1][i].bytes && runs[0][i].cleanPoint == runs[1][i].cleanPoint, "AV1 output is not repeatable");
    // Buffered input must surface a mux error at Finish, retain the failure,
    // and prevent publication. This also catches ignoring EOS drain failures.
    const auto stopped = directory / L"av1-stopped.mp4";
    encoded(encoder.open(stopped.wstring(), width, height, fps, error, balanced, av1), error);
    encoded(encoder.write(pattern(0), error), error);
    encoded(encoder.write(pattern(1), error), error);
    failSample = true; const bool finished = encoder.finishForPublication(error); failSample = false;
    require(!finished && !error.empty(), "Injected AV1 drain failure was ignored");
    const auto terminal = error;
    require(!encoder.finish(error) && error == terminal, "AV1 drain failure was not retained");
    require(!encoder.write(pattern(2), error), "AV1 recording continued after finalization failed");
    require(encoder.publish((directory / L"failed-published.mp4").wstring()) != ERROR_SUCCESS,
        "Failed AV1 output was published");
    // Direct encoder guards.
    lapse::Av1Encoder direct; std::vector<uint8_t> nv12(size_t(width) * height * 3 / 2, 128);
    require(!direct.encode(nv12.data(), error) && direct.sequenceHeader().empty(),
        "Unopened AV1 encoder accepted a frame");
    require(!direct.open(width + 1, height, fps, balanced, error) && !direct.open(width, height, 0, balanced, error) &&
        !direct.open(width, height, fps, static_cast<lapse::EncodingQuality>(9), error), "AV1 encoder accepted invalid settings");
    encoded(direct.open(width, height, fps, balanced, error), error);
    require(!direct.open(width, height, fps, balanced, error) && !direct.sequenceHeader().empty(), "AV1 encoder reopened");
    encoded(direct.encode(nv12.data(), error), error);
    encoded(direct.encode(nv12.data(), error), error);
    encoded(direct.end(error), error);
    encoded(direct.end(error), error);
    bool eos = false; int packets = 0;
    while (!eos) {
        lapse::Av1Packet packet;
        encoded(direct.receive(packet, eos, error), error);
        if (packet.unit.empty()) continue;
        require(packet.pts == packets, "Direct AV1 packet lost presentation order");
        require(packet.keyFrame == (packets == 0), "Direct AV1 keyframe changed");
        if (packets == 0)
            require(obus(packet.unit.data(), packet.unit.size()).front().type == 1,
                "First AV1 unit lacks a sequence header");
        ++packets;
    }
    require(packets == 2 && !direct.encode(nv12.data(), error), "AV1 EOS lost frames or accepted more input");
    std::cout << "AV1: keyframe spacing, av1C/colr/OBU layout, repeatable output, stop after failure and encoder guards passed.\n";
}
void advancedSettings(const std::filesystem::path& directory) {
    using namespace lapse;
    std::wstring error;
    const auto rejected = directory / L"invalid-options.mp4";
    for (int field = 0; field < 4; ++field) {
        EncodingOptions options;
        if (field == 0) options.av1Preset = 12;
        if (field == 1) { options.rateControl = EncodingRateControl::ConstantQuality; options.av1Crf = 71; }
        if (field == 2) { options.rateControl = EncodingRateControl::TargetBitrate; options.bitrateKbps = 0; }
        if (field == 3) options.rateControl = static_cast<EncodingRateControl>(99);
        Encoder encoder;
        require(!encoder.open(rejected.wstring(), width, height, fps, error, EncodingQuality::Balanced,
                EncodingMode::SoftwareAV1, false, options) && !std::filesystem::exists(rejected),
            "Invalid advanced settings created an output file");
    }
    EncodingOptions crf;
    crf.rateControl = EncodingRateControl::ConstantQuality;
    Encoder invalid;
    require(!invalid.open(rejected.wstring(), width, height, fps, error, EncodingQuality::Balanced,
            EncodingMode::Compatible, false, crf) && !std::filesystem::exists(rejected),
        "H.264 silently accepted AV1 CRF settings");
    std::vector<std::vector<Av1Sample>> crfRuns;
    for (int value : {18, 58}) {
        crf.av1Crf = value;
        Encoder encoder;
        const auto path = directory / (L"custom-crf-" + std::to_wstring(value) + L".mp4");
        encoded(encoder.open(path.wstring(), width, height, fps, error, EncodingQuality::Balanced,
            EncodingMode::SoftwareAV1, false, crf), error);
        for (int i = 0; i < 31; ++i) encoded(encoder.write(texture(i), error), error);
        encoded(encoder.finish(error), error);
        verify(path, 31, EncodingMode::SoftwareAV1, fps, false); verifyAv1(path, 31, {0});
        crfRuns.push_back(compressedSamples(path));
    }
    uint64_t sizes[2]{};
    for (int run = 0; run < 2; ++run) for (const auto& sample : crfRuns[run]) sizes[run] += sample.bytes.size();
    require(sizes[0] > sizes[1], "Custom AV1 CRF did not change compression");
    EncodingOptions fast = crf;
    fast.av1Crf = 18;
    fast.av1Preset = 11;
    Encoder preset;
    const auto fastPath = directory / L"av1-preset-11.mp4";
    encoded(preset.open(fastPath.wstring(), width, height, fps, error, EncodingQuality::ExtraSmall,
        EncodingMode::SoftwareAV1, false, fast), error);
    for (int i = 0; i < 31; ++i) encoded(preset.write(texture(i), error), error);
    encoded(preset.finish(error), error); verify(fastPath, 31, EncodingMode::SoftwareAV1, fps, false);
    const auto fastSamples = compressedSamples(fastPath);
    bool different = false;
    for (size_t i = 0; i < fastSamples.size(); ++i) different = different || fastSamples[i].bytes != crfRuns[0][i].bytes;
    require(different, "Selected SVT-AV1 preset did not change the compressed output");
    EncodingOptions bitrate;
    bitrate.rateControl = EncodingRateControl::TargetBitrate; bitrate.bitrateKbps = 300;
    for (auto mode : {EncodingMode::Compatible, EncodingMode::Efficient, EncodingMode::QualityH264,
                      EncodingMode::SoftwareAV1}) {
        Encoder encoder;
        const auto path = directory / (L"custom-bitrate-" + std::to_wstring(int(mode)) + L".mp4");
        encoded(encoder.open(path.wstring(), width, height, fps, error, EncodingQuality::Balanced, mode, false, bitrate), error);
        for (int i = 0; i < 31; ++i) encoded(encoder.write(pattern(i), error), error);
        encoded(encoder.finish(error), error); verify(path, 31, mode);
    }
    std::cout << "Advanced: invalid settings rejected before storage, custom CRF changes compression, preset 11 and target bitrate outputs preserve frames.\n";
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
            lapse::EncodingMode::HardwareH264, lapse::EncodingMode::HardwareHEVC, lapse::EncodingMode::QualityH264,
            lapse::EncodingMode::SoftwareAV1}) exercise(directory, mode);
        av1Specifics(directory);
        advancedSettings(directory);
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) { std::cerr << error.what() << "\nArtifacts kept at " << directory.string() << '\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

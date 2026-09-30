#include "encoder.h"
#include "encoder_conversion.h"
#include "config.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <limits>
#include <algorithm>
#include <cstring>
#include <codecapi.h>
#include <mftransform.h>
#include <mferror.h>

namespace lapse {
namespace {
using Microsoft::WRL::ComPtr;
constexpr LONGLONG ticksPerSecond = 10000000;

bool fail(std::wstring& error, const wchar_t* operation, HRESULT hr) {
    error = std::wstring(operation) + L": " + errorText(hr);
    return false;
}

void appendCleanupError(std::wstring& error, const std::wstring& path, HRESULT hr) {
    if (FAILED(hr)) error += L" Could not remove the incomplete output file (" + path + L"): " + errorText(hr);
}

HRESULT setVideoType(IMFMediaType* type, REFGUID subtype, int width, int height, int fps) {
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return hr;
}

// Enabling hardware in the sink writer is only a preference: it can silently
// fall back to software. Explicit modes must describe the encoder actually
// negotiated. Inspect the selected MFT before accepting any video frames.
HRESULT verifyEncoder(IMFSinkWriter* writer, DWORD stream, REFGUID subtype, bool hardware) {
    ComPtr<IMFSinkWriterEx> extended;
    HRESULT hr = writer->QueryInterface(IID_PPV_ARGS(&extended));
    if (FAILED(hr)) return hr;
    for (DWORD index = 0; ; ++index) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        hr = extended->GetTransformForStream(stream, index, &category, &transform);
        if (FAILED(hr)) return MF_E_TOPO_CODEC_NOT_FOUND;
        if (category != MFT_CATEGORY_VIDEO_ENCODER) continue;
        ComPtr<IMFMediaType> output;
        hr = transform->GetOutputCurrentType(0, &output);
        GUID actualSubtype{};
        if (SUCCEEDED(hr)) hr = output->GetGUID(MF_MT_SUBTYPE, &actualSubtype);
        if (FAILED(hr)) return hr;
        if (actualSubtype != subtype) return MF_E_INVALIDMEDIATYPE;
        ComPtr<IMFAttributes> attributes;
        UINT32 length = 0;
        const bool actualHardware = SUCCEEDED(transform->GetAttributes(&attributes)) &&
            SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length)) && length != 0;
        return actualHardware == hardware ? S_OK : MF_E_TOPO_CODEC_NOT_FOUND;
    }
}

using encoding_detail::toNv12;
}

struct Encoder::Impl {
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFByteStream> bytes;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    // Retain the terminal outcome before allocating any diagnostic text.
    HRESULT finishResult = S_OK, finishCleanup = S_OK;
    bool finishEmpty = false;
    bool publicationAttempted = false;
    DWORD publicationResult = ERROR_INVALID_STATE;
    DWORD stream = 0;
    DWORD bufferSize = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    LONGLONG frames = 0;
    bool writing = false;

    ~Impl() {
        release();
        releaseFile(frames == 0);
    }

    void release() {
        writer.Reset();
        if (bytes) bytes->Close();
        bytes.Reset();
        writing = false;
    }

    HRESULT releaseFile(bool discard) {
        if (file == INVALID_HANDLE_VALUE) return S_OK;
        HRESULT result = S_OK;
        if (discard) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)))
                result = HRESULT_FROM_WIN32(GetLastError());
        }
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        return result;
    }

    void finalize(bool retainFile = false) {
        if (writer) {
            finishEmpty = frames == 0;
            finishResult = writing ? writer->Finalize() : S_OK;
            // Release MF before owned-file cleanup or handle publication.
            // Preserve terminal results before allocating diagnostics.
            release();
        }
        if (file != INVALID_HANDLE_VALUE && (!retainFile || finishEmpty))
            finishCleanup = releaseFile(finishEmpty);
    }

    bool finish(std::wstring& error, bool retainFile) {
        error.clear();
        finalize(retainFile);
        if (finishEmpty) {
            error = L"No video frames were recorded.";
            appendCleanupError(error, path, finishCleanup);
            return false;
        }
        if (FAILED(finishResult))
            return fail(error, L"Cannot finalize the MP4 file", finishResult);
        return true;
    }

    // Quotient/remainder avoids cumulative rounding error at rates like 30 fps.
    LONGLONG timestamp(LONGLONG index) const {
        return (index / fps) * ticksPerSecond + (index % fps) * ticksPerSecond / fps;
    }
};

Encoder::Encoder() : impl_(std::make_unique<Impl>()) {}
Encoder::~Encoder() { impl_->finalize(); }

bool Encoder::open(const std::wstring& path, int width, int height, int fps, std::wstring& error,
                   EncodingQuality quality, EncodingMode mode) {
    error.clear();
    if (impl_->writer || impl_->file != INVALID_HANDLE_VALUE) {
        error = L"Finish the current recording before opening another output file.";
        return false;
    }
    if (path.empty() || path.find(L'\0') != std::wstring::npos) {
        error = L"Choose a valid output filename.";
        return false;
    }
    if (!validateVideoSize(width, height, error)) return false;
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    if (pixels > std::numeric_limits<DWORD>::max() / 4) {
        error = L"The output dimensions are too large.";
        return false;
    }
    if (fps < 1 || fps > 120) {
        error = L"Playback frame rate must be between 1 and 120.";
        return false;
    }
    if (quality != EncodingQuality::Compact && quality != EncodingQuality::Balanced && quality != EncodingQuality::Detail) {
        error = L"Choose a valid video quality.";
        return false;
    }

    if (mode != EncodingMode::Compatible && mode != EncodingMode::Efficient &&
        mode != EncodingMode::HardwareH264 && mode != EncodingMode::HardwareHEVC && mode != EncodingMode::QualityH264) {
        error = L"Choose a valid encoding mode.";
        return false;
    }
    const bool hardware = mode == EncodingMode::HardwareH264 || mode == EncodingMode::HardwareHEVC;
    const bool hevc = mode == EncodingMode::HardwareHEVC;
    const bool compatible = mode == EncodingMode::Compatible;
    const bool efficient = mode == EncodingMode::Efficient;
    const bool qualitySoftware = mode == EncodingMode::QualityH264;
    const GUID subtype = hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    auto candidate = std::make_unique<Impl>();
    candidate->path = fileIOPath(path);
    candidate->width = width;
    candidate->height = height;
    candidate->fps = fps;
    candidate->bufferSize = static_cast<DWORD>(pixels * 3 / 2);
    // Deny rename/deletion while MF binds and writes. DELETE-only access lets
    // MF retain its existing restriction on external writers. Cleanup acts on
    // this owned object rather than a pathname another process could reuse.
    candidate->file = CreateFileW(candidate->path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (candidate->file == INVALID_HANDLE_VALUE) {
        return fail(error, L"Cannot create the output file (choose an unused filename)", HRESULT_FROM_WIN32(GetLastError()));
    }
    auto abandon = [&](const wchar_t* stage, HRESULT result) {
        candidate->release();
        const HRESULT cleanup = candidate->releaseFile(true);
        fail(error, stage, result);
        appendCleanupError(error, candidate->path, cleanup);
        return false;
    };
    auto abandonConfiguration = [&](const wchar_t* stage, HRESULT result) {
        abandon(stage, result);
        error += L" Requested video size: " + std::to_wstring(width) + L" x " + std::to_wstring(height) + L".";
        error += hardware ? L" Try a smaller video size, or choose Compatible or Efficient." :
                            L" Try a smaller video size or another encoding mode.";
        return false;
    };
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                             MF_FILEFLAGS_NONE, candidate->path.c_str(), &candidate->bytes);
    if (FAILED(hr)) return abandon(L"Cannot access the new output file", hr);

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware);
    // Keep default back-pressure; the encoder cannot accumulate an unbounded
    // queue when the disk is slow or frames are submitted faster than encoding.
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(nullptr, candidate->bytes.Get(),
                                                     attributes.Get(), &candidate->writer);
    if (FAILED(hr)) return abandon(L"Cannot create the MP4 writer", hr);

    ComPtr<IMFMediaType> output;
    hr = MFCreateMediaType(&output);
    if (SUCCEEDED(hr)) hr = setVideoType(output.Get(), subtype, width, height, fps);
    const int divisor = quality == EncodingQuality::Compact ? 10 : quality == EncodingQuality::Detail ? 4 : 7;
    const int64_t nominal = int64_t(width) * height * fps;
    // Scale before the original bounds: very small and very large resolutions
    // retain the same 0.5–28 Mbps guardrails. Compatible arithmetic is unchanged.
    const UINT32 bitrate = UINT32(std::clamp<int64_t>(efficient ? nominal * 3 / (divisor * 4) :
        nominal / divisor, 500000, 28000000));
    if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    // Main profile enables more efficient entropy coding. Software B frames
    // are disabled below; hardware uses low-latency operation. Mode tests check
    // that short clips retain their frame count and start at timestamp zero.
    if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_MPEG2_PROFILE, hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_Main);
    if (SUCCEEDED(hr)) hr = candidate->writer->AddStream(output.Get(), &candidate->stream);
    if (FAILED(hr)) return abandonConfiguration(hevc ? L"Cannot configure H.265/HEVC output" : L"Cannot configure H.264 output", hr);

    ComPtr<IMFMediaType> input;
    hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) hr = setVideoType(input.Get(), MFVideoFormat_NV12, width, height, fps);
    if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
    if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_SAMPLE_SIZE, candidate->bufferSize);
    // Configure before media-type negotiation allocates encoder buffers.
    // The software path uses two slice workers. Hardware MFTs need not support
    // worker count or B-picture count, so do not send those optional settings.
    ComPtr<IMFAttributes> encoding;
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&encoding, 9);
    if (SUCCEEDED(hr) && !hardware) hr = encoding->SetUINT32(CODECAPI_AVEncNumWorkerThreads, 2);
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
    if (SUCCEEDED(hr) && !hevc) hr = encoding->SetUINT32(CODECAPI_AVEncH264CABACEnable, TRUE);
    if (SUCCEEDED(hr) && !hardware) hr = encoding->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    // Efficient keeps a bitrate target at every quality level, including
    // wholesale scene changes. This is a target, not a file-size cap. Quality H.264
    // and hardware modes retain detail using content-dependent file sizes.
    // Five playback seconds between keyframes improves static compression.
    const bool constantQuality = hardware || qualitySoftware || (compatible && quality == EncodingQuality::Detail);
    const auto rateControl = constantQuality ? eAVEncCommonRateControlMode_Quality :
        eAVEncCommonRateControlMode_UnconstrainedVBR;
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVEncCommonRateControlMode, rateControl);
    const UINT64 qp = compatible ? 18 :
        (quality == EncodingQuality::Compact ? 28 : quality == EncodingQuality::Detail ? 20 : 24) + (hevc ? 2 : 0);
    if (SUCCEEDED(hr) && constantQuality) hr = encoding->SetUINT64(CODECAPI_AVEncVideoEncodeQP, qp);
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed,
        efficient || qualitySoftware ? 66 : 100);
    if (SUCCEEDED(hr) && !compatible) hr = encoding->SetUINT32(CODECAPI_AVEncMPVGOPSize, 5 * fps);
    if (SUCCEEDED(hr)) hr = candidate->writer->SetInputMediaType(candidate->stream, input.Get(), encoding.Get());
    if (FAILED(hr)) return abandonConfiguration(hardware ?
        L"Cannot initialize the requested hardware encoder" : L"Cannot initialize the H.264 encoder", hr);
    hr = verifyEncoder(candidate->writer.Get(), candidate->stream, subtype, hardware);
    if (FAILED(hr)) return abandonConfiguration(hardware ?
        L"The requested hardware encoder is unavailable" : L"Cannot verify the software H.264 encoder", hr);
    hr = candidate->writer->BeginWriting();
    if (FAILED(hr)) return abandon(L"Cannot start the MP4 recording", hr);
    candidate->writing = true;
    impl_ = std::move(candidate);
    return true;
}

bool Encoder::write(const Frame& frame, std::wstring& error) {
    error.clear();
    if (!impl_->writing) {
        error = L"The video encoder is not open.";
        return false;
    }
    if (frame.width != impl_->width || frame.height != impl_->height || !frame.valid()) {
        error = L"The captured frame does not match the recording dimensions.";
        return false;
    }
    // Leave room for the following frame's timestamp as well as this one.
    const LONGLONG maxSeconds = std::numeric_limits<LONGLONG>::max() / ticksPerSecond;
    if (impl_->frames / impl_->fps >= maxSeconds - 1) {
        error = L"The recording has reached the maximum supported duration.";
        return false;
    }
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(impl_->bufferSize, &buffer);
    if (FAILED(hr)) return fail(error, L"Cannot allocate a video frame", hr);
    BYTE* destination = nullptr;
    hr = buffer->Lock(&destination, nullptr, nullptr);
    if (FAILED(hr)) return fail(error, L"Cannot access the video frame buffer", hr);
    toNv12(frame, destination);
    hr = buffer->Unlock();
    if (SUCCEEDED(hr)) hr = buffer->SetCurrentLength(impl_->bufferSize);
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    const LONGLONG timestamp = impl_->timestamp(impl_->frames);
    const LONGLONG duration = impl_->timestamp(impl_->frames + 1) - timestamp;
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(timestamp);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr)) hr = impl_->writer->WriteSample(impl_->stream, sample.Get());
    if (FAILED(hr)) return fail(error, L"Cannot encode the video frame", hr);
    ++impl_->frames;
    return true;
}

bool Encoder::finish(std::wstring& error) { return impl_->finish(error, false); }

bool Encoder::finishForPublication(std::wstring& error) { return impl_->finish(error, true); }

DWORD Encoder::publish(const std::wstring& destination) {
    if (impl_->publicationAttempted) return impl_->publicationResult;
    if (impl_->writer || impl_->file == INVALID_HANDLE_VALUE || impl_->frames == 0 || FAILED(impl_->finishResult))
        return ERROR_INVALID_STATE;
    if (destination.empty() || destination.find(L'\0') != std::wstring::npos)
        return ERROR_INVALID_NAME;
    const auto path = fileIOPath(destination);
    if (path.size() > (std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO)) / sizeof(wchar_t))
        return ERROR_FILENAME_EXCED_RANGE;
    const size_t nameBytes = path.size() * sizeof(wchar_t);
    const DWORD bufferSize = static_cast<DWORD>(sizeof(FILE_RENAME_INFO) + nameBytes);
    std::vector<BYTE> buffer(bufferSize, 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(rename->FileName, path.c_str(), nameBytes);
    // All allocation precedes this identity-bound operation. Keep the guard
    // through Engine's success/failure message and status commit. A retry
    // reports this result without ever renaming a reused pathname.
    const BOOL renamed = SetFileInformationByHandle(impl_->file, FileRenameInfo, rename, bufferSize);
    impl_->publicationResult = renamed ? ERROR_SUCCESS : GetLastError();
    impl_->publicationAttempted = true;
    return impl_->publicationResult;
}

void Encoder::releasePublication() noexcept {
    if (!impl_->writer) impl_->releaseFile(false);
}

uint64_t Encoder::frames() const { return uint64_t(impl_->frames); }
}

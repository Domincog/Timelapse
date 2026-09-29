#include "encoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <limits>
#include <algorithm>
#include <cstring>
#include <codecapi.h>

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

// NV12 is always top-down. Use an explicit BT.709 limited-range conversion,
// averaging all four source pixels for each 2x2 chroma sample. The fixed-point
// coefficients retain the full black/white range and avoid RGB32 orientation
// differences among Media Foundation color converters.
void toNv12(const Frame& frame, BYTE* target) {
    const size_t width = static_cast<size_t>(frame.width);
    const size_t height = static_cast<size_t>(frame.height);
    BYTE* uv = target + width * height;
    for (size_t y = 0; y < height; y += 2) {
        for (size_t x = 0; x < width; x += 2) {
            int red = 0, green = 0, blue = 0;
            for (size_t dy = 0; dy < 2; ++dy) {
                for (size_t dx = 0; dx < 2; ++dx) {
                    const size_t index = (y + dy) * width + x + dx;
                    const BYTE* pixel = frame.pixels.data() + index * 4;
                    const int b = pixel[0], g = pixel[1], r = pixel[2];
                    target[index] = static_cast<BYTE>(16 +
                        (11966 * r + 40254 * g + 4064 * b + 32768) / 65536);
                    red += r;
                    green += g;
                    blue += b;
                }
            }
            const size_t chroma = (y / 2) * width + x;
            uv[chroma] = static_cast<BYTE>((128 * 262144 - 6596 * red -
                22189 * green + 28784 * blue + 131072) / 262144);
            uv[chroma + 1] = static_cast<BYTE>((128 * 262144 + 28784 * red -
                26145 * green - 2639 * blue + 131072) / 262144);
        }
    }
}
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
                   EncodingQuality quality) {
    error.clear();
    if (impl_->writer || impl_->file != INVALID_HANDLE_VALUE) {
        error = L"Finish the current recording before opening another output file.";
        return false;
    }
    if (path.empty() || path.find(L'\0') != std::wstring::npos) {
        error = L"Choose a valid output filename.";
        return false;
    }
    if (width < 16 || height < 16 || width > 4096 || height > 4096 || (width & 1) || (height & 1)) {
        error = L"Video dimensions must be even numbers between 16 and 4096 pixels.";
        return false;
    }
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
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                             MF_FILEFLAGS_NONE, candidate->path.c_str(), &candidate->bytes);
    if (FAILED(hr)) return abandon(L"Cannot access the new output file", hr);

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    // Keep default back-pressure; the encoder cannot accumulate an unbounded
    // queue when the disk is slow or frames are submitted faster than encoding.
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(nullptr, candidate->bytes.Get(),
                                                     attributes.Get(), &candidate->writer);
    if (FAILED(hr)) return abandon(L"Cannot create the MP4 writer", hr);

    ComPtr<IMFMediaType> output;
    hr = MFCreateMediaType(&output);
    if (SUCCEEDED(hr)) hr = setVideoType(output.Get(), MFVideoFormat_H264, width, height, fps);
    const int divisor = quality == EncodingQuality::Compact ? 10 : quality == EncodingQuality::Detail ? 4 : 7;
    const UINT32 bitrate = UINT32(std::clamp<int64_t>(int64_t(width) * height * fps / divisor, 500000, 28000000));
    if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    // Main profile enables more efficient entropy coding. Explicitly disable
    // B frames below so even a one-frame timelapse starts at timestamp zero.
    if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    if (SUCCEEDED(hr)) hr = candidate->writer->AddStream(output.Get(), &candidate->stream);
    if (FAILED(hr)) return abandon(L"Cannot configure H.264 output", hr);

    ComPtr<IMFMediaType> input;
    hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) hr = setVideoType(input.Get(), MFVideoFormat_NV12, width, height, fps);
    if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
    if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_SAMPLE_SIZE, candidate->bufferSize);
    // Configure before media-type negotiation allocates encoder buffers.
    // Timelapse needs only a few workers, with no multi-frame batching.
    ComPtr<IMFAttributes> encoding;
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&encoding, 7);
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVEncNumWorkerThreads, 2);
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVEncH264CABACEnable, TRUE);
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    // Detail prioritizes retained image information instead of a target size.
    // The other presets retain their target-bitrate policy.
    const auto rateControl = quality == EncodingQuality::Detail
        ? eAVEncCommonRateControlMode_Quality : eAVEncCommonRateControlMode_UnconstrainedVBR;
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVEncCommonRateControlMode, rateControl);
    if (SUCCEEDED(hr) && quality == EncodingQuality::Detail)
        hr = encoding->SetUINT64(CODECAPI_AVEncVideoEncodeQP, 18);
    if (SUCCEEDED(hr)) hr = encoding->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 100);
    if (SUCCEEDED(hr)) hr = candidate->writer->SetInputMediaType(candidate->stream, input.Get(), encoding.Get());
    if (FAILED(hr)) return abandon(L"Cannot initialize the H.264 encoder", hr);
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

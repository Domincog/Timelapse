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
#include <new>
#include <utility>
#include <fstream>

namespace lapse { std::wstring testEncoderErrorText(HRESULT); }
HRESULT WINAPI testCreateWriter(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**);
BOOL WINAPI testFileDisposition(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
// Compile the encoder here to fault diagnostic formatting without changing its
// production API. The linked archive supplies the other core definitions.
#define errorText testEncoderErrorText
#define MFCreateSinkWriterFromURL testCreateWriter
#define SetFileInformationByHandle testFileDisposition
#include "../src/encoder.cpp"
#undef SetFileInformationByHandle
#undef MFCreateSinkWriterFromURL
#undef errorText

namespace {
thread_local bool failNextEncoderAllocation = false;
thread_local bool failAllocationAfterRename = false;
thread_local int actualHandleRenames = 0;
thread_local bool failFinishDiagnostic = false;
thread_local int encoderAllocationFailures = 0;
thread_local bool failWriterSetup = false, throwWriterSetup = false, failFileDisposition = false;
thread_local int writerSetupFailures = 0, dispositionFailures = 0, dispositionCalls = 0;
}
void* operator new(std::size_t size) {
    if (std::exchange(failNextEncoderAllocation, false)) {
        ++encoderAllocationFailures;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

HRESULT WINAPI testCreateWriter(LPCWSTR url, IMFByteStream* bytes, IMFAttributes* attributes, IMFSinkWriter** writer) {
    if (std::exchange(failWriterSetup, false)) {
        ++writerSetupFailures; *writer = nullptr; return E_ACCESSDENIED;
    }
    if (std::exchange(throwWriterSetup, false)) {
        ++writerSetupFailures;
        failNextEncoderAllocation = true;
        // A real one-shot C++ allocation failure after the file and MF stream
        // exist exercises candidate unwinding, without changing the public API.
        const std::wstring resource(1024, L'x');
        (void)resource;
    }
    return MFCreateSinkWriterFromURL(url, bytes, attributes, writer);
}
BOOL WINAPI testFileDisposition(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD size) {
    ++dispositionCalls;
    if (std::exchange(failFileDisposition, false)) {
        ++dispositionFailures; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    const BOOL result = SetFileInformationByHandle(file, kind, info, size);
    if (kind == FileRenameInfo) {
        ++actualHandleRenames;
        if (result && std::exchange(failAllocationAfterRename, false)) failNextEncoderAllocation = true;
    }
    return result;
}

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

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct AllocationFault {
    explicit AllocationFault(bool diagnostic) {
        if (diagnostic) failFinishDiagnostic = true;
        else failNextEncoderAllocation = true;
    }
    ~AllocationFault() { failFinishDiagnostic = failNextEncoderAllocation = false; }
};

void finalizationAllocationRecovery(const std::filesystem::path& directory) {
    lapse::Encoder encoder;
    std::wstring error;
    const auto path = directory / L"failed-finalize.recording.mp4";
    encoded(encoder.open(path.wstring(), width, height, fps, error), error);
    for (int index = 0; index < 3; ++index) encoded(encoder.write(pattern(index), error), error);
    {
        FileHandle reader{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(reader.value != INVALID_HANDLE_VALUE, "Cannot open finalization fault fixture");
        OVERLAPPED range{};
        require(LockFileEx(reader.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                           0, MAXDWORD, 0x7fffffff, &range) != FALSE, "Cannot lock owned recording");
        const int failuresBefore = encoderAllocationFailures;
        bool caught = false;
        {
            AllocationFault fault(true);
            try { (void)encoder.finish(error); } catch (const std::bad_alloc&) { caught = true; }
        }
        require(UnlockFileEx(reader.value, 0, MAXDWORD, 0x7fffffff, &range) != FALSE, "Cannot unlock owned recording");
        require(caught && encoderAllocationFailures == failuresBefore + 1,
                "Finalization formatting did not receive exactly one allocation failure");
    }
    require(!encoder.finish(error) && error.find(L"0x80070021") != std::wstring::npos && encoder.frames() == 3,
            "Finalization failure became success after diagnostic allocation failed");
    const auto failure = error;
    require(!encoder.finish(error) && error == failure && std::filesystem::is_regular_file(path),
            "Repeated finalization changed its error or lost partial output");
    for (auto quality : {lapse::EncodingQuality::Compact, lapse::EncodingQuality::Balanced, lapse::EncodingQuality::Detail}) {
        const auto recovered = directory / (L"quality-recovery-" + std::to_wstring(static_cast<int>(quality)) + L".mp4");
        encoded(encoder.open(recovered.wstring(), width, height, fps, error, quality), error);
        require(encoder.frames() == 0, "Reopening after failed finalization retained old frames");
        encoded(encoder.write(pattern(0), error), error);
        encoded(encoder.finish(error), error);
        require(encoder.finish(error) && error.empty() && encoder.frames() == 1, "Recovery retained terminal failure");
        verifyVideo(recovered, 1);
    }
    require(std::filesystem::is_regular_file(path), "Recovery removed the previous partial output");
}

void emptyFinalizationAllocationRecovery(const std::filesystem::path& directory) {
    auto encoder = std::make_unique<lapse::Encoder>();
    std::wstring error;
    const auto path = directory / L"empty-allocation.mp4";
    encoded(encoder->open(path.wstring(), width, height, fps, error), error);
    bool caught = false;
    const int failuresBefore = encoderAllocationFailures;
    {
        AllocationFault fault(false);
        try { (void)encoder->finish(error); } catch (const std::bad_alloc&) { caught = true; }
    }
    require(caught && encoderAllocationFailures == failuresBefore + 1, "Empty finish diagnostic was not faulted");
    require(!encoder->finish(error) && error == L"No video frames were recorded." && !std::filesystem::exists(path),
            "Empty finalization became success or lost its cleanup");
    const auto failure = error;
    require(!encoder->finish(error) && error == failure, "Repeated empty finish changed its outcome");
    bool allocationUnused = false;
    {
        AllocationFault fault(false);
        encoder.reset();
        allocationUnused = failNextEncoderAllocation;
    }
    require(allocationUnused, "Finalized destruction unexpectedly allocated a diagnostic");
    const auto active = directory / L"active-empty-destructor.mp4";
    encoder = std::make_unique<lapse::Encoder>();
    encoded(encoder->open(active.wstring(), width, height, fps, error), error);
    {
        AllocationFault fault(false);
        encoder.reset();
        allocationUnused = failNextEncoderAllocation;
    }
    require(allocationUnused, "Active destruction unexpectedly allocated a diagnostic");
    require(!std::filesystem::exists(active), "Active empty destruction failed cleanup");
}

void createSentinel(const std::filesystem::path& path) {
    FileHandle file;
    file.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file.value != INVALID_HANDLE_VALUE, "Cannot create owned replacement sentinel");
    constexpr char text[] = "A replacement file must survive.";
    DWORD written = 0;
    require(WriteFile(file.value, text, sizeof(text) - 1, &written, nullptr) && written == sizeof(text) - 1,
            "Cannot write owned replacement sentinel");
}
void verifySentinel(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(value == "A replacement file must survive.", "A later encoder operation changed a replacement file");
}
BY_HANDLE_FILE_INFORMATION fileIdentity(const std::filesystem::path& path) {
    FileHandle file;
    file.value = CreateFileW(lapse::fileIOPath(path.wstring()).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file.value != INVALID_HANDLE_VALUE, "Cannot inspect owned file identity");
    BY_HANDLE_FILE_INFORMATION result{};
    require(GetFileInformationByHandle(file.value, &result) != FALSE, "Cannot query owned file identity");
    return result;
}
void ownedFileProtection(const std::filesystem::path& directory) {
    const auto path = directory / L"protected-active.mp4", saved = directory / L"protected-finished.mp4";
    auto encoder = std::make_unique<lapse::Encoder>();
    std::wstring error;
    encoded(encoder->open(path.wstring(), width, height, fps, error), error);
    const auto original = fileIdentity(path);
    require(!MoveFileExW(path.c_str(), saved.c_str(), MOVEFILE_WRITE_THROUGH) && GetLastError() == ERROR_SHARING_VIOLATION,
            "Active output can be moved and replaced");
    require(!DeleteFileW(path.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION, "Active output can be deleted");
    FileHandle outsider;
    outsider.value = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(outsider.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
            "Output ownership relaxed MF's external writer restriction");
    encoded(encoder->write(pattern(0), error), error);
    encoded(encoder->finish(error), error);
    require(MoveFileExW(path.c_str(), saved.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "Finish retained the ownership handle");
    const auto finished = fileIdentity(saved);
    require(original.dwVolumeSerialNumber == finished.dwVolumeSerialNumber && original.nFileIndexHigh == finished.nFileIndexHigh &&
            original.nFileIndexLow == finished.nFileIndexLow, "Finish replaced the owned file identity");
    createSentinel(path);
    require(encoder->finish(error), "Repeated successful finish changed its result");
    encoder.reset();
    verifySentinel(path);
    verifyVideo(saved, 1);
    std::cout << "Owned output: active rename/delete/write blocked; finish releases original identity; replacement preserved.\n";
}
void setupAndCleanupOwnership(const std::filesystem::path& directory) {
    lapse::Encoder encoder;
    std::wstring error;
    for (bool allocation : {false, true}) {
        const auto path = directory / (allocation ? L"setup-unwind.mp4" : L"setup-failure.mp4");
        const int before = writerSetupFailures, allocationsBefore = encoderAllocationFailures;
        failWriterSetup = !allocation; throwWriterSetup = allocation;
        bool opened = false, caught = false;
        try { opened = encoder.open(path.wstring(), width, height, fps, error); }
        catch (const std::bad_alloc&) { caught = true; }
        require(writerSetupFailures == before + 1 && !opened && caught == allocation &&
                (!allocation || encoderAllocationFailures == allocationsBefore + 1), "Writer setup fault missed its intended boundary");
        require(!std::filesystem::exists(path), "Failed writer setup/unwind retained its owned output");
        if (!allocation) require(error.find(L"Cannot create the MP4 writer") != std::wstring::npos &&
                                 error.find(L"0x80070005") != std::wstring::npos, "Setup lost its stage or HRESULT");
        encoded(encoder.open(path.wstring(), width, height, fps, error), error);
        encoded(encoder.write(pattern(0), error), error);
        encoded(encoder.finish(error), error);
        verifyVideo(path, 1);
    }
    const auto setupPath = directory / L"setup-retained.mp4", setupRetained = directory / L"setup-retained-moved.mp4";
    const int setupBefore = writerSetupFailures, cleanupBefore = dispositionFailures;
    failWriterSetup = failFileDisposition = true;
    require(!encoder.open(setupPath.wstring(), width, height, fps, error) && writerSetupFailures == setupBefore + 1 &&
            dispositionFailures == cleanupBefore + 1 && error.find(L"Cannot create the MP4 writer") != std::wstring::npos &&
            error.find(L"Could not remove the incomplete output file") != std::wstring::npos &&
            error.find(setupPath.wstring()) != std::wstring::npos && error.find(L"0x80070005") != std::wstring::npos,
            "Failed setup lost its primary or cleanup error");
    require(MoveFileExW(setupPath.c_str(), setupRetained.c_str(), 0) != FALSE, "Failed setup cleanup retained an owned handle");
    createSentinel(setupPath);
    require(encoder.finish(error) && encoder.frames() == 1, "Failed candidate changed the prior finished recording");
    verifySentinel(setupPath);

    const auto path = directory / L"retained-empty.mp4", retained = directory / L"retained-empty-moved.mp4";
    auto empty = std::make_unique<lapse::Encoder>();
    encoded(empty->open(path.wstring(), width, height, fps, error), error);
    error.reserve(512); // Fail the appended cleanup detail, not the initial empty message.
    const int failedBefore = dispositionFailures, allocationsBefore = encoderAllocationFailures;
    failFileDisposition = true;
    bool caught = false;
    {
        AllocationFault fault(false);
        try { (void)empty->finish(error); } catch (const std::bad_alloc&) { caught = true; }
    }
    require(caught && encoderAllocationFailures == allocationsBefore + 1 && dispositionFailures == failedBefore + 1 &&
            error == L"No video frames were recorded.", "Cleanup diagnostic allocation fault missed its intended boundary");
    const int attempts = dispositionCalls;
    require(!empty->finish(error) && error.find(L"No video frames were recorded.") != std::wstring::npos &&
            error.find(path.wstring()) != std::wstring::npos && error.find(L"0x80070005") != std::wstring::npos,
            "Repeated empty finish lost cached cleanup failure or retained pathname");
    require(std::filesystem::is_regular_file(path) && dispositionCalls == attempts, "Finish retried failed cleanup or removed retained output");
    const auto failure = error;
    require(MoveFileExW(path.c_str(), retained.c_str(), 0) != FALSE, "Cleanup failure retained its ownership handle");
    createSentinel(path);
    require(!empty->finish(error) && error == failure && dispositionCalls == attempts, "Repeated finish changed terminal cleanup outcome");
    {
        AllocationFault fault(false);
        empty.reset();
        require(failNextEncoderAllocation, "Destruction allocated a cleanup diagnostic");
    }
    verifySentinel(path);
    require(std::filesystem::is_regular_file(retained), "Destruction discarded the retained empty output");
    std::cout << "Owned cleanup: setup failure/unwind recovery, cached cleanup failure, released handles and replacement preservation passed.\n";
}

void temporarySuffixBoundary(const std::filesystem::path& directory) {
    const std::wstring name = L"Timelapse-20260929-120000-123-12345";
    const size_t folderLength = MAX_PATH - 2 - 1 - name.size() - 4;
    const auto prefix = (directory / L"save \u65e5 ").wstring();
    require(prefix.size() < folderLength, "Encoder fixture root is too long for the path boundary");
    const std::filesystem::path folder(prefix + std::wstring(folderLength - prefix.size(), L'x'));
    require(std::filesystem::create_directory(folder), "Could not create boundary save folder");
    const auto temporary = folder / (name + L".recording.mp4");
    const auto finalPath = folder / (name + L".mp4");
    require(folder.wstring().size() < MAX_PATH && finalPath.wstring().size() == MAX_PATH - 2 &&
            temporary.wstring().size() >= MAX_PATH, "Temporary suffix did not cross MAX_PATH");
    const auto temporaryIO = lapse::fileIOPath(temporary.wstring());
    std::wstring error;
    lapse::Encoder encoder;
    encoded(encoder.open(temporary.wstring(), width, height, fps, error), error);
    encoded(encoder.write(pattern(0), error), error);
    encoded(encoder.finish(error), error);
    const auto completedSize = std::filesystem::file_size(temporaryIO);
    require(!encoder.open(temporary.wstring(), width, height, fps, error) &&
            std::filesystem::file_size(temporaryIO) == completedSize,
            "Boundary path reopen changed the existing movie");
    require(MoveFileExW(temporaryIO.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
            "Could not publish boundary movie");
    verifyVideo(finalPath, 1);
    encoded(encoder.open(temporary.wstring(), width, height, fps, error), error);
    require(!encoder.finish(error) && error == L"No video frames were recorded." &&
            GetFileAttributesW(temporaryIO.c_str()) == INVALID_FILE_ATTRIBUTES,
            "Empty boundary recording was not removed");
    {
        lapse::Encoder abandoned;
        encoded(abandoned.open(temporary.wstring(), width, height, fps, error), error);
    }
    require(GetFileAttributesW(temporaryIO.c_str()) == INVALID_FILE_ATTRIBUTES &&
            std::filesystem::file_size(finalPath) == completedSize,
            "Empty boundary destruction leaked a file or changed the saved movie");
    std::cout << "Temporary suffix boundary: folder=" << folder.wstring().size()
              << " final=" << finalPath.wstring().size() << " temporary=" << temporary.wstring().size()
              << " decoded=1 empty_cleanup=1 destructor_cleanup=1 collision_preserved=1\n";
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
    finalizationAllocationRecovery(directory);
    emptyFinalizationAllocationRecovery(directory);
    temporarySuffixBoundary(directory);
    ownedFileProtection(directory);
    setupAndCleanupOwnership(directory);
}
}

namespace lapse {
std::wstring testEncoderErrorText(HRESULT result) {
    auto message = errorText(result);
    // Arm only after the real lock-error lookup, leaving MF worker allocations
    // and the Windows message buffer outside the synthetic formatting fault.
    if (failFinishDiagnostic && result == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)) {
        failFinishDiagnostic = false;
        failNextEncoderAllocation = true;
    }
    return message;
}
}

namespace {
bool sameIdentity(const BY_HANDLE_FILE_INFORMATION& left,const BY_HANDLE_FILE_INFORMATION& right) {
    return left.dwVolumeSerialNumber==right.dwVolumeSerialNumber&&left.nFileIndexHigh==right.nFileIndexHigh&&left.nFileIndexLow==right.nFileIndexLow;
}
void blockedRename(const std::filesystem::path& path,const std::filesystem::path& to) {
    require(!MoveFileExW(path.c_str(),to.c_str(),0)&&GetLastError()==ERROR_SHARING_VIOLATION,"Retained output lost guard");
}
void noAllocationAfterNativeRename(const std::filesystem::path& directory) {
    const auto path=directory/L"post-native.recording.mp4",finalPath=directory/L"post-native.mp4",moved=directory/L"post-native-moved.mp4";
    auto encoder=std::make_unique<lapse::Encoder>();std::wstring error;
    encoded(encoder->open(path.wstring(),width,height,fps,error),error);
    encoded(encoder->write(pattern(0),error),error);encoded(encoder->finishForPublication(error),error);
    const auto original=fileIdentity(path);const int calls=actualHandleRenames;
    failAllocationAfterRename=true;
    const DWORD result=encoder->publish(finalPath.wstring());
    const bool allocationUnused=std::exchange(failNextEncoderAllocation,false);
    require(result==ERROR_SUCCESS&&allocationUnused&&!failAllocationAfterRename&&actualHandleRenames==calls+1,
        "Publication allocated after successful native rename or missed real syscall");
    require(sameIdentity(original,fileIdentity(finalPath)),"Native rename lost original file identity");blockedRename(finalPath,moved);
    createSentinel(path);
    require(encoder->publish((directory/L"never-retarget.mp4").wstring())==ERROR_SUCCESS&&actualHandleRenames==calls+1,
        "Repeated successful publication called native rename again");
    verifySentinel(path);
    bool releaseUnused=false,destructionUnused=false;
    {
        AllocationFault fault(false);encoder->releasePublication();releaseUnused=failNextEncoderAllocation;
        encoder.reset();destructionUnused=failNextEncoderAllocation;
    }
    require(releaseUnused&&destructionUnused,"Release or destruction allocated after publication");verifySentinel(path);
    require(MoveFileExW(finalPath.c_str(),moved.c_str(),0)!=FALSE,"Publication release retained file handle");verifyVideo(moved,1);
    std::cout<<"PASS native rename boundary: allocation unused, native_calls=1, cached repeat, noalloc release/destruction, original decoded.\n";
}
void failedFinalizationRetained(const std::filesystem::path& directory) {
    const auto path=directory/L"retained-failed-finalize.mp4",moved=directory/L"retained-failed-moved.mp4";
    lapse::Encoder encoder;std::wstring error;
    encoded(encoder.open(path.wstring(),width,height,fps,error),error);
    for(int index=0;index<3;++index)encoded(encoder.write(pattern(index),error),error);
    const auto original=fileIdentity(path);const int allocations=encoderAllocationFailures,renames=actualHandleRenames;
    bool caught=false;
    {
        FileHandle reader{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
        require(reader.value!=INVALID_HANDLE_VALUE,"Cannot open owned finalization-lock file");
        OVERLAPPED range{};
        require(LockFileEx(reader.value,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,MAXDWORD,0x7fffffff,&range)!=FALSE,"Cannot lock owned output");
        {
            AllocationFault fault(true);
            try{(void)encoder.finishForPublication(error);}catch(const std::bad_alloc&){caught=true;}
        }
        require(UnlockFileEx(reader.value,0,MAXDWORD,0x7fffffff,&range)!=FALSE,"Cannot unlock owned output");
    }
    require(caught&&encoderAllocationFailures==allocations+1,"Retained finalize diagnostic did not hit actual allocation");
    blockedRename(path,moved);
    require(!encoder.finishForPublication(error)&&error.find(L"0x80070021")!=std::wstring::npos,"Retained finalization failure changed on retry");
    require(encoder.publish(moved.wstring())==ERROR_INVALID_STATE&&actualHandleRenames==renames,"Failed finalization attempted publication");
    require(!encoder.open((directory/L"forbidden-reopen.mp4").wstring(),width,height,fps,error),"Reopen dropped failed-finalization guard");
    encoder.releasePublication();
    require(MoveFileExW(path.c_str(),moved.c_str(),0)!=FALSE&&sameIdentity(original,fileIdentity(moved)),"Failed finalized original was not retained/released");
    require(!encoder.finish(error)&&error.find(L"0x80070021")!=std::wstring::npos,"Plain finish forgot failed outcome");
    const auto recovery=directory/L"retained-failure-recovery.mp4";
    encoded(encoder.open(recovery.wstring(),width,height,fps,error),error);encoded(encoder.write(pattern(0),error),error);
    encoded(encoder.finishForPublication(error),error);encoder.releasePublication();verifyVideo(recovery,1);
    std::cout<<"PASS retained failed finalization: actual lock error + diagnostic allocation, protected partial identity, no rename, released/recovered decode.\n";
}
void retainedDestructionAndEmpty(const std::filesystem::path& directory) {
    const auto path=directory/L"retained-destructor.mp4",moved=directory/L"retained-destructor-moved.mp4";
    std::wstring error;auto encoder=std::make_unique<lapse::Encoder>();
    encoded(encoder->open(path.wstring(),width,height,fps,error),error);encoded(encoder->write(pattern(0),error),error);
    encoded(encoder->finishForPublication(error),error);blockedRename(path,moved);
    bool unused=false;
    {AllocationFault fault(false);encoder.reset();unused=failNextEncoderAllocation;}
    require(unused&&MoveFileExW(path.c_str(),moved.c_str(),0)!=FALSE,"Abandoned publication did not release without allocation");verifyVideo(moved,1);
    const auto empty=directory/L"retained-empty-allocation.mp4";
    encoder=std::make_unique<lapse::Encoder>();encoded(encoder->open(empty.wstring(),width,height,fps,error),error);
    bool caught=false;const int allocations=encoderAllocationFailures;
    {AllocationFault fault(false);try{(void)encoder->finishForPublication(error);}catch(const std::bad_alloc&){caught=true;}}
    require(caught&&encoderAllocationFailures==allocations+1&&!std::filesystem::exists(empty),"Empty retained cleanup/diagnostic failure missed");
    require(!encoder->finishForPublication(error)&&error==L"No video frames were recorded."&&encoder->publish(path.wstring())==ERROR_INVALID_STATE,
        "Empty outcome was not cached or allowed publication");
    std::cout<<"PASS retained destructor and empty diagnostic cleanup/retry controls.\n";
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
        noAllocationAfterNativeRename(directory);
        failedFinalizationRetained(directory);
        retainedDestructionAndEmpty(directory);
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

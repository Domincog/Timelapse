#pragma once
// Independent playback checks for successful synthetic split recordings.
#include "engine.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace split_test {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
inline uint64_t decodedFrames(const std::filesystem::path& path, uint64_t upperBound, bool recovery = false) {
    using Microsoft::WRL::ComPtr;
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    require(SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)), "Cannot open split MP4");
    ComPtr<IMFMediaType> type;
    require(SUCCEEDED(MFCreateMediaType(&type)) &&
        SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
        SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) &&
        SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, type.Get())), "Cannot decode split MP4");
    uint64_t count = 0;
    for (uint64_t attempt = 0; attempt < upperBound + 100; ++attempt) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        require(SUCCEEDED(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample)) &&
            !(flags & MF_SOURCE_READERF_ERROR), "Split MP4 decoder failed");
        if (sample) {
            // The Windows fragmented sink quantizes timestamps more coarsely
            // than ordinary MP4. Match its established 33.4us bound only there.
            require(count < upperBound &&
                std::llabs(timestamp - static_cast<LONGLONG>(count) * 10000000 / 30) <= (recovery ? 334 : 1),
                "Split MP4 did not restart at zero with 30fps timestamps");
            ++count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            require(count > 0, "Empty split MP4 was published");
            return count;
        }
    }
    throw std::runtime_error("Split MP4 has no bounded end of stream");
}
inline void verify(const std::filesystem::path& folder, const lapse::Status& result, bool paired, bool recovery = false) {
    require(result.state == lapse::State::Idle && !result.error && !result.recordingFailed &&
        result.frames > 0 && result.completedSegments > 0 && result.savedPaths.size() == (paired ? 2u : 1u),
        "Split recording lost successful session facts");
    uint64_t totals[2]{};
    std::map<std::wstring, std::pair<uint64_t, uint64_t>> parts;
    size_t fileCount = 0;
    for (const auto& item : std::filesystem::directory_iterator(folder)) {
        if (!item.is_regular_file() || item.path().extension() != L".mp4") continue;
        const auto name = item.path().filename().wstring();
        require(name.find(L".recording.mp4") == std::wstring::npos, "Successful split left an unfinished MP4");
        bool camera = paired && name.find(L"-camera.mp4") != std::wstring::npos;
        const std::wstring suffix = paired ? (camera ? L"-camera.mp4" : L"-desktop.mp4") : L".mp4";
        require(name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix,
            "Unexpected split output suffix");
        const auto frames = decodedFrames(item.path(), result.frames, recovery);
        totals[camera ? 1 : 0] += frames;
        auto& pair = parts[name.substr(0, name.size() - suffix.size())];
        (camera ? pair.second : pair.first) = frames;
        ++fileCount;
    }
    require(parts.size() == result.completedSegments &&
        fileCount == result.completedSegments * (paired ? 2u : 1u) && totals[0] == result.frames &&
        (!paired || totals[1] == result.frames), "Split files lost or duplicated admitted frames");
    for (const auto& part : parts)
        require(part.second.first > 0 && (!paired || part.second.first == part.second.second),
            "Split pair has different frame counts");
    for (const auto& path : result.savedPaths)
        require(std::filesystem::exists(path), "Latest split output path does not exist");
}
}

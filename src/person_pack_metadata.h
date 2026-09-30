#pragma once
#include <cstdint>
namespace lapse {
inline constexpr wchar_t PersonPackFilename[] = L"Timelapse-person-nanodet-r1.exe";
inline constexpr wchar_t PersonPackDownloadUrl[] = L"https://github.com/Domincog/Timelapse/releases/download/v0.9.0/Timelapse-person-nanodet-r1.exe";
inline constexpr wchar_t PersonPackLicenseUrl[] = L"https://github.com/Domincog/Timelapse/blob/main/person-pack/README.md#licenses";
// Exact optional release artifact, built from the pinned person-pack sources.
// Rebuilding the worker requires explicitly updating these admission pins.
inline constexpr uint64_t PersonPackExpectedBytes = 3820032;
inline constexpr char PersonPackExpectedSha256[] = "f477f4e070e59e17161522fd97c870ab8be065b0d10ce4b165a597e5b64e4f4b";
}

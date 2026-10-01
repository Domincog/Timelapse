#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace lapse {
enum class EncodingMode;
inline constexpr int MinCaptureIntervalMs = 100;
inline constexpr int MaxCaptureIntervalMs = 86400000;
inline constexpr int MinOutputFps = 1;
inline constexpr int MaxOutputFps = 120;
inline constexpr int DefaultOutputFps = 30;
inline constexpr int MinVideoDimension = 48;
inline constexpr int MaxVideoDimension = 4096;
inline constexpr int64_t MaxVideoPixels = int64_t(4096) * 2160;
enum class DurationUnit { Seconds, Minutes, Hours, Days };
int64_t durationUnitMs(DurationUnit unit) noexcept;
// Strict unsigned decimal, optional surrounding ASCII whitespace, up to nine
// fractional digits. Require an exact multiple of quantumMs; never round.
// On failure result is unchanged. Minimum may be zero for recording offsets;
// maximum and quantum must be positive.
bool parseDuration(std::wstring_view text, DurationUnit unit, int64_t minMs,
                   int64_t maxMs, int64_t quantumMs, int64_t& result,
                   std::wstring& error);
bool parsePixelDimension(std::wstring_view text, int& result, std::wstring& error);
// Whole frames per second, with optional surrounding ASCII whitespace.
// Reject invalid drafts without changing the committed result.
bool parseOutputFps(std::wstring_view text, int& result, std::wstring& error);
bool validateOutputFps(int fps, std::wstring& error);
bool validateCaptureInterval(int milliseconds, std::wstring& error);
bool validateVideoSize(int width, int height, std::wstring& error);
// A one-time source-size suggestion, never a following/recording policy.
// Keep exact supported sizes; otherwise downscale and round down to even pixels
// within the video bounds. Return {0,0} if no usable fit exists. No allocation.
std::pair<int, int> sourceVideoDimensions(int64_t width, int64_t height) noexcept;
// Recovery mode uses the native fragmented sink, which accepts H.264 only.
bool validateEncodingMode(EncodingMode mode, bool recoveryMode, std::wstring& error);
// Largest exact single unit requiring at most three fractional digits.
std::wstring formatDuration(int64_t milliseconds, bool compact = false);
// Disposable preview fits 640x360, uses even dimensions and no large allocation.
// Invalid output dimensions fall back to the default 16:9 preview.
std::pair<int, int> previewDimensions(int width, int height) noexcept;
}

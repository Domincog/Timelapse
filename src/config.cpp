#include "config.h"
#include "core.h"
#include <algorithm>
#include <limits>

namespace lapse {
namespace {
bool whitespace(wchar_t c) noexcept {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}
std::wstring_view trimmed(std::wstring_view value) noexcept {
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
    return value;
}
bool videoSizeValid(int width, int height) noexcept {
    return width >= MinVideoDimension && height >= MinVideoDimension &&
        width <= MaxVideoDimension && height <= MaxVideoDimension &&
        width % 2 == 0 && height % 2 == 0 && int64_t(width) * height <= MaxVideoPixels;
}
}
int64_t durationUnitMs(DurationUnit unit) noexcept {
    switch (unit) {
    case DurationUnit::Seconds: return 1000;
    case DurationUnit::Minutes: return 60000;
    case DurationUnit::Hours: return 3600000;
    case DurationUnit::Days: return 86400000;
    }
    return 0;
}
bool parseDuration(std::wstring_view text, DurationUnit unit, int64_t minMs,
                   int64_t maxMs, int64_t quantumMs, int64_t& result,
                   std::wstring& error) {
    error.clear();
    text = trimmed(text);
    const int64_t multiplier = durationUnitMs(unit);
    if (!multiplier || minMs < 0 || maxMs <= 0 || maxMs < minMs || quantumMs <= 0) {
        error = L"The duration limits are invalid.";
        return false;
    }
    if (text.empty() || text.size() > 64) {
        error = minMs == 0 ? L"Enter zero or a positive number." : L"Enter a positive number.";
        return false;
    }
    int64_t whole = 0, fraction = 0, scale = 1;
    bool point = false, digit = false, fractionalDigit = false;
    for (wchar_t c : text) {
        if (c == L'.' && !point && digit) { point = true; continue; }
        if (c < L'0' || c > L'9') {
            error = L"Use digits and an optional decimal point (for example, 1.5).";
            return false;
        }
        digit = true;
        const int64_t value = c - L'0';
        if (!point) {
            if (whole > ((std::numeric_limits<int64_t>::max)() - value) / 10) {
                error = L"The duration is too large.";
                return false;
            }
            whole = whole * 10 + value;
        } else {
            if (scale >= 1000000000) {
                error = L"Use no more than nine decimal places.";
                return false;
            }
            fraction = fraction * 10 + value;
            scale *= 10;
            fractionalDigit = true;
        }
    }
    if (point && !fractionalDigit) {
        error = L"Enter digits after the decimal point.";
        return false;
    }
    if (whole > maxMs / multiplier) {
        error = L"Choose a duration from " + formatDuration(minMs) + L" to " + formatDuration(maxMs) + L".";
        return false;
    }
    // The fractional product is bounded by (1e9-1)*86400000 < INT64_MAX.
    const int64_t fractionalProduct = fraction * multiplier;
    if (fractionalProduct % scale) {
        error = L"The duration must be an exact number of milliseconds.";
        return false;
    }
    const int64_t fractionalMs = fractionalProduct / scale;
    const int64_t wholeMs = whole * multiplier;
    if (fractionalMs > maxMs - wholeMs) {
        error = L"Choose a duration from " + formatDuration(minMs) + L" to " + formatDuration(maxMs) + L".";
        return false;
    }
    const int64_t parsed = wholeMs + fractionalMs;
    if (parsed < minMs) {
        error = L"Choose a duration from " + formatDuration(minMs) + L" to " + formatDuration(maxMs) + L".";
        return false;
    }
    if (parsed % quantumMs) {
        error = quantumMs == 1000 ? L"Use a duration equal to a whole number of seconds." :
            L"Use a duration in exact increments of " + formatDuration(quantumMs) + L".";
        return false;
    }
    result = parsed;
    return true;
}
bool parsePixelDimension(std::wstring_view text, int& result, std::wstring& error) {
    error.clear();
    text = trimmed(text);
    if (text.empty() || text.size() > 16) {
        error = L"Enter a whole number of pixels.";
        return false;
    }
    int value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || value > ((std::numeric_limits<int>::max)() - (c - L'0')) / 10) {
            error = L"Enter a whole number of pixels from 48 to 4096.";
            return false;
        }
        value = value * 10 + (c - L'0');
    }
    if (value < MinVideoDimension || value > MaxVideoDimension || value % 2) {
        error = L"Use an even number of pixels from 48 to 4096.";
        return false;
    }
    result = value;
    return true;
}
bool validateOutputFps(int fps, std::wstring& error) {
    error.clear();
    if (fps < MinOutputFps || fps > MaxOutputFps) {
        error = L"Playback FPS must be a whole number from 1 to 120.";
        return false;
    }
    return true;
}
bool parseOutputFps(std::wstring_view text, int& result, std::wstring& error) {
    error.clear();
    text = trimmed(text);
    int value = 0;
    if (text.empty() || text.size() > 16) {
        error = L"Playback FPS must be a whole number from 1 to 120.";
        return false;
    }
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || value > (MaxOutputFps - (c - L'0')) / 10) {
            error = L"Playback FPS must be a whole number from 1 to 120.";
            return false;
        }
        value = value * 10 + (c - L'0');
    }
    if (!validateOutputFps(value, error)) return false;
    result = value;
    return true;
}
bool validateCaptureInterval(int milliseconds, std::wstring& error) {
    error.clear();
    if (milliseconds < MinCaptureIntervalMs || milliseconds > MaxCaptureIntervalMs) {
        error = L"Capture every must be from 0.1 seconds to 24 hours.";
        return false;
    }
    return true;
}
std::pair<int, int> sourceVideoDimensions(int64_t width, int64_t height) noexcept {
    if (width < MinVideoDimension || height < MinVideoDimension ||
        width > (std::numeric_limits<int>::max)() || height > (std::numeric_limits<int>::max)()) return {};
    const int evenWidth = int(width) & ~1, evenHeight = int(height) & ~1;
    if (videoSizeValid(evenWidth, evenHeight)) return {evenWidth, evenHeight};
    const bool landscape = width >= height;
    const int64_t longer = landscape ? width : height, shorter = landscape ? height : width;
    int low = 1, high = int((std::min)(longer, int64_t(MaxVideoDimension))) / 2;
    int fittedLong = 0, fittedShort = 0;
    // Search even long edges. The proportional short edge and resulting area
    // are monotonic; wide intermediates cover the full accepted source range.
    while (low <= high) {
        const int middle = low + (high - low) / 2, candidateLong = middle * 2;
        const int candidateShort = int(shorter * candidateLong / longer) & ~1;
        if (int64_t(candidateLong) * candidateShort <= MaxVideoPixels) {
            fittedLong = candidateLong; fittedShort = candidateShort; low = middle + 1;
        } else high = middle - 1;
    }
    if (!videoSizeValid(fittedLong, fittedShort)) return {};
    return landscape ? std::pair<int, int>{fittedLong, fittedShort} : std::pair<int, int>{fittedShort, fittedLong};
}
bool validateVideoSize(int width, int height, std::wstring& error) {
    error.clear();
    if (!videoSizeValid(width, height)) {
        error = L"Video size needs even dimensions from 48 to 4096 pixels, with at most 8,847,360 pixels (4096 x 2160).";
        return false;
    }
    return true;
}
bool validateEncodingMode(EncodingMode mode, bool recoveryMode, std::wstring& error) {
    error.clear();
    if (mode != EncodingMode::Compatible && mode != EncodingMode::Efficient &&
        mode != EncodingMode::HardwareH264 && mode != EncodingMode::HardwareHEVC && mode != EncodingMode::QualityH264) {
        error = L"Choose a valid encoding mode.";
        return false;
    }
    if (recoveryMode && mode == EncodingMode::HardwareHEVC) {
        error = L"MP4 recovery mode requires H.264. Choose an H.264 encoder or turn recovery mode off.";
        return false;
    }
    return true;
}
std::wstring formatDuration(int64_t milliseconds, bool compact) {
    if (milliseconds < 0) return L"Invalid duration";
    struct Unit { int64_t ms; const wchar_t* shortName; const wchar_t* singular; const wchar_t* plural; };
    constexpr Unit units[] = {{86400000,L"d",L"day",L"days"},
        {3600000,L"h",L"hour",L"hours"},{60000,L"min",L"minute",L"minutes"},
        {1000,L"s",L"second",L"seconds"}};
    for (const auto& unit : units) {
        const int64_t remainder = milliseconds % unit.ms;
        if ((milliseconds >= unit.ms || unit.ms == 1000) && (remainder * 1000) % unit.ms == 0) {
            auto number = std::to_wstring(milliseconds / unit.ms);
            const int64_t fraction = remainder * 1000 / unit.ms;
            if (fraction) {
                auto decimal = std::to_wstring(1000 + fraction).substr(1);
                while (decimal.back() == L'0') decimal.pop_back();
                number += L"." + decimal;
            }
            return number + (compact ? L" " + std::wstring(unit.shortName) :
                L" " + std::wstring(milliseconds == unit.ms ? unit.singular : unit.plural));
        }
    }
    return L"0 seconds";
}
std::pair<int, int> previewDimensions(int width, int height) noexcept {
    if (!videoSizeValid(width, height)) return {640, 360};
    // Round the shorter fitted edge to the nearest even pixel. UI layout uses
    // the exact output ratio; this bounded bitmap can differ by at most a pixel.
    if (int64_t(width) * 360 >= int64_t(height) * 640) {
        const int scaled = static_cast<int>((int64_t(height) * 320 + width / 2) / width) * 2;
        return {640, std::clamp(scaled, 2, 360)};
    }
    const int scaled = static_cast<int>((int64_t(width) * 180 + height / 2) / height) * 2;
    return {std::clamp(scaled, 2, 640), 360};
}
}

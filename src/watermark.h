#pragma once
#include "core.h"
#include <array>

namespace lapse {
enum class WatermarkTimeKind { ActiveElapsed, RecordedLocal };
enum class WatermarkTextSize { Small, Medium, Large };
struct WatermarkSettings {
    bool enabled = false, showTime = true, showSpeed = true;
    WatermarkTimeKind timeKind = WatermarkTimeKind::ActiveElapsed;
    // Position within the remaining canvas after the tile and safe margins.
    int x = 10000, y = 10000;
    WatermarkTextSize textSize = WatermarkTextSize::Medium;
};
struct WatermarkContext {
    int64_t activeMs = 0;
    SYSTEMTIME recordedLocal{}; // Local wall clock at admission, not source exposure.
    int64_t targetIntervalMs = 5000; // Incoming scheduled capture interval.
    int outputFps = 30; // Frozen saved playback rate.
};
constexpr int64_t WatermarkMaxIntervalMs = 86400000LL * 64;
constexpr size_t WatermarkTextCapacity = 96;
bool validateWatermarkSettings(const WatermarkSettings&, std::wstring& error);
bool sameWatermarkSettings(const WatermarkSettings&, const WatermarkSettings&) noexcept;
// At most two lines: time then Target. Active time uses whole seconds, with
// days capped by the explicit label "Elapsed >999999d". Recorded dates must be
// valid Gregorian local dates in 1601..9999. Target preserves exact thousandths.
// Failure leaves output unchanged. Disabled settings produce an empty string.
bool formatWatermarkText(const WatermarkSettings&, const WatermarkContext&,
                         std::array<wchar_t, WatermarkTextCapacity>& output, std::wstring& error);
class WatermarkRenderer {
public:
    WatermarkRenderer() noexcept;
    ~WatermarkRenderer();
    WatermarkRenderer(const WatermarkRenderer&) = delete;
    WatermarkRenderer& operator=(const WatermarkRenderer&) = delete;
    // Single-thread owned GDI resources. Off allocates none. Enabled prepare
    // preflights the longest supported text before file creation. Relative font
    // heights use the shorter output edge, with legible minimum pixel heights.
    bool prepare(const WatermarkSettings&, int outputWidth, int outputHeight, std::wstring& error);
    // Stamp directly into a valid BGRA frame. Layout is based on prepared output
    // geometry and scaled to this frame (including a smaller disposable preview).
    // Reuses the logical text tile; no full-frame copy or warmed heap allocation.
    // On failure the input frame is unchanged. Alpha inside the box becomes 255.
    // A reset/unprepared renderer is a no-op; callers must check prepare's result.
    bool apply(Frame&, const WatermarkContext&, std::wstring& error);
    void reset() noexcept;
    // Rectangle in the most recent successfully stamped TARGET frame's pixels;
    // empty before a stamp or after reset/disabled preparation.
    RECT lastBounds() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RECT lastBounds_{};
};
}

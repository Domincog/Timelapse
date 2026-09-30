#pragma once
#include "core.h"
#include <array>

namespace lapse {
constexpr unsigned TimeSkipMaxRanges = 16;
constexpr int TimeSkipWidth = 64, TimeSkipHeight = 36;
constexpr int TimeSkipObservationMs = 1000;
enum class TimeSkipMode { Off, Quiet, Manual, QuietWithinSchedule };
struct TimeSkipRange { int startSeconds = 0, endSeconds = 0; };
struct TimeSkipSettings {
    TimeSkipMode mode = TimeSkipMode::Off;
    int multiplier = 4;
    int64_t quietAfterMs = 120000;
    int rampFrames = 30;
    int repeatSeconds = 0;
    unsigned rangeCount = 0;
    std::array<TimeSkipRange, TimeSkipMaxRanges> ranges{};
};
// Validate into a temporary, sort and merge touching/overlapping ranges. On
// failure settings is unchanged. Ranges use active recording time, [start,end).
bool normalizeTimeSkipSettings(TimeSkipSettings& settings, std::wstring& error);
struct TimeSkipDescriptor {
    std::array<uint8_t, TimeSkipWidth * TimeSkipHeight> y{}, u{}, v{};
};
// Fixed sixteen stratified BGRA samples per thumbnail cell; no allocation.
// This is an image-change descriptor, not semantic activity recognition.
bool describeTimeSkipFrame(const Frame& frame, TimeSkipDescriptor& output) noexcept;
enum class TimeSkipReason { Off, Normal, Checking, Quiet, Manual, Unavailable };
struct TimeSkipDecision {
    int64_t intervalMs = 0;
    int64_t nextBoundaryMs = 0; // Absolute active time; zero when no boundary.
    TimeSkipReason reason = TimeSkipReason::Off;
    bool insideSchedule = false;
    bool accelerated = false;
    bool returnToBase = false; // One-shot request to bring the engine deadline forward.
};
class TimeSkipController {
public:
    // sourceMask bits: desktop=1, camera=2. Settings must be normalized. Invalid
    // input returns false and leaves a safe disabled controller. No allocation.
    bool reset(const TimeSkipSettings&, int64_t baseIntervalMs, unsigned sourceMask) noexcept;
    // Only distinct, trusted source observations count. Active time excludes
    // pauses. The engine owns wall-clock freshness and marks stale input unavailable.
    bool observe(unsigned sourceIndex, const TimeSkipDescriptor&, uint64_t epoch,
                 uint64_t sequence, int64_t activeMs) noexcept;
    void unavailable(unsigned sourceIndex) noexcept;
    // Does not advance ramp phase. May request a prompt base-rate return after
    // activity, unavailable input, a schedule boundary or a discontinuous clock.
    TimeSkipDecision inspect(int64_t activeMs) noexcept;
    // Call once per admitted output frame. Returns the following interval;
    // actual capture/admission times and missed-slot handling remain in Engine.
    int64_t onFrame(int64_t activeMs) noexcept;
private:
    struct Detector {
        TimeSkipDescriptor previous, anchor;
        uint64_t epoch = 0, sequence = 0;
        int64_t observationMs = -1, eventMs = 0;
        unsigned weak = 0;
        bool initialized = false, available = false, compared = false;
    };
    TimeSkipSettings settings_;
    std::array<Detector, 2> sources_{};
    std::array<int64_t, 61> intervals_{}, returnSpans_{};
    int64_t baseMs_ = 1000, lastInspectMs_ = -1, lastFrameMs_ = -1, windowStartMs_ = -1, windowEndMs_ = 0;
    unsigned sourceMask_ = 0, phase_ = 0;
    bool valid_ = false, descending_ = false, finished_ = false, returnPending_ = false;
    void baseReturn() noexcept;
    TimeSkipDecision evaluate(int64_t activeMs) noexcept;
};
}

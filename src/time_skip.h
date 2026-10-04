#pragma once
#include "core.h"
#include "config.h"
#include <array>

namespace lapse {
constexpr unsigned TimeSkipMaxRanges = 16;
constexpr int TimeSkipWidth = 64, TimeSkipHeight = 36;
constexpr int TimeSkipObservationMs = 1000;
// Values are saved in preferences; append new modes only. PersonOnly saves
// nothing while no person is detected, after quietAfterMs of absence.
enum class TimeSkipMode { Off, Quiet, Manual, QuietWithinSchedule, NoPerson, NoPersonWithinSchedule, PersonOnly };
enum class QuietSensitivity { Low, Standard, High };
struct TimeSkipRange { int startSeconds = 0, endSeconds = 0; };
struct TimeSkipSettings {
    TimeSkipMode mode = TimeSkipMode::Off;
    int multiplier = 4;
    int64_t quietAfterMs = 120000;
    // Image-change sensitivity applies only to Quiet modes, not person checks.
    QuietSensitivity quietSensitivity = QuietSensitivity::Standard;
    // Completed uncertain person checks can count toward absence. Missing,
    // stale or failed checks must be marked unavailable by the engine.
    bool uncertainAsAbsent = true;
    // Canonical 30fps units: 15/30/60 select 0.5/1/2 seconds of saved video.
    // The controller scales to whole output frames at the session playback FPS.
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
enum class TimeSkipReason { Off, Normal, Checking, Quiet, Manual, Unavailable, NoPerson, PersonPresent, PersonUncertain, NoPersonUncertain };
enum class PersonPresence { Unknown, Present, QualifiedAbsent };
struct PersonObservation {
    PersonPresence presence = PersonPresence::Unknown;
    uint64_t epoch = 0, sequence = 0;
    int64_t activeMs = 0;
};
struct TimeSkipDecision {
    int64_t intervalMs = 0;
    int64_t nextBoundaryMs = 0; // Absolute active time; zero when no boundary.
    TimeSkipReason reason = TimeSkipReason::Off;
    bool insideSchedule = false;
    bool accelerated = false;
    bool returnToBase = false; // One-shot request to bring the engine deadline forward.
    // PersonOnly: admit no frame until a check ends absence (returnToBase).
    bool suspended = false;
};
class TimeSkipController {
public:
    // sourceMask bits: desktop=1, camera=2. Settings must be normalized. Invalid
    // input returns false and leaves a safe disabled controller. No allocation.
    bool reset(const TimeSkipSettings&, int64_t baseIntervalMs, unsigned sourceMask,
               int outputFps = DefaultOutputFps) noexcept;
    // Change capture cadence without losing observation history or ramp phase.
    bool rebase(int64_t baseIntervalMs) noexcept;
    // Only distinct, trusted source observations count. Active time excludes
    // pauses. The engine owns wall-clock freshness and marks stale input unavailable.
    bool observe(unsigned sourceIndex, const TimeSkipDescriptor&, uint64_t epoch,
                 uint64_t sequence, int64_t activeMs) noexcept;
    void unavailable(unsigned sourceIndex) noexcept;
    // Engine validates model/session identity and wall-clock source freshness.
    // A person observation describes the selected camera only. Qualification
    // requires distinct eligible reports spanning the full active-time dwell;
    // the uncertainty setting controls whether healthy Unknown is eligible.
    bool observePerson(const PersonObservation&) noexcept;
    void personUnavailable() noexcept;
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
    struct PersonDetector {
        uint64_t epoch = 0, sequence = 0;
        int64_t observationMs = -1, absentSinceMs = -1;
        unsigned absentCount = 0;
        PersonPresence presence = PersonPresence::Unknown;
        bool initialized = false, available = false;
    };
    TimeSkipSettings settings_;
    std::array<Detector, 2> sources_{};
    PersonDetector person_;
    std::array<int64_t, 2 * MaxOutputFps + 1> intervals_{}, returnSpans_{};
    int64_t baseMs_ = 1000, lastInspectMs_ = -1, lastFrameMs_ = -1, windowStartMs_ = -1, windowEndMs_ = 0;
    unsigned sourceMask_ = 0, phase_ = 0;
    bool valid_ = false, descending_ = false, finished_ = false, returnPending_ = false, suspended_ = false;
    void buildIntervals() noexcept;
    void baseReturn() noexcept;
    TimeSkipDecision evaluate(int64_t activeMs) noexcept;
    TimeSkipDecision evaluateCore(int64_t activeMs) noexcept;
};
}

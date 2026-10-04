#include "time_skip.h"
#include <algorithm>
#include <cmath>
#include <climits>

namespace lapse {
namespace {
constexpr int pixels = TimeSkipWidth * TimeSkipHeight;
enum class Invalid { None, Mode, Multiplier, Quiet, Sensitivity, Ramp, Repeat, Count, Range, Period, Empty };
Invalid normalize(TimeSkipSettings& value) noexcept {
    if (value.mode < TimeSkipMode::Off || value.mode > TimeSkipMode::PersonOnly) return Invalid::Mode;
    if (value.multiplier < 2 || value.multiplier > 64) return Invalid::Multiplier;
    if (value.quietAfterMs < 1000 || value.quietAfterMs > int64_t(INT_MAX) * 1000 || value.quietAfterMs % 1000) return Invalid::Quiet;
    if (value.quietSensitivity < QuietSensitivity::Low || value.quietSensitivity > QuietSensitivity::High) return Invalid::Sensitivity;
    if (value.rampFrames != 15 && value.rampFrames != 30 && value.rampFrames != 60) return Invalid::Ramp;
    if (value.repeatSeconds < 0) return Invalid::Repeat;
    if (value.rangeCount > TimeSkipMaxRanges) return Invalid::Count;
    for (unsigned i = 0; i < value.rangeCount; ++i) {
        const auto& range = value.ranges[i];
        if (range.startSeconds < 0 || range.endSeconds <= range.startSeconds) return Invalid::Range;
        if (value.repeatSeconds && range.endSeconds > value.repeatSeconds) return Invalid::Period;
    }
    std::sort(value.ranges.begin(), value.ranges.begin() + value.rangeCount,
        [](const TimeSkipRange& a, const TimeSkipRange& b) { return a.startSeconds < b.startSeconds; });
    unsigned count = 0;
    for (unsigned i = 0; i < value.rangeCount; ++i) {
        const auto range = value.ranges[i];
        if (count && range.startSeconds <= value.ranges[count - 1].endSeconds)
            value.ranges[count - 1].endSeconds = std::max(range.endSeconds, value.ranges[count - 1].endSeconds);
        else value.ranges[count++] = range;
    }
    value.rangeCount = count;
    for (unsigned i = count; i < TimeSkipMaxRanges; ++i) value.ranges[i] = {};
    if (!count && (value.mode == TimeSkipMode::Manual || value.mode == TimeSkipMode::QuietWithinSchedule ||
                  value.mode == TimeSkipMode::NoPersonWithinSchedule)) return Invalid::Empty;
    return Invalid::None;
}
bool automatic(TimeSkipMode mode) noexcept { return mode == TimeSkipMode::Quiet || mode == TimeSkipMode::QuietWithinSchedule; }
bool personMode(TimeSkipMode mode) noexcept {
    return mode == TimeSkipMode::NoPerson || mode == TimeSkipMode::NoPersonWithinSchedule || mode == TimeSkipMode::PersonOnly;
}
int64_t addBounded(int64_t a, int64_t b) noexcept {
    return b > INT64_MAX - a ? INT64_MAX : a + b;
}
struct Window { bool inside = false; int64_t start = 0, end = 0, next = 0; };
Window windowAt(const TimeSkipSettings& settings, int64_t time) noexcept {
    Window result;
    if (!settings.rangeCount) return result;
    const int64_t period = int64_t(settings.repeatSeconds) * 1000;
    const int64_t origin = period ? time - time % period : 0;
    const int64_t local = time - origin;
    if (period && settings.rangeCount == 1 && settings.ranges[0].startSeconds == 0 &&
        int64_t(settings.ranges[0].endSeconds) * 1000 == period) {
        result.inside = true; return result; // Continuous periodic union, no exit.
    }
    for (unsigned i = 0; i < settings.rangeCount; ++i) {
        const auto& range = settings.ranges[i];
        const int64_t start = int64_t(range.startSeconds) * 1000, end = int64_t(range.endSeconds) * 1000;
        if (local < start) { result.next = addBounded(origin, start); return result; }
        if (local >= end) continue;
        result.inside = true; result.start = addBounded(origin, start); result.end = addBounded(origin, end);
        // The first and last ranges can form one continuous window across a
        // repeat boundary. Avoid a needless slowdown at that internal seam.
        if (period && settings.rangeCount > 1 && settings.ranges[0].startSeconds == 0 &&
            settings.ranges[settings.rangeCount - 1].endSeconds == settings.repeatSeconds) {
            if (i == 0) result.start = origin >= period ? origin - period +
                int64_t(settings.ranges[settings.rangeCount - 1].startSeconds) * 1000 : 0;
            if (i == settings.rangeCount - 1) result.end = addBounded(addBounded(origin, period),
                int64_t(settings.ranges[0].endSeconds) * 1000);
        }
        result.next = result.end; return result;
    }
    if (period) result.next = addBounded(addBounded(origin, period), int64_t(settings.ranges[0].startSeconds) * 1000);
    return result;
}
int percentile(const std::array<uint8_t, pixels>& plane, int numerator, int denominator) noexcept {
    std::array<int, 256> histogram{};
    for (auto value : plane) ++histogram[value];
    const int desired = (pixels * numerator + denominator - 1) / denominator;
    int total = 0;
    for (int value = 0; value < 256; ++value) { total += histogram[value]; if (total >= desired) return value; }
    return 255;
}
int noiseFloor(const std::array<int, 511>& histogram) noexcept {
    int total = 0, median = 0;
    for (int i = 0; i < 511; ++i) { total += histogram[i]; if (total >= pixels / 2) { median = i; break; } }
    total = histogram[median]; int deviation = 0;
    while (total < pixels / 2 && deviation < 255) {
        ++deviation;
        if (median - deviation >= 0) total += histogram[median - deviation];
        if (median + deviation < 511) total += histogram[median + deviation];
    }
    return std::clamp(deviation * 4, 9, 24);
}
struct Difference { bool active = false, strong = false; };
int sensitivityThreshold(int value, QuietSensitivity sensitivity) noexcept {
    return sensitivity == QuietSensitivity::Low ? (value * 3 + 1) / 2 :
        sensitivity == QuietSensitivity::High ? (value * 3 + 3) / 4 : value;
}
Difference compare(const TimeSkipDescriptor& a, const TimeSkipDescriptor& b, QuietSensitivity sensitivity) noexcept {
    const int am = percentile(a.y, 1, 2), bm = percentile(b.y, 1, 2);
    const int ar = percentile(a.y, 3, 4) - percentile(a.y, 1, 4);
    const int br = percentile(b.y, 3, 4) - percentile(b.y, 1, 4);
    const int gain = ar >= 16 ? std::clamp((br * 256 + ar / 2) / ar, 224, 288) : 256;
    const int shift = std::clamp(bm - am, -12, 12);
    std::array<int, 511> hy{}, hu{}, hv{};
    for (int i = 0; i < pixels; ++i) {
        const int predicted = std::clamp(am + shift + ((int(a.y[i]) - am) * gain) / 256, 0, 255);
        ++hy[255 + int(b.y[i]) - predicted]; ++hu[255 + int(b.u[i]) - a.u[i]]; ++hv[255 + int(b.v[i]) - a.v[i]];
    }
    const int ty = sensitivityThreshold(noiseFloor(hy), sensitivity);
    const int tc = sensitivityThreshold(std::max(noiseFloor(hu), noiseFloor(hv)), sensitivity);
    std::array<int, 48> changed{}; int sum = 0, count = 0;
    for (int i = 0; i < pixels; ++i) {
        const int predicted = std::clamp(am + shift + ((int(a.y[i]) - am) * gain) / 256, 0, 255);
        const int dy = std::abs(int(b.y[i]) - predicted);
        const int dc = std::max(std::abs(int(b.u[i]) - a.u[i]), std::abs(int(b.v[i]) - a.v[i]));
        sum += std::max(std::max(0, dy - (ty - 4)), std::max(0, dc - (tc - 4)));
        if (dy >= ty || dc >= tc) { ++count; ++changed[(i / TimeSkipWidth / 6) * 8 + (i % TimeSkipWidth / 8)]; }
    }
    const int maximum = *std::max_element(changed.begin(), changed.end());
    return { count * 40 >= pixels || maximum >= 4 || sum * 2 >= pixels * 3,
             count * 10 >= pixels || maximum >= 12 || sum >= pixels * 5 };
}
}

bool normalizeTimeSkipSettings(TimeSkipSettings& settings, std::wstring& error) {
    error.clear(); auto candidate = settings;
    switch (normalize(candidate)) {
    case Invalid::None: settings = candidate; return true;
    case Invalid::Mode: error = L"Choose a valid time-compression mode."; break;
    case Invalid::Multiplier: error = L"Time-compression speed must be between 2 and 64 times the normal capture interval."; break;
    case Invalid::Quiet: error = settings.mode == TimeSkipMode::PersonOnly
        ? L"Keep-recording time must be a whole number of seconds between 1 and 2147483647."
        : personMode(settings.mode)
        ? L"No-person time must be a whole number of seconds between 1 and 2147483647."
        : L"Quiet time must be a whole number of seconds between 1 and 2147483647."; break;
    case Invalid::Sensitivity: error = L"Choose a valid quiet-scene sensitivity."; break;
    case Invalid::Ramp: error = L"Choose a transition of 0.5, 1, or 2 seconds of saved video."; break;
    case Invalid::Repeat: error = L"The schedule repeat duration cannot be negative."; break;
    case Invalid::Count: error = L"Use at most 16 time-compression ranges."; break;
    case Invalid::Range: error = L"Each range needs a nonnegative start and an end after its start."; break;
    case Invalid::Period: error = L"Each range must end within the schedule repeat duration."; break;
    case Invalid::Empty: error = L"Add at least one time range for this time-compression mode."; break;
    }
    return false;
}
bool describeTimeSkipFrame(const Frame& input, TimeSkipDescriptor& output) noexcept {
    if (!input.valid()) return false;
    for (int ty = 0; ty < TimeSkipHeight; ++ty) for (int tx = 0; tx < TimeSkipWidth; ++tx) {
        int b = 0, g = 0, r = 0;
        for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
            const int x = std::min(input.width - 1, int((int64_t(tx * 8 + sx * 2 + 1) * input.width) / (TimeSkipWidth * 8)));
            const int y = std::min(input.height - 1, int((int64_t(ty * 8 + sy * 2 + 1) * input.height) / (TimeSkipHeight * 8)));
            const auto* p = &input.pixels[(size_t(y) * input.width + x) * 4]; b += p[0]; g += p[1]; r += p[2];
        }
        b = (b + 8) / 16; g = (g + 8) / 16; r = (r + 8) / 16;
        const int y = (19 * b + 183 * g + 54 * r + 128) / 256, at = ty * TimeSkipWidth + tx;
        output.y[at] = static_cast<uint8_t>(y);
        output.u[at] = static_cast<uint8_t>(128 + (b - y) / 2);
        output.v[at] = static_cast<uint8_t>(128 + (r - y) / 2);
    }
    return true;
}
bool TimeSkipController::reset(const TimeSkipSettings& settings, int64_t base, unsigned mask, int fps) noexcept {
    settings_ = settings; valid_ = false; sourceMask_ = mask;
    baseMs_ = base >= 100 && base <= 86400000 ? base : 1000;
    phase_ = 0; descending_ = finished_ = returnPending_ = suspended_ = false;
    lastInspectMs_ = lastFrameMs_ = windowStartMs_ = -1; windowEndMs_ = 0;
    for (auto& source : sources_) { source.initialized = source.available = source.compared = false; source.observationMs = -1; source.weak = 0; }
    person_ = {};
    if (base != baseMs_ || mask > 3 || fps < MinOutputFps || fps > MaxOutputFps ||
        normalize(settings_) != Invalid::None) { settings_.mode = TimeSkipMode::Off; return false; }
    if (automatic(settings_.mode) && !mask) { settings_.mode = TimeSkipMode::Off; return false; }
    if (personMode(settings_.mode) && !(mask & 2)) { settings_.mode = TimeSkipMode::Off; return false; }
    // Preserve the selected duration in saved-video time, to the nearest whole
    // output frame (at least one). The persisted values stay compatible with
    // older preferences; only this owned runtime snapshot is scaled.
    settings_.rampFrames = std::max(1, (settings_.rampFrames * fps + DefaultOutputFps / 2) / DefaultOutputFps);
    buildIntervals();
    valid_ = true; return true;
}
bool TimeSkipController::rebase(int64_t base) noexcept {
    if (!valid_ || base < MinCaptureIntervalMs || base > MaxCaptureIntervalMs) return false;
    baseMs_ = base;
    buildIntervals();
    return true;
}
void TimeSkipController::buildIntervals() noexcept {
    returnSpans_.fill(0);
    for (int i = 0; i <= settings_.rampFrames; ++i) {
        const double x = double(i) / settings_.rampFrames;
        const double eased = x * x * x * (10 + x * (-15 + 6 * x));
        intervals_[i] = static_cast<int64_t>(std::llround(baseMs_ * std::exp(std::log(double(settings_.multiplier)) * eased)));
        returnSpans_[i] = i ? returnSpans_[i - 1] + intervals_[i - 1] : 0;
    }
    intervals_[0] = baseMs_; intervals_[settings_.rampFrames] = baseMs_ * settings_.multiplier;
}
void TimeSkipController::baseReturn() noexcept {
    if (phase_) returnPending_ = true;
    phase_ = 0; descending_ = finished_ = false;
}
void TimeSkipController::unavailable(unsigned index) noexcept {
    if (personMode(settings_.mode)) { if (index == 1) personUnavailable(); return; }
    if (index >= sources_.size() || !(sourceMask_ & (1u << index))) return;
    sources_[index].available = sources_[index].initialized = sources_[index].compared = false;
    sources_[index].weak = 0;
    if (automatic(settings_.mode)) baseReturn();
}
bool TimeSkipController::observe(unsigned index, const TimeSkipDescriptor& input, uint64_t epoch,
                                 uint64_t sequence, int64_t activeMs) noexcept {
    if (personMode(settings_.mode)) return false;
    if (!valid_ || index >= sources_.size() || !(sourceMask_ & (1u << index))) return false;
    auto& source = sources_[index];
    if (!epoch || !sequence || activeMs < 0 || (source.initialized && epoch == source.epoch &&
        (sequence <= source.sequence || activeMs <= source.observationMs))) { unavailable(index); return false; }
    if (!source.initialized || epoch != source.epoch || activeMs - source.observationMs > 3000) {
        source.previous = source.anchor = input; source.epoch = epoch; source.sequence = sequence;
        source.observationMs = source.eventMs = activeMs; source.weak = 0; source.initialized = source.available = true; source.compared = false;
        if (automatic(settings_.mode)) baseReturn();
        return true;
    }
    const auto recent = compare(source.previous, input, settings_.quietSensitivity);
    const auto anchor = compare(source.anchor, input, settings_.quietSensitivity);
    source.compared = true;
    source.weak = recent.active || anchor.active ? source.weak + 1 : 0;
    // Even one plausible weak change ends quiet eligibility. Persistence only
    // controls reference replacement; it must not delay conservative capture.
    if (source.weak) {
        source.eventMs = activeMs;
        if (automatic(settings_.mode)) baseReturn();
    }
    if (recent.strong || anchor.strong || source.weak >= 2) {
        source.anchor = input; source.eventMs = activeMs; source.weak = 0;
        if (automatic(settings_.mode)) baseReturn();
    }
    source.previous = input; source.sequence = sequence; source.observationMs = activeMs; source.available = true;
    return true;
}
void TimeSkipController::personUnavailable() noexcept {
    if (!personMode(settings_.mode)) return;
    person_.available = false; person_.presence = PersonPresence::Unknown;
    person_.absentSinceMs = -1; person_.absentCount = 0;
    baseReturn();
}
bool TimeSkipController::observePerson(const PersonObservation& input) noexcept {
    if (!valid_ || !personMode(settings_.mode) || !(sourceMask_ & 2)) return false;
    if (input.presence < PersonPresence::Unknown || input.presence > PersonPresence::QualifiedAbsent ||
        !input.epoch || !input.sequence || input.activeMs < 0 ||
        (person_.initialized && (input.activeMs <= person_.observationMs ||
            (input.epoch == person_.epoch && input.sequence <= person_.sequence)))) {
        personUnavailable(); return false;
    }
    const bool restart = !person_.initialized || !person_.available || input.epoch != person_.epoch ||
        input.activeMs - person_.observationMs > 3000;
    if (restart) personUnavailable();
    person_.epoch = input.epoch; person_.sequence = input.sequence;
    person_.observationMs = input.activeMs; person_.initialized = true;
    person_.presence = input.presence; person_.available = true;
    if (input.presence == PersonPresence::Present ||
        (input.presence == PersonPresence::Unknown && !settings_.uncertainAsAbsent)) {
        person_.absentSinceMs = -1; person_.absentCount = 0; baseReturn();
    } else {
        if (!person_.absentCount) person_.absentSinceMs = input.activeMs;
        if (person_.absentCount < 2) ++person_.absentCount;
    }
    return true;
}
TimeSkipDecision TimeSkipController::evaluate(int64_t activeMs) noexcept {
    auto result = evaluateCore(activeMs);
    // Leaving a person-only suspension requests a prompt capture; the ramp
    // phase never advances in that mode, so baseReturn() cannot signal it.
    if (suspended_ && !result.suspended) returnPending_ = true;
    suspended_ = result.suspended;
    return result;
}
TimeSkipDecision TimeSkipController::evaluateCore(int64_t activeMs) noexcept {
    TimeSkipDecision result; result.intervalMs = baseMs_;
    if (!valid_ || settings_.mode == TimeSkipMode::Off) return result;
    if (activeMs < 0 || (lastInspectMs_ >= 0 && activeMs < lastInspectMs_)) {
        for (unsigned i = 0; i < sources_.size(); ++i) unavailable(i);
        personUnavailable();
        baseReturn(); windowStartMs_ = -1; windowEndMs_ = 0; lastFrameMs_ = -1;
        result.reason = TimeSkipReason::Unavailable; lastInspectMs_ = activeMs; return result;
    }
    lastInspectMs_ = activeMs;
    const auto window = windowAt(settings_, activeMs);
    const bool scheduled = settings_.mode == TimeSkipMode::Manual || settings_.mode == TimeSkipMode::QuietWithinSchedule ||
        settings_.mode == TimeSkipMode::NoPersonWithinSchedule;
    result.insideSchedule = window.inside;
    if (scheduled) {
        result.nextBoundaryMs = window.next;
        const int64_t start = window.inside ? window.start : -1;
        if (start != windowStartMs_) { baseReturn(); windowStartMs_ = start; }
        windowEndMs_ = window.inside ? window.end : 0;
        if (!window.inside) {
            if (personMode(settings_.mode) && (!person_.initialized || !person_.available ||
                person_.observationMs > activeMs || activeMs - person_.observationMs > 3000))
                personUnavailable();
            baseReturn(); result.reason = TimeSkipReason::Normal; return result;
        }
    }
    if (personMode(settings_.mode)) {
        if (!person_.initialized || !person_.available || person_.observationMs > activeMs || activeMs - person_.observationMs > 3000) {
            personUnavailable(); result.reason = TimeSkipReason::Unavailable; return result;
        }
        if (person_.presence == PersonPresence::Present) {
            baseReturn(); result.reason = TimeSkipReason::PersonPresent; return result;
        }
        if (person_.presence == PersonPresence::Unknown && !settings_.uncertainAsAbsent) {
            baseReturn(); result.reason = TimeSkipReason::PersonUncertain; return result;
        }
        if (person_.absentCount < 2 || person_.observationMs - person_.absentSinceMs < settings_.quietAfterMs) {
            baseReturn(); result.reason = TimeSkipReason::Checking; return result;
        }
        result.reason = person_.presence == PersonPresence::Unknown ? TimeSkipReason::NoPersonUncertain : TimeSkipReason::NoPerson;
        if (settings_.mode == TimeSkipMode::PersonOnly) { result.suspended = true; return result; }
    } else if (automatic(settings_.mode)) {
        bool quiet = true;
        for (unsigned i = 0; i < sources_.size(); ++i) if (sourceMask_ & (1u << i)) {
            auto& source = sources_[i];
            if (!source.initialized || !source.available || source.observationMs > activeMs || activeMs - source.observationMs > 3000) {
                unavailable(i); result.reason = TimeSkipReason::Unavailable; return result;
            }
            if (!source.compared || activeMs - source.eventMs < settings_.quietAfterMs) quiet = false;
        }
        if (!quiet) { baseReturn(); result.reason = TimeSkipReason::Checking; return result; }
        result.reason = TimeSkipReason::Quiet;
    } else result.reason = TimeSkipReason::Manual;
    result.intervalMs = intervals_[phase_]; result.accelerated = result.intervalMs > baseMs_;
    return result;
}
TimeSkipDecision TimeSkipController::inspect(int64_t activeMs) noexcept {
    auto result = evaluate(activeMs); result.returnToBase = returnPending_; returnPending_ = false; return result;
}
int64_t TimeSkipController::onFrame(int64_t activeMs) noexcept {
    const auto status = evaluate(activeMs);
    if (activeMs < 0 || activeMs <= lastFrameMs_) return status.intervalMs;
    lastFrameMs_ = activeMs; returnPending_ = false;
    // Person-only recording either captures at the base cadence or not at all.
    if (settings_.mode == TimeSkipMode::PersonOnly) return baseMs_;
    if (status.reason != TimeSkipReason::Quiet && status.reason != TimeSkipReason::Manual &&
        status.reason != TimeSkipReason::NoPerson && status.reason != TimeSkipReason::NoPersonUncertain) return baseMs_;
    if (finished_) return baseMs_;
    const bool bounded = windowEndMs_ != 0;
    const int64_t remaining = bounded ? std::max(int64_t(0), windowEndMs_ - activeMs) : INT64_MAX;
    if (bounded && phase_ && returnSpans_[phase_] > remaining) {
        // Work delay consumed the reserved slowdown time. Prompt base sampling
        // takes precedence; an actual-time discontinuity cannot be repaired here.
        baseReturn(); finished_ = true; returnPending_ = false; return baseMs_;
    }
    if (descending_) phase_ = phase_ ? phase_ - 1 : 0;
    else {
        const unsigned candidate = std::min(phase_ + 1, static_cast<unsigned>(settings_.rampFrames));
        if (!bounded || intervals_[candidate] + returnSpans_[candidate] <= remaining) phase_ = candidate;
        else { descending_ = true; phase_ = phase_ ? phase_ - 1 : 0; }
    }
    if (!phase_ && descending_) finished_ = true;
    return intervals_[phase_];
}
}

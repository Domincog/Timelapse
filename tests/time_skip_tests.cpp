#include "time_skip.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace { std::atomic<size_t> allocations{0}; }
void* operator new(size_t bytes) {
    allocations.fetch_add(1); if (void* value = std::malloc(bytes ? bytes : 1)) return value; throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
using namespace lapse;
namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
TimeSkipSettings manual(int endSeconds = 7200) {
    TimeSkipSettings settings; settings.mode = TimeSkipMode::Manual; settings.multiplier = 16;
    settings.rangeCount = 1; settings.ranges[0] = {0, endSeconds}; return settings;
}
TimeSkipDescriptor flat(uint8_t value = 80) {
    TimeSkipDescriptor result; result.y.fill(value); result.u.fill(128); result.v.fill(128); return result;
}
void normalization() {
    std::wstring error; auto settings = manual(); settings.rangeCount = 4;
    settings.ranges[0] = {30, 40}; settings.ranges[1] = {0, 10}; settings.ranges[2] = {8, 20}; settings.ranges[3] = {20, 30};
    require(normalizeTimeSkipSettings(settings, error) && settings.rangeCount == 1 &&
        settings.ranges[0].startSeconds == 0 && settings.ranges[0].endSeconds == 40 && settings.ranges[1].endSeconds == 0,
        "Ranges were not sorted/merged/cleared");
    for (int kind = 0; kind < 10; ++kind) {
        auto invalid = settings;
        switch (kind) {
        case 0: invalid.mode = static_cast<TimeSkipMode>(99); break;
        case 1: invalid.multiplier = 65; break;
        case 2: invalid.quietAfterMs = int64_t(INT_MAX) * 1000 + 1; break;
        case 3: invalid.rampFrames = 31; break;
        case 4: invalid.repeatSeconds = -1; break;
        case 5: invalid.rangeCount = 17; break;
        case 6: invalid.ranges[0] = {9, 9}; break;
        case 7: invalid.repeatSeconds = 20; break;
        case 8: invalid.rangeCount = 0; break;
        case 9: invalid.quietAfterMs = 1500; break;
        }
        const auto before = invalid;
        require(!normalizeTimeSkipSettings(invalid, error) && !error.empty() && invalid.rangeCount == before.rangeCount &&
            invalid.ranges[0].endSeconds == before.ranges[0].endSeconds && invalid.mode == before.mode,
            "Invalid settings accepted or partially modified");
        TimeSkipController controller;
        require(!controller.reset(invalid, 1000, 3) && controller.inspect(0).reason == TimeSkipReason::Off,
            "Invalid controller was not safely disabled");
    }
    auto boundary = manual(INT_MAX); boundary.multiplier = 64; boundary.quietAfterMs = int64_t(INT_MAX) * 1000;
    boundary.repeatSeconds = INT_MAX; boundary.rampFrames = 60;
    require(normalizeTimeSkipSettings(boundary, error), "Maximum legal settings rejected");
    std::cout << "PASS normalization, rollback, validation bounds\n";
}
void profile(TimeSkipSettings settings, int64_t base) {
    TimeSkipController controller; require(controller.reset(settings, base, 0), "Prepare manual profile");
    const int64_t end = int64_t(settings.ranges[0].endSeconds) * 1000;
    int64_t now = 0, previous = base; unsigned count = 0;
    const double bound = std::exp(std::log(double(settings.multiplier)) * 1.875 / settings.rampFrames) * (1 + 1. / base);
    while (now < end + base * 2) {
        const auto status = controller.inspect(now);
        const int64_t interval = controller.onFrame(now);
        require(interval >= base && interval <= base * settings.multiplier, "Interval violated configured bounds");
        const double ratio = std::max(double(interval) / previous, double(previous) / interval);
        require(ratio <= bound + 1e-10, "On-time actual frame spacing violated log-ramp bound");
        if (interval > base) require(status.insideSchedule && interval <= end - now, "Accelerated interval crossed manual end");
        now += interval; previous = interval; require(++count < 20000, "Manual scheduling loop was not bounded");
    }
    require(!controller.inspect(now).accelerated, "Manual exit retained acceleration");
}
void manualProfiles() {
    for (int64_t base : {100LL, 137LL, 5000LL, 86400000LL}) for (int multiplier : {2, 4, 16, 64})
        for (int frames : {15, 30, 60}) for (int scale : {1, 2, 3, 5, 10, 20, 60, 200}) for (int offset : {0, 1}) {
            const int duration = static_cast<int>(std::max(int64_t(1), base * scale / 1000));
            auto settings = manual(duration + offset); settings.ranges[0].startSeconds = offset;
            settings.multiplier = multiplier; settings.rampFrames = frames; profile(settings, base);
        }
    TimeSkipController shortRange;
    require(shortRange.reset(manual(2), 5000, 0) && shortRange.onFrame(0) == 5000, "Sub-interval range invented a frame or speedup");
    std::cout << "PASS 768 manual profile/boundary combinations; shorter ranges lower their peak\n";
}
void scheduleBoundaries() {
    auto settings = manual(); settings.rangeCount = 2; settings.ranges[0] = {0, 10}; settings.ranges[1] = {50, 60}; settings.repeatSeconds = 60;
    TimeSkipController controller; require(controller.reset(settings, 100, 0), "Prepare repeat schedule");
    auto status = controller.inspect(49000); require(!status.insideSchedule && status.nextBoundaryMs == 50000, "Next repeat entry wrong");
    for (int i = 0; i < 12; ++i) controller.onFrame(55000 + i * 100);
    const auto before = controller.inspect(59999), after = controller.inspect(60000);
    require(before.accelerated && after.accelerated && !after.returnToBase && after.nextBoundaryMs == 70000,
        "Touching repeat-wrap union restarted or used the wrong end");
    status = controller.inspect(70000); require(!status.insideSchedule && status.returnToBase && status.intervalMs == 100,
        "Half-open manual end did not return to base");
    require(!controller.inspect(70000).returnToBase, "Base return was not one-shot");
    status = controller.inspect(int64_t(INT_MAX) * 1000 + 123); require(status.nextBoundaryMs > int64_t(INT_MAX) * 1000 + 123,
        "Large repeat clock did not advance in constant time");
    auto continuous = manual(60); continuous.repeatSeconds = 60; continuous.multiplier = 64;
    require(controller.reset(continuous, 86400000, 0), "Prepare full-period union");
    int64_t now = 0, last = 0; for (int i = 0; i < 100; ++i) { last = controller.onFrame(now); now += last; }
    require(last == 86400000LL * 64 && controller.inspect(now).nextBoundaryMs == 0, "Full-period union gained a false boundary or 24-hour cap");
    require(controller.inspect(1).returnToBase && !controller.inspect(1).accelerated, "Regressing clock inherited acceleration");
    std::cout << "PASS half-open boundaries, repeating wrap union, multi-year jumps and 64-bit intervals\n";
}
void quietAndSources() {
    TimeSkipSettings settings; settings.mode = TimeSkipMode::Quiet; settings.quietAfterMs = 5000;
    TimeSkipController controller; require(controller.reset(settings, 1000, 3), "Prepare two-source quiet policy");
    auto image = flat(), changed = image; changed.u.fill(210); // Same luma, changed color.
    require(controller.inspect(0).reason == TimeSkipReason::Unavailable, "Unknown source was quiet");
    for (int i = 0; i <= 10; ++i) {
        for (unsigned source = 0; source < 2; ++source)
            require(controller.observe(source, image, 1, i + 1, i * 1000), "Fresh source sample rejected");
        const auto status = controller.inspect(i * 1000);
        require(status.reason == (i < 5 ? TimeSkipReason::Checking : TimeSkipReason::Quiet), "Quiet dwell did not require trusted observations");
        controller.onFrame(i * 1000);
    }
    require(controller.inspect(10000).accelerated, "Qualified sources did not accelerate");
    require(controller.observe(1, changed, 1, 12, 11000), "Chroma change observation failed");
    auto status = controller.inspect(11000);
    require(status.returnToBase && !status.accelerated && status.reason == TimeSkipReason::Checking, "Second-source color activity was hidden");
    require(!controller.inspect(11000).returnToBase, "Activity return repeated");
    require(!controller.observe(0, image, 1, 11, 11000) && controller.inspect(11000).reason == TimeSkipReason::Unavailable,
        "Duplicate sample qualified quiet");
    require(controller.observe(0, image, 2, 1, 12000), "New epoch rejected");
    require(controller.inspect(15001).reason == TimeSkipReason::Unavailable, "Long observation gap remained quiet");
    require(controller.reset(settings, 1000, 3) && controller.inspect(0).reason == TimeSkipReason::Unavailable,
        "Pause/source reset retained quiet or acceleration");
    auto combined = settings; combined.mode = TimeSkipMode::QuietWithinSchedule; combined.rangeCount = 1; combined.ranges[0] = {10, 60};
    require(controller.reset(combined, 1000, 1), "Prepare combined policy");
    for (int i = 0; i <= 10; ++i) {
        controller.observe(0, image, 3, i + 1, i * 1000);
        const auto observed = controller.inspect(i * 1000);
        require(observed.reason == (i < 10 ? TimeSkipReason::Normal : TimeSkipReason::Quiet), "Outside-window observation failed to prequalify entry");
    }
    require(controller.onFrame(10000) >= 1000 && controller.inspect(10000).insideSchedule, "Combined schedule failed admission");
    settings.quietAfterMs = 1000;
    require(controller.reset(settings, 1000, 1) && controller.observe(0, image, 1, 1, 0), "Prepare single-baseline guard");
    require(controller.inspect(1000).reason == TimeSkipReason::Checking && controller.onFrame(1000) == 1000,
        "A single baseline falsely qualified quiet without a comparison");
    require(controller.observe(0, image, 1, 2, 1000) && controller.inspect(1000).reason == TimeSkipReason::Quiet,
        "A second trusted matching observation did not qualify quiet");
    for (int i = 2; i <= 8; ++i) { controller.observe(0, image, 1, i + 1, i * 1000); controller.onFrame(i * 1000); }
    auto weak = image; for (int i = 0; i < 4; ++i) weak.y[i] = 110;
    require(controller.observe(0, weak, 1, 10, 9000), "Weak change observation failed");
    status = controller.inspect(9000);
    require(status.returnToBase && status.reason == TimeSkipReason::Checking && !status.accelerated,
        "First weak activity continued accelerated sampling");
    std::cout << "PASS distinct source observations, conservative union, chroma, quiet dwell, pause/reset and combined prequalification\n";
}
void descriptorNoise() {
    Frame frame{1280, 720, std::vector<uint8_t>(1280 * 720 * 4)};
    TimeSkipSettings settings; settings.mode = TimeSkipMode::Quiet; settings.quietAfterMs = 5000;
    TimeSkipController controller; TimeSkipDescriptor descriptor;
    for (int kind = 0; kind < 3; ++kind) {
        require(controller.reset(settings, 1000, 1), "Reset generated image detector");
        for (int index = 0; index < 14; ++index) {
            uint32_t noise = 77 + index * 139;
            for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x) {
                int base = 55 + (x / 90 + y / 70) % 5 * 22;
                if (kind == 1) base += index & 1 ? 6 : -6;
                if (kind == 2) base = int(base * (index & 1 ? 1.05 : .95));
                for (int channel = 0; channel < 3; ++channel) {
                    noise = noise * 1664525u + 1013904223u;
                    const int value = base + (kind == 0 ? int((noise >> 8) % 25) - 12 : 0);
                    frame.pixels[(size_t(y) * frame.width + x) * 4 + channel] = static_cast<uint8_t>(value);
                }
            }
            require(describeTimeSkipFrame(frame, descriptor), "Generated 720p thumbnail failed");
            controller.observe(0, descriptor, kind + 1, index + 1, index * 1000);
            if (index >= 5) require(controller.inspect(index * 1000).reason == TimeSkipReason::Quiet,
                "Ordinary static noise/exposure fluctuation prevented quiet at 1 Hz");
        }
    }
    Frame invalid; auto before = descriptor;
    require(!describeTimeSkipFrame(invalid, descriptor) && descriptor.y == before.y && descriptor.u == before.u,
        "Invalid descriptor source changed output");
    std::cout << "PASS generated 720p noise/flicker/gain at production 1 Hz; invalid frames preserve descriptor\n";
}
void delaysAndAllocation() {
    TimeSkipController controller; auto settings = manual(30);
    require(controller.reset(settings, 5000, 0), "Prepare delayed policy");
    auto first = controller.onFrame(0); controller.onFrame(first);
    auto status = controller.inspect(60000);
    require(status.returnToBase && status.intervalMs == 5000 && !status.accelerated, "Late range end failed prompt safe base return");
    require(controller.onFrame(60000) == 5000 && controller.onFrame(60000) == 5000, "Duplicate frame advanced phase");
    TimeSkipSettings off; require(controller.reset(off, 86400000, 0) && controller.onFrame(INT64_MAX) == 86400000 &&
        controller.inspect(INT64_MAX).nextBoundaryMs == 0, "Off policy changed normal bounds");
    Frame frame{64, 36, std::vector<uint8_t>(64 * 36 * 4, 90)}; TimeSkipDescriptor image;
    settings.mode = TimeSkipMode::Quiet; settings.quietAfterMs = 1000;
    const auto count = allocations.load();
    require(controller.reset(settings, 1000, 1), "Prepare no-allocation policy");
    for (int i = 0; i < 1000; ++i) {
        describeTimeSkipFrame(frame, image); controller.observe(0, image, 1, i + 1, i * 1000);
        controller.inspect(i * 1000); controller.onFrame(i * 1000);
    }
    require(allocations.load() == count, "Steady descriptor/controller allocated");
    std::cout << "PASS delay/base fallback, duplicate admission, Off and 1000 allocation-free controller observations\n";
}
}
int main() {
    try { normalization(); manualProfiles(); scheduleBoundaries(); quietAndSources(); descriptorNoise(); delaysAndAllocation();
        std::cout << "All time-compression policy contracts passed. Controller bytes: " << sizeof(TimeSkipController) << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

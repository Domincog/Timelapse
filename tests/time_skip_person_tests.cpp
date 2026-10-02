#include "time_skip.h"
#include <atomic>
#include <climits>
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
TimeSkipSettings policy(int64_t dwell = 1000) {
    TimeSkipSettings value; value.mode = TimeSkipMode::NoPerson; value.quietAfterMs = dwell;
    value.rampFrames = 15; return value;
}
bool report(TimeSkipController& controller, int64_t time, uint64_t sequence,
            PersonPresence presence = PersonPresence::QualifiedAbsent, uint64_t epoch = 1) {
    return controller.observePerson({presence, epoch, sequence, time});
}
void accelerate(TimeSkipController& controller, unsigned mask = 2, bool uncertainAsAbsent = true) {
    auto settings = policy(); settings.uncertainAsAbsent = uncertainAsAbsent;
    require(controller.reset(settings, 100, mask), "Initialize person controller");
    require(report(controller, 0, 1), "First absence");
    for (int i = 1; i <= 12; ++i) {
        require(report(controller, i * 1000, i + 1), "Fresh absence");
        controller.onFrame(i * 1000);
    }
    require(controller.inspect(12000).accelerated, "Precondition: person mode accelerated");
}
void returned(TimeSkipController& controller, int64_t time, TimeSkipReason reason) {
    const auto value = controller.inspect(time);
    require(value.intervalMs == 100 && !value.accelerated && value.returnToBase && value.reason == reason,
            "Unsafe input did not promptly restore base and explain why");
    require(!controller.inspect(time).returnToBase, "Return-to-base was not one-shot");
}
void configuration() {
    require(TimeSkipSettings{}.uncertainAsAbsent, "Uncertainty must count as absence by default");
    static_assert(int(TimeSkipMode::Off) == 0 && int(TimeSkipMode::Quiet) == 1 &&
        int(TimeSkipMode::Manual) == 2 && int(TimeSkipMode::QuietWithinSchedule) == 3 &&
        int(TimeSkipMode::NoPerson) == 4 && int(TimeSkipMode::NoPersonWithinSchedule) == 5);
    TimeSkipController controller; std::wstring error;
    for (auto mode : {TimeSkipMode::NoPerson, TimeSkipMode::NoPersonWithinSchedule}) {
        auto settings = policy(); settings.mode = mode;
        settings.rangeCount = 3; settings.ranges[0] = {10, 20}; settings.ranges[1] = {0, 10}; settings.ranges[2] = {19, 30};
        require(normalizeTimeSkipSettings(settings, error) && settings.rangeCount == 1 &&
            settings.ranges[0].startSeconds == 0 && settings.ranges[0].endSeconds == 30, "Person ranges did not reuse canonical union");
        for (unsigned mask : {0u, 1u}) require(!controller.reset(settings, 100, mask) &&
            controller.inspect(0).reason == TimeSkipReason::Off, "Camera-less person policy enabled");
        for (unsigned mask : {2u, 3u}) require(controller.reset(settings, 100, mask), "Selected camera rejected");
    }
    auto invalid = policy(); invalid.mode = TimeSkipMode::NoPersonWithinSchedule;
    require(!normalizeTimeSkipSettings(invalid, error) && invalid.mode == TimeSkipMode::NoPersonWithinSchedule && invalid.rangeCount == 0,
            "Empty person schedule accepted/partially normalized");
    invalid = policy(); invalid.quietAfterMs = 1001;
    require(!normalizeTimeSkipSettings(invalid, error) && invalid.quietAfterMs == 1001, "Fractional absence dwell accepted/modified");
    std::cout << "PASS enum compatibility, camera scope, schedule normalization and atomic validation\n";
}
void fullObservedDwell() {
    TimeSkipController controller; require(controller.reset(policy(), 100, 2), "Dwell reset");
    require(controller.inspect(0).reason == TimeSkipReason::Unavailable, "Unknown initialized absent");
    require(report(controller, 0, 1), "First dwell observation");
    require(controller.inspect(1000).reason == TimeSkipReason::Checking && controller.onFrame(1000) == 100,
            "One absence qualified by waiting");
    require(report(controller, 500, 2), "Second short-span observation");
    require(controller.inspect(1000).reason == TimeSkipReason::Checking, "Inspection alone filled unobserved dwell tail");
    require(report(controller, 1000, 3) && controller.inspect(1000).reason == TimeSkipReason::NoPerson,
            "Exact full observed dwell did not qualify");
    auto settings = policy(5000); require(controller.reset(settings, 100, 2), "Long dwell reset");
    for (int i = 0; i <= 5; ++i) {
        require(report(controller, i * 1000, i + 1), "Long dwell report");
        require(controller.inspect(i * 1000).reason == (i < 5 ? TimeSkipReason::Checking : TimeSkipReason::NoPerson),
                "Long dwell threshold incorrect");
    }
    std::cout << "PASS at least two distinct reports and entire observed absence dwell\n";
}
void presenceAndUnknown() {
    TimeSkipController controller;
    for (auto presence : {PersonPresence::Present, PersonPresence::Unknown}) {
        accelerate(controller, 2, false);
        require(report(controller, 13000, 14, presence), "Typed report rejected");
        returned(controller, 13000, presence == PersonPresence::Present ? TimeSkipReason::PersonPresent : TimeSkipReason::PersonUncertain);
        require(report(controller, 14000, 15) && controller.inspect(14000).reason == TimeSkipReason::Checking,
                "First new negative inherited earlier dwell");
        require(report(controller, 15000, 16) && controller.inspect(15000).reason == TimeSkipReason::NoPerson,
                "New full dwell failed recovery");
    }
    accelerate(controller, 2, false); controller.personUnavailable(); returned(controller, 12000, TimeSkipReason::Unavailable);
    require(!report(controller, 13000, 13), "Unavailable replay erased source watermark");
    require(report(controller, 14000, 14) && controller.inspect(14000).reason == TimeSkipReason::Checking,
            "Fresh report after unavailable did not restart dwell");
    require(report(controller, 15000, 15, PersonPresence::Unknown) &&
        controller.inspect(18000).reason == TimeSkipReason::PersonUncertain &&
        controller.inspect(18001).reason == TimeSkipReason::Unavailable,
        "Healthy uncertain check was confused with missing or stale checks");
    std::cout << "PASS Present/Unknown/stale API clear dwell, preserve identity and recover conservatively\n";
}
void configurableUncertainty() {
    for (auto mode : {TimeSkipMode::NoPerson, TimeSkipMode::NoPersonWithinSchedule}) {
        auto settings = policy(2000); settings.mode = mode;
        settings.rangeCount = 1; settings.ranges[0] = {2, 40};
        TimeSkipController controller;
        require(controller.reset(settings, 100, 2) && controller.inspect(0).reason ==
            (mode == TimeSkipMode::NoPersonWithinSchedule ? TimeSkipReason::Normal : TimeSkipReason::Unavailable),
            "Default uncertainty policy treated a missing check as absence");
        require(report(controller, 0, 1, PersonPresence::Unknown) && controller.onFrame(0) == 100,
            "First uncertain check skipped the configured dwell");
        require(report(controller, 1000, 2) && controller.onFrame(1000) == 100,
            "Mixed absence/uncertainty skipped the full observed dwell");
        require(report(controller, 2000, 3, PersonPresence::Unknown) &&
            controller.inspect(2000).reason == TimeSkipReason::NoPersonUncertain,
            "Default uncertainty policy failed the exact mixed-history dwell");
        uint64_t sequence = 4;
        for (int64_t time = 2100; time <= 4000; time += 100) {
            require(report(controller, time, sequence++, PersonPresence::Unknown), "Fresh uncertain report rejected");
            controller.onFrame(time);
        }
        require(controller.inspect(4000).accelerated && controller.inspect(4000).intervalMs == 400,
            "Uncertain reports did not use the selected capture ramp/multiplier");
        require(report(controller, 4100, sequence++) && controller.inspect(4100).reason == TimeSkipReason::NoPerson &&
            controller.inspect(4100).accelerated && !controller.inspect(4100).returnToBase,
            "A qualified negative discarded eligible uncertainty history");
        require(report(controller, 4200, sequence++, PersonPresence::Present), "Present after uncertainty rejected");
        returned(controller, 4200, TimeSkipReason::PersonPresent);
        require(report(controller, 4300, sequence++, PersonPresence::Unknown) &&
            controller.inspect(4300).reason == TimeSkipReason::Checking,
            "Uncertainty inherited dwell across detected presence");
        require(report(controller, 6300, sequence++, PersonPresence::Unknown), "Fresh replacement uncertainty rejected");
        controller.onFrame(6300);
        require(controller.inspect(6300).reason == TimeSkipReason::NoPersonUncertain,
            "Fresh uncertain history failed to recover after the full dwell");
        controller.personUnavailable();
        require(controller.inspect(6300).reason == TimeSkipReason::Unavailable && controller.inspect(6300).intervalMs == 100,
            "Unavailable input accelerated under the default uncertainty policy");
        require(report(controller, 6400, sequence++, PersonPresence::Unknown) &&
            controller.inspect(6400).reason == TimeSkipReason::Checking,
            "Uncertainty inherited dwell across unavailable input");
        require(controller.inspect(9401).reason == TimeSkipReason::Unavailable &&
            report(controller, 9500, sequence++, PersonPresence::Unknown) &&
            controller.inspect(9500).reason == TimeSkipReason::Checking,
            "Stale uncertainty qualified absence or retained old dwell");
        require(!report(controller, 9600, sequence - 1, PersonPresence::Unknown) &&
            controller.inspect(9600).reason == TimeSkipReason::Unavailable,
            "A duplicate uncertain report qualified absence");
        if (mode == TimeSkipMode::NoPersonWithinSchedule)
            require(controller.inspect(40000).reason == TimeSkipReason::Normal && controller.onFrame(40000) == 100,
                "Uncertainty bypassed the scheduled range end");

        settings.uncertainAsAbsent = false;
        require(controller.reset(settings, 100, 2), "Strict uncertainty policy rejected");
        for (int i = 0; i <= 12; ++i) {
            require(report(controller, int64_t(i) * 1000, uint64_t(i) + 1, PersonPresence::Unknown),
                "Strict fresh uncertainty rejected");
            const auto status = controller.inspect(int64_t(i) * 1000);
            require(status.reason == (mode == TimeSkipMode::NoPersonWithinSchedule && i < 2
                    ? TimeSkipReason::Normal : TimeSkipReason::PersonUncertain) &&
                !status.accelerated && controller.onFrame(int64_t(i) * 1000) == 100,
                "Opted-out uncertainty qualified absence or changed cadence");
        }
        require(report(controller, 13000, 14) && controller.inspect(13000).reason == TimeSkipReason::Checking &&
            report(controller, 14000, 15) && controller.inspect(14000).reason == TimeSkipReason::Checking &&
            report(controller, 15000, 16) && controller.inspect(15000).reason == TimeSkipReason::NoPerson,
            "Opt-out retained uncertain dwell or prevented qualified absence");
    }
    std::cout << "PASS default uncertainty eligibility, full mixed dwell, truthful status, presence/failure/stale resets, schedule gate and opt-out\n";
}
void rejectedInput() {
    TimeSkipController controller;
    for (int kind = 0; kind < 8; ++kind) {
        accelerate(controller);
        PersonObservation input{PersonPresence::QualifiedAbsent, 1, 14, 13000};
        switch (kind) {
        case 0: input.epoch = 0; break;
        case 1: input.sequence = 0; break;
        case 2: input.sequence = 13; break;
        case 3: input.sequence = 12; break;
        case 4: input.activeMs = 12000; break;
        case 5: input.activeMs = 11999; break;
        case 6: input.activeMs = -1; break;
        case 7: input.presence = static_cast<PersonPresence>(99); break;
        }
        require(!controller.observePerson(input), "Malformed/duplicate/backwards report accepted");
        returned(controller, 13000, TimeSkipReason::Unavailable);
    }
    accelerate(controller); require(report(controller, 13000, 14), "Future report admission");
    returned(controller, 12500, TimeSkipReason::Unavailable);
    accelerate(controller); returned(controller, 11000, TimeSkipReason::Unavailable);
    accelerate(controller);
    require(controller.inspect(14000).reason == TimeSkipReason::NoPerson, "Fresh pre-regression person history");
    returned(controller, 13000, TimeSkipReason::Unavailable);
    require(controller.inspect(14000).reason == TimeSkipReason::Unavailable,
            "Clock regression retained earlier qualified absence");
    require(report(controller, 14000, 14) && controller.inspect(14000).reason == TimeSkipReason::Checking,
            "Post-regression report inherited earlier dwell");
    require(controller.reset(policy(), 100, 2) && report(controller, 0, 1) &&
        controller.inspect(0).reason == TimeSkipReason::Checking, "Reset retained old clock/source qualification");
    std::cout << "PASS invalid IDs/states, duplicate/reordered samples, future/regressing clocks and reset\n";
}
void gapsAndEpochs() {
    TimeSkipController controller; accelerate(controller);
    require(controller.inspect(15000).reason == TimeSkipReason::NoPerson, "Exact 3-second freshness boundary rejected");
    returned(controller, 15001, TimeSkipReason::Unavailable);
    accelerate(controller); require(report(controller, 15001, 14), "Fresh sample after gap rejected");
    returned(controller, 15001, TimeSkipReason::Checking);
    accelerate(controller); require(report(controller, 15000, 14) && controller.inspect(15000).reason == TimeSkipReason::NoPerson,
            "Exact allowed observation gap lost dwell");
    accelerate(controller); require(report(controller, 13000, 1, PersonPresence::QualifiedAbsent, 2), "New source epoch rejected");
    returned(controller, 13000, TimeSkipReason::Checking);
    require(report(controller, 14000, 2, PersonPresence::QualifiedAbsent, 2) &&
        controller.inspect(14000).reason == TimeSkipReason::NoPerson, "New epoch did not require/recover full dwell");
    std::cout << "PASS exact gap boundaries, silent staleness and source-epoch resets\n";
}
void inputIsolation() {
    TimeSkipController camera, paired; accelerate(camera, 2); accelerate(paired, 3);
    TimeSkipDescriptor changed; changed.y.fill(255); changed.u.fill(0); changed.v.fill(255);
    for (unsigned index : {0u, 1u}) require(!paired.observe(index, changed, 99, 999, 13000), "Image observations entered person detector");
    paired.unavailable(0); require(paired.inspect(12000).accelerated, "Unrelated desktop failure cleared camera absence");
    require(camera.onFrame(13000) == paired.onFrame(13000), "Paired files did not share camera-only cadence");
    paired.unavailable(1); returned(paired, 13000, TimeSkipReason::Unavailable);
    for (auto mode : {TimeSkipMode::Off, TimeSkipMode::Quiet, TimeSkipMode::Manual, TimeSkipMode::QuietWithinSchedule}) {
        auto settings = policy(); settings.mode = mode; settings.rangeCount = 1; settings.ranges[0] = {0, 1000};
        TimeSkipController actual, expected; require(actual.reset(settings, 100, 3) && expected.reset(settings, 100, 3), "Legacy mode reset");
        for (int i = 0; i < 10; ++i) {
            require(!report(actual, i * 1000, i + 1), "Person observations entered legacy mode"); actual.personUnavailable();
            require(actual.onFrame(i * 1000) == expected.onFrame(i * 1000), "Person API changed old mode cadence");
            require(actual.inspect(i * 1000).reason == expected.inspect(i * 1000).reason, "Person API changed old mode reason");
        }
    }
    std::cout << "PASS image/person separation, camera-only paired cadence and unchanged legacy modes\n";
}
void schedules() {
    auto settings = policy(); settings.mode = TimeSkipMode::NoPersonWithinSchedule;
    settings.rangeCount = 1; settings.ranges[0] = {10, 20};
    TimeSkipController controller; require(controller.reset(settings, 100, 2), "Person schedule reset");
    for (int i = 0; i <= 10; ++i) {
        require(report(controller, i * 1000, i + 1), "Outside-schedule qualification failed");
        const auto state = controller.inspect(i * 1000);
        require(state.reason == (i < 10 ? TimeSkipReason::Normal : TimeSkipReason::NoPerson), "Person schedule was not AND gated");
        controller.onFrame(i * 1000);
    }
    for (int i = 11; i <= 14; ++i) { report(controller, i * 1000, i + 1); controller.onFrame(i * 1000); }
    require(controller.inspect(14000).accelerated, "Scheduled absence failed acceleration");
    returned(controller, 20000, TimeSkipReason::Normal);
    settings.ranges[0] = {0, 10}; settings.ranges[1] = {50, 60}; settings.rangeCount = 2; settings.repeatSeconds = 60;
    require(controller.reset(settings, 100, 2), "Wrap reset");
    report(controller, 49000, 1); report(controller, 50000, 2);
    uint64_t sequence = 3;
    for (int64_t time = 50100; time <= 59900; time += 100) { report(controller, time, sequence++); controller.onFrame(time); }
    auto before = controller.inspect(59999), after = controller.inspect(60000);
    require(before.accelerated && after.accelerated && !after.returnToBase && after.nextBoundaryMs == 70000,
            "Person repeat seam introduced artificial slowdown");
    std::cout << "PASS schedule AND, outside-window qualification, half-open end and repeat union\n";
}
void rampEquivalence() {
    unsigned combinations = 0;
    for (int base : {100, 137, 1000}) for (int speed : {2, 4, 16, 64}) for (int frames : {15, 30, 60}) {
        auto absent = policy(); absent.mode = TimeSkipMode::NoPersonWithinSchedule;
        absent.multiplier = speed; absent.rampFrames = frames; absent.rangeCount = 1; absent.ranges[0] = {1, 40};
        auto manual = absent; manual.mode = TimeSkipMode::Manual;
        TimeSkipController person, control; require(person.reset(absent, base, 2) && control.reset(manual, base, 0), "Ramp reset");
        report(person, 0, 1); report(person, 1000, 2);
        int64_t frame = 1000, observation = 2000; uint64_t sequence = 3;
        while (frame <= 42000) {
            while (observation <= frame) { require(report(person, observation, sequence++), "Ramp fresh report"); observation += 1000; }
            const auto first = person.onFrame(frame), second = control.onFrame(frame);
            require(first == second, "Qualified person mode changed established manual ramp/known-end slowdown");
            frame += first;
        }
        ++combinations;
    }
    std::cout << "PASS " << combinations << " person/manual ramp and known-end slowdown equivalence cases\n";
}
void scheduledStaleHistory() {
    auto settings = policy(5000); settings.mode = TimeSkipMode::NoPersonWithinSchedule;
    settings.rangeCount = 1; settings.ranges[0] = {10, 20};
    for (int kind = 0; kind < 3; ++kind) {
        TimeSkipController controller; require(controller.reset(settings, 100, 2), "Scheduled stale reset");
        for (int i = 0; i <= 6; ++i) {
            require(report(controller, i * 1000, i + 1), "Initial outside-window absence");
            require(controller.inspect(i * 1000).reason == TimeSkipReason::Normal && controller.onFrame(i * 1000) == 100,
                "Outside-window cadence changed");
        }
        if (kind == 1) {
            for (int i = 7; i <= 10; ++i) require(report(controller, i * 1000, i + 1), "Continuously fresh absence");
        } else {
            require(controller.inspect(kind == 0 ? 9001 : 9000).reason == TimeSkipReason::Normal,
                "Outside-window status changed on age inspection");
            require(report(controller, 9000, 8) && report(controller, 10000, 9), "Delayed but fresh source reports");
        }
        const auto entry = controller.inspect(10000);
        require(entry.insideSchedule && entry.reason == (kind == 0 ? TimeSkipReason::Checking : TimeSkipReason::NoPerson),
            "Outside-window stale inspection retained dwell, or valid boundary/fresh history was lost");
        if (kind == 0) {
            for (int64_t frame = 10000; frame <= 10600; frame += 100)
                require(controller.onFrame(frame) == 100, "Incomplete replacement dwell advanced ramp");
            for (int i = 11; i <= 14; ++i) {
                require(report(controller, i * 1000, i - 1), "Replacement absence report");
                require(controller.inspect(i * 1000).reason == (i < 14 ? TimeSkipReason::Checking : TimeSkipReason::NoPerson),
                    "Replacement dwell failed exact five-second recovery");
            }
        } else {
            int64_t following = 100;
            for (int64_t frame = 10000; frame <= 10600; frame += 100) following = controller.onFrame(frame);
            require(following > 100 && controller.inspect(10600).accelerated, "Fresh scheduled absence failed to accelerate");
        }
    }
    std::cout << "PASS scheduled stale inspection resets dwell; fresh and exact-three-second controls remain eligible\n";
}
void boundedMemoryAndLargeTime() {
    auto settings = policy(1000); settings.multiplier = 64; settings.rampFrames = 60;
    TimeSkipController controller;
    const auto before = allocations.load(); require(controller.reset(settings, 86400000, 2), "Large-interval reset");
    for (int i = 0; i < 10000; ++i) {
        require(report(controller, int64_t(i) * 1000, uint64_t(i) + 1), "Allocation run report");
        controller.inspect(int64_t(i) * 1000); controller.onFrame(int64_t(i) * 1000);
    }
    require(allocations.load() == before, "Person steady state allocated");
    require(controller.inspect(9999000).intervalMs == 86400000LL * 64, "Person scaled interval truncated to 24 hours");
    require(controller.reset(settings, 100, 2) && report(controller, INT64_MAX - 2000, 1) &&
        report(controller, INT64_MAX - 1000, 2) && controller.inspect(INT64_MAX).reason == TimeSkipReason::NoPerson,
        "Large active time overflowed qualification/freshness");
    settings.quietAfterMs = int64_t(INT_MAX) * 1000; require(controller.reset(settings, 100, 2), "Maximum dwell reset");
    require(report(controller, 0, 1) && report(controller, 1000, 2) && controller.inspect(1000).reason == TimeSkipReason::Checking,
        "Maximum dwell overflowed into eligibility");
    std::cout << "PASS 10,000 allocation-free reports, bounded state, 64-bit intervals and extreme active times\n";
}
}
int main() {
    try {
        configuration(); fullObservedDwell(); presenceAndUnknown(); configurableUncertainty(); rejectedInput(); gapsAndEpochs(); inputIsolation();
        schedules(); rampEquivalence(); scheduledStaleHistory(); boundedMemoryAndLargeTime();
        std::cout << "Person policy passed. Controller bytes: " << sizeof(TimeSkipController)
                  << "; observation bytes: " << sizeof(PersonObservation) << "; settings bytes: " << sizeof(TimeSkipSettings) << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

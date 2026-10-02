#pragma once
#include "camera_host.h"
#include <cmath>
#include <memory>

namespace lapse {
struct PersonCheckResult {
    person::Source source;
    person::Output output;
    bool sufficientDetail = false; // Detail of this exact submitted thumbnail.
};
// The pinned worker's conservative absence threshold leaves ordinary empty
// rooms in its Ambiguous band. For cadence, an informative, healthy image with
// every raw person score below the detection threshold means no person detected.
// Preserve the wire verdict and all uncertain/invalid/low-detail safeguards.
inline person::Verdict personCadenceVerdict(const person::Output& output, bool sufficientDetail) noexcept {
    if (!std::isfinite(output.rawMaxPerson) || !std::isfinite(output.validMaxPerson) ||
        output.rawMaxPerson < 0 || output.rawMaxPerson > 1 || output.validMaxPerson < 0 ||
        output.validMaxPerson > output.rawMaxPerson) return person::Verdict::Unknown;
    if (output.verdict == person::Verdict::Present && output.reason == person::Reason::None &&
        output.validMaxPerson >= person::PresentThreshold) return person::Verdict::Present;
    if (output.verdict == person::Verdict::QualifiedAbsent && output.reason == person::Reason::None &&
        output.rawMaxPerson <= person::AbsentThreshold) return person::Verdict::QualifiedAbsent;
    if (output.verdict == person::Verdict::Unknown && output.reason == person::Reason::Ambiguous &&
        output.rawMaxPerson < person::PresentThreshold && sufficientDetail) return person::Verdict::QualifiedAbsent;
    return person::Verdict::Unknown;
}
enum class PersonPoll { Pending, Ready, Complete, Unavailable };
// Owns only the optional detector. Startup file verification/launch is performed
// outside the recording worker. Poll, submit and cancellation never wait for
// inference or process shutdown. One startup/inference and no request queue.
class PersonClient {
public:
    PersonClient();
    ~PersonClient();
    PersonClient(const PersonClient&) = delete;
    PersonClient& operator=(const PersonClient&) = delete;
    bool start(uint64_t sessionToken, std::wstring& error);
    void cancel() noexcept;
    PersonPoll poll(PersonCheckResult&) noexcept;
    bool submit(const CameraPersonInput&) noexcept;
    const wchar_t* diagnostic() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

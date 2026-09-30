#pragma once
#include "camera_host.h"
#include <memory>

namespace lapse {
struct PersonCheckResult {
    person::Source source;
    person::Output output;
};
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

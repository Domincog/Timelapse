#include "person_client.h"
#include "person_pack.h"
#include <objbase.h>
#include <process.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace lapse {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    void reset() noexcept { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};
struct Lock {
    HANDLE handle;
    DWORD result;
    explicit Lock(HANDLE h) noexcept : handle(h), result(WaitForSingleObject(h, 0)) {}
    ~Lock() { if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) ReleaseMutex(handle); }
    explicit operator bool() const noexcept { return result == WAIT_OBJECT_0; }
};
struct Transport {
    Handle mapping, mutex, stop, request, job, process, pack;
    person::Shared* shared = nullptr;
    uint64_t next = 0, pending = 0, submitted = 0, contentionSince = 0;
    person::Source expected{}, lastAccepted{};
    bool ready = false, retired = false;
    ~Transport() { retire(); if (shared) UnmapViewOfFile(shared); }
    void retire() noexcept {
        retired = true; pending = 0;
        if (stop.value) SetEvent(stop.value);
        // The job contains only our optional detector. Never wait here.
        job.reset();
        pack.reset();
    }
    bool terminal() const noexcept { return !process.value || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT; }
};
struct Startup {
    std::atomic<bool> cancelled{false}, done{false};
    Handle cancellation;
    std::unique_ptr<Transport> transport;
    std::array<wchar_t, 256> error{};
};
// A cancelled filesystem/OS launch call may still be completing on its private
// startup thread. Do not create another such task until that thread releases it.
std::atomic<bool> startupBusy{false};
void message(std::array<wchar_t, 256>& destination, const wchar_t* text) noexcept {
    destination = {};
    std::copy_n(text, std::min(std::wcslen(text), destination.size() - 1), destination.begin());
}
bool launch(Startup& startup, Handle& observer) {
    auto transport = std::make_unique<Transport>();
    std::wstring path, error;
    transport->pack.value = openVerifiedPersonPack(path, error, &startup.cancelled);
    if (!transport->pack.value) { message(startup.error, error.c_str()); return false; }
    if (startup.cancelled.load()) return false;
    GUID guid{}; wchar_t token[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, static_cast<int>(std::size(token)))) return false;
    const std::wstring name = std::wstring(L"Local\\Timelapse.Person.") + token;
    transport->mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(person::Shared), name.c_str());
    if (!transport->mapping.value || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    transport->mutex.value = CreateMutexW(nullptr, FALSE, (name + L".mutex").c_str());
    if (!transport->mutex.value || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    transport->stop.value = CreateEventW(nullptr, TRUE, FALSE, (name + L".stop").c_str());
    if (!transport->stop.value || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    transport->request.value = CreateEventW(nullptr, FALSE, FALSE, (name + L".request").c_str());
    if (!transport->request.value || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    transport->shared = static_cast<person::Shared*>(MapViewOfFile(transport->mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(person::Shared)));
    if (!transport->shared) return false;
    // New mapping pages are already zero. Avoid touching the large pixel region.
    auto& shared = *transport->shared;
    shared.magic = person::Magic; shared.version = person::ProtocolVersion; shared.structBytes = sizeof(shared);
    shared.modelRevision = person::ModelRevision; shared.preprocessingRevision = person::PreprocessingRevision;
    shared.phase = person::Phase::Starting;
    transport->job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!transport->job.value || !SetInformationJobObject(transport->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
    if (startup.cancelled.load()) return false;
    std::wstring command = L"\"" + path + L"\" --person-host \"" + name + L"\"";
    STARTUPINFOW info{}; info.cb = sizeof(info); PROCESS_INFORMATION child{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, nullptr, &info, &child)) return false;
    transport->process.value = child.hProcess; Handle thread; thread.value = child.hThread;
    // Track every created child, including failures before transport publication.
    // Keeping this handle in the initializer also keeps the global launch gate
    // closed until cancellation or a failed launch has reached actual exit.
    if (!DuplicateHandle(GetCurrentProcess(), child.hProcess, GetCurrentProcess(), &observer.value,
                         SYNCHRONIZE | PROCESS_TERMINATE, FALSE, 0)) {
        TerminateProcess(child.hProcess, ERROR_PROCESS_ABORTED);
        WaitForSingleObject(child.hProcess, INFINITE); // Private startup thread only.
        return false;
    }
    if (!AssignProcessToJobObject(transport->job.value, child.hProcess)) {
        TerminateProcess(child.hProcess, ERROR_PROCESS_ABORTED); return false;
    }
    if (startup.cancelled.load() || ResumeThread(thread.value) == static_cast<DWORD>(-1)) return false;
    startup.transport = std::move(transport);
    return true;
}
unsigned __stdcall initialize(void* argument) noexcept {
    std::unique_ptr<std::shared_ptr<Startup>> owned(static_cast<std::shared_ptr<Startup>*>(argument));
    const auto startup = *owned;
    Handle observer;
    try {
        if (!launch(*startup, observer) && !startup->error[0]) message(startup->error, L"The optional person detector could not start; using normal cadence.");
    } catch (...) { message(startup->error, L"The optional person detector could not allocate startup resources; using normal cadence."); }
    if (startup->cancelled.load() && startup->transport) startup->transport->retire();
#ifdef LAPSE_PERSON_CLIENT_BEFORE_PUBLISH
    LAPSE_PERSON_CLIENT_BEFORE_PUBLISH(); // Deterministic fixture for the cancellation publication boundary.
#endif
    // Only the release store exposes all startup-owned values to the parent.
    startup->done.store(true, std::memory_order_release);
    // This existing startup thread becomes a zero-wakeup process observer. It
    // keeps the single-detector gate until cancellation has actually terminated
    // the old child, without making Record/Pause/Finish wait or poll in idle.
    if (observer.value) {
        HANDLE waits[] = {observer.value, startup->cancellation.value};
        const DWORD wake = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wake != WAIT_OBJECT_0) {
            // Covers cancellation between the final flag read and done.store:
            // the parent may have observed done=false and could not retire the
            // unpublished transport. This event cannot lose that cancellation.
            TerminateProcess(observer.value, ERROR_PROCESS_ABORTED);
            WaitForSingleObject(observer.value, INFINITE);
        }
    }
    observer.reset();
    startupBusy.store(false, std::memory_order_release);
    return 0;
}
bool validOutput(const person::Output& output) noexcept {
    if (!std::isfinite(output.rawMaxPerson) || !std::isfinite(output.validMaxPerson) ||
        output.rawMaxPerson < 0 || output.rawMaxPerson > 1 || output.validMaxPerson < 0 ||
        output.validMaxPerson > output.rawMaxPerson || output.verdict > person::Verdict::Present ||
        output.reason > person::Reason::InsufficientDetail) return false;
    if (output.verdict == person::Verdict::Present)
        return output.validMaxPerson >= person::PresentThreshold && output.reason == person::Reason::None;
    if (output.verdict == person::Verdict::QualifiedAbsent)
        return output.rawMaxPerson <= person::AbsentThreshold && output.reason == person::Reason::None;
    return output.reason != person::Reason::None;
}
}
struct PersonClient::Impl {
    std::shared_ptr<Startup> startup;
    std::unique_ptr<Transport> active, retired;
    uint64_t token = 0, started = 0;
    bool cancelled = true, failed = false;
    std::array<wchar_t, 256> detail{};
    void retire() noexcept {
        cancelled = true;
        if (startup) {
            startup->cancelled.store(true);
            if (startup->cancellation.value) SetEvent(startup->cancellation.value);
            if (startup->done.load(std::memory_order_acquire) && startup->transport) startup->transport->retire();
        }
        if (active) { active->retire(); retired = std::move(active); }
    }
    PersonPoll fail(const wchar_t* text) noexcept {
        failed = true; message(detail, text); retire(); return PersonPoll::Unavailable;
    }
};
PersonClient::PersonClient() : impl_(std::make_unique<Impl>()) {}
PersonClient::~PersonClient() { cancel(); }
void PersonClient::cancel() noexcept { impl_->retire(); }
const wchar_t* PersonClient::diagnostic() const noexcept { return impl_->detail.data(); }
bool PersonClient::start(uint64_t token, std::wstring& error) {
    error.clear(); cancel();
    auto& state = *impl_;
    if (!token) { error = L"Person checks require a valid recording session."; return false; }
    if (state.startup && !state.startup->done.load(std::memory_order_acquire)) {
        error = L"The previous optional detector startup is still closing; using normal cadence."; return false;
    }
    if (state.startup && state.startup->transport) state.retired = std::move(state.startup->transport);
    state.startup.reset();
    if (state.retired && !state.retired->terminal()) {
        error = L"The previous optional detector is still closing; using normal cadence."; return false;
    }
    state.retired.reset();
    bool expected = false;
    if (!startupBusy.compare_exchange_strong(expected, true)) {
        error = L"An optional detector startup is still closing; using normal cadence."; return false;
    }
    try {
        state.startup = std::make_shared<Startup>();
        state.startup->cancellation.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!state.startup->cancellation.value) {
            state.startup.reset(); startupBusy.store(false);
            error = L"Windows could not create optional person cancellation."; return false;
        }
        auto argument = std::make_unique<std::shared_ptr<Startup>>(state.startup);
        const uintptr_t thread = _beginthreadex(nullptr, 0, initialize, argument.get(), 0, nullptr);
        if (!thread) { state.startup.reset(); startupBusy.store(false); error = L"Windows could not start optional person checks."; return false; }
        argument.release(); CloseHandle(reinterpret_cast<HANDLE>(thread));
        state.token = token; state.started = GetTickCount64(); state.cancelled = state.failed = false; state.detail = {};
        return true;
    } catch (...) {
        state.startup.reset(); startupBusy.store(false);
        error = L"Not enough memory to start optional person checks; using normal cadence."; return false;
    }
}
PersonPoll PersonClient::poll(PersonCheckResult& output) noexcept {
    auto& state = *impl_;
    if (state.cancelled || state.failed) return PersonPoll::Unavailable;
    uint64_t now = GetTickCount64();
    if (!state.active) {
        if (now < state.started || now - state.started >= 8000) return state.fail(L"Person detector startup timed out; using normal cadence.");
        if (!state.startup || !state.startup->done.load(std::memory_order_acquire)) return PersonPoll::Pending;
        if (!state.startup->transport) return state.fail(state.startup->error.data());
        state.active = std::move(state.startup->transport); state.startup.reset();
    }
    auto& transport = *state.active;
    if (transport.terminal()) return state.fail(L"The optional person detector stopped; using normal cadence.");
    if (transport.pending && (now < transport.submitted || now - transport.submitted >= 3000))
        return state.fail(L"Person inference timed out; using normal cadence.");
    if (!transport.ready && now - state.started >= 8000) return state.fail(L"Person detector startup timed out; using normal cadence.");
    Lock lock(transport.mutex.value);
    if (!lock) {
        if (lock.result != WAIT_TIMEOUT) return state.fail(L"Person detector sharing failed; using normal cadence.");
        if (!transport.contentionSince) transport.contentionSince = now;
        if (now - transport.contentionSince >= 3000) return state.fail(L"Person detector sharing timed out; using normal cadence.");
        return PersonPoll::Pending;
    }
    transport.contentionSince = 0;
    auto& shared = *transport.shared;
    const bool publicationMatches = shared.published == transport.next ||
        (transport.pending && transport.next && shared.published == transport.next - 1);
    if (!person::validHeader(shared) || !publicationMatches || shared.phase == person::Phase::Failed || shared.requested != transport.next ||
        (transport.ready && shared.phase != person::Phase::Ready)) return state.fail(L"The optional detector returned invalid data; using normal cadence.");
    if (shared.phase != person::Phase::Ready) return PersonPoll::Pending;
    transport.ready = true;
    if (!transport.pending) return PersonPoll::Ready;
    if (!person::sameSource(shared.inputSource, transport.expected) ||
        shared.inputBytes != transport.expected.width * transport.expected.height * 3)
        return state.fail(L"The optional detector changed input identity; using normal cadence.");
    if (shared.published != transport.pending) return PersonPoll::Pending;
    now = GetTickCount64();
    if (!person::sameSource(shared.outputSource, transport.expected) || !validOutput(shared.output) ||
        now < transport.submitted || now - transport.submitted >= 3000)
        return state.fail(L"The optional detector returned an invalid result; using normal cadence.");
    output.source = transport.expected; output.output = shared.output;
    // A late but otherwise well-formed source is Unknown, not new absence.
    if (output.source.receivedTick > now || now - output.source.receivedTick > person::SourceFreshnessMs) {
        output.output.verdict = person::Verdict::Unknown; output.output.reason = person::Reason::StaleSource;
    }
    transport.lastAccepted = transport.expected; transport.pending = 0;
    return PersonPoll::Complete;
}
bool PersonClient::submit(const CameraPersonInput& input) noexcept {
    auto& state = *impl_;
    if (state.cancelled || !state.active) return false;
    auto& transport = *state.active;
    const auto& source = input.source;
    const uint64_t now = GetTickCount64();
    if (!transport.ready || transport.pending || transport.retired || !person::validGeometry(source) ||
        source.sessionToken != state.token || !source.cameraEpoch || !source.sequence || !source.receivedTick ||
        source.receivedTick < state.started || source.receivedTick > now || now - source.receivedTick > person::SourceFreshnessMs ||
        (transport.lastAccepted.cameraEpoch && (source.receivedTick < transport.lastAccepted.receivedTick ||
         source.cameraEpoch < transport.lastAccepted.cameraEpoch ||
         (source.cameraEpoch == transport.lastAccepted.cameraEpoch && source.sequence <= transport.lastAccepted.sequence)))) return false;
    if (transport.terminal()) { state.fail(L"The optional person detector stopped; using normal cadence."); return false; }
    Lock lock(transport.mutex.value);
    if (!lock) {
        if (lock.result != WAIT_TIMEOUT) state.fail(L"Person detector sharing failed; using normal cadence.");
        return false;
    }
    auto& shared = *transport.shared;
    if (!person::validHeader(shared) || shared.phase != person::Phase::Ready || shared.requested != transport.next ||
        shared.published != transport.next || transport.next == UINT64_MAX) {
        state.fail(L"The optional person detector lost request identity; using normal cadence."); return false;
    }
    transport.expected = source; transport.submitted = now; transport.pending = ++transport.next;
    shared.inputSource = source; shared.inputBytes = source.width * source.height * 3;
    std::memcpy(shared.input.data(), input.bgr.data(), shared.inputBytes);
    shared.requested = transport.pending;
    if (!SetEvent(transport.request.value)) { state.fail(L"Windows could not request person inference; using normal cadence."); return false; }
    return true;
}
}

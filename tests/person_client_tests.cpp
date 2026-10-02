// Owned synthetic subprocesses; no physical capture or user pack installation.
#include "person_client.h"
#include "person_pack.h"
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <stdexcept>
#include <thread>
#include <limits>

namespace fixture {
using namespace lapse;
enum class Fault : LONG { Healthy, InitHang, InvokeHang, Crash, HoldMutex, Abandon, WrongIdentity,
    BadScore, BadVerdict, BadHeader, RegressPhase, InputIdentity, InputLength, Regression, Delayed, Ambiguous };
struct Ack { volatile LONG fault = 0, stage = 0, invocations = 0, release = 1; };
std::atomic<int64_t> clockOffset{0};
std::atomic<bool> publishArmed{false}, publishReached{false}, publishRelease{false}, gateTimeout{false};
std::atomic<bool> verifyArmed{false}, verifyReached{false}, verifyRelease{false}, duplicateFailure{false}, eventFailure{false};
std::atomic<HANDLE> childHandle{nullptr}, requestHandle{nullptr};
std::atomic<unsigned> created{0};
unsigned confirmed = 0;
std::wstring workerPath, ackName;
Ack* ack = nullptr;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void boundedGate(std::atomic<bool>& reached, std::atomic<bool>& released) {
    reached.store(true, std::memory_order_release);
    const uint64_t end = GetTickCount64() + 4000;
    while (!released.load(std::memory_order_acquire)) {
        if (GetTickCount64() >= end) { gateTimeout = true; break; } Sleep(1);
    }
}
void beforePublish() { if (publishArmed.exchange(false)) boundedGate(publishReached, publishRelease); }
ULONGLONG WINAPI tick() { return ULONGLONG(int64_t(GetTickCount64()) + clockOffset.load()); }
BOOL WINAPI duplicate(HANDLE sourceProcess, HANDLE source, HANDLE targetProcess, LPHANDLE target,
    DWORD access, BOOL inherit, DWORD flags) {
    if ((access & PROCESS_TERMINATE) && duplicateFailure.exchange(false)) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return DuplicateHandle(sourceProcess, source, targetProcess, target, access, inherit, flags);
}
BOOL WINAPI createProcess(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES processAttributes,
    LPSECURITY_ATTRIBUTES threadAttributes, BOOL inherit, DWORD flags, LPVOID environment,
    LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    const BOOL result = CreateProcessW(application, command, processAttributes, threadAttributes, inherit,
        flags, environment, directory, startup, process);
    if (result) {
        HANDLE observed = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), process->hProcess, GetCurrentProcess(), &observed, SYNCHRONIZE, FALSE, 0)) return FALSE;
        childHandle.store(observed, std::memory_order_release); ++created;
    }
    return result;
}
HANDLE WINAPI createEvent(LPSECURITY_ATTRIBUTES attributes, BOOL manual, BOOL initial, LPCWSTR name) {
    const HANDLE result = CreateEventW(attributes, manual, initial, name);
    if (name && !manual) requestHandle = result;
    return result;
}
BOOL WINAPI signal(HANDLE event) {
    if (event == requestHandle.load() && eventFailure.exchange(false)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return SetEvent(event);
}
}

namespace lapse {
HANDLE openVerifiedPersonPack(std::wstring& path, std::wstring& error, const std::atomic<bool>* cancelled) {
    error.clear();
    if (fixture::verifyArmed.exchange(false)) fixture::boundedGate(fixture::verifyReached, fixture::verifyRelease);
    if (cancelled && cancelled->load()) { error = L"Cancelled fixture verification."; return nullptr; }
    path = fixture::workerPath;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    return file == INVALID_HANDLE_VALUE ? nullptr : file;
}
}
#define GetTickCount64 fixture::tick
#define DuplicateHandle fixture::duplicate
#define CreateProcessW fixture::createProcess
#define CreateEventW fixture::createEvent
#define SetEvent fixture::signal
#define LAPSE_PERSON_CLIENT_BEFORE_PUBLISH fixture::beforePublish
#include "../src/person_client.cpp"
#undef LAPSE_PERSON_CLIENT_BEFORE_PUBLISH
#undef SetEvent
#undef CreateEventW
#undef CreateProcessW
#undef DuplicateHandle
#undef GetTickCount64

namespace fixture {
struct Handle { HANDLE value = nullptr; ~Handle() { if (value) CloseHandle(value); } };
struct View { void* value = nullptr; ~View() { if (value) UnmapViewOfFile(value); } };
LONG read(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
bool alive(HANDLE stop) { return WaitForSingleObject(stop, 0) == WAIT_TIMEOUT; }
struct AbandonArgs { HANDLE mutex; person::Shared* shared; Ack* state; uint64_t id; };
unsigned __stdcall abandon(void* data) {
    auto& a = *static_cast<AbandonArgs*>(data);
    if (WaitForSingleObject(a.mutex, 1000) != WAIT_OBJECT_0) return 1;
    a.shared->outputSource = a.shared->inputSource; a.shared->output.rawMaxPerson = .05f;
    a.shared->published = a.id;
    InterlockedExchange(&a.state->stage, 4);
    return 0; // Deliberately abandon a plausible partial publication.
}
int syntheticWorker(const wchar_t* name) {
    wchar_t stateName[200]{}; if (!GetEnvironmentVariableW(L"TIMELAPSE_CLIENT_TEST_ACK", stateName, 200)) return 2;
    Handle am, mapping, mutex, stop, request; View av, view;
    am.value = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, stateName);
    if (!am.value) return 3;
    av.value = MapViewOfFile(am.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ack)); if (!av.value) return 3;
    auto& state = *static_cast<Ack*>(av.value); const Fault fault = Fault(read(state.fault));
    mapping.value = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    mutex.value = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (std::wstring(name) + L".mutex").c_str());
    stop.value = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(name) + L".stop").c_str());
    request.value = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(name) + L".request").c_str());
    if (!mapping.value || !mutex.value || !stop.value || !request.value) return 4;
    view.value = MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(person::Shared)); if (!view.value) return 5;
    auto& shared = *static_cast<person::Shared*>(view.value);
    InterlockedExchange(&state.stage, 1);
    if (fault == Fault::InitHang) { Sleep(INFINITE); return 6; }
    if (WaitForSingleObject(mutex.value, 1000) != WAIT_OBJECT_0) return 7;
    shared.phase = person::Phase::Ready; ReleaseMutex(mutex.value);
    HANDLE events[] = {stop.value, request.value}; uint64_t serviced = 0;
    for (;;) {
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
        if (WaitForSingleObject(mutex.value, 1000) != WAIT_OBJECT_0) return 8;
        const auto source = shared.inputSource; const uint64_t id = shared.requested;
        ReleaseMutex(mutex.value);
        if (!id || id == serviced) continue;
        InterlockedIncrement(&state.invocations); InterlockedExchange(&state.stage, 2);
        if (fault == Fault::InvokeHang) { Sleep(INFINITE); return 9; }
        if (fault == Fault::Crash) { TerminateProcess(GetCurrentProcess(), 42); return 9; }
        if (fault == Fault::HoldMutex) {
            if (WaitForSingleObject(mutex.value, 1000) != WAIT_OBJECT_0) return 10;
            InterlockedExchange(&state.stage, 3); Sleep(INFINITE); return 10;
        }
        if (fault == Fault::Abandon) {
            AbandonArgs args{mutex.value, &shared, &state, id};
            Handle thread; thread.value = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, abandon, &args, 0, nullptr));
            if (!thread.value) return 11; WaitForSingleObject(thread.value, INFINITE);
            WaitForSingleObject(stop.value, INFINITE); return 0;
        }
        while (!read(state.release) && alive(stop.value)) Sleep(1);
        if (!alive(stop.value)) return 0;
        if (WaitForSingleObject(mutex.value, 1000) != WAIT_OBJECT_0) return 12;
        shared.outputSource = source;
        shared.output = {.05f, .04f, person::Verdict::QualifiedAbsent, person::Reason::None};
        if (fault == Fault::Ambiguous) shared.output = {.2294526f, .2294526f, person::Verdict::Unknown, person::Reason::Ambiguous};
        if (fault == Fault::WrongIdentity) ++shared.outputSource.sequence;
        if (fault == Fault::BadScore) shared.output.rawMaxPerson = std::numeric_limits<float>::quiet_NaN();
        if (fault == Fault::BadVerdict) shared.output.rawMaxPerson = .20f;
        if (fault == Fault::BadHeader) ++shared.structBytes;
        if (fault == Fault::RegressPhase) shared.phase = person::Phase::Starting;
        if (fault == Fault::InputIdentity) ++shared.inputSource.cameraEpoch;
        if (fault == Fault::InputLength) --shared.inputBytes;
        shared.published = id;
        if (fault == Fault::Regression && id >= 2) shared.published = 0;
        serviced = id; ReleaseMutex(mutex.value); InterlockedExchange(&state.stage, 5);
    }
}
void waitFlag(const std::atomic<bool>& flag, const char* text) {
    const uint64_t end = GetTickCount64() + 3500;
    while (!flag.load(std::memory_order_acquire) && GetTickCount64() < end) Sleep(1);
    require(flag.load(), text);
}
void waitStage(LONG value) {
    const uint64_t end = GetTickCount64() + 3500;
    while (read(ack->stage) < value && GetTickCount64() < end) Sleep(1);
    require(read(ack->stage) >= value, "child fault stage reached");
}
void terminal() {
    const HANDLE child = childHandle.exchange(nullptr);
    if (child) { const DWORD result = WaitForSingleObject(child, 3500); CloseHandle(child); require(result == WAIT_OBJECT_0, "owned child terminal"); ++confirmed; }
    const uint64_t end = GetTickCount64() + 3500;
    while (lapse::startupBusy.load(std::memory_order_acquire) && GetTickCount64() < end) Sleep(1);
    require(!lapse::startupBusy.load(), "global gate eventually released");
    requestHandle = nullptr; clockOffset = 0;
}
void reset(Fault fault = Fault::Healthy) {
    require(!lapse::startupBusy.load(), "previous process fully retired before reset");
    *ack = {}; ack->fault = LONG(fault); ack->release = 1;
    publishArmed = publishReached = publishRelease = verifyArmed = verifyReached = verifyRelease = false;
    duplicateFailure = eventFailure = gateTimeout = false; clockOffset = 0;
}
PersonPoll pump(PersonClient& client, PersonPoll wanted) {
    PersonCheckResult result; PersonPoll status = PersonPoll::Pending;
    const uint64_t end = GetTickCount64() + 4000;
    while (GetTickCount64() < end) {
        status = client.poll(result); if (status == wanted || status == PersonPoll::Unavailable) return status; Sleep(1);
    }
    return status;
}
std::unique_ptr<CameraPersonInput> input(uint64_t sequence = 1) {
    auto frame = std::make_unique<CameraPersonInput>(); frame->source = {91, 1, sequence, tick(), 320, 320, 320, 320}; return frame;
}
void start(PersonClient& client) { std::wstring error; require(client.start(91, error), "client start accepted"); }
void ready(PersonClient& client) { start(client); require(pump(client, PersonPoll::Ready) == PersonPoll::Ready, "client ready"); }
struct Release { ~Release() { publishRelease = verifyRelease = true; if (ack) InterlockedExchange(&ack->release, 1); } };
void cadencePolicy() {
    using person::Output; using person::Reason; using person::Verdict;
    auto verdict = [](Output output, bool detail = true) { return personCadenceVerdict(output, detail); };
    require(verdict({.2294526f, .2294526f, Verdict::Unknown, Reason::Ambiguous}) == Verdict::QualifiedAbsent,
        "ordinary empty-office ambiguous result can qualify cadence absence");
    require(verdict({std::nextafter(.25f, 0.f), 0, Verdict::Unknown, Reason::Ambiguous}) == Verdict::QualifiedAbsent,
        "raw score just below person detection threshold qualifies");
    require(verdict({.25f, .20f, Verdict::Unknown, Reason::Ambiguous}) == Verdict::Unknown &&
        verdict({.8f, 0, Verdict::Unknown, Reason::Ambiguous}) == Verdict::Unknown,
        "high raw score without valid detection remains uncertain");
    require(verdict({.25f, .25f, Verdict::Present, Reason::None}, false) == Verdict::Present,
        "exact detection boundary and low detail preserve presence");
    require(verdict({.10f, .04f, Verdict::QualifiedAbsent, Reason::None}) == Verdict::QualifiedAbsent &&
        verdict({.10001f, .04f, Verdict::QualifiedAbsent, Reason::None}) == Verdict::Unknown,
        "worker wire absence threshold remains strict");
    require(verdict({.2f, .1f, Verdict::Unknown, Reason::Ambiguous}, false) == Verdict::Unknown,
        "uninformative ambiguous image cannot qualify absence");
    for (const Reason reason : {Reason::InvalidInput, Reason::ModelFailure, Reason::InvalidOutput,
        Reason::StaleSource, Reason::InsufficientDetail})
        require(verdict({.2f, .1f, Verdict::Unknown, reason}) == Verdict::Unknown, "unhealthy or stale reply remains unknown");
    require(verdict({std::numeric_limits<float>::quiet_NaN(), 0, Verdict::Unknown, Reason::Ambiguous}) == Verdict::Unknown &&
        verdict({.2f, .3f, Verdict::Unknown, Reason::Ambiguous}) == Verdict::Unknown,
        "invalid scores cannot qualify cadence absence");
    person::Input pixels{};
    for (const int level : {0, 12, 128, 243, 255}) {
        pixels.fill(uint8_t(level)); require(!lapse::sufficientPersonDetail(pixels.data(), pixels.size()), "flat frames fail host detail gate");
    }
    for (const auto& boundary : {std::array<int, 3>{4, 20, 1}, {3, 19, 0}, {235, 251, 1}, {236, 252, 0}, {120, 136, 1}, {120, 135, 0}}) {
        for (size_t at = 0; at < pixels.size(); ++at) pixels[at] = uint8_t((at / 3) % 2 ? boundary[0] : boundary[1]);
        require(lapse::sufficientPersonDetail(pixels.data(), pixels.size()) == bool(boundary[2]), "host mean/spread boundaries match worker");
    }
    require(!lapse::sufficientPersonDetail(nullptr, pixels.size()) &&
        !lapse::sufficientPersonDetail(pixels.data(), pixels.size() - 1), "invalid host detail input rejected");
}
void scenarios(const wchar_t* self, const wchar_t* real) {
    cadencePolicy();
    workerPath = self;
    DWORD baselineHandles = 0;
    {
        reset(); PersonClient client; ready(client); auto frame = input();
        require(client.submit(*frame) && !client.submit(*frame), "one request, no queue");
        PersonCheckResult result; const uint64_t end = GetTickCount64() + 2000;
        PersonPoll status; do { status = client.poll(result); if (status != PersonPoll::Pending) break; Sleep(1); } while (GetTickCount64() < end);
        require(status == PersonPoll::Complete && result.output.verdict == person::Verdict::QualifiedAbsent, "synthetic result accepted once");
        require(client.poll(result) == PersonPoll::Ready && !client.submit(*frame) && read(ack->invocations) == 1, "completion and source not replayed");
        frame = input(2); frame->source.receivedTick = tick() + 1; require(!client.submit(*frame), "future input rejected");
        frame->source.receivedTick = tick() - 4000; require(!client.submit(*frame), "stale input rejected");
        client.cancel(); terminal();
    }
    {
        reset(Fault::Ambiguous); PersonClient client; ready(client);
        auto frame = input();
        for (size_t at = 0; at < frame->bgr.size(); ++at) frame->bgr[at] = uint8_t((at / 3) % 2 ? 64 : 192);
        require(client.submit(*frame), "informative ambiguous input submitted");
        frame->bgr.fill(0); // The completion must retain the submitted image's detail.
        PersonCheckResult result; PersonPoll status = PersonPoll::Pending;
        const uint64_t end = GetTickCount64() + 2000;
        while (status == PersonPoll::Pending && GetTickCount64() < end) { status = client.poll(result); Sleep(1); }
        require(status == PersonPoll::Complete && result.output.verdict == person::Verdict::Unknown &&
            result.sufficientDetail && personCadenceVerdict(result.output, result.sufficientDetail) == person::Verdict::QualifiedAbsent,
            "validated ambiguous wire reply qualifies only with matching submitted detail");
        frame = input(2); require(client.submit(*frame), "flat ambiguous input submitted");
        for (size_t at = 0; at < frame->bgr.size(); ++at) frame->bgr[at] = uint8_t((at / 3) % 2 ? 64 : 192);
        status = PersonPoll::Pending; const uint64_t flatEnd = GetTickCount64() + 2000;
        while (status == PersonPoll::Pending && GetTickCount64() < flatEnd) { status = client.poll(result); Sleep(1); }
        require(status == PersonPoll::Complete && !result.sufficientDetail &&
            personCadenceVerdict(result.output, result.sufficientDetail) == person::Verdict::Unknown,
            "matching flat thumbnail cannot be replaced by later informative pixels");
        client.cancel(); terminal();
    }
    Sleep(10); // The already-terminal observer releases its local startup objects.
    require(GetProcessHandleCount(GetCurrentProcess(), &baselineHandles) != FALSE, "warmed handle inventory");
    {
        reset(); Release release; verifyArmed = true; PersonClient client; start(client); waitFlag(verifyReached, "verification entered");
        client.cancel(); PersonClient next; std::wstring error;
        require(!next.start(92, error), "cancelled verification still owns single startup gate");
        verifyRelease = true; terminal(); require(!gateTimeout, "verification gate bounded");
    }
    {
        reset(); Release release; publishArmed = true;
        auto client = std::make_unique<PersonClient>(); start(*client); waitFlag(publishReached, "publication boundary entered");
        require(childHandle.load() != nullptr, "child created before cancellation boundary");
        client.reset(); PersonClient next; std::wstring error;
        require(!next.start(92, error), "destroyed client cannot overlap unpublished live child");
        publishRelease = true; terminal(); require(!gateTimeout, "publication gate bounded");
    }
    {
        reset(); duplicateFailure = true; PersonClient client; start(client);
        require(pump(client, PersonPoll::Unavailable) == PersonPoll::Unavailable, "observer duplication failure closed");
        client.cancel(); terminal();
    }
    for (const Fault fault : {Fault::InitHang, Fault::InvokeHang, Fault::Crash, Fault::HoldMutex, Fault::Abandon,
        Fault::WrongIdentity, Fault::BadScore, Fault::BadVerdict, Fault::BadHeader, Fault::RegressPhase,
        Fault::InputIdentity, Fault::InputLength}) {
        reset(fault); PersonClient client; start(client);
        if (fault == Fault::InitHang) { waitStage(1); clockOffset = 8001; }
        else {
            require(pump(client, PersonPoll::Ready) == PersonPoll::Ready, "fault ready"); require(client.submit(*input()), "fault submission");
            waitStage(fault == Fault::HoldMutex ? 3 : fault == Fault::Abandon ? 4 : 2);
            if (fault == Fault::InvokeHang || fault == Fault::HoldMutex) clockOffset = 3001;
        }
        require(pump(client, PersonPoll::Unavailable) == PersonPoll::Unavailable, "fault cannot yield trusted result");
        client.cancel(); terminal();
    }
    {
        reset(Fault::Regression); PersonClient client; ready(client); require(client.submit(*input()), "regression first request");
        require(pump(client, PersonPoll::Complete) == PersonPoll::Complete, "regression first completion");
        require(client.submit(*input(2)), "regression second request");
        require(pump(client, PersonPoll::Unavailable) == PersonPoll::Unavailable, "publication counter regression rejected");
        client.cancel(); terminal();
    }
    {
        reset(Fault::Delayed); Release release; ack->release = 0; PersonClient client; ready(client);
        auto frame = input(); clockOffset = 2500; require(client.submit(*frame), "source initially fresh"); waitStage(2);
        clockOffset = 3101; InterlockedExchange(&ack->release, 1);
        PersonCheckResult result; PersonPoll status = PersonPoll::Pending; const uint64_t end = GetTickCount64() + 2000;
        while (status == PersonPoll::Pending && GetTickCount64() < end) { status = client.poll(result); Sleep(1); }
        require(status == PersonPoll::Complete && result.output.verdict == person::Verdict::Unknown && result.output.reason == person::Reason::StaleSource,
            "inflight source expiry stays unknown without resetting request deadline");
        client.cancel(); terminal();
    }
    {
        reset(); PersonClient client; ready(client); eventFailure = true;
        require(!client.submit(*input()), "failed request signal rejected"); PersonCheckResult result;
        require(client.poll(result) == PersonPoll::Unavailable, "request failure remains unavailable"); client.cancel(); terminal();
    }
    if (real) {
        reset(); workerPath = real; PersonClient client; ready(client); require(client.submit(*input()), "real embedded worker input");
        PersonCheckResult result; PersonPoll status = PersonPoll::Pending; const uint64_t end = GetTickCount64() + 2500;
        while (status == PersonPoll::Pending && GetTickCount64() < end) { status = client.poll(result); Sleep(1); }
        require(status == PersonPoll::Complete && result.output.verdict == person::Verdict::Unknown && result.output.reason == person::Reason::InsufficientDetail,
            "real candidate blank input remains unknown"); client.cancel(); terminal(); workerPath = self;
    }
    require(confirmed == created.load(), "all created children reached terminal state");
    DWORD finalHandles = 0;
    const uint64_t cleanupDeadline = GetTickCount64() + 1500;
    do {
        require(GetProcessHandleCount(GetCurrentProcess(), &finalHandles) != FALSE, "final handle inventory");
        if (finalHandles == baselineHandles) break;
        Sleep(1);
    } while (GetTickCount64() < cleanupDeadline);
    std::printf("handles: warmed=%lu final=%lu; children: created=%u terminal=%u\n", baselineHandles, finalHandles, created.load(), confirmed);
    require(finalHandles == baselineHandles, "event, pack, process, job and mapping handles released");
}
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 3 && !std::wcscmp(argv[1], L"--person-host")) return fixture::syntheticWorker(argv[2]);
    if (argc < 1 || argc > 2) return 1;
    try {
        wchar_t self[32768]{}; fixture::require(GetModuleFileNameW(nullptr, self, 32768) != 0, "self path");
        fixture::ackName = L"Local\\Timelapse.ClientFixture." + std::to_wstring(GetCurrentProcessId());
        fixture::Handle mapping; fixture::View view;
        mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(fixture::Ack), fixture::ackName.c_str());
        fixture::require(mapping.value != nullptr, "fixture state mapping");
        view.value = MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fixture::Ack));
        fixture::require(view.value != nullptr, "fixture state view"); fixture::ack = static_cast<fixture::Ack*>(view.value);
        fixture::require(SetEnvironmentVariableW(L"TIMELAPSE_CLIENT_TEST_ACK", fixture::ackName.c_str()) != FALSE, "fixture child state");
        fixture::scenarios(self, argc == 2 ? argv[1] : nullptr);
        fixture::require(!fixture::gateTimeout, "bounded fixture gates completed");
        std::printf("PASS: startup cancellation publication boundary, cancelled verification, partial launch failure, owned terminal gate, inference/process/mutex/protocol faults, identity/freshness/replay, signal failure, cleanup; real worker=%s (%u children)\n", argc == 2 ? "included" : "optional/not requested", fixture::created.load());
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s (%lu)\n", error.what(), GetLastError()); return 10; }
}


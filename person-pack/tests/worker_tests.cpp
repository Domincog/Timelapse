#include "../../src/person_protocol.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
using namespace lapse::person;
namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Client {
    Handle mapping, mutex, stop, request, job, process;
    Shared* shared = nullptr;
    ~Client() { if (stop.value) SetEvent(stop.value); if (job.value) TerminateJobObject(job.value, 0); if (process.value) WaitForSingleObject(process.value, 3000); if (shared) UnmapViewOfFile(shared); }
    Client(const wchar_t* executable) {
        wchar_t suffix[64]{};
        swprintf_s(suffix, L"%08lX-%016llX", GetCurrentProcessId(), GetTickCount64());
        const std::wstring name = std::wstring(L"Local\\Timelapse.Person.") + suffix;
        mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
        require(mapping.value && GetLastError() != ERROR_ALREADY_EXISTS, "unique mapping");
        mutex.value = CreateMutexW(nullptr, FALSE, (name + L".mutex").c_str());
        stop.value = CreateEventW(nullptr, TRUE, FALSE, (name + L".stop").c_str());
        request.value = CreateEventW(nullptr, FALSE, FALSE, (name + L".request").c_str());
        require(mutex.value && stop.value && request.value, "transport objects");
        shared = static_cast<Shared*>(MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
        require(shared != nullptr, "map view");
        new (shared) Shared{}; shared->structBytes = sizeof(Shared);
        job.value = CreateJobObjectW(nullptr, nullptr); require(job.value != nullptr, "job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "kill-on-close");
        std::wstring command = std::wstring(L"\"") + executable + L"\" --person-host " + name;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
        require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr, nullptr, &startup, &child) != FALSE, "launch isolated worker");
        process.value = child.hProcess; Handle thread; thread.value = child.hThread;
        if (!AssignProcessToJobObject(job.value, process.value)) { TerminateProcess(process.value, 1); require(false, "job assignment"); }
        require(ResumeThread(thread.value) != DWORD(-1), "resume");
    }
    void lock() { require(WaitForSingleObject(mutex.value, 500) == WAIT_OBJECT_0, "parent mutex"); }
    void unlock() { require(ReleaseMutex(mutex.value) != FALSE, "release mutex"); }
    void ready() {
        const uint64_t deadline = GetTickCount64() + 5000;
        while (GetTickCount64() < deadline) {
            require(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT, "worker alive during startup");
            lock(); const Phase phase = shared->phase; unlock();
            if (phase == Phase::Ready) return;
            require(phase == Phase::Starting, "worker initialized"); Sleep(5);
        }
        require(false, "startup deadline");
    }
    void submit(uint64_t sequence, bool stale = false, bool contention = false) {
        lock();
        shared->input.fill(0); shared->inputBytes = uint32_t(MaxBgrBytes);
        shared->inputSource = {1, 2, sequence, stale ? GetTickCount64() - 4000 : GetTickCount64(), 320, 320, 320, 320};
        ++shared->requested;
        require(SetEvent(request.value) != FALSE, "request event");
        if (contention) Sleep(140); // Child must retain the consumed signal across a 50 ms timeout.
        unlock();
    }
    Output completed(uint64_t id) {
        const uint64_t deadline = GetTickCount64() + 2500;
        while (GetTickCount64() < deadline) {
            require(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT, "worker alive during request");
            lock(); const bool complete = shared->published == id;
            const Output output = shared->output;
            if (complete) require(sameSource(shared->inputSource, shared->outputSource), "exact publication source");
            unlock(); if (complete) return output; Sleep(5);
        }
        require(false, "request deadline"); return {};
    }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    try {
        Client client(argv[1]); client.ready();
        client.submit(1, false, true);
        auto output = client.completed(1);
        require(output.verdict == Verdict::Unknown && output.reason == Reason::InsufficientDetail, "black source unqualified after request contention");
        client.submit(2); output = client.completed(2);
        require(output.verdict == Verdict::Unknown && output.reason == Reason::InsufficientDetail, "second distinct request");
        require(SetEvent(client.request.value) != FALSE, "duplicate event"); Sleep(100);
        client.lock(); require(client.shared->published == 2 && client.shared->requested == 2, "duplicate event cannot advance publication"); client.unlock();
        client.submit(2); output = client.completed(3);
        require(output.verdict == Verdict::Unknown && output.reason == Reason::InvalidInput, "replayed source rejected");
        client.submit(3, true); output = client.completed(4);
        require(output.verdict == Verdict::Unknown && output.reason == Reason::StaleSource, "stale source remains unknown");
        client.lock(); ++client.shared->modelRevision; client.unlock();
        require(SetEvent(client.request.value) != FALSE && WaitForSingleObject(client.process.value, 1500) == WAIT_OBJECT_0,
            "wrong model identity terminates worker");
        std::puts("PASS: standalone embedded worker, named protocol, real inference, retained request contention, duplicate/replayed/stale sources, immutable model identity and owned child cleanup");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s (%lu)\n", error.what(), GetLastError()); return 1; }
}

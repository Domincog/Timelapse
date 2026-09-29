#pragma once
#include <windows.h>
namespace probe {
enum class Stage { None, Snapshot, Session, Camera, Encoder, Close, Count };
struct Scope {
    Stage previous;
    bool priorQueued;
    explicit Scope(Stage stage, bool queued = false) noexcept;
    ~Scope();
};
HRESULT WINAPI initialize(LPVOID, DWORD);
EXECUTION_STATE WINAPI executionState(EXECUTION_STATE);
BOOL WINAPI forbiddenProcess(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
}

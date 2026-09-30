#pragma once
#include "core.h"
#include <atomic>

namespace lapse {
enum class PersonPackState { Missing, Ready, Invalid };
struct PersonPackInfo {
    PersonPackState state = PersonPackState::Missing;
    uint64_t bytes = 0;
    std::wstring path, detail;
};
PersonPackInfo inspectPersonPack();
void showPersonPackDialog(HWND owner);
// Returns a read handle that denies modification/replacement until closed.
// Call on a startup/background thread; caller owns CloseHandle on success.
// Never downloads or creates a directory. Cancellation is checked while hashing.
HANDLE openVerifiedPersonPack(std::wstring& path, std::wstring& error, const std::atomic<bool>* cancelled = nullptr);
// Installation helpers share the same fixed location and authentication policy.
std::wstring personPackPath();
bool verifyPersonPackFile(HANDLE file, const std::atomic<bool>* cancelled = nullptr);
}

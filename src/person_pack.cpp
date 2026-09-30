#include "person_pack.h"
#include "person_pack_metadata.h"
#include <shlobj.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <cwchar>

namespace lapse {
namespace {
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE value = nullptr;
    ~Hash() { if (value) BCryptDestroyHash(value); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};
bool hashMatches(HANDLE file, const std::atomic<bool>* cancelled) {
    if (std::size(PersonPackExpectedSha256) != 65) return false;
    LARGE_INTEGER begin{};
    if (!SetFilePointerEx(file, begin, nullptr, FILE_BEGIN)) return false;
    Hash hash;
    if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(hash.algorithm, &hash.value, nullptr, 0, nullptr, 0, 0) < 0) return false;
    std::array<unsigned char, 65536> buffer{};
    uint64_t total = 0;
    for (;;) {
        if (cancelled && cancelled->load()) return false;
        DWORD bytes = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) return false;
        if (!bytes) break;
        total += bytes;
        if (total > PersonPackExpectedBytes || BCryptHashData(hash.value, buffer.data(), bytes, 0) < 0) return false;
    }
    if (total != PersonPackExpectedBytes) return false;
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i) {
        if (PersonPackExpectedSha256[2 * i] != hex[digest[i] >> 4] ||
            PersonPackExpectedSha256[2 * i + 1] != hex[digest[i] & 15]) return false;
    }
    return true;
}
}
std::wstring personPackPath() {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &folder))) return {};
    std::wstring result;
    try { result = std::wstring(folder) + L"\\Timelapse\\Person\\NanoDet-r1\\" + PersonPackFilename; }
    catch (...) { CoTaskMemFree(folder); throw; }
    CoTaskMemFree(folder); return result;
}
// Shared with the explicit-download dialog; it verifies an owned temporary file
// by the same compiled size/hash before making it the installed revision.
bool verifyPersonPackFile(HANDLE file, const std::atomic<bool>* cancelled) {
    LARGE_INTEGER length{};
    return PersonPackExpectedBytes && GetFileSizeEx(file, &length) && length.QuadPart >= 0 &&
        uint64_t(length.QuadPart) == PersonPackExpectedBytes && hashMatches(file, cancelled);
}
HANDLE openVerifiedPersonPack(std::wstring& path, std::wstring& error, const std::atomic<bool>* cancelled) {
    path = personPackPath(); error.clear();
    if (path.empty()) { error = L"The local person-model folder is unavailable; using normal cadence."; return nullptr; }
    if (!PersonPackExpectedBytes) { error = L"This development build has no approved person detector; using normal cadence."; return nullptr; }
    File file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) {
        error = L"The optional person detector is missing or unavailable. Open Advanced, Time compression, Manage to install it.";
        return nullptr;
    }
    if (!verifyPersonPackFile(file.value, cancelled)) {
        error = cancelled && cancelled->load() ? L"Person detector startup was cancelled." :
            L"The optional person detector did not pass verification. Reinstall it using Manage; recording uses normal cadence.";
        return nullptr;
    }
    // Resolve the opened image before launch rather than trusting a relative path.
    std::wstring finalPath(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file.value, finalPath.data(), static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED);
    if (!length || length >= finalPath.size()) { error = L"The person detector path could not be verified."; return nullptr; }
    finalPath.resize(length); path = std::move(finalPath);
    const HANDLE result = file.value; file.value = INVALID_HANDLE_VALUE; return result;
}
PersonPackInfo inspectPersonPack() {
    PersonPackInfo info;
    try {
        info.path = personPackPath();
        if (info.path.empty()) { info.state = PersonPackState::Invalid; info.detail = L"Local model storage is unavailable."; return info; }
        const DWORD attributes = GetFileAttributesW(info.path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
            info.detail = L"Not installed. Optional download; checks run locally."; return info;
        }
        std::wstring openedPath;
        File file; file.value = openVerifiedPersonPack(openedPath, info.detail);
        if (!file.value) { file.value = INVALID_HANDLE_VALUE; info.state = PersonPackState::Invalid; return info; }
        info.state = PersonPackState::Ready; info.bytes = PersonPackExpectedBytes;
        info.detail = L"Ready. NanoDet-m, local CPU detector, revision 1.";
    } catch (...) { info.state = PersonPackState::Invalid; info.detail = L"The optional detector could not be inspected."; }
    return info;
}
}

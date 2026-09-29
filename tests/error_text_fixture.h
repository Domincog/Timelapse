// Included only by core_tests. Ordinary calls use the real Windows formatter;
// the one-shot allocation fault is armed only after an injected API succeeds.
#pragma once
#include <cstdlib>
#include <new>

namespace {
bool coreTestFailNextAllocation = false;
struct ErrorTextFixture {
    bool enabled = false, failLookup = false, failCopy = false;
    bool argumentsValid = true;
    int allocations = 0, frees = 0, mismatchedFrees = 0;
    DWORD code = 0;
    std::wstring message;
    HLOCAL allocation = nullptr;
} coreTestError;

DWORD WINAPI coreTestFormatMessageW(DWORD flags, LPCVOID source, DWORD code, DWORD language,
                                    LPWSTR output, DWORD size, va_list* arguments) {
    if (!coreTestError.enabled) return ::FormatMessageW(flags, source, code, language, output, size, arguments);
    coreTestError.argumentsValid &= flags == (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS) && !source && language == 0 && output && size == 0 && !arguments;
    coreTestError.code = code;
    auto message = reinterpret_cast<LPWSTR*>(output);
    *message = nullptr;
    if (coreTestError.failLookup) { SetLastError(ERROR_MR_MID_NOT_FOUND); return 0; }
    *message = static_cast<LPWSTR>(LocalAlloc(LMEM_FIXED, (coreTestError.message.size() + 1) * sizeof(wchar_t)));
    if (!*message) return 0;
    std::copy(coreTestError.message.c_str(), coreTestError.message.c_str() + coreTestError.message.size() + 1, *message);
    coreTestError.allocation = *message;
    ++coreTestError.allocations;
    coreTestFailNextAllocation = coreTestError.failCopy;
    return static_cast<DWORD>(coreTestError.message.size());
}
HLOCAL WINAPI coreTestLocalFree(HLOCAL memory) {
    if (coreTestError.enabled) {
        if (memory != coreTestError.allocation) ++coreTestError.mismatchedFrees;
        else coreTestError.allocation = nullptr;
        ++coreTestError.frees;
    }
    return ::LocalFree(memory);
}
}

void* operator new(size_t size) {
    if (coreTestFailNextAllocation) { coreTestFailNextAllocation = false; throw std::bad_alloc(); }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

#define FormatMessageW coreTestFormatMessageW
#define LocalFree coreTestLocalFree
#include "../src/core.cpp"
#undef LocalFree
#undef FormatMessageW

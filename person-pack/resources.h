#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace lapse::person {
inline constexpr int ParamResource = 101, WeightsResource = 102;
inline constexpr int NanoLicenseResource = 201, NcnnLicenseResource = 202, NoticeResource = 203;
struct Resource { const uint8_t* data = nullptr; size_t bytes = 0; };
inline Resource resource(int id) noexcept {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&resource), &module)) return {};
    const HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!info) return {};
    const HGLOBAL loaded = LoadResource(module, info);
    if (!loaded) return {};
    return {static_cast<const uint8_t*>(LockResource(loaded)), SizeofResource(module, info)};
}
}

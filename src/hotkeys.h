#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>

namespace lapse {
// Windows HOTKEY control representation: virtual key in the low byte and
// Shift=1, Control=2, Alt=4, Extended=8 in the high byte. Extended is a
// display hint; RegisterHotKey identifies a chord by key and modifiers only.
// Zero disables the shortcut.
inline uint16_t hotkeyIdentity(uint16_t value) noexcept { return value & uint16_t(0xf7ff); }
inline bool validHotkey(uint16_t value) noexcept {
    if (!value) return true;
    const unsigned key = value & 0xff, modifiers = value >> 8;
    if ((modifiers & ~15u) || !(modifiers & 6u)) return false;
    // Preserve the native Alt+letter access keys used throughout the app.
    if (key >= 'A' && key <= 'Z' && (modifiers & 4u) && !(modifiers & 2u)) return false;
    const bool supported = (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
        (key >= VK_F1 && key <= VK_F24 && key != VK_F12) ||
        (key >= VK_NUMPAD0 && key <= VK_DIVIDE) ||
        key == VK_SPACE || key == VK_RETURN || key == VK_TAB || key == VK_BACK ||
        key == VK_ESCAPE || key == VK_INSERT || key == VK_DELETE ||
        (key >= VK_PRIOR && key <= VK_DOWN);
    return supported && !(key == VK_DELETE && (modifiers & 6u) == 6u);
}
inline UINT hotkeyRegistrationModifiers(uint16_t value) noexcept {
    const unsigned modifiers = value >> 8;
    return MOD_NOREPEAT | ((modifiers & 1u) ? MOD_SHIFT : 0u) |
        ((modifiers & 2u) ? MOD_CONTROL : 0u) | ((modifiers & 4u) ? MOD_ALT : 0u);
}
inline bool validateHotkeys(uint16_t pause, uint16_t stop, std::wstring& error) {
    error.clear();
    if (!validHotkey(pause) || !validHotkey(stop)) {
        error = L"Use Ctrl or Alt with a letter, number, function or navigation key. Add Ctrl to Alt+letter combinations to keep interface access keys available. F12 and Ctrl+Alt+Delete are reserved. Clear the field to disable a shortcut.";
        return false;
    }
    if (pause && hotkeyIdentity(pause) == hotkeyIdentity(stop)) {
        error = L"Choose different shortcuts for Pause / resume and Stop and save.";
        return false;
    }
    return true;
}
// The third global action opens the status window from anywhere.
inline bool validateHotkeys(uint16_t pause, uint16_t stop, uint16_t status, std::wstring& error) {
    if (!validateHotkeys(pause, stop, error)) return false;
    if (!validHotkey(status)) {
        error = L"Use Ctrl or Alt with a letter, number, function or navigation key. Add Ctrl to Alt+letter combinations to keep interface access keys available. F12 and Ctrl+Alt+Delete are reserved. Clear the field to disable a shortcut.";
        return false;
    }
    if (status && ((pause && hotkeyIdentity(status) == hotkeyIdentity(pause)) || (stop && hotkeyIdentity(status) == hotkeyIdentity(stop)))) {
        error = L"Choose a Set status shortcut that differs from Pause / resume and Stop and save.";
        return false;
    }
    return true;
}
}

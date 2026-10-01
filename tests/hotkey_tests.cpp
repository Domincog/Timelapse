#include "hotkeys.h"
#include <iostream>

int main() {
    using namespace lapse;
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    auto shortcut = [](unsigned key, unsigned modifiers) { return uint16_t(key | (modifiers << 8)); };
    std::wstring error;
    check(validateHotkeys(0, 0, error) && error.empty(), "both shortcuts default to disabled");
    for (unsigned modifiers : {2u, 3u, 4u, 5u, 6u, 7u}) {
        for (unsigned key : {unsigned('P'), unsigned('0'), unsigned(VK_F1), unsigned(VK_F24),
                             unsigned(VK_SPACE), unsigned(VK_LEFT), unsigned(VK_NUMPAD0)})
            check(validHotkey(shortcut(key, modifiers)) == !(key == unsigned('P') && !(modifiers & 2u)), "modified supported keys accepted while Alt access keys remain available");
    }
    for (unsigned modifiers : {0u, 1u, 8u, 16u, 18u, 255u})
        check(!validHotkey(shortcut('P', modifiers)), "bare keys, shift-only and unsupported flags rejected");
    for (unsigned key : {0u, unsigned(VK_CONTROL), unsigned(VK_SHIFT), unsigned(VK_MENU),
                         unsigned(VK_LWIN), unsigned(VK_RWIN), unsigned(VK_F12), 255u})
        check(!validHotkey(shortcut(key, 2)), "modifier-only and reserved keys rejected");
    check(!validHotkey(shortcut(VK_DELETE, 6)) && !validHotkey(shortcut(VK_DELETE, 7)), "secure attention shortcut rejected");
    check(validHotkey(shortcut(VK_LEFT, 10)) && validHotkey(shortcut(VK_DELETE, 12)), "native extended navigation keys accepted");
    check(hotkeyIdentity(shortcut(VK_LEFT, 10)) == shortcut(VK_LEFT, 2), "extended display bit is absent from registration identity");
    check(hotkeyRegistrationModifiers(shortcut('P', 7)) == (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT),
          "control representation maps to Win32 registration with no repeat");
    check(!validateHotkeys(shortcut('P', 6), shortcut('P', 6), error) && !error.empty(), "duplicate actions rejected");
    check(!validateHotkeys(shortcut(VK_LEFT, 2), shortcut(VK_LEFT, 10), error) && !error.empty(), "extended hint cannot disguise duplicate chord");
    check(validateHotkeys(shortcut('P', 6), shortcut('S', 6), error) && error.empty(), "distinct actions accepted");
    return failures ? 1 : 0;
}

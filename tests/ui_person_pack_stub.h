#pragma once
// Include before main.cpp in owned UI fixtures. No disk/hash/network, installer,
// model load, child process or real manager dialog can run through these seams.
#include "../src/person_pack.h"
namespace lapse {
inline PersonPackInfo uiPersonPackInfo{};
inline unsigned uiPersonPackInspections = 0, uiPersonPackDialogs = 0;
inline HWND uiPersonPackOwner = nullptr;
inline bool uiPersonPackInstallOnDialog = false;
inline PersonPackInfo uiInspectPersonPack() { ++uiPersonPackInspections; return uiPersonPackInfo; }
inline void uiShowPersonPackDialog(HWND owner) {
    ++uiPersonPackDialogs; uiPersonPackOwner = owner;
    if (uiPersonPackInstallOnDialog) uiPersonPackInfo.state = PersonPackState::Ready;
}
}
#define inspectPersonPack uiInspectPersonPack
#define showPersonPackDialog uiShowPersonPackDialog

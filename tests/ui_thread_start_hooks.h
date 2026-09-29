#pragma once
#include <process.h>
namespace probe {
uintptr_t __cdecl launch(void*, unsigned, _beginthreadex_proc_type, void*, unsigned, unsigned*);
}
// Use the installed MSVC <thread> implementation unchanged except its launch
// call target. Force-include in every fixture TU so the class definition agrees.
// process.h is included first, keeping the actual CRT declaration untouched.
#define _beginthreadex probe::launch
#include <thread>
#undef _beginthreadex

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
namespace probe {
extern unsigned launches, comCalls, mediaCalls, captureCalls, processCalls, powerCalls;
extern bool callbackPresent, argumentPresent;
HRESULT WINAPI initialize(LPVOID, DWORD);
EXECUTION_STATE WINAPI power(EXECUTION_STATE);
HRESULT WINAPI startup(ULONG, DWORD = MFSTARTUP_FULL);
BOOL WINAPI process(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
HDC WINAPI desktop(HWND);
HRESULT WINAPI enumerate(IMFAttributes*, IMFActivate***, UINT32*);
HRESULT WINAPI device(IMFAttributes*, IMFMediaSource**);
HRESULT WINAPI writer(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**);
}

// The standalone target applies this header to every TU. Production app/core
// targets never receive it; all worker-side effects are fatal test sentinels.
#define CoInitializeEx probe::initialize
#define SetThreadExecutionState probe::power
#define MFStartup probe::startup
#define CreateProcessW probe::process
#define GetDC probe::desktop
#define MFEnumDeviceSources probe::enumerate
#define MFCreateDeviceSource probe::device
#define MFCreateSinkWriterFromURL probe::writer

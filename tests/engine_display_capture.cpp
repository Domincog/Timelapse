#include <windows.h>
int WINAPI reviewGetSystemMetrics(int);
HDC WINAPI reviewGetDC(HWND);
int WINAPI reviewReleaseDC(HWND, HDC);
BOOL WINAPI reviewGetCursorInfo(PCURSORINFO);
BOOL WINAPI reviewEnumDisplayMonitors(HDC, LPCRECT, MONITORENUMPROC, LPARAM);
BOOL WINAPI reviewGetMonitorInfoW(HMONITOR, MONITORINFOEXW*);
BOOL WINAPI reviewEnumDisplayDevicesW(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);
BOOL WINAPI reviewStretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
#define GetSystemMetrics reviewGetSystemMetrics
#define GetDC reviewGetDC
#define ReleaseDC reviewReleaseDC
#define GetCursorInfo reviewGetCursorInfo
#define EnumDisplayMonitors reviewEnumDisplayMonitors
#define GetMonitorInfoW reviewGetMonitorInfoW
#define EnumDisplayDevicesW reviewEnumDisplayDevicesW
#define StretchBlt reviewStretchBlt
#include "../src/capture.cpp"

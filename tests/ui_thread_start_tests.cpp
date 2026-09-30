// Actual Engine and MSVC std::thread; the CRT launch boundary never starts a thread.
#include "ui_thread_start_hooks.h"
#include "engine.h"
#include <process.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace probe {
unsigned launches=0, comCalls=0, mediaCalls=0, captureCalls=0, processCalls=0, powerCalls=0;
bool callbackPresent=false, argumentPresent=false;
[[noreturn]] void unexpected(const char* api) { std::fprintf(stderr,"Unexpected worker API: %s\n",api); std::abort(); }
HRESULT WINAPI initialize(LPVOID,DWORD) { ++comCalls; unexpected("CoInitializeEx"); }
EXECUTION_STATE WINAPI power(EXECUTION_STATE) { ++powerCalls; unexpected("SetThreadExecutionState"); }
HRESULT WINAPI startup(ULONG,DWORD) { ++mediaCalls; unexpected("MFStartup"); }
BOOL WINAPI process(LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,
                    LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION) { ++processCalls; unexpected("CreateProcessW"); }
HDC WINAPI desktop(HWND) { ++captureCalls; unexpected("GetDC"); }
HRESULT WINAPI enumerate(IMFAttributes*,IMFActivate***,UINT32*) { ++captureCalls; unexpected("MFEnumDeviceSources"); }
HRESULT WINAPI device(IMFAttributes*,IMFMediaSource**) { ++captureCalls; unexpected("MFCreateDeviceSource"); }
HRESULT WINAPI writer(LPCWSTR,IMFByteStream*,IMFAttributes*,IMFSinkWriter**) { ++mediaCalls; unexpected("MFCreateSinkWriterFromURL"); }
}

// The shared forced header redirects the installed <thread> implementation's launch call.
// No replacement std::thread, Engine constructor, or synthetic throw is used.
uintptr_t __cdecl probe::launch(void*, unsigned,
    _beginthreadex_proc_type start, void* argument, unsigned, unsigned* threadId) {
    ++probe::launches;
    probe::callbackPresent = start != nullptr;
    probe::argumentPresent = argument != nullptr;
    if(threadId) *threadId=0;
    errno=EAGAIN;
    return 0;
}

int constructorProof() {
    bool observed=false;
    try {
        lapse::Engine actual;
        std::fprintf(stderr,"Engine construction unexpectedly succeeded\n");
        return 1;
    } catch(const std::system_error& error) {
        observed = error.code() == std::make_error_code(std::errc::resource_unavailable_try_again);
        std::printf("actual Engine constructor: std::system_error code=%d category=%s message=%s\n",
            error.code().value(), error.code().category().name(), error.what());
    } catch(...) {
        std::fprintf(stderr,"Unexpected exception type\n");
        return 1;
    }
    const bool ok=observed && probe::launches==1 && probe::callbackPresent && probe::argumentPresent &&
        probe::comCalls==0 && probe::mediaCalls==0 && probe::captureCalls==0 && probe::processCalls==0 && probe::powerCalls==0;
    std::printf("launches=%u callback=%d argument=%d workerCOM=%u media=%u capture=%u process=%u power=%u: %s\n",
        probe::launches,probe::callbackPresent,probe::argumentPresent,probe::comCalls,probe::mediaCalls,
        probe::captureCalls,probe::processCalls,probe::powerCalls,ok?"PASS":"FAIL");
    return ok?0:1;
}

#include "capture.h"
#include "camera_host.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <new>
#include <utility>

namespace {
int windows=0, fontsCreated=0, profileReads=0, keyWrites=0, timerStarts=0, timerStops=0, quits=0;
int debugMessages=0, choiceQueries=0, monitorEnumerations=0, cameraEnumerations=0;
std::array<int,256> selections{}, counts{};
wchar_t lastDebug[256]{};
void require(bool value,const char* message) { if(!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); } }
int index(HWND window) { auto value=reinterpret_cast<INT_PTR>(window); require(value>=0&&value<256,"Unexpected fake handle"); return static_cast<int>(value); }
HBRUSH WINAPI fakeBrush(COLORREF) { return reinterpret_cast<HBRUSH>(1); }
BOOL WINAPI fakeDelete(HGDIOBJ) { return TRUE; }
HFONT WINAPI fakeFont(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCWSTR) { return reinterpret_cast<HFONT>(static_cast<INT_PTR>(20+(++fontsCreated))); }
BOOL WINAPI fakeChildren(HWND,WNDENUMPROC,LPARAM) { return TRUE; }
UINT WINAPI fakeDpi(HWND) { return 96; }
HWND WINAPI fakeWindow(DWORD,LPCWSTR,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID) { return reinterpret_cast<HWND>(static_cast<INT_PTR>(100+(++windows))); }
LRESULT WINAPI fakeSend(HWND window,UINT message,WPARAM value,LPARAM) {
    const int i=index(window);
    if(message==CB_GETCURSEL) { ++choiceQueries; return selections[i]; }
    if(message==CB_SETCURSEL) { selections[i]=static_cast<int>(value); return static_cast<LRESULT>(value); }
    if(message==CB_ADDSTRING) return counts[i]++;
    if(message==CB_RESETCONTENT) { counts[i]=0; selections[i]=-1; }
    return 0;
}
DWORD WINAPI fakeProfileString(LPCWSTR,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD count,LPCWSTR) {
    if(std::wcsncmp(key,L"TimeSkip",8)==0 || std::wcscmp(key,L"StopOnLowDiskSpace")==0 || std::wcscmp(key,L"RecoveryMode")==0 || std::wcscmp(key,L"NightEnabled")==0 || std::wcscmp(key,L"NightDurationMs")==0 || std::wcscmp(key,L"NightTargetBrightness")==0){require(count>std::wcslen(fallback),"Option buffer too small");wcscpy_s(output,count,fallback);return static_cast<DWORD>(std::wcslen(fallback));}
    ++profileReads; const wchar_t value[]=L"C:\\OwnedSynthetic";
    require(count>_countof(value),"Profile buffer too small"); std::wmemcpy(output,value,_countof(value)); return _countof(value)-1;
}
UINT WINAPI fakeProfileInt(LPCWSTR,LPCWSTR,INT fallback,LPCWSTR) { return static_cast<UINT>(fallback); }
BOOL WINAPI fakeProfileWrite(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR) { ++keyWrites; require(false,"Failed startup attempted preferences write"); return FALSE; }
BOOL WINAPI fakeIconic(HWND) { return TRUE; }
BOOL WINAPI fakeEnable(HWND,BOOL) { return TRUE; }
BOOL WINAPI fakeText(HWND,LPCWSTR) { return TRUE; }
BOOL WINAPI fakeInvalidate(HWND,const RECT*,BOOL) { return TRUE; }
UINT_PTR WINAPI fakeTimer(HWND,UINT_PTR id,UINT,TIMERPROC) { ++timerStarts; return id; }
BOOL WINAPI fakeKillTimer(HWND,UINT_PTR) { ++timerStops; return TRUE; }
BOOL WINAPI fakeAffinity(HWND,DWORD) { return TRUE; }
void WINAPI fakeQuit(int) { ++quits; }
void WINAPI fakeDebug(LPCWSTR message) { ++debugMessages; const auto size=std::wcslen(message)+1; require(size<=_countof(lastDebug),"Debug message too long"); std::wmemcpy(lastDebug,message,size); }
std::vector<lapse::Monitor> fakeMonitors() { ++monitorEnumerations; return {{L"Synthetic display",{0,0,1280,720},L"owned-display"}}; }
std::vector<lapse::CameraDevice> fakeCameras(std::wstring&) { ++cameraEnumerations; return {}; }
int fakeHost(const wchar_t*) { require(false,"Unexpected application entry"); return 1; }
}

#define CreateSolidBrush fakeBrush
#define DeleteObject fakeDelete
#define CreateFontW fakeFont
#define EnumChildWindows fakeChildren
#define GetDpiForWindow fakeDpi
#define CreateWindowExW fakeWindow
#define SendMessageW fakeSend
#define GetPrivateProfileStringW fakeProfileString
#define GetPrivateProfileIntW fakeProfileInt
#define WritePrivateProfileStringW fakeProfileWrite
#define IsIconic fakeIconic
#define EnableWindow fakeEnable
#define SetWindowTextW fakeText
#define InvalidateRect fakeInvalidate
#define SetTimer fakeTimer
#define KillTimer fakeKillTimer
#define SetWindowDisplayAffinity fakeAffinity
#define PostQuitMessage fakeQuit
#define OutputDebugStringW fakeDebug
#define enumerateMonitors fakeMonitors
#define enumerateCameras fakeCameras
#define runCameraHost fakeHost
#pragma warning(push)
#pragma warning(disable: 4702) // The helper-entry sentinel makes GUI entry unreachable.
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#pragma warning(pop)
#undef CreateSolidBrush
#undef DeleteObject
#undef CreateFontW
#undef EnumChildWindows
#undef GetDpiForWindow
#undef CreateWindowExW
#undef SendMessageW
#undef GetPrivateProfileStringW
#undef GetPrivateProfileIntW
#undef WritePrivateProfileStringW
#undef IsIconic
#undef EnableWindow
#undef SetWindowTextW
#undef InvalidateRect
#undef SetTimer
#undef KillTimer
#undef SetWindowDisplayAffinity
#undef PostQuitMessage
#undef OutputDebugStringW
#undef enumerateMonitors
#undef enumerateCameras
#undef runCameraHost

int callbackProof() {
    selections.fill(-1); counts.fill(0);
    app.preferences=L"unused-synthetic-profile.ini";
    LRESULT result=123;
    bool escaped=false, correctCode=false;
    try { result=windowProc(reinterpret_cast<HWND>(2),WM_CREATE,0,0); }
    catch(const std::system_error& error) {
        escaped=true; correctCode=error.code()==std::make_error_code(std::errc::resource_unavailable_try_again);
        std::printf("callback escape: actual system_error code=%d category=%s\n",error.code().value(),error.code().category().name());
    }
    require(probe::launches==1&&probe::callbackPresent&&probe::argumentPresent,"Actual Engine did not reach launch boundary once");
    require(!app.engine&&!app.startupComplete&&profileReads==1&&monitorEnumerations==1&&cameraEnumerations==1,"Unexpected creation progress");
    require(probe::comCalls==0&&probe::mediaCalls==0&&probe::captureCalls==0&&probe::processCalls==0&&probe::powerCalls==0,"Worker activity occurred");
    const int beforeResize=choiceQueries;
    windowProc(reinterpret_cast<HWND>(2),WM_SIZE,SIZE_MINIMIZED,0);
    const int resizeQueries=choiceQueries-beforeResize;
    windowProc(reinterpret_cast<HWND>(2),WM_DESTROY,0,0);
    require(!app.engine&&!app.startupComplete&&keyWrites==0&&timerStarts==0&&timerStops==1&&quits==1,"Failed startup teardown changed");
    std::printf("callback result=%lld escaped=%d startupComplete=%d enginePresent=%d resizeQueries=%d debug=%d preferences=%d timers=%d/%d quit=%d\n",
        static_cast<long long>(result),escaped,app.startupComplete,app.engine!=nullptr,resizeQueries,debugMessages,keyWrites,timerStarts,timerStops,quits);
    require(!escaped&&!correctCode&&result==-1&&resizeQueries==0&&debugMessages==1,"Candidate did not contain startup thread failure");
    require(std::wcscmp(lastDebug,L"Timelapse could not start its recording worker.\n")==0,"Unexpected diagnostic");
    std::puts("PASS: actual thread failure contained; incomplete-startup resize and preferences guarded.");

    return 0;
}

namespace {
unsigned routedCreates=0, routedDestroys=0, routeEscapes=0;
LRESULT CALLBACK routedWindow(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message!=WM_CREATE&&message!=WM_DESTROY) return DefWindowProcW(window,message,wparam,lparam);
    if(message==WM_CREATE) ++routedCreates;
    else ++routedDestroys;
    try { return windowProc(window,message,wparam,lparam); }
    catch(...) { // Safety fence must remain unused, rather than masking a missing production catch.
        ++routeEscapes;
        return message==WM_CREATE ? -1 : 0;
    }
}
}
int osProof() {
    selections.fill(-1); counts.fill(0);
    wchar_t name[128]{};
    swprintf_s(name,L"TimelapseOwnedThreadStart_%lu_%llu",GetCurrentProcessId(),GetTickCount64());
    const auto owned=std::filesystem::absolute(std::filesystem::current_path()/name);
    require(std::filesystem::create_directory(owned),"Owned preference directory already exists");
    const auto preferences=owned/L"settings.ini";
    const std::string before="; owned thread-start sentinel\r\n[Settings]\r\nFolder=C:\\Prior\r\nInterval=4\r\nQuality=1\r\nEncodingQuality=0\r\n";
    { std::ofstream output(preferences,std::ios::binary); output<<before; require(bool(output),"Cannot seed owned preferences"); }
    app.preferences=preferences.native();
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSEXW definition{};
    definition.cbSize=sizeof(definition); definition.hInstance=instance;
    definition.lpfnWndProc=routedWindow; definition.lpszClassName=name;
    require(RegisterClassExW(&definition)!=0,"Cannot register owned hidden window class");
    const HWND created=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,name,L"",WS_POPUP,
        0,0,1,1,nullptr,nullptr,instance,nullptr);
    const bool destroyedBeforeReturn=routedDestroys==1;
    require(!created,"Failed startup unexpectedly created a real window");
    require(UnregisterClassW(name,instance)!=FALSE,"Owned window class still in use");
    require(routedCreates==1&&destroyedBeforeReturn&&routeEscapes==0,"OS callback sequence escaped or skipped destruction");
    require(probe::launches==1&&probe::callbackPresent&&probe::argumentPresent,"Actual Engine did not reach launch boundary");
    require(probe::comCalls==0&&probe::mediaCalls==0&&probe::captureCalls==0&&probe::processCalls==0&&probe::powerCalls==0,"Worker activity occurred");
    require(!app.engine&&!app.mode&&!app.startupComplete&&keyWrites==0&&debugMessages==1&&timerStarts==0&&timerStops==1&&quits==1,"Failed OS startup teardown changed");
    std::ifstream input(preferences,std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(input),{}};
    require(after==before,"Failed startup changed existing owned preferences");
    for(const auto& entry:std::filesystem::directory_iterator(owned)) require(entry.path()==preferences,"Failed startup staged preferences");
    std::printf("hidden candidate: hwndNull=%d create=%u destroy=%u destroyBeforeReturn=%d routeEscapes=%u actualLaunchFailures=%u workerCOM=%u media=%u capture=%u process=%u power=%u debug=%d preferences=%d oldBytesUnchanged=%d timers=%d/%d quit=%d: PASS\n",
        created==nullptr,routedCreates,routedDestroys,destroyedBeforeReturn,routeEscapes,probe::launches,
        probe::comCalls,probe::mediaCalls,probe::captureCalls,probe::processCalls,probe::powerCalls,
        debugMessages,keyWrites,after==before,timerStarts,timerStops,quits);
    input.close();
    require(owned.is_absolute()&&owned.parent_path()==std::filesystem::current_path()&&preferences.parent_path()==owned,
        "Owned cleanup path escaped its fixture directory");
    require(std::filesystem::remove(preferences),"Cannot remove owned preferences after readback");
    require(std::filesystem::remove(owned),"Owned directory is not empty after cleanup");
    return 0;
}

namespace {
void resetCase() {
    require(!app.engine,"Previous Engine unexpectedly survived");
    probe::launches=probe::comCalls=probe::mediaCalls=probe::captureCalls=probe::processCalls=probe::powerCalls=0;
    probe::callbackPresent=probe::argumentPresent=false;
    windows=fontsCreated=profileReads=keyWrites=timerStarts=timerStops=quits=debugMessages=choiceQueries=monitorEnumerations=cameraEnumerations=0;
    routedCreates=routedDestroys=routeEscapes=0;lastDebug[0]=0;
    selections.fill(-1);counts.fill(0);
    app.window=nullptr;app.mode=nullptr;app.preview=nullptr;app.startupComplete=false;app.layingOut=false;
    app.monitors.clear();app.cameras.clear();app.selectedMonitorId.clear();
    app.settings.cameraId.clear();app.settings.monitorId.clear();app.settings.folder.clear();
}
}
int main() {
    try {
        resetCase();if(constructorProof()!=0)return 1;
        resetCase();if(callbackProof()!=0)return 1;
        resetCase();if(osProof()!=0)return 1;
        std::puts("PASS 3/3: actual constructor, direct callback and owned hidden creation; no worker launched.");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Unexpected fixture failure: %s\n",error.what());return 1;
    }
}

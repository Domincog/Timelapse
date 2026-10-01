// Actual native entry, entirely synthetic startup/window/message/profile APIs.
// No Engine worker, real COM/MF initialization, device enumeration or app window.
#include "../src/engine.h"
#include "../src/capture.h"
#include "../src/camera_host.h"
#include <objbase.h>
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>
namespace {
HRESULT comResult=S_OK,mediaResult=S_OK;
int helperResult=-1, comStarts=0,mediaStarts=0,comStops=0,mediaStops=0;
int windows=0,classes=0,engines=0,engineStops=0,recordRequests=0,profileCalls=0,folderBuffers=0,folderFrees=0,dialogs=0,messages=0;
int quitCode=27,allocationFailures=0,lastErrorReads=0;
int dialogComStops=0,dialogMediaStops=0,dialogEngineStops=0;
bool forbidWindowFailureAllocation=false,residualWindowEngine=false,dialogEngineAlive=false;
HWND dialogParent=nullptr;UINT dialogFlags=0;
std::array<char,16> dialogCleanup{};size_t dialogCleanupSize=0;
bool engineAlive=false,mediaStoppedAlive=false,invalidFolder=false,failWindow=false,loopError=false;
bool normalDestroy=false,failPathCopy=false,failDiagnostic=false,failNextAllocation=false;
bool mutexErrorPending=false;DWORD fixtureMutexError=ERROR_SUCCESS;
int mutexOpens=0,mutexCreates=0,mutexCloses=0;
bool setupPresent=false,setupRacing=false,instancePresent=false,instanceUnavailable=false;
bool startInTray=false;int shows=0,lastShow=-1,existingSignals=0;
bool markerClosedWithLiveEngine=false;
std::array<char,16> cleanup{};size_t cleanupSize=0;
std::array<wchar_t,1024> messageText{},debugText{};
void capture(std::array<wchar_t,1024>& target,const wchar_t* text){
    const auto length=std::wcslen(text);if(length>=target.size())std::abort();std::copy_n(text,length+1,target.begin());
}
bool contains(const wchar_t* text){return std::wcsstr(messageText.data(),text)!=nullptr;}
void log(char c){if(cleanupSize>=cleanup.size())std::abort();cleanup[cleanupSize++]=c;}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void createEngine();void destroyEngine();
HRESULT WINAPI fakeCom(LPVOID,DWORD){++comStarts;return comResult;}
void WINAPI fakeComStop(){++comStops;log('c');}
HRESULT WINAPI fakeMedia(ULONG,DWORD){++mediaStarts;return mediaResult;}
HRESULT WINAPI fakeMediaStop(){++mediaStops;mediaStoppedAlive|=engineAlive;log('m');return S_OK;}
HRESULT WINAPI fakeFolder(REFKNOWNFOLDERID id,DWORD flags,HANDLE,PWSTR* out){
    require(flags==KF_FLAG_DONT_VERIFY,"Known-folder policy changed");*out=nullptr;
    if(invalidFolder&&IsEqualGUID(id,FOLDERID_Videos))return E_ACCESSDENIED;
    const wchar_t* path=IsEqualGUID(id,FOLDERID_Videos)?L"C:\\Synthetic profile\\Videos":L"C:\\Synthetic profile\\Local";
    const auto count=std::wcslen(path)+1;*out=static_cast<PWSTR>(std::malloc(count*sizeof(wchar_t)));
    if(!*out)return E_OUTOFMEMORY;std::copy(path,path+count,*out);++folderBuffers;
    if(std::exchange(failPathCopy,false))failNextAllocation=true;
    return S_OK;
}
void WINAPI fakeFree(void* value){if(value){--folderBuffers;++folderFrees;std::free(value);}}
BOOL WINAPI fakeControls(const INITCOMMONCONTROLSEX*){return TRUE;}
ATOM WINAPI fakeClass(const WNDCLASSEXW*){++classes;return 1;}
HWND WINAPI fakeWindow(DWORD,LPCWSTR,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID){
    ++windows;
    if(failWindow){
        // Model a surviving inert owner without claiming actual OS dispatch.
        if(residualWindowEngine)createEngine();
        if(forbidWindowFailureAllocation)failNextAllocation=true;
        return nullptr;
    }
    createEngine();return reinterpret_cast<HWND>(1);
}
BOOL WINAPI fakeShow(HWND,int show){++shows;lastShow=show;return TRUE;}BOOL WINAPI fakeUpdate(HWND){return TRUE;}
BOOL WINAPI fakeMessage(LPMSG msg,HWND,UINT,UINT){
    ++messages;if(loopError){if(failDiagnostic)failNextAllocation=true;return -1;}
    if(normalDestroy)destroyEngine();msg->message=WM_QUIT;msg->wParam=quitCode;return 0;
}
DWORD WINAPI fakeLastError(){if(std::exchange(mutexErrorPending,false))return fixtureMutexError;++lastErrorReads;return ERROR_NOT_ENOUGH_MEMORY;}
HANDLE WINAPI fakeOpenMutex(DWORD,BOOL,LPCWSTR){++mutexOpens;mutexErrorPending=true;fixtureMutexError=ERROR_FILE_NOT_FOUND;return setupPresent||(setupRacing&&mutexOpens==2)?reinterpret_cast<HANDLE>(2):nullptr;}
HANDLE WINAPI fakeCreateMutex(LPSECURITY_ATTRIBUTES,BOOL,LPCWSTR){++mutexCreates;mutexErrorPending=true;fixtureMutexError=instancePresent?ERROR_ALREADY_EXISTS:ERROR_SUCCESS;return reinterpret_cast<HANDLE>(1);}
BOOL WINAPI fakeCloseHandle(HANDLE handle){++mutexCloses;if(handle==reinterpret_cast<HANDLE>(1)&&engineAlive)markerClosedWithLiveEngine=true;return TRUE;}
HWND WINAPI fakeFindWindow(LPCWSTR,LPCWSTR){return instanceUnavailable?nullptr:reinterpret_cast<HWND>(3);}
LRESULT WINAPI fakeSendTimeout(HWND,UINT,WPARAM,LPARAM,UINT,UINT,PDWORD_PTR){++existingSignals;return 1;}
UINT WINAPI fakeRegisterMessage(LPCWSTR){return 0xc123;}
BOOL WINAPI fakeTray(DWORD,PNOTIFYICONDATAW){return TRUE;}
HBITMAP WINAPI fakeDib(HDC,const BITMAPINFO*,UINT,void**,HANDLE,DWORD){return nullptr;}
HBITMAP WINAPI fakeBitmap(int,int,UINT,UINT,const void*){return nullptr;}
HMENU WINAPI fakeSystemMenu(HWND,BOOL){return nullptr;}
int WINAPI fakeDialog(HWND parent,LPCWSTR message,LPCWSTR,UINT flags){
    ++dialogs;dialogParent=parent;dialogFlags=flags;
    dialogComStops=comStops;dialogMediaStops=mediaStops;dialogEngineStops=engineStops;dialogEngineAlive=engineAlive;
    dialogCleanup=cleanup;dialogCleanupSize=cleanupSize;
    capture(messageText,message);return IDOK;
}
void WINAPI fakeDebug(LPCWSTR message){capture(debugText,message);}
HBRUSH WINAPI fakeBrush(COLORREF){return reinterpret_cast<HBRUSH>(1);}
BOOL WINAPI fakeDelete(HGDIOBJ){return TRUE;}
HCURSOR WINAPI fakeCursor(HINSTANCE,LPCWSTR){return nullptr;}HICON WINAPI fakeIcon(HINSTANCE,LPCWSTR){return nullptr;}
UINT WINAPI fakeDpi(){return 96;}BOOL WINAPI fakePoint(LPPOINT p){*p={0,0};return TRUE;}
HMONITOR WINAPI fakeMonitor(POINT,DWORD){return reinterpret_cast<HMONITOR>(1);}
BOOL WINAPI fakeMonitorInfo(HMONITOR,LPMONITORINFO info){info->rcMonitor=info->rcWork={0,0,1920,1080};return TRUE;}
LPWSTR WINAPI fakeCommand(){static wchar_t command[]=L"synthetic.exe";return command;}
LPWSTR* WINAPI fakeArguments(LPCWSTR,int* count){static wchar_t exe[]=L"synthetic.exe",tray[]=L"--tray";static LPWSTR args[]={exe,tray};*count=startInTray?2:0;return startInTray?args:nullptr;}
HLOCAL WINAPI fakeLocalFree(HLOCAL){return nullptr;}
UINT WINAPI fakeProfileInt(LPCWSTR,LPCWSTR,INT,LPCWSTR){++profileCalls;throw std::runtime_error("Unexpected profile read");}
DWORD WINAPI fakeProfileString(LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,DWORD,LPCWSTR){++profileCalls;throw std::runtime_error("Unexpected profile read");}
BOOL WINAPI fakeProfileWrite(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR){++profileCalls;throw std::runtime_error("Unexpected profile write");}
}
namespace lapse {
class LifecycleEngine {
public:
    LifecycleEngine(){++engines;engineAlive=true;}
    ~LifecycleEngine(){++engineStops;engineAlive=false;log('e');}
    void configure(const Settings&){}void refreshSources(){}void record(){++recordRequests;}void pause(){}void setPaused(bool){}void finish(){} void cancelDelayedStart() noexcept {}
    Status status(){return {};}
};
std::vector<Monitor> enumerateMonitors(){throw std::runtime_error("Unexpected display enumeration");}
std::vector<CameraDevice> enumerateCameras(std::wstring&){throw std::runtime_error("Unexpected camera enumeration");}
int runCameraHost(const wchar_t*){return helperResult;}
}
#define Engine LifecycleEngine
#define CoInitializeEx fakeCom
#define CoUninitialize fakeComStop
#define MFStartup fakeMedia
#define MFShutdown fakeMediaStop
#define SHGetKnownFolderPath fakeFolder
#define CoTaskMemFree fakeFree
#define InitCommonControlsEx fakeControls
#define RegisterClassExW fakeClass
#define CreateWindowExW fakeWindow
#define ShowWindow fakeShow
#define UpdateWindow fakeUpdate
#define GetMessageW fakeMessage
#define GetLastError fakeLastError
#define OpenMutexW fakeOpenMutex
#define CreateMutexW fakeCreateMutex
#define CloseHandle fakeCloseHandle
#define FindWindowW fakeFindWindow
#define SendMessageTimeoutW fakeSendTimeout
#define RegisterWindowMessageW fakeRegisterMessage
#define Shell_NotifyIconW fakeTray
#define CreateDIBSection fakeDib
#define CreateBitmap fakeBitmap
#define GetSystemMenu fakeSystemMenu
#define MessageBoxW fakeDialog
#define OutputDebugStringW fakeDebug
#define CreateSolidBrush fakeBrush
#define DeleteObject fakeDelete
#define LoadCursorW fakeCursor
#define LoadIconW fakeIcon
#define GetDpiForSystem fakeDpi
#define GetCursorPos fakePoint
#define MonitorFromPoint fakeMonitor
#define GetMonitorInfoW fakeMonitorInfo
#define GetCommandLineW fakeCommand
#define CommandLineToArgvW fakeArguments
#define LocalFree fakeLocalFree
#define GetPrivateProfileIntW fakeProfileInt
#define GetPrivateProfileStringW fakeProfileString
#define WritePrivateProfileStringW fakeProfileWrite
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#undef Engine
#undef CoInitializeEx
#undef CoUninitialize
#undef MFStartup
#undef MFShutdown
#undef SHGetKnownFolderPath
#undef CoTaskMemFree
#undef InitCommonControlsEx
#undef RegisterClassExW
#undef CreateWindowExW
#undef ShowWindow
#undef UpdateWindow
#undef GetMessageW
#undef GetLastError
#undef OpenMutexW
#undef CreateMutexW
#undef CloseHandle
#undef FindWindowW
#undef SendMessageTimeoutW
#undef RegisterWindowMessageW
#undef Shell_NotifyIconW
#undef CreateDIBSection
#undef CreateBitmap
#undef GetSystemMenu
#undef MessageBoxW
#undef OutputDebugStringW
#undef CreateSolidBrush
#undef DeleteObject
#undef LoadCursorW
#undef LoadIconW
#undef GetDpiForSystem
#undef GetCursorPos
#undef MonitorFromPoint
#undef GetMonitorInfoW
#undef GetCommandLineW
#undef CommandLineToArgvW
#undef LocalFree
#undef GetPrivateProfileIntW
#undef GetPrivateProfileStringW
#undef WritePrivateProfileStringW
namespace {
void createEngine(){app.engine=std::make_unique<lapse::LifecycleEngine>();}
void destroyEngine(){app.engine.reset();}
void reset(HRESULT com=S_OK,HRESULT media=S_OK){
    failNextAllocation=false;app.engine.reset();require(folderBuffers==0,"Previous known-folder buffer leaked");
    comResult=com;mediaResult=media;helperResult=-1;
    comStarts=mediaStarts=comStops=mediaStops=windows=classes=engines=engineStops=recordRequests=profileCalls=folderFrees=dialogs=messages=allocationFailures=0;
    engineAlive=mediaStoppedAlive=invalidFolder=failWindow=loopError=normalDestroy=failPathCopy=failDiagnostic=false;
    cleanupSize=0;cleanup={};messageText={};debugText={};quitCode=27;
    lastErrorReads=dialogComStops=dialogMediaStops=dialogEngineStops=0;
    forbidWindowFailureAllocation=residualWindowEngine=dialogEngineAlive=false;
    dialogParent=nullptr;dialogFlags=0;dialogCleanup={};dialogCleanupSize=0;
    mutexOpens=mutexCreates=mutexCloses=0;setupPresent=setupRacing=instancePresent=instanceUnavailable=mutexErrorPending=false;
    app.hiddenToTray=app.trayRegistered=false;app.taskbarCreated=0;app.trayTooltip.clear();
    startInTray=false;shows=existingSignals=0;lastShow=-1;
    markerClosedWithLiveEngine=false;
}
struct Outcome{int code;bool threw;bool unusedAllocation;};
Outcome entry(bool direct=false){
    // This pointer made escaped exceptions observable in the initial probe;
    // it does not imply that production cleanup happened on those paths.
    int (WINAPI* volatile call)(HINSTANCE,HINSTANCE,LPWSTR,int)=&wWinMain;
    try{const int code=direct ? wWinMain(nullptr,nullptr,nullptr,SW_HIDE) : call(nullptr,nullptr,nullptr,SW_HIDE);
        const bool unused=std::exchange(failNextAllocation,false);return {code,false,unused};}
    catch(const std::bad_alloc&){failNextAllocation=false;return {-99,true,false};}
}
bool order(const char* expected){const auto n=std::char_traits<char>::length(expected);return cleanupSize==n&&std::equal(cleanup.begin(),cleanup.begin()+n,expected);}
void clean(int com,int media,const char* events){
    require(comStops==com&&mediaStops==media&&!mediaStoppedAlive&&!app.engine&&order(events),"Initialization teardown ownership/order failed");
    require(profileCalls==0&&folderBuffers==0,"Probe touched profiles or leaked folder allocation");
    require(recordRequests==0,"Normal/helper/tray startup armed a recording without an explicit Record command");
    require(!markerClosedWithLiveEngine,"Installer running marker was released before the recording engine joined");
}
void initialization(HRESULT com,HRESULT media,bool forbidAllocation=false){
    reset(com,media);failNextAllocation=forbidAllocation;const auto out=entry();
    std::cout<<"  init hr=0x"<<std::hex<<static_cast<unsigned>(com)<<"/0x"<<static_cast<unsigned>(media)<<std::dec
             <<" starts="<<comStarts<<','<<mediaStarts<<" stops="<<comStops<<','<<mediaStops<<'\n';
    require(out.code==1&&!out.threw&&comStarts==1&&mediaStarts==(SUCCEEDED(com)?1:0),"COM failure still entered Media Foundation startup");
    clean(SUCCEEDED(com)?1:0,0,SUCCEEDED(com)?"c":"");
    require(windows==0&&classes==0&&engines==0&&messages==0&&dialogs==1,"Initialization failure reached GUI work");
    require(contains(FAILED(com)?L"COM initialization failed":L"media initialization failed"),
            "Initialization diagnostic attributed the wrong stage");
    require(contains(L"Media Feature Pack")== (SUCCEEDED(com)&&media==E_NOTIMPL),"Feature Pack hint applied to unrelated failure");
    require(!forbidAllocation||(out.unusedAllocation&&allocationFailures==0),"Initialization diagnostic attempted a C++ allocation");
}
void earlyReturn(bool paths,bool forbidAllocation=false,bool residualEngine=false){
    reset(S_FALSE,S_OK);invalidFolder=paths;failWindow=!paths;
    forbidWindowFailureAllocation=forbidAllocation;residualWindowEngine=residualEngine;
    const auto out=entry();
    require(out.code==1&&!out.threw,"Early startup failure returned success");
    const char* expectedOrder=residualEngine?"emc":"mc";
    clean(1,1,expectedOrder);
    require(engines==(residualEngine?1:0)&&messages==0&&(paths?windows==0:windows==1),"Early return passed expected startup gate");
    if(paths){
        require(dialogs==1&&contains(L"Windows could not locate"),"Existing profile failure notification changed");
    }else{
        require(dialogs==1&&std::wcscmp(messageText.data(),L"Timelapse could not open its window. Close other applications, then try again.")==0&&dialogParent==nullptr&&dialogFlags==(MB_OK|MB_ICONERROR),"Fixed window-creation notification missing or duplicated");
        const auto length=std::char_traits<char>::length(expectedOrder);
        require(dialogComStops==1&&dialogMediaStops==1&&dialogEngineStops==(residualEngine?1:0)&&!dialogEngineAlive&&dialogCleanupSize==length&&std::equal(dialogCleanup.begin(),dialogCleanup.begin()+length,expectedOrder),"Window notification preceded runtime cleanup");
        require(lastErrorReads==0,"Window notification used a stale LastError diagnostic");
        require(out.unusedAllocation==forbidAllocation&&allocationFailures==0,"Window notification required a C++ allocation after refusal");
        std::cout<<"  window refusal result="<<out.code<<" dialogs="<<dialogs<<" atDialogStops="<<dialogEngineStops<<','<<dialogMediaStops<<','<<dialogComStops<<" allocationGuardUnused="<<out.unusedAllocation<<" allocations="<<allocationFailures<<'\n';
    }
}
void loopCase(bool error,bool destroyed,HRESULT com=S_FALSE){reset(com,S_OK);loopError=error;normalDestroy=destroyed;const auto out=entry();
    std::cout<<"  loop error="<<error<<" destroyed="<<destroyed<<" code="<<out.code<<" engineAliveAtMFStop="<<mediaStoppedAlive<<'\n';
    require(!out.threw&&out.code==(error?1:27),"Message error/quit exit code was lost");clean(1,1,"emc");
    require(engineStops==1&&messages==1&&dialogs==0,"Loop exit duplicated engine cleanup or raised modal dialog");
    require(error?debugText[0]!=0:debugText[0]==0,"Message error debugger diagnostic missing/incorrect");}
void helper(int code){reset();helperResult=code;const auto out=entry();require(out.code==code&&!out.threw&&comStarts==0&&mediaStarts==0&&windows==0,"Helper mode entered GUI runtime");clean(0,0,"");}
void fallback(){require(dialogs==1&&std::wcscmp(messageText.data(),L"Timelapse ran out of memory. Close other applications, then try again.")==0,"Static allocation-failure fallback missing");}
void pathAllocation(bool direct=false){reset();failPathCopy=true;const auto out=entry(direct);
    std::cout<<"  path failure code="<<out.code<<" threw="<<out.threw<<" direct="<<direct<<" alloc="<<allocationFailures<<" stops="<<comStops<<','<<mediaStops<<" order=";
    for(size_t i=0;i<cleanupSize;++i)std::cout<<cleanup[i];std::cout<<'\n';
    require(!out.threw&&out.code==1&&allocationFailures==1&&folderFrees==1&&windows==0,"Actual known-folder allocation failure was not contained");clean(1,1,"mc");fallback();}
void diagnosticAllocation(bool direct=false){reset();loopError=failDiagnostic=true;const auto out=entry(direct);
    std::cout<<"  diagnostic failure code="<<out.code<<" threw="<<out.threw<<" direct="<<direct<<" alloc="<<allocationFailures<<" stops="<<comStops<<','<<mediaStops<<" engine="<<engineAlive<<" order=";
    for(size_t i=0;i<cleanupSize;++i)std::cout<<cleanup[i];std::cout<<'\n';
    require(!out.threw&&out.code==1&&allocationFailures==1,"Actual message-error allocation failure was not contained");clean(1,1,"emc");fallback();}
void installedLifecycle(int scenario) {
    reset();setupPresent=scenario==0;setupRacing=scenario==1;instancePresent=scenario>=2&&scenario<=4;
    instanceUnavailable=scenario==3;startInTray=scenario>=4;
    const auto out=entry();require(!out.threw,"Installed lifecycle exception escaped");
    if(scenario<2){require(out.code==1&&windows==0&&dialogs==1&&contains(L"setup is running")&&mutexCreates==(scenario==1?1:0),"Setup/app race did not refuse startup");clean(1,1,"mc");}
    else if(scenario<=4){require(out.code==(scenario==3?1:0)&&windows==0&&mutexCreates==1&&mutexCloses==1&&existingSignals==(scenario==2?1:0),"Duplicate instance behavior changed");clean(1,1,"mc");}
    else {require(out.code==27&&windows==1&&shows==1&&lastShow==SW_HIDE&&app.hiddenToTray&&!app.trayRegistered,"Tray startup became visible or leaked icon");clean(1,1,"emc");}
}
}
void* operator new(size_t bytes){if(std::exchange(failNextAllocation,false)){++allocationFailures;throw std::bad_alloc();}if(void* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](size_t bytes){return ::operator new(bytes);}void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}void operator delete(void* p,size_t)noexcept{std::free(p);}void operator delete[](void* p,size_t)noexcept{std::free(p);}
int main(){
    std::cout<<std::unitbuf;int good=0,bad=0;
    const auto test=[&](const char* name,auto work){try{work();++good;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++bad;failNextAllocation=false;std::cout<<"FAIL "<<name<<": "<<e.what()<<'\n';}};
    test("COM out-of-memory skips otherwise successful MF",[]{initialization(E_OUTOFMEMORY,S_OK);});
    test("COM changed mode skips otherwise successful MF",[]{initialization(RPC_E_CHANGED_MODE,S_OK);});
    test("COM failure skips missing MF",[]{initialization(E_OUTOFMEMORY,E_NOTIMPL);});
    test("media unavailable balances successful COM",[]{initialization(S_OK,E_NOTIMPL);});
    test("media failure balances S_FALSE COM and avoids unrelated advice",[]{initialization(S_FALSE,E_OUTOFMEMORY);});
    test("default path early return retains existing guard",[]{earlyReturn(true);});
    test("window refusal notifies after runtime cleanup",[]{earlyReturn(false);});
    test("window refusal notification needs no C++ allocation",[]{earlyReturn(false,true);});
    test("window refusal releases surviving inert owner before notification",[]{earlyReturn(false,true,true);});
    test("GetMessage error reports failure and closes engine first",[]{loopCase(true,false);});
    test("WM_QUIT with surviving engine retains code and order",[]{loopCase(false,false);});
    test("WM_QUIT after destruction does not double cleanup",[]{loopCase(false,true);});
    test("S_OK normal startup and quit",[]{loopCase(false,false,S_OK);});
    test("helper success returns before GUI initialization",[]{helper(0);});
    test("helper failure code returns before GUI initialization",[]{helper(37);});
    test("COM initialization diagnostic needs no C++ allocation",[]{initialization(E_OUTOFMEMORY,S_OK,true);});
    test("media initialization diagnostic needs no C++ allocation",[]{initialization(S_OK,E_OUTOFMEMORY,true);});
    test("actual startup allocation is contained after runtime cleanup",[]{pathAllocation();});
    test("actual error allocation is contained after engine/runtime cleanup",[]{diagnosticAllocation();});
    test("direct entry contains actual startup allocation",[]{pathAllocation(true);});
    test("direct entry contains actual error allocation",[]{diagnosticAllocation(true);});
    test("setup blocks GUI startup",[]{installedLifecycle(0);});
    test("setup racing application mutex blocks GUI startup",[]{installedLifecycle(1);});
    test("second normal launch shows existing instance",[]{installedLifecycle(2);});
    test("busy existing instance reports a recoverable startup message",[]{installedLifecycle(3);});
    test("second tray launch never raises existing instance",[]{installedLifecycle(4);});
    test("tray startup remains hidden and removes notification icon on exit",[]{installedLifecycle(5);});
    failNextAllocation=false;app.engine.reset();std::cout<<good<<'/'<<good+bad<<" passed; no real app window/COM/MF/profile/capture/input.\n";return bad?1:0;
}

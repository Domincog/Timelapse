// Actual native controls and handlers, with an inert engine and owned windows.
// Generated files, real bounded background threads, and intercepted Shell/PIDL
// operations verify file selection without ever invoking Explorer or a player.
// The native mnemonic case temporarily shows only a nonactivating tool window
// wholly offscreen. No ordinary app entry, devices, profile I/O or clipboard.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <exception>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>
#include <atomic>
#include <chrono>
#include <mutex>
#include <process.h>
#include <thread>

namespace detailsProbe {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
thread_local bool failAllocation=false,countAllocations=false;
bool failDialog=false,routeFailed=false;
size_t allocations=0;
int allocationFailures=0,dialogs=0,notices=0,records=0,finishes=0,pauses=0;
int statusQueries=0,configures=0,enumerations=0,textWrites=0,focusCalls=0,invalidFocus=0;
int trayCalls=0,shows=0,hides=0,foregrounds=0,quits=0,profileReads=0;
int folderCalls=0;std::atomic<int> shellCalls{0};
std::atomic<bool> blockFolderShell{false},folderShellEntered{false},releaseFolderShell{false};
std::wstring expectedOpenFolder;
std::atomic<int> fileThreadStarts{0},fileThreadExits{0},fileThreadCloses{0},fileComStarts{0},fileComEnds{0},fileParses{0},fileSelections{0},filePidls{0},fileFrees{0};
std::atomic<bool> blockFileParse{false},fileParseEntered{false},releaseFileParse{false},fileProbeError{false};
bool failFileThread=false,failFileTimer=false;
std::atomic<int> failFileParseAt{0};
std::atomic<int> blockFileParseAt{1};
std::atomic<bool> mismatchedFileParent{false},failFileClone{false};
std::atomic<HRESULT> fileComResult{S_OK},fileSelectResult{S_OK};
int fileTimers=0,fileTimerKills=0;
DWORD ownerThread=0;
std::mutex fileMutex;
std::vector<std::wstring> parsedFiles,selectedFiles;
std::wstring selectedFolder;
HANDLE fileThread{};
struct ThreadCall {unsigned (__stdcall* function)(void*);void* parameter;};
unsigned __stdcall runFileThread(void* raw){
    const auto call=*static_cast<ThreadCall*>(raw);delete static_cast<ThreadCall*>(raw);
    const unsigned result=call.function(call.parameter);++fileThreadExits;return result;
}
uintptr_t __cdecl beginThread(void* security,unsigned stack,unsigned (__stdcall* function)(void*),void* parameter,unsigned flags,unsigned* id){
    ++fileThreadStarts;if(failFileThread)return 0;
    auto* call=new ThreadCall{function,parameter};
    const auto result=_beginthreadex(security,stack,runFileThread,call,flags,id);
    if(!result){delete call;return 0;}
    // Retain a separate observer handle only in this fixture for bounded cleanup.
    if(!DuplicateHandle(GetCurrentProcess(),reinterpret_cast<HANDLE>(result),GetCurrentProcess(),&fileThread,SYNCHRONIZE,FALSE,0))fileProbeError=true;
    return result;
}
BOOL WINAPI closeHandle(HANDLE value){++fileThreadCloses;return CloseHandle(value);}
HRESULT WINAPI comInitialize(LPVOID,DWORD flags){
    if(GetCurrentThreadId()==ownerThread || (flags!=COINIT_APARTMENTTHREADED && flags!=(COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))fileProbeError=true;
    ++fileComStarts;return fileComResult;
}
void WINAPI comUninitialize(){++fileComEnds;}
HRESULT WINAPI parseFile(PCWSTR path,IBindCtx*,PIDLIST_ABSOLUTE* output,SFGAOF requested,SFGAOF* attributes){
    *output=nullptr;const int call=++fileParses;
    if(GetCurrentThreadId()==ownerThread)fileProbeError=true;
    if(blockFileParse&&call==blockFileParseAt&&blockFileParse.exchange(false)){
        fileParseEntered=true;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!releaseFileParse && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!releaseFileParse){fileProbeError=true;return E_ABORT;}
    }
    if(call==failFileParseAt)return E_ACCESSDENIED;
    const DWORD nativeAttributes=path?GetFileAttributesW(path):INVALID_FILE_ATTRIBUTES;
    if(nativeAttributes==INVALID_FILE_ATTRIBUTES)return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    const bool directory=(nativeAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    if(attributes)*attributes=requested&(SFGAO_FILESYSTEM|(directory?SFGAO_FOLDER:0));
    const std::filesystem::path actual(path);
    std::array<std::wstring,2> items{directory?actual.wstring():actual.parent_path().wstring(),directory?L"":actual.filename().wstring()};
    if(!directory&&mismatchedFileParent)items[0]+=L"-wrong-parent";
    const size_t count=directory?1:2;size_t total=sizeof(USHORT);
    for(size_t i=0;i<count;++i){const auto bytes=sizeof(USHORT)+(items[i].size()+1)*sizeof(wchar_t);if(bytes>USHRT_MAX)return E_INVALIDARG;total+=bytes;}
    auto* data=static_cast<BYTE*>(CoTaskMemAlloc(total));if(!data)return E_OUTOFMEMORY;
    BYTE* destination=data;
    for(size_t i=0;i<count;++i){const auto bytes=(items[i].size()+1)*sizeof(wchar_t);const USHORT length=static_cast<USHORT>(sizeof(USHORT)+bytes);std::memcpy(destination,&length,sizeof(length));std::memcpy(destination+sizeof(length),items[i].c_str(),bytes);destination+=length;}
    const USHORT end=0;std::memcpy(destination,&end,sizeof(end));
    *output=reinterpret_cast<PIDLIST_ABSOLUTE>(data);++filePidls;
    {std::lock_guard<std::mutex> lock(fileMutex);parsedFiles.emplace_back(path);}
    return S_OK;
}
void WINAPI freePidl(void* value){if(value)++fileFrees;CoTaskMemFree(value);}
PIDLIST_ABSOLUTE clonePidl(PCIDLIST_ABSOLUTE value){if(failFileClone)return nullptr;auto* clone=ILCloneFull(value);if(clone)++filePidls;return clone;}
BOOL WINAPI equalPidl(PCIDLIST_ABSOLUTE first,PCIDLIST_ABSOLUTE second){const auto bytes=ILGetSize(first);return bytes==ILGetSize(second)&&std::memcmp(first,second,bytes)==0;}
std::wstring pidlPath(PCUIDLIST_RELATIVE value){return value?reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(value)+sizeof(USHORT)):L"";}
HRESULT WINAPI selectFiles(PCIDLIST_ABSOLUTE folder,UINT count,PCUITEMID_CHILD_ARRAY children,DWORD flags){
    if(GetCurrentThreadId()==ownerThread || count<1 || count>lapse::MaxRecordingOutputs || flags)fileProbeError=true;
    {std::lock_guard<std::mutex> lock(fileMutex);selectedFolder=pidlPath(folder);selectedFiles.clear();for(UINT i=0;i<count;++i)selectedFiles.push_back((std::filesystem::path(selectedFolder)/pidlPath(children[i])).wstring());}
    ++fileSelections;return fileSelectResult;
}
lapse::Status current;
lapse::Settings configured;
std::wstring notice;
HWND mainWindow{};
bool mainVisible=true;
bool iconic=false;
struct Modal {HWND window{};INT_PTR result=0;bool ended=false;};
std::vector<Modal> modals;
std::function<void(HWND)> script;
std::exception_ptr scriptFailure;
RECT workArea{0,0,1920,1080};
INT_PTR WINAPI modal(HINSTANCE,LPCDLGTEMPLATEW,HWND,DLGPROC,LPARAM);
BOOL WINAPI endModal(HWND window,INT_PTR result){
    for(auto& value:modals)if(value.window==window){value.result=result;value.ended=true;return TRUE;}
    return FALSE;
}
BOOL WINAPI show(HWND window,int command){
    if(GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD)return ShowWindow(window,command);
    if(command==SW_HIDE){if(window==mainWindow)mainVisible=false;++hides;return ShowWindow(window,SW_HIDE);}
    if(window==mainWindow)mainVisible=true;++shows;return TRUE; // Never raise the fixture.
}
BOOL WINAPI visible(HWND window){return window==mainWindow?mainVisible:IsWindowVisible(window);}
BOOL WINAPI isIconic(HWND){return iconic;}
BOOL WINAPI foreground(HWND){++foregrounds;return TRUE;}
BOOL WINAPI notify(DWORD,PNOTIFYICONDATAW){++trayCalls;return TRUE;}
int WINAPI message(HWND,LPCWSTR value,LPCWSTR,UINT){++notices;notice=value;return IDOK;}
BOOL WINAPI setText(HWND window,LPCWSTR value){++textWrites;return SetWindowTextW(window,value);}
HWND WINAPI focus(HWND window){++focusCalls;if(window&&!IsWindow(window))++invalidFocus;return SetFocus(window);}
UINT_PTR WINAPI timer(HWND,UINT_PTR id,UINT,TIMERPROC){if(id==2){++fileTimers;if(failFileTimer)return 0;}return id;}
BOOL WINAPI killTimer(HWND,UINT_PTR id){if(id==2)++fileTimerKills;return TRUE;}
void WINAPI quit(int){++quits;}
BOOL WINAPI affinity(HWND,DWORD){return TRUE;}
HMONITOR WINAPI monitor(HWND,DWORD){return reinterpret_cast<HMONITOR>(1);}
HMONITOR WINAPI monitorRect(LPCRECT,DWORD){return reinterpret_cast<HMONITOR>(1);}
BOOL WINAPI monitorInfo(HMONITOR,LPMONITORINFO value){value->rcMonitor=value->rcWork=workArea;return TRUE;}
UINT WINAPI profileInt(LPCWSTR,LPCWSTR,INT fallback,LPCWSTR){++profileReads;return static_cast<UINT>(fallback);}
DWORD WINAPI profileString(LPCWSTR,LPCWSTR,LPCWSTR fallback,LPWSTR output,DWORD count,LPCWSTR){
    ++profileReads;const size_t size=std::min<size_t>(std::wcslen(fallback),count-1);
    std::wmemcpy(output,fallback,size);output[size]=0;return static_cast<DWORD>(size);
}
BOOL WINAPI profileWrite(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR){throw std::runtime_error("Unexpected preference write.");}
HRESULT WINAPI folderFactory(REFCLSID id,LPUNKNOWN,DWORD,REFIID,LPVOID* output){
    *output=nullptr;require(id==CLSID_FileOpenDialog,"Unexpected COM activation.");++folderCalls;
    return HRESULT_FROM_WIN32(ERROR_CANCELLED); // Count dispatch; never display a picker.
}
BOOL WINAPI shellEx(SHELLEXECUTEINFOW* value){
    if(!value || value->cbSize!=sizeof(*value) || value->hwnd ||
       value->fMask!=(SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI) || !value->lpVerb || std::wcscmp(value->lpVerb,L"open") ||
       !value->lpFile || value->lpFile!=expectedOpenFolder || value->lpParameters || value->lpDirectory ||
       value->nShow!=SW_SHOWNORMAL || GetCurrentThreadId()==ownerThread){fileProbeError=true;return FALSE;}
    ++shellCalls;
    if(blockFolderShell){folderShellEntered=true;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!releaseFolderShell && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!releaseFolderShell){fileProbeError=true;return FALSE;}}
    return TRUE; // Intercept actual async dispatch; never launch Explorer.
}
}
void* operator new(size_t size){
    if(detailsProbe::countAllocations)++detailsProbe::allocations;
    if(std::exchange(detailsProbe::failAllocation,false)){++detailsProbe::allocationFailures;throw std::bad_alloc();}
    if(void* value=std::malloc(size?size:1))return value;throw std::bad_alloc();
}
void* operator new[](size_t size){return ::operator new(size);}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,size_t)noexcept{std::free(value);}
void operator delete[](void* value,size_t)noexcept{std::free(value);}
namespace lapse {
class DetailsEngine {
public:
    void configure(const Settings& value){detailsProbe::configured=value;++detailsProbe::configures;}
    void refreshSources(){}
    void record(){++detailsProbe::records;detailsProbe::current.state=State::Starting;}
    void pause(){}
    void setPaused(bool value){++detailsProbe::pauses;detailsProbe::current.state=value?State::Paused:State::Recording;}
    void finish(){++detailsProbe::finishes;detailsProbe::current.state=State::Finishing;} void cancelDelayedStart() noexcept {} void setStatus(const StatusItem&) {}
    Status status(){++detailsProbe::statusQueries;return detailsProbe::current;}
};
std::vector<Monitor> enumerateMonitors(){++detailsProbe::enumerations;return {{L"Synthetic display",{0,0,1280,720},L"owned-display"}};}
std::vector<CameraDevice> enumerateCameras(std::wstring& error){++detailsProbe::enumerations;error.clear();return {};}
int runCameraHost(const wchar_t*){throw std::runtime_error("Unexpected normal app or camera helper entry.");}
}
#define Engine DetailsEngine
#define DialogBoxIndirectParamW detailsProbe::modal
#define EndDialog detailsProbe::endModal
#define ShowWindow detailsProbe::show
#define IsWindowVisible detailsProbe::visible
#define IsIconic detailsProbe::isIconic
#define SetForegroundWindow detailsProbe::foreground
#define Shell_NotifyIconW detailsProbe::notify
#define MessageBoxW detailsProbe::message
#define SetWindowTextW detailsProbe::setText
#define SetFocus detailsProbe::focus
#define SetTimer detailsProbe::timer
#define KillTimer detailsProbe::killTimer
#define PostQuitMessage detailsProbe::quit
#define SetWindowDisplayAffinity detailsProbe::affinity
#define MonitorFromWindow detailsProbe::monitor
#define MonitorFromRect detailsProbe::monitorRect
#define GetMonitorInfoW detailsProbe::monitorInfo
#define GetPrivateProfileIntW detailsProbe::profileInt
#define GetPrivateProfileStringW detailsProbe::profileString
#define WritePrivateProfileStringW detailsProbe::profileWrite
#define CoCreateInstance detailsProbe::folderFactory
#define ShellExecuteExW detailsProbe::shellEx
#define _beginthreadex detailsProbe::beginThread
#define CloseHandle detailsProbe::closeHandle
#define CoInitializeEx detailsProbe::comInitialize
#define CoUninitialize detailsProbe::comUninitialize
#define SHParseDisplayName detailsProbe::parseFile
#define SHOpenFolderAndSelectItems detailsProbe::selectFiles
#define CoTaskMemFree detailsProbe::freePidl
#pragma push_macro("ILCloneFull")
#undef ILCloneFull
#define ILCloneFull detailsProbe::clonePidl
#define ILIsEqual detailsProbe::equalPidl
#pragma warning(push)
#pragma warning(disable: 4702) // Deliberate normal-entry sentinel above.
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#pragma warning(pop)
#undef Engine
#undef DialogBoxIndirectParamW
#undef EndDialog
#undef ShowWindow
#undef IsWindowVisible
#undef IsIconic
#undef SetForegroundWindow
#undef Shell_NotifyIconW
#undef MessageBoxW
#undef SetWindowTextW
#undef SetFocus
#undef SetTimer
#undef KillTimer
#undef PostQuitMessage
#undef SetWindowDisplayAffinity
#undef MonitorFromWindow
#undef MonitorFromRect
#undef GetMonitorInfoW
#undef GetPrivateProfileIntW
#undef GetPrivateProfileStringW
#undef WritePrivateProfileStringW
#undef CoCreateInstance
#undef ShellExecuteExW
#undef _beginthreadex
#undef CloseHandle
#undef CoInitializeEx
#undef CoUninitialize
#undef SHParseDisplayName
#undef SHOpenFolderAndSelectItems
#undef CoTaskMemFree
#undef ILCloneFull
#pragma pop_macro("ILCloneFull")
#undef ILIsEqual

INT_PTR WINAPI detailsProbe::modal(HINSTANCE instance,LPCDLGTEMPLATEW resource,HWND owner,DLGPROC procedure,LPARAM parameter){
    ++dialogs;if(failDialog)return -1;
    const bool wasEnabled=IsWindowEnabled(owner)!=FALSE;EnableWindow(owner,FALSE);
    HWND window=CreateDialogIndirectParamW(instance,resource,owner,procedure,parameter);
    require(window&&!IsWindowVisible(window),"Only owned hidden native dialogs may be created.");
    modals.push_back({window});
    try{if(script)script(window);else SendMessageW(window,WM_COMMAND,IDCANCEL,0);}
    catch(...){scriptFailure=std::current_exception();modals.pop_back();if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))EnableWindow(owner,wasEnabled);throw;}
    const auto outcome=modals.back();modals.pop_back();
    if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))EnableWindow(owner,wasEnabled);
    require(outcome.ended||!IsWindow(owner),"Script left an owned modal unclosed.");return outcome.result;
}

namespace {
using detailsProbe::require;
void checkCallback(){if(detailsProbe::scriptFailure)std::rethrow_exception(detailsProbe::scriptFailure);require(!detailsProbe::routeFailed,"An exception escaped the actual window handler.");}
LRESULT CALLBACK ownedMain(HWND window,UINT message,WPARAM wp,LPARAM lp){
    try{
        if(message==WM_CREATE){detailsProbe::mainWindow=window;const auto value=windowProc(window,message,wp,lp);app.startupComplete=false;return value;}
        if(message==WM_DESTROY)app.startupComplete=false;
        return windowProc(window,message,wp,lp);
    }catch(...){detailsProbe::routeFailed=true;detailsProbe::failAllocation=false;return message==WM_CREATE?-1:0;}
}
struct Fixture {
    HWND window{};
    Fixture(){
        cancelOpenFolder();app.shellBusyObserved=app.openFolderBusyShown=false;
        app.engine.reset();app.settings={};app.status=detailsProbe::current={};
        app.window=app.mode=app.preview=nullptr;app.customDialog=nullptr;
        app.hiddenToTray=app.closeWhenDone=app.startupComplete=app.layingOut=false;
        app.trayRegistered=app.trayVersion4=app.trayStateValid=app.trayNoticeShown=false;
        app.scrollX=app.scrollY=0;app.settings.folder=L"C:\\Owned synthetic reports";app.preferences=L"inert";
        detailsProbe::failAllocation=detailsProbe::countAllocations=detailsProbe::failDialog=detailsProbe::routeFailed=false;
        detailsProbe::dialogs=detailsProbe::notices=detailsProbe::records=detailsProbe::finishes=detailsProbe::pauses=0;
        detailsProbe::folderCalls=detailsProbe::shellCalls=0;
        detailsProbe::blockFolderShell=detailsProbe::folderShellEntered=detailsProbe::releaseFolderShell=false;
        detailsProbe::expectedOpenFolder=std::filesystem::current_path().wstring();
        detailsProbe::focusCalls=detailsProbe::invalidFocus=detailsProbe::quits=detailsProbe::allocationFailures=0;
        detailsProbe::script={};detailsProbe::scriptFailure={};detailsProbe::notice.clear();detailsProbe::workArea={0,0,1920,1080};detailsProbe::mainWindow=nullptr;detailsProbe::mainVisible=true;detailsProbe::iconic=false;
        WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=ownedMain;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"OwnedStatusDetailsMain";
        require(RegisterClassExW(&cls)||GetLastError()==ERROR_CLASS_ALREADY_EXISTS,"Cannot register owned main class.");
        cls.lpfnWndProc=DefWindowProcW;cls.lpszClassName=L"LapsePreview";
        require(RegisterClassExW(&cls)||GetLastError()==ERROR_CLASS_ALREADY_EXISTS,"Cannot register inert preview class.");
        window=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE, L"OwnedStatusDetailsMain",L"Owned Details fixture",WS_OVERLAPPEDWINDOW,0,0,920,720,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window&&app.engine&&!IsWindowVisible(window)&&!detailsProbe::routeFailed,"Owned actual UI creation failed or became visible.");
    }
    ~Fixture(){detailsProbe::failAllocation=detailsProbe::countAllocations=false;detailsProbe::script={};app.startupComplete=false;if(IsWindow(window))DestroyWindow(window);app.window=app.mode=app.preview=nullptr;app.customDialog=nullptr;detailsProbe::mainWindow=nullptr;}
    void publish(const Status& value){detailsProbe::current=value;applyStatus(value,true);}
    void tick(){SendMessageW(window,WM_TIMER,1,0);checkCallback();}
    void command(int value){SendMessageW(window,WM_COMMAND,MAKEWPARAM(value,BN_CLICKED),0);checkCallback();}
};
std::wstring textOf(HWND window){const int length=GetWindowTextLengthW(window);std::wstring result(static_cast<size_t>(length)+1,L'\0');GetWindowTextW(window,result.data(),length+1);result.resize(static_cast<size_t>(length));return result;}
HWND classChild(HWND window,const wchar_t* name){
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){wchar_t type[40]{};GetClassNameW(child,type,_countof(type));if(_wcsicmp(type,name)==0)return child;}return nullptr;
}
bool ownVisible(HWND window){return (GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE)!=0;}
RECT childRect(HWND window,HWND child){RECT value{};GetWindowRect(child,&value);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&value),2);return value;}
void assertDetailsLayout(HWND window){
    SCROLLINFO horizontal{sizeof(horizontal),SIF_RANGE|SIF_PAGE|SIF_POS},vertical=horizontal;
    require(GetScrollInfo(window,SB_HORZ,&horizontal)&&GetScrollInfo(window,SB_VERT,&vertical),"Details did not expose bounded native scroll geometry.");
    std::vector<RECT> rectangles;
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))if(ownVisible(child)){
        const auto rect=childRect(window,child);auto logical=rect;OffsetRect(&logical,horizontal.nPos,vertical.nPos);
        require(logical.left>=0&&logical.top>=0&&logical.right<=horizontal.nMax+1&&logical.bottom<=vertical.nMax+1&&logical.right>logical.left&&logical.bottom>logical.top,"Details content escaped its nonnegative readable canvas.");
        for(const auto& other:rectangles){RECT intersection{};require(!IntersectRect(&intersection,&rect,&other),"Visible Details controls overlap.");}rectangles.push_back(rect);
        wchar_t type[40]{};GetClassNameW(child,type,_countof(type));
        if(_wcsicmp(type,L"BUTTON")&&_wcsicmp(type,L"STATIC"))continue;
        const auto label=textOf(child);if(label.empty())continue;
        HDC dc=GetDC(child);require(dc!=nullptr,"Cannot measure native control text.");
        const auto font=reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0));const auto previous=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));
        RECT measured{0,0,rect.right-rect.left,0};const bool button=_wcsicmp(type,L"BUTTON")==0;
        DrawTextW(dc,label.c_str(),static_cast<int>(label.size()),&measured,DT_CALCRECT|(button?DT_SINGLELINE:DT_WORDBREAK|DT_NOPREFIX));
        SelectObject(dc,previous);ReleaseDC(child,dc);
        require(measured.right<=rect.right-rect.left-(button?8:0)&&measured.bottom<=rect.bottom-rect.top-(button?2:0),"Native button/help/feedback text is clipped.");
    }
}
void assertRevealed(HWND window,HWND child){RECT client{};GetClientRect(window,&client);const auto rect=childRect(window,child);require(rect.left>=0&&rect.top>=0&&rect.right<=client.right&&rect.bottom<=client.bottom,"Focused Details action is outside the visible viewport.");}
Status retainedReport(bool paired){
    Status value;value.error=value.recordingFailed=true;value.frames=180;value.elapsed=123.4;value.completedSegments=paired?9:0;
    const auto base=L"C:\\Owned\\"+std::wstring(180,L'\u754c')+L"\\\u0417\u0430\u043f\u0438\u0441\u044c-\u65e5\u672c\u8a9e-\u00e9\U0001F4F7";
    value.savedPath=base+L"-desktop.recording.mp4";value.savedPaths.push_back(value.savedPath);
    value.message=L"Recording stopped: Synthetic storage failure. Desktop finished; could not publish final filename. Finished video remains at: "+value.savedPath+L".";
    if(paired){value.savedPaths.push_back(base+L"-camera.mp4");value.message+=L"\nCamera saved: "+value.savedPaths.back()+L". 9 prior complete sets remain available.";}
    return value;
}
std::wstring nativeLines(const std::wstring& value){std::wstring result;for(wchar_t c:value){if(c==L'\n')result+=L'\r';result+=c;}return result;}
void closeDetails(HWND window){SendMessageW(window,WM_COMMAND,IDCANCEL,0);}
void showOffscreen(Fixture& fixture){
    SetWindowPos(fixture.window,nullptr,-30000,-30000,920,720,SWP_NOZORDER|SWP_NOACTIVATE);
    RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
    screen.right+=GetSystemMetrics(SM_CXVIRTUALSCREEN);screen.bottom+=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT placed{},intersect{};GetWindowRect(fixture.window,&placed);
    require(!IntersectRect(&intersect,&placed,&screen),"Keyboard fixture must remain entirely outside the desktop.");
    ShowWindow(fixture.window,SW_SHOWNOACTIVATE);
}
void nativeMnemonic(HWND start,wchar_t character){
    SetFocus(start);MSG message{};message.hwnd=start;message.message=WM_SYSCHAR;message.wParam=character;message.lParam=1L<<29;
    dispatchAppMessage(app.window,message);checkCallback();
}
void waitFileWorker(){
    if(detailsProbe::fileThread){
        require(WaitForSingleObject(detailsProbe::fileThread,6000)==WAIT_OBJECT_0,"Owned Shell worker failed to terminate.");
        CloseHandle(detailsProbe::fileThread);detailsProbe::fileThread=nullptr;
    }
}
template<class Predicate> void waitFiles(Predicate predicate){
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()){require(std::chrono::steady_clock::now()<until,"Expected Shell task state was not reached.");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
struct OwnedFiles {
    std::filesystem::path folder;
    std::vector<std::wstring> files;
    OwnedFiles(){
        waitFileWorker();
        detailsProbe::fileThreadStarts=detailsProbe::fileThreadExits=detailsProbe::fileThreadCloses=0;
        detailsProbe::fileComStarts=detailsProbe::fileComEnds=detailsProbe::fileParses=detailsProbe::fileSelections=0;
        detailsProbe::filePidls=detailsProbe::fileFrees=0;
        detailsProbe::blockFileParse=detailsProbe::fileParseEntered=detailsProbe::releaseFileParse=detailsProbe::fileProbeError=false;
        detailsProbe::mismatchedFileParent=detailsProbe::failFileClone=false;
        detailsProbe::failFileThread=detailsProbe::failFileTimer=false;detailsProbe::failFileParseAt=0;
        detailsProbe::blockFileParseAt=1;
        detailsProbe::fileComResult=detailsProbe::fileSelectResult=S_OK;
        detailsProbe::fileTimers=detailsProbe::fileTimerKills=0;
        {std::lock_guard<std::mutex> lock(detailsProbe::fileMutex);detailsProbe::parsedFiles.clear();detailsProbe::selectedFiles.clear();detailsProbe::selectedFolder.clear();}
        static unsigned ordinal=0;
        folder=std::filesystem::current_path()/(L"owned-details-files-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++ordinal));
        require(std::filesystem::create_directory(folder),"Cannot create owned output fixture.");
        for(const wchar_t* name:{L"\u65e5\u672c\u8a9e-part-000042-desktop.recording.mp4",L"\u0417\u0430\u043f\u0438\u0441\u044c-part-000042-camera.mp4"}){
            files.push_back((folder/name).wstring());std::ofstream output(std::filesystem::path(files.back()),std::ios::binary);output<<"Owned synthetic selection fixture; not a real recording.";require(output.good(),"Cannot create owned file fixture.");
        }
    }
    ~OwnedFiles(){
        detailsProbe::releaseFileParse=true;detailsProbe::releaseFolderShell=true;
        if(detailsProbe::fileThread){WaitForSingleObject(detailsProbe::fileThread,6000);CloseHandle(detailsProbe::fileThread);detailsProbe::fileThread=nullptr;}
        std::error_code ignored;std::filesystem::remove_all(folder,ignored);
    }
    Status report(bool paired=true)const{
        Status value;value.error=value.recordingFailed=true;value.completedSegments=41;
        value.savedPaths=paired?files:std::vector<std::wstring>{files.front()};value.savedPath=value.savedPaths.front();
        value.message=L"Synthetic marker failure. Latest finalized output: "+value.savedPath+L". 41 earlier sets remain in the folder.";return value;
    }
    void assertClean(int successfulThreads)const{
        require(!detailsProbe::fileProbeError&&detailsProbe::fileThreadExits==successfulThreads&&detailsProbe::fileThreadCloses==successfulThreads,"Worker escaped its thread or leaked its returned thread handle.");
        require(detailsProbe::filePidls==detailsProbe::fileFrees,"Shell PIDL ownership did not balance.");
    }
};
void exactSelectableReports(){
    for(bool paired:{false,true}){
        Fixture fixture;auto report=retainedReport(paired);
        if(paired)report.message+=L"\nAdditional synthetic diagnostic: "+std::wstring(70000,L'\u8a9e');
        fixture.publish(report);
        require(ownVisible(app.statusDetails)&&IsWindowEnabled(app.statusDetails),"Retained report has no reachable Details action.");
        detailsProbe::script=[&](HWND window){
            require(textOf(window)==L"Status details"&&GetWindow(window,GW_OWNER)==fixture.window,"Details title/owner changed.");
            HWND edit=classChild(window,L"EDIT");require(edit!=nullptr,"Details has no native selectable text control.");
            const LONG_PTR style=GetWindowLongPtrW(edit,GWL_STYLE);
            require((style&(ES_READONLY|ES_MULTILINE|WS_TABSTOP))==(ES_READONLY|ES_MULTILINE|WS_TABSTOP),"Report is editable or not keyboard selectable.");
            require(textOf(edit)==nativeLines(report.message),"Long Unicode/per-file report was truncated, reordered or changed.");
            const DWORD start=37,end=127;SendMessageW(edit,EM_SETSEL,start,end);DWORD actualStart=0,actualEnd=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&actualStart),reinterpret_cast<LPARAM>(&actualEnd));
            require(actualStart==start&&actualEnd==end,"Native selection failed.");
            SendMessageW(edit,WM_CHAR,L'X',0);require(textOf(edit)==nativeLines(report.message),"Typing modified the read-only report.");
            struct KeyboardState {BYTE prior[256]{};KeyboardState(){require(GetKeyboardState(prior)!=FALSE,"Cannot snapshot thread keyboard state.");}~KeyboardState(){SetKeyboardState(prior);}} keyboard;
            BYTE controlState[256]{};controlState[VK_CONTROL]=0x80;require(SetKeyboardState(controlState)!=FALSE,"Cannot set owned thread control state.");
            SendMessageW(edit,WM_KEYDOWN,L'A',0);SendMessageW(edit,WM_CHAR,1,0);SendMessageW(edit,WM_KEYUP,L'A',0);
            SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&actualStart),reinterpret_cast<LPARAM>(&actualEnd));
            require(actualStart==0&&actualEnd==static_cast<DWORD>(GetWindowTextLengthW(edit))&&textOf(edit)==nativeLines(report.message),"Actual Ctrl+A did not select the complete immutable report.");
            closeDetails(window);
        };
        fixture.command(StatusDetails);
        require(detailsProbe::dialogs==1&&!app.customDialog&&app.status.message==report.message&&app.status.savedPaths==report.savedPaths&&detailsProbe::records==0&&detailsProbe::finishes==0&&detailsProbe::pauses==0,"Inspecting report changed the session or retained paths.");
    }
    std::cout<<"PASS exact >70K UTF-16 paired/split and ordinary Unicode reports, native Ctrl+A/selection and read-only typing\n";
}
void outcomeAndPrimaryPrecedence(){
    Fixture fixture;Status value;fixture.publish(value);
    require(!ownVisible(app.statusDetails),"Ordinary ready status adds an unnecessary Details action.");
    const auto sameMessage=value.message;value.savedPaths={L"C:\\Owned\\camera.mp4"};detailsProbe::current=value;applyStatus(value);
    require(ownVisible(app.statusDetails),"Same-message saved-path outcome failed to reveal Details.");
    value.savedPaths.clear();detailsProbe::current=value;applyStatus(value);require(!ownVisible(app.statusDetails),"Same-message removed outcome retained Details.");
    value.recordingFailed=true;detailsProbe::current=value;applyStatus(value);require(ownVisible(app.statusDetails),"Same-message failure flag failed to reveal Details.");
    value.recordingFailed=false;detailsProbe::current=value;applyStatus(value);require(!ownVisible(app.statusDetails),"Same-message cleared failure retained Details.");
    auto report=retainedReport(true);report.error=report.recordingFailed=false;fixture.publish(report);
    app.watermarkValidation=L"Synthetic current output setting needs correction.";updateStatusText();
    require(statusCaption()==app.watermarkValidation,"Fixture did not expose the idle-validation precedence case.");
    detailsProbe::script=[&](HWND window){const auto shown=textOf(classChild(window,L"EDIT"));const auto primary=nativeLines(report.message);
        require(shown.compare(0,primary.size(),primary)==0&&shown.find(app.watermarkValidation)>primary.size(),"Idle validation displaced or lost the original saved-file report.");closeDetails(window);};
    fixture.command(StatusDetails);
    app.status=detailsProbe::current={};app.settings.layers=preset(Mode::Desktop);app.cameraListError.clear();app.watermarkValidation.clear();updateStatusText();require(!ownVisible(app.statusDetails),"Cleared report retained Details.");
    app.settings.layers=preset(Mode::Camera);app.cameraListError=L"Synthetic source-list failure without a changed engine message.";choose(app.camera,-1);updateStatusText();
    require(ownVisible(app.statusDetails),"Same-message camera-list diagnostic did not reveal Details.");
    app.settings.layers=preset(Mode::Desktop);updateStatusText();require(!ownVisible(app.statusDetails),"Desktop-only selection retained irrelevant camera Details.");
    app.hiddenToTray=true;detailsProbe::mainVisible=false;report=retainedReport(false);fixture.publish(report);
    require(!ownVisible(app.statusDetails)&&app.visibleDirty,"Hidden report changed controls or lost deferred update.");
    SendMessageW(fixture.window,ShowExistingMessage,0,0);checkCallback();require(ownVisible(app.statusDetails)&&!app.hiddenToTray,"Restore did not reveal deferred Details.");
    detailsProbe::iconic=true;Status clear;fixture.publish(clear);require(ownVisible(app.statusDetails)&&app.visibleDirty,"Minimized update did not defer controls.");
    detailsProbe::iconic=false;fixture.tick();require(!ownVisible(app.statusDetails),"Unminimized refresh retained old Details eligibility.");
    app.watermarkValidation.clear();report.error=report.recordingFailed=true;fixture.publish(report);
    app.settings.layers=preset(Mode::Camera);app.cameras.clear();app.cameraListError=L"Synthetic camera listing failed (0x80070005).";choose(app.camera,-1);updateStatusText();
    detailsProbe::script=[&](HWND window){const auto shown=textOf(classChild(window,L"EDIT"));const auto primary=nativeLines(report.message);
        require(shown.compare(0,primary.size(),primary)==0&&shown.find(app.cameraListError)>primary.size()&&shown.find(L"Refresh sources to retry.")!=std::wstring::npos,"Secondary camera cause replaced primary recovery or lost retry hint.");closeDetails(window);};
    fixture.command(StatusDetails);
    require(detailsProbe::records==0&&detailsProbe::finishes==0,"Detail eligibility triggered engine work.");
    std::cout<<"PASS conditional action, same-message outcome changes and primary saved/recovery precedence over idle validation/camera cause\n";
}
void keyboardAndCompactLayout(){
    Fixture fixture;fixture.publish(retainedReport(false));
    SetWindowPos(fixture.window,nullptr,-30000,-30000,920,720,SWP_NOZORDER|SWP_NOACTIVATE);
    RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
    screen.right=screen.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);screen.bottom=screen.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT placed{},intersect{};GetWindowRect(fixture.window,&placed);
    require(!IntersectRect(&intersect,&placed,&screen),"Keyboard fixture must remain entirely outside the desktop.");
    ShowWindow(fixture.window,SW_SHOWNOACTIVATE);
    const HWND action=app.statusDetails;const auto label=textOf(action);const auto amp=label.find(L'&');
    require(amp!=std::wstring::npos&&amp+1<label.size(),"Details action has no mnemonic.");
    for(int dpi:{96,144,192,288}){
        app.dpi=dpi;fonts();SetWindowPos(fixture.window,nullptr,0,0,640,480,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);layout();
        require(GetWindowLongPtrW(action,GWL_STYLE)&WS_TABSTOP,"Details missing native tab navigation.");
        HWND previous=GetNextDlgTabItem(fixture.window,action,TRUE);require(previous&&previous!=action,"Details absent from main tab order.");SetFocus(previous);
        MSG tab{};tab.hwnd=previous;tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;dispatchAppMessage(fixture.window,tab);checkCallback();
        require(GetFocus()==action,"Actual Tab routing did not reach Details.");
        RECT view{};GetClientRect(fixture.window,&view);const auto actionRect=childRect(fixture.window,action);
        require(actionRect.left>=0&&actionRect.top>=0&&actionRect.right<=view.right&&actionRect.bottom<=view.bottom,"Focused Details action was not revealed in compact/high-DPI viewport.");
        RECT overlap{};const auto statusRect=childRect(fixture.window,app.statusText);
        require(!IntersectRect(&overlap,&actionRect,&statusRect),"Details overlaps the status message.");
        detailsProbe::script=[&](HWND window){
            HWND edit=classChild(window,L"EDIT"),files=GetDlgItem(window,StatusDetailsFiles),close=GetDlgItem(window,IDCANCEL);require(edit&&files&&close,"Details navigation controls missing.");
            RECT proposed{0,0,420,260};SendMessageW(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&proposed));
            SetWindowPos(window,nullptr,0,0,420,260,SWP_NOZORDER|SWP_NOACTIVATE);SendMessageW(window,WM_SIZE,0,0);
            assertDetailsLayout(window);
            require(!ownVisible(GetDlgItem(window,StatusDetailsFilesStatus)),"Idle file feedback reserves unnecessary visible content.");
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(edit),TRUE);
            MSG next{};next.hwnd=edit;next.message=WM_KEYDOWN;next.wParam=VK_TAB;require(IsDialogMessageW(window,&next)!=FALSE&&GetFocus()==files,"Native Details Tab failed to reach Show files.");
            assertRevealed(window,files);detailsProbe::failFileTimer=true;SendMessageW(files,BM_CLICK,0,0);detailsProbe::failFileTimer=false;checkCallback();
            require(ownVisible(GetDlgItem(window,StatusDetailsFilesStatus)),"Failed request has no visible inline feedback.");assertDetailsLayout(window);assertRevealed(window,files);
            next.hwnd=files;require(IsDialogMessageW(window,&next)!=FALSE&&GetFocus()==close,"Native Details Tab failed to reach Close after Show files.");assertRevealed(window,close);
            MSG escape{};escape.hwnd=close;escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;require(IsDialogMessageW(window,&escape)!=FALSE&&detailsProbe::modals.back().ended,"Native Escape did not close Details.");
        };
        MSG mnemonic{};mnemonic.hwnd=action;mnemonic.message=WM_SYSCHAR;mnemonic.wParam=towlower(label[amp+1]);mnemonic.lParam=1L<<29;const int before=detailsProbe::dialogs;dispatchAppMessage(fixture.window,mnemonic);checkCallback();
        require(detailsProbe::dialogs==before+1&&!app.customDialog&&GetFocus()==action,"Native mnemonic failed to invoke/close Details and restore its focus.");
        GetWindowRect(fixture.window,&placed);require(!IntersectRect(&intersect,&placed,&screen),"Keyboard fixture moved onto the desktop.");
    }
    require(detailsProbe::records==0&&detailsProbe::finishes==0&&detailsProbe::invalidFocus==0,"Keyboard inspection activated recording or focused a retired window.");
    std::cout<<"PASS actual main Tab/mnemonic, compact high-DPI layout, native dialog Tab/Escape and safe focus\n";
}
void stableSnapshot(){
    Fixture fixture;const auto report=retainedReport(true);fixture.publish(report);const auto originalNotice=app.failureNotice;
    detailsProbe::script=[&](HWND window){
        const HWND edit=classChild(window,L"EDIT");const auto original=textOf(edit);SendMessageW(edit,EM_SETSEL,41,151);
        Status later;later.state=State::Recording;later.message=L"New current status while the report snapshot stays open.";detailsProbe::current=later;fixture.tick();
        DWORD first=0,last=0;SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
        require(textOf(edit)==original&&first==41&&last==151&&app.status.message==later.message,"Timer replacement mutated or deselected the opened snapshot, or stopped main updates.");
        const int calls=detailsProbe::dialogs;fixture.command(StatusDetails);require(detailsProbe::dialogs==calls,"Reentrant Details opened another owned dialog.");
        closeDetails(window);
    };
    fixture.command(StatusDetails);
    require(!app.customDialog&&detailsProbe::records==0&&detailsProbe::finishes==0&&detailsProbe::notices==0,"Snapshot inspection mutated session or added a warning.");
    require(originalNotice==FailureNotice::Pending||originalNotice==FailureNotice::Presented,"Fixture lacked retained failure.");
    std::cout<<"PASS immutable snapshot and selection while main status changes; duplicate-open guard\n";
}
void modalLifecycle(){
    for(State state:{State::Recording,State::Waiting,State::Starting})for(int action:{0,1,2}){
        Fixture fixture;Status recording;recording.state=state;recording.startDeadlineTick=state==State::Waiting?GetTickCount64()+5000:0;recording.error=true;recording.message=L"Synthetic optional preview diagnostic during recording.";fixture.publish(recording);
        int afterFocus=0;
        detailsProbe::script=[&](HWND window){
            if(action==0)SendMessageW(fixture.window,WM_CLOSE,0,0);
            else if(action==1)fixture.command(TrayExit);
            else SendMessageW(fixture.window,WM_ENDSESSION,TRUE,0);
            afterFocus=detailsProbe::focusCalls;
            require(detailsProbe::modals.back().ended||!IsWindow(window),"Hide/Exit/session end did not cancel owned Details.");
        };
        fixture.command(StatusDetails);
        require(!app.customDialog&&detailsProbe::focusCalls==afterFocus&&detailsProbe::invalidFocus==0,"Modal shutdown retained ownership or restored invalid/hidden focus.");
        require(detailsProbe::records==0&&detailsProbe::pauses==0&&detailsProbe::finishes==(action==1?1:0),"Inspecting/closing Details triggered extra recording commands.");
        if(action==0)require(app.hiddenToTray&&detailsProbe::notices==0,"Hide raised an unexpected details failure.");
        if(action==1)require(app.closeWhenDone&&detailsProbe::notices==(state==State::Waiting?0:1),"Exit confirmation did not match waiting cancellation or prepared/active recording.");
        if(action==2)require(!IsWindow(fixture.window),"Confirmed session end left owner alive.");
    }
    Fixture fixture;Status before;before.error=true;before.state=State::Recording;before.message=L"Preview issue before terminal result.";fixture.publish(before);
    detailsProbe::script=[&](HWND window){const auto snapshot=textOf(classChild(window,L"EDIT"));detailsProbe::current=retainedReport(true);fixture.tick();
        require(app.failureNotice==FailureNotice::Pending&&detailsProbe::notices==0&&textOf(classChild(window,L"EDIT"))==snapshot,"Failure arriving behind disabled owner was acknowledged, replaced snapshot or duplicated notice.");
        closeDetails(window);
    };
    fixture.command(StatusDetails);fixture.tick();
    require(app.failureNotice==FailureNotice::Presented&&detailsProbe::notices==0&&!app.customDialog,"Normal visible acknowledgement after close added a popup or orphan.");
    std::cout<<"PASS Hide/Exit/session shutdown cancellation and pending failure inside modal without duplicates or stale focus\n";
}
void failuresAndUnchangedTicks(){
    Fixture fixture;const auto report=retainedReport(true);fixture.publish(report);const auto paths=app.status.savedPaths;
    detailsProbe::failAllocation=true;fixture.command(StatusDetails);
    require(detailsProbe::allocationFailures==1&&detailsProbe::dialogs==0&&!app.customDialog&&app.status.message==report.message&&app.status.savedPaths==paths,"Snapshot allocation failure escaped or changed recovery facts.");
    detailsProbe::failDialog=true;fixture.command(StatusDetails);detailsProbe::failDialog=false;
    require(detailsProbe::dialogs==1&&!app.customDialog&&app.status.message==report.message&&app.status.savedPaths==paths&&!detailsProbe::notice.empty(),"Dialog failure lost original report or recovery.");
    fixture.tick();fixture.tick();
    detailsProbe::countAllocations=true;detailsProbe::allocations=0;for(int i=0;i<100;++i){auto copied=detailsProbe::current;(void)copied;}const auto copyAllocations=detailsProbe::allocations;
    detailsProbe::allocations=0;detailsProbe::textWrites=0;const int dialogs=detailsProbe::dialogs,sourceCalls=detailsProbe::enumerations,queries=detailsProbe::statusQueries;
    const int fileThreads=detailsProbe::fileThreadStarts,fileParses=detailsProbe::fileParses,fileTimers=detailsProbe::fileTimers;
    for(int i=0;i<100;++i)fixture.tick();const auto tickAllocations=detailsProbe::allocations;detailsProbe::countAllocations=false;
    require(detailsProbe::statusQueries==queries+100&&detailsProbe::dialogs==dialogs&&detailsProbe::enumerations==sourceCalls&&detailsProbe::textWrites==0&&tickAllocations==copyAllocations,"Unchanged ticks added Details work beyond existing Status copies.");
    require(detailsProbe::fileThreadStarts==fileThreads&&detailsProbe::fileParses==fileParses&&detailsProbe::fileTimers==fileTimers,"Unchanged ticks started Shell/file polling work.");
    require(detailsProbe::records==0&&detailsProbe::finishes==0&&detailsProbe::invalidFocus==0,"Failure handling changed recording or focused dead UI.");
    std::cout<<"PASS snapshot/dialog failures preserve report; 100 unchanged ticks add no text, enumeration, modal or allocations beyond Status copies\n";
}
void cursorKeyboard(){
    Fixture fixture;showOffscreen(fixture);fixture.command(TabRecording);
    require(IsWindowEnabled(app.captureCursor)&&ownVisible(app.captureCursor)&&SendMessageW(app.captureCursor,BM_GETCHECK,0,0)==BST_CHECKED,"Cursor checkbox did not start visible, editable and checked on the Recording page.");
    const int configured=detailsProbe::configures,folders=detailsProbe::folderCalls,dialogs=detailsProbe::dialogs;
    nativeMnemonic(app.interval,L'k');
    require(!app.settings.captureCursor&&!detailsProbe::configured.captureCursor&&detailsProbe::configures==configured+1&&GetFocus()==app.captureCursor,"Native Alt+K did not toggle only the desktop cursor option.");
    SendMessageW(app.captureCursor,WM_KEYDOWN,VK_SPACE,0);SendMessageW(app.captureCursor,WM_KEYUP,VK_SPACE,0);checkCallback();
    require(app.settings.captureCursor&&detailsProbe::configures==configured+2,"Native Space did not restore the cursor checkbox.");
    nativeMnemonic(app.interval,L'k');fixture.tick();fixture.tick();
    const int stable=detailsProbe::configures;detailsProbe::textWrites=0;detailsProbe::allocations=0;detailsProbe::countAllocations=true;
    for(int i=0;i<100;++i){auto copied=detailsProbe::current;(void)copied;}const auto copyAllocations=detailsProbe::allocations;detailsProbe::allocations=0;
    for(int i=0;i<100;++i)fixture.tick();detailsProbe::countAllocations=false;
    require(detailsProbe::configures==stable&&detailsProbe::textWrites==0&&detailsProbe::allocations==copyAllocations,"Unchanged cursor-off status ticks added UI work beyond existing Status copies.");
    for(State state:{State::Waiting,State::Starting,State::Recording,State::Paused,State::Finishing}){
        Status status;status.state=state;fixture.publish(status);const int calls=detailsProbe::configures;
        nativeMnemonic(app.preview,L'k');require(!app.settings.captureCursor&&detailsProbe::configures==calls&&!IsWindowEnabled(app.captureCursor),"Disabled cursor mnemonic changed an active session.");
    }
    fixture.publish({});choose(app.mode,1);SendMessageW(fixture.window,WM_COMMAND,MAKEWPARAM(ModeBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.mode));checkCallback();
    const int calls=detailsProbe::configures;nativeMnemonic(app.preview,L'k');require(!app.settings.captureCursor&&detailsProbe::configures==calls&&!IsWindowEnabled(app.captureCursor),"Camera-only native cursor mnemonic changed the retained choice.");
    require(detailsProbe::folderCalls==folders&&detailsProbe::dialogs==dialogs&&!detailsProbe::records&&!detailsProbe::finishes,"Cursor shortcut activated an unrelated command.");
    std::cout<<"PASS actual Alt+K/Space, desktop/active/camera locks, distinct commands and no added unchanged cursor-off UI work\n";
}
void distinctMainMnemonics(){
    Fixture fixture;showOffscreen(fixture);app.settings.folder=std::filesystem::current_path().wstring();
    int compressionDialogs=0,detailDialogs=0;bool expectReadOnly=false;
    detailsProbe::script=[&](HWND window){
        const auto title=textOf(window);
        if(title==L"Time compression"){
            ++compressionDialogs;require((IsWindowEnabled(GetDlgItem(window,SkipMode))==FALSE)==expectReadOnly,"Compression shortcut lost active read-only state.");
            require(ownVisible(GetDlgItem(window,IDOK))!=expectReadOnly,"Compression shortcut exposed Apply while active or hid it while idle.");
        }else{require(title==L"Status details","Shortcut dispatched an unrelated dialog.");++detailDialogs;}
        closeDetails(window);
    };
    for(State state:{State::Idle,State::Recording,State::Paused})for(bool expanded:{false,true}){
        if(app.panelTab!=(expanded?RecordingTab:CaptureTab))fixture.command(expanded?TabRecording:TabCapture);
        Status report;report.state=state;report.message=L"Saved synthetic part: owned-camera.mp4";report.savedPath=L"owned-camera.mp4";report.savedPaths={report.savedPath};fixture.publish(report);
        require(ownVisible(app.skipConfigure)==expanded,"Settings page visibility is inconsistent.");expectReadOnly=state!=State::Idle;const auto settings=app.settings;const int configurations=detailsProbe::configures;
        std::vector<HWND> starts{app.tabs[app.panelTab],app.statusDetails};if(expanded)starts.push_back(app.skipConfigure);else starts.push_back(app.openFolder);if(state==State::Idle && !expanded)starts.push_back(app.folder);
        for(HWND start:starts){
            const int folders=detailsProbe::folderCalls,compressed=compressionDialogs,shells=detailsProbe::shellCalls,details=detailDialogs;
            nativeMnemonic(start,L'h');require(detailsProbe::folderCalls==folders+(state==State::Idle?1:0)&&compressionDialogs==compressed&&detailDialogs==details,"Alt+H failed to route only to the enabled Change action.");
            // IsDialogMessage also activates unique mnemonics of buttons on hidden pages.
            nativeMnemonic(start,L'c');require(compressionDialogs==compressed+1&&detailsProbe::folderCalls==folders+(state==State::Idle?1:0)&&detailDialogs==details,"Alt+C changed folder or failed to invoke compression.");
            nativeMnemonic(start,L'o');waitFileWorker();fixture.tick();
            require(detailsProbe::shellCalls==shells+1 && !detailsProbe::fileProbeError && !app.openFolderTask,"Alt+O no longer completes the configured folder action asynchronously.");
            nativeMnemonic(start,L'i');require(detailDialogs==details+1,"Alt+I no longer invokes Details.");
        }
        require(detailsProbe::configures==configurations&&app.settings.folder==settings.folder&&app.settings.intervalMs==settings.intervalMs&&skipValues(app.settings.timeSkip)==skipValues(settings.timeSkip),"Canceled keyboard inspection changed accepted settings/configuration.");
    }
    require(detailsProbe::records==0&&detailsProbe::finishes==0&&detailsProbe::pauses==0&&detailsProbe::notices==0&&!app.customDialog,"Main shortcuts dispatched recording, notices or left a modal owner.");
    std::cout<<"PASS distinct native Alt+H/Alt+C plus preserved Alt+O/Alt+I from multiple controls, Capture/Recording pages and idle/recording/paused locks\n";
}
void delayKeyboard(){Fixture fixture;showOffscreen(fixture);
    if(app.panelTab!=RecordingTab)fixture.command(TabRecording);
    nativeMnemonic(app.tabs[RecordingTab],L'a');
    require(GetFocus()==app.startDelay && IsWindowEnabled(app.startDelay) && ownVisible(app.startDelay),
        "Native Alt+A failed to focus the visible editable start-delay selector");
    const auto selected=choice(app.startDelay);const int configs=detailsProbe::configures;
    for(bool expanded:{false,true})for(State state:{State::Waiting,State::Starting}){
        if(app.panelTab!=(expanded?RecordingTab:CaptureTab))fixture.command(expanded?TabRecording:TabCapture);
        for(HWND start:{app.tabs[app.panelTab],app.preview}){Status value;value.state=state;
            value.startDeadlineTick=state==State::Waiting?GetTickCount64()+5000:0;fixture.publish(value);
            require(!IsWindowEnabled(app.stopAfter) && !IsWindowEnabled(app.startDelay) && IsWindowEnabled(app.finish) &&
                textOf(app.finish)==L"Cancel s&tart","Waiting/Preparing keyboard preconditions were not locked");
            const int finishes=detailsProbe::finishes,records=detailsProbe::records,pauses=detailsProbe::pauses;
            nativeMnemonic(start,L't');
            std::cout<<"Native delay key state="<<static_cast<int>(state)<<" expanded="<<expanded<<" start="<<GetDlgCtrlID(start)
                <<" focus="<<GetDlgCtrlID(GetFocus())<<" finishes="<<detailsProbe::finishes-finishes
                <<" records="<<detailsProbe::records-records<<" pauses="<<detailsProbe::pauses-pauses
                <<" configs="<<detailsProbe::configures-configs<<" selection="<<choice(app.startDelay)<<'\n';
            require(detailsProbe::finishes==finishes+1 && detailsProbe::records==records && detailsProbe::pauses==pauses &&
                choice(app.startDelay)==selected && detailsProbe::configures==configs,
                "Native Alt+T failed to cancel start or routed to the disabled Stop-after setting");
        }
    }
    fixture.publish(Status{});nativeMnemonic(app.tabs[app.panelTab],L't');
    require(GetFocus()==app.stopAfter && IsWindowEnabled(app.stopAfter) && detailsProbe::configures==configs,
        "Returning idle failed to restore the native Stop-after mnemonic");
    std::cout<<"PASS actual Alt+A delay focus and Alt+T Cancel across Waiting/Preparing, Capture/Recording pages and multiple starting controls\n";
}
void clickFiles(HWND window){const HWND button=GetDlgItem(window,StatusDetailsFiles);require(button&&IsWindowEnabled(button),"Expected Show files action is unavailable.");SendMessageW(button,BM_CLICK,0,0);checkCallback();}
void completeFiles(HWND window){waitFileWorker();SendMessageW(window,WM_TIMER,StatusDetailsFilesTimer,0);checkCallback();require(IsWindowEnabled(GetDlgItem(window,StatusDetailsFiles))&&!shellOperationBusy,"Completed task retained busy action/global slot.");}
void sharedFolderSlot(){
    OwnedFiles files;Fixture fixture;auto report=files.report();fixture.publish(report);
    app.settings.folder=files.folder.wstring();detailsProbe::expectedOpenFolder=app.settings.folder;
    detailsProbe::blockFolderShell=true;fixture.command(OpenFolder);
    waitFiles([]{return detailsProbe::folderShellEntered.load();});
    const auto mainTask=app.openFolderTask;
    require(mainTask && shellOperationBusy && detailsProbe::fileThreadStarts==1,"Open folder failed to own the shared Shell slot.");
    detailsProbe::script=[&](HWND window){const auto snapshot=textOf(classChild(window,L"EDIT"));clickFiles(window);
        require(detailsProbe::fileThreadStarts==1 && detailsProbe::fileSelections==0 &&
            textOf(GetDlgItem(window,StatusDetailsFilesStatus)).find(L"Another file request")!=std::wstring::npos,
            "Show files bypassed a pending main folder request.");
        detailsProbe::releaseFolderShell=true;waitFileWorker();fixture.tick();
        require(!app.openFolderTask && !shellOperationBusy && detailsProbe::notices==0 && textOf(classChild(window,L"EDIT"))==snapshot,
            "Successful main completion behind Details changed the immutable report or raised a modal.");
        detailsProbe::blockFileParse=true;clickFiles(window);waitFiles([]{return detailsProbe::fileParseEntered.load();});closeDetails(window);
    };fixture.command(StatusDetails);
    require(shellOperationBusy && detailsProbe::fileThreadStarts==2,"Closing Details released its still-blocked worker slot early.");
    fixture.command(OpenFolder);
    require(!app.openFolderTask && detailsProbe::fileThreadStarts==2 && detailsProbe::shellCalls==1,
        "Main action bypassed a cancelled but still-running Show files request.");
    detailsProbe::releaseFileParse=true;waitFileWorker();fixture.tick();
    require(!shellOperationBusy && IsWindowEnabled(app.openFolder) && detailsProbe::fileSelections==0 &&
        app.status.message==report.message && app.status.savedPaths==report.savedPaths,"Shared-slot retirement changed recovery facts or disabled retry.");
    detailsProbe::blockFolderShell=false;fixture.command(OpenFolder);waitFileWorker();fixture.tick();
    require(detailsProbe::shellCalls==2 && detailsProbe::fileThreadStarts==3 && !app.openFolderTask,"Main action could not retry after the shared slot retired.");
    files.assertClean(3);
    std::cout<<"PASS actual cross-action Shell exclusion, detached Details cancellation and main retry without stale report mutation\n";
}
void fileSnapshotSelection(){
    for(bool paired:{false,true}){
        OwnedFiles files;Fixture fixture;const auto report=files.report(paired);fixture.publish(report);if(paired)detailsProbe::fileComResult=S_FALSE;
        detailsProbe::script=[&](HWND window){
            const auto snapshot=textOf(classChild(window,L"EDIT"));detailsProbe::blockFileParse=true;clickFiles(window);
            waitFiles([]{return detailsProbe::fileParseEntered.load();});
            require(!IsWindowEnabled(GetDlgItem(window,StatusDetailsFiles))&&detailsProbe::fileTimers==1,"Pending action was not disabled or lacked completion timer.");
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(StatusDetailsFiles,BN_CLICKED),0);require(detailsProbe::fileThreadStarts==1,"Duplicate pending action started another worker.");
            auto later=report;later.savedPaths={L"C:\\Not the opened snapshot\\later.mp4"};later.savedPath=later.savedPaths.front();later.message=L"A later output set.";
            detailsProbe::current=later;app.settings.folder=L"C:\\Unrelated current folder";fixture.tick();
            require(textOf(classChild(window,L"EDIT"))==snapshot&&app.status.message==later.message,"Pending file action mutated snapshot or blocked main status updates.");
            detailsProbe::releaseFileParse=true;completeFiles(window);
            require(detailsProbe::fileSelections==1&&detailsProbe::selectedFiles==report.savedPaths&&detailsProbe::selectedFolder==files.folder.wstring(),"Shell selected current settings, earlier split parts, or an incomplete/reordered output pair.");
            require(textOf(classChild(window,L"EDIT"))==snapshot&&textOf(GetDlgItem(window,StatusDetailsFilesStatus)).find(L"Selection requested")!=std::wstring::npos,"Successful selection overwrote report or omitted completion feedback.");
            require(detailsProbe::fileComStarts==1&&detailsProbe::fileComEnds==1&&detailsProbe::fileTimerKills==1,"Success leaked COM or completion timer.");closeDetails(window);
        };
        fixture.command(StatusDetails);files.assertClean(1);
        require(!detailsProbe::notices&&!detailsProbe::records&&!detailsProbe::shellCalls,"File action launched a player, recording, picker, or warning popup.");
    }
    std::cout<<"PASS worker selects immutable single/paired latest outputs including retained partial names while status/settings change\n";
}
void fileEligibility(){
    OwnedFiles files;Fixture fixture;
    for(int kind=0;kind<7;++kind){
        auto report=files.report();
        if(kind==0){report.savedPaths.clear();report.savedPath=files.files[0];}
        if(kind==1){report.savedPaths.clear();report.savedPath.clear();report.message=L"Failure text mentions C:\\unfinalized.recording.mp4 only.";}
        if(kind==2)report.savedPaths={L"relative.mp4"};
        if(kind==3)report.savedPaths.push_back(files.files[0]);
        if(kind==4)report.savedPaths[1]=(files.folder.parent_path()/L"other-folder/file.mp4").wstring();
        if(kind==5)report.savedPaths[0]+=std::wstring(1,L'\0')+L"suffix";
        if(kind==6)for(const wchar_t* name:{L"third.mp4",L"fourth.mp4"})report.savedPaths.push_back((files.folder/name).wstring());
        fixture.publish(report);detailsProbe::script=[&](HWND window){
            const HWND button=GetDlgItem(window,StatusDetailsFiles);require((button!=nullptr)==(kind==0),"Malformed/unfinalized status offered Show files or compatible savedPath fallback was lost.");
            require(textOf(classChild(window,L"EDIT"))==nativeLines(report.message),"Selection eligibility changed the original report.");
            if(kind==0){clickFiles(window);completeFiles(window);require(detailsProbe::selectedFiles==std::vector<std::wstring>{files.files[0]},"savedPath compatibility selected a different file.");}closeDetails(window);
        };fixture.command(StatusDetails);
    }
    files.assertClean(1);require(detailsProbe::fileThreadStarts==1,"Opening Details itself started filesystem work.");
    std::cout<<"PASS bounded finalized-path eligibility, single savedPath compatibility, and no text-parsed recovery guesses\n";
}
void threeFileSelection(){
    OwnedFiles files;Fixture fixture;
    const auto combined=(files.folder/L"Timelapse-three-files.mp4").wstring();
    {std::ofstream output(std::filesystem::path(combined),std::ios::binary);output<<"Owned synthetic selection fixture; not a real recording.";require(output.good(),"Cannot create third owned file fixture.");}
    auto report=files.report();report.savedPaths.insert(report.savedPaths.begin(),combined);report.savedPath=combined;
    fixture.publish(report);
    detailsProbe::script=[&](HWND window){
        require(GetDlgItem(window,StatusDetailsFiles)!=nullptr,"A combined video with two companion files did not offer Show files.");
        clickFiles(window);completeFiles(window);
        require(detailsProbe::fileSelections==1&&detailsProbe::selectedFiles==report.savedPaths&&detailsProbe::selectedFolder==files.folder.wstring(),
            "Show files did not select the whole three-file set in order.");
        closeDetails(window);
    };fixture.command(StatusDetails);
    waitFileWorker();files.assertClean(1);
    std::cout<<"PASS three-file companion set selects every saved file\n";
}
void fileFailures(){
    for(int fault=0;fault<11;++fault){
        OwnedFiles files;Fixture fixture;const auto report=files.report();fixture.publish(report);
        detailsProbe::script=[&](HWND window){
            const auto original=textOf(classChild(window,L"EDIT"));
            if(fault==0)detailsProbe::failFileTimer=true;
            if(fault==1)detailsProbe::failFileThread=true;
            if(fault==2)detailsProbe::fileComResult=E_FAIL;
            if(fault==3)detailsProbe::failFileParseAt=1;
            if(fault==4)detailsProbe::failFileParseAt=3;
            if(fault==5)detailsProbe::failFileClone=true;
            if(fault==6)detailsProbe::mismatchedFileParent=true;
            if(fault==7)detailsProbe::fileSelectResult=E_ACCESSDENIED;
            if(fault==8)require(DeleteFileW(files.files[1].c_str())!=FALSE,"Cannot remove owned saved-file fixture.");
            if(fault==9)detailsProbe::failAllocation=true;
            if(fault==10)require(MoveFileW(files.files[0].c_str(),(files.folder/L"moved-output.mp4").c_str())!=FALSE,"Cannot move owned file fixture.");
            clickFiles(window);if((fault>1&&fault<9)||fault==10)completeFiles(window);
            require(!shellOperationBusy&&IsWindowEnabled(GetDlgItem(window,StatusDetailsFiles)),"Failed file request consumed the retry slot/action.");
            require(textOf(classChild(window,L"EDIT"))==original&&app.status.message==report.message&&app.status.savedPaths==report.savedPaths,"File failure changed saved/recovery facts.");
            require(!textOf(GetDlgItem(window,StatusDetailsFilesStatus)).empty()&&!detailsProbe::notices,"File failure lacked inline feedback or produced a modal warning.");
            require(detailsProbe::fileSelections==(fault==7?1:0),"A failed prerequisite still issued Shell selection.");
            if(fault==0||fault==9)require(detailsProbe::fileThreadStarts==0,"Timer/allocation failure launched a worker.");
            if(fault==2)require(detailsProbe::fileComStarts==1&&detailsProbe::fileComEnds==0,"Failed COM startup was uninitialized.");
            if((fault>=3&&fault<=8)||fault==10)require(detailsProbe::fileComStarts==1&&detailsProbe::fileComEnds==1,"Failure leaked a successful COM initialization.");
            closeDetails(window);
        };fixture.command(StatusDetails);files.assertClean((fault>=2&&fault<=8)||fault==10?1:0);
    }
    std::cout<<"PASS timer/thread/allocation/COM, parent/item/clone/identity, missing-file and Shell errors preserve report and release resources\n";
}
void filePendingLifecycle(){
    for(int action=0;action<4;++action){
        OwnedFiles files;Fixture fixture;auto report=files.report();report.state=State::Recording;fixture.publish(report);
        detailsProbe::script=[&](HWND window){
            detailsProbe::blockFileParseAt=action==0?3:1;detailsProbe::blockFileParse=true;clickFiles(window);waitFiles([]{return detailsProbe::fileParseEntered.load();});
            const int reads=detailsProbe::statusQueries;for(int i=0;i<10;++i)fixture.tick();require(detailsProbe::statusQueries==reads+10,"Blocked Shell parse stopped main timer routing.");
            const auto began=std::chrono::steady_clock::now();
            if(action==0)closeDetails(window);
            if(action==1)SendMessageW(fixture.window,WM_CLOSE,0,0);
            if(action==2)fixture.command(TrayExit);
            if(action==3)SendMessageW(fixture.window,WM_ENDSESSION,TRUE,0);
            require(std::chrono::steady_clock::now()-began<std::chrono::seconds(1),"Closing/hiding/exiting waited for blocked Shell parsing.");
            require(detailsProbe::modals.back().ended||!IsWindow(window),"Owned dialog survived Close/Hide/Exit/session end.");
            require(shellOperationBusy&&detailsProbe::fileThreadExits==0&&detailsProbe::fileTimerKills==1,"Pending cancellation released global work early or retained dialog polling.");
        };fixture.command(StatusDetails);
        const int focus=detailsProbe::focusCalls;detailsProbe::releaseFileParse=true;waitFileWorker();
        require(!shellOperationBusy&&detailsProbe::fileSelections==0&&!app.customDialog&&detailsProbe::focusCalls==focus,"Cancelled detached work selected files or touched a stale dialog/focus.");
        require(detailsProbe::records==0&&detailsProbe::finishes==(action==2?1:0),"Pending file request changed recording lifecycle.");files.assertClean(1);
    }
    std::cout<<"PASS blocked parsing keeps status ticks and Close/Hide/Exit/session shutdown responsive; cancellation suppresses late selection\n";
}
void fileReopenedAndKeyboard(){
    OwnedFiles files;Fixture fixture;fixture.publish(files.report());showOffscreen(fixture);
    detailsProbe::script=[&](HWND window){detailsProbe::blockFileParse=true;clickFiles(window);waitFiles([]{return detailsProbe::fileParseEntered.load();});closeDetails(window);};
    fixture.command(StatusDetails);
    auto report=files.report(false);fixture.publish(report);
    detailsProbe::script=[&](HWND window){
        const auto original=textOf(classChild(window,L"EDIT"));clickFiles(window);
        require(detailsProbe::fileThreadStarts==1&&IsWindowEnabled(GetDlgItem(window,StatusDetailsFiles)),"Reopened Details bypassed single global job slot or permanently disabled retry.");
        require(textOf(GetDlgItem(window,StatusDetailsFilesStatus)).find(L"Another file request")!=std::wstring::npos,"Detached-busy state lacked retry explanation.");
        const auto feedback=textOf(GetDlgItem(window,StatusDetailsFilesStatus));detailsProbe::releaseFileParse=true;waitFileWorker();require(detailsProbe::fileSelections==0&&!shellOperationBusy&&textOf(GetDlgItem(window,StatusDetailsFilesStatus))==feedback,"Old dialog's worker selected or changed newer feedback after cancellation.");
        SetWindowPos(window,nullptr,-30000,-30000,640,380,SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(window,SW_SHOWNOACTIVATE);
        HWND edit=classChild(window,L"EDIT"),button=GetDlgItem(window,StatusDetailsFiles),close=GetDlgItem(window,IDCANCEL);
        SetFocus(edit);MSG mnemonic{};mnemonic.hwnd=edit;mnemonic.message=WM_SYSCHAR;mnemonic.wParam=L'f';mnemonic.lParam=1L<<29;
        require(IsDialogMessageW(window,&mnemonic)!=FALSE,"Native Show files mnemonic was not routed.");
        completeFiles(window);require(detailsProbe::fileThreadStarts==2&&detailsProbe::selectedFiles==report.savedPaths,"Reopened dialog selected stale pair or native Alt+F did not reach the action.");
        require(textOf(edit)==original&&IsWindowEnabled(button),"Reopened completion changed text or action availability.");
        SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(edit),TRUE);MSG enter{};enter.hwnd=edit;enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
        const auto defaultId=SendMessageW(window,DM_GETDEFID,0,0),keyCode=SendMessageW(edit,WM_GETDLGCODE,VK_RETURN,reinterpret_cast<LPARAM>(&enter));const int priorFocus=GetDlgCtrlID(GetFocus());const BOOL handled=IsDialogMessageW(window,&enter);
        // The native multiline edit defers Return to its dialog. This owned
        // modal seam must perform the same message dispatch as DialogBox.
        MSG queued{};unsigned pumped=0;while(pumped<32&&PeekMessageW(&queued,window,0,0,PM_REMOVE)){if(!IsDialogMessageW(window,&queued)){TranslateMessage(&queued);DispatchMessageW(&queued);}++pumped;}
        std::cout<<"  Enter handled="<<handled<<" ended="<<detailsProbe::modals.back().ended<<" workers="<<detailsProbe::fileThreadStarts<<" priorFocus="<<priorFocus<<" focus="<<GetDlgCtrlID(GetFocus())<<" default="<<LOWORD(defaultId)<<" dlgCode="<<keyCode<<" pumped="<<pumped<<'\n';
        require(handled!=FALSE&&detailsProbe::modals.back().ended&&detailsProbe::fileThreadStarts==2,"Default Enter selected files instead of closing Details.");
        (void)close;
    };fixture.command(StatusDetails);files.assertClean(2);
    std::cout<<"PASS stale/reopened dialog single-job bound, retry after retirement, native Alt+F and safe default Enter\n";
}
}

int main(){try{
    detailsProbe::ownerThread=GetCurrentThreadId();
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};require(InitCommonControlsEx(&controls)!=FALSE,"Common controls unavailable.");
    exactSelectableReports();outcomeAndPrimaryPrecedence();keyboardAndCompactLayout();stableSnapshot();modalLifecycle();failuresAndUnchangedTicks();distinctMainMnemonics();cursorKeyboard();delayKeyboard();
    fileSnapshotSelection();fileEligibility();threeFileSelection();fileFailures();filePendingLifecycle();fileReopenedAndKeyboard();sharedFolderSlot();
    std::cout<<"All fifteen status/keyboard/file groups passed with inert engine, owned native windows and intercepted Shell selection.\n";return 0;
}catch(const std::exception& error){std::cerr<<"STATUS DETAILS FAILURE: "<<error.what()<<'\n';return 1;}}

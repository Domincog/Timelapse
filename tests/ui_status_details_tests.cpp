// Actual native controls and handlers, with an inert engine and owned windows.
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
#include <filesystem>
#include <exception>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>

namespace detailsProbe {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool failAllocation=false,countAllocations=false,failDialog=false,routeFailed=false;
size_t allocations=0;
int allocationFailures=0,dialogs=0,notices=0,records=0,finishes=0,pauses=0;
int statusQueries=0,configures=0,enumerations=0,textWrites=0,focusCalls=0,invalidFocus=0;
int trayCalls=0,shows=0,hides=0,foregrounds=0,quits=0,profileReads=0;
int folderCalls=0,shellCalls=0;
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
UINT_PTR WINAPI timer(HWND,UINT_PTR id,UINT,TIMERPROC){return id;}
BOOL WINAPI killTimer(HWND,UINT_PTR){return TRUE;}
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
HINSTANCE WINAPI shell(HWND,LPCWSTR verb,LPCWSTR file,LPCWSTR parameters,LPCWSTR directory,INT showMode){
    require(verb&&std::wcscmp(verb,L"open")==0&&file&&std::filesystem::path(file)==std::filesystem::current_path()&&!parameters&&!directory&&showMode==SW_SHOWNORMAL,"Unexpected shell invocation.");
    ++shellCalls;return reinterpret_cast<HINSTANCE>(33); // No Explorer process/window.
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
    void finish(){++detailsProbe::finishes;detailsProbe::current.state=State::Finishing;}
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
#define ShellExecuteW detailsProbe::shell
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
#undef ShellExecuteW

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
        app.engine.reset();app.settings={};app.status=detailsProbe::current={};
        app.window=app.mode=app.preview=nullptr;app.customDialog=nullptr;
        app.hiddenToTray=app.closeWhenDone=app.startupComplete=app.layingOut=false;
        app.trayRegistered=app.trayVersion4=app.trayStateValid=app.trayNoticeShown=false;
        app.scrollX=app.scrollY=0;app.settings.folder=L"C:\\Owned synthetic reports";app.preferences=L"inert";
        detailsProbe::failAllocation=detailsProbe::countAllocations=detailsProbe::failDialog=detailsProbe::routeFailed=false;
        detailsProbe::dialogs=detailsProbe::notices=detailsProbe::records=detailsProbe::finishes=detailsProbe::pauses=0;
        detailsProbe::folderCalls=detailsProbe::shellCalls=0;
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
    for(int dpi:{96,192,288}){
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
            HWND edit=classChild(window,L"EDIT"),close=GetDlgItem(window,IDCANCEL);require(edit&&close,"Details navigation controls missing.");
            RECT proposed{0,0,420,260};SendMessageW(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&proposed));
            SetWindowPos(window,nullptr,0,0,420,260,SWP_NOZORDER|SWP_NOACTIVATE);SendMessageW(window,WM_SIZE,0,0);
            RECT client{};GetClientRect(window,&client);for(HWND child:{edit,close}){const auto rect=childRect(window,child);require(rect.left>=0&&rect.top>=0&&rect.right<=client.right&&rect.bottom<=client.bottom&&rect.right>rect.left&&rect.bottom>rect.top,"Constrained Details edit/Close control clipped.");}
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(edit),TRUE);
            MSG next{};next.hwnd=edit;next.message=WM_KEYDOWN;next.wParam=VK_TAB;require(IsDialogMessageW(window,&next)!=FALSE&&GetFocus()==close,"Native Details Tab failed to reach Close.");
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
    for(int action:{0,1,2}){
        Fixture fixture;Status recording;recording.state=State::Recording;recording.error=true;recording.message=L"Synthetic optional preview diagnostic during recording.";fixture.publish(recording);
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
        if(action==1)require(app.closeWhenDone&&detailsProbe::notices==1,"Exit lost its single explicit finish confirmation.");
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
    for(int i=0;i<100;++i)fixture.tick();const auto tickAllocations=detailsProbe::allocations;detailsProbe::countAllocations=false;
    require(detailsProbe::statusQueries==queries+100&&detailsProbe::dialogs==dialogs&&detailsProbe::enumerations==sourceCalls&&detailsProbe::textWrites==0&&tickAllocations==copyAllocations,"Unchanged ticks added Details work beyond existing Status copies.");
    require(detailsProbe::records==0&&detailsProbe::finishes==0&&detailsProbe::invalidFocus==0,"Failure handling changed recording or focused dead UI.");
    std::cout<<"PASS snapshot/dialog failures preserve report; 100 unchanged ticks add no text, enumeration, modal or allocations beyond Status copies\n";
}
void cursorKeyboard(){
    Fixture fixture;showOffscreen(fixture);fixture.command(AdvancedToggle);
    require(IsWindowEnabled(app.captureCursor)&&ownVisible(app.captureCursor)&&SendMessageW(app.captureCursor,BM_GETCHECK,0,0)==BST_CHECKED,"Cursor checkbox did not start visible, editable and checked in Advanced.");
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
    for(State state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
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
        if(app.advancedExpanded!=expanded)fixture.command(AdvancedToggle);
        Status report;report.state=state;report.message=L"Saved synthetic part: owned-camera.mp4";report.savedPath=L"owned-camera.mp4";report.savedPaths={report.savedPath};fixture.publish(report);
        require(ownVisible(app.skipConfigure)==expanded,"Advanced visibility state is inconsistent.");expectReadOnly=state!=State::Idle;const auto settings=app.settings;const int configurations=detailsProbe::configures;
        std::vector<HWND> starts{app.openFolder,app.advanced,app.statusDetails};if(expanded)starts.push_back(app.skipConfigure);if(state==State::Idle)starts.push_back(app.folder);
        for(HWND start:starts){
            const int folders=detailsProbe::folderCalls,compressed=compressionDialogs,shells=detailsProbe::shellCalls,details=detailDialogs;
            nativeMnemonic(start,L'h');require(detailsProbe::folderCalls==folders+(state==State::Idle?1:0)&&compressionDialogs==compressed&&detailDialogs==details,"Alt+H failed to route only to the enabled Change action.");
            // IsDialogMessage also activates unique mnemonics of collapsed advanced buttons.
            nativeMnemonic(start,L'c');require(compressionDialogs==compressed+1&&detailsProbe::folderCalls==folders+(state==State::Idle?1:0)&&detailDialogs==details,"Alt+C changed folder or failed to invoke compression.");
            nativeMnemonic(start,L'o');require(detailsProbe::shellCalls==shells+1,"Alt+O no longer opens the configured folder through its existing command.");
            nativeMnemonic(start,L'i');require(detailDialogs==details+1,"Alt+I no longer invokes Details.");
        }
        require(detailsProbe::configures==configurations&&app.settings.folder==settings.folder&&app.settings.intervalMs==settings.intervalMs&&skipValues(app.settings.timeSkip)==skipValues(settings.timeSkip),"Canceled keyboard inspection changed accepted settings/configuration.");
    }
    require(detailsProbe::records==0&&detailsProbe::finishes==0&&detailsProbe::pauses==0&&detailsProbe::notices==0&&!app.customDialog,"Main shortcuts dispatched recording, notices or left a modal owner.");
    std::cout<<"PASS distinct native Alt+H/Alt+C plus preserved Alt+O/Alt+I from multiple controls, collapsed/expanded and idle/recording/paused locks\n";
}
}

int main(){try{
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};require(InitCommonControlsEx(&controls)!=FALSE,"Common controls unavailable.");
    exactSelectableReports();outcomeAndPrimaryPrecedence();keyboardAndCompactLayout();stableSnapshot();modalLifecycle();failuresAndUnchangedTicks();distinctMainMnemonics();cursorKeyboard();
    std::cout<<"All eight status/keyboard groups passed with inert engine and owned native windows.\n";return 0;
}catch(const std::exception& error){std::cerr<<"STATUS DETAILS FAILURE: "<<error.what()<<'\n';return 1;}}

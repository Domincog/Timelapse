// Actual UI handlers, synthetic source lists and inert recording engine.
// Own hidden controls only; no device enumeration, capture, user input or INI I/O.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
bool startupUnderTest=false;
int startupCOM=0, startupMedia=0, startupMediaStops=0, startupCOMStops=0;
int startupClasses=0, startupWindows=0, startupEngines=0, profileReads=0;
std::wstring startupMessage;
struct KnownFolderResult {
    std::wstring path;
    HRESULT result=S_OK;
    bool missing=false, provideBuffer=true;
};
KnownFolderResult videosResult, localDataResult;
std::vector<DWORD> knownFolderFlags;
std::vector<void*> knownFolderBuffers;
int knownFolderFrees=0;
HRESULT WINAPI fixtureKnownFolder(REFKNOWNFOLDERID id,DWORD flags,HANDLE,PWSTR* output){
    knownFolderFlags.push_back(flags);
    auto& value=IsEqualGUID(id,FOLDERID_Videos)?videosResult:localDataResult;
    *output=nullptr;
    if(value.missing && !(flags&KF_FLAG_DONT_VERIFY))return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    if(value.provideBuffer){
        *output=static_cast<PWSTR>(CoTaskMemAlloc((value.path.size()+1)*sizeof(wchar_t)));
        if(!*output)return E_OUTOFMEMORY;
        std::copy(value.path.c_str(),value.path.c_str()+value.path.size()+1,*output);
        knownFolderBuffers.push_back(*output);
    }
    return value.result;
}
void WINAPI fixtureTaskMemFree(void* memory){
    const auto found=std::find(knownFolderBuffers.begin(),knownFolderBuffers.end(),memory);
    if(found!=knownFolderBuffers.end()){knownFolderBuffers.erase(found);++knownFolderFrees;}
    CoTaskMemFree(memory);
}
HRESULT WINAPI fixtureCoInitialize(LPVOID,DWORD){++startupCOM;return S_OK;}
void WINAPI fixtureCoUninitialize(){++startupCOMStops;}
HRESULT WINAPI fixtureMFStartup(ULONG,DWORD){++startupMedia;return S_OK;}
HRESULT WINAPI fixtureMFShutdown(){++startupMediaStops;return S_OK;}
BOOL WINAPI fixtureInitControls(const INITCOMMONCONTROLSEX*){return TRUE;}
ATOM WINAPI fixtureRegisterClass(const WNDCLASSEXW*){++startupClasses;return 1;}
HWND WINAPI fixtureCreateWindow(DWORD ex,LPCWSTR cls,LPCWSTR title,DWORD style,int x,int y,int width,int height,
                               HWND parent,HMENU menu,HINSTANCE instance,LPVOID parameter){
    if(startupUnderTest){++startupWindows;return nullptr;}
    return CreateWindowExW(ex,cls,title,style,x,y,width,height,parent,menu,instance,parameter);
}
int WINAPI fixtureMessageBox(HWND,LPCWSTR message,LPCWSTR,UINT){startupMessage=message;return IDOK;}
LPWSTR WINAPI fixtureCommandLine(){static wchar_t command[]=L"Timelapse.exe";return command;}
UINT WINAPI fixtureSystemDpi(){return 96;}
BOOL WINAPI fixtureCursorPosition(LPPOINT point){*point={0,0};return TRUE;}
HMONITOR WINAPI fixtureMonitorFromPoint(POINT,DWORD){return reinterpret_cast<HMONITOR>(1);}
BOOL WINAPI fixtureMonitorInfo(HMONITOR,LPMONITORINFO info){info->rcMonitor=info->rcWork={0,0,1920,1080};return TRUE;}
BOOL WINAPI fixtureWriteProfile(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR){throw std::runtime_error("Unexpected real preferences save.");}
HANDLE WINAPI fixtureOpenMutex(DWORD,BOOL,LPCWSTR){SetLastError(ERROR_FILE_NOT_FOUND);return nullptr;}
HANDLE WINAPI fixtureCreateMutex(LPSECURITY_ATTRIBUTES,BOOL,LPCWSTR){SetLastError(ERROR_SUCCESS);return reinterpret_cast<HANDLE>(1);}
BOOL WINAPI fixtureCloseHandle(HANDLE){return TRUE;}
UINT WINAPI fixtureRegisterMessage(LPCWSTR){return 0xc123;}
}

namespace lapse {
std::vector<Monitor> listedMonitors;
std::vector<CameraDevice> listedCameras;
std::wstring listError;
Settings configured, recorded;
Status fixtureStatus;
int configurationCalls=0, recordCalls=0, refreshCalls=0;
class FixtureEngine {
public:
    FixtureEngine(){if(startupUnderTest)++startupEngines;}
    void configure(const Settings& value) { configured=value; ++configurationCalls; }
    void refreshSources() { ++refreshCalls; }
    void record() { recorded=configured; ++recordCalls; fixtureStatus.state=State::Starting; }
    void pause() {}
    void setPaused(bool) {}
    void finish() {}
    Status status() { return fixtureStatus; }
};
std::vector<Monitor> enumerateMonitors() { return listedMonitors; }
std::vector<CameraDevice> enumerateCameras(std::wstring& error) { error=listError; return listedCameras; }
int runCameraHost(const wchar_t*) { if(startupUnderTest)return -1; throw std::runtime_error("Unexpected application entry."); }
}
namespace {
bool overrideIndexes=false;
std::wstring savedLowDisk=L"1";
UINT savedInterval=0, savedSize=0, savedEncoding=0, savedEncodingMode=0, savedRecordingLimit=0;
UINT WINAPI fixtureProfileInt(LPCWSTR, LPCWSTR key, INT fallback, LPCWSTR) {
    ++profileReads;
    if(!overrideIndexes) return fallback;
    if(std::wcscmp(key,L"Interval")==0)return savedInterval;
    if(std::wcscmp(key,L"Quality")==0)return savedSize;
    if(std::wcscmp(key,L"EncodingQuality")==0)return savedEncoding;
    if(std::wcscmp(key,L"EncodingMode")==0)return savedEncodingMode;
    if(std::wcscmp(key,L"RecordingLimit")==0)return savedRecordingLimit;
    throw std::runtime_error("Unexpected persisted index.");
}
DWORD WINAPI fixtureProfileString(LPCWSTR,LPCWSTR key,LPCWSTR fallback,LPWSTR target,DWORD capacity,LPCWSTR) {
    ++profileReads;
    if(overrideIndexes && std::wcscmp(key,L"StopOnLowDiskSpace")==0)fallback=savedLowDisk.c_str();
    const auto count=std::min<size_t>(std::wcslen(fallback),capacity-1);
    std::wmemcpy(target,fallback,count); target[count]=L'\0'; return static_cast<DWORD>(count);
}
}
#define Engine FixtureEngine
#define GetPrivateProfileIntW fixtureProfileInt
#define GetPrivateProfileStringW fixtureProfileString
#define SHGetKnownFolderPath fixtureKnownFolder
#define CoTaskMemFree fixtureTaskMemFree
#define CoInitializeEx fixtureCoInitialize
#define CoUninitialize fixtureCoUninitialize
#define MFStartup fixtureMFStartup
#define MFShutdown fixtureMFShutdown
#define InitCommonControlsEx fixtureInitControls
#define RegisterClassExW fixtureRegisterClass
#define CreateWindowExW fixtureCreateWindow
#define MessageBoxW fixtureMessageBox
#define GetCommandLineW fixtureCommandLine
#define GetDpiForSystem fixtureSystemDpi
#define GetCursorPos fixtureCursorPosition
#define MonitorFromPoint fixtureMonitorFromPoint
#define GetMonitorInfoW fixtureMonitorInfo
#define WritePrivateProfileStringW fixtureWriteProfile
#define OpenMutexW fixtureOpenMutex
#define CreateMutexW fixtureCreateMutex
#define CloseHandle fixtureCloseHandle
#define RegisterWindowMessageW fixtureRegisterMessage
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#undef SHGetKnownFolderPath
#undef CoTaskMemFree
#undef CoInitializeEx
#undef CoUninitialize
#undef MFStartup
#undef MFShutdown
#undef InitCommonControlsEx
#undef RegisterClassExW
#undef CreateWindowExW
#undef MessageBoxW
#undef GetCommandLineW
#undef GetDpiForSystem
#undef GetCursorPos
#undef MonitorFromPoint
#undef GetMonitorInfoW
#undef WritePrivateProfileStringW
#undef OpenMutexW
#undef CreateMutexW
#undef CloseHandle
#undef RegisterWindowMessageW
#undef Engine
#undef GetPrivateProfileIntW
#undef GetPrivateProfileStringW

namespace {
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
bool same(const RECT& a,const RECT& b) { return EqualRect(&a,&b)!=FALSE; }
std::wstring caption(HWND box) {
    wchar_t text[200]{};
    GetWindowTextW(box,text,200); return text;
}
struct HiddenFixture {
    HiddenFixture() {
        app.dpi=96;app.visibleDirty=true;app.controlsUpdated=false;
        app.skipRevision=0;app.advancedSkipRevision=app.skipSummaryRevision=app.skipVisibility=-1;app.skipSummaryCaption.clear();app.skipDetailCaption.clear();app.skipCheckAge=UINT64_MAX;
        app.window=CreateWindowExW(0,L"STATIC",L"Selection review",WS_OVERLAPPED,0,0,920,720,nullptr,nullptr,nullptr,nullptr);
        require(app.window!=nullptr,"Hidden parent creation.");
        auto child=[&](const wchar_t* cls,DWORD style) {
            auto handle=CreateWindowExW(0,cls,L"",WS_CHILD|style,0,0,100,100,app.window,nullptr,nullptr,nullptr);
            require(handle!=nullptr,"Hidden child creation."); return handle;
        };
        auto combo=[&](int count) {
            auto handle=child(L"COMBOBOX",CBS_DROPDOWNLIST);
            for(int i=0;i<count;++i)add(handle,std::to_wstring(i)); choose(handle,0); return handle;
        };
        app.mode=combo(6); app.interval=combo(6); app.videoSize=combo(2); app.encodingQuality=combo(3); app.encodingMode=combo(5);app.stopAfter=combo(6);
        app.lowDisk=child(L"BUTTON",BS_AUTOCHECKBOX);SendMessageW(app.lowDisk,BM_SETCHECK,BST_CHECKED,0);
        app.recoveryMode=child(L"BUTTON",BS_AUTOCHECKBOX);
        app.nightEnabled=child(L"BUTTON",BS_AUTOCHECKBOX);app.nightDuration=combo(6);app.nightTarget=combo(3);choose(app.nightTarget,1);
        app.monitor=combo(0); app.camera=combo(0);
        app.skipConfigure=child(L"BUTTON",BS_PUSHBUTTON);app.skipSummary=child(L"STATIC",0);app.skipDetail=child(L"STATIC",0);
        app.preview=child(L"STATIC",0);
        app.statusText=child(L"STATIC",0);
        for(auto target:{&app.refresh,&app.record,&app.pause,&app.finish,&app.folder,&app.openFolder,&app.reset,&app.forward})
            *target=child(L"BUTTON",BS_PUSHBUTTON);
        app.engine=std::make_unique<lapse::FixtureEngine>();
    }
    ~HiddenFixture() { app.engine.reset(); DestroyWindow(app.window); app.window=app.preview=nullptr; }
};
const lapse::Monitor displayA{L"Display A",{0,0,640,360},L"display-a"}, displayB{L"Display B",{640,0,1280,360},L"display-b"};
const lapse::CameraDevice cameraA{L"Camera A",L"camera-a"}, cameraB{L"Camera B",L"camera-b"};
void sourceMode(Mode mode) {
    choose(app.mode,static_cast<int>(mode));
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(ModeBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.mode));
}
void seed(bool camera) {
    app.status=lapse::fixtureStatus={}; app.settings={}; app.selectedMonitorId.clear();
    lapse::listedMonitors={displayA,displayB}; lapse::listedCameras={cameraA,cameraB}; lapse::listError.clear();
    SendMessageW(app.monitor,CB_RESETCONTENT,0,0); SendMessageW(app.camera,CB_RESETCONTENT,0,0);
    refreshSources(); sourceMode(camera?Mode::Camera:Mode::Desktop);
    choose(camera?app.camera:app.monitor,1); configure(); updateControls();
    lapse::configurationCalls=lapse::recordCalls=lapse::refreshCalls=0;
}
void refresh() {
    require(IsWindowEnabled(app.refresh)!=FALSE,"Refresh was not user-reachable.");
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(Refresh,BN_CLICKED),reinterpret_cast<LPARAM>(app.refresh));
}
void record() {
    require(IsWindowEnabled(app.record)!=FALSE,"Record was not enabled.");
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(Record,BN_CLICKED),reinterpret_cast<LPARAM>(app.record));
}
std::wstring selectedId(bool camera) { return camera?app.settings.cameraId:app.selectedMonitorId; }
void removeSelected(bool camera) {
    // Friendly labels deliberately collide; matching must use device identity.
    if(camera) { auto replacement=cameraA; replacement.name=cameraB.name; lapse::listedCameras={replacement}; }
    else { auto replacement=displayA; replacement.name=displayB.name; lapse::listedMonitors={replacement}; }
}
void restoreSelected(bool camera, bool first) {
    if(camera)lapse::listedCameras=first?std::vector<lapse::CameraDevice>{cameraB,cameraA}:std::vector<lapse::CameraDevice>{cameraA,cameraB};
    else lapse::listedMonitors=first?std::vector<lapse::Monitor>{displayB,displayA}:std::vector<lapse::Monitor>{displayA,displayB};
}
void unavailableSelection(bool camera) {
    seed(camera); const auto original=selectedId(camera); const HWND box=camera?app.camera:app.monitor;
    removeSelected(camera); refresh();
    for(int attempt=0;attempt<2;++attempt) {
        require(selectedId(camera)==original&&choice(box)==1&&SendMessageW(box,CB_GETCOUNT,0,0)==2,
                "Refresh replaced or forgot the unavailable selected device.");
        require(caption(box)==(camera?L"Selected camera unavailable":L"Selected display unavailable")&&!IsWindowEnabled(app.record),
                "Unavailable source was not displayed or Record remained enabled.");
        if(camera)require(lapse::configured.cameraId==L"camera-b","Preview switched to a replacement camera.");
        else {
            require(same(app.settings.monitor,{})&&same(lapse::configured.monitor,{}),"Unavailable display retained capture bounds.");
            require(app.settings.monitorId==original&&lapse::configured.monitorId==original,
                    "Unavailable display identity was lost or replaced in engine settings.");
        }
        // Test the command guard independently of disabled button delivery.
        windowProc(app.window,WM_COMMAND,MAKEWPARAM(Record,BN_CLICKED),reinterpret_cast<LPARAM>(app.record));
        require(lapse::recordCalls==0,"A missing-source Record command reached the engine.");
        refresh();
    }
    restoreSelected(camera,true); refresh();
    require(choice(box)==0&&selectedId(camera)==original&&IsWindowEnabled(app.record)&&SendMessageW(box,CB_GETCOUNT,0,0)==2,
            "Reconnected selected device did not recover without a placeholder.");
    restoreSelected(camera,false); refresh();
    require(choice(box)==1&&selectedId(camera)==original&&IsWindowEnabled(app.record),"Enumeration reorder changed selected identity.");
    record();
    require(lapse::recordCalls==1&&(camera?lapse::recorded.cameraId==L"camera-b":
            lapse::recorded.monitorId==displayB.id&&same(lapse::recorded.monitor,displayB.bounds)),
            "Recovered Record command used the wrong source.");
    std::cout<<"PASS "<<(camera?"camera":"display")<<" disappearance, command guard, reconnect and reorder\n";
}
void emptyListRecovery(bool camera) {
    seed(camera); const auto original=selectedId(camera);
    if(camera){lapse::listedCameras.clear();lapse::listError=L"Synthetic enumeration error.";}
    else lapse::listedMonitors.clear();
    refresh();
    require(selectedId(camera)==original&&!IsWindowEnabled(app.record),"Empty/error list lost selected identity.");
    if(camera)require(caption(app.camera)==L"Camera list unavailable","Enumeration error lost unavailable caption.");
    else require(app.settings.monitorId==original&&lapse::configured.monitorId==original,
                 "Empty display list lost the engine's selected identity.");
    restoreSelected(camera,false);lapse::listError.clear();refresh();
    require(selectedId(camera)==original&&choice(camera?app.camera:app.monitor)==1&&IsWindowEnabled(app.record),
            "Selected device did not recover after empty/error list.");
    std::cout<<"PASS "<<(camera?"camera":"display")<<" empty/error list retains choice\n";
}
void explicitReplacement(bool camera) {
    seed(camera);removeSelected(camera);refresh();
    const HWND box=camera?app.camera:app.monitor;
    const int before=lapse::configurationCalls;
    choose(box,0);
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(camera?CameraBox:MonitorBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(box));
    require(selectedId(camera)==(camera?L"camera-a":L"display-a")&&SendMessageW(box,CB_GETCOUNT,0,0)==1&&IsWindowEnabled(app.record),
            "Explicit device selection did not remove placeholder and enable Record immediately.");
    require(lapse::configurationCalls>before&&(camera?lapse::configured.cameraId==L"camera-a":
            app.settings.monitorId==displayA.id&&lapse::configured.monitorId==displayA.id&&same(lapse::configured.monitor,displayA.bounds)),
            "Explicit device selection did not configure preview immediately.");
    restoreSelected(camera,true);refresh();
    require(choice(box)==1&&selectedId(camera)==(camera?L"camera-a":L"display-a"),"A returning old device replaced the explicit choice.");
    std::cout<<"PASS "<<(camera?"camera":"display")<<" explicit replacement is immediate and retained\n";
}
void modeSwitchPreservesMissingChoice(bool camera) {
    seed(camera);const auto original=selectedId(camera);
    sourceMode(camera?Mode::Desktop:Mode::Camera);removeSelected(camera);refresh();
    require(selectedId(camera)==original&&IsWindowEnabled(app.record),"Irrelevant missing device blocked an available source or lost its identity.");
    sourceMode(camera?Mode::Camera:Mode::Desktop);
    require(selectedId(camera)==original&&!IsWindowEnabled(app.record),"Switching back silently substituted the missing source.");
    std::cout<<"PASS "<<(camera?"camera":"display")<<" missing identity survives mode switches\n";
}
void changedMetadata(bool camera) {
    seed(camera);
    if(camera)lapse::listedCameras={{L"Renamed camera B",L"CAMERA-B"},cameraA};
    else lapse::listedMonitors={{L"Display B (primary)",{-800,10,0,610},L"DISPLAY-B"},displayA};
    refresh();const HWND box=camera?app.camera:app.monitor;
    require(choice(box)==0&&selectedId(camera)==(camera?L"CAMERA-B":L"DISPLAY-B")&&IsWindowEnabled(app.record),
            "Case/label changes lost a device with the same identity.");
    if(camera)require(caption(box)==L"Renamed camera B"&&lapse::configured.cameraId==L"CAMERA-B","Camera metadata was not updated.");
    else require(caption(box)==L"Display B (primary)"&&lapse::configured.monitorId==L"DISPLAY-B"&&same(lapse::configured.monitor,{-800,10,0,610}),
                 "Display identity retained obsolete geometry, label or engine identity.");
    std::cout<<"PASS "<<(camera?"camera":"display")<<" same identity updates case, label and geometry\n";
}
void initialDefaults() {
    app.status=lapse::fixtureStatus={};app.settings={};app.selectedMonitorId.clear();
    lapse::listedMonitors.clear();lapse::listedCameras.clear();lapse::listError.clear();
    refreshSources();sourceMode(Mode::Desktop);
    require(app.selectedMonitorId.empty()&&app.settings.monitorId.empty()&&lapse::configured.monitorId.empty()&&
            app.settings.cameraId.empty()&&!IsWindowEnabled(app.record),"Empty initial list made a selection.");
    lapse::listedMonitors={displayA,displayB};lapse::listedCameras={cameraA,cameraB};refresh();
    require(app.selectedMonitorId==L"display-a"&&app.settings.monitorId==displayA.id&&lapse::configured.monitorId==displayA.id&&
            app.settings.cameraId==L"camera-a"&&choice(app.monitor)==0&&choice(app.camera)==0&&IsWindowEnabled(app.record),
            "First available devices were not selected before any prior choice.");
    std::cout<<"PASS initial first-available defaults after initially empty lists\n";
}
void activeControls() {
    seed(true);
    for(auto state:{State::Starting,State::Recording,State::Paused,State::Finishing}) {
        app.status.state=state; updateControls();
        for(auto control:{app.refresh,app.mode,app.monitor,app.camera,app.interval,app.videoSize,app.encodingQuality,app.encodingMode,app.stopAfter,app.lowDisk,app.nightEnabled,app.nightDuration,app.nightTarget,app.folder,app.record})
            require(!IsWindowEnabled(control),"An active-session source/settings control remained enabled.");
    }
    std::cout<<"PASS Refresh/source/settings controls disabled in Starting, Recording, Paused and Finishing.\n";
}
void resetKnownFolders(){
    require(knownFolderBuffers.empty(),"A previous known-folder lookup leaked its returned buffer.");
    videosResult={L"C:\\Synthetic profile\\Videos",S_OK};
    localDataResult={L"C:\\Synthetic profile\\AppData\\Local",S_OK};
    knownFolderFlags.clear();knownFolderFrees=0;
}
void startupPathGuard(){
    for(bool failVideos:{true,false}){
        resetKnownFolders();
        (failVideos?videosResult:localDataResult)={L"",E_ACCESSDENIED,false,false};
        app.settings.folder=L"unchanged recording folder";app.preferences=L"unchanged preferences";
        startupCOM=startupMedia=startupMediaStops=startupCOMStops=0;
        startupClasses=startupWindows=startupEngines=profileReads=0;startupMessage.clear();
        startupUnderTest=true;wchar_t command[]=L"";
        const int result=wWinMain(GetModuleHandleW(nullptr),nullptr,command,SW_HIDE);
        startupUnderTest=false;
        std::wcout<<L"startup failure "<<(failVideos?L"Videos":L"LocalAppData")<<L": result="<<result
            <<L" windows="<<startupWindows<<L" classes="<<startupClasses<<L" folder="<<app.settings.folder
            <<L" preferences="<<app.preferences<<L"\n";
        require(result==1 && startupWindows==0 && startupClasses==0 && startupEngines==0 && profileReads==0,
                "Failed known folder reached window/engine/preferences startup.");
        require(startupCOM==1 && startupMedia==1 && startupMediaStops==1 && startupCOMStops==1,
                "Default-path rejection did not balance successful COM/media startup.");
        require(startupMessage.find(L"Windows user profile")!=std::wstring::npos,
                "Default-path rejection did not show an actionable startup diagnostic.");
        require(app.settings.folder==L"unchanged recording folder" && app.preferences==L"unchanged preferences",
                "Startup rejection partially replaced path outputs.");
        require(knownFolderBuffers.empty(),"Startup rejection leaked a known-folder result.");
    }
    std::cout<<"PASS actual startup rejects unresolved profile paths before windows/preferences and balances COM/media.\n";
}
#ifndef BASELINE_PATH_REVIEW
void defaultPathComposition(){
    const std::pair<std::wstring,std::wstring> configuredPaths[]={
        {L"C:\\Absent profile\\Videos",L"C:\\Absent profile\\Local"},
        {L"D:\\Redirected videos",L"E:\\Redirected local data"},
        {L"\\\\synthetic-server\\videos",L"\\\\synthetic-server\\profiles\\local"}
    };
    for(const auto& paths:configuredPaths){
        resetKnownFolders();videosResult={paths.first,S_OK,true};localDataResult={paths.second,S_OK,true};
        std::wstring folder=L"old folder",preferencesPath=L"old settings";
        require(defaultPaths(folder,preferencesPath),"Configured missing absolute profile locations were rejected.");
        require(folder==(std::filesystem::path(paths.first)/L"Timelapse").wstring() &&
                preferencesPath==(std::filesystem::path(paths.second)/L"Timelapse"/L"settings.ini").wstring(),
                "Configured missing/redirected paths were replaced.");
        require(knownFolderFlags.size()==2 && std::all_of(knownFolderFlags.begin(),knownFolderFlags.end(),
                    [](DWORD flags){return flags==KF_FLAG_DONT_VERIFY;}),
                "Lookup verified existence or overrode the configured location.");
        require(knownFolderBuffers.empty() && knownFolderFrees==2,"Successful lookup did not release both COM buffers.");
    }
    const KnownFolderResult invalid[]={
        {L"",S_OK},{L"relative",S_OK},{L"C:drive-relative",S_OK},{L"\\root-relative",S_OK},
        {L"C:\\Returned on failure",E_FAIL},{L"",E_FAIL,false,false},{L"",S_OK,false,false}
    };
    for(bool invalidVideos:{true,false})for(const auto& value:invalid){
        resetKnownFolders();(invalidVideos?videosResult:localDataResult)=value;
        std::wstring folder=L"original folder",preferencesPath=L"original preferences";
        require(!defaultPaths(folder,preferencesPath),"Invalid or unresolved profile location became a usable default.");
        require(folder==L"original folder" && preferencesPath==L"original preferences","Rejected roots partially changed outputs.");
        require(knownFolderBuffers.empty() && knownFolderFrees==(value.provideBuffer?2:1),"Rejected lookup leaked a returned COM buffer.");
    }
    std::cout<<"PASS missing/redirected/UNC defaults and invalid/relative/null/failed result rejection with unchanged outputs.\n";
}
void missingPathsReachStartup(){
    resetKnownFolders();videosResult.missing=localDataResult.missing=true;
    startupCOM=startupMedia=startupMediaStops=startupCOMStops=0;
    startupClasses=startupWindows=startupEngines=profileReads=0;startupMessage.clear();
    startupUnderTest=true;wchar_t command[]=L"";
    const int result=wWinMain(GetModuleHandleW(nullptr),nullptr,command,SW_HIDE);
    startupUnderTest=false;
    // The seam deliberately refuses window creation after the validated path gate.
    require(result==1 && startupClasses==2 && startupWindows==1 && startupEngines==0 && profileReads==0,
            "Missing configured folders could not reach the normal window-creation step.");
    require(startupMessage==L"Timelapse could not open its window. Close other applications, then try again.",
            "Refused fixture window did not show the fixed window-creation diagnostic.");
    require(std::filesystem::path(app.settings.folder).is_absolute() &&
            std::filesystem::path(app.preferences).is_absolute(),"Missing-folder startup produced a relative path.");
    require(startupCOM==1 && startupMedia==1 && startupMediaStops==1 && startupCOMStops==1,
            "Refused fixture window did not balance successful COM/media startup.");
    require(knownFolderBuffers.empty(),"Missing-folder startup leaked a known-folder buffer.");
    std::cout<<"PASS configured nonexistent folders reach validated startup; owned seam stops before creating a window.\n";
}
#endif

void indexLoads() {
    struct Input { UINT interval,size,quality; int seconds,width; EncodingQuality expected; };
    const Input cases[]={{5,1,2,60,1920,EncodingQuality::Detail},{999,999,999,60,1920,EncodingQuality::Detail},
        {static_cast<UINT>(-1),static_cast<UINT>(-1),static_cast<UINT>(-1),1,1280,EncodingQuality::Compact}};
    seed(false);
    overrideIndexes=false; preferences(false); configure();
    require(app.settings.intervalMs==5000&&app.settings.width==1280&&app.settings.encodingQuality==EncodingQuality::Balanced&&choice(app.mode)==0,
            "Default persisted option mapping failed.");
    for(const auto& input:cases) {
        overrideIndexes=true; savedInterval=input.interval; savedSize=input.size; savedEncoding=input.quality;
        preferences(false); configure();
        require(app.settings.intervalMs==input.seconds*1000&&app.settings.width==input.width&&app.settings.encodingQuality==input.expected&&choice(app.mode)==0,
                "Persisted option index mapping/clamping failed.");
    }
    for(UINT mode:{0u,1u,2u,3u,4u,5u,999u,static_cast<UINT>(-1)}){
        overrideIndexes=true;savedEncodingMode=mode;preferences(false);configure();
        require(app.settings.encodingMode==static_cast<EncodingMode>(mode<=4?mode:0),"Persisted encoding mode mapping failed.");
    }
    savedEncodingMode=0;
    const int seconds[]={0,900,3600,14400,28800,86400};
    for(UINT limit:{0u,1u,2u,3u,4u,5u,6u,999u,static_cast<UINT>(-1)}){
        savedRecordingLimit=limit;preferences(false);configure();
        require(app.settings.recordingLimitSeconds==seconds[limit<=5?limit:0]&&lapse::configured.recordingLimitSeconds==app.settings.recordingLimitSeconds,"Persisted recording-limit mapping failed.");
    }
    savedRecordingLimit=0;
    for(const wchar_t* value:{L"0",L"1",L"",L"-1",L"2",L"false",L"0junk"}){
        savedLowDisk=value;preferences(false);configure();
        require(app.settings.stopOnLowDiskSpace==(savedLowDisk!=L"0")&&lapse::configured.stopOnLowDiskSpace==app.settings.stopOnLowDiskSpace,"Persisted low disk protection validation failed.");
    }
    savedLowDisk=L"1";
    overrideIndexes=false; std::cout<<"PASS persisted defaults, valid indexes, high clamp and negative clamp; startup mode remains Desktop.\n";
}
void separateSources() {
    seed(false);choose(app.mode,SeparateFilesMode);changeLayout(false);
    require(app.settings.separateFiles&&lapse::configured.separateFiles&&hasSource(Source::Desktop)&&hasSource(Source::Camera),"Separate-file source mode did not configure both sources");
    require(IsWindowEnabled(app.monitor)&&IsWindowEnabled(app.camera)&&IsWindowEnabled(app.record)&&!IsWindowEnabled(app.reset)&&!IsWindowEnabled(app.forward),"Separate-file source controls are incorrect");
    const auto layers=app.settings.layers;app.selected=0;
    windowProc(app.window,WM_COMMAND,Reset,0);windowProc(app.window,WM_COMMAND,Forward,0);
    previewProc(app.preview,WM_KEYDOWN,VK_SPACE,0);previewProc(app.preview,WM_KEYDOWN,VK_RIGHT,0);
    previewProc(app.preview,WM_LBUTTONDOWN,0,0);
    require(app.settings.separateFiles&&app.modeIndex==SeparateFilesMode&&app.selected==0&&!app.dragging&&app.settings.layers[0].rect.x==layers[0].rect.x,"Separate-file preview accepted collage editing");
    lapse::listedCameras.clear();refresh();require(!IsWindowEnabled(app.record),"Separate recording accepted missing camera");
    lapse::listedCameras={cameraA};refresh();lapse::listedMonitors.clear();refresh();require(!IsWindowEnabled(app.record),"Separate recording accepted missing display");
    lapse::listedMonitors={displayA,displayB};refresh();record();require(lapse::recorded.separateFiles&&lapse::recordCalls==1,"Record lost separate-file mode");
    lapse::fixtureStatus=app.status={};sourceMode(Mode::Desktop);require(!app.settings.separateFiles,"Returning to desktop retained separate-file mode");
    std::cout<<"PASS separate-file mode requires both sources and prevents collage edits.\n";
}
void nightSettings(){
    seed(true);choose(app.interval,2);choose(app.nightDuration,0);choose(app.nightTarget,1);
    SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,NightBox,0);
    require(app.settings.night.enabled&&lapse::configured.night.enabled&&app.settings.night.durationMs==0&&app.settings.night.targetBrightness==96&&IsWindowEnabled(app.record),"Night opt-in lost Auto/Balanced settings.");
    for(int target=0;target<3;++target){choose(app.nightTarget,target);windowProc(app.window,WM_COMMAND,MAKEWPARAM(NightTargetBox,CBN_SELCHANGE),0);require(lapse::configured.night.targetBrightness==NightTargets[target],"Night target mapping failed.");}
    choose(app.nightDuration,5);windowProc(app.window,WM_COMMAND,MAKEWPARAM(NightDurationBox,CBN_SELCHANGE),0);
    require(app.settings.night.durationMs==30000&&!app.nightValidation.empty()&&!IsWindowEnabled(app.record)&&caption(app.statusText).find(L"Capture every")!=std::wstring::npos,"Invalid blend was silently shortened or failed to show validation.");
    const auto records=lapse::recordCalls;windowProc(app.window,WM_COMMAND,Record,0);require(lapse::recordCalls==records,"Record admitted a blend longer than the capture interval.");
    lapse::fixtureStatus.error=true;lapse::fixtureStatus.recordingFailed=true;lapse::fixtureStatus.message=L"Synthetic camera failure";windowProc(app.window,WM_TIMER,1,0);
    require(caption(app.statusText)==lapse::fixtureStatus.message,"Night validation hid a capture failure.");
    lapse::fixtureStatus.error=false;lapse::fixtureStatus.message=L"Ready to record.";windowProc(app.window,WM_TIMER,1,0);require(caption(app.statusText)==app.nightValidation,"A prior failed recording hid idle validation after its current error was acknowledged.");
    choose(app.interval,4);windowProc(app.window,WM_COMMAND,MAKEWPARAM(IntervalBox,CBN_SELCHANGE),0);
    require(app.nightValidation.empty()&&IsWindowEnabled(app.record)&&app.settings.night.durationMs==30000,"Equal duration/interval was rejected or coerced.");
    record();require(lapse::recorded.night.enabled&&lapse::recorded.night.durationMs==30000&&lapse::recorded.night.targetBrightness==128,"Record did not freeze the chosen night policy.");
    for(State state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;updateControls();const int calls=lapse::configurationCalls;
        require(!IsWindowEnabled(app.nightEnabled)&&!IsWindowEnabled(app.nightDuration)&&!IsWindowEnabled(app.nightTarget),"Active session allowed night edits.");
        windowProc(app.window,WM_COMMAND,NightBox,0);windowProc(app.window,WM_COMMAND,MAKEWPARAM(NightDurationBox,CBN_SELCHANGE),0);windowProc(app.window,WM_COMMAND,MAKEWPARAM(NightTargetBox,CBN_SELCHANGE),0);
        require(lapse::configurationCalls==calls,"Disabled night commands still configured active session.");
    }
    app.status=lapse::fixtureStatus={};sourceMode(Mode::Desktop);
    require(!app.settings.night.enabled&&SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_CHECKED&&IsWindowEnabled(app.record),"Desktop source applied camera processing or lost the saved camera preference.");
    sourceMode(Mode::Camera);require(app.settings.night.enabled,"Camera source did not restore night preference.");
    choose(app.mode,SeparateFilesMode);changeLayout(false);require(lapse::configured.night.enabled&&lapse::configured.separateFiles,"Separate output mode lost camera night settings.");
    SendMessageW(app.nightEnabled,BM_SETCHECK,BST_UNCHECKED,0);windowProc(app.window,WM_COMMAND,NightBox,0);sourceMode(Mode::Desktop);
    std::cout<<"PASS night Auto/manual/target mapping, equality, validation/error priority, active locks and camera-only scope\n";
}
void recoverySettings(){
    seed(false);choose(app.encodingMode,0);SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);
    windowProc(app.window,WM_COMMAND,RecoveryBox,0);
    require(!app.settings.recoveryMode&&app.encodingValidation.empty()&&IsWindowEnabled(app.record),"Recovery default changed ordinary recording.");
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,RecoveryBox,0);
    for(int mode:{0,1,2,4}){
        choose(app.encodingMode,mode);windowProc(app.window,WM_COMMAND,MAKEWPARAM(EncodingModeBox,CBN_SELCHANGE),0);
        require(app.settings.recoveryMode&&lapse::configured.recoveryMode&&app.encodingValidation.empty()&&IsWindowEnabled(app.record),"An H.264 mode was rejected or silently disabled recovery.");
    }
    choose(app.encodingMode,3);windowProc(app.window,WM_COMMAND,MAKEWPARAM(EncodingModeBox,CBN_SELCHANGE),0);
    require(app.settings.recoveryMode&&app.settings.encodingMode==EncodingMode::HardwareHEVC&&!app.encodingValidation.empty()&&!IsWindowEnabled(app.record)&&caption(app.statusText)==app.encodingValidation,
        "HEVC/recovery incompatibility was hidden or a chosen option was coerced.");
    const auto calls=lapse::recordCalls;windowProc(app.window,WM_COMMAND,Record,0);require(lapse::recordCalls==calls,"Forged Record admitted HEVC recovery.");
    lapse::fixtureStatus.error=true;lapse::fixtureStatus.recordingFailed=true;lapse::fixtureStatus.message=L"Synthetic save failed; retained owned file";
    windowProc(app.window,WM_TIMER,1,0);require(caption(app.statusText)==lapse::fixtureStatus.message,"Encoding validation hid a save failure.");
    app.status=lapse::fixtureStatus={};updateControls();
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);windowProc(app.window,WM_COMMAND,RecoveryBox,0);
    require(app.settings.encodingMode==EncodingMode::HardwareHEVC&&app.encodingValidation.empty()&&IsWindowEnabled(app.record),"Turning recovery off did not restore HEVC eligibility.");
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,RecoveryBox,0);
    choose(app.encodingMode,0);windowProc(app.window,WM_COMMAND,MAKEWPARAM(EncodingModeBox,CBN_SELCHANGE),0);
    choose(app.mode,SeparateFilesMode);changeLayout(false);choose(app.interval,2);choose(app.nightDuration,2);
    SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,NightBox,0);record();
    require(lapse::recorded.recoveryMode&&lapse::recorded.separateFiles&&lapse::recorded.night.enabled&&lapse::recorded.night.durationMs==2000,
        "Accepted paired/Night recording lost recovery or its independent settings.");
    for(State state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;updateControls();const auto configurations=lapse::configurationCalls;
        require(!IsWindowEnabled(app.recoveryMode)&&!IsWindowEnabled(app.encodingMode),"Active session exposed encoding/recovery edits.");
        SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);windowProc(app.window,WM_COMMAND,RecoveryBox,0);
        choose(app.encodingMode,3);windowProc(app.window,WM_COMMAND,MAKEWPARAM(EncodingModeBox,CBN_SELCHANGE),0);
        require(lapse::configurationCalls==configurations&&SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==BST_CHECKED&&choice(app.encodingMode)==0,
            "Forged active recovery/encoder command changed the session or displayed a false selection.");
    }
    app.status=lapse::fixtureStatus={};SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);SendMessageW(app.nightEnabled,BM_SETCHECK,BST_UNCHECKED,0);sourceMode(Mode::Desktop);
    std::cout<<"PASS recovery H.264 modes, HEVC validation/error priority, actual Record guard, paired/Night settings and active command locks\n";
}
}
int main() {
    std::cout<<std::unitbuf; std::wcout<<std::unitbuf;
    try {
        startupPathGuard();
#ifdef BASELINE_PATH_REVIEW
        return 0;
#else
        defaultPathComposition();missingPathsReachStartup();
#endif
        HiddenFixture fixture;
        initialDefaults();
        for(bool camera:{false,true}) {
            unavailableSelection(camera);emptyListRecovery(camera);explicitReplacement(camera);
            modeSwitchPreservesMissingChoice(camera);changedMetadata(camera);
        }
        activeControls();indexLoads();separateSources();nightSettings();recoverySettings();
        require(!IsWindowVisible(app.window)&&!IsWindowVisible(app.preview),"Fixture became visible.");
        std::cout<<"All source selection and settings assertions passed with hidden controls. No actual source was opened.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"PROBE FAILURE: "<<error.what()<<'\n';return 1;}
}


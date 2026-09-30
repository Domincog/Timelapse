// Actual callback cases with synthetic controls/Engine and owned preferences.
// Seven prior startup cases plus an independent required-child and optional-tooltip matrix.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <objbase.h>
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>
namespace {
enum class Fault { None, ProfileCopy, AfterEngine, Timer };
Fault fault=Fault::None;
bool failNextAllocation=false;
int failedAllocations=0,windows=0,fontsCreated=0,profileReads=0,keyWrites=0;
int engineStarts=0,engineStops=0,engineConfigures=0,syntheticThrows=0,choiceQueries=0;
int timerStarts=0,timerStops=0,quits=0,debugMessages=0;
size_t failedBytes=0;
int failedChild=-1,creationFailures=0,nullMessages=0,recordCalls=0,lastConfiguredInterval=0,lastRecordedInterval=0;
int monitorEnumerations=0,cameraEnumerations=0;
bool failTooltip=false;int tooltipAttempts=0;
void resetChildFaults(){
    failedChild=-1;failTooltip=false;
    creationFailures=nullMessages=recordCalls=lastConfiguredInterval=lastRecordedInterval=monitorEnumerations=cameraEnumerations=tooltipAttempts=0;
}
std::array<int,256> selections{};
std::array<int,256> counts{};
std::filesystem::path ownedRun,ownedCase;
void require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
int index(HWND hwnd){auto value=reinterpret_cast<INT_PTR>(hwnd);require(value>=0&&value<256,"Unexpected fake handle");return static_cast<int>(value);}
HBRUSH WINAPI fakeBrush(COLORREF){return reinterpret_cast<HBRUSH>(1);}
BOOL WINAPI fakeDelete(HGDIOBJ){return TRUE;}
HFONT WINAPI fakeFont(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCWSTR){return reinterpret_cast<HFONT>(static_cast<INT_PTR>(20+(++fontsCreated)));}
BOOL WINAPI fakeChildren(HWND,WNDENUMPROC,LPARAM){return TRUE;}
UINT WINAPI fakeDpi(HWND){return 96;}
HWND WINAPI fakeWindow(DWORD,LPCWSTR cls,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU menu,HINSTANCE,LPVOID){
    ++windows;const bool tooltip=!menu&&std::wcscmp(cls,TOOLTIPS_CLASSW)==0;
    if(tooltip)++tooltipAttempts;
    if((menu&&reinterpret_cast<INT_PTR>(menu)==failedChild)||(tooltip&&failTooltip)){++creationFailures;return nullptr;}
    return reinterpret_cast<HWND>(static_cast<INT_PTR>(100+windows));
}
LRESULT WINAPI fakeSend(HWND hwnd,UINT message,WPARAM value,LPARAM parameter){
    // No real recipient exists; retain Win32 NULL-handle results for omitted controls.
    if(!hwnd){++nullMessages;return ::SendMessageW(nullptr,message,value,parameter);}
    const int i=index(hwnd);
    if(message==CB_GETCURSEL){++choiceQueries;return selections[i];}
    if(message==CB_SETCURSEL){selections[i]=static_cast<int>(value);return static_cast<LRESULT>(value);}
    if(message==CB_ADDSTRING)return counts[i]++;
    if(message==CB_RESETCONTENT){counts[i]=0;selections[i]=-1;}
    return 0;
}
DWORD WINAPI fakeProfileString(LPCWSTR section,LPCWSTR key,LPCWSTR fallback,LPWSTR output,DWORD count,LPCWSTR path){
    if(std::wcscmp(key,L"StopOnLowDiskSpace")==0 || std::wcscmp(key,L"NightEnabled")==0 || std::wcscmp(key,L"NightDurationMs")==0 || std::wcscmp(key,L"NightTargetBrightness")==0){require(count>std::wcslen(fallback),"Option buffer too small");wcscpy_s(output,count,fallback);return static_cast<DWORD>(std::wcslen(fallback));}
    require(std::wcscmp(section,L"Settings")==0&&std::wcscmp(key,L"Folder")==0&&count>=1025&&path&&*path,"Unexpected profile read");
    ++profileReads;std::wmemset(output,L'x',1024);output[0]=L'C';output[1]=L':';output[2]=L'\\';output[1024]=0;
    if(fault==Fault::ProfileCopy)failNextAllocation=true;
    return 1024;
}
UINT WINAPI fakeProfileInt(LPCWSTR,LPCWSTR,INT fallback,LPCWSTR){return static_cast<UINT>(fallback);}
BOOL WINAPI ownedProfileWrite(LPCWSTR section,LPCWSTR key,LPCWSTR value,LPCWSTR file){
    require(file&&std::filesystem::path(file).parent_path()==std::filesystem::path(lapse::fileIOPath(ownedCase.wstring())),"Profile write escaped owned fixture");
    if(section&&key)++keyWrites;
    return ::WritePrivateProfileStringW(section,key,value,file);
}
BOOL WINAPI fakeIconic(HWND){return TRUE;} // Keep unrelated layout work outside this probe.
BOOL WINAPI fakeEnable(HWND,BOOL){return TRUE;}
BOOL WINAPI fakeText(HWND,LPCWSTR){return TRUE;}
BOOL WINAPI fakeInvalidate(HWND,const RECT*,BOOL){return TRUE;}
UINT_PTR WINAPI fakeTimer(HWND,UINT_PTR id,UINT,TIMERPROC){++timerStarts;return fault==Fault::Timer?0:id;}
BOOL WINAPI fakeKillTimer(HWND,UINT_PTR){++timerStops;return TRUE;}
BOOL WINAPI fakeAffinity(HWND,DWORD){return TRUE;}
void WINAPI fakeQuit(int){++quits;}
void WINAPI fakeDebug(LPCWSTR){++debugMessages;}
}
namespace lapse {
class ProbeEngine {
public:
    ProbeEngine(){++engineStarts;}
    ~ProbeEngine(){++engineStops;}
    void configure(const Settings& settings){++engineConfigures;lastConfiguredInterval=settings.interval;if(fault==Fault::AfterEngine){++syntheticThrows;throw std::bad_alloc();}}
    void refreshSources(){} void record(){++recordCalls;lastRecordedInterval=lastConfiguredInterval;} void pause(){} void setPaused(bool){} void finish(){}
    Status status(){return {};}
};
std::vector<Monitor> enumerateMonitors(){++monitorEnumerations;return {{L"Synthetic display",{0,0,1280,720},L"owned-display"}};}
std::vector<CameraDevice> enumerateCameras(std::wstring&){++cameraEnumerations;return {};}
int runCameraHost(const wchar_t*){throw std::runtime_error("Unexpected application entry");}
}
#define Engine ProbeEngine
#define CreateSolidBrush fakeBrush
#define DeleteObject fakeDelete
#define CreateFontW fakeFont
#define EnumChildWindows fakeChildren
#define GetDpiForWindow fakeDpi
#define CreateWindowExW fakeWindow
#define SendMessageW fakeSend
#define GetPrivateProfileStringW fakeProfileString
#define GetPrivateProfileIntW fakeProfileInt
#define WritePrivateProfileStringW ownedProfileWrite
#define IsIconic fakeIconic
#define EnableWindow fakeEnable
#define SetWindowTextW fakeText
#define InvalidateRect fakeInvalidate
#define SetTimer fakeTimer
#define KillTimer fakeKillTimer
#define SetWindowDisplayAffinity fakeAffinity
#define PostQuitMessage fakeQuit
#define OutputDebugStringW fakeDebug
#pragma warning(push)
#pragma warning(disable: 4702) // The helper-entry sentinel deliberately throws.
#include "../src/main.cpp"
#pragma warning(pop)
#undef Engine
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
namespace {
std::string readBytes(const std::filesystem::path& path){std::ifstream input(path,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
void run(const wchar_t* name,Fault selectedFault){
    failNextAllocation=false;fault=Fault::None;app.engine.reset();
    fault=selectedFault;resetChildFaults();
    failedAllocations=windows=fontsCreated=profileReads=keyWrites=engineStarts=engineStops=engineConfigures=syntheticThrows=choiceQueries=timerStarts=timerStops=quits=debugMessages=0;failedBytes=0;
    selections.fill(-1);counts.fill(0);
    std::wstring{}.swap(app.settings.folder);app.monitors.clear();app.cameras.clear();app.selectedMonitorId.clear();
    app.settings.cameraId.clear();app.settings.monitorId.clear();app.mode=nullptr;app.preview=nullptr;app.window=nullptr;app.layingOut=false;
    app.advancedExpanded=true;app.advancedVisibility=1;app.advancedLimitIndex=5;
    app.advancedNightState=2;app.nightVisibility=2;app.nightValidation=L"stale validation";app.nightHintCaption=L"stale hint";app.nightDetailCaption=L"stale blend";
    app.visibleDirty=false;app.controlsUpdated=true;app.controlsState=State::Paused;app.trayStateValid=true;
    ownedCase=ownedRun/name;require(std::filesystem::create_directory(ownedCase),"Owned case already exists");
    const auto path=ownedCase/L"settings.ini";app.preferences=path.native();
    const std::string original="; Owned startup sentinel\r\n[Settings]\r\nFolder=C:\\Prior\r\nInterval=4\r\nQuality=1\r\nEncodingQuality=0\r\n";
    {std::ofstream file(path,std::ios::binary);file<<original;require(bool(file),"Cannot seed owned preferences");}
    LRESULT created=123;bool escaped=false;
    try{created=windowProc(reinterpret_cast<HWND>(2),WM_CREATE,0,0);}catch(const std::bad_alloc&){escaped=true;}
    failNextAllocation=false;
    const bool failed=selectedFault!=Fault::None;
    if(selectedFault==Fault::Timer)require(created==-1&&!escaped&&!app.startupComplete&&!app.mode&&app.engine&&engineStarts==1&&engineStops==0&&timerStarts==1,"Failed timer accepted startup or changed Engine ownership before destroy");
    const int configBefore=engineConfigures,queriesBefore=choiceQueries;
    fault=Fault::None; // The late-engine exception is one synthetic callback event.
    if(failed)windowProc(reinterpret_cast<HWND>(2),WM_SIZE,SIZE_MINIMIZED,0);
    const int resizeQueries=choiceQueries-queriesBefore,resizeConfigures=engineConfigures-configBefore;
    windowProc(reinterpret_cast<HWND>(2),WM_DESTROY,0,0);
    const bool unchanged=readBytes(path)==original;
    std::wcout<<name<<L": result="<<created<<L" escaped="<<escaped<<L" realAllocationFailures="<<failedAllocations<<L" bytes="<<failedBytes
              <<L" syntheticThrows="<<syntheticThrows<<L" resizeQueries="<<resizeQueries<<L" resizeConfigures="<<resizeConfigures
              <<L" preferenceWrites="<<keyWrites<<L" oldBytesUnchanged="<<unchanged<<L" engines="<<engineStarts<<L"/"<<engineStops<<L" timers="<<timerStarts<<L"/"<<timerStops<<L" quit="<<quits<<L'\n';
    require(!app.engine&&engineStarts==engineStops&&timerStops==1&&quits==1,"Destroy did not reset inert engine/timer/quit");
    if(selectedFault==Fault::ProfileCopy)require(failedAllocations==1&&failedBytes>0&&syntheticThrows==0&&engineStarts==0,"Real profile allocation fault changed");
    if(selectedFault==Fault::AfterEngine)require(failedAllocations==0&&syntheticThrows==1&&engineStarts==1,"Synthetic post-engine exception changed");
    if(selectedFault==Fault::Timer)require(failedAllocations==0&&syntheticThrows==0&&engineStarts==1&&engineStops==1,"Synthetic zero-timer cleanup changed");
    if(failed){require(!escaped&&created==-1,"WM_CREATE did not reject failed startup");require(resizeQueries==0&&resizeConfigures==0,"Failed startup processed resize configuration");require(keyWrites==0&&unchanged&&debugMessages==1&&timerStarts==(selectedFault==Fault::Timer?1:0),"Failed startup changed preferences or timer attempts");}
    else {require(!escaped&&created==0&&failedAllocations==0&&engineStarts==1&&keyWrites==10&&!unchanged&&timerStarts==1&&debugMessages==0,"Healthy create/destroy changed");
        require(!app.advancedExpanded&&app.advancedVisibility==0&&app.advancedLimitIndex==0,"Recreated controls inherited stale Advanced caption or expanded state");
        require(app.advancedNightState==0&&app.nightVisibility==0&&app.nightValidation.empty()&&app.nightHintCaption.empty()&&app.nightDetailCaption.empty(),"Recreated controls inherited stale night visibility, validation or facts");
        require(app.visibleDirty&&app.controlsUpdated&&app.controlsState==State::Idle&&!app.trayStateValid,"Recreated controls inherited stale visual/tray caches");}
    for(const auto& file:std::filesystem::directory_iterator(ownedCase))require(file.path()==path,"Owned staged preferences leaked");
}
}
void* operator new(size_t bytes){if(std::exchange(failNextAllocation,false)){++failedAllocations;failedBytes=bytes;throw std::bad_alloc();}if(void* memory=std::malloc(bytes?bytes:1))return memory;throw std::bad_alloc();}
void* operator new[](size_t bytes){return ::operator new(bytes);}void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}void operator delete(void* value,size_t)noexcept{std::free(value);}void operator delete[](void* value,size_t)noexcept{std::free(value);}
namespace {
int routedCreates=0,routedDestroys=0,routeEscapes=0;
LRESULT CALLBACK route(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_CREATE||message==WM_DESTROY){
        if(message==WM_CREATE)++routedCreates;else ++routedDestroys;
        // A fixture safety fence prevents any unexpected exception reaching OS
        // dispatch. The assertions require that this fence was never entered.
        try{return windowProc(window,message,wp,lp);}
        catch(...){++routeEscapes;failNextAllocation=false;return message==WM_CREATE?-1:0;}
    }
    return DefWindowProcW(window,message,wp,lp);
}
void osCase(const wchar_t* className,const wchar_t* name,Fault selectedFault){
    failNextAllocation=false;fault=Fault::None;app.engine.reset();fault=selectedFault;resetChildFaults();
    failedAllocations=windows=fontsCreated=profileReads=keyWrites=engineStarts=engineStops=engineConfigures=syntheticThrows=choiceQueries=timerStarts=timerStops=quits=debugMessages=0;failedBytes=0;
    routedCreates=routedDestroys=routeEscapes=0;selections.fill(-1);counts.fill(0);
    std::wstring{}.swap(app.settings.folder);app.monitors.clear();app.cameras.clear();app.selectedMonitorId.clear();
    app.settings.cameraId.clear();app.settings.monitorId.clear();app.mode=nullptr;app.preview=nullptr;app.window=nullptr;app.layingOut=false;
    ownedCase=ownedRun/name;require(std::filesystem::create_directory(ownedCase),"Owned OS case exists");
    const auto path=ownedCase/L"settings.ini";app.preferences=path.native();
    const std::string original="; Owned hidden-window startup sentinel\r\n[Settings]\r\nFolder=C:\\Prior\r\nInterval=4\r\nQuality=1\r\nEncodingQuality=0\r\n";
    {std::ofstream file(path,std::ios::binary);file<<original;require(bool(file),"Cannot seed owned OS settings");}
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,className,L"Owned hidden startup control",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    failNextAllocation=false;
    const bool created=window!=nullptr;
    const int destroysAtReturn=routedDestroys,writesAtReturn=keyWrites,stopsAtReturn=engineStops;
    const bool acceptedAtReturn=app.startupComplete;
    if(window){
        require(!IsWindowVisible(window),"Owned window unexpectedly visible");
        require(DestroyWindow(window)!=FALSE,"Owned hidden destroy failed");
        require(!IsWindow(window),"Owned hidden HWND survived destruction");
    }
    const bool unchanged=readBytes(path)==original;
    std::wcout<<name<<L": created="<<created<<L" actualCreateMessages="<<routedCreates<<L" destroysAtCreateReturn="<<destroysAtReturn
              <<L" actualDestroyMessages="<<routedDestroys<<L" routeEscapes="<<routeEscapes<<L" realAllocationFailures="<<failedAllocations<<L" bytes="<<failedBytes
              <<L" acceptedAtReturn="<<acceptedAtReturn<<L" engineStopsAtReturn="<<stopsAtReturn<<L" writesAtCreateReturn="<<writesAtReturn<<L" preferenceWrites="<<keyWrites<<L" oldBytesUnchanged="<<unchanged<<L" engines="<<engineStarts<<L"/"<<engineStops<<L" timers="<<timerStarts<<L"/"<<timerStops<<L" quit="<<quits<<L'\n';
    require(routeEscapes==0&&routedCreates==1&&routedDestroys==1&&!app.engine&&timerStops==1&&quits==1,"Unexpected callback escape or missing OS cleanup");
    if(selectedFault==Fault::ProfileCopy)require(!created&&!acceptedAtReturn&&destroysAtReturn==1&&failedAllocations==1&&failedBytes>0&&keyWrites==0&&unchanged&&engineStarts==0&&engineStops==0&&timerStarts==0&&debugMessages==1,"OS failed-creation behavior changed");
    else if(selectedFault==Fault::Timer)require(!created&&!acceptedAtReturn&&!app.mode&&destroysAtReturn==1&&stopsAtReturn==1&&writesAtReturn==0&&failedAllocations==0&&syntheticThrows==0&&keyWrites==0&&unchanged&&engineStarts==1&&engineStops==1&&timerStarts==1&&debugMessages==1,"OS failed-timer cleanup or preference preservation changed");
    else require(created&&acceptedAtReturn&&destroysAtReturn==0&&stopsAtReturn==0&&writesAtReturn==0&&failedAllocations==0&&keyWrites==10&&!unchanged&&engineStarts==1&&engineStops==1&&timerStarts==1&&debugMessages==0,"OS healthy-creation behavior changed");
    for(const auto& file:std::filesystem::directory_iterator(ownedCase))require(file.path()==path,"Owned settings stage remained");
}
}
namespace {
std::filesystem::path currentIni;
std::string priorBytes;
void prepareChildCase(const std::wstring& name,int rejected,bool tooltip){
    fault=Fault::None;failNextAllocation=false;app.engine.reset();
    failedAllocations=windows=fontsCreated=profileReads=keyWrites=engineStarts=engineStops=engineConfigures=syntheticThrows=choiceQueries=timerStarts=timerStops=quits=debugMessages=0;
    failedChild=rejected;failTooltip=tooltip;creationFailures=nullMessages=recordCalls=lastConfiguredInterval=lastRecordedInterval=monitorEnumerations=cameraEnumerations=tooltipAttempts=0;
    selections.fill(-1);counts.fill(0);app.settings={};app.status={};
    app.monitors.clear();app.cameras.clear();app.selectedMonitorId.clear();app.mode=nullptr;app.preview=nullptr;app.window=nullptr;app.layingOut=false;app.closeWhenDone=false;
    ownedCase=ownedRun/name;require(std::filesystem::create_directory(ownedCase),"Owned case exists");
    currentIni=ownedCase/L"settings.ini";app.preferences=currentIni.native();
    priorBytes="; owned required-child sentinel\r\n[Settings]\r\nFolder=C:\\Prior\r\nInterval=2\r\nQuality=0\r\nEncodingQuality=1\r\n";
    {std::ofstream output(currentIni,std::ios::binary);output<<priorBytes;require(bool(output),"Cannot seed owned preferences");}
}
bool childUnchanged(){std::ifstream input(currentIni,std::ios::binary);return std::string(std::istreambuf_iterator<char>(input),{})==priorBytes;}
void verifyChildCleanup(bool rejected){
    require(!app.engine&&!app.startupComplete&&timerStops==1&&quits==1,"Destruction missed Engine/timer/quit cleanup");
    if(rejected)require(engineStarts==0&&engineStops==0&&engineConfigures==0&&timerStarts==0&&profileReads==0&&monitorEnumerations==0&&cameraEnumerations==0&&tooltipAttempts==0&&keyWrites==0&&debugMessages==1&&childUnchanged(),"Failed required child reached initialization or changed preferences");
    else require(engineStarts==1&&engineStops==1&&timerStarts==1&&profileReads==1&&monitorEnumerations==1&&cameraEnumerations==1&&tooltipAttempts==1&&keyWrites==10&&debugMessages==0&&lastConfiguredInterval==5&&!childUnchanged(),"Healthy initialization/persistence changed");
    require(GetPrivateProfileIntW(L"Settings",L"Interval",99,currentIni.c_str())==2,"Owned interval preference changed");
    for(const auto& entry:std::filesystem::directory_iterator(ownedCase))require(entry.path()==currentIni,"Owned preference stage leaked");
}
void directChildCase(const std::wstring& name,int rejected,bool tooltip=false){
    prepareChildCase(name,rejected,tooltip);
    const bool failure=rejected>=0;
    const LRESULT result=windowProc(reinterpret_cast<HWND>(2),WM_CREATE,0,0);
    require(creationFailures==((failure||tooltip)?1:0),"Wrong creation failure count");
    if(failure){
        require(result==-1&&!app.mode&&!app.startupComplete&&!app.engine,"Required child failure accepted startup");
        const int queries=choiceQueries;
        windowProc(reinterpret_cast<HWND>(2),WM_SIZE,SIZE_MINIMIZED,0);
        require(choiceQueries==queries,"Incomplete window performed resize configuration");
    }else{
        require(result==0&&app.startupComplete&&app.engine&&app.record&&lastConfiguredInterval==5,"Healthy child startup failed");
        require((app.tooltip==nullptr)==tooltip,"Optional tooltip failure changed");
        windowProc(reinterpret_cast<HWND>(2),WM_COMMAND,MAKEWPARAM(Record,BN_CLICKED),reinterpret_cast<LPARAM>(app.record));
        require(recordCalls==1&&lastRecordedInterval==5,"Actual healthy Record handler lost selected interval");
    }
    windowProc(reinterpret_cast<HWND>(2),WM_DESTROY,0,0);
    verifyChildCleanup(failure);
    std::wcout<<name<<L": result="<<result<<L" engine="<<engineStarts<<L'/'<<engineStops<<L" timerStarts="<<timerStarts<<L" profileReads="<<profileReads
        <<L" sources="<<monitorEnumerations<<L'/'<<cameraEnumerations<<L" tooltipAttempts="<<tooltipAttempts<<L" preferenceWrites="<<keyWrites<<L" oldBytesUnchanged="<<childUnchanged()<<L'\n';
}
}
namespace {
void childOsCase(const wchar_t* cls,const wchar_t* name,int rejected,bool tooltip=false){
    prepareChildCase(name,rejected,tooltip);routedCreates=routedDestroys=routeEscapes=0;
    const bool failure=rejected>=0;
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    const bool created=window!=nullptr;const unsigned destroyedAtReturn=routedDestroys;
    const int enginesAtReturn=engineStarts;
    if(failure)require(!created&&destroyedAtReturn==1&&!app.mode,"Failed child was not destroyed during actual creation");
    else{require(created&&destroyedAtReturn==0&&app.startupComplete&&!IsWindowVisible(window),"Healthy hidden startup failed");require((app.tooltip==nullptr)==tooltip,"Tooltip control differs");require(DestroyWindow(window)!=FALSE,"Actual hidden destruction failed");}
    require(routedCreates==1&&routedDestroys==1&&routeEscapes==0,"OS safety fence/cleanup contract failed");
    verifyChildCleanup(failure);
    std::wcout<<name<<L": created="<<created<<L" destroysAtReturn="<<destroyedAtReturn<<L" routeEscapes="<<routeEscapes<<L" enginesAtReturn="<<enginesAtReturn
        <<L" timerStarts="<<timerStarts<<L" profileReads="<<profileReads<<L" sources="<<monitorEnumerations<<L'/'<<cameraEnumerations
        <<L" preferenceWrites="<<keyWrites<<L" oldBytesUnchanged="<<childUnchanged()<<L'\n';
}
}

namespace {
struct OwnedFiles {
    const std::filesystem::path base=std::filesystem::current_path();
    OwnedFiles(){
        ownedRun=base/(L"ui-create-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(base.is_absolute()&&ownedRun.parent_path()==base,"Invalid owned fixture directory");
        require(std::filesystem::create_directory(ownedRun),"Owned run already exists");
    }
    ~OwnedFiles(){
        failNextAllocation=false;fault=Fault::None;app.engine.reset();
        // Only flat files inside the 49 fixed case directories created by
        // this fixture are removed; no recursive traversal or user paths.
        std::error_code error;
        if(!ownedRun.is_absolute()||ownedRun.parent_path()!=base)return;
        for(const auto* name:{L"profile-copy",L"post-engine-synthetic",L"healthy",L"zero-timer-synthetic",L"failed-create",L"healthy-create",L"failed-timer-create",
            L"required-100",L"required-101",L"required-102",L"required-103",L"required-104",L"required-105",L"required-106",L"required-107",L"required-108",L"required-109",L"required-110",L"required-111",L"required-112",L"required-113",L"required-114",L"required-115",L"required-116",L"required-117",L"required-118",L"required-119",L"required-120",L"required-121",L"required-122",L"required-123",
            L"required-label-200",L"required-label-201",L"required-label-202",L"required-label-203",L"required-label-204",L"required-label-205",L"required-label-206",L"required-label-207",L"required-label-208",L"required-label-209",L"required-status",L"child-healthy",L"child-tooltip",
            L"os-child-interval",L"os-child-record",L"os-child-preview",L"os-child-healthy",L"os-child-tooltip"}){
            const auto directory=ownedRun/name;
            if(!std::filesystem::exists(directory,error)){error.clear();continue;}
            for(std::filesystem::directory_iterator it(directory,error),end;!error&&it!=end;it.increment(error)){
                if(it->path().parent_path()==directory&&it->is_regular_file(error))std::filesystem::remove(it->path(),error);
            }
            if(!error)std::filesystem::remove(directory,error);
            error.clear();
        }
        std::filesystem::remove(ownedRun,error);
    }
};
}
int main(){
    std::wcout<<std::unitbuf;int passed=0,failed=0;
    try{
        OwnedFiles files;
        for(const auto& item:{std::pair<const wchar_t*,Fault>{L"profile-copy",Fault::ProfileCopy},{L"post-engine-synthetic",Fault::AfterEngine},{L"healthy",Fault::None},{L"zero-timer-synthetic",Fault::Timer}}){
            try{run(item.first,item.second);++passed;}catch(const std::exception& error){failNextAllocation=false;++failed;std::cout<<"FAIL "<<error.what()<<'\n';}
        }
        const auto child=[&](const std::wstring& name,int id,bool tooltip=false){
            try{directChildCase(name,id,tooltip);++passed;}catch(const std::exception& error){failNextAllocation=false;++failed;std::cout<<"FAIL "<<error.what()<<'\n';}
        };
        for(int id=ModeBox;id<=NightDetail;++id)child(L"required-"+std::to_wstring(id),id);
        for(int id=200;id<210;++id)child(L"required-label-"+std::to_wstring(id),id);
        child(L"required-status",210);child(L"child-healthy",-1);child(L"child-tooltip",-1,true);
        const auto className=L"TimelapseOwnedCreateTests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=route;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=className.c_str();
        require(RegisterClassExW(&cls)!=0,"Owned hidden class registration failed");
        for(const auto& item:{std::pair<const wchar_t*,Fault>{L"failed-create",Fault::ProfileCopy},{L"healthy-create",Fault::None},{L"failed-timer-create",Fault::Timer}}){
            try{osCase(className.c_str(),item.first,item.second);++passed;}catch(const std::exception& error){failNextAllocation=false;++failed;std::cout<<"FAIL "<<error.what()<<'\n';}
        }
        const auto childOs=[&](const wchar_t* name,int id,bool tooltip=false){
            try{childOsCase(className.c_str(),name,id,tooltip);++passed;}catch(const std::exception& error){failNextAllocation=false;++failed;std::cout<<"FAIL "<<error.what()<<'\n';}
        };
        childOs(L"os-child-interval",IntervalBox);childOs(L"os-child-record",Record);childOs(L"os-child-preview",Preview);
        childOs(L"os-child-healthy",-1);childOs(L"os-child-tooltip",-1,true);
        require(UnregisterClassW(className.c_str(),cls.hInstance)!=FALSE,"Owned class was not released");
    }catch(const std::exception& error){failNextAllocation=false;++failed;std::cout<<"FAIL "<<error.what()<<'\n';}
    std::cout<<passed<<"/49 cases passed: seven prior startup cases, 37 required-child direct controls and five hidden OS controls; children/fonts/Engine/timer synthetic, only owned preferences.\n";
    return failed||passed!=49?1:0;
}

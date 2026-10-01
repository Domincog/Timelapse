// Actual UI command handlers + actual worker. Hidden private controls,
// synthetic pixels/encoder files, no real capture, app startup, input or INI I/O.
#include "../src/engine.h"
#include "../src/capture.h"
#include "../src/camera_host.h"
#include "../src/encoder.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <atomic>
#include <cstring>

namespace probe {
using Clock=std::chrono::steady_clock;
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
long long elapsed(Clock::time_point began) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-began).count(); }
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool armed=false,reached=false,released=false,timedOut=false;
    void arm() { std::lock_guard<std::mutex> lock(mutex);armed=true;reached=released=timedOut=false; }
    void enter() {
        std::unique_lock<std::mutex> lock(mutex);
        if(!armed)return;
        armed=false;reached=true;changed.notify_all();
        if(!changed.wait_for(lock,std::chrono::seconds(3),[&]{return released;}))timedOut=true;
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock,std::chrono::milliseconds(1500),[&]{return reached;}),"Encoder finish gate not reached");
    }
    void release() { std::lock_guard<std::mutex> lock(mutex);released=true;changed.notify_all(); }
} finishGate, captureGate;
std::atomic<uint64_t> captureCalls{0};
std::atomic<bool> failCapture{false}, failFinish{false}, failRename{false};
std::atomic<int> captureWidth{0};
std::atomic<int> captureGateWidth{0};
std::atomic<unsigned> encoderOpens{0};
std::atomic<bool> encoderRecovery{false};
std::atomic<unsigned> wakeLoads{0},wakeQueries{0},wakeFrees{0};
HMODULE WINAPI loadWakeModule(LPCWSTR name,HANDLE file,DWORD flags){
    require(name && std::wcscmp(name,L"powrprof.dll")==0 && !file && flags==LOAD_LIBRARY_SEARCH_SYSTEM32,"Unexpected module request in self-timer fixture");
    ++wakeLoads;return reinterpret_cast<HMODULE>(1);
}
LONG WINAPI wakeInformation(POWER_INFORMATION_LEVEL level,PVOID input,ULONG inputBytes,PVOID output,ULONG outputBytes){
    require(level==LastWakeTime && !input && !inputBytes && output && outputBytes==sizeof(uint64_t),"Unexpected self-timer wake query");
    ++wakeQueries;const uint64_t epoch=17;std::memcpy(output,&epoch,sizeof(epoch));return 0;
}
FARPROC WINAPI wakeAddress(HMODULE module,LPCSTR name){
    require(module==reinterpret_cast<HMODULE>(1) && name && std::strcmp(name,"CallNtPowerInformation")==0,"Unexpected self-timer export");
    const auto function=&wakeInformation;FARPROC address{};static_assert(sizeof(address)==sizeof(function));std::memcpy(&address,&function,sizeof(address));return address;
}
BOOL WINAPI freeWakeModule(HMODULE module){require(module==reinterpret_cast<HMODULE>(1),"Wrong owned wake module");++wakeFrees;return TRUE;}
int confirmations=0, errorDialogs=0, foregroundCalls=0, destroyCalls=0;
std::wstring dialogMessage;
int WINAPI messageBox(HWND,LPCWSTR message,LPCWSTR title,UINT flags) {
    if(flags & MB_ICONQUESTION) { ++confirmations; return IDOK; }
    require(std::wstring(title)==L"Timelapse could not finish normally", "Unexpected modal path");
    ++errorDialogs;dialogMessage=message;return IDOK;
}
BOOL WINAPI foreground(HWND) { ++foregroundCalls;return TRUE; }
BOOL WINAPI destroy(HWND) { ++destroyCalls;return TRUE; }
BOOL WINAPI show(HWND,int) { return TRUE; } // Keep every synthetic fixture hidden.
BOOL WINAPI tray(DWORD,PNOTIFYICONDATAW) { return TRUE; } // No notification-area side effects.
DWORD publishFile(HANDLE file,const std::wstring& path) {
    if(failRename)return ERROR_ACCESS_DENIED;
    const size_t nameBytes=path.size()*sizeof(wchar_t);
    std::vector<BYTE> buffer(sizeof(FILE_RENAME_INFO)+nameBytes,0);
    auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;
    rename->FileNameLength=static_cast<DWORD>(nameBytes);
    std::copy(path.begin(),path.end(),rename->FileName);
    return SetFileInformationByHandle(file,FileRenameInfo,rename,static_cast<DWORD>(buffer.size()))?ERROR_SUCCESS:GetLastError();
}
EXECUTION_STATE WINAPI power(EXECUTION_STATE){return ES_CONTINUOUS;}
}
namespace lapse {
std::vector<Monitor> enumerateMonitors() { throw std::runtime_error("Unexpected enumeration"); }
std::vector<CameraDevice> enumerateCameras(std::wstring&) { throw std::runtime_error("Unexpected camera enumeration"); }
int runCameraHost(const wchar_t*) { throw std::runtime_error("Unexpected application entry"); }
void releaseDesktopCaptureCache() noexcept {}
bool captureMonitor(const std::wstring& id,int width,int,bool,Frame& output,std::wstring& error) {
    error.clear();
    if(id!=L"owned-display"){error=L"Synthetic display unavailable.";return false;}
    ++probe::captureCalls;probe::captureWidth=width;
    if(!probe::captureGateWidth || probe::captureGateWidth==width)probe::captureGate.enter();
    if(probe::failCapture) { error=L"Synthetic display capture failed.";return false; }
    output.width=32;output.height=18;output.pixels.assign(32*18*4,100);return true;
}
bool CameraClient::beginNight(uint64_t, uint32_t, const NightSettings&, std::wstring& error) {
    error = L"Unexpected night request in ordinary-mode fixture."; return false;
}
bool CameraClient::nightResult(uint64_t, Frame&, NightWindowResult&, std::wstring& error) {
    error = L"Unexpected night result in ordinary-mode fixture."; return false;
}
void CameraClient::cancelNight() noexcept {}
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) {
    error=L"Unexpected activity observer in ordinary recording fixture.";return false;
}
void CameraClient::cancelActivityObservation() noexcept {}
struct CameraClient::Impl {};
CameraClient::CameraClient():impl_(std::make_unique<Impl>()){}
CameraClient::~CameraClient(){}
bool CameraClient::start(const std::wstring&,std::wstring&,CameraResolution) { throw std::runtime_error("Physical camera path forbidden"); }
void CameraClient::stop(){}
bool CameraClient::latest(Frame&,std::wstring&) { throw std::runtime_error("Physical camera path forbidden"); }
struct Encoder::Impl { HANDLE file=INVALID_HANDLE_VALUE;uint64_t count=0;bool emptyDiscarded=false; };
Encoder::Encoder():impl_(std::make_unique<Impl>()){}
Encoder::~Encoder(){if(impl_->file!=INVALID_HANDLE_VALUE)CloseHandle(impl_->file);}
bool Encoder::open(const std::wstring& path,int,int,int,std::wstring& error,EncodingQuality,EncodingMode,bool recoveryMode) {
    ++probe::encoderOpens;probe::encoderRecovery=recoveryMode;
    if(impl_->file!=INVALID_HANDLE_VALUE)CloseHandle(impl_->file);
    impl_->count=0;impl_->emptyDiscarded=false;error.clear();
    impl_->file=CreateFileW(path.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(impl_->file==INVALID_HANDLE_VALUE){error=L"Owned synthetic output could not open.";return false;}
    return true;
}
bool Encoder::write(const Frame& frame,std::wstring& error){
    error.clear();if(!frame.valid())return false;
    const DWORD marker=0x54455354;DWORD written=0;
    if(!WriteFile(impl_->file,&marker,sizeof(marker),&written,nullptr)||written!=sizeof(marker))return false;
    ++impl_->count;return true;
}
bool Encoder::finishForPublication(std::wstring& error){probe::finishGate.enter();error.clear();
    if(probe::failFinish) { error=L"Synthetic encoder finalization failed.";return false; }
    if(!impl_->count){FILE_DISPOSITION_INFO remove{TRUE};
        if(impl_->file==INVALID_HANDLE_VALUE || !SetFileInformationByHandle(impl_->file,FileDispositionInfo,&remove,sizeof(remove))){error=L"Synthetic empty output could not be removed.";return false;}
        CloseHandle(impl_->file);impl_->file=INVALID_HANDLE_VALUE;impl_->emptyDiscarded=true;error=L"No video frames were recorded.";return false;
    }return true;
}
bool Encoder::finish(std::wstring& error){const bool result=finishForPublication(error);releasePublication();return result;}
DWORD Encoder::publish(const std::wstring& path){return probe::publishFile(impl_->file,path);}
void Encoder::releasePublication() noexcept {if(impl_->file!=INVALID_HANDLE_VALUE)CloseHandle(impl_->file);impl_->file=INVALID_HANDLE_VALUE;}
uint64_t Encoder::frames()const{return impl_->count;}
bool Encoder::emptyOutputDiscarded()const noexcept{return impl_->emptyDiscarded;}
}

#define SetThreadExecutionState probe::power
#define LoadLibraryExW probe::loadWakeModule
#define GetProcAddress probe::wakeAddress
#define FreeLibrary probe::freeWakeModule
#include "../src/engine.cpp"
#undef FreeLibrary
#undef GetProcAddress
#undef LoadLibraryExW
#undef SetThreadExecutionState
// The reject-entry sentinel intentionally makes the GUI entry unreachable.
#pragma warning(push)
#pragma warning(disable: 4702)
#define MessageBoxW probe::messageBox
#define SetForegroundWindow probe::foreground
#define DestroyWindow probe::destroy
#define ShowWindow probe::show
#define Shell_NotifyIconW probe::tray
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#undef Shell_NotifyIconW
#undef ShowWindow
#undef DestroyWindow
#undef SetForegroundWindow
#undef MessageBoxW
#pragma warning(pop)

namespace {
using probe::require;
using probe::Clock;
std::filesystem::path ownedRoot;
std::wstring caption(HWND control){wchar_t text[50]{};GetWindowTextW(control,text,50);return text;}
void click(int id,HWND control){require(IsWindowEnabled(control)!=FALSE,"Command button must be enabled in cached UI");windowProc(app.window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(control));}
void tick(){windowProc(app.window,WM_TIMER,1,0);}
Status waitState(State state){
    const auto began=Clock::now();
    while(probe::elapsed(began)<1500){auto status=app.engine->status();if(status.state==state && (state!=State::Recording||status.frames))return status;Sleep(1);}
    throw std::runtime_error("Worker state deadline expired");
}
struct HiddenFixture {
    explicit HiddenFixture(const wchar_t* name,bool visiblePreview=true){
        probe::failCapture=false;probe::failFinish=false;probe::failRename=false;
        probe::captureGateWidth=0;
        probe::confirmations=probe::errorDialogs=probe::foregroundCalls=probe::destroyCalls=0;
        probe::dialogMessage.clear();
        app.settings={};app.status={};app.selected=-1;app.closeWhenDone=false;app.modeIndex=0;
        app.failureNotice=FailureNotice::None;
        app.visibleDirty=true;app.controlsUpdated=app.trayStateValid=false;
        app.encodingValidation.clear();
        app.hiddenToTray=!visiblePreview;app.trayRegistered=app.trayVersion4=app.trayNoticeShown=false;app.trayTooltip.clear();
        probe::wakeLoads=probe::wakeQueries=probe::wakeFrees=0;
        app.settings.folder=(ownedRoot/name).wstring();
        app.window=CreateWindowExW(0,L"STATIC",L"Owned pause review",WS_OVERLAPPED,0,0,920,720,nullptr,nullptr,nullptr,nullptr);
        require(app.window&&!IsWindowVisible(app.window),"Hidden parent creation failed");
        auto child=[&](const wchar_t* cls,DWORD style){auto w=CreateWindowExW(0,cls,L"",WS_CHILD|style,0,0,100,100,app.window,nullptr,nullptr,nullptr);require(w!=nullptr,"Hidden child failed");return w;};
        auto combo=[&](int count){auto w=child(L"COMBOBOX",CBS_DROPDOWNLIST);for(int i=0;i<count;++i)add(w,std::to_wstring(i));choose(w,0);return w;};
        app.mode=combo(6);app.interval=combo(6);choose(app.interval,5);app.videoSize=combo(2);app.encodingQuality=combo(3);app.encodingMode=combo(5);
        app.startDelay=combo(6);
        app.recoveryMode=child(L"BUTTON",BS_AUTOCHECKBOX);
        app.monitor=combo(1);app.camera=combo(0);app.monitors={{L"Synthetic",{0,0,640,360},L"owned-display"}};app.cameras.clear();
        app.preview=child(L"STATIC",0);app.statusText=child(L"STATIC",0);
        for(auto p:{&app.refresh,&app.record,&app.pause,&app.finish,&app.folder,&app.openFolder,&app.reset,&app.forward})*p=child(L"BUTTON",BS_PUSHBUTTON);
        app.engine=std::make_unique<Engine>();configure();updateControls();
    }
    ~HiddenFixture(){probe::finishGate.release();probe::captureGate.release();removeTray();app.engine.reset();DestroyWindow(app.window);app.window=app.preview=app.statusText=nullptr;}
    void start(bool shortCadence=false){if(shortCadence){choose(app.interval,0);configure();}click(Record,app.record);waitState(State::Recording);tick();require(caption(app.pause)==L"&Pause","Recording caption");}
};
void settleCommand() {
    // A later capture after a released capture gate belongs to another worker
    // snapshot, proving the queued command was consumed before assertions.
    probe::captureGate.arm();probe::captureGate.wait();
    const auto seen=probe::captureCalls.load();probe::captureGate.release();
    const auto began=Clock::now();
    while(probe::captureCalls.load()==seen&&probe::elapsed(began)<1500)Sleep(1);
    require(probe::captureCalls.load()>seen,"No later worker snapshot after queued command");
    // Capture entry precedes a possible recording write. Retain the published
    // buffer until it changes, proving that this or a later cycle completed.
    const auto published=app.engine->status().preview;
    const auto finishing=Clock::now();
    while(app.engine->status().preview==published&&probe::elapsed(finishing)<1500)Sleep(1);
    require(app.engine->status().preview!=published,"Later worker cycle did not publish completion");
}
void repeatedPause(){
    HiddenFixture f(L"pause");f.start();
    click(Pause,app.pause);waitState(State::Paused);
    require(app.status.state==State::Recording&&caption(app.pause)==L"&Pause","First pause unexpectedly refreshed cached UI");
    const auto frames=app.engine->status().frames;
    click(Pause,app.pause);settleCommand();
    require(app.engine->status().state==State::Paused&&app.engine->status().frames==frames,"Repeated visible Pause resumed or appended a frame");
    require(app.status.state==State::Recording&&caption(app.pause)==L"&Pause","Second click label changed");
    std::cout<<"PASS repeated visible Pause remains Paused without another recording frame.\n";
}
void repeatedResume(){
    HiddenFixture f(L"resume");f.start();click(Pause,app.pause);waitState(State::Paused);tick();
    require(caption(app.pause)==L"&Resume","Paused caption");
    const auto pausedFrames=app.engine->status().frames;
    click(Pause,app.pause);
    const auto resumed=Clock::now();
    while(app.engine->status().frames==pausedFrames&&probe::elapsed(resumed)<1500)Sleep(1);
    require(app.engine->status().state==State::Recording&&app.engine->status().frames==pausedFrames+1,
            "Initial Resume did not complete exactly one due frame");
    require(app.status.state==State::Paused&&caption(app.pause)==L"&Resume","First resume unexpectedly refreshed cached UI");
    const auto frames=app.engine->status().frames;
    click(Pause,app.pause);settleCommand();
    require(app.engine->status().state==State::Recording&&app.engine->status().frames==frames,"Repeated visible Resume paused or reset the recording schedule");
    require(app.status.state==State::Paused&&caption(app.pause)==L"&Resume","Second resume label changed");
    std::cout<<"PASS repeated visible Resume remains Recording without an extra due frame.\n";
}
void finishThenRecord(bool duplicateFinish,bool queuedPause){
    HiddenFixture f(duplicateFinish?L"duplicate-finish":queuedPause?L"pause-during-finish":L"single-finish");f.start();
    probe::finishGate.arm();click(Finish,app.finish);probe::finishGate.wait();
    require(app.engine->status().state==State::Finishing&&app.status.state==State::Recording,"Finish must be in flight with stale cached Recording UI");
    if(duplicateFinish)click(Finish,app.finish);
    if(queuedPause)click(Pause,app.pause);
    probe::finishGate.release();waitState(State::Idle);tick();
    require(IsWindowEnabled(app.record)&&!IsWindowEnabled(app.finish)&&!IsWindowEnabled(app.pause),"Idle timer must enable only new Record");
    click(Record,app.record);
    const auto began=Clock::now();Status outcome;
    while(probe::elapsed(began)<1500){outcome=app.engine->status();if(outcome.state==State::Idle||(outcome.state==State::Recording&&outcome.frames))break;Sleep(1);}
    if(duplicateFinish){
        require(outcome.state==State::Recording&&outcome.frames==1,"Duplicate Finish cancelled the later enabled Record");
        std::cout<<"PASS duplicate visible Finish during finalization leaves the later Record intact.\n";
    }else{
        require(outcome.state==State::Recording&&outcome.frames==1,"Control new Record did not proceed normally");
        std::cout<<"PASS "<<(queuedPause?"stale Pause during Finish":"single Finish")<<" then new Record remains Recording with one frame.\n";
    }
}
template<class Predicate> Status waitFor(Predicate predicate,const char* message) {
    const auto began=Clock::now();
    while(probe::elapsed(began)<1500){auto status=app.engine->status();if(predicate(status))return status;Sleep(1);}
    throw std::runtime_error(message);
}
void recordingWhileHidden(){
    HiddenFixture f(L"recording-in-tray");f.start(true);const auto original=app.engine->status().frames;
    windowProc(app.window,WM_CLOSE,0,0);
    require(app.hiddenToTray&&app.trayRegistered&&!app.settings.preview&&!app.closeWhenDone&&probe::confirmations==0&&probe::destroyCalls==0,"Close failed to retain active worker in tray");
    const auto recorded=waitFor([&](const Status& status){return status.state==State::Recording&&status.frames>original;},"Hidden worker did not record the next due frame");
    require(recorded.frames==original+1,"Hidden transition duplicated recording capture");
    click(Finish,app.finish);const auto saved=waitState(State::Idle);tick();
    require(!saved.recordingFailed&&!saved.savedPath.empty()&&saved.frames==recorded.frames&&std::filesystem::file_size(saved.savedPath)==saved.frames*sizeof(DWORD),"Hidden recording did not finalize every captured frame");
    require(app.hiddenToTray&&probe::destroyCalls==0,"Finish unexpectedly exited tray application");
    std::cout<<"PASS actual worker continues due captures with preview disabled after Close and finishes in tray.\n";
}
void recoveryRecording(){
    HiddenFixture f(L"recovery");const auto opened=probe::encoderOpens.load();
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,RecoveryBox,0);
    choose(app.encodingMode,3);windowProc(app.window,WM_COMMAND,MAKEWPARAM(EncodingModeBox,CBN_SELCHANGE),0);
    require(!IsWindowEnabled(app.record)&&!app.encodingValidation.empty(),"HEVC/recovery did not visibly block Record.");
    windowProc(app.window,WM_COMMAND,Record,0);
    require(app.engine->status().state==State::Idle&&probe::encoderOpens==opened,"Invalid UI command started the encoder.");
    choose(app.encodingMode,0);windowProc(app.window,WM_COMMAND,MAKEWPARAM(EncodingModeBox,CBN_SELCHANGE),0);f.start();
    require(probe::encoderOpens==opened+1&&probe::encoderRecovery,"Actual Record/worker path did not pass the recovery choice to its encoder.");
    click(Finish,app.finish);auto saved=waitState(State::Idle);tick();
    require(!saved.recordingFailed&&!saved.savedPath.empty()&&saved.frames==1,"Recovery option disrupted normal synthetic finalization.");
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);windowProc(app.window,WM_COMMAND,RecoveryBox,0);f.start();
    require(probe::encoderOpens==opened+2&&!probe::encoderRecovery,"Later ordinary recording retained recovery mode.");
    click(Finish,app.finish);waitState(State::Idle);tick();
    std::cout<<"PASS actual Record/worker rejects HEVC recovery, passes enabled H.264 policy, finishes and restores ordinary subsequent recording\n";
}
void unseenWorkerFailure(bool exit){
    HiddenFixture f(exit?L"unseen-failure-exit":L"unseen-failure-hide");f.start();
    if(exit)windowProc(app.window,WM_CLOSE,0,0);
    probe::failFinish=true;click(Finish,app.finish);const auto failed=waitState(State::Idle);
    require(failed.recordingFailed&&failed.error&&!app.status.recordingFailed&&app.status.state==State::Recording,
        "Need a real unobserved terminal worker failure before the next timer");
    if(exit)windowProc(app.window,WM_COMMAND,TrayExit,0);else windowProc(app.window,WM_CLOSE,0,0);
    require(!app.hiddenToTray&&probe::errorDialogs==1&&!probe::destroyCalls&&!probe::confirmations&&probe::dialogMessage==failed.message,
        "Lifecycle command hid or destroyed an unreported real worker failure");
    tick();windowProc(app.window,WM_COMMAND,TrayExit,0);
    require(probe::destroyCalls==1&&probe::errorDialogs==1,"Deliberate Exit after the real failure warning did not proceed exactly once");
    std::cout<<"PASS actual worker failed finalization is surfaced before "<<(exit?"Exit":"Hide")<<" even without an intervening status timer\n";
}
void requestClose() {
    windowProc(app.window,WM_COMMAND,TrayExit,0);
    require(probe::confirmations==1 && app.closeWhenDone && !IsWindowEnabled(app.window),
            "Actual close handler did not queue Finish-and-close and disable the window");
}
bool successfulClose(bool previewFails) {
    HiddenFixture f(previewFails?L"success-then-preview-error":L"healthy-success");f.start();
    probe::finishGate.arm();requestClose();probe::finishGate.wait();
    require(app.engine->status().state==State::Finishing,"Finish gate must own Finishing");
    probe::captureGate.arm();probe::failCapture=previewFails;probe::finishGate.release();
    const auto saved=waitState(State::Idle);
    require(!saved.recordingFailed && !saved.error && saved.frames==1 && !saved.savedPath.empty() &&
            std::filesystem::file_size(saved.savedPath)==sizeof(DWORD),
            "Worker did not successfully finalize/rename the one-frame synthetic output");
    probe::captureGate.wait();
    require(probe::captureWidth==640 && app.engine->status().state==State::Idle,
            "Post-save gate must be disposable idle preview capture");
    const auto oldPreview=saved.preview;probe::captureGate.release();
    const auto observed=waitFor([&](const Status& s){return previewFails?s.error:s.preview!=oldPreview;},
            "Idle preview cycle did not publish its expected outcome");
    require(!observed.recordingFailed && observed.state==State::Idle && observed.frames==saved.frames && observed.savedPath==saved.savedPath,
            "Preview changed saved movie ownership or frame count");
    if(previewFails)require(observed.message==L"Synthetic display capture failed.","Wrong preview error provenance");
    tick();
    const bool closed=probe::destroyCalls==1 && probe::errorDialogs==0 && probe::foregroundCalls==0 && !app.closeWhenDone;
    std::cout<<(closed?"PASS ":"FAIL ")<<(previewFails?"successful save then idle preview error":"healthy successful save")
        <<": saved=1 frames="<<observed.frames<<" preview_error="<<observed.error
        <<" destroy="<<probe::destroyCalls<<" error_dialog="<<probe::errorDialogs
        <<" foreground="<<probe::foregroundCalls<<'\n';
    return closed;
}
bool terminalFailure(int kind) {
    HiddenFixture f(kind==0?L"due-capture-failure":kind==1?L"finalize-failure":L"rename-failure");
    f.start(kind==0);
    if(kind==0) {
        // Preview and recording have independent deadlines; gate the due
        // full-resolution sample regardless of which preview arrives first.
        probe::captureGateWidth=app.settings.width;
        probe::captureGate.arm();probe::captureGate.wait();
        require(probe::captureWidth==app.settings.width,"Failure control must be a real due recording capture");
        probe::failCapture=true;requestClose();probe::captureGate.release();probe::captureGateWidth=0;
    } else {
        probe::failFinish=kind==1;probe::failRename=kind==2;
        probe::finishGate.arm();requestClose();probe::finishGate.wait();probe::finishGate.release();
    }
    const auto failed=waitState(State::Idle);
    require(failed.recordingFailed && failed.error && failed.frames==1,"Genuine failure must keep its terminal recording status");
    if(kind==1)require(failed.savedPath.empty()&&failed.message.find(L"Could not finish video")!=std::wstring::npos,
                      "Finalization control did not retain its specific error");
    else require(!failed.savedPath.empty()&&std::filesystem::file_size(failed.savedPath)==sizeof(DWORD),
                 "Capture/rename failure should retain its finalized synthetic movie");
    if(kind==0)require(failed.message.find(L"Capture stopped: Synthetic display capture failed.")==0,
                      "Due failure was classified as preview failure");
    if(kind==2)require(failed.savedPath.find(L".recording.mp4")!=std::wstring::npos&&
                      failed.message.find(L"could not rename")!=std::wstring::npos,
                      "Rename control did not retain the complete temporary movie");
    tick();
    const bool stayed=probe::destroyCalls==0 && probe::errorDialogs==1 && probe::foregroundCalls==1 &&
        !app.closeWhenDone && IsWindowEnabled(app.window) && probe::dialogMessage==failed.message;
    std::cout<<(stayed?"PASS ":"FAIL ")<<(kind==0?"genuine due capture failure":kind==1?"finalization failure":"rename failure")
        <<": frames="<<failed.frames<<" saved="<<!failed.savedPath.empty()<<" destroy="<<probe::destroyCalls
        <<" error_dialog="<<probe::errorDialogs<<'\n';
    app.engine->refreshSources();
    const auto refreshed=app.engine->status();
    require(refreshed.state==failed.state && refreshed.error==failed.error && refreshed.recordingFailed==failed.recordingFailed &&
            refreshed.message==failed.message && refreshed.savedPath==failed.savedPath && refreshed.savedPaths==failed.savedPaths &&
            refreshed.frames==failed.frames && refreshed.elapsed==failed.elapsed && refreshed.completedSegments==failed.completedSegments,
            "Source Refresh changed the terminal recording report, recovery paths or statistics");
    tick();
    require(app.status.error && app.status.recordingFailed && app.status.message==failed.message &&
            probe::destroyCalls==0 && probe::errorDialogs==1 && !app.closeWhenDone,
            "Refreshed failure report exited the UI or repeated the acknowledged failure notice");
    return stayed;
}

void delayedUiCancellation(){
    for(int action=0;action<4;++action){HiddenFixture f((L"delay-cancel-"+std::to_wstring(action)).c_str(),false);
        choose(app.startDelay,1);configure();const auto opens=probe::encoderOpens.load();const auto captures=probe::captureCalls.load();
        click(Record,app.record);const auto armed=waitState(State::Waiting);tick();
        require(armed.startDeadlineTick && !armed.frames && armed.elapsed==0 && app.status.state==State::Waiting,
            "Actual Record failed to expose a Waiting request");
        windowProc(app.window,WM_COMMAND,Record,0);
        windowProc(app.window,WM_CLOSE,0,0);
        require(app.engine->status().startDeadlineTick==armed.startDeadlineTick && app.engine->status().state==State::Waiting &&
            app.hiddenToTray && probe::encoderOpens==opens && probe::captureCalls==captures,
            "Duplicate Record or Hide changed the armed deadline or performed hidden recording work");
        if(action==0){windowProc(app.window,WM_COMMAND,TrayFinish,0);windowProc(app.window,WM_COMMAND,TrayFinish,0);}
        else if(action==1)windowProc(app.window,WM_POWERBROADCAST,PBT_APMSUSPEND,0);
        else if(action==2)windowProc(app.window,WM_COMMAND,TrayExit,0);
        else {
            require(windowProc(app.window,WM_QUERYENDSESSION,0,0)==TRUE,"Session-end query rejected shutdown");
            windowProc(app.window,WM_ENDSESSION,FALSE,0);
            require(app.engine->status().state==State::Waiting && !probe::destroyCalls,"Rejected session end canceled the timer");
            windowProc(app.window,WM_ENDSESSION,TRUE,0);
            require(probe::destroyCalls==1,"Confirmed session end failed to destroy the waiting owner");
            // DestroyWindow is an inert seam here; deliver its documented
            // destruction callback explicitly to run actual worker teardown.
            windowProc(app.window,WM_DESTROY,0,0);
            require(!app.engine,"Session teardown retained the armed worker");
        }
        if(action!=3){const auto cancelled=waitState(State::Idle);tick();
            require(!cancelled.error && !cancelled.recordingFailed && !cancelled.frames && cancelled.elapsed==0 &&
                !cancelled.startDeadlineTick && cancelled.savedPath.empty() && cancelled.savedPaths.empty(),
                "Deliberate delayed cancellation became a recording failure or saved-output claim");
            windowProc(app.window,WM_POWERBROADCAST,PBT_APMRESUMEAUTOMATIC,0);
            windowProc(app.window,WM_POWERBROADCAST,PBT_APMRESUMESUSPEND,0);
            require(app.engine->status().state==State::Idle,"Resume notification rearmed the cancelled timer");
        }
        require(probe::encoderOpens==opens && probe::captureCalls==captures && !std::filesystem::exists(app.settings.folder) &&
            !probe::errorDialogs && probe::wakeLoads==probe::wakeFrees && probe::wakeQueries>0,
            "Cancelled hidden timer performed output/capture work, leaked its query module or raised a failure notice");
        if(action==2)require(probe::confirmations==0 && probe::destroyCalls==1,"Dormant Waiting Exit did not finish its direct cancellation path exactly once");
        if(action<2){const auto queries=probe::wakeQueries.load();choose(app.startDelay,0);configure();tick();
            windowProc(app.window,WM_COMMAND,Record,0);waitState(State::Recording);tick();windowProc(app.window,WM_COMMAND,TrayFinish,0);const auto saved=waitState(State::Idle);
            require(saved.frames==1 && !saved.error && !saved.savedPath.empty() && probe::wakeQueries==queries,
                "Explicit immediate Record after cancellation inherited the timer or its wake-query work");
        }
    }
    std::cout<<"PASS actual UI/worker Waiting, fixed deadline, Hide, duplicate commands, Cancel/suspend/Exit/session end and explicit next Record\n";
}
void immediatePowerControls(){HiddenFixture f(L"immediate-power",false);
    probe::captureGate.arm();click(Record,app.record);probe::captureGate.wait();
    require(app.engine->status().state==State::Starting,"Immediate preparation gate was not reached");
    for(UINT event:{PBT_APMSUSPEND,PBT_APMRESUMEAUTOMATIC,PBT_APMRESUMESUSPEND,PBT_APMRESUMECRITICAL})windowProc(app.window,WM_POWERBROADCAST,event,0);
    require(app.engine->status().state==State::Starting && !probe::wakeLoads,"Power notification changed default-Off preparation or loaded its delay query");
    probe::captureGate.release();waitState(State::Recording);tick();
    for(bool paused:{false,true}){if(paused){windowProc(app.window,WM_COMMAND,TrayPause,0);waitState(State::Paused);tick();}
        const auto before=app.engine->status();for(UINT event:{PBT_APMSUSPEND,PBT_APMRESUMEAUTOMATIC,PBT_APMRESUMESUSPEND,PBT_APMRESUMECRITICAL})windowProc(app.window,WM_POWERBROADCAST,event,0);
        const auto after=app.engine->status();require(after.state==before.state && after.frames==before.frames && !probe::wakeLoads,
            "New delayed-start power routing changed ordinary active/paused recording");
    }
    windowProc(app.window,WM_COMMAND,TrayFinish,0);const auto saved=waitState(State::Idle);require(!saved.error && saved.frames==1,"Ordinary power-control output did not finish normally");
    std::cout<<"PASS suspend/resume messages preserve immediate Preparing and established Recording/Paused without delay-query work\n";
}
}
int main(){
    std::cout<<std::unitbuf;
    try{
        ownedRoot=std::filesystem::current_path()/L"fixtures"/(L"owned-"+std::to_wstring(GetCurrentProcessId()));
        require(ownedRoot.is_absolute()&&ownedRoot.parent_path()==std::filesystem::current_path()/L"fixtures","Fixture path escaped review");
        require(!std::filesystem::exists(ownedRoot),"Owned fixture directory already exists");
        repeatedPause();repeatedResume();finishThenRecord(false,false);finishThenRecord(false,true);finishThenRecord(true,false);
        recordingWhileHidden();recoveryRecording();unseenWorkerFailure(false);unseenWorkerFailure(true);
        delayedUiCancellation();immediatePowerControls();
        int closePassed=0;
        closePassed+=successfulClose(false);closePassed+=successfulClose(true);
        closePassed+=terminalFailure(0);closePassed+=terminalFailure(1);closePassed+=terminalFailure(2);
        require(closePassed==5,"A recording close-outcome case failed");
        require(!probe::finishGate.timedOut&&!probe::captureGate.timedOut,"Owned gate timed out");
        // Remove only the known synthetic leaves and their empty owned folders.
        for(const auto& directory:std::filesystem::directory_iterator(ownedRoot)) {
            require(directory.is_directory(),"Unexpected owned fixture entry");
            for(const auto& file:std::filesystem::directory_iterator(directory.path())) {
                require(file.is_regular_file()&&file.path().extension()==L".mp4","Unexpected synthetic output");
                require(std::filesystem::remove(file.path()),"Synthetic output cleanup failed");
            }
            require(std::filesystem::remove(directory.path()),"Synthetic folder cleanup failed");
        }
        require(std::filesystem::remove(ownedRoot),"Owned fixture root cleanup failed");
        std::cout<<"PASS all sixteen actual-handler/worker command, delay and exit/tray groups; no visible UI, input, hardware or real encoding.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FIXTURE_FAILURE: "<<error.what()<<'\n';return 1;}
}

#include "engine_person_camera_stub.h"

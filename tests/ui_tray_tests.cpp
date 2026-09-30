// Actual window/tray handlers, inert engine and owned hidden controls. Shell
// notification, menus, visibility, dialogs and foreground activation are seams.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace probe {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
lapse::Settings configured;
lapse::Status current;
int records=0,finishes=0,pauses=0,shows=0,hides=0,foregrounds=0,destroys=0,dialogs=0,quits=0;
int confirmations=0,confirmation=IDOK,menus=0,menuX=0,menuY=0;
UINT pauseFlags=0,finishFlags=0,exitFlags=0;
bool failAdd=false,failModify=false,failVersion=false;
bool iconic=false;
int statusQueries=0,enables=0,textWrites=0;
struct Invalidated {HWND window;bool whole;RECT rect;};
std::vector<Invalidated> invalidated;
BOOL WINAPI enable(HWND window,BOOL value){++enables;return EnableWindow(window,value);}
BOOL WINAPI setText(HWND window,LPCWSTR value){++textWrites;return SetWindowTextW(window,value);}
BOOL WINAPI invalidate(HWND window,const RECT* rect,BOOL erase){invalidated.push_back({window,rect==nullptr,rect?*rect:RECT{}});return InvalidateRect(window,rect,erase);}
BOOL WINAPI isIconic(HWND){return iconic;}
std::wstring tip,lastDialog;
std::vector<DWORD> notifications;
void resetWork(){statusQueries=enables=textWrites=0;invalidated.clear();notifications.clear();}
BOOL WINAPI notify(DWORD operation,PNOTIFYICONDATAW data){
    require(data&&data->hWnd&&data->uID==1,"Tray icon ownership changed");notifications.push_back(operation);
    if(operation==NIM_ADD){require((data->uFlags&(NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP))==(NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP),"Tray registration flags");if(failAdd)return FALSE;}
    if(operation==NIM_MODIFY && failModify)return FALSE;
    if(operation==NIM_SETVERSION){require(data->uVersion==NOTIFYICON_VERSION_4,"Tray callback version");return !failVersion;}
    if(data->uFlags&NIF_TIP)tip=data->szTip;
    return TRUE;
}
BOOL WINAPI show(HWND,int command){if(command==SW_HIDE)++hides;else ++shows;return TRUE;}
BOOL WINAPI foreground(HWND){++foregrounds;return TRUE;}
BOOL WINAPI destroy(HWND){++destroys;return TRUE;}
int WINAPI dialog(HWND,LPCWSTR message,LPCWSTR,UINT flags){lastDialog=message;++dialogs;if((flags&MB_ICONMASK)==MB_ICONQUESTION){++confirmations;return confirmation;}return IDOK;}
BOOL WINAPI point(LPPOINT value){*value={17,29};return TRUE;}
BOOL WINAPI menu(HMENU value,UINT flags,int x,int y,int,HWND,const RECT*){
    require((flags&TPM_RETURNCMD)!=0,"Menu must not dispatch commands asynchronously");++menus;menuX=x;menuY=y;
    pauseFlags=GetMenuState(value,4002,MF_BYCOMMAND);finishFlags=GetMenuState(value,4003,MF_BYCOMMAND);exitFlags=GetMenuState(value,4004,MF_BYCOMMAND);return 0;
}
UINT WINAPI registerMessage(LPCWSTR){return 0xc123;}
void WINAPI quit(int){++quits;}
}
namespace lapse {
class TrayEngine {
public:
    void configure(const Settings& settings){probe::configured=settings;}
    void refreshSources(){}
    void record(){++probe::records;probe::current.state=State::Starting;}
    void pause(){}
    void setPaused(bool paused){++probe::pauses;probe::current.state=paused?State::Paused:State::Recording;}
    void finish(){++probe::finishes;probe::current.state=State::Finishing;}
    Status status(){++probe::statusQueries;return probe::current;}
};
std::vector<Monitor> enumerateMonitors(){throw std::runtime_error("Unexpected display enumeration");}
std::vector<CameraDevice> enumerateCameras(std::wstring&){throw std::runtime_error("Unexpected camera enumeration");}
int runCameraHost(const wchar_t*){throw std::runtime_error("Unexpected application entry");}
}
#define Engine TrayEngine
#define Shell_NotifyIconW probe::notify
#define ShowWindow probe::show
#define SetForegroundWindow probe::foreground
#define DestroyWindow probe::destroy
#define MessageBoxW probe::dialog
#define GetCursorPos probe::point
#define TrackPopupMenu probe::menu
#define RegisterWindowMessageW probe::registerMessage
#define PostQuitMessage probe::quit
#define EnableWindow probe::enable
#define SetWindowTextW probe::setText
#define InvalidateRect probe::invalidate
#define IsIconic probe::isIconic
#pragma warning(push)
#pragma warning(disable: 4702)
#include "../src/main.cpp"
#pragma warning(pop)
#undef Engine
#undef Shell_NotifyIconW
#undef ShowWindow
#undef SetForegroundWindow
#undef DestroyWindow
#undef MessageBoxW
#undef GetCursorPos
#undef TrackPopupMenu
#undef RegisterWindowMessageW
#undef PostQuitMessage
#undef EnableWindow
#undef SetWindowTextW
#undef InvalidateRect
#undef IsIconic

namespace {
using probe::require;
struct Fixture {
    Fixture(){
        app.settings={};app.status=probe::current={};app.selected=-1;app.closeWhenDone=false;
        app.visibleDirty=true;app.controlsUpdated=app.trayStateValid=probe::iconic=false;
        app.advancedExpanded=false;app.advancedLimitIndex=app.advancedVisibility=-1;
        app.advancedNightState=app.nightVisibility=-1;app.nightValidation.clear();
        app.scrollX=app.scrollY=0;app.contentWidth=920;app.contentHeight=720;
        app.hiddenToTray=app.trayRegistered=app.trayNoticeShown=app.trayVersion4=app.startupComplete=false;
        app.taskbarCreated=0;app.trayTooltip.clear();
        probe::records=probe::finishes=probe::pauses=probe::shows=probe::hides=probe::foregrounds=probe::destroys=probe::dialogs=probe::quits=probe::confirmations=probe::menus=0;
        probe::failAdd=probe::failModify=probe::failVersion=false;probe::confirmation=IDOK;
        probe::notifications.clear();probe::tip.clear();probe::lastDialog.clear();
        app.window=CreateWindowExW(0,L"STATIC",L"Owned tray test",WS_OVERLAPPED,0,0,920,720,nullptr,nullptr,nullptr,nullptr);
        require(app.window&&!IsWindowVisible(app.window),"Owned hidden window creation failed");
        auto child=[&](const wchar_t* cls,DWORD style){auto w=CreateWindowExW(0,cls,L"",WS_CHILD|style,0,0,100,100,app.window,nullptr,nullptr,nullptr);require(w!=nullptr,"Child control creation failed");return w;};
        auto combo=[&](int count){auto w=child(L"COMBOBOX",CBS_DROPDOWNLIST);for(int i=0;i<count;++i)add(w,std::to_wstring(i));choose(w,0);return w;};
        app.mode=combo(6);app.interval=combo(6);app.videoSize=combo(2);app.encodingQuality=combo(3);app.encodingMode=combo(5);app.monitor=combo(1);app.camera=combo(1);
        app.stopAfter=combo(6);app.lowDisk=child(L"BUTTON",BS_AUTOCHECKBOX);SendMessageW(app.lowDisk,BM_SETCHECK,BST_CHECKED,0);
        app.nightEnabled=child(L"BUTTON",BS_AUTOCHECKBOX);app.nightDuration=combo(6);app.nightTarget=combo(3);choose(app.nightTarget,1);
        app.nightHint=child(L"STATIC",0);app.nightDetail=child(L"STATIC",0);
        app.monitors={{L"Synthetic display",{0,0,640,360},L"owned-display"}};app.cameras={{L"Synthetic camera",L"owned-camera"}};
        app.preview=child(L"STATIC",0);app.statusText=child(L"STATIC",0);
        for(auto p:{&app.refresh,&app.record,&app.pause,&app.finish,&app.folder,&app.openFolder,&app.reset,&app.forward})*p=child(L"BUTTON",BS_PUSHBUTTON);
        app.engine=std::make_unique<lapse::TrayEngine>();configure();updateControls();
    }
    ~Fixture(){removeTray();app.engine.reset();DestroyWindow(app.window);app.window=app.preview=app.statusText=nullptr;}
    void state(State value){app.status=probe::current={};app.status.state=probe::current.state=value;}
    void close(){windowProc(app.window,WM_CLOSE,0,0);}
    void command(UINT value){windowProc(app.window,WM_COMMAND,value,0);}
    void tick(){windowProc(app.window,WM_TIMER,1,0);}
    int notifications(DWORD operation){return static_cast<int>(std::count(probe::notifications.begin(),probe::notifications.end(),operation));}
};
void hideAndShow(){Fixture f;f.state(State::Recording);f.close();
    require(app.hiddenToTray&&app.trayRegistered&&app.trayVersion4&&!probe::configured.preview&&probe::hides==1&&probe::finishes==0&&probe::records==0&&probe::destroys==0&&probe::dialogs==0,"Close interrupted recording or hid without tray ownership");
    require(f.notifications(NIM_ADD)==1&&f.notifications(NIM_SETVERSION)==1&&probe::tip.find(L"Recording")!=std::wstring::npos,"Recording indicator missing");
    f.close();require(f.notifications(NIM_ADD)==1&&probe::finishes==0,"Repeated close duplicated tray or finished recording");
    windowProc(app.window,TrayMessage,MAKELPARAM(31,47),MAKELPARAM(NIN_KEYSELECT,1));
    require(!app.hiddenToTray&&probe::configured.preview&&probe::shows==1&&probe::current.state==State::Recording,"Keyboard activation failed to restore window/preview");
    f.close();windowProc(app.window,ShowExistingMessage,0,0);require(!app.hiddenToTray&&probe::shows==2,"Second normal launch did not restore hidden window");
}
void failedRegistration(){Fixture f;f.state(State::Recording);probe::failAdd=true;f.close();
    require(!app.hiddenToTray&&!app.trayRegistered&&probe::shows==1&&probe::hides==0&&probe::dialogs==1&&probe::configured.preview&&probe::finishes==0,"Tray failure stranded recorder");
    probe::confirmation=IDCANCEL;windowProc(app.window,WM_SYSCOMMAND,ExitSystemCommand,0);
    require(probe::confirmations==1&&probe::finishes==0&&!app.closeWhenDone,"Visible system-menu Exit cancellation lost recording");
}
void restart(bool fail){Fixture f;f.state(State::Recording);f.close();probe::failAdd=fail;
    windowProc(app.window,app.taskbarCreated,0,0);
    require(f.notifications(NIM_ADD)==2&&probe::finishes==0,"Explorer restart did not attempt restoration");
    require(fail?(!app.hiddenToTray&&!app.trayRegistered&&probe::configured.preview):(app.hiddenToTray&&app.trayRegistered&&app.trayVersion4&&!probe::configured.preview),"Explorer restart left recorder inaccessible");
}
void legacyFallback(){Fixture f;probe::failVersion=true;f.close();
    require(app.hiddenToTray&&app.trayRegistered&&!app.trayVersion4,"Legacy fallback lost tray icon");
    windowProc(app.window,TrayMessage,1,WM_LBUTTONUP);require(!app.hiddenToTray&&probe::shows==1,"Legacy click failed");
}
void menuAndCommands(){Fixture f;f.state(State::Recording);f.close();
    windowProc(app.window,TrayMessage,MAKELPARAM(123,456),MAKELPARAM(WM_CONTEXTMENU,1));
    require(probe::menus==1&&probe::menuX==123&&probe::menuY==456&&!(probe::pauseFlags&MF_GRAYED)&&!(probe::finishFlags&MF_GRAYED),"Keyboard tray context menu position/state failed");
    f.command(TrayPause);require(probe::current.state==State::Paused&&probe::pauses==1,"Tray Pause failed");f.tick();
    require(probe::tip.find(L"Paused")!=std::wstring::npos,"Paused indicator missing");f.command(TrayPause);require(probe::current.state==State::Recording,"Tray Resume failed");
    f.command(TrayFinish);require(probe::finishes==1&&!app.closeWhenDone&&probe::destroys==0,"Tray Finish exited application");
    f.state(State::Idle);f.tick();windowProc(app.window,TrayMessage,0,MAKELPARAM(WM_CONTEXTMENU,1));
    require((probe::pauseFlags&MF_GRAYED)&&(probe::finishFlags&MF_GRAYED)&&!(probe::exitFlags&MF_GRAYED),"Idle menu exposes invalid recording commands");
    f.command(TrayExit);require(probe::destroys==1,"Idle Exit failed");
}
void exitOutcome(bool fail){Fixture f;f.state(State::Recording);f.close();f.command(TrayExit);
    require(probe::finishes==1&&app.closeWhenDone&&!IsWindowEnabled(app.window)&&probe::destroys==0,"Exit did not wait for finalization");
    f.command(TrayExit);require(probe::finishes==1,"Repeated Exit queued duplicate finalization");
    probe::current.state=State::Idle;probe::current.recordingFailed=fail;probe::current.error=fail;probe::current.message=L"Synthetic recording recovery path";f.tick();
    require(!app.closeWhenDone,"Exit completion flag retained");
    if(fail)require(probe::destroys==0&&!app.hiddenToTray&&probe::shows==1&&IsWindowEnabled(app.window)&&probe::dialogs==2&&probe::lastDialog==probe::current.message,"Failed save was hidden or discarded on exit");
    else require(probe::destroys==1&&probe::dialogs==1,"Successful save did not exit cleanly");
}
void backgroundFailure(){Fixture f;f.state(State::Recording);f.close();probe::current.state=State::Idle;probe::current.recordingFailed=true;probe::current.message=L"Synthetic recording stopped";f.tick();
    require(!app.hiddenToTray&&probe::shows==1&&probe::dialogs==1&&probe::destroys==0,"Background recording failure did not become visible");f.tick();require(probe::dialogs==1,"Background failure notification repeated");
}
void modifierFailure(){Fixture f;f.state(State::Recording);f.close();probe::failModify=true;probe::current.state=State::Paused;f.tick();
    require(!app.hiddenToTray&&!app.trayRegistered&&probe::shows==1,"Lost tray icon left hidden recorder");
}
void cleanupAndStartup(){Fixture f;app.hiddenToTray=true;configure();require(!probe::configured.preview&&probe::records==0,"Hidden startup enabled capture preview or recording");
    require(updateTray(true),"Fixture tray registration failed");windowProc(app.window,WM_DESTROY,0,0);
    require(!app.engine&&!app.trayRegistered&&f.notifications(NIM_DELETE)==1&&probe::quits==1,"Window destruction leaked notification icon or engine");
}
void unchangedWork(){Fixture f;
    for(State state:{State::Idle,State::Starting,State::Recording,State::Paused,State::Finishing}){
        probe::current.state=state;f.tick();probe::resetWork();
        for(int i=0;i<1000;++i)f.tick();
        require(probe::statusQueries==1000&&probe::enables==0&&probe::textWrites==0&&probe::invalidated.empty()&&probe::notifications.empty(),"Unchanged visible status performed redundant visual work or lost observation");
    }
    f.close();probe::resetWork();for(int i=0;i<1000;++i)f.tick();
    require(probe::statusQueries==1000&&probe::enables==0&&probe::textWrites==0&&probe::invalidated.empty()&&probe::notifications.empty(),"Unchanged hidden status performed redundant visual work or lost observation");
    probe::resetWork();windowProc(app.window,WM_TIMER,99,0);require(probe::statusQueries==0,"Foreign timer polled engine status");
}
void scopedPaint(){Fixture f;probe::current.state=State::Recording;f.tick();app.scrollX=43;app.scrollY=61;
    probe::resetWork();probe::current.frames=1;probe::current.elapsed=.2;f.tick();
    RECT expected{app.scale(26)-43,app.contentHeight-app.scale(147)-61,app.contentWidth-app.scale(26)-43,app.contentHeight-app.scale(123)-61};
    require(probe::invalidated.size()==1&&!probe::invalidated[0].whole&&probe::invalidated[0].window==app.window&&EqualRect(&expected,&probe::invalidated[0].rect)&&probe::enables==0,"Frame update did not invalidate only the scrolled statistics region");
    probe::resetWork();probe::current.elapsed=.9;f.tick();require(probe::invalidated.empty(),"Fractional elapsed time repainted unchanged displayed seconds");
    probe::current.elapsed=1;f.tick();require(probe::invalidated.size()==1,"Displayed second did not repaint statistics");
    probe::resetWork();probe::current.message=L"New status";f.tick();require(probe::textWrites==1&&probe::invalidated.empty(),"Message update touched unrelated visuals");
    probe::resetWork();probe::current.error=true;f.tick();require(probe::invalidated.size()==1&&probe::invalidated[0].window==app.statusText,"Error color did not repaint status text");
    probe::resetWork();probe::current.preview=std::make_shared<Frame>();f.tick();require(probe::invalidated.size()==1&&probe::invalidated[0].window==app.preview,"New preview did not repaint only preview");
    probe::resetWork();probe::current.state=State::Paused;f.tick();
    require(probe::enables>0&&probe::textWrites==1&&probe::invalidated.size()==2&&!probe::invalidated[0].whole&&!probe::invalidated[1].whole,"State transition missed control, badge, or statistics update");
}
void deferredVisuals(bool minimized){Fixture f;probe::current.state=State::Recording;f.tick();
    SendMessageW(app.lowDisk,BM_SETCHECK,BST_UNCHECKED,0);choose(app.stopAfter,2);configure();
    if(minimized)probe::iconic=true;else f.close();
    probe::resetWork();probe::current.state=State::Paused;probe::current.frames=8;probe::current.elapsed=100;
    probe::current.message=L"Paused with a synthetic preview warning";probe::current.error=true;probe::current.preview=std::make_shared<Frame>();f.tick();
    require(app.visibleDirty&&app.status.state==State::Paused&&probe::enables==0&&probe::textWrites==0&&probe::invalidated.empty(),"Hidden status changed visual controls or was discarded");
    if(minimized){probe::iconic=false;windowProc(app.window,WM_SIZE,SIZE_RESTORED,0);}else f.command(TrayShow);
    wchar_t value[128]{};GetWindowTextW(app.statusText,value,128);
    require(!app.visibleDirty&&std::wstring(value)==probe::current.message&&!IsWindowEnabled(app.lowDisk)&&!probe::configured.stopOnLowDiskSpace&&probe::configured.recordingLimitSeconds==3600,"Restore lost deferred text, active lock, or Advanced settings");
    GetWindowTextW(app.pause,value,128);require(std::wstring(value)==L"&Resume","Restore left a stale Pause label");
    probe::resetWork();f.tick();require(probe::enables==0&&probe::textWrites==0&&probe::invalidated.empty(),"Restored status repeated its full refresh");
}
void unchangedTrayTip(){Fixture f;f.close();probe::resetWork();app.settings.separateFiles=true;
    require(updateTray()&&app.trayStateValid&&app.traySeparate&&probe::notifications.empty(),"Equivalent idle tray text did not cache its new inputs");
    probe::current.state=State::Recording;f.tick();require(probe::tip.find(L"desktop + camera files")!=std::wstring::npos,"Active separate-file tray summary was not refreshed");
    probe::resetWork();app.status.recordingFailed=true;updateTray();
    require(app.trayFailure&&probe::notifications.empty(),"Equivalent active tray text did not cache its failure input");
}
void nightResultDetails(){Fixture f;app.settings.layers=preset(Mode::Camera);app.advancedExpanded=true;
    SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);configure();updateControls();f.tick();
    wchar_t value[300]{};GetWindowTextW(app.nightDetail,value,300);
    require(std::wstring(value).find(L"during recording")!=std::wstring::npos,"Night help claims processed idle preview.");
    probe::current.state=State::Starting;probe::current.nightEnabled=true;probe::current.nightWaiting=true;f.tick();GetWindowTextW(app.nightDetail,value,300);
    require(std::wstring(value).find(L"first full blend")!=std::wstring::npos,"Initial night work lacks preparing detail.");
    probe::current.state=State::Recording;probe::current.nightDurationMs=5000;probe::current.night.samples=17;probe::current.night.appliedGain=2.5;probe::current.night.targetLimited=true;f.tick();GetWindowTextW(app.nightDetail,value,300);
    const std::wstring last=value;
    require(last.find(L"5.0 s")!=std::wstring::npos&&last.find(L"17 camera frames")!=std::wstring::npos&&last.find(L"2.5")!=std::wstring::npos&&last.find(L"target limited")!=std::wstring::npos,"Night result lost admitted duration/count/gain/limit facts.");
    probe::resetWork();for(int i=0;i<1000;++i)f.tick();require(probe::textWrites==0&&probe::enables==0&&probe::invalidated.empty(),"Unchanged night facts repeated visual work.");
    f.close();probe::resetWork();probe::current.nightDurationMs=10000;probe::current.night.samples=31;probe::current.night.appliedGain=3;f.tick();
    require(app.visibleDirty&&probe::textWrites==0&&probe::invalidated.empty(),"Hidden night results repainted the window.");
    f.command(TrayShow);GetWindowTextW(app.nightDetail,value,300);require(std::wstring(value).find(L"10.0 s")!=std::wstring::npos&&std::wstring(value).find(L"31 camera frames")!=std::wstring::npos,"Restore lost deferred night facts.");
    app.advancedExpanded=false;probe::resetWork();probe::current.night.samples=32;f.tick();require(probe::textWrites==0,"Collapsed night facts rewrote hidden detail.");
    app.advancedExpanded=true;updateNightText();GetWindowTextW(app.nightDetail,value,300);require(std::wstring(value).find(L"32 camera frames")!=std::wstring::npos,"Expanding night details left stale facts.");
}
}
int main(){std::cout<<std::unitbuf;try{
    hideAndShow();failedRegistration();restart(false);restart(true);legacyFallback();menuAndCommands();exitOutcome(false);exitOutcome(true);backgroundFailure();modifierFailure();cleanupAndStartup();
    unchangedWork();scopedPaint();deferredVisuals(false);deferredVisuals(true);unchangedTrayTip();nightResultDetails();
    std::cout<<"PASS 17 tray/status cases: owned hidden windows, synthetic engine, no tray icons, captures, input, or settings writes.\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}

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
std::wstring tip,lastDialog;
std::vector<DWORD> notifications;
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
    Status status(){return probe::current;}
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

namespace {
using probe::require;
struct Fixture {
    Fixture(){
        app.settings={};app.status=probe::current={};app.selected=-1;app.closeWhenDone=false;
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
}
int main(){std::cout<<std::unitbuf;try{
    hideAndShow();failedRegistration();restart(false);restart(true);legacyFallback();menuAndCommands();exitOutcome(false);exitOutcome(true);backgroundFailure();modifierFailure();cleanupAndStartup();
    std::cout<<"PASS 11 tray lifecycle cases: owned hidden windows, synthetic engine, no tray icons, captures, input, or settings writes.\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}

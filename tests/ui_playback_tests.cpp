// Real owned HOTKEY controls/dialog handlers, inert recording engine and
// synthetic global registrations. No keyboard injection or real shortcuts.
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <vector>
#include <filesystem>
#include <fstream>
namespace playbackProbe {
struct Registration {HWND window{};int id=0;UINT modifiers=0,key=0;};
std::vector<Registration> registrations;
UINT blockedKey=0;int attempts=0,removals=0;
BOOL WINAPI registerHotkey(HWND window,int id,UINT modifiers,UINT key){
    ++attempts;
    if(!(modifiers&MOD_NOREPEAT) || key==blockedKey)return FALSE;
    for(const auto& entry:registrations)if(entry.id==id || (entry.modifiers==modifiers && entry.key==key))return FALSE;
    registrations.push_back({window,id,modifiers,key});return TRUE;
}
BOOL WINAPI unregisterHotkey(HWND window,int id){
    for(auto it=registrations.begin();it!=registrations.end();++it)if(it->window==window && it->id==id){registrations.erase(it);++removals;return TRUE;}
    return FALSE;
}
std::function<void(HWND,LPARAM)> dialogScript;
INT_PTR dialogOutcome=0;int dialogCalls=0;bool failDialog=false;
INT_PTR WINAPI ownedDialog(HINSTANCE,LPCDLGTEMPLATEW,HWND,DLGPROC,LPARAM);
BOOL WINAPI endDialog(HWND,INT_PTR outcome){dialogOutcome=outcome;return TRUE;}
}
#define RegisterHotKey playbackProbe::registerHotkey
#define UnregisterHotKey playbackProbe::unregisterHotkey
#define DialogBoxIndirectParamW playbackProbe::ownedDialog
#define EndDialog playbackProbe::endDialog
#define main trayFixtureMain
#include "ui_tray_tests.cpp"
#undef main
#undef EndDialog
#undef DialogBoxIndirectParamW
#undef RegisterHotKey
#undef UnregisterHotKey

INT_PTR WINAPI playbackProbe::ownedDialog(HINSTANCE instance,LPCDLGTEMPLATEW resource,HWND owner,DLGPROC procedure,LPARAM parameter){
    ++dialogCalls;dialogOutcome=0;if(failDialog)return -1;
    const bool enabled=IsWindowEnabled(owner)!=FALSE;EnableWindow(owner,FALSE);
    HWND window=CreateDialogIndirectParamW(instance,resource,owner,procedure,parameter);
    probe::require(window && !IsWindowVisible(window),"Playback fixture must create only an owned hidden dialog.");
    try{if(dialogScript)dialogScript(window,parameter);else playbackProc(window,WM_COMMAND,IDCANCEL,0);}
    catch(...){if(IsWindow(window))DestroyWindow(window);EnableWindow(owner,enabled);throw;}
    if(IsWindow(window))DestroyWindow(window);EnableWindow(owner,enabled);return dialogOutcome;
}
namespace {
uint16_t key(UINT virtualKey,UINT flags=HOTKEYF_CONTROL|HOTKEYF_ALT){return static_cast<uint16_t>(virtualKey|(flags<<8));}
std::wstring playbackText(HWND window){wchar_t value[1024]{};GetWindowTextW(window,value,1024);return value;}
struct PlaybackFixture {
    Fixture owned;
    PlaybackFixture(){
        playbackProbe::registrations.clear();playbackProbe::attempts=playbackProbe::removals=0;playbackProbe::blockedKey=0;
        playbackProbe::dialogScript={};playbackProbe::dialogCalls=0;playbackProbe::failDialog=false;
        app.pauseHotkey=app.stopHotkey=0;app.pauseHotkeyId=app.stopHotkeyId=0;app.hotkeyWarning.clear();app.recordedOutputFps=DefaultOutputFps;app.advancedOutputFps=-1;
        app.advanced=CreateWindowExW(0,L"BUTTON",L"Advanced",WS_CHILD|BS_AUTOCHECKBOX,0,0,240,30,app.window,nullptr,nullptr,nullptr);
        app.watermarkConfigure=CreateWindowExW(0,L"BUTTON",L"Watermark",WS_CHILD,0,0,202,30,app.window,nullptr,nullptr,nullptr);
        app.watermarkSummary=CreateWindowExW(0,L"STATIC",L"Off",WS_CHILD,0,0,160,30,app.window,nullptr,nullptr,nullptr);
        app.playbackConfigure=CreateWindowExW(0,L"BUTTON",L"Playback && shortcuts...",WS_CHILD|WS_TABSTOP,0,0,202,30,app.window,nullptr,nullptr,nullptr);
        fonts();app.advancedVisibility=-1;app.advancedExpanded=false;app.advancedCaption.clear();app.advancedTooltip.clear();updateAdvanced();
    }
    ~PlaybackFixture(){unregisterRecordingHotkeys();app.pauseHotkey=app.stopHotkey=0;app.advanced=app.watermarkConfigure=app.watermarkSummary=app.playbackConfigure=nullptr;app.customDialog=nullptr;}
};
void acceptPlayback(HWND window){playbackProc(window,WM_COMMAND,IDOK,0);}
void sendHotkey(int id,uint16_t binding){windowProc(app.window,WM_HOTKEY,id,MAKELPARAM(hotkeyRegistrationModifiers(binding)&~MOD_NOREPEAT,LOBYTE(binding)));}
void cancellationAndValidation(){
    PlaybackFixture fixture;const int configured=probe::enables;
    playbackProbe::dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<PlaybackDraft*>(parameter);
        require(draft.fps==30 && !draft.pauseHotkey && !draft.stopHotkey,"New playback defaults changed.");
        require(GetNextDlgTabItem(window,draft.first,FALSE)==draft.second && GetNextDlgTabItem(window,draft.second,FALSE)==draft.clearPause &&
            GetNextDlgTabItem(window,draft.clearPause,FALSE)==draft.stop,"Playback native tab order skipped a shortcut.");
        SetWindowTextW(draft.first,L"60");SendMessageW(draft.second,HKM_SETHOTKEY,key('P'),0);playbackProc(window,WM_CLOSE,0,0);};
    editPlayback();require(app.settings.outputFps==30 && !app.pauseHotkey && playbackProbe::registrations.empty(),"Cancel committed a draft or registered a shortcut.");
    playbackProbe::dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<PlaybackDraft*>(parameter);
        for(const wchar_t* invalid:{L"",L"0",L"121",L"29.97",L"60junk",L"-1",L"999999999999999999999999999999"}){
            SetWindowTextW(draft.first,invalid);acceptPlayback(window);require(!playbackProbe::dialogOutcome && !playbackText(draft.error).empty(),"Invalid output fps was accepted.");}
        SetWindowTextW(draft.first,L"60");SendMessageW(draft.second,HKM_SETHOTKEY,key('P',0),0);acceptPlayback(window);require(!playbackProbe::dialogOutcome,"Unmodified letter shortcut was accepted.");
        SendMessageW(draft.second,HKM_SETHOTKEY,key('P'),0);SendMessageW(draft.stop,HKM_SETHOTKEY,key('P'),0);acceptPlayback(window);
        require(!playbackProbe::dialogOutcome && playbackProbe::registrations.empty(),"Duplicate shortcuts partially registered or were accepted.");
        SendMessageW(draft.stop,HKM_SETHOTKEY,key('S'),0);acceptPlayback(window);require(playbackProbe::dialogOutcome==IDOK,"Valid fps and shortcuts did not commit.");};
    editPlayback();require(app.settings.outputFps==60 && app.pauseHotkey==key('P') && app.stopHotkey==key('S') && playbackProbe::registrations.size()==2 && probe::configured.outputFps==60,
        "Accepted settings did not reach the engine or global registrations.");
    require(configured<=probe::enables,"Fixture control accounting failed.");
    std::cout<<"PASS playback native draft cancellation, tab order, strict integer fps, shortcut validation and engine configuration\n";
}
void conflictsSwapsAndCleanup(){
    PlaybackFixture fixture;std::wstring error;require(setRecordingHotkeys(key('P'),key('S'),error),"Initial hotkeys failed.");
    const int pauseId=app.pauseHotkeyId,stopId=app.stopHotkeyId;const int attempts=playbackProbe::attempts;
    require(setRecordingHotkeys(key('S'),key('P'),error) && app.pauseHotkeyId==stopId && app.stopHotkeyId==pauseId && playbackProbe::attempts==attempts,"Swapped actions failed or duplicated system registrations.");
    playbackProbe::blockedKey='X';require(!setRecordingHotkeys(key('Q'),key('X'),error) && !error.empty(),"Conflicting stop shortcut was accepted.");
    require(app.pauseHotkey==key('S') && app.stopHotkey==key('P') && playbackProbe::registrations.size()==2,"Conflict lost working bindings or leaked staged registration.");
    playbackProbe::dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<PlaybackDraft*>(parameter);SetWindowTextW(draft.first,L"25");
        SendMessageW(draft.second,HKM_SETHOTKEY,key('Q'),0);SendMessageW(draft.stop,HKM_SETHOTKEY,key('X'),0);acceptPlayback(window);
        require(!playbackProbe::dialogOutcome && playbackText(draft.error).find(L"unavailable")!=std::wstring::npos,"Conflict closed the dialog or lacked actionable feedback.");
        playbackProc(window,WM_COMMAND,IDCANCEL,0);};
    editPlayback();require(app.settings.outputFps==30 && app.pauseHotkey==key('S') && app.stopHotkey==key('P'),"Conflicted dialog changed committed settings.");
    playbackProbe::blockedKey=0;require(setRecordingHotkeys(key(VK_LEFT,HOTKEYF_CONTROL|HOTKEYF_EXT),0,error),"Native extended navigation shortcut failed.");
    const int extendedId=app.pauseHotkeyId;require(setRecordingHotkeys(key(VK_LEFT,HOTKEYF_CONTROL),0,error) && app.pauseHotkeyId==extendedId,"EXT-only display change duplicated a native combination.");
    windowProc(app.window,WM_DESTROY,0,0);require(playbackProbe::registrations.empty() && !app.pauseHotkeyId && !app.stopHotkeyId,"WM_DESTROY leaked global shortcut registrations.");
    std::cout<<"PASS atomic shortcut conflicts, swaps, native extended-key identity and destruction cleanup\n";
}
void shortcutLifecycle(){
    PlaybackFixture fixture;std::wstring error;require(setRecordingHotkeys(key('P'),key('S'),error),"Shortcut seed failed.");
    app.hiddenToTray=true;probe::current.state=State::Recording;app.status.state=State::Idle;
    sendHotkey(app.pauseHotkeyId,app.pauseHotkey);require(probe::current.state==State::Paused && probe::pauses==1 && !probe::records,"Hidden pause failed to use fresh engine state.");
    sendHotkey(app.pauseHotkeyId,app.pauseHotkey);require(probe::current.state==State::Recording && probe::pauses==2,"Hidden resume did not toggle recording.");
    const int pauseCalls=probe::pauses;sendHotkey(app.pauseHotkeyId,key('Q'));require(probe::pauses==pauseCalls,"Stale queued key for a reused id dispatched another action.");
    for(auto state:{State::Idle,State::Waiting,State::Starting,State::Finishing}){probe::current.state=state;sendHotkey(app.pauseHotkeyId,app.pauseHotkey);require(probe::pauses==pauseCalls && !probe::records,"Pause started or changed an ineligible state.");}
    for(auto state:{State::Waiting,State::Starting,State::Recording,State::Paused}){probe::current.state=state;const int finishes=probe::finishes;sendHotkey(app.stopHotkeyId,app.stopHotkey);require(probe::finishes==finishes+1,"Stop failed to finish/cancel an eligible state.");}
    for(auto state:{State::Idle,State::Finishing}){probe::current.state=state;const int finishes=probe::finishes;sendHotkey(app.stopHotkeyId,app.stopHotkey);require(probe::finishes==finishes,"Stop changed an ineligible state.");}
    probe::current.state=State::Recording;const int finishes=probe::finishes;
    for(int blocker=0;blocker<5;++blocker){if(blocker==0)app.customDialog=app.pause;if(blocker==1)app.trayMenuOpen=true;if(blocker==2)app.closeWhenDone=true;if(blocker==3)app.failureNotice=FailureNotice::Presenting;if(blocker==4)EnableWindow(app.window,FALSE);
        sendHotkey(app.stopHotkeyId,app.stopHotkey);require(probe::finishes==finishes,"Shortcut interrupted an owned modal or shutdown.");
        app.customDialog=nullptr;app.trayMenuOpen=app.closeWhenDone=false;app.failureNotice=FailureNotice::None;EnableWindow(app.window,TRUE);}
    std::cout<<"PASS minimized shortcuts, fresh-state pause/resume, start cancellation, stale-message rejection and modal/state guards\n";
}
void dialogStateAndGeometry(){
    PlaybackFixture fixture;
    for(auto state:{State::Waiting,State::Starting,State::Recording,State::Paused,State::Finishing}){
        fixture.owned.state(state);playbackProbe::dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<PlaybackDraft*>(parameter);
            require(draft.readOnly && !IsWindowEnabled(draft.first) && !IsWindowEnabled(draft.second) && !IsWindowEnabled(draft.stop) && !IsWindowEnabled(draft.okay),"Session settings were editable.");
            SetWindowTextW(draft.first,L"120");acceptPlayback(window);};editPlayback();require(app.settings.outputFps==30 && playbackProbe::registrations.empty(),"Forged active OK changed frozen settings.");}
    fixture.owned.state(State::Idle);playbackProbe::dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<PlaybackDraft*>(parameter);
        for(int dpi:{96,144,192}){draft.dpi=dpi;customFont(window,draft);SetWindowPos(window,nullptr,0,0,draft.scale(460),draft.scale(450),SWP_NOZORDER|SWP_NOACTIVATE);playbackLayout(window,draft);
            RECT pause{},clear{},stop{},help{},error{},okay{};GetWindowRect(draft.second,&pause);GetWindowRect(draft.clearPause,&clear);GetWindowRect(draft.stop,&stop);GetWindowRect(draft.help,&help);GetWindowRect(draft.error,&error);GetWindowRect(draft.okay,&okay);
            require(pause.right<clear.left && pause.bottom<stop.top && stop.bottom<help.top && help.bottom<error.top && error.bottom<okay.top,"Playback dialog controls overlap at supported dpi.");
            SetWindowPos(window,nullptr,0,0,draft.scale(280),draft.scale(160),SWP_NOZORDER|SWP_NOACTIVATE);playbackLayout(window,draft);playbackReveal(window,draft,draft.cancel);
            RECT bounds{},client{};GetWindowRect(draft.cancel,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
            require(bounds.left>=0 && bounds.top>=0 && bounds.right<=client.right && bounds.bottom<=client.bottom,"Small viewport cannot reveal its Cancel button.");}
        playbackProc(window,WM_COMMAND,IDCANCEL,0);};editPlayback();
    app.dpi=96;app.advancedExpanded=true;layout();RECT watermark{},summary{},playback{};GetWindowRect(app.watermarkConfigure,&watermark);GetWindowRect(app.watermarkSummary,&summary);GetWindowRect(app.playbackConfigure,&playback);
    require(watermark.bottom<=summary.top && summary.bottom<playback.top && watermark.left==playback.left,"Panel playback entry overlaps the watermark controls or leaves the panel column.");
    app.advancedExpanded=false;updateAdvanced();require(!(GetWindowLongPtrW(app.playbackConfigure,GWL_STYLE)&WS_VISIBLE),"Playback entry leaked into the collapsed main interface.");
    playbackProbe::failDialog=true;editPlayback();require(probe::lastDialog.find(L"could not be opened")!=std::wstring::npos && !app.customDialog,"Failed dialog lost recovery feedback or modal ownership.");
    std::cout<<"PASS active-settings lock, dialog DPI/scroll geometry, compact main row and creation failure recovery\n";
}
struct OwnedPreferences {
    std::filesystem::path directory;std::wstring previous=app.preferences;
    OwnedPreferences(){const auto base=std::filesystem::current_path();directory=base/(L"ui-playback-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(directory.is_absolute() && directory.parent_path()==base && std::filesystem::create_directory(directory),"Cannot create owned playback preferences directory.");app.preferences=(directory/L"settings.ini").wstring();}
    ~OwnedPreferences(){WritePrivateProfileStringW(nullptr,nullptr,nullptr,app.preferences.c_str());std::error_code error;std::filesystem::remove(std::filesystem::path(app.preferences),error);std::filesystem::remove(directory,error);app.preferences=previous;}
    void write(const wchar_t* name,const wchar_t* value){require(WritePrivateProfileStringW(L"Settings",name,value,app.preferences.c_str())!=FALSE,"Cannot seed owned playback preference.");}
};
void persistedSettingsAndTiming(){
    PlaybackFixture fixture;OwnedPreferences preferencesFile;std::wstring error;app.settings.outputFps=24;require(setRecordingHotkeys(key('P'),key('S'),error) && savePreferences(),"Playback preferences save failed.");
    app.settings.outputFps=120;require(setRecordingHotkeys(0,0,error),"Shortcut disabling failed.");preferences(false);
    require(app.settings.outputFps==24 && app.recordedOutputFps==24 && app.pauseHotkey==key('P') && app.stopHotkey==key('S') && playbackProbe::registrations.size()==2,"Playback preference roundtrip lost exact values.");
    for(const wchar_t* invalid:{L"",L"0",L"121",L"24.0",L"024",L"60junk",L"-1",L"999999999999999999999999999999999"}){preferencesFile.write(L"OutputFps",invalid);preferences(false);require(app.settings.outputFps==30,"Malformed fps enabled a prefix or retained prior value.");}
    preferencesFile.write(L"OutputFps",L"120");preferencesFile.write(L"PauseHotkey",std::to_wstring(key('P')).c_str());preferencesFile.write(L"StopHotkey",std::to_wstring(key('P')).c_str());preferences(false);
    require(app.pauseHotkey==key('P') && !app.stopHotkey && app.settings.outputFps==120,"Persisted duplicate did not preserve pause and disable stop deterministically.");
    preferencesFile.write(L"PauseHotkey",L"65536");preferencesFile.write(L"StopHotkey",L"1347junk");preferences(false);require(!app.pauseHotkey && !app.stopHotkey && playbackProbe::registrations.empty(),"Malformed hotkeys were coerced or left registered.");
    preferencesFile.write(L"PauseHotkey",std::to_wstring(key('P')).c_str());playbackProbe::blockedKey='P';preferences(false);
    require(app.pauseHotkey==key('P') && !app.pauseHotkeyId && !app.hotkeyWarning.empty() && playbackProbe::registrations.empty(),"Unavailable saved shortcut was silently reported as active or forgotten.");
    playbackProbe::blockedKey=0;preferences(false);require(app.pauseHotkeyId && app.hotkeyWarning.empty(),"Available saved shortcut failed to recover on reload.");
    Status status;status.state=State::Recording;status.frames=120;status.elapsed=17;wchar_t progress[160]{};require(trayProgressText(status,progress) && std::wstring(progress).find(L"00:00:01 video")!=std::wstring::npos,"Tray duration retained fixed30fps.");
    app.settings.outputFps=24;require(trayProgressText(status,progress) && std::wstring(progress).find(L"00:00:01 video")!=std::wstring::npos,"Editing next-recording fps changed previous recording duration.");
    std::cout<<"PASS exact preference roundtrip, malformed/default recovery, saved-shortcut conflicts and stable recorded-fps timing\n";
}
}
int main(){
    std::cout<<std::unitbuf;INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&controls);
    try{cancellationAndValidation();conflictsSwapsAndCleanup();shortcutLifecycle();dialogStateAndGeometry();persistedSettingsAndTiming();std::cout<<"All playback UI cases passed with owned controls and synthetic registrations.\n";return 0;}
    catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}

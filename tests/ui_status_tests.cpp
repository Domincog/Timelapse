// Owned hidden native dialogs and inert engine. Nested modal calls are scripted;
// no device capture, ordinary application launch, persistent UI or settings I/O.
#include <windows.h>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>
namespace {
std::function<void(HWND,DLGPROC,LPARAM)> statusScript;
struct ModalOutcome {HWND window{};INT_PTR value=0;};
std::vector<ModalOutcome> modalOutcomes;
int statusDialogCalls=0,flashes=0,beeps=0;
int statusTextComboMessages=0,statusTextLimitMessages=0,statusTextSelectionMessages=0,statusMenus=0;
LRESULT WINAPI statusSend(HWND window,UINT message,WPARAM wp,LPARAM lp){
    wchar_t title[32]{};GetWindowTextW(GetParent(window),title,32);
    if(GetDlgCtrlID(window)==5601 && std::wcscmp(title,L"Status")==0){
        if(message>=CB_GETEDITSEL && message<=CB_MSGMAX)++statusTextComboMessages;
        if(message==EM_SETLIMITTEXT)++statusTextLimitMessages;
        if(message==EM_SETSEL)++statusTextSelectionMessages;
    }
    return SendMessageW(window,message,wp,lp);
}
// The Status window has no popup menus; count any attempt to open one.
BOOL WINAPI statusMenu(HMENU,UINT,int,int,int,HWND,const RECT*){++statusMenus;return 0;}
bool statusDialogFailure=false;
INT_PTR WINAPI statusOwnedDialog(HINSTANCE,LPCDLGTEMPLATEW,HWND,DLGPROC,LPARAM);
BOOL WINAPI statusOwnedEndDialog(HWND window,INT_PTR value){for(auto& modal:modalOutcomes)if(modal.window==window)modal.value=value;return TRUE;}
BOOL WINAPI statusFlash(PFLASHWINFO){++flashes;return TRUE;}
BOOL WINAPI statusBeep(UINT){++beeps;return TRUE;}
// Never claim real system-wide shortcuts from a test.
std::vector<int> registeredHotkeys;
BOOL WINAPI statusRegister(HWND,int id,UINT,UINT){registeredHotkeys.push_back(id);return TRUE;}
BOOL WINAPI statusUnregister(HWND,int id){for(auto i=registeredHotkeys.begin();i!=registeredHotkeys.end();++i)if(*i==id){registeredHotkeys.erase(i);return TRUE;}return FALSE;}
}
#define DialogBoxIndirectParamW statusOwnedDialog
#define EndDialog statusOwnedEndDialog
#define FlashWindowEx statusFlash
#define MessageBeep statusBeep
#define RegisterHotKey statusRegister
#define UnregisterHotKey statusUnregister
#define SendMessageW statusSend
#define TrackPopupMenu statusMenu
#define main sourceFixtureMain
#include "ui_source_tests.cpp"
#undef main
#undef TrackPopupMenu
#undef SendMessageW
#undef UnregisterHotKey
#undef RegisterHotKey
#undef MessageBeep
#undef FlashWindowEx
#undef EndDialog
#undef DialogBoxIndirectParamW
namespace {
INT_PTR WINAPI statusOwnedDialog(HINSTANCE instance,LPCDLGTEMPLATEW resource,HWND owner,DLGPROC procedure,LPARAM parameter){
    ++statusDialogCalls;if(statusDialogFailure)return -1;
    const bool enabled=IsWindowEnabled(owner)!=FALSE;EnableWindow(owner,FALSE);
    HWND window=CreateDialogIndirectParamW(instance,resource,owner,procedure,parameter);
    require(window && !IsWindowVisible(window),"Only an owned hidden dialog may be created.");modalOutcomes.push_back({window,0});
    try{if(statusScript)statusScript(window,procedure,parameter);else procedure(window,WM_COMMAND,IDCANCEL,0);}
    catch(...){modalOutcomes.pop_back();if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))EnableWindow(owner,enabled);throw;}
    const auto result=modalOutcomes.back().value;modalOutcomes.pop_back();if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))EnableWindow(owner,enabled);return result;
}
INT_PTR outcome(){return modalOutcomes.back().value;}
bool visible(HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;}
std::wstring fullText(HWND child){std::wstring value(size_t(GetWindowTextLengthW(child))+1,L' ');value.resize(size_t(GetWindowTextW(child,value.data(),int(value.size()))));return value;}
bool focused(HWND child){return GetFocus()==child;}
struct StatusFixture : HiddenFixture {
    StatusFixture(){
        const auto make=[&](const wchar_t* type,int id){HWND child=CreateWindowExW(0,type,L"",WS_CHILD|(std::wcscmp(type,L"BUTTON")==0?BS_PUSHBUTTON:0),0,0,100,24,app.window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);require(child!=nullptr,"Status control creation.");return child;};
        app.liveStatusSummary=make(L"STATIC",StatusSummaryLine);app.liveStatusSet=make(L"BUTTON",SetStatusButton);app.liveStatusClear=make(L"BUTTON",ClearStatusButton);
        app.nightHint=app.nightDetail=nullptr;for(auto& tab:app.tabs)tab=nullptr;app.customDialog=nullptr;app.panelTab=CaptureTab;app.hiddenToTray=false;app.trayRegistered=false;
        app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;choose(app.interval,2);choose(app.videoSize,0);choose(app.stopAfter,0);
        seed(false);
        app.liveStatus={};app.liveStatusSequence=app.statusNoticeSequence=app.statusNoticePhase=0;app.statusNoticeIcon=StatusIcon::None;
        app.liveStatusCaption={};app.settings.statusFeed={};app.settings.saveStatusLog=false;
        app.statusDraftKind=0;app.statusDraftTimerMs=25*60000;app.statusDraftBreakMs=5*60000;app.statusDraftRepeat=false;
        app.statusHotkey=0;app.statusHotkeyId=0;
        statusScript={};statusDialogCalls=flashes=beeps=0;statusDialogFailure=false;lapse::statusCalls=0;lapse::engineStatus={};
        statusTextComboMessages=statusTextLimitMessages=statusTextSelectionMessages=statusMenus=0;
        configure();updateControls();
    }
    ~StatusFixture(){unregisterRecordingHotkeys();app.pauseHotkey=app.stopHotkey=app.statusHotkey=0;app.liveStatusSummary=app.liveStatusSet=app.liveStatusClear=nullptr;}
};
StatusDraft& draftOf(LPARAM parameter){return *reinterpret_cast<StatusDraft*>(parameter);}
void select(HWND window,HWND box,int id,int index){choose(box,index);statusProc(window,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(box));}
void type(HWND,HWND box,int,const wchar_t* text){SetWindowTextW(box,text);}
void check(HWND window,HWND box,bool checked){SendMessageW(box,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);statusProc(window,WM_COMMAND,MAKEWPARAM(StatusRepeat,BN_CLICKED),reinterpret_cast<LPARAM>(box));}
void submit(HWND window){statusProc(window,WM_COMMAND,MAKEWPARAM(IDOK,BN_CLICKED),0);}

void noteAndStopwatch(){
    StatusFixture owned;
    require(caption(app.liveStatusSummary).find(L"No status")==0 && !IsWindowEnabled(app.liveStatusClear),"Empty status summary or Clear state wrong.");
    statusScript=[&](HWND window,DLGPROC procedure,LPARAM parameter){auto& draft=draftOf(parameter);
        require(procedure==statusProc && caption(draft.text).empty() && choice(draft.kind)==0 && !visible(draft.minutes) && !visible(draft.breakMinutes),"Text-only default dialog wrong.");
        require(!draft.illustration.pixels.empty() && draft.renderer.lastBounds().right>draft.renderer.lastBounds().left,"Dialog illustration did not draw the replacement preview.");
        type(window,draft.text,StatusTextBox,L"  First task done!!  ");submit(window);require(outcome()==IDOK,"Valid note was rejected.");
    };
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(SetStatusButton,BN_CLICKED),reinterpret_cast<LPARAM>(app.liveStatusSet));
    require(app.liveStatus.kind==StatusKind::Note && std::wstring(app.liveStatus.text.data())==L"First task done!!" && lapse::statusCalls==1 &&
        lapse::engineStatus.sequence==app.liveStatus.sequence && lapse::engineStatus.startTick==app.liveStatus.startTick,"Note did not reach the engine trimmed and once.");
    require(caption(app.liveStatusSummary)==L"First task done!!" && IsWindowEnabled(app.liveStatusClear),"Note summary or Clear wrong.");
    const auto firstSequence=app.liveStatus.sequence;
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        require(caption(draft.text)==L"First task done!!" && IsWindowEnabled(draft.clear) && GetDlgItem(window,5611)==draft.saveLogBox,"Dialog did not reopen with the current status.");
        select(window,draft.kind,StatusKindBox,1);require(!visible(draft.minutes),"Stopwatch showed timer fields.");
        type(window,draft.text,StatusTextBox,L"Shower");submit(window);
    };
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(TraySetStatus,0),0);
    require(app.liveStatus.kind==StatusKind::Stopwatch && app.liveStatus.sequence==firstSequence+1 && lapse::statusCalls==2 && app.statusDraftKind==1,"Tray stopwatch failed.");
    require(caption(app.liveStatusSummary).rfind(L"Shower · 0:0",0)==0,"Stopwatch summary wrong.");
    // Re-setting the same text is a new status (the stopwatch restarts).
    statusScript=[&](HWND window,DLGPROC,LPARAM){submit(window);};
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(SetStatusButton,BN_CLICKED),reinterpret_cast<LPARAM>(app.liveStatusSet));
    require(app.liveStatus.sequence==firstSequence+2,"Same text was not a new status.");
    std::cout<<"PASS note and stopwatch from the panel and tray, trimmed text, engine handoff, summary and restart\n";
}
void timerAndValidation(){
    StatusFixture owned;
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        submit(window);require(!outcome() && focused(draft.text) && !caption(draft.error).empty(),"Empty status was accepted.");
        type(window,draft.text,StatusTextBox,L"Work on Essay");select(window,draft.kind,StatusKindBox,2);
        require(visible(draft.minutes) && visible(draft.repeat) && !visible(draft.breakMinutes) && caption(draft.minutes)==L"25","Timer fields or default minutes wrong.");
        for(const wchar_t* bad:{L"abc",L"0",L"601",L"0.001",L""}){type(window,draft.minutes,StatusMinutes,bad);submit(window);
            require(!outcome() && GetFocus()==draft.minutes,"Invalid timer length accepted or focus lost.");}
        type(window,draft.minutes,StatusMinutes,L"60");check(window,draft.repeat,true);require(visible(draft.breakMinutes) && caption(draft.breakMinutes)==L"5","Break field or default wrong.");
        type(window,draft.breakMinutes,StatusBreak,L"0");submit(window);require(!outcome() && GetFocus()==draft.breakMinutes,"Invalid break accepted.");
        type(window,draft.breakMinutes,StatusBreak,L"7.5");submit(window);require(outcome()==IDOK,"Valid repeating timer rejected.");
    };
    editStatus();
    require(app.liveStatus.kind==StatusKind::Timer && app.liveStatus.durationMs==3600000 && app.liveStatus.breakMs==450000 &&
        app.statusDraftKind==2 && app.statusDraftTimerMs==3600000 && app.statusDraftBreakMs==450000 && app.statusDraftRepeat,"Repeating timer or remembered choices wrong.");
    require(caption(app.liveStatusSummary)==L"Work on Essay · 1:00:00 left","Timer summary wrong.");
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        require(choice(draft.kind)==2 && caption(draft.minutes)==L"60" && caption(draft.breakMinutes)==L"7.5" && SendMessageW(draft.repeat,BM_GETCHECK,0,0)==BST_CHECKED,"Dialog lost the last timer choices.");
        require(GetWindow(draft.text,GW_CHILD)==nullptr,"Status text box has a nested combo edit.");
        SetWindowTextW(draft.text,L"");const std::wstring pasted(80,L'x');SendMessageW(draft.text,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(pasted.c_str()));
        require(GetWindowTextLengthW(draft.text)==StatusMaxTextLength,"Text box accepted more than 60 characters.");
        statusProc(window,WM_COMMAND,MAKEWPARAM(IDCANCEL,BN_CLICKED),0);
    };
    const auto before=app.liveStatus.sequence;editStatus();require(app.liveStatus.sequence==before,"Cancel changed the status.");
    std::cout<<"PASS timer and repeating breaks with exact minutes, field validation and focus, remembered choices and Cancel\n";
}
struct TextNotifications {HWND text{};int changes=0,focus=0;};
LRESULT CALLBACK observeStatusText(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR reference){
    if(message==WM_COMMAND){auto& seen=*reinterpret_cast<TextNotifications*>(reference);if(reinterpret_cast<HWND>(lp)==seen.text){
        if(HIWORD(wp)==EN_CHANGE)++seen.changes;
        if(HIWORD(wp)==EN_SETFOCUS)++seen.focus;
    }}
    const auto result=DefSubclassProc(window,message,wp,lp);
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,observeStatusText,id);
    return result;
}
struct TextObservationGuard {HWND window{};~TextObservationGuard(){if(IsWindow(window))RemoveWindowSubclass(window,observeStatusText,81);}};
void plainTextAndList(){
    StatusFixture owned;
    app.liveStatus.kind=StatusKind::Note;app.liveStatus.sequence=7;setStatusText(app.liveStatus,L"Current status");
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        wchar_t typeName[32]{};require(GetClassNameW(draft.text,typeName,32)>0 && _wcsicmp(typeName,L"EDIT")==0,"Status text is not a native EDIT control.");
        require(!(GetWindowLongPtrW(draft.text,GWL_STYLE)&ES_MULTILINE) && (GetWindowLongPtrW(draft.text,GWL_STYLE)&ES_AUTOHSCROLL) &&
            SendMessageW(draft.text,EM_GETLIMITTEXT,0,0)==StatusMaxTextLength,"Status text is not a bounded single-line editor.");
        DWORD first=99,last=99;SendMessageW(draft.text,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
        require(GetFocus()==draft.text && first==0 && last==std::wcslen(L"Current status") && statusTextSelectionMessages>0,
            "Reopening did not focus and select the complete status text.");
        require(GetNextDlgTabItem(window,draft.text,FALSE)==draft.kind,"Something interrupted the text/Show tab order.");
        TextNotifications seen{draft.text};require(SetWindowSubclass(window,observeStatusText,81,reinterpret_cast<DWORD_PTR>(&seen))!=FALSE,"Cannot observe owned native edit notifications.");
        TextObservationGuard observation{window};
        const auto before=draft.illustration.pixels;
        for(wchar_t character:std::wstring(L"New note"))SendMessageW(draft.text,WM_CHAR,character,1);
        require(fullText(draft.text)==L"New note" && seen.changes>=8 && before!=draft.illustration.pixels && !outcome(),
            "Native typing did not replace selection and update the live preview through EN_CHANGE.");
        SendMessageW(draft.text,WM_KEYDOWN,VK_DOWN,1);
        require(fullText(draft.text)==L"New note" && statusMenus==0,"An editor arrow key opened a menu or changed the text.");
        for(int dpi:{96,192,288}){
            RECT proposed{0,0,360,250};statusProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&proposed));
            RECT text{},kind{},style{},list{},preview{};GetWindowRect(draft.text,&text);GetWindowRect(draft.kind,&kind);GetWindowRect(draft.style,&style);GetWindowRect(draft.saveLogBox,&list);GetWindowRect(draft.preview,&preview);
            require(text.bottom-text.top==draft.scale(28) && text.bottom<kind.top && text.left==kind.left && text.right>kind.right && style.bottom<list.top && list.bottom<preview.top,
                "Status editor, Show row or status list option overlap at supported DPI.");
            draft.scrollY=100000;statusLayout(window,draft);SetFocus(draft.kind);const int focusNotices=seen.focus;SetFocus(draft.text);
            RECT bounds{},client{};GetWindowRect(draft.text,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
            require(seen.focus>focusNotices && bounds.top>=0 && bounds.bottom<=client.bottom,"Native EN_SETFOCUS did not reveal the plain text field after scrolling.");
        }
        require(statusTextComboMessages==0 && statusTextLimitMessages==1,"Status text still receives combobox-only editing messages.");
        RemoveWindowSubclass(window,observeStatusText,81);
        statusProc(window,WM_COMMAND,IDCANCEL,0);
    };
    editStatus();require(app.liveStatus.sequence==7 && !lapse::statusCalls,"Cancel published text draft changes.");
    app.liveStatus={};
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        const std::wstring pasted(80,L'x');SendMessageW(draft.text,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(pasted.c_str()));
        require(GetWindowTextLengthW(draft.text)==StatusMaxTextLength,"Native plain-text paste exceeded the 60-character limit.");
        MSG enter{};enter.hwnd=draft.text;enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
        require(IsDialogMessageW(window,&enter) && outcome()==IDOK,"Enter in the single-line editor did not submit the status.");
    };
    editStatus();require(app.liveStatus.kind==StatusKind::Note && std::wcslen(app.liveStatus.text.data())==StatusMaxTextLength && lapse::statusCalls==1 && statusMenus==0,
        "Plain-text Enter submission did not reach the engine exactly once.");
    std::cout<<"PASS native EDIT class, direct typing/EN_CHANGE, focus/select-all, text limit, compact DPI geometry and no status history\n";
}
void statusList(){
    StatusFixture owned;const int configures=lapse::configurationCalls;
    require(!app.settings.saveStatusLog && !lapse::configured.saveStatusLog,"The status list was on by default.");
    // The option alone can be changed without setting a status.
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        require(IsWindowEnabled(draft.saveLogBox) && SendMessageW(draft.saveLogBox,BM_GETCHECK,0,0)==BST_UNCHECKED,"Idle status list option was locked or checked.");
        SendMessageW(draft.saveLogBox,BM_SETCHECK,BST_CHECKED,0);statusProc(window,WM_COMMAND,MAKEWPARAM(StatusSaveLog,BN_CLICKED),reinterpret_cast<LPARAM>(draft.saveLogBox));
        submit(window);require(outcome()==IDOK,"Choosing only the status list was rejected.");
    };
    editStatus();
    require(app.settings.saveStatusLog && lapse::configured.saveStatusLog && lapse::configurationCalls==configures+1 && app.liveStatus.kind==StatusKind::None && !lapse::statusCalls,
        "The status list option did not reach the engine once or changed the status.");
    // Cancel keeps it; Clear status also applies the option.
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        require(SendMessageW(draft.saveLogBox,BM_GETCHECK,0,0)==BST_CHECKED,"Reopening lost the status list choice.");
        SendMessageW(draft.saveLogBox,BM_SETCHECK,BST_UNCHECKED,0);statusProc(window,WM_COMMAND,IDCANCEL,0);};
    editStatus();require(app.settings.saveStatusLog,"Cancel changed the status list option.");
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        SendMessageW(draft.saveLogBox,BM_SETCHECK,BST_UNCHECKED,0);statusProc(window,WM_COMMAND,MAKEWPARAM(StatusClearAction,BN_CLICKED),reinterpret_cast<LPARAM>(draft.clear));};
    editStatus();require(!app.settings.saveStatusLog && !lapse::configured.saveStatusLog,"Clear status did not apply the status list option.");
    // Fixed while recording, like the appearance.
    app.status.state=State::Recording;
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        require(!IsWindowEnabled(draft.saveLogBox),"The status list option was editable while recording.");
        SendMessageW(draft.saveLogBox,BM_SETCHECK,BST_CHECKED,0);type(window,draft.text,StatusTextBox,L"Live");submit(window);};
    editStatus();require(!app.settings.saveStatusLog && app.liveStatus.kind==StatusKind::Note,"Recording changed the frozen status list option or blocked the status.");
    app.status={};
    std::cout<<"PASS status list off by default, opt-in alone or with Clear, kept on Cancel and fixed while recording\n";
}
void clearing(){
    StatusFixture owned;
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){type(window,draftOf(parameter).text,StatusTextBox,L"Reading");submit(window);};editStatus();
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        statusProc(window,WM_COMMAND,MAKEWPARAM(StatusClearAction,BN_CLICKED),reinterpret_cast<LPARAM>(draft.clear));require(outcome()==StatusDialogCleared,"Dialog Clear failed.");};
    editStatus();require(app.liveStatus.kind==StatusKind::None && lapse::engineStatus.kind==StatusKind::None && lapse::statusCalls==2 && !IsWindowEnabled(app.liveStatusClear),"Dialog Clear did not clear the engine status.");
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(ClearStatusButton,BN_CLICKED),0);require(lapse::statusCalls==2,"Clearing nothing sent another update.");
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){type(window,draftOf(parameter).text,StatusTextBox,L"Lunch");submit(window);};editStatus();
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(TrayClearStatus,0),0);require(app.liveStatus.kind==StatusKind::None && lapse::statusCalls==4,"Tray Clear failed.");
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){type(window,draftOf(parameter).text,StatusTextBox,L"Lunch");submit(window);};editStatus();
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(ClearStatusButton,BN_CLICKED),0);require(app.liveStatus.kind==StatusKind::None && lapse::statusCalls==6,"Panel Clear failed.");
    std::cout<<"PASS Clear from the dialog, panel and tray, with no update when nothing is set\n";
}
void appearance(){
    StatusFixture owned;const int configures=lapse::configurationCalls;
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        require(!draft.readOnlyAppearance && IsWindowEnabled(draft.corner),"Idle appearance was locked.");
        select(window,draft.corner,StatusCornerBox,3);select(window,draft.size,StatusSizeBox,2);select(window,draft.style,StatusStyleBox,1);
        const auto bounds=draft.renderer.lastBounds();
        require(bounds.right==draft.illustration.width && bounds.top>draft.illustration.height/2,"Illustration did not follow the chosen corner.");
        submit(window);require(outcome()==IDOK,"Appearance-only change was rejected.");
    };
    editStatus();
    require(app.settings.statusFeed.corner==StatusCorner::BottomRight && app.settings.statusFeed.textSize==StatusTextSize::Large && app.settings.statusFeed.style==StatusStyle::EdgeFade &&
        sameStatusFeedSettings(app.settings.statusFeed,lapse::configured.statusFeed) && lapse::configurationCalls==configures+1 && app.liveStatus.kind==StatusKind::None && lapse::statusCalls==0,
        "Appearance-only change did not configure the engine once or changed the status.");
    // Appearance changes live while recording; the status list choice does not.
    const auto frozen=app.settings.statusFeed;app.status.state=State::Recording;
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        require(!draft.readOnlyAppearance && draft.readOnlySaveLog && IsWindowEnabled(draft.corner) && IsWindowEnabled(draft.style) && !IsWindowEnabled(draft.saveLogBox) &&
            fullText(draft.help).find(L"apply to this recording now")!=std::wstring::npos,"Recording locked appearance or unlocked the status list.");
        select(window,draft.corner,StatusCornerBox,0);type(window,draft.text,StatusTextBox,L"Live change");submit(window);require(outcome()==IDOK,"Status could not change while recording.");
    };
    editStatus();
    require(app.settings.statusFeed.corner==StatusCorner::TopLeft && sameStatusFeedSettings(app.settings.statusFeed,lapse::configured.statusFeed) &&
        app.liveStatus.kind==StatusKind::Note && lapse::statusCalls==1,"Recording lost the live appearance or blocked the live status.");
    // Finishing keeps appearance fixed.
    app.status.state=State::Finishing;const auto finishing=app.settings.statusFeed;
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);
        require(draft.readOnlyAppearance && !IsWindowEnabled(draft.corner) && !IsWindowEnabled(draft.style) && fullText(draft.help).find(L"starting or finishing")!=std::wstring::npos,"Finishing did not lock appearance.");
        select(window,draft.corner,StatusCornerBox,3);type(window,draft.text,StatusTextBox,L"Late change");submit(window);};
    editStatus();require(sameStatusFeedSettings(finishing,app.settings.statusFeed),"Finishing changed the appearance.");
    app.settings.statusFeed=frozen;configure();
    app.status={};
    // Too small to show the feed: the dialog says so.
    app.hasCustomSize=true;app.customWidth=app.customHeight=48;app.committedSize=2;customItems();configure();
    statusScript=[&](HWND window,DLGPROC,LPARAM parameter){require(caption(draftOf(parameter).error).find(L"too small")!=std::wstring::npos,"Tiny video warning missing.");statusProc(window,WM_COMMAND,IDCANCEL,0);};
    editStatus();
    std::cout<<"PASS idle appearance commit with live illustration, live appearance while recording, locked while finishing and tiny-video warning\n";
}
void shortcutAndNotices(){
    StatusFixture owned;std::wstring error;
    const auto key=[](wchar_t letter){return static_cast<uint16_t>(MAKEWORD(letter,HOTKEYF_CONTROL|HOTKEYF_ALT));};
    require(!setRecordingHotkeys(key('P'),key('S'),key('P'),error) && !error.empty(),"Duplicate status shortcut accepted.");
    require(setRecordingHotkeys(key('P'),key('S'),key('U'),error) && app.statusHotkeyId && app.statusHotkeyId!=app.pauseHotkeyId && app.statusHotkeyId!=app.stopHotkeyId,"Status shortcut registration failed.");
    require(setRecordingHotkeys(key('U'),key('S'),key('P'),error) && app.statusHotkey==key('P'),"Swapping status and pause shortcuts failed.");
    statusScript=[&](HWND window,DLGPROC procedure,LPARAM){require(procedure==statusProc,"Shortcut opened the wrong window.");statusProc(window,WM_COMMAND,IDCANCEL,0);};
    app.hiddenToTray=true;const int dialogs=statusDialogCalls;
    windowProc(app.window,WM_HOTKEY,app.statusHotkeyId,MAKELPARAM(MOD_CONTROL|MOD_ALT,'P'));
    require(statusDialogCalls==dialogs+1 && !app.customDialog,"Global status shortcut did not open the window while hidden.");
    windowProc(app.window,WM_HOTKEY,app.statusHotkeyId,MAKELPARAM(MOD_CONTROL|MOD_ALT,'Q'));require(statusDialogCalls==dialogs+1,"Stale shortcut key opened the window.");
    app.hiddenToTray=false;
    // Timers announce their end once; repeating blocks announce each change.
    StatusItem timer;timer.sequence=++app.liveStatusSequence;timer.kind=StatusKind::Timer;setStatusText(timer,L"Essay");timer.startTick=GetTickCount64();timer.durationMs=60000;
    publishStatus(timer);checkStatusNotice();require(!flashes && !beeps,"Running timer notified early.");
    app.liveStatus.startTick-=61000;checkStatusNotice();require(flashes==1 && beeps==1,"Finished timer did not notify.");
    checkStatusNotice();require(flashes==1,"Overtime notified twice.");
    StatusItem blocks=timer;blocks.sequence=++app.liveStatusSequence;blocks.breakMs=30000;publishStatus(blocks);
    app.liveStatus.startTick-=61000;checkStatusNotice();require(flashes==2,"Break start did not notify.");
    app.liveStatus.startTick-=30000;checkStatusNotice();require(flashes==3,"Next work block did not notify.");
    StatusItem fresh=timer;fresh.sequence=++app.liveStatusSequence;fresh.startTick=GetTickCount64()-120000;publishStatus(fresh);checkStatusNotice();
    require(flashes==3,"A status that started already overdue notified as a change.");
    std::cout<<"PASS status shortcut validation, registration, swap and hidden dispatch; timer end and block notices once each\n";
}
void lifecycle(){
    StatusFixture owned;
    statusDialogFailure=true;editStatus();statusDialogFailure=false;
    require(startupMessage.find(L"could not be opened")!=std::wstring::npos && app.liveStatus.kind==StatusKind::None,"Dialog creation failure was silent or changed the status.");
    for(int dpi:{96,192,288}){statusScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=draftOf(parameter);select(window,draft.kind,StatusKindBox,2);check(window,draft.repeat,true);
        RECT proposed{0,0,360,250};statusProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&proposed));
        for(HWND child:{draft.text,draft.kind,draft.minutes,draft.repeat,draft.breakMinutes,draft.corner,draft.size,draft.style,draft.saveLogBox,draft.clear,draft.okay,draft.cancel}){
            if(!IsWindowEnabled(child))continue;
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(child),TRUE);RECT rect{},client{};GetWindowRect(child,&rect);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&rect),2);GetClientRect(window,&client);
            require(rect.right>0 && rect.left<client.right && rect.bottom>0 && rect.top<client.bottom && !outcome(),"Focus did not reveal a status control at a constrained DPI.");
        }
        MSG escape{};escape.hwnd=window;escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;require(IsDialogMessageW(window,&escape)&&outcome()==IDCANCEL,"Escape did not cancel.");};editStatus();}
    statusScript=[&](HWND window,DLGPROC,LPARAM){windowProc(app.window,WM_COMMAND,TrayExit,0);require(outcome()==IDCANCEL || !IsWindow(window),"Exit failed to cancel the status window.");};editStatus();
    require(!IsWindow(app.window) && !app.customDialog,"Exit left the status window or owner behind.");
    std::cout<<"PASS open failure, constrained-DPI focus reveal, Escape and owned-modal Exit\n";
}
}
int main(){try{noteAndStopwatch();timerAndValidation();plainTextAndList();statusList();clearing();appearance();shortcutAndNotices();lifecycle();std::cout<<"All eight status UI groups passed with owned synthetic fixtures.\n";return 0;}
    catch(const std::exception& error){std::cerr<<"STATUS UI FAILURE: "<<error.what()<<'\n';return 1;}}

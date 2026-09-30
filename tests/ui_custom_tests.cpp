// Actual custom-dialog callbacks and controls in owned windows. Lifecycle checks
// show only a nonactivating tool parent wholly offscreen; dialogs remain hidden.
// No user input, device, normal app, actual tray icon or settings I/O is used.
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <functional>
namespace {
std::function<void(HWND,LPARAM)> dialogScript;
INT_PTR dialogOutcome=0;
int dialogCalls=0;
bool failDialog=false;
bool allowTray=true;
bool wheelParameters=false;
UINT wheelLines=1,wheelCharacters=1;
HWND openWheelCombo=nullptr;
bool failWheelSubclass=false;
int wheelSubclassFailures=0;
BOOL WINAPI customSubclass(HWND window,SUBCLASSPROC procedure,UINT_PTR id,DWORD_PTR data){
    if(failWheelSubclass){++wheelSubclassFailures;SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FALSE;}
    return SetWindowSubclass(window,procedure,id,data);
}
BOOL WINAPI customParameters(UINT action,UINT parameter,PVOID value,UINT flags){
    if(wheelParameters && (action==SPI_GETWHEELSCROLLLINES || action==SPI_GETWHEELSCROLLCHARS)){
        *static_cast<UINT*>(value)=action==SPI_GETWHEELSCROLLLINES?wheelLines:wheelCharacters;return TRUE;
    }
    return SystemParametersInfoW(action,parameter,value,flags);
}
LRESULT WINAPI customMessage(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==CB_GETDROPPEDSTATE && window==openWheelCombo)return TRUE;
    return SendMessageW(window,message,wp,lp);
}
BOOL WINAPI customTray(DWORD message,PNOTIFYICONDATAW){return message!=NIM_ADD || allowTray;}
BOOL WINAPI customForeground(HWND){return TRUE;}
BOOL WINAPI customShow(HWND window,int mode){return ShowWindow(window,mode==SW_RESTORE?SW_SHOWNOACTIVATE:mode);}
INT_PTR WINAPI ownedDialog(HINSTANCE,LPCDLGTEMPLATEW,HWND,DLGPROC,LPARAM);
BOOL WINAPI ownedEndDialog(HWND,INT_PTR value){dialogOutcome=value;return TRUE;}
}
#define DialogBoxIndirectParamW ownedDialog
#define EndDialog ownedEndDialog
#define Shell_NotifyIconW customTray
#define SetForegroundWindow customForeground
#define ShowWindow customShow
#define SystemParametersInfoW customParameters
#define SendMessageW customMessage
#define SetWindowSubclass customSubclass
#define main sourceFixtureMain
#include "ui_source_tests.cpp"
#undef main
#undef EndDialog
#undef DialogBoxIndirectParamW
#undef Shell_NotifyIconW
#undef SetForegroundWindow
#undef ShowWindow
#undef SystemParametersInfoW
#undef SendMessageW
#undef SetWindowSubclass

namespace {
INT_PTR WINAPI ownedDialog(HINSTANCE instance,LPCDLGTEMPLATEW resource,HWND owner,DLGPROC procedure,LPARAM parameter){
    ++dialogCalls;dialogOutcome=0;if(failDialog)return -1;
    const bool enabled=IsWindowEnabled(owner)!=FALSE;EnableWindow(owner,FALSE);
    HWND window=CreateDialogIndirectParamW(instance,resource,owner,procedure,parameter);
    require(window && !IsWindowVisible(window),"Custom fixture must create only an owned hidden dialog.");
    try{if(dialogScript)dialogScript(window,parameter);else customProc(window,WM_COMMAND,IDCANCEL,0);}
    catch(...){if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))EnableWindow(owner,enabled);throw;}
    if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))EnableWindow(owner,enabled);
    return dialogOutcome;
}
void setupCustom(){
    app.advanced=app.nightHint=app.nightDetail=nullptr;
    app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;
    app.hasCustomSegment=false;app.committedSegment=0;app.customSegmentSeconds=900;choose(app.splitEvery,0);
    app.customIntervalMs=5000;app.customWidth=1280;app.customHeight=720;app.customLimitSeconds=900;
    app.committedInterval=2;app.committedSize=app.committedLimit=0;
    choose(app.interval,2);choose(app.videoSize,0);choose(app.stopAfter,0);
    customItems();seed(false);dialogCalls=0;failDialog=false;dialogScript={};
}
void invoke(CustomKind kind){
    HWND box=kind==CustomKind::Interval?app.interval:kind==CustomKind::Size?app.videoSize:kind==CustomKind::Segment?app.splitEvery:app.stopAfter;
    int id=kind==CustomKind::Interval?IntervalBox:kind==CustomKind::Size?SizeBox:kind==CustomKind::Segment?SegmentBox:StopAfterBox;
    const int action=static_cast<int>(SendMessageW(box,CB_GETCOUNT,0,0))-1;
    choose(box,action);windowProc(app.window,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(box));
}
void accept(HWND window){customProc(window,WM_COMMAND,IDOK,0);}
void intervalAndCancellation(){
    HiddenFixture owned;setupCustom();const Settings original=app.settings;const int calls=lapse::configurationCalls;
    for(auto kind:{CustomKind::Interval,CustomKind::Size,CustomKind::Limit,CustomKind::Segment}){
        dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
            require(GetNextDlgTabItem(window,draft.first,FALSE)==(draft.second?draft.second:draft.units),"Custom dialog tab order skipped its second field.");
            SetWindowTextW(draft.first,L"invalid draft");customProc(window,WM_CLOSE,0,0);};
        invoke(kind);
    }
    require(app.settings.intervalMs==original.intervalMs && app.settings.width==original.width && app.settings.height==original.height &&
        app.settings.recordingLimitSeconds==0 && app.settings.segmentDurationSeconds==0 && lapse::configurationCalls==calls && choice(app.interval)==2 && choice(app.videoSize)==0 && choice(app.stopAfter)==0 && choice(app.splitEvery)==0,
        "Cancel changed committed settings, selection or engine configuration.");
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
        SetWindowTextW(draft.first,(L"1"+std::wstring(110,L' ')+L"junk").c_str());accept(window);
        require(dialogOutcome==0,"Oversized edit text was validated as a truncated numeric prefix.");
        SetWindowTextW(draft.first,L"0.0005");accept(window);
        require(dialogOutcome==0 && !caption(draft.error).empty(),"Inexact milliseconds closed the dialog or hid its error.");
        SetWindowTextW(draft.first,L"0.125");accept(window);require(dialogOutcome==IDOK,"Valid subsecond interval was rejected.");};
    invoke(CustomKind::Interval);
    require(app.settings.intervalMs==125 && lapse::configured.intervalMs==125 && app.committedInterval==6 && caption(app.interval)==L"0.125 seconds",
        "Accepted custom interval was rounded or displayed incorrectly.");
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
        require(caption(draft.first)==L"0.125","Reopened interval lost its exact value.");SetWindowTextW(draft.first,L"2");accept(window);};
    invoke(CustomKind::Interval);require(app.settings.intervalMs==2000 && choice(app.interval)==1 && !app.hasCustomInterval && SendMessageW(app.interval,CB_GETCOUNT,0,0)==7,
        "Exact preset value retained a duplicate custom row or lost the Custom action.");
    std::cout<<"PASS custom actions reopen, draft cancellation is atomic, invalid drafts stay visible and exact subsecond values commit\n";
}
void dimensionsAndLimit(){
    HiddenFixture owned;setupCustom();
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
        SetWindowTextW(draft.first,L"49");SetWindowTextW(draft.second,L"720");accept(window);require(!dialogOutcome,"Odd width was accepted.");
        SetWindowTextW(draft.first,L"4096");SetWindowTextW(draft.second,L"4096");accept(window);require(!dialogOutcome,"Oversized area was accepted.");
        SetWindowTextW(draft.first,L"1080");SetWindowTextW(draft.second,L"1920");accept(window);};
    invoke(CustomKind::Size);
    require(app.settings.width==1080 && app.settings.height==1920 && caption(app.videoSize)==L"1080 × 1920","Portrait dimensions lost their exact displayed value.");
    const RECT rectangle=previewVideoRect({0,0,800,600});
    require(rectangle.bottom-rectangle.top==600 && rectangle.right-rectangle.left==337,"Portrait preview retained a 16:9 hit-test canvas.");
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
        SetWindowTextW(draft.first,L"0");accept(window);require(!dialogOutcome,"Custom zero silently enabled Never.");
        SetWindowTextW(draft.first,L"1.001");accept(window);require(!dialogOutcome,"Fractional stop second was accepted.");
        SetWindowTextW(draft.first,L"2147483647");accept(window);};
    invoke(CustomKind::Limit);
    require(app.settings.recordingLimitSeconds==INT_MAX && app.customLimitSeconds==INT_MAX && choice(app.stopAfter)==6,"Maximum stop duration overflowed or became Never.");
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
        require(caption(draft.first)==L"2147483647","Maximum stop duration lost precision on reopen.");
        SetWindowTextW(draft.first,L"1.5");choose(draft.units,1);accept(window);};
    invoke(CustomKind::Limit);require(app.settings.recordingLimitSeconds==90 && caption(app.stopAfter)==L"1.5 min","Unit conversion rounded or misrepresented stop duration.");
    std::cout<<"PASS custom portrait geometry, even/area bounds, whole active seconds, units and maximum stop duration\n";
}
void activeAndNight(){
    HiddenFixture owned;setupCustom();
    for(auto state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;const int calls=lapse::configurationCalls;
        for(auto kind:{CustomKind::Interval,CustomKind::Size,CustomKind::Limit,CustomKind::Segment})invoke(kind);
        require(!dialogCalls && lapse::configurationCalls==calls && app.settings.intervalMs==5000 && app.settings.width==1280 && app.settings.recordingLimitSeconds==0 && app.settings.segmentDurationSeconds==0 && choice(app.splitEvery)==0,
            "Active session accepted a forged custom command.");
    }
    app.status={};sourceMode(Mode::Camera);SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);choose(app.nightDuration,0);configure();
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);SetWindowTextW(draft.first,L"0.1");accept(window);};
    invoke(CustomKind::Interval);
    require(app.settings.intervalMs==100 && app.settings.night.enabled && !IsWindowEnabled(app.record) && app.nightValidation.find(L"at least 1 second")!=std::wstring::npos,
        "Subsecond Night Auto was silently coerced or allowed to record.");
    sourceMode(Mode::Desktop);require(app.nightValidation.empty() && IsWindowEnabled(app.record),"Camera-only Night validation blocked ordinary desktop cadence.");
    failDialog=true;const int interval=app.settings.intervalMs;invoke(CustomKind::Interval);
    require(app.settings.intervalMs==interval && !app.customDialog && startupMessage.find(L"could not be opened")!=std::wstring::npos,"Dialog failure lost committed state or recovery message.");
    std::cout<<"PASS active command guards, camera-only subsecond Night validation and failed dialog recovery\n";
}
void segmentDurations(){
    HiddenFixture owned;setupCustom();
    require(!app.settings.segmentDurationSeconds && SendMessageW(app.splitEvery,CB_GETCOUNT,0,0)==6,"Splitting was enabled by default or lost Custom action.");
    for(int i=0;i<5;++i){choose(app.splitEvery,i);windowProc(app.window,WM_COMMAND,MAKEWPARAM(SegmentBox,CBN_SELCHANGE),0);
        require(app.settings.segmentDurationSeconds==SegmentDurations[i] && lapse::configured.segmentDurationSeconds==SegmentDurations[i],"Split preset did not reach exact engine setting.");}
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
        wchar_t help[512]{};GetWindowTextW(draft.help,help,512);
        require(draft.kind==CustomKind::Segment && std::wstring(help).find(L"Shorter parts")!=std::wstring::npos,"Split dialog did not explain its active-time/overhead meaning.");
        for(auto invalid:{L"0",L"0.5",L"1.001",L"2147483648",L"1x"}){SetWindowTextW(draft.first,invalid);accept(window);require(!dialogOutcome,"Invalid split duration was accepted.");}
        const std::wstring pasted=L"1"+std::wstring(110,L' ')+L"junk";SendMessageW(draft.first,EM_SETSEL,0,-1);SendMessageW(draft.first,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(pasted.c_str()));
        accept(window);require(!dialogOutcome && GetWindowTextLengthW(draft.first)==96,"Native split paste accepted a truncated prefix.");
        SetWindowTextW(draft.first,L"2147483647");accept(window);};
    invoke(CustomKind::Segment);require(app.settings.segmentDurationSeconds==INT_MAX && app.committedSegment==5 && app.hasCustomSegment,"Maximum whole-second split overflowed.");
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);require(caption(draft.first)==L"2147483647","Exact split value lost on reopen.");
        SetWindowTextW(draft.first,L"1.5");choose(draft.units,1);accept(window);};
    invoke(CustomKind::Segment);require(app.settings.segmentDurationSeconds==90 && caption(app.splitEvery)==L"1.5 min","Exact custom split unit conversion failed.");
    sourceMode(static_cast<Mode>(SeparateFilesMode));SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);choose(app.nightDuration,0);choose(app.interval,2);
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);SetWindowTextW(draft.first,L"1");accept(window);};
    invoke(CustomKind::Segment);record();
    require(lapse::recorded.segmentDurationSeconds==1 && lapse::recorded.intervalMs==5000 && lapse::recorded.night.enabled && lapse::recorded.separateFiles,
        "Short split was silently coerced or blocked paired/Night recording.");
    for(State state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;updateControls();const int calls=lapse::configurationCalls;choose(app.splitEvery,0);
        windowProc(app.window,WM_COMMAND,MAKEWPARAM(SegmentBox,CBN_SELCHANGE),0);
        require(!IsWindowEnabled(app.splitEvery) && choice(app.splitEvery)==5 && app.settings.segmentDurationSeconds==1 && lapse::configurationCalls==calls,"Active split setting changed or displayed a false selection.");}
    app.status=lapse::fixtureStatus={};
    dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);SetWindowTextW(draft.first,L"6");choose(draft.units,2);accept(window);};
    invoke(CustomKind::Segment);require(app.settings.segmentDurationSeconds==21600 && choice(app.splitEvery)==3 && !app.hasCustomSegment && SendMessageW(app.splitEvery,CB_GETCOUNT,0,0)==6,
        "Preset-equivalent split retained a duplicate custom row.");
    std::cout<<"PASS exact split presets/custom/native paste, short paired/Night split, active locks and preset normalization\n";
}
void dialogDpiAndLifecycle(){
    HiddenFixture owned;setupCustom();
    for(int dpi:{96,144,192,288}){
        dialogScript=[dpi](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
            RECT proposed{0,0,MulDiv(460,dpi,96),MulDiv(280,dpi,96)};customProc(window,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&proposed));
            require(draft.dpi==dpi && draft.font && !draft.layingOut,"DPI reflow did not complete.");
            for(HWND child:{draft.first,draft.units,draft.okay,draft.cancel}){RECT bounds{};GetWindowRect(child,&bounds);require(bounds.right>bounds.left && bounds.bottom>bounds.top,"DPI reflow collapsed a required control.");}
            SetWindowPos(window,nullptr,0,0,320,210,SWP_NOZORDER|SWP_NOACTIVATE);customProc(window,WM_SIZE,0,0);
            SetWindowTextW(draft.error,L"Video dimensions must be even numbers from 48 to 4096, with at most 8,847,360 pixels. Reduce one dimension and try again.");customLayout(window,draft);
            for(HWND child:{draft.help,draft.error}){RECT actual{};GetClientRect(child,&actual);RECT measured{0,0,actual.right,0};wchar_t text[1024]{};GetWindowTextW(child,text,1024);
                HDC dc=GetDC(child);const auto previous=SelectObject(dc,draft.font);DrawTextW(dc,text,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,previous);ReleaseDC(child,dc);
                require(measured.bottom<=actual.bottom,"Wrapped custom help or error text was clipped at a constrained DPI.");}
            customReveal(window,draft,draft.cancel);RECT bounds{},client{};GetWindowRect(draft.cancel,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
            require(bounds.left>=0 && bounds.top>=0 && bounds.right<=client.right && bounds.bottom<=client.bottom,"Constrained dialog cannot reveal its Cancel button.");
            customProc(window,WM_COMMAND,IDCANCEL,0);};
        invoke(CustomKind::Interval);invoke(CustomKind::Segment);require(!app.customDialog,"Destroyed custom dialog retained its HWND.");
    }
    dialogScript=[](HWND,LPARAM){windowProc(app.window,WM_COMMAND,TrayExit,0);};invoke(CustomKind::Interval);
    require(!IsWindow(app.window) && !app.customDialog,"Tray Exit left an owned custom dialog or owner behind.");
    std::cout<<"PASS owned dialog DPI/reflow/scroll reachability, cleanup and explicit tray Exit\n";
}
void canceledDialogFocus(){
    for(auto kind:{CustomKind::Interval,CustomKind::Size,CustomKind::Limit,CustomKind::Segment})for(int action=0;action<5;++action){
        HiddenFixture owned;setupCustom();app.hiddenToTray=app.closeWhenDone=app.trayRegistered=app.trayNoticeShown=app.startupComplete=false;
        app.failureNotice=FailureNotice::None;app.trayStateValid=false;app.taskbarCreated=0;app.customDialog=nullptr;app.trayTooltip.clear();allowTray=action!=2;
        const HWND owner=app.window,box=kind==CustomKind::Interval?app.interval:kind==CustomKind::Size?app.videoSize:kind==CustomKind::Segment?app.splitEvery:app.stopAfter;
        RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
        screen.right=screen.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);screen.bottom=screen.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const auto outside=[&]{if(IsWindow(owner)){RECT bounds{},overlap{};GetWindowRect(owner,&bounds);require(!IntersectRect(&overlap,&bounds,&screen),"Custom lifecycle fixture entered the visible desktop.");}};
        SetWindowLongPtrW(owner,GWL_EXSTYLE,GetWindowLongPtrW(owner,GWL_EXSTYLE)|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE);
        SetWindowPos(owner,nullptr,screen.right+20000,screen.bottom+20000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        outside();ShowWindow(box,SW_SHOWNA);ShowWindow(owner,SW_SHOWNOACTIVATE);outside();
        const auto prior=app.settings;const int configurations=lapse::configurationCalls;
        dialogScript=[&](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
            SetFocus(draft.first);require(GetFocus()==draft.first,"Native custom field did not receive focus.");SetWindowTextW(draft.first,L"123");
            if(action==0)customProc(window,WM_COMMAND,IDCANCEL,0);
            else if(action<=2)windowProc(owner,WM_CLOSE,0,0);
            else if(action==3)windowProc(owner,WM_COMMAND,TrayExit,0);
            else windowProc(owner,WM_ENDSESSION,TRUE,0);
            outside();
        };
        invoke(kind);const HWND focused=GetFocus();
        require(dialogOutcome==IDCANCEL && !app.customDialog,"Lifecycle cancellation left a dialog or accepted its draft.");
        require(app.settings.intervalMs==prior.intervalMs && app.settings.width==prior.width && app.settings.height==prior.height &&
            app.settings.recordingLimitSeconds==prior.recordingLimitSeconds && app.settings.segmentDurationSeconds==prior.segmentDurationSeconds,
            "Canceled lifecycle path committed draft settings.");
        require(lapse::recordCalls==0 && lapse::configurationCalls-configurations==(action==1 || action==2?1:0),"Dialog cancellation changed recording or added configuration work.");
        if(action==0 || action==2)require(IsWindow(owner)&&IsWindowVisible(owner)&&!app.hiddenToTray&&focused==box,"Visible Cancel or rejected Hide failed to restore the invoking control.");
        else if(action==1)require(IsWindow(owner)&&!IsWindowVisible(owner)&&app.hiddenToTray&&focused!=owner&&!IsChild(owner,focused),"Successful Hide restored native focus inside the hidden owner.");
        else require(!IsWindow(owner),"Exit or session shutdown retained the custom dialog owner.");
        outside();if(IsWindow(owner))ShowWindow(owner,SW_HIDE);app.trayRegistered=false;allowTray=true;
    }
    std::cout<<"PASS native custom focus after Cancel/Hide/rejected Hide/Exit/session end for all four custom kinds, with canceled drafts unchanged\n";
}
void nativeDialogWheel(){
    HiddenFixture owned;setupCustom();app.hiddenToTray=false;app.closeWhenDone=false;wheelParameters=true;wheelLines=wheelCharacters=1;
    struct Reset{~Reset(){wheelParameters=false;openWheelCombo=nullptr;failWheelSubclass=false;}} reset;
    const auto position=[](HWND window,int bar){SCROLLINFO info{sizeof(info),SIF_POS};GetScrollInfo(window,bar,&info);return info.nPos;};
    const auto wheel=[](HWND target,int delta,UINT message=WM_MOUSEWHEEL,WORD keys=0){SendMessageW(target,message,MAKEWPARAM(keys,static_cast<WORD>(delta)),0);};
    const auto prior=app.settings;const int configurations=lapse::configurationCalls;
    for(int dpi:{96,192}){
        dialogScript=[&](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
            RECT constrained{0,0,320,230};SendMessageW(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&constrained));
            require((GetWindowLongPtrW(window,GWL_STYLE)&(WS_VSCROLL|WS_HSCROLL))==(WS_VSCROLL|WS_HSCROLL),"Wheel fixture lacks both overflow axes.");
            const auto top=[&]{SendMessageW(window,WM_VSCROLL,SB_TOP,0);SendMessageW(window,WM_HSCROLL,SB_LEFT,0);};
            SetFocus(draft.first);const HWND focused=GetFocus();int singleStep=0;
            for(HWND target:{window,draft.help,draft.cancel,draft.first,draft.units}){
                top();choose(draft.units,0);wheel(target,-WHEEL_DELTA);const int after=position(window,SB_VERT);
                require(after>0 && choice(draft.units)==0 && GetFocus()==focused,"Wheel failed to scroll or changed draft selection/focus.");
                if(!singleStep)singleStep=after;else require(after==singleStep,"Native child propagation scrolled a different number of times.");
            }
            top();wheel(window,-WHEEL_DELTA/2);wheel(window,WHEEL_DELTA/2,WM_MOUSEHWHEEL);
            require(position(window,SB_VERT)==0 && position(window,SB_HORZ)==0,"Partial detent moved the page too early.");
            wheel(window,-WHEEL_DELTA/2);require(position(window,SB_VERT)>0 && position(window,SB_HORZ)==0,"Wheel axes mixed partial deltas.");
            wheel(window,WHEEL_DELTA/2,WM_MOUSEHWHEEL);require(position(window,SB_HORZ)>0,"Horizontal partial deltas did not accumulate.");
            top();wheel(window,-WHEEL_DELTA,WM_MOUSEWHEEL,MK_SHIFT);require(position(window,SB_HORZ)>0 && position(window,SB_VERT)==0,"Shift-wheel did not stay on the horizontal axis.");
            top();wheel(window,-WHEEL_DELTA,WM_MOUSEWHEEL,MK_CONTROL);require(position(window,SB_HORZ)==0 && position(window,SB_VERT)==0,"Ctrl-wheel scrolled the settings page.");
            wheelLines=2;top();wheel(window,-WHEEL_DELTA);require(position(window,SB_VERT)>singleStep,"System wheel line count was ignored.");
            wheelLines=WHEEL_PAGESCROLL;top();wheel(window,-WHEEL_DELTA);SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,SB_VERT,&info);
            require(position(window,SB_VERT)>singleStep && position(window,SB_VERT)<=static_cast<int>(info.nPage),"Page wheel setting was not bounded to a page.");
            wheelLines=0;top();choose(draft.units,0);wheel(draft.units,-WHEEL_DELTA);require(position(window,SB_VERT)==0 && choice(draft.units)==0,"Zero wheel setting changed page or closed combo.");
            wheel(draft.units,-WHEEL_DELTA/2);wheelLines=1;wheel(draft.units,-WHEEL_DELTA/2);
            require(position(window,SB_VERT)==0 && choice(draft.units)==0,"Disabled wheel input leaked a partial detent after re-enabling.");
            wheel(draft.units,-WHEEL_DELTA/2);require(position(window,SB_VERT)>0,"Re-enabled wheel input failed to accumulate fresh detents.");
            SendMessageW(window,WM_VSCROLL,SB_BOTTOM,0);const int bottom=position(window,SB_VERT);wheel(draft.units,-WHEEL_DELTA);
            require(position(window,SB_VERT)==bottom && choice(draft.units)==0,"Wheel at page edge changed a closed combo.");
            top();openWheelCombo=draft.units;wheel(draft.units,-WHEEL_DELTA);openWheelCombo=nullptr;
            require(position(window,SB_VERT)==0 && choice(draft.units)==1 && !SendMessageW(draft.units,CB_GETDROPPEDSTATE,0,0),"Open-dropdown route failed to yield to native control without a popup.");
            top();wheel(window,-WHEEL_DELTA/2);wheel(window,WHEEL_DELTA/2,WM_MOUSEHWHEEL);
            SetWindowPos(window,nullptr,0,0,1200,900,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);SendMessageW(window,WM_SIZE,0,0);
            require(!(GetWindowLongPtrW(window,GWL_STYLE)&(WS_VSCROLL|WS_HSCROLL)),"No-overflow wheel control still has scrollbars.");
            choose(draft.units,0);wheel(draft.units,-WHEEL_DELTA);require(choice(draft.units)==1,"Nonoverflowing dialog stole native combo wheel behavior.");
            SendMessageW(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&constrained));top();
            wheel(window,-WHEEL_DELTA/2);wheel(window,WHEEL_DELTA/2,WM_MOUSEHWHEEL);
            require(position(window,SB_VERT)==0 && position(window,SB_HORZ)==0,"Retired overflow carried partial input into the new viewport.");
            wheel(window,-WHEEL_DELTA/2);wheel(window,WHEEL_DELTA/2,WM_MOUSEHWHEEL);
            require(position(window,SB_VERT)>0 && position(window,SB_HORZ)>0,"New viewport failed to accumulate fresh input per axis.");
            require(!IsWindowVisible(window)&&!IsWindowVisible(app.window),"Native wheel fixture became visible.");customProc(window,WM_COMMAND,IDCANCEL,0);
        };invoke(CustomKind::Interval);
    }
    failWheelSubclass=true;const int failures=wheelSubclassFailures;
    dialogScript=[](HWND,LPARAM){require(dialogOutcome==-1,"Failed wheel routing installation left an editable partial dialog.");};
    invoke(CustomKind::Interval);failWheelSubclass=false;
    require(wheelSubclassFailures==failures+1 && startupMessage.find(L"could not be opened")!=std::wstring::npos && !app.customDialog,
        "Wheel subclass failure did not clean up and report a recoverable dialog error.");
    require(app.settings.intervalMs==prior.intervalMs && lapse::configurationCalls==configurations,"Canceled wheel inspection changed configured settings.");
    std::cout<<"PASS native dialog/child wheel routing at two DPIs, independent partial axes, modifiers/system amounts, closed-combo protection and native ownership\n";
}
void nativeNumericInsertion(){
    HiddenFixture owned;setupCustom();
    for(int field=0;field<4;++field){
        const auto kind=field==0?CustomKind::Interval:field==1?CustomKind::Limit:CustomKind::Size;
        dialogScript=[field](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
            const HWND edit=field==3?draft.second:draft.first;const std::wstring prefix=field<2?L"1":L"720";
            const auto payload=prefix+std::wstring(110-prefix.size(),L' ')+L"junk";
            SendMessageW(edit,EM_SETSEL,0,-1);SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(payload.c_str()));
            require(SendMessageW(edit,EM_GETLIMITTEXT,0,0)==96 && GetWindowTextLengthW(edit)==96,"Native edit truncation bypasses validator rejection length.");
            accept(window);require(!dialogOutcome && !caption(draft.error).empty(),"Native pasted invalid suffix was truncated into a valid custom value.");
            SendMessageW(edit,EM_SETSEL,0,-1);SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(prefix.c_str()));accept(window);
            require(dialogOutcome==IDOK,"Ordinary exact native insertion was rejected.");};
        invoke(kind);
    }
    std::cout<<"PASS actual native paste rejects truncated invalid interval, stop, width and height inputs while exact insertion remains valid\n";
}
void nativeButtonNavigation(){
    HiddenFixture owned;setupCustom();
    const auto visibleFocus=[](HWND window,HWND child){
        RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
        require(GetFocus()==child && bounds.left>=0 && bounds.top>=0 && bounds.right<=client.right && bounds.bottom<=client.bottom,
            "Native modal focus remained outside the viewport.");
    };
    for(int dpi:{96,192})for(auto kind:{CustomKind::Interval,CustomKind::Size,CustomKind::Limit,CustomKind::Segment}){
        dialogScript=[=](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
            RECT suggested{0,0,320,210};customProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.first),TRUE);
            // Use the dialog manager's real sequential focus route. Do not
            // fabricate BN_SETFOCUS or call the production reveal helper.
            for(HWND target:{draft.second?draft.second:draft.units,draft.okay,draft.cancel,draft.first}){
                SendMessageW(window,WM_NEXTDLGCTL,FALSE,FALSE);visibleFocus(window,target);
                require(!dialogOutcome,"Focus transition accepted or cancelled the custom draft.");
            }
            MSG key{};key.hwnd=GetFocus();key.message=WM_KEYDOWN;key.wParam=VK_ESCAPE;
            require(IsDialogMessageW(window,&key) && dialogOutcome==IDCANCEL,"Native Escape did not cancel the custom dialog.");
        };invoke(kind);
    }
    for(int activation=0;activation<3;++activation){
        dialogScript=[activation](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
            SetWindowTextW(draft.first,L"2");
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(activation==2?draft.first:draft.okay),TRUE);
            require(!dialogOutcome,"Focusing the custom default button activated it.");
            if(activation==0)SendMessageW(draft.okay,BM_CLICK,0,0);
            else if(activation==1){SendMessageW(draft.okay,WM_KEYDOWN,VK_SPACE,0);SendMessageW(draft.okay,WM_KEYUP,VK_SPACE,0);}
            else {MSG key{};key.hwnd=GetFocus();key.message=WM_KEYDOWN;key.wParam=VK_RETURN;require(IsDialogMessageW(window,&key),"Native Enter was not handled.");}
            require(dialogOutcome==IDOK,"Click, Space or default Enter failed to accept a valid custom value.");
        };invoke(CustomKind::Interval);require(app.settings.intervalMs==2000,"Native activation did not commit the validated duration.");
    }
    std::cout<<"PASS native custom tab focus scrolls without activation at two DPIs; Click, Space, Enter and Escape retain their meanings\n";
}
void sourceSizeSnapshots(){
    HiddenFixture owned;setupCustom();
    const auto open=[](){windowProc(app.window,WM_COMMAND,MAKEWPARAM(SizeBox,CBN_DROPDOWN),reinterpret_cast<LPARAM>(app.videoSize));};
    const auto select=[](int item){choose(app.videoSize,item);windowProc(app.window,WM_COMMAND,MAKEWPARAM(SizeBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.videoSize));};
    const auto itemText=[](int item){wchar_t value[128]{};SendMessageW(app.videoSize,CB_GETLBTEXT,item,reinterpret_cast<LPARAM>(value));return std::wstring(value);};
    lapse::listedMonitors[1].bounds={100,200,5220,1640}; // Live topology differs from the cached source list.
    const auto sourceList=app.monitors;
    open();require(app.sizeSuggestions[0].width==4096 && app.sizeSuggestions[0].height==1152 &&
        itemText(app.sizeSuggestions[0].item)==L"Fit screen: 4096 × 1152" && itemText(2)==L"Custom..." && choice(app.videoSize)==0,
        "Fitted source action changed preset/custom identity or ignored live screen bounds.");
    const int calls=lapse::configurationCalls;select(app.sizeSuggestions[0].item);
    require(app.settings.width==4096 && app.settings.height==1152 && app.committedSize==2 && app.hasCustomSize &&
        lapse::configurationCalls==calls+1 && !dialogCalls && caption(app.videoSize)==L"4096 × 1152",
        "Source action did not materialize one exact custom setting without a modal.");
    require(EqualRect(&app.monitors[1].bounds,&sourceList[1].bounds),"Suggestion replaced the source-list snapshot.");
    open();require(itemText(3)==L"Custom..." && app.sizeSuggestions[0].item==4,"Source actions displaced committed custom or Custom... indices.");
    for(int changed=0;changed<3;++changed){
        lapse::listedMonitors={displayA,displayB};open();const int action=app.sizeSuggestions[0].item;
        if(changed==0)lapse::listedMonitors[1].bounds.right+=2;
        if(changed==1)lapse::listedMonitors.erase(lapse::listedMonitors.begin()+1);
        if(changed==2){lapse::listedMonitors[1].bounds.left+=10;lapse::listedMonitors[1].bounds.right+=10;}
        const auto committed=app.settings;const int before=lapse::configurationCalls;select(action);
        require(app.settings.width==committed.width && app.settings.height==committed.height && choice(app.videoSize)==2 && lapse::configurationCalls==before,
            "Changed, moved or removed screen committed a stale dropdown snapshot.");
    }
    lapse::listedMonitors={displayA,displayB};lapse::listedMonitors[1].bounds={0,0,1280,720};open();select(app.sizeSuggestions[0].item);
    require(!app.hasCustomSize && app.committedSize==0 && choice(app.videoSize)==0,"Exact preset source dimensions left a duplicate custom row.");
    sourceMode(Mode::Camera);app.status.cameraInput={1920,1080,99};lapse::fixtureStatus.cameraInput={};
    open();require(app.sizeSuggestions[0].item<0 && app.sizeSuggestions[1].item<0 &&
        std::wstring(videoSizeTooltip()).find(L"current frame")!=std::wstring::npos,"Old polled camera metadata or unrelated screen became a suggestion.");
    lapse::fixtureStatus.cameraInput={640,480,7};open();require(itemText(app.sizeSuggestions[1].item)==L"Use camera input: 640 × 480","Verified camera feed was not labeled as input.");
    const int cameraAction=app.sizeSuggestions[1].item;lapse::fixtureStatus.cameraInput.generation=8;select(cameraAction);
    require(app.settings.width==1280 && choice(app.videoSize)==0,"Retired camera generation committed a stale action.");
    open();const int changedSize=app.sizeSuggestions[1].item;lapse::fixtureStatus.cameraInput.width=642;select(changedSize);
    require(app.settings.width==1280,"Changed camera dimensions committed a stale action.");
    open();const int changedCamera=app.sizeSuggestions[1].item;choose(app.camera,1-choice(app.camera));configure();select(changedCamera);
    require(app.settings.width==1280,"New selected camera reused the old source action.");
    lapse::fixtureStatus.cameraInput={640,480,9};open();select(app.sizeSuggestions[1].item);
    require(app.settings.width==640 && app.settings.height==480 && app.hasCustomSize,"Current camera input was not committed exactly.");
    lapse::fixtureStatus.cameraInput={1920,1080,10};open();
    require(itemText(app.sizeSuggestions[1].item)==L"Use camera input: 1920 × 1080","Verified high-resolution input lost its exact source label.");
    select(app.sizeSuggestions[1].item);
    require(app.settings.width==1920 && app.settings.height==1080 && app.committedSize==1 && !app.hasCustomSize && choice(app.videoSize)==1,
        "Verified 1080p input did not normalize to the existing output preset.");
    open();const int retiredHigh=app.sizeSuggestions[1].item;
    lapse::fixtureStatus.cameraInput={0,0,11};const int beforeRetired=lapse::configurationCalls;select(retiredHigh);
    require(app.settings.width==1920 && choice(app.videoSize)==1 && lapse::configurationCalls==beforeRetired,
        "A camera restart committed its retired high-resolution suggestion.");
    open();require(app.sizeSuggestions[1].item<0,"Pending replacement camera advertised the old input size.");
    lapse::fixtureStatus.cameraInput={640,480,12};open();select(app.sizeSuggestions[1].item);
    require(app.settings.width==640 && app.settings.height==480 && app.hasCustomSize && choice(app.videoSize)==2,
        "Smaller actual camera fallback was replaced by a promised 1080p suggestion.");
    for(auto state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=State::Idle;lapse::fixtureStatus.cameraInput={1280,720,10};open();const int action=app.sizeSuggestions[1].item;
        app.status.state=state;const int before=lapse::configurationCalls;select(action);
        require(app.settings.width==640 && choice(app.videoSize)==2 && lapse::configurationCalls==before,"Active-session source-size action changed frozen geometry.");
    }
    app.status={};lapse::fixtureStatus.cameraInput={32,32,11};open();require(app.sizeSuggestions[1].item<0 &&
        std::wstring(videoSizeTooltip()).find(L"cannot fit")!=std::wstring::npos,"Unsupported tiny camera input offered an upscaled match.");
    const auto protectedMessage=L"Saved earlier parts; retained last movie.";app.status.message=protectedMessage;
    enumerationAllocation::failNext=true;open();require(app.sizeSuggestions[0].item<0 && app.sizeSuggestions[1].item<0 &&
        choice(app.videoSize)==2 && app.status.message==protectedMessage,"Suggestion allocation failure lost committed output or primary status.");
    std::cout<<"PASS source-size exact/Fit snapshots, live topology, stable custom indices, actual 1080p/fallback, camera generation/identity, active locks and allocation fallback\n";
}
}
int main(){
    try{intervalAndCancellation();dimensionsAndLimit();activeAndNight();segmentDurations();nativeNumericInsertion();nativeButtonNavigation();dialogDpiAndLifecycle();canceledDialogFocus();nativeDialogWheel();sourceSizeSnapshots();
        std::cout<<"All custom UI cases passed with owned controls and synthetic engine.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

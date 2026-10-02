// Owned native dialogs and inert engine. Mnemonic checks temporarily show a
// nonactivating tool window wholly offscreen; other dialogs remain hidden.
// No device capture, ordinary application launch, persistent UI or settings I/O.
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <vector>
namespace {
std::function<void(HWND,DLGPROC,LPARAM)> skipScript;
struct ModalOutcome {HWND window{};INT_PTR value=0;};
std::vector<ModalOutcome> modalOutcomes;
int skipDialogCalls=0;
bool skipDialogFailure=false;
HWND reportedOpenWheelCombo{},yieldWheelCombo{};
int nativeWheelYields=0;
BOOL WINAPI skipWheelPreference(UINT action,UINT parameter,PVOID value,UINT flags){
    if(action==SPI_GETWHEELSCROLLLINES || action==SPI_GETWHEELSCROLLCHARS){*static_cast<UINT*>(value)=3;return TRUE;}
    return SystemParametersInfoW(action,parameter,value,flags);
}
LRESULT WINAPI skipWheelSend(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==CB_GETDROPPEDSTATE && window==reportedOpenWheelCombo)return TRUE;
    return SendMessageW(window,message,wp,lp);
}
LRESULT CALLBACK skipWheelDefault(HWND window,UINT message,WPARAM wp,LPARAM lp){
    // Observe native-control ownership without opening a popup or changing its
    // selection during the explicit Ctrl/open-dropdown yield checks.
    if(window==yieldWheelCombo && (message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL)){++nativeWheelYields;return 0;}
    return DefSubclassProc(window,message,wp,lp);
}
INT_PTR WINAPI skipOwnedDialog(HINSTANCE,LPCDLGTEMPLATEW,HWND,DLGPROC,LPARAM);
BOOL WINAPI skipOwnedEndDialog(HWND window,INT_PTR value){for(auto& modal:modalOutcomes)if(modal.window==window)modal.value=value;return TRUE;}
}
#define DialogBoxIndirectParamW skipOwnedDialog
#define EndDialog skipOwnedEndDialog
#define SendMessageW skipWheelSend
#define DefSubclassProc skipWheelDefault
#define SystemParametersInfoW skipWheelPreference
#define main sourceFixtureMain
#include "ui_source_tests.cpp"
#undef main
#undef EndDialog
#undef DialogBoxIndirectParamW
#undef SendMessageW
#undef DefSubclassProc
#undef SystemParametersInfoW
namespace {
INT_PTR WINAPI skipOwnedDialog(HINSTANCE instance,LPCDLGTEMPLATEW resource,HWND owner,DLGPROC procedure,LPARAM parameter){
    ++skipDialogCalls;if(skipDialogFailure)return -1;
    const bool enabled=IsWindowEnabled(owner)!=FALSE;EnableWindow(owner,FALSE);
    HWND window=CreateDialogIndirectParamW(instance,resource,owner,procedure,parameter);
    require(window && !IsWindowVisible(window),"Only an owned hidden dialog may be created.");modalOutcomes.push_back({window,0});
    try{if(skipScript)skipScript(window,procedure,parameter);else procedure(window,WM_COMMAND,IDCANCEL,0);}
    catch(...){modalOutcomes.pop_back();if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))EnableWindow(owner,enabled);throw;}
    const auto result=modalOutcomes.back().value;modalOutcomes.pop_back();if(IsWindow(window))DestroyWindow(window);if(IsWindow(owner))EnableWindow(owner,enabled);return result;
}
INT_PTR outcome(){return modalOutcomes.back().value;}
void setupSkip(){
    app.advanced=app.nightHint=app.nightDetail=nullptr;app.customDialog=nullptr;app.advancedExpanded=false;app.hiddenToTray=false;
    app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;choose(app.interval,2);choose(app.videoSize,0);choose(app.stopAfter,0);
    seed(false);skipScript={};skipDialogCalls=0;skipDialogFailure=false;
    reportedOpenWheelCombo=yieldWheelCombo=nullptr;nativeWheelYields=0;
    app.personPack={};app.personPackKnown=false;lapse::uiPersonPackInfo={};lapse::uiPersonPackInspections=0;
    lapse::uiPersonPackDialogs=0;lapse::uiPersonPackOwner=nullptr;lapse::uiPersonPackInstallOnDialog=false;
}
void mode(HWND window,SkipDraft& draft,TimeSkipMode value){choose(draft.mode,static_cast<int>(value));skipProc(window,WM_COMMAND,MAKEWPARAM(SkipMode,CBN_SELCHANGE),reinterpret_cast<LPARAM>(draft.mode));}
void rangeDraft(HWND window,CustomDraft& draft,int start,int end){SetWindowTextW(draft.first,std::to_wstring(start).c_str());SetWindowTextW(draft.second,std::to_wstring(end).c_str());customProc(window,WM_COMMAND,IDOK,0);}
void transactionalRanges(){
    HiddenFixture owned;setupSkip();const int initialCalls=lapse::configurationCalls;int range=0;
    skipScript=[&](HWND window,DLGPROC procedure,LPARAM parameter){
        if(procedure==customProc){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);require(draft.kind==CustomKind::Range,"Wrong nested editor.");
            if(range==0){SetWindowTextW(draft.first,L"1.5");SetWindowTextW(draft.second,L"3");customProc(window,WM_COMMAND,IDOK,0);require(!outcome(),"Fractional active seconds accepted.");
                SetWindowTextW(draft.first,(L"1"+std::wstring(100,L' ')+L"bad").c_str());customProc(window,WM_COMMAND,IDOK,0);require(!outcome(),"Truncated range text accepted.");}
            rangeDraft(window,draft,range==0?20:0,range==0?40:20);++range;return;}
        auto& draft=*reinterpret_cast<SkipDraft*>(parameter);require(!app.active() && !draft.readOnly,"Idle editor frozen.");mode(window,draft,TimeSkipMode::Manual);
        skipProc(window,WM_COMMAND,IDOK,0);require(!outcome() && !caption(draft.error).empty(),"Empty manual schedule accepted.");
        for(int i=0;i<2;++i){skipProc(window,WM_COMMAND,SkipAdd,0);require(app.customDialog==window,"Inner editor failed to restore outer modal ownership.");}
        require(draft.policy.rangeCount==1 && draft.policy.ranges[0].startSeconds==0 && draft.policy.ranges[0].endSeconds==40,"Touching ranges were not sorted/merged visibly.");
        require(SendMessageW(draft.ranges,LB_GETCOUNT,0,0)==1,"Merged list still displayed two ranges.");
        require(app.settings.timeSkip.mode==TimeSkipMode::Off,"Inner acceptance committed outside the outer draft.");
        skipProc(window,WM_COMMAND,IDCANCEL,0);
    };
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::Off && lapse::configurationCalls==initialCalls && !app.customDialog,"Outer Cancel changed committed policy.");
    range=0;
    skipScript=[&](HWND window,DLGPROC procedure,LPARAM parameter){
        if(procedure==customProc){rangeDraft(window,*reinterpret_cast<CustomDraft*>(parameter),0,60);return;}
        auto& draft=*reinterpret_cast<SkipDraft*>(parameter);mode(window,draft,TimeSkipMode::QuietWithinSchedule);choose(draft.speed,5);choose(draft.ramp,2);
        SetWindowTextW(draft.quiet,L"1.5");choose(draft.quietUnits,1);skipProc(window,WM_COMMAND,SkipAdd,0);
        SetWindowTextW(draft.repeat,L"59");skipProc(window,WM_COMMAND,IDOK,0);require(!outcome(),"Range beyond repeat period accepted.");
        SetWindowTextW(draft.repeat,L"60");skipProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Valid combined policy rejected.");
    };
    editSkip();const auto& policy=app.settings.timeSkip;
    require(policy.mode==TimeSkipMode::QuietWithinSchedule && policy.multiplier==64 && policy.quietAfterMs==90000 && policy.rampFrames==60 && policy.repeatSeconds==60 && policy.rangeCount==1,"Exact accepted policy did not reach settings.");
    require(lapse::configured.timeSkip.repeatSeconds==60 && lapse::configurationCalls==initialCalls+1,"Committed policy not configured exactly once.");
    std::cout<<"PASS nested range drafts, whole seconds, merge visibility, repeat validation, atomic cancel and exact combined commit\n";
}
void boundsAndFreeze(){
    HiddenFixture owned;setupSkip();app.settings.timeSkip.mode=TimeSkipMode::Manual;app.settings.timeSkip.rangeCount=16;
    for(unsigned i=0;i<16;++i)app.settings.timeSkip.ranges[i]={int(i*10),int(i*10+5)};
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
        require(!IsWindowEnabled(draft.add),"Sixteenth range did not disable Add.");const auto calls=skipDialogCalls;skipProc(window,WM_COMMAND,SkipAdd,0);require(skipDialogCalls==calls,"Forged Add exceeded bounded list.");
        skipProc(window,WM_COMMAND,SkipRemove,0);require(draft.policy.rangeCount==15 && IsWindowEnabled(draft.add),"Remove did not restore bounded Add.");skipProc(window,WM_COMMAND,IDCANCEL,0);};
    editSkip();require(app.settings.timeSkip.rangeCount==16,"Cancelled removal mutated policy.");
    for(auto state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;const int initial=lapse::configurationCalls;
        skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);require(draft.readOnly,"Active editor not read only.");
            for(HWND child:{draft.mode,draft.speed,draft.ramp,draft.quiet,draft.repeat,draft.uncertain,draft.add,draft.edit,draft.remove,draft.okay})require(!IsWindowEnabled(child),"Active edit control enabled.");
            require(IsWindowEnabled(draft.ranges) && IsWindowEnabled(draft.cancel),"Active schedule cannot be inspected or closed.");
            const auto count=draft.policy.rangeCount;skipProc(window,WM_COMMAND,SkipRemove,0);skipProc(window,WM_COMMAND,SkipAdd,0);require(count==draft.policy.rangeCount,"Forged active mutation changed draft.");
            choose(draft.mode,0);skipProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDCANCEL,"Forged active OK committed.");};
        editSkip();require(app.settings.timeSkip.rangeCount==16 && app.settings.timeSkip.mode==TimeSkipMode::Manual && initial==lapse::configurationCalls,"Active policy changed.");
    }
    app.status={};skipDialogFailure=true;const auto before=app.settings.timeSkip;editSkip();require(app.settings.timeSkip.mode==before.mode && !startupMessage.empty(),"Dialog creation failure lost settings or its message.");
    std::cout<<"PASS bounded sixteen-range editor, active read-only inspection, forged-command locks and creation failure\n";
}
void modalLayoutAndInactiveDraft(){
    HiddenFixture owned;setupSkip();app.settings.timeSkip.mode=TimeSkipMode::Manual;app.settings.timeSkip.multiplier=3;
    app.settings.timeSkip.repeatSeconds=60;app.settings.timeSkip.rangeCount=1;app.settings.timeSkip.ranges[0]={0,60};
    skipScript=[](HWND window,DLGPROC procedure,LPARAM parameter){
        if(procedure==customProc){rangeDraft(window,*reinterpret_cast<CustomDraft*>(parameter),0,120);return;}
        auto& draft=*reinterpret_cast<SkipDraft*>(parameter);require(caption(draft.speed)==L"3×","Valid nonpreset multiplier silently displayed 4×.");
        require(GetNextDlgTabItem(window,draft.mode,FALSE)==draft.speed && GetNextDlgTabItem(window,draft.speed,FALSE)==draft.ranges,"Manual mode tab order skipped its visible schedule.");
        skipProc(window,WM_COMMAND,SkipEdit,0);mode(window,draft,TimeSkipMode::Off);
        require(draft.repeatCleared && draft.policy.repeatSeconds==0 && draft.policy.rangeCount==1 && draft.policy.ranges[0].endSeconds==120,"Off did not retain ranges with a valid inactive repeat.");
        wchar_t help[2048]{};GetWindowTextW(draft.help,help,2048);require(std::wstring(help).find(L"reset to Never")!=std::wstring::npos,"Inactive repeat reset was not explained.");
        skipProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Off was blocked by hidden repeat validation.");
    };
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.multiplier==3 && !app.settings.timeSkip.repeatSeconds && app.settings.timeSkip.ranges[0].endSeconds==120,"Exact nonpreset/inactive policy changed on commit.");
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);mode(window,draft,TimeSkipMode::QuietWithinSchedule);
        SetWindowTextW(draft.quiet,L"0");skipProc(window,WM_COMMAND,IDOK,0);require(!outcome() && !caption(draft.error).empty(),"Invalid quiet duration was accepted.");
        for(int dpi:{96,144,192,288}){
            draft.fineExpanded=true;skipHelp(draft);RECT suggested{0,0,320,260};skipProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
            require(draft.dpi==dpi && (GetWindowLongPtrW(window,GWL_STYLE)&WS_HSCROLL) && (GetWindowLongPtrW(window,GWL_STYLE)&WS_VSCROLL),"Constrained modal lost DPI/scroll state.");
            for(HWND child:{draft.help,draft.error}){RECT bounds{};GetClientRect(child,&bounds);wchar_t value[2048]{};GetWindowTextW(child,value,2048);RECT measured{0,0,bounds.right,0};
                HDC dc=GetDC(window);auto prior=SelectObject(dc,draft.font);DrawTextW(dc,value,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,prior);ReleaseDC(window,dc);
                require(bounds.bottom>=measured.bottom,"Modal help/error text was clipped instead of measured/wrapped.");}
            for(HWND child:{draft.mode,draft.speed,draft.ramp,draft.quiet,draft.quietUnits,draft.ranges,draft.add,draft.repeat,draft.repeatUnits,draft.okay,draft.cancel}){
                skipReveal(window,draft,child);RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
                // A native list can be taller than a very short viewport; its
                // own scrolling remains available, while every field is reachable.
                require(bounds.right>0 && bounds.left<client.right && bounds.bottom>0 && bounds.top<client.bottom,"Modal keyboard target cannot be scrolled into view.");}
        }
        skipProc(window,WM_COMMAND,IDCANCEL,0);
    };
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::Off,"Cancelled DPI/error draft changed policy.");
    std::cout<<"PASS exact nonpreset multiplier, always-committable Off, visible inactive-repeat reset, native tab order and measured modal DPI/scroll layout\n";
}
void strictPolicy(){
    TimeSkipSettings original;original.mode=TimeSkipMode::QuietWithinSchedule;original.multiplier=64;original.rampFrames=60;original.quietAfterMs=int64_t(INT_MAX)*1000;
    original.repeatSeconds=INT_MAX;original.rangeCount=2;original.ranges[0]={INT_MAX-1,INT_MAX};original.ranges[1]={0,1};original.uncertainAsAbsent=false;
    const auto values=skipValues(original);TimeSkipSettings parsed;
    require(parseSkipValues(values,parsed) && !parsed.uncertainAsAbsent && parsed.quietAfterMs==int64_t(INT_MAX)*1000 && parsed.ranges[0].startSeconds==0 && parsed.ranges[1].endSeconds==INT_MAX,"Maximum policy failed exact canonical roundtrip.");
    for(size_t key=0;key<values.size();++key)for(auto invalid:{L"?",L"-1",L"1junk",L"999999999999999999999999999999999"}){
        auto bad=values;bad[key]=invalid;TimeSkipSettings untouched=original;require(!parseSkipValues(bad,untouched) && untouched.ranges[0].startSeconds==INT_MAX-1,"Malformed policy partially changed destination.");}
    auto bad=values;bad[5]=L"0:2;2:3";require(parseSkipValues(bad,parsed) && parsed.rangeCount==1 && parsed.ranges[0].endSeconds==3,"Loaded touching ranges not canonicalized.");
    for(auto invalid:{L"0:1;",L"0:1;;2:3",L"0:1:2",L"1:1",L"2:1",L"01:2",L"0:2147483648"}){bad[5]=invalid;require(!parseSkipValues(bad,parsed),"Malformed range string accepted.");}
    bad=values;bad[2]=L"1001";require(!parseSkipValues(bad,parsed),"Fractional quiet second loaded.");bad=values;bad[3]=L"16";require(!parseSkipValues(bad,parsed),"Invalid ramp frame count loaded.");
    require(skipSummary(original,86400000).find(L"64 d")!=std::wstring::npos,"Maximum target interval capped or overflowed.");
    std::cout<<"PASS exact bounded policy serialization, all-or-nothing parse, normalization and 64-day target display\n";
}
void nativeCompressionInsertion(){
    HiddenFixture owned;setupSkip();
    for(int field=0;field<4;++field){
        CustomDraft range;range.kind=CustomKind::Range;range.startSeconds=10;range.endSeconds=60;
        SkipDraft policy;policy.policy.mode=field==2?TimeSkipMode::Quiet:TimeSkipMode::Manual;policy.policy.repeatSeconds=600;policy.policy.rangeCount=1;policy.policy.ranges[0]={0,60};
        skipScript=[field](HWND window,DLGPROC procedure,LPARAM parameter){
            HWND edit{},error{};
            if(field<2){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);edit=field==0?draft.first:draft.second;error=draft.error;}
            else {auto& draft=*reinterpret_cast<SkipDraft*>(parameter);edit=field==2?draft.quiet:draft.repeat;error=draft.error;}
            const std::wstring prefix=field==0 || field==2?L"1":L"120",payload=prefix+std::wstring(110-prefix.size(),L' ')+L"junk";
            const auto insert=[&](const std::wstring& value){SendMessageW(edit,EM_SETSEL,0,-1);SendMessageW(edit,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(value.c_str()));};
            insert(payload);require(SendMessageW(edit,EM_GETLIMITTEXT,0,0)==96 && GetWindowTextLengthW(edit)==96,"Compression native truncation bypasses rejection length.");
            procedure(window,WM_COMMAND,IDOK,0);require(!outcome() && !caption(error).empty(),"Native pasted compression suffix became an accepted prefix.");
            insert(field==0 || field==3?L"0":prefix);procedure(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Ordinary native compression value was rejected.");
        };
        CustomTemplate resource;skipOwnedDialog(GetModuleHandleW(nullptr),&resource.dialog,app.window,field<2?customProc:skipProc,
            field<2?reinterpret_cast<LPARAM>(&range):reinterpret_cast<LPARAM>(&policy));
        require(app.settings.timeSkip.mode==TimeSkipMode::Off && !app.customDialog,"Standalone native insertion fixture committed live settings or kept a dialog.");
    }
    std::cout<<"PASS actual native paste rejects malformed range start/end, quiet and repeat fields; exact inputs including zero remain valid\n";
}
void statusAndNestedClose(){
    HiddenFixture owned;setupSkip();app.advancedExpanded=true;app.settings.timeSkip.mode=TimeSkipMode::Quiet;app.skipRevision++;
    Status value;value.state=State::Recording;value.timeSkip.enabled=true;value.timeSkip.reason=TimeSkipReason::Quiet;value.timeSkip.intervalMs=20000;
    value.timeSkip.lastCheckTick=GetTickCount64()-3000;value.timeSkip.observationDelayed=true;value.message=L"Original save error";value.error=true;
    applyStatus(value,true);require(app.skipDetailCaption.find(L"target every 20 s")!=std::wstring::npos && app.skipDetailCaption.find(L"3 s ago")!=std::wstring::npos && app.skipDetailCaption.find(L"delayed")!=std::wstring::npos,"Live target/freshness/delay facts missing.");
    require(statusCaption()==L"Original save error","Compression details replaced a main error.");
    const auto shown=app.skipDetailCaption;app.hiddenToTray=true;value.timeSkip.reason=TimeSkipReason::Unavailable;applyStatus(value);
    require(app.status.timeSkip.reason==TimeSkipReason::Unavailable && app.skipDetailCaption==shown && app.visibleDirty,"Hidden status did visual work or lost new facts.");
    app.hiddenToTray=false;applyStatus(value,true);require(app.skipDetailCaption.find(L"unavailable")!=std::wstring::npos,"Restore failed to refresh deferred facts.");
    value.state=State::Paused;applyStatus(value);require(app.skipDetailCaption.find(L"paused")!=std::wstring::npos && app.skipDetailCaption.find(L"ago")==std::wstring::npos,"Pause falsely claimed ongoing checks.");
    app.status={};app.advancedExpanded=false;
    skipScript=[](HWND window,DLGPROC procedure,LPARAM parameter){
        if(procedure==customProc){require(modalOutcomes.size()==2,"Missing nested ownership.");cancelOwnedDialogs();require(modalOutcomes[0].value==IDCANCEL && modalOutcomes[1].value==IDCANCEL,"Nested cancellation did not terminate both owned dialogs.");return;}
        auto& draft=*reinterpret_cast<SkipDraft*>(parameter);mode(window,draft,TimeSkipMode::Manual);skipProc(window,WM_COMMAND,SkipAdd,0);
    };
    editSkip();require(!app.customDialog && app.settings.timeSkip.mode==TimeSkipMode::Quiet,"Nested close left an orphan dialog or committed draft.");
    std::cout<<"PASS truthful live status, failure priority, hidden deferral, pause wording and nested close ownership\n";
}
void personModesAndManagement(){
    HiddenFixture owned;setupSkip();const int initial=lapse::configurationCalls;
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){
        auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
        const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
        require(SendMessageW(draft.mode,CB_GETCOUNT,0,0)==6 && lapse::uiPersonPackInspections==0 && !visible(draft.packManage) && !visible(draft.uncertain),
            "Default editor inspected pack or lacks six explicit modes.");
        mode(window,draft,TimeSkipMode::NoPerson);
        require(lapse::uiPersonPackInspections==1 && lapse::uiPersonPackDialogs==0 && visible(draft.packInfo) && visible(draft.packManage) && visible(draft.uncertain),
            "Person mode did not expose one inspected explicit management action.");
        require(caption(draft.labels[3]).find(L"person")!=std::wstring::npos && visible(draft.quiet) && !visible(draft.ranges),
            "Person mode did not reuse dwell without an unsolicited schedule.");
        require(caption(draft.packInfo).find(L"not selected")!=std::wstring::npos && caption(draft.help).find(L"even when other things move")!=std::wstring::npos,
            "Person scope/absence trigger explanation missing.");
        for(int i=0;i<10;++i){skipLayout(window,draft);skipHelp(draft);updateControls();}
        require(lapse::uiPersonPackInspections==1 && !lapse::uiPersonPackDialogs,"Repeated layout/status inspected or opened the pack.");
        lapse::uiPersonPackInstallOnDialog=true;skipProc(window,WM_COMMAND,SkipPackManage,0);
        require(lapse::uiPersonPackDialogs==1 && lapse::uiPersonPackOwner==window && lapse::uiPersonPackInspections==2 &&
            caption(draft.packInfo).find(L"installed")!=std::wstring::npos,"Explicit manager did not refresh cached availability.");
        mode(window,draft,TimeSkipMode::Quiet);skipProc(window,WM_COMMAND,SkipPackManage,0);
        require(!visible(draft.packManage) && !visible(draft.uncertain) && lapse::uiPersonPackDialogs==1,"Person-only controls remained visible outside person mode.");
        mode(window,draft,TimeSkipMode::NoPersonWithinSchedule);
        require(visible(draft.ranges) && visible(draft.quiet) && lapse::uiPersonPackInspections==2,"Person schedule lost controls or repeated hash inspection.");
        skipProc(window,WM_COMMAND,IDCANCEL,0);
    };
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::Off && lapse::configurationCalls==initial && app.personPackKnown,
        "Explicit pack management committed a cancelled recording policy.");
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
        mode(window,draft,TimeSkipMode::NoPersonWithinSchedule);SetWindowTextW(draft.quiet,L"1.5");choose(draft.quietUnits,1);
        draft.policy.rangeCount=1;draft.policy.ranges[0]={0,600};skipList(draft);skipProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Valid person policy rejected.");};
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::NoPersonWithinSchedule && app.settings.timeSkip.quietAfterMs==90000 &&
        app.settings.timeSkip.rangeCount==1 && app.settings.timeSkip.ranges[0].endSeconds==600 && IsWindowEnabled(app.record),
        "Exact person policy lost values or camera-only checks blocked ordinary desktop recording.");
    const unsigned inspections=lapse::uiPersonPackInspections,dialogs=lapse::uiPersonPackDialogs;
    for(auto state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;
        skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
            require(draft.readOnly && !IsWindowEnabled(draft.packManage),"Active manager remained enabled.");
            skipProc(window,WM_COMMAND,SkipPackManage,0);skipProc(window,WM_COMMAND,IDCANCEL,0);};
        editSkip();
    }
    require(inspections==lapse::uiPersonPackInspections && dialogs==lapse::uiPersonPackDialogs,"Active inspection/forged management did pack work.");
    app.status={};std::cout<<"PASS explicit person modes, exact reused dwell/schedule, cached management, cancel isolation and active locks\n";
}
void personLayoutAndStatus(){
    HiddenFixture owned;setupSkip();app.settings.timeSkip.mode=TimeSkipMode::NoPersonWithinSchedule;
    app.settings.timeSkip.rangeCount=1;app.settings.timeSkip.ranges[0]={0,600};
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
        for(int dpi:{96,144,192,288}){
            RECT suggested{0,0,320,260};skipProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
            for(HWND child:{draft.uncertain,draft.packInfo,draft.help,draft.error}){RECT bounds{};GetClientRect(child,&bounds);wchar_t value[2048]{};GetWindowTextW(child,value,2048);
                RECT measured{0,0,bounds.right-(child==draft.uncertain?draft.scale(24):0),0};HDC dc=GetDC(window);auto prior=SelectObject(dc,draft.font);
                DrawTextW(dc,value,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,prior);ReleaseDC(window,dc);
                require(bounds.bottom>=measured.bottom,"Person management/help text clipped at constrained DPI.");}
            for(HWND child:{draft.quiet,draft.uncertain,draft.packManage,draft.ranges,draft.repeat,draft.okay,draft.cancel}){
                skipReveal(window,draft,child);RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
                require(bounds.right>0 && bounds.left<client.right && bounds.bottom>0 && bounds.top<client.bottom,"Person dialog keyboard control unreachable.");}
            const auto checked=SendMessageW(draft.uncertain,BM_GETCHECK,0,0);
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.cancel),TRUE);SendMessageW(window,WM_VSCROLL,SB_BOTTOM,0);
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.uncertain),TRUE);
            RECT checkBounds{},checkClient{};GetWindowRect(draft.uncertain,&checkBounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&checkBounds),2);GetClientRect(window,&checkClient);
            if(GetFocus()!=draft.uncertain || checkBounds.top<0 || checkBounds.bottom>checkClient.bottom || checkBounds.right<=0 || checkBounds.left>=checkClient.right || SendMessageW(draft.uncertain,BM_GETCHECK,0,0)!=checked)
                std::cerr<<"Uncertainty focus dpi="<<dpi<<" focused="<<(GetFocus()==draft.uncertain)<<" bounds="<<checkBounds.left<<","<<checkBounds.top<<","<<checkBounds.right<<","<<checkBounds.bottom<<" client="<<checkClient.right<<","<<checkClient.bottom<<" checked="<<SendMessageW(draft.uncertain,BM_GETCHECK,0,0)<<" expected="<<checked<<"\n";
            require(GetFocus()==draft.uncertain && checkBounds.top>=0 && checkBounds.bottom<=checkClient.bottom && checkBounds.right>0 && checkBounds.left<checkClient.right && SendMessageW(draft.uncertain,BM_GETCHECK,0,0)==checked,"Native uncertainty checkbox focus did not reveal its wrapped row or changed its value.");
            HDC dc=GetDC(window);auto prior=SelectObject(dc,draft.font);RECT combo{};GetClientRect(draft.mode,&combo);
            for(const auto* label:SkipModeLabels){SIZE size{};GetTextExtentPoint32W(dc,label,static_cast<int>(std::wcslen(label)),&size);
                require(size.cx+draft.scale(28)<=combo.right,"Selected person mode label truncates at minimum canvas width.");}
            auto manage=caption(draft.packManage);manage.erase(std::remove(manage.begin(),manage.end(),L'&'),manage.end());SIZE extent{};RECT button{};GetClientRect(draft.packManage,&button);
            GetTextExtentPoint32W(dc,manage.c_str(),static_cast<int>(manage.size()),&extent);
            require(extent.cx+draft.scale(12)<=button.right,"Detector management button truncates at constrained DPI.");
            SelectObject(dc,prior);ReleaseDC(window,dc);
        }
        skipProc(window,WM_COMMAND,IDCANCEL,0);
    };
    editSkip();app.advancedExpanded=true;++app.skipRevision;
    Status status;status.state=State::Recording;status.timeSkip.enabled=true;status.timeSkip.intervalMs=20000;
    status.timeSkip.lastCheckTick=GetTickCount64()-2000;status.message=L"Keep original save failure";status.error=true;
    for(auto item:{std::pair{TimeSkipReason::NoPerson,L"No person detected"},std::pair{TimeSkipReason::NoPersonUncertain,L"No person detected (uncertain)"},std::pair{TimeSkipReason::PersonPresent,L"Person detected"},
        std::pair{TimeSkipReason::Checking,L"Checking for absence"},std::pair{TimeSkipReason::Unavailable,L"Checks unavailable"},
        std::pair{TimeSkipReason::PersonUncertain,L"Person check uncertain"}}){
        status.timeSkip.reason=item.first;applyStatus(status,true);require(app.skipDetailCaption.find(item.second)!=std::wstring::npos &&
            app.skipDetailCaption.find(L"source check")!=std::wstring::npos && statusCaption()==L"Keep original save failure", "Person status lost scope, age or error priority.");}
    const unsigned inspections=lapse::uiPersonPackInspections;for(int i=0;i<10;++i)applyStatus(status);
    require(lapse::uiPersonPackInspections==inspections,"Person status polling hashed the pack.");
    app.status={};std::cout<<"PASS person modal DPI/wrap/focus/labels and truthful status without polling pack work\n";
}
void personUncertaintyChoice(){
    HiddenFixture owned;setupSkip();const int initial=lapse::configurationCalls;
    require(app.settings.timeSkip.uncertainAsAbsent,"Default uncertainty policy is not no person.");
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
        const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
        require(!visible(draft.uncertain) && SendMessageW(draft.uncertain,BM_GETCHECK,0,0)==BST_CHECKED,"Default draft checkbox lost the enabled default.");
        mode(window,draft,TimeSkipMode::NoPerson);
        require(visible(draft.uncertain) && IsWindowEnabled(draft.uncertain) && caption(draft.help).find(L"count toward the no-person waiting time")!=std::wstring::npos,"Person mode did not explain the enabled uncertainty choice.");
        require(GetNextDlgTabItem(window,draft.quietUnits,FALSE)==draft.uncertain && GetNextDlgTabItem(window,draft.uncertain,FALSE)==draft.packManage,"Uncertainty checkbox is absent from the person-mode tab order.");
        SendMessageW(draft.uncertain,BM_CLICK,0,0);
        require(SendMessageW(draft.uncertain,BM_GETCHECK,0,0)==BST_UNCHECKED && caption(draft.help).find(L"Uncertain checks keep normal speed")!=std::wstring::npos && app.settings.timeSkip.uncertainAsAbsent,"Native uncertainty click did not update help or mutated accepted settings.");
        mode(window,draft,TimeSkipMode::Quiet);require(!visible(draft.uncertain),"Quiet mode exposed the person uncertainty setting.");
        mode(window,draft,TimeSkipMode::NoPerson);require(SendMessageW(draft.uncertain,BM_GETCHECK,0,0)==BST_UNCHECKED,"Mode switching lost the uncertainty draft.");
        skipProc(window,WM_COMMAND,IDCANCEL,0);
    };
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.uncertainAsAbsent && lapse::configurationCalls==initial,"Cancelled uncertainty opt-out changed accepted settings.");
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);mode(window,draft,TimeSkipMode::NoPerson);
        SendMessageW(draft.uncertain,BM_CLICK,0,0);SendMessageW(draft.okay,BM_CLICK,0,0);require(outcome()==IDOK,"Valid uncertainty opt-out failed native acceptance.");};
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::NoPerson && !app.settings.timeSkip.uncertainAsAbsent && !lapse::configured.timeSkip.uncertainAsAbsent && lapse::configurationCalls==initial+1,"Accepted uncertainty opt-out was not configured exactly once.");
    app.settings.layers=preset(Mode::Camera);app.personPackKnown=true;app.personPack.state=PersonPackState::Ready;
    require(personAvailability().find(L"Uncertain, missing, failed or stale checks keep")!=std::wstring::npos,"Opt-out availability claims uncertainty can speed up.");
    app.settings.timeSkip.uncertainAsAbsent=true;
    require(personAvailability().find(L"Uncertain checks count as no person")!=std::wstring::npos,"Enabled availability hides its uncertainty policy.");
    app.settings.timeSkip.uncertainAsAbsent=false;
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
        require(SendMessageW(draft.uncertain,BM_GETCHECK,0,0)==BST_UNCHECKED,"Reopened dialog lost uncertainty opt-out.");SendMessageW(draft.uncertain,BM_CLICK,0,0);skipProc(window,WM_COMMAND,IDCANCEL,0);};
    editSkip();require(!app.settings.timeSkip.uncertainAsAbsent,"Cancelled uncertainty opt-in changed accepted settings.");
    for(auto state:{State::Waiting,State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;
        skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
            require(draft.readOnly && !IsWindowEnabled(draft.uncertain) && SendMessageW(draft.uncertain,BM_GETCHECK,0,0)==BST_UNCHECKED,"Active uncertainty option is editable or displays a different choice.");
            SendMessageW(draft.uncertain,BM_SETCHECK,BST_CHECKED,0);skipProc(window,WM_COMMAND,SkipUncertain,0);skipProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDCANCEL,"Forged active uncertainty acceptance committed.");};
        editSkip();require(!app.settings.timeSkip.uncertainAsAbsent && lapse::configurationCalls==initial+1,"Active uncertainty mutation reached accepted or engine settings.");
    }
    app.status={};std::cout<<"PASS enabled uncertainty default, native opt-out, mode/tab behavior, cancel isolation, exact configuration, truthful availability and frozen active choices\n";
}
void nativeModalButtons(){
    HiddenFixture owned;setupSkip();
    const auto focus=[](HWND window,HWND child){
        SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(child),TRUE);
        RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
        if(GetFocus()!=child || bounds.left<0 || bounds.top<0 || bounds.right>client.right || bounds.bottom>client.bottom)
            std::cerr<<"Native focus id="<<GetDlgCtrlID(child)<<" focused="<<(GetFocus()==child)<<" bounds="<<bounds.left<<","<<bounds.top<<","<<bounds.right<<","<<bounds.bottom<<" client="<<client.right<<","<<client.bottom<<"\n";
        require(GetFocus()==child && bounds.left>=0 && bounds.top>=0 && bounds.right<=client.right && bounds.bottom<=client.bottom,
            "Native compression button focus did not reveal the whole control.");
    };
    const auto key=[](HWND window,WPARAM value){MSG message{};message.hwnd=GetFocus();message.message=WM_KEYDOWN;message.wParam=value;
        require(IsDialogMessageW(window,&message),"Native compression dialog key was not handled.");};
    int nested=0;
    skipScript=[&](HWND window,DLGPROC procedure,LPARAM parameter){
        if(procedure==customProc){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);++nested;
            SetWindowTextW(draft.first,nested==1?L"30":L"31");SetWindowTextW(draft.second,L"40");
            focus(window,draft.okay);require(!outcome(),"Nested range OK focus accepted the draft.");SendMessageW(draft.okay,BM_CLICK,0,0);return;}
        auto& draft=*reinterpret_cast<SkipDraft*>(parameter);mode(window,draft,TimeSkipMode::NoPersonWithinSchedule);
        draft.policy.rangeCount=1;draft.policy.ranges[0]={10,20};skipList(draft,0);
        for(int dpi:{96,192}){
            RECT suggested{0,0,380,230};skipProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
            const unsigned dialogs=lapse::uiPersonPackDialogs,inspections=lapse::uiPersonPackInspections;
            const int calls=skipDialogCalls;
            for(HWND button:{draft.add,draft.edit,draft.remove,draft.packManage,draft.okay,draft.cancel}){
                focus(window,button);
                require(!outcome() && skipDialogCalls==calls && lapse::uiPersonPackDialogs==dialogs && lapse::uiPersonPackInspections==inspections &&
                    draft.policy.rangeCount==1 && draft.policy.ranges[0].startSeconds==10 && draft.policy.ranges[0].endSeconds==20,
                    "Compression focus activated a range, management, OK or Cancel action.");
            }
        }
        SendMessageW(draft.add,BM_CLICK,0,0);require(nested==1 && draft.policy.rangeCount==2,"Actual Add click failed.");
        SendMessageW(draft.edit,BM_CLICK,0,0);require(nested==2 && draft.policy.ranges[1].startSeconds==31,"Actual Edit click failed.");
        focus(window,draft.remove);SendMessageW(draft.remove,WM_KEYDOWN,VK_SPACE,0);SendMessageW(draft.remove,WM_KEYUP,VK_SPACE,0);
        require(draft.policy.rangeCount==1,"Actual Remove Space activation failed.");
        const unsigned managed=lapse::uiPersonPackDialogs;SendMessageW(draft.packManage,BM_CLICK,0,0);
        require(lapse::uiPersonPackDialogs==managed+1,"Explicit detector management click failed.");
        SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.mode),TRUE);
        require(GetFocus()==draft.mode,"Native compression mode focus failed.");
        key(window,VK_RETURN);require(outcome()==IDOK,"Native Enter failed to accept a valid compression draft.");
    };
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::NoPersonWithinSchedule && app.settings.timeSkip.rangeCount==1,"Native dialog acceptance failed to commit policy.");
    for(bool readOnly:{false,true}){
        app.status.state=readOnly?State::Recording:State::Idle;
        const auto policy=app.settings.timeSkip;const int configurationCount=lapse::configurationCalls;
        skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
            require(!outcome() && draft.readOnly==readOnly,"Read-only initialization activated Close on focus.");
            RECT suggested{0,0,380,230};
            skipProc(window,WM_DPICHANGED,MAKELONG(96,96),reinterpret_cast<LPARAM>(&suggested));
            focus(window,draft.cancel);require(!outcome(),"Focusing Close/Cancel dismissed the dialog.");
            if(readOnly){SendMessageW(draft.cancel,WM_KEYDOWN,VK_SPACE,0);SendMessageW(draft.cancel,WM_KEYUP,VK_SPACE,0);}
            else key(window,VK_ESCAPE);
            require(outcome()==IDCANCEL,"Read-only Close Space or idle Escape failed.");
        };editSkip();
        require(lapse::configurationCalls==configurationCount && app.settings.timeSkip.mode==policy.mode && app.settings.timeSkip.rangeCount==policy.rangeCount,
            "Closing modal inspection changed recording options.");
    }
    app.status={};std::cout<<"PASS native compression button focus, safe actions, nested ranges, manager clicks, Enter/Escape/Space and read-only Close\n";
}
void fineTuning(){
    HiddenFixture owned;setupSkip();app.settings.timeSkip.mode=TimeSkipMode::Quiet;
    app.settings.timeSkip.rampFrames=60;app.settings.timeSkip.quietSensitivity=QuietSensitivity::High;
    const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    const auto toggle=[](HWND window,SkipDraft& draft){
        const bool before=draft.fineExpanded;SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.fine),TRUE);
        require(GetFocus()==draft.fine && draft.fineExpanded==before && !outcome(),"Focusing Fine tuning activated the disclosure or dialog.");
        SendMessageW(draft.fine,WM_KEYDOWN,VK_SPACE,0);SendMessageW(draft.fine,WM_KEYUP,VK_SPACE,0);
        require(draft.fineExpanded!=before && !outcome(),"Space failed to toggle Fine tuning safely.");
    };
    const int initial=lapse::configurationCalls;
    skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
        require(!draft.fineExpanded && !visible(draft.ramp) && !visible(draft.sensitivity) &&
            caption(draft.fine).find(L"2 s transition")!=std::wstring::npos && caption(draft.fine).find(L"High sensitivity")!=std::wstring::npos,
            "Default collapse hid nondefault tuning settings or left tuning controls visible.");
        require(caption(draft.help).find(L"not people")!=std::wstring::npos && caption(draft.help).find(L"stale")!=std::wstring::npos,
            "Quiet help lost image-change scope or conservative fallback.");
        toggle(window,draft);require(visible(draft.ramp)&&visible(draft.sensitivity),"Expanded Quiet tuning did not show both settings.");
        for(int dpi:{96,144,192,288}){
            RECT suggested{0,0,360,260};skipProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.sensitivity),TRUE);
            RECT bounds{},client{};GetWindowRect(draft.sensitivity,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
            require(GetFocus()==draft.sensitivity && bounds.right>0 && bounds.left<client.right && bounds.bottom>0 && bounds.top<client.bottom,
                "Fine tuning field could not be revealed by native focus at constrained DPI.");
            HDC dc=GetDC(window);auto prior=SelectObject(dc,draft.font);RECT button{};GetClientRect(draft.fine,&button);auto value=caption(draft.fine);value.erase(std::remove(value.begin(),value.end(),L'&'),value.end());SIZE measured{};
            GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&measured);SelectObject(dc,prior);ReleaseDC(window,dc);
            require(measured.cx+draft.scale(12)<=button.right,"Fine tuning nondefault summary truncated at minimum canvas width.");
        }
        toggle(window,draft);choose(draft.ramp,-1);skipProc(window,WM_COMMAND,IDOK,0);
        require(!outcome() && draft.fineExpanded && GetFocus()==draft.ramp && !caption(draft.error).empty(),"Hidden invalid transition did not expand and focus its error.");
        choose(draft.ramp,1);toggle(window,draft);choose(draft.sensitivity,-1);skipProc(window,WM_COMMAND,IDOK,0);
        require(!outcome() && draft.fineExpanded && GetFocus()==draft.sensitivity,"Hidden invalid sensitivity did not reveal its field.");
        mode(window,draft,TimeSkipMode::NoPerson);require(!visible(draft.sensitivity)&&visible(draft.ramp)&&caption(draft.help).find(L"camera only")!=std::wstring::npos,
            "Person mode exposed a quiet sensitivity or lost camera scope.");
        mode(window,draft,TimeSkipMode::Off);skipProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Off was blocked by an invalid inactive tuning control.");
    };editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.quietSensitivity==QuietSensitivity::High &&
        lapse::configurationCalls==initial+1,"Atomic Off commit changed inactive sensitivity.");
    app.settings.timeSkip.mode=TimeSkipMode::Quiet;
    skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);toggle(window,draft);choose(draft.sensitivity,0);choose(draft.ramp,0);skipProc(window,WM_COMMAND,IDCANCEL,0);};
    editSkip();require(app.settings.timeSkip.quietSensitivity==QuietSensitivity::High && app.settings.timeSkip.rampFrames==30,"Cancelled tuning changed saved policy.");
    for(auto state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;
        skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
            require(IsWindowEnabled(draft.fine) && !IsWindowEnabled(draft.ramp) && !IsWindowEnabled(draft.sensitivity),"Read-only tuning lost inspection or unlocked session options.");
            toggle(window,draft);require(visible(draft.sensitivity),"Read-only disclosure could not expose saved tuning.");
            skipProc(window,WM_COMMAND,IDCANCEL,0);
        };editSkip();
    }
    app.status={};require(app.settings.timeSkip.quietSensitivity==QuietSensitivity::High && lapse::configurationCalls==initial+1,"Read-only inspection reconfigured tuning.");
    std::cout<<"PASS Fine tuning default/nondefault disclosure, native keyboard/DPI, hidden validation, Off, cancellation and read-only inspection\n";
}
void nativeScheduleMnemonicsAndReadOnlyEnter(){
    HiddenFixture owned;setupSkip();
    const auto key=[](HWND window,UINT message,WPARAM value){MSG input{};input.hwnd=GetFocus();input.message=message;input.wParam=value;
        if(message==WM_SYSCHAR)input.lParam=1L<<29;require(IsDialogMessageW(window,&input),"Native schedule key was not handled.");};
    for(auto policyMode:{TimeSkipMode::Manual,TimeSkipMode::QuietWithinSchedule,TimeSkipMode::NoPersonWithinSchedule}){
        app.settings.timeSkip.mode=policyMode;app.settings.timeSkip.rangeCount=1;app.settings.timeSkip.ranges[0]={0,60};
        const int configurations=lapse::configurationCalls;
        skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
            require(IsWindowEnabled(draft.remove),"Selected schedule did not enable Remove.");
            RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
            screen.right=screen.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);screen.bottom=screen.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
            SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE);
            SetWindowPos(window,nullptr,screen.right+20000,screen.bottom+20000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            RECT placed{},overlap{};GetWindowRect(window,&placed);require(!IntersectRect(&overlap,&placed,&screen),"Schedule mnemonic fixture must remain offscreen.");
            ShowWindow(window,SW_SHOWNOACTIVATE);
            for(HWND start:{draft.mode,draft.ranges,draft.add,draft.edit,draft.remove,draft.cancel}){
                SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(start),TRUE);key(window,WM_SYSCHAR,L'r');
                require(GetFocus()==draft.ranges && draft.policy.rangeCount==1 && !outcome(),"Alt+R must focus ranges without removing a selected range.");
            }
            key(window,WM_SYSCHAR,L'm');require(draft.policy.rangeCount==0 && !IsWindowEnabled(draft.remove) && !outcome(),"Alt+M failed to remove only the selected draft range.");
            GetWindowRect(window,&placed);require(!IntersectRect(&overlap,&placed,&screen),"Schedule mnemonic fixture moved onto the desktop.");
            ShowWindow(window,SW_HIDE);skipProc(window,WM_COMMAND,IDCANCEL,0);
        };editSkip();
        require(app.settings.timeSkip.rangeCount==1 && lapse::configurationCalls==configurations,"Cancelled mnemonic edit changed persistent policy.");
    }
    for(auto state:{State::Starting,State::Recording,State::Paused,State::Finishing}){
        app.status.state=state;const int configurations=lapse::configurationCalls;
        skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);
            require(draft.readOnly && IsWindowEnabled(draft.ranges) && !IsWindowEnabled(draft.remove),"Read-only schedule inspection locks changed.");
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.ranges),TRUE);require(GetFocus()==draft.ranges,"Read-only range focus failed.");
            key(window,WM_KEYDOWN,VK_RETURN);require(outcome()==IDCANCEL,"Native Enter from the inspected schedule did not close the read-only dialog.");
        };editSkip();require(app.settings.timeSkip.rangeCount==1 && lapse::configurationCalls==configurations,"Read-only Enter changed recording policy.");
    }
    app.status={};std::cout<<"PASS unambiguous native range/Remove mnemonics, canceled drafts and Enter-to-close across active states\n";
}
void nativePageWheelOwnership(){
    const auto position=[](HWND window){SCROLLINFO info{sizeof(info),SIF_POS};require(GetScrollInfo(window,SB_VERT,&info)!=FALSE,"Native page scrollbar unavailable.");return info.nPos;};
    const auto constrain=[](HWND window){RECT suggested{0,0,320,230};SendMessageW(window,WM_DPICHANGED,MAKELONG(192,192),reinterpret_cast<LPARAM>(&suggested));
        SCROLLINFO info{sizeof(info),SIF_ALL};require(GetScrollInfo(window,SB_VERT,&info)!=FALSE && int64_t(info.nMax)-info.nPage+1>info.nMin,"Wheel fixture must have real vertical overflow.");};
    const auto wheel=[](HWND target,WORD keys=0){SendMessageW(target,WM_MOUSEWHEEL,MAKEWPARAM(keys,static_cast<WORD>(-WHEEL_DELTA)),0);};
    for(auto policyMode:{TimeSkipMode::QuietWithinSchedule,TimeSkipMode::NoPersonWithinSchedule})for(bool readOnly:{false,true}){
        HiddenFixture owned;setupSkip();app.settings.timeSkip.mode=policyMode;
        app.settings.timeSkip.rangeCount=16;for(unsigned i=0;i<16;++i)app.settings.timeSkip.ranges[i]={int(i*60),int(i*60+30)};
        app.status.state=readOnly?State::Recording:State::Idle;
        const auto accepted=skipValues(app.settings.timeSkip);const int configurationCount=lapse::configurationCalls;int nested=0;HWND outer{};
        skipScript=[&](HWND window,DLGPROC procedure,LPARAM parameter){
            if(procedure==customProc){
                ++nested;auto& draft=*reinterpret_cast<CustomDraft*>(parameter);require(outer && draft.kind==CustomKind::Range,"Unexpected nested wheel owner.");
                constrain(window);const int outerBefore=position(outer);const auto first=caption(draft.first),second=caption(draft.second);
                for(HWND target:{draft.help,draft.first}){SetFocus(draft.first);SendMessageW(window,WM_VSCROLL,SB_TOP,0);const HWND focused=GetFocus();wheel(target);
                    require(position(window)>0 && position(outer)==outerBefore && GetFocus()==focused && !outcome(),"Nested wheel moved the outer page, changed focus or failed to scroll its own page.");}
                require(caption(draft.first)==first && caption(draft.second)==second,"Nested page wheel changed range values.");
                SendMessageW(window,WM_COMMAND,IDCANCEL,0);return;
            }
            auto& draft=*reinterpret_cast<SkipDraft*>(parameter);outer=window;require(draft.readOnly==readOnly,"Wheel inspection lock mismatch.");constrain(window);
            const auto policy=skipValues(draft.policy);const int speed=choice(draft.speed);const auto quiet=caption(draft.quiet),repeat=caption(draft.repeat);const auto uncertain=SendMessageW(draft.uncertain,BM_GETCHECK,0,0);
            std::vector<HWND> targets{window,draft.help,draft.cancel};if(!readOnly){targets.push_back(draft.quiet);targets.push_back(draft.speed);}
            if(skipPerson(policyMode))targets.push_back(draft.uncertain);
            for(HWND target:targets){SetFocus(readOnly?draft.cancel:draft.quiet);SendMessageW(window,WM_VSCROLL,SB_TOP,0);const HWND focused=GetFocus();wheel(target);
                require(position(window)>0 && GetFocus()==focused && !outcome(),"Compression wheel did not scroll the page without focus/action side effects.");
                require(choice(draft.speed)==speed && caption(draft.quiet)==quiet && caption(draft.repeat)==repeat && SendMessageW(draft.uncertain,BM_GETCHECK,0,0)==uncertain && skipValues(draft.policy)==policy,"Page, checkbox or closed-combo wheel changed the compression draft.");}
            if(!readOnly){
                struct YieldReset {~YieldReset(){reportedOpenWheelCombo=yieldWheelCombo=nullptr;}} reset;
                yieldWheelCombo=draft.speed;SendMessageW(window,WM_VSCROLL,SB_TOP,0);const int before=nativeWheelYields;
                wheel(draft.speed,MK_CONTROL);require(position(window)==0 && nativeWheelYields==before+1,"Ctrl+wheel failed to yield to native combo processing.");
                reportedOpenWheelCombo=draft.speed;wheel(draft.speed);require(position(window)==0 && nativeWheelYields==before+2,"Open dropdown failed to retain native wheel ownership.");
            }
            SetFocus(draft.ranges);SendMessageW(window,WM_VSCROLL,SB_TOP,0);SendMessageW(draft.ranges,LB_SETTOPINDEX,0,0);
            const auto selected=SendMessageW(draft.ranges,LB_GETCURSEL,0,0);UINT nativeLines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&nativeLines,0);wheel(draft.ranges);
            const auto listTop=SendMessageW(draft.ranges,LB_GETTOPINDEX,0,0);
            require((nativeLines?listTop>0:listTop==0) && position(window)==0 && SendMessageW(draft.ranges,LB_GETCURSEL,0,0)==selected && !outcome(),"Native range list wheel moved its parent or changed selection instead of preserving local scroll behavior.");
            if(!readOnly){SendMessageW(draft.edit,BM_CLICK,0,0);require(nested==1 && app.customDialog==window && !outcome(),"Nested range wheel left wrong modal ownership.");}
            require(skipValues(draft.policy)==policy && choice(draft.speed)==speed && caption(draft.quiet)==quiet && caption(draft.repeat)==repeat,"Wheel inspection changed the final draft.");
            require(!IsWindowVisible(window) && !IsWindowVisible(app.window),"Wheel fixture became visible.");SendMessageW(window,WM_COMMAND,IDCANCEL,0);
        };
        editSkip();require(skipValues(app.settings.timeSkip)==accepted && lapse::configurationCalls==configurationCount && !app.customDialog && !lapse::recordCalls,"Wheel navigation committed settings, started capture or left a modal.");
        app.status={};
    }
    std::cout<<"PASS compression page/closed-combo wheel, native list ownership, nested range isolation, read-only scrolling and safe Ctrl/open-dropdown yield\n";
}
}
int main(){try{transactionalRanges();boundsAndFreeze();modalLayoutAndInactiveDraft();strictPolicy();nativeCompressionInsertion();statusAndNestedClose();personModesAndManagement();personLayoutAndStatus();personUncertaintyChoice();nativeModalButtons();fineTuning();nativeScheduleMnemonicsAndReadOnlyEnter();nativePageWheelOwnership();std::cout<<"All thirteen time-compression UI groups passed using owned synthetic windows only.\n";return 0;}
catch(const std::exception& error){std::cerr<<"TIME COMPRESSION UI FAILURE: "<<error.what()<<'\n';return 1;}}

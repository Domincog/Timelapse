// Owned hidden native dialogs and inert engine. Nested modal calls are scripted;
// no device capture, ordinary application launch, persistent UI or settings I/O.
#include <windows.h>
#include <functional>
#include <vector>
namespace {
std::function<void(HWND,DLGPROC,LPARAM)> skipScript;
struct ModalOutcome {HWND window{};INT_PTR value=0;};
std::vector<ModalOutcome> modalOutcomes;
int skipDialogCalls=0;
bool skipDialogFailure=false;
INT_PTR WINAPI skipOwnedDialog(HINSTANCE,LPCDLGTEMPLATEW,HWND,DLGPROC,LPARAM);
BOOL WINAPI skipOwnedEndDialog(HWND window,INT_PTR value){for(auto& modal:modalOutcomes)if(modal.window==window)modal.value=value;return TRUE;}
}
#define DialogBoxIndirectParamW skipOwnedDialog
#define EndDialog skipOwnedEndDialog
#define main sourceFixtureMain
#include "ui_source_tests.cpp"
#undef main
#undef EndDialog
#undef DialogBoxIndirectParamW
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
            for(HWND child:{draft.mode,draft.speed,draft.ramp,draft.quiet,draft.repeat,draft.add,draft.edit,draft.remove,draft.okay})require(!IsWindowEnabled(child),"Active edit control enabled.");
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
        require(GetNextDlgTabItem(window,draft.mode,FALSE)==draft.speed && GetNextDlgTabItem(window,draft.speed,FALSE)==draft.ramp,"Native modal tab order changed.");
        skipProc(window,WM_COMMAND,SkipEdit,0);mode(window,draft,TimeSkipMode::Off);
        require(draft.repeatCleared && draft.policy.repeatSeconds==0 && draft.policy.rangeCount==1 && draft.policy.ranges[0].endSeconds==120,"Off did not retain ranges with a valid inactive repeat.");
        wchar_t help[2048]{};GetWindowTextW(draft.help,help,2048);require(std::wstring(help).find(L"reset to Never")!=std::wstring::npos,"Inactive repeat reset was not explained.");
        skipProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Off was blocked by hidden repeat validation.");
    };
    editSkip();require(app.settings.timeSkip.mode==TimeSkipMode::Off && app.settings.timeSkip.multiplier==3 && !app.settings.timeSkip.repeatSeconds && app.settings.timeSkip.ranges[0].endSeconds==120,"Exact nonpreset/inactive policy changed on commit.");
    skipScript=[](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<SkipDraft*>(parameter);mode(window,draft,TimeSkipMode::QuietWithinSchedule);
        SetWindowTextW(draft.quiet,L"0");skipProc(window,WM_COMMAND,IDOK,0);require(!outcome() && !caption(draft.error).empty(),"Invalid quiet duration was accepted.");
        for(int dpi:{96,144,192,288}){
            RECT suggested{0,0,320,260};skipProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
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
    original.repeatSeconds=INT_MAX;original.rangeCount=2;original.ranges[0]={INT_MAX-1,INT_MAX};original.ranges[1]={0,1};
    const auto values=skipValues(original);TimeSkipSettings parsed;
    require(parseSkipValues(values,parsed) && parsed.quietAfterMs==int64_t(INT_MAX)*1000 && parsed.ranges[0].startSeconds==0 && parsed.ranges[1].endSeconds==INT_MAX,"Maximum policy failed exact canonical roundtrip.");
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
        require(SendMessageW(draft.mode,CB_GETCOUNT,0,0)==6 && lapse::uiPersonPackInspections==0 && !visible(draft.packManage),
            "Default editor inspected pack or lacks six explicit modes.");
        mode(window,draft,TimeSkipMode::NoPerson);
        require(lapse::uiPersonPackInspections==1 && lapse::uiPersonPackDialogs==0 && visible(draft.packInfo) && visible(draft.packManage),
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
        require(!visible(draft.packManage) && lapse::uiPersonPackDialogs==1,"Hidden manager remained actionable outside person mode.");
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
            for(HWND child:{draft.packInfo,draft.help,draft.error}){RECT bounds{};GetClientRect(child,&bounds);wchar_t value[2048]{};GetWindowTextW(child,value,2048);
                RECT measured{0,0,bounds.right,0};HDC dc=GetDC(window);auto prior=SelectObject(dc,draft.font);
                DrawTextW(dc,value,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,prior);ReleaseDC(window,dc);
                require(bounds.bottom>=measured.bottom,"Person management/help text clipped at constrained DPI.");}
            for(HWND child:{draft.quiet,draft.packManage,draft.ranges,draft.repeat,draft.okay,draft.cancel}){
                skipReveal(window,draft,child);RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
                require(bounds.right>0 && bounds.left<client.right && bounds.bottom>0 && bounds.top<client.bottom,"Person dialog keyboard control unreachable.");}
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
    for(auto item:{std::pair{TimeSkipReason::NoPerson,L"No person detected"},std::pair{TimeSkipReason::PersonPresent,L"Person detected"},
        std::pair{TimeSkipReason::Checking,L"Checking for absence"},std::pair{TimeSkipReason::Unavailable,L"Checks unavailable"}}){
        status.timeSkip.reason=item.first;applyStatus(status,true);require(app.skipDetailCaption.find(item.second)!=std::wstring::npos &&
            app.skipDetailCaption.find(L"source check")!=std::wstring::npos && statusCaption()==L"Keep original save failure", "Person status lost scope, age or error priority.");}
    const unsigned inspections=lapse::uiPersonPackInspections;for(int i=0;i<10;++i)applyStatus(status);
    require(lapse::uiPersonPackInspections==inspections,"Person status polling hashed the pack.");
    app.status={};std::cout<<"PASS person modal DPI/wrap/focus/labels and truthful status without polling pack work\n";
}
}
int main(){try{transactionalRanges();boundsAndFreeze();modalLayoutAndInactiveDraft();strictPolicy();nativeCompressionInsertion();statusAndNestedClose();personModesAndManagement();personLayoutAndStatus();std::cout<<"All eight time-compression UI groups passed using owned hidden windows only.\n";return 0;}
catch(const std::exception& error){std::cerr<<"TIME COMPRESSION UI FAILURE: "<<error.what()<<'\n';return 1;}}

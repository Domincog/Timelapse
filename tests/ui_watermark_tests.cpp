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
BOOL WINAPI watermarkWheelSettings(UINT action,UINT parameter,PVOID value,UINT flags){
    if(action==SPI_GETWHEELSCROLLLINES || action==SPI_GETWHEELSCROLLCHARS){*static_cast<UINT*>(value)=3;return TRUE;}
    return SystemParametersInfoW(action,parameter,value,flags);
}
INT_PTR WINAPI skipOwnedDialog(HINSTANCE,LPCDLGTEMPLATEW,HWND,DLGPROC,LPARAM);
BOOL WINAPI skipOwnedEndDialog(HWND window,INT_PTR value){for(auto& modal:modalOutcomes)if(modal.window==window)modal.value=value;return TRUE;}
}
#define DialogBoxIndirectParamW skipOwnedDialog
#define EndDialog skipOwnedEndDialog
#define SystemParametersInfoW watermarkWheelSettings
#define main sourceFixtureMain
#include "ui_source_tests.cpp"
#undef main
#undef EndDialog
#undef DialogBoxIndirectParamW
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
    app.nightHint=app.nightDetail=nullptr;for(auto& tab:app.tabs)tab=nullptr;app.customDialog=nullptr;app.panelTab=CaptureTab;app.hiddenToTray=false;
    app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;choose(app.interval,2);choose(app.videoSize,0);choose(app.stopAfter,0);
    seed(false);skipScript={};skipDialogCalls=0;skipDialogFailure=false;
    app.personPack={};app.personPackKnown=false;lapse::uiPersonPackInfo={};lapse::uiPersonPackInspections=0;
    lapse::uiPersonPackDialogs=0;lapse::uiPersonPackOwner=nullptr;lapse::uiPersonPackInstallOnDialog=false;
}
}
namespace {
void setupWatermark(){setupSkip();app.settings.watermark={};app.watermarkChecked={};app.watermarkCheckValid=false;app.watermarkValidation.clear();app.watermarkCaption.clear();app.watermarkRevision=0;invalidatePanel();configure();}
void markCheck(HWND window,HWND child,int id,bool checked){SendMessageW(child,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);watermarkProc(window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(child));}
void markSelect(HWND window,HWND child,int id,int selected){choose(child,selected);watermarkProc(window,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(child));}
bool markVisible(HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;}
void draftTransactions(){
    HiddenFixture owned;setupWatermark();const int before=lapse::configurationCalls;
    skipScript=[&](HWND window,DLGPROC procedure,LPARAM parameter){require(procedure==watermarkProc,"Wrong watermark modal procedure.");auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        require(!draft.policy.enabled && !markVisible(draft.timeKind) && !markVisible(draft.x) && !draft.illustration.pixels.empty(),"Off watermark lost its minimal illustrated default.");
        markCheck(window,draft.enabled,MarkEnabled,true);markSelect(window,draft.timeKind,MarkTimeKind,1);markSelect(window,draft.position,MarkPosition,4);
        SetWindowTextW(draft.x,L"12.34");SetWindowTextW(draft.y,L"56.78");markSelect(window,draft.size,MarkSize,2);
        require(!app.settings.watermark.enabled && !caption(draft.error).size(),"Draft edits leaked into settings or failed valid rendering.");
        watermarkProc(window,WM_COMMAND,IDCANCEL,0);
    };editWatermark();require(!app.settings.watermark.enabled && lapse::configurationCalls==before && !app.customDialog,"Cancel changed watermark configuration or retained modal ownership.");
    skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        markCheck(window,draft.enabled,MarkEnabled,true);markSelect(window,draft.timeKind,MarkTimeKind,1);markSelect(window,draft.position,MarkPosition,4);
        SetWindowTextW(draft.x,L"12.34");SetWindowTextW(draft.y,L"56.78");markSelect(window,draft.size,MarkSize,2);
        const auto box=draft.renderer.lastBounds();require(box.right>box.left && box.bottom>box.top && box.left>=0 && box.top>=0 && box.right<=draft.illustration.width && box.bottom<=draft.illustration.height,"Synthetic watermark preview escaped its exact canvas.");
        watermarkProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Valid watermark Apply failed.");
    };editWatermark();const auto& value=app.settings.watermark;
    require(value.enabled&&value.showTime&&value.showSpeed&&value.timeKind==WatermarkTimeKind::RecordedLocal&&value.x==1234&&value.y==5678&&value.textSize==WatermarkTextSize::Large&&
        sameWatermarkSettings(value,lapse::configured.watermark)&&lapse::configurationCalls==before+1,"Apply did not commit the entire exact draft once.");
    const int revision=app.watermarkRevision;configure();configure();require(app.watermarkRevision==revision,"Unchanged configuration repeated watermark preflight.");
    std::cout<<"PASS default Off illustration, draft cancel, exact whole-policy Apply, bounded real-renderer preview and unchanged preflight cache\n";
}
void fieldsPositionsAndOff(){
    HiddenFixture owned;setupWatermark();app.settings.watermark.enabled=true;configure();
    for(int position=0;position<4;++position){skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        markSelect(window,draft.position,MarkPosition,position);markCheck(window,draft.time,MarkTime,false);choose(draft.timeKind,-1);
        watermarkProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Speed-only watermark was blocked by hidden time choice.");};editWatermark();
        require(app.settings.watermark.x==(position%2?10000:0)&&app.settings.watermark.y==(position>=2?10000:0)&&!app.settings.watermark.showTime&&app.settings.watermark.showSpeed,"Corner or speed-only choice changed.");}
    skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        markCheck(window,draft.speed,MarkSpeed,false);watermarkProc(window,WM_COMMAND,IDOK,0);require(!outcome()&&GetFocus()==draft.time&&!caption(draft.error).empty(),"Empty enabled watermark was accepted.");
        markCheck(window,draft.time,MarkTime,true);markSelect(window,draft.timeKind,MarkTimeKind,0);markSelect(window,draft.position,MarkPosition,4);
        for(const wchar_t* invalid:{L"-1",L"100.01",L"0.001",L"1junk"}){SetWindowTextW(draft.x,invalid);watermarkProc(window,WM_COMMAND,IDOK,0);require(!outcome()&&GetFocus()==draft.x,"Invalid exact percent accepted or focus lost.");}
        const auto pasted=L"1"+std::wstring(100,L' ')+L"bad";SetWindowTextW(draft.x,L"");SendMessageW(draft.x,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(pasted.c_str()));
        require(GetWindowTextLengthW(draft.x)==96,"Owned native insertion did not reach its rejection boundary.");watermarkProc(window,WM_COMMAND,IDOK,0);require(!outcome(),"Native pasted suffix was truncated into a valid percent.");
        markCheck(window,draft.enabled,MarkEnabled,false);watermarkProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Off was blocked by inactive invalid percent text.");
    };editWatermark();require(!app.settings.watermark.enabled&&app.settings.watermark.x==10000&&app.settings.watermark.y==10000,"Off invented inactive draft coordinates.");
    std::cout<<"PASS four whole-box corners, independent speed/time, empty-field rejection, exact percent/native paste limits and graceful Off\n";
}
void preflightAndFreeze(){
    HiddenFixture owned;setupWatermark();app.settings.watermark.enabled=true;
    app.hasCustomSize=true;app.customWidth=app.customHeight=48;app.committedSize=2;customItems();configure();updateControls();
    require(!app.watermarkValidation.empty()&&!IsWindowEnabled(app.record),"Unreadable tiny output did not block Record visibly.");
    const int before=lapse::recordCalls;windowProc(app.window,WM_COMMAND,Record,0);require(lapse::recordCalls==before,"Forged Record bypassed watermark preflight.");
    app.status.error=true;app.status.message=L"Owned primary engine failure";require(statusCaption()==app.status.message,"Watermark validation replaced the current engine failure.");app.status={};
    app.settings.watermark.enabled=false;configure();updateControls();require(app.watermarkValidation.empty()&&IsWindowEnabled(app.record),"Off retained an irrelevant tiny-output failure.");
    app.hasCustomSize=false;app.committedSize=0;customItems();app.settings.watermark.enabled=true;choose(app.mode,SeparateFilesMode);app.settings.separateFiles=true;app.settings.layers=preset(Mode::SideBySide);
    SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);choose(app.nightDuration,0);configure();require(app.watermarkValidation.empty()&&app.settings.night.enabled&&app.settings.separateFiles,"Watermark conflicted with paired Night settings.");
    const auto prior=app.settings.watermark;const int configuredBefore=lapse::configurationCalls;
    for(auto state:{State::Starting,State::Finishing}){app.status.state=state;skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        require(draft.readOnly&&!markVisible(draft.okay)&&!IsWindowEnabled(draft.enabled)&&!IsWindowEnabled(draft.position)&&!draft.illustration.pixels.empty(),"Starting/finishing watermark dialog was editable or lacked preview.");
        markCheck(window,draft.enabled,MarkEnabled,false);watermarkProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDCANCEL,"Forged locked Apply was accepted.");};editWatermark();}
    require(sameWatermarkSettings(prior,app.settings.watermark)&&lapse::configurationCalls==configuredBefore,"Read-only inspection changed session policy.");
    // Recording or paused: the watermark changes live.
    for(auto state:{State::Recording,State::Paused}){app.status.state=state;skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        require(!draft.readOnly&&markVisible(draft.okay)&&IsWindowEnabled(draft.enabled)&&IsWindowEnabled(draft.position)&&!draft.illustration.pixels.empty(),"Live watermark dialog was locked or lacked preview.");
        markCheck(window,draft.enabled,MarkEnabled,false);watermarkProc(window,WM_COMMAND,IDOK,0);require(outcome()==IDOK,"Live Apply was refused.");};editWatermark();
        require(!app.settings.watermark.enabled&&lapse::configurationCalls>configuredBefore,"Live watermark change did not reach the session.");
        app.settings.watermark=prior;app.watermarkCheckValid=false;configure();}
    app.status={};require(sameWatermarkSettings(prior,app.settings.watermark),"Live watermark edit could not be restored.");
    std::cout<<"PASS cached tiny-output preflight and actual Record guard, engine error precedence, Off and paired/Night compatibility, live edits and starting/finishing locks\n";
}
void nativeNavigationAndLifecycle(){
    HiddenFixture owned;setupWatermark();app.settings.watermark.enabled=true;configure();
    for(int dpi:{96,192,288}){skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);markSelect(window,draft.position,MarkPosition,4);
        RECT proposed{0,0,360,250};watermarkProc(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&proposed));
        for(HWND child:{draft.enabled,draft.time,draft.speed,draft.timeKind,draft.position,draft.size,draft.x,draft.y,draft.okay,draft.cancel}){
            SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(child),TRUE);RECT rect{},client{};GetWindowRect(child,&rect);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&rect),2);GetClientRect(window,&client);
            require(GetFocus()==child && !outcome() && rect.right>0 && rect.left<client.right && rect.bottom>0 && rect.top<client.bottom,"Native focus did not reveal watermark control without activation.");
            if(rect.right-rect.left<=client.right)require(rect.left>=0&&rect.right<=client.right,"Fitted watermark control clipped horizontally.");
            if(rect.bottom-rect.top<=client.bottom)require(rect.top>=0&&rect.bottom<=client.bottom,"Fitted watermark control clipped vertically.");
        }
        MSG escape{};escape.hwnd=window;escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;require(IsDialogMessageW(window,&escape)&&outcome()==IDCANCEL,"Native Escape failed to cancel watermark.");};editWatermark();}
    skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.enabled),TRUE);const bool before=SendMessageW(draft.enabled,BM_GETCHECK,0,0)==BST_CHECKED;
        SendMessageW(draft.enabled,WM_KEYDOWN,VK_SPACE,0);SendMessageW(draft.enabled,WM_KEYUP,VK_SPACE,0);require((SendMessageW(draft.enabled,BM_GETCHECK,0,0)==BST_CHECKED)!=before&&!outcome(),"Native Space failed or activated Apply.");
        SendMessageW(window,WM_NEXTDLGCTL,reinterpret_cast<WPARAM>(draft.okay),TRUE);SendMessageW(draft.okay,BM_CLICK,0,0);require(outcome()==IDOK,"Actual Apply click failed.");};editWatermark();
    const auto prior=app.settings.watermark;skipDialogFailure=true;editWatermark();skipDialogFailure=false;require(sameWatermarkSettings(prior,app.settings.watermark)&&startupMessage.find(L"could not be opened")!=std::wstring::npos,"Dialog creation failure lost settings or actionable error.");
    skipScript=[&](HWND window,DLGPROC,LPARAM){windowProc(app.window,WM_COMMAND,TrayExit,0);require(outcome()==IDCANCEL || !IsWindow(window),"Exit failed to cancel owned watermark modal.");};editWatermark();
    require(!IsWindow(app.window)&&!app.customDialog,"Exit left watermark dialog or owner behind.");
    std::cout<<"PASS native focus/Space/click/Escape at constrained DPIs, dialog creation error and owned-modal Exit\n";
}
void nativeWheelNavigation(){
    HiddenFixture owned;setupWatermark();app.settings.watermark.enabled=true;configure();
    const auto accepted=app.settings.watermark;const int configurations=lapse::configurationCalls;
    const auto wheel=[](HWND target,UINT message,short delta){RECT bounds{};GetWindowRect(target,&bounds);
        SendMessageW(target,message,MAKEWPARAM(0,static_cast<WORD>(delta)),MAKELPARAM(bounds.left+4,bounds.top+4));};
    const auto reach=[&](HWND window,HWND target){
        for(int attempt=0;attempt<64;++attempt){RECT rect{},client{};GetWindowRect(target,&rect);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&rect),2);GetClientRect(window,&client);
            // Coarse wheel steps need not land in the narrow range that fully
            // fits a wide control. Its usable center must be reachable; the
            // keyboard tests separately prove exact full-fit focus reveal.
            const int centerX=(rect.left+rect.right)/2,centerY=(rect.top+rect.bottom)/2;
            if(centerX>=0 && centerX<client.right && centerY>=0 && centerY<client.bottom)return;
            if(centerX<0 || centerX>=client.right)wheel(window,WM_MOUSEHWHEEL,centerX<0?-WHEEL_DELTA:WHEEL_DELTA);
            if(centerY<0 || centerY>=client.bottom)wheel(window,WM_MOUSEWHEEL,centerY<0?WHEEL_DELTA:-WHEEL_DELTA);
        }
        require(false,"Watermark wheel navigation failed to reach a control's usable center.");
    };
    for(int dpi:{96,192,288}){
        skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
            markSelect(window,draft.timeKind,MarkTimeKind,1);markSelect(window,draft.position,MarkPosition,4);markSelect(window,draft.size,MarkSize,2);
            SetWindowTextW(draft.x,L"12.34");SetWindowTextW(draft.y,L"56.78");
            RECT proposed{0,0,360,250};SendMessageW(window,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&proposed));
            SetWindowPos(window,nullptr,0,0,360,250,SWP_NOZORDER|SWP_NOACTIVATE);watermarkLayout(window,draft);
            require((GetWindowLongPtrW(window,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL))==(WS_HSCROLL|WS_VSCROLL),"Watermark wheel fixture lacks two-axis overflow.");
            const auto pixels=draft.illustration.pixels;const auto previousObserved=observedStatus;observedStatus=draft.error;
            SetFocus(draft.x);const auto focused=GetFocus();require(focused==draft.x,"Watermark wheel fixture lost native edit focus.");
            statusWrites=0;
            for(HWND target:{window,draft.help,draft.enabled,draft.x,draft.preview,draft.timeKind,draft.position,draft.size}){
                SendMessageW(window,WM_VSCROLL,SB_TOP,0);const int before=draft.scrollY;
                wheel(target,WM_MOUSEWHEEL,-WHEEL_DELTA);
                require(draft.scrollY>before && GetFocus()==focused && !outcome(),"Native watermark wheel did not scroll the page without moving focus or closing it.");
                require(choice(draft.timeKind)==1 && choice(draft.position)==4 && choice(draft.size)==2 && caption(draft.x)==L"12.34" && caption(draft.y)==L"56.78",
                    "Page wheel changed watermark time, placement, text size or exact coordinate text.");
            }
            reach(window,draft.position);reach(window,draft.x);reach(window,draft.y);reach(window,draft.okay);
            require(statusWrites==0 && draft.illustration.pixels==pixels && GetFocus()==focused,"Scrolling regenerated the illustration, changed its pixels or snapped back to focused content.");
            // Every illustration rebuild writes draft.error through the existing text seam.
            observedStatus=previousObserved;watermarkProc(window,WM_COMMAND,IDCANCEL,0);
        };editWatermark();
        require(sameWatermarkSettings(accepted,app.settings.watermark)&&lapse::configurationCalls==configurations,"Wheel/Cancel committed watermark changes.");
    }
    app.status.state=State::Finishing;
    skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        RECT proposed{0,0,360,250};SendMessageW(window,WM_DPICHANGED,MAKELONG(192,192),reinterpret_cast<LPARAM>(&proposed));
        SetWindowPos(window,nullptr,0,0,360,250,SWP_NOZORDER|SWP_NOACTIVATE);watermarkLayout(window,draft);SendMessageW(window,WM_VSCROLL,SB_TOP,0);
        const auto pixels=draft.illustration.pixels;const int before=draft.scrollY;wheel(draft.help,WM_MOUSEWHEEL,-WHEEL_DELTA);
        require(draft.readOnly && draft.scrollY>before && !IsWindowEnabled(draft.position) && !markVisible(draft.okay),"Read-only watermark inspection did not scroll while preserving recording locks.");
        reach(window,draft.cancel);require(draft.illustration.pixels==pixels&&!outcome(),"Read-only wheel altered illustration or closed inspection.");
        watermarkProc(window,WM_COMMAND,IDCANCEL,0);
    };editWatermark();app.status={};
    require(sameWatermarkSettings(accepted,app.settings.watermark)&&lapse::configurationCalls==configurations,"Read-only wheel changed the recording policy.");
    skipScript=[&](HWND window,DLGPROC,LPARAM parameter){auto& draft=*reinterpret_cast<WatermarkDraft*>(parameter);
        SetWindowPos(window,nullptr,0,0,1200,1600,SWP_NOZORDER|SWP_NOACTIVATE);watermarkLayout(window,draft);
        SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,SB_VERT,&info);require(info.nMax<int(info.nPage),"Native combo control needs a page without vertical overflow.");
        SetFocus(draft.size);choose(draft.size,0);wheel(draft.size,WM_MOUSEWHEEL,-WHEEL_DELTA);
        require(choice(draft.size)==1 && draft.scrollY==0 && !outcome(),"Non-overflow watermark combo lost its native wheel behavior.");
        watermarkProc(window,WM_COMMAND,IDCANCEL,0);
    };editWatermark();
    require(sameWatermarkSettings(accepted,app.settings.watermark)&&lapse::configurationCalls==configurations,"Native combo/Cancel changed accepted watermark settings.");
    std::cout<<"PASS native watermark page wheel across controls and DPIs, stable draft/illustration, coordinate/Apply reachability, read-only scrolling and native non-overflow combo behavior\n";
}
}
int main(){try{draftTransactions();fieldsPositionsAndOff();preflightAndFreeze();nativeNavigationAndLifecycle();nativeWheelNavigation();std::cout<<"All five watermark UI groups passed with owned synthetic fixtures.\n";return 0;}catch(const std::exception& error){std::cerr<<"WATERMARK UI FAILURE: "<<error.what()<<'\n';return 1;}}

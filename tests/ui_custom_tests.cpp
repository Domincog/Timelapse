// Actual custom-dialog callbacks and controls in owned hidden windows. The modal
// invocation is scripted; no user input, device, normal app or settings is used.
#include <windows.h>
#include <functional>
namespace {
std::function<void(HWND,LPARAM)> dialogScript;
INT_PTR dialogOutcome=0;
int dialogCalls=0;
bool failDialog=false;
INT_PTR WINAPI ownedDialog(HINSTANCE,LPCDLGTEMPLATEW,HWND,DLGPROC,LPARAM);
BOOL WINAPI ownedEndDialog(HWND,INT_PTR value){dialogOutcome=value;return TRUE;}
}
#define DialogBoxIndirectParamW ownedDialog
#define EndDialog ownedEndDialog
#define main sourceFixtureMain
#include "ui_source_tests.cpp"
#undef main
#undef EndDialog
#undef DialogBoxIndirectParamW

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
    app.customIntervalMs=5000;app.customWidth=1280;app.customHeight=720;app.customLimitSeconds=900;
    app.committedInterval=2;app.committedSize=app.committedLimit=0;
    choose(app.interval,2);choose(app.videoSize,0);choose(app.stopAfter,0);
    customItems();seed(false);dialogCalls=0;failDialog=false;dialogScript={};
}
void invoke(CustomKind kind){
    HWND box=kind==CustomKind::Interval?app.interval:kind==CustomKind::Size?app.videoSize:app.stopAfter;
    int id=kind==CustomKind::Interval?IntervalBox:kind==CustomKind::Size?SizeBox:StopAfterBox;
    const int action=static_cast<int>(SendMessageW(box,CB_GETCOUNT,0,0))-1;
    choose(box,action);windowProc(app.window,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(box));
}
void accept(HWND window){customProc(window,WM_COMMAND,IDOK,0);}
void intervalAndCancellation(){
    HiddenFixture owned;setupCustom();const Settings original=app.settings;const int calls=lapse::configurationCalls;
    for(auto kind:{CustomKind::Interval,CustomKind::Size,CustomKind::Limit}){
        dialogScript=[](HWND window,LPARAM parameter){auto& draft=*reinterpret_cast<CustomDraft*>(parameter);
            require(GetNextDlgTabItem(window,draft.first,FALSE)==(draft.second?draft.second:draft.units),"Custom dialog tab order skipped its second field.");
            SetWindowTextW(draft.first,L"invalid draft");customProc(window,WM_CLOSE,0,0);};
        invoke(kind);
    }
    require(app.settings.intervalMs==original.intervalMs && app.settings.width==original.width && app.settings.height==original.height &&
        app.settings.recordingLimitSeconds==0 && lapse::configurationCalls==calls && choice(app.interval)==2 && choice(app.videoSize)==0 && choice(app.stopAfter)==0,
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
        for(auto kind:{CustomKind::Interval,CustomKind::Size,CustomKind::Limit})invoke(kind);
        require(!dialogCalls && lapse::configurationCalls==calls && app.settings.intervalMs==5000 && app.settings.width==1280 && app.settings.recordingLimitSeconds==0,
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
        invoke(CustomKind::Interval);require(!app.customDialog,"Destroyed custom dialog retained its HWND.");
    }
    dialogScript=[](HWND,LPARAM){windowProc(app.window,WM_COMMAND,TrayExit,0);};invoke(CustomKind::Interval);
    require(!IsWindow(app.window) && !app.customDialog,"Tray Exit left an owned custom dialog or owner behind.");
    std::cout<<"PASS owned dialog DPI/reflow/scroll reachability, cleanup and explicit tray Exit\n";
}
}
int main(){
    try{intervalAndCancellation();dimensionsAndLimit();activeAndNight();dialogDpiAndLifecycle();
        std::cout<<"All custom UI cases passed with hidden controls and synthetic engine.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

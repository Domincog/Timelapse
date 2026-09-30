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
}
int main(){
    try{intervalAndCancellation();dimensionsAndLimit();activeAndNight();segmentDurations();nativeNumericInsertion();nativeButtonNavigation();dialogDpiAndLifecycle();
        std::cout<<"All custom UI cases passed with hidden controls and synthetic engine.\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

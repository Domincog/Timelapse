// Actual frozen candidate handlers, owned hidden controls and no capture/devices.
// Synthetic app DPI/work areas, thread-local focus/capture, memory-only parent paint.
#include "../src/engine.h"
#include "../src/capture.h"
#include "../src/camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <climits>

namespace lapse {
class FixtureEngine {
public:
    void configure(const Settings&) {}
    void refreshSources() {}
    void record() {}
    void pause() {}
    void setPaused(bool) {}
    void finish() {} void cancelDelayedStart() noexcept {}
    Status status() { return {}; }
};
std::vector<Monitor> enumerateMonitors() { throw std::runtime_error("Unexpected monitor enumeration."); }
std::vector<CameraDevice> enumerateCameras(std::wstring&) { throw std::runtime_error("Unexpected camera enumeration."); }
int runCameraHost(const wchar_t*) { throw std::runtime_error("Unexpected application entry point."); }
}
namespace {
RECT syntheticWork{0,0,10000,10000};
HWND ownedFocus{}, ownedCapture{};
HWND nextDialogFocus{}, droppedCombo{};
int releases=0;
bool releasedDuringDrag=false;
bool invalidatedWholeParent=false;
BOOL WINAPI fixtureInvalidateRect(HWND,const RECT*,BOOL);
HDC paintDC{};
struct TextDraw { std::wstring value; RECT logical, device; };
std::vector<TextDraw> drawnText;
BOOL WINAPI fixtureIsDialogMessage(HWND,LPMSG);
LRESULT WINAPI fixtureDispatchMessage(const MSG*);
LRESULT WINAPI fixtureSendMessage(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==CB_GETDROPPEDSTATE && window==droppedCombo)return TRUE;
    return SendMessageW(window,message,wp,lp);
}
HMONITOR WINAPI fixtureMonitorFromWindow(HWND,DWORD){return reinterpret_cast<HMONITOR>(1);}
HMONITOR WINAPI fixtureMonitorFromRect(LPCRECT,DWORD){return reinterpret_cast<HMONITOR>(1);}
BOOL WINAPI fixtureGetMonitorInfo(HMONITOR,LPMONITORINFO info){info->rcMonitor=info->rcWork=syntheticWork;return TRUE;}
HWND WINAPI fixtureGetFocus(){return ownedFocus;}
HWND WINAPI fixtureSetFocus(HWND window){auto previous=ownedFocus;ownedFocus=window;return previous;}
HWND WINAPI fixtureGetCapture(){return ownedCapture;}
HWND WINAPI fixtureSetCapture(HWND window){auto previous=ownedCapture;ownedCapture=window;return previous;}
BOOL WINAPI fixtureReleaseCapture();
SHORT WINAPI fixtureGetKeyState(int){return 0;}
BOOL WINAPI fixtureSystemParametersInfo(UINT action,UINT parameter,PVOID output,UINT flags){
    if(action==SPI_GETWHEELSCROLLCHARS || action==SPI_GETWHEELSCROLLLINES){*static_cast<UINT*>(output)=3;return TRUE;}
    return SystemParametersInfoW(action,parameter,output,flags);
}
HDC WINAPI fixtureBeginPaint(HWND window,LPPAINTSTRUCT paint){
    if(!paintDC)return BeginPaint(window,paint);
    GetClientRect(window,&paint->rcPaint);paint->hdc=paintDC;return paintDC;
}
BOOL WINAPI fixtureEndPaint(HWND window,const PAINTSTRUCT* paint){return paintDC?TRUE:EndPaint(window,paint);}
int WINAPI fixtureDrawText(HDC dc,LPCWSTR value,int count,LPRECT rect,UINT flags){
    if(dc==paintDC){
        TextDraw draw{count<0?std::wstring(value):std::wstring(value,count),*rect,*rect};
        LPtoDP(dc,reinterpret_cast<POINT*>(&draw.device),2);drawnText.push_back(draw);
    }
    return DrawTextW(dc,value,count,rect,flags);
}
}
#define Engine FixtureEngine
#define MonitorFromWindow fixtureMonitorFromWindow
#define MonitorFromRect fixtureMonitorFromRect
#define GetMonitorInfoW fixtureGetMonitorInfo
#define GetFocus fixtureGetFocus
#define SetFocus fixtureSetFocus
#define GetCapture fixtureGetCapture
#define SetCapture fixtureSetCapture
#define ReleaseCapture fixtureReleaseCapture
#define GetKeyState fixtureGetKeyState
#define SystemParametersInfoW fixtureSystemParametersInfo
#define BeginPaint fixtureBeginPaint
#define EndPaint fixtureEndPaint
#define DrawTextW fixtureDrawText
#define SendMessageW fixtureSendMessage
#define InvalidateRect fixtureInvalidateRect
#define IsDialogMessageW fixtureIsDialogMessage
#define DispatchMessageW fixtureDispatchMessage
// The reject-entry sentinel intentionally makes the GUI entry unreachable.
#pragma warning(push)
#pragma warning(disable: 4702)
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#pragma warning(pop)
#undef Engine
#undef MonitorFromWindow
#undef MonitorFromRect
#undef GetMonitorInfoW
#undef GetFocus
#undef SetFocus
#undef GetCapture
#undef SetCapture
#undef ReleaseCapture
#undef GetKeyState
#undef SystemParametersInfoW
#undef BeginPaint
#undef EndPaint
#undef DrawTextW
#undef SendMessageW
#undef InvalidateRect
#undef IsDialogMessageW
#undef DispatchMessageW

namespace {
void require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
BOOL WINAPI fixtureInvalidateRect(HWND window,const RECT* rect,BOOL erase){
    if(window==app.window && !rect)invalidatedWholeParent=true;
    return InvalidateRect(window,rect,erase);
}
BOOL WINAPI fixtureReleaseCapture(){
    ++releases;releasedDuringDrag|=app.dragging;auto previous=ownedCapture;ownedCapture=nullptr;
    if(previous==app.preview)previewProc(app.preview,WM_CAPTURECHANGED,0,0);return TRUE;
}
LRESULT CALLBACK fixtureWindowProc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    // Creation/destruction must never run preferences, source startup or app shutdown.
    if(msg==WM_CREATE || msg==WM_DESTROY || msg==WM_NCDESTROY)return DefWindowProcW(w,msg,wp,lp);
    return windowProc(w,msg,wp,lp);
}
RECT bounds(HWND child){RECT r{};require(GetWindowRect(child,&r)!=FALSE,"Missing child bounds.");MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&r),2);return r;}
RECT client(){RECT r{};GetClientRect(app.window,&r);return r;}
bool equal(const RECT& a,const RECT& b){return EqualRect(&a,&b)!=FALSE;}
bool equal(const Rect& a,const Rect& b){return std::abs(a.x-b.x)<1e-8 && std::abs(a.y-b.y)<1e-8 && std::abs(a.w-b.w)<1e-8 && std::abs(a.h-b.h)<1e-8;}
bool intersects(const RECT& a,const RECT& b){RECT r{};return IntersectRect(&r,&a,&b)!=FALSE;}
BOOL WINAPI fixtureIsDialogMessage(HWND,LPMSG message){
    if(nextDialogFocus && (message->message==WM_KEYDOWN || message->message==WM_SYSCHAR)){
        ownedFocus=nextDialogFocus;nextDialogFocus=nullptr;return TRUE;
    }
    return FALSE;
}
LRESULT WINAPI fixtureDispatchMessage(const MSG* message){
    if(message->hwnd==app.preview)return previewProc(app.preview,message->message,message->wParam,message->lParam);
    return windowProc(app.window,message->message,message->wParam,message->lParam);
}
std::vector<HWND> tabControls(){
    std::vector<HWND> result={app.mode,app.interval,app.videoSize,app.encodingQuality,app.monitor,app.camera,app.advanced,app.refresh};
    if(app.advancedExpanded){
        result.push_back(app.encodingMode);result.push_back(app.stopAfter);result.push_back(app.lowDisk);result.push_back(app.recoveryMode);result.push_back(app.splitEvery);result.push_back(app.captureCursor);result.push_back(app.startDelay);result.push_back(app.skipConfigure);result.push_back(app.watermarkConfigure);result.push_back(app.playbackConfigure);
        if(nightRow())result.push_back(app.nightEnabled);
        if(nightRow()==2){result.push_back(app.nightDuration);result.push_back(app.nightTarget);}
    }
    for(HWND child:{app.record,app.pause,app.finish,app.folder,app.openFolder,app.reset,app.forward,app.preview})result.push_back(child);
    return result;
}
struct HiddenWindow {
    HiddenWindow(int width,int height,int dpi){
        app.dpi=dpi;app.scrollX=app.scrollY=app.wheelVertical=app.wheelHorizontal=0;
        app.contentWidth=app.contentHeight=0;app.layingOut=app.dragging=app.advancedExpanded=false;app.advancedLimitIndex=app.advancedVisibility=-1;
        app.visibleDirty=true;app.controlsUpdated=false;app.hiddenToTray=false;
        app.skipRevision=0;app.advancedSkipRevision=app.skipSummaryRevision=app.skipVisibility=-1;app.skipSummaryCaption.clear();app.skipDetailCaption.clear();app.skipCheckAge=UINT64_MAX;
        app.settings={};app.status={};app.watermarkCheckValid=false;app.watermarkValidation.clear();app.watermarkCaption.clear();app.watermarkRevision=0;app.advancedWatermarkRevision=-1;app.nightValidation.clear();app.encodingValidation.clear();app.advancedNightState=app.advancedRecoveryState=app.nightVisibility=-1;
        app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;app.advancedCaption.clear();app.advancedTooltip.clear();
        app.hasCustomSegment=false;app.customSegmentSeconds=900;app.committedSegment=0;app.advancedSegmentSeconds=-1;
        app.advancedCursorState=app.advancedDelaySeconds=-1;app.committedStartDelay=0;app.waitingRemaining=UINT64_MAX;app.waitingCaption.clear();
        app.committedInterval=2;app.committedSize=app.committedLimit=0;
        ownedFocus=ownedCapture=nullptr;
        app.window=CreateWindowExW(0,L"STATIC",L"Owned hidden scrolling fixture",WS_POPUP|WS_CLIPCHILDREN,0,0,width,height,nullptr,nullptr,nullptr,nullptr);
        require(app.window && !IsWindowVisible(app.window),"Hidden parent creation failed.");
        auto child=[&](const wchar_t* cls,const wchar_t* name,DWORD style,int id){auto w=control(cls,name,style,id);require(w!=nullptr,"Child creation failed.");return w;};
        auto combo=[&](int label,const wchar_t* caption,int id){app.labels[label]=child(L"STATIC",caption,0,200+label);auto w=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,id);add(w,L"First");add(w,L"Second");choose(w,0);return w;};
        app.mode=combo(0,L"&Source",ModeBox);app.interval=combo(1,L"Capture &every",IntervalBox);
        app.videoSize=combo(2,L"Video si&ze",SizeBox);app.encodingQuality=combo(3,L"Video &quality",EncodingQualityBox);
        app.monitor=combo(4,L"&Display",MonitorBox);app.camera=combo(5,L"Ca&mera",CameraBox);
        auto button=[&](const wchar_t* name,int id){return child(L"BUTTON",name,WS_TABSTOP|BS_PUSHBUTTON,id);};
        app.advanced=child(L"BUTTON",L"&Advanced",WS_TABSTOP|BS_AUTOCHECKBOX|BS_PUSHLIKE,AdvancedToggle);app.refresh=button(L"Re&fresh",Refresh);
        app.encodingMode=combo(6,L"Encodin&g",EncodingModeBox);app.stopAfter=combo(7,L"S&top after",StopAfterBox);
        SendMessageW(app.encodingMode,CB_RESETCONTENT,0,0);for(auto name:EncodingModeLabels)add(app.encodingMode,name);choose(app.encodingMode,0);
        SendMessageW(app.stopAfter,CB_RESETCONTENT,0,0);for(auto name:RecordingLimitLabels)add(app.stopAfter,name);choose(app.stopAfter,0);
        app.lowDisk=child(L"BUTTON",L"Stop on &low disk space",WS_TABSTOP|BS_AUTOCHECKBOX,LowDiskBox);SendMessageW(app.lowDisk,BM_SETCHECK,BST_CHECKED,0);
        app.recoveryMode=child(L"BUTTON",L"MP4 recover&y mode (H.264)",WS_TABSTOP|BS_AUTOCHECKBOX,RecoveryBox);
        app.segmentLabel=child(L"STATIC",L"Split files e&very",0,211);app.splitEvery=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,SegmentBox);
        for(auto name:SegmentLabels)add(app.splitEvery,name);choose(app.splitEvery,0);
        app.captureCursor=child(L"BUTTON",L"Show des&ktop cursor",WS_TABSTOP|BS_AUTOCHECKBOX,CursorBox);SendMessageW(app.captureCursor,BM_SETCHECK,BST_CHECKED,0);
        app.startDelayLabel=child(L"STATIC",L"Delay ne&xt recording",0,212);
        app.startDelay=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,StartDelayBox);
        for(auto label:StartDelayLabels)add(app.startDelay,label);choose(app.startDelay,0);
        app.startDelayHint=child(L"STATIC",L"Preparation starts after the delay. Visible preview continues; Stop after counts active time.",SS_LEFT|SS_NOPREFIX,213);
        app.skipConfigure=button(L"Time &compression...",SkipConfigure);app.skipSummary=child(L"STATIC",L"Off",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,SkipSummary);app.skipDetail=child(L"STATIC",L"",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,SkipDetail);
        app.watermarkConfigure=button(L"&Watermark...",WatermarkConfigure);app.watermarkSummary=child(L"STATIC",L"Off",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,WatermarkSummary);
        app.playbackConfigure=button(L"Playback && shortcuts...",PlaybackConfigure);
        app.nightEnabled=child(L"BUTTON",L"&Night camera (software blend)",WS_TABSTOP|BS_AUTOCHECKBOX,NightBox);
        app.nightDuration=combo(8,L"Blend d&uration",NightDurationBox);SendMessageW(app.nightDuration,CB_RESETCONTENT,0,0);for(auto name:NightDurationLabels)add(app.nightDuration,name);choose(app.nightDuration,0);
        app.nightTarget=combo(9,L"Auto &brightness",NightTargetBox);SendMessageW(app.nightTarget,CB_RESETCONTENT,0,0);for(auto name:{L"Dark",L"Balanced",L"Bright"})add(app.nightTarget,name);choose(app.nightTarget,1);
        app.nightHint=child(L"STATIC",L"",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,NightHint);app.nightDetail=child(L"STATIC",L"",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,NightDetail);
        app.record=button(L"&Record",Record);app.pause=button(L"&Pause",Pause);app.finish=button(L"&Finish",Finish);
        app.folder=button(L"C&hange...",Folder);app.openFolder=button(L"&Open folder",OpenFolder);app.reset=button(L"Reset layout",Reset);app.forward=button(L"Bring forward",Forward);
        app.preview=child(L"STATIC",L"Preview",WS_TABSTOP,Preview);app.statusText=child(L"STATIC",L"Ready",SS_LEFT|SS_CENTERIMAGE,210);
        SetWindowLongPtrW(app.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(fixtureWindowProc));
        app.engine=std::make_unique<FixtureEngine>();
        fonts();layout();
    }
    ~HiddenWindow(){
        ownedFocus=ownedCapture=nullptr;SetWindowLongPtrW(app.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(DefWindowProcW));
        DestroyWindow(app.window);app.window=app.preview=app.captureCursor=app.startDelay=app.startDelayLabel=app.startDelayHint=app.playbackConfigure=nullptr;app.engine.reset();
    }
};
void checkLayout(){
    const auto view=client();
    require(app.contentWidth>=app.scale(830) && app.contentHeight>=app.scale(541),"Logical minimum lost.");
    require(app.scrollX>=0 && app.scrollX<=app.contentWidth-view.right && app.scrollY>=0 && app.scrollY<=app.contentHeight-view.bottom,"Offsets exceed viewport range.");
    require(!intersects(bounds(app.forward),bounds(app.folder)),"Bring forward overlaps Change folder.");
    require(!intersects(bounds(app.finish),bounds(app.openFolder)),"Finish overlaps Open folder.");
    require(!intersects(bounds(app.monitor),bounds(app.camera)) && !intersects(bounds(app.camera),bounds(app.advanced)) &&
            !intersects(bounds(app.advanced),bounds(app.refresh)),"Source or Advanced controls overlap.");
    if(app.advancedExpanded)require(bounds(app.watermarkConfigure).bottom<=bounds(app.preview).top && !intersects(bounds(app.watermarkConfigure),bounds(app.skipConfigure)) && !intersects(bounds(app.watermarkConfigure),bounds(app.watermarkSummary)),"Watermark controls overlap the other options or preview.");
    if(app.advancedExpanded){
        require(bounds(app.playbackConfigure).top==bounds(app.watermarkConfigure).top &&
            bounds(app.playbackConfigure).bottom<=bounds(app.preview).top &&
            !intersects(bounds(app.playbackConfigure),bounds(app.watermarkConfigure)) &&
            !intersects(bounds(app.playbackConfigure),bounds(app.watermarkSummary)),"Playback disclosure adds height or overlaps the watermark row.");
        wchar_t raw[100]{};GetWindowTextW(app.playbackConfigure,raw,100);std::wstring caption=raw;
        const auto escapedAmpersand=caption.find(L"&&");if(escapedAmpersand!=std::wstring::npos)caption.erase(escapedAmpersand,1);
        HDC dc=GetDC(app.playbackConfigure);auto previous=SelectObject(dc,app.font);SIZE extent{};
        GetTextExtentPoint32W(dc,caption.c_str(),static_cast<int>(caption.size()),&extent);SelectObject(dc,previous);ReleaseDC(app.playbackConfigure,dc);
        require(extent.cx+app.scale(18)<=bounds(app.playbackConfigure).right-bounds(app.playbackConfigure).left,"Playback disclosure caption truncates at this DPI.");
    }
    if(app.advancedExpanded)require(!intersects(bounds(app.encodingMode),bounds(app.stopAfter))&&!intersects(bounds(app.stopAfter),bounds(app.lowDisk))&&bounds(app.lowDisk).bottom<bounds(app.preview).top,"Advanced options overlap each other or preview.");
    HDC textDc=GetDC(app.mode);auto oldFont=SelectObject(textDc,app.font);SIZE labelSize{};
    GetTextExtentPoint32W(textDc,SeparateFilesLabel,static_cast<int>(std::wcslen(SeparateFilesLabel)),&labelSize);
    SelectObject(textDc,oldFont);ReleaseDC(app.mode,textDc);
    const RECT modeBounds=bounds(app.mode);
    require(labelSize.cx+GetSystemMetricsForDpi(SM_CXVSCROLL,app.dpi)+app.scale(12)<=modeBounds.right-modeBounds.left,"Separate-files label truncates in the selected source control.");
    textDc=GetDC(app.lowDisk);oldFont=SelectObject(textDc,app.font);
    const wchar_t diskLabel[]=L"Stop on low disk space";GetTextExtentPoint32W(textDc,diskLabel,_countof(diskLabel)-1,&labelSize);
    SelectObject(textDc,oldFont);ReleaseDC(app.lowDisk,textDc);
    require(labelSize.cx+app.scale(26)<=bounds(app.lowDisk).right-bounds(app.lowDisk).left,"Low disk checkbox label truncates at minimum layout width.");
    const auto preview=bounds(app.preview);
    require(preview.bottom-preview.top>=app.scale(160),"Preview is unusably short.");
    RECT local{};GetClientRect(app.preview,&local);require(equal(app.videoRect,previewVideoRect(local)),"Hit-test geometry was left waiting for paint.");
    auto all=tabControls();all.push_back(app.statusText);for(auto label:app.labels)all.push_back(label);
    for(auto w:all){const auto r=bounds(w);require(r.left+app.scrollX>=0 && r.right+app.scrollX<=app.contentWidth && r.top+app.scrollY>=0 && r.bottom+app.scrollY<=app.contentHeight,"Child escaped the logical canvas.");}
    require(!IsWindowVisible(app.window),"Fixture became visible.");
}
void checkFocusReachability(){
    for(auto control:tabControls()){
        ownedFocus=control;revealFocusedControl();const auto r=bounds(control),view=client();
        // A wide preview may span pages; every smaller control must fit entirely.
        if(r.right-r.left<=view.right)require(r.left>=0 && r.right<=view.right,"Focused control remains horizontally clipped.");
        if(r.bottom-r.top<=view.bottom)require(r.top>=0 && r.bottom<=view.bottom,"Focused control remains vertically clipped.");
    }
    ownedFocus=nullptr;
}
void paintCheck(){
    paintDC=CreateCompatibleDC(nullptr);require(paintDC!=nullptr,"Memory paint DC failed.");
    HBITMAP bitmap=CreateBitmap(1,1,1,32,nullptr);auto old=SelectObject(paintDC,bitmap);drawnText.clear();
    windowProc(app.window,WM_PAINT,0,0);
    require(drawnText.size()==5,"Unexpected parent painted-text count.");
    for(const auto& draw:drawnText){RECT expected=draw.logical;OffsetRect(&expected,-app.scrollX,-app.scrollY);require(equal(expected,draw.device),"Parent paint origin does not follow scrolling.");}
    require(drawnText.back().logical.top==app.contentHeight-app.scale(92),"Painted save path uses viewport height.");
    require(drawnText.back().device.top==bounds(app.folder).top,"Painted save path detached from Change button.");
    SelectObject(paintDC,old);DeleteObject(bitmap);DeleteDC(paintDC);paintDC=nullptr;
}
void scenario(int dpi,bool constrained){
    const int width=constrained?MulDiv(500,dpi,96):MulDiv(1000,dpi,96), height=constrained?MulDiv(400,dpi,96):MulDiv(740,dpi,96);
    HiddenWindow owned(width,height,dpi);checkLayout();
    const auto style=GetWindowLongPtrW(app.window,GWL_STYLE);
    require(((style&WS_HSCROLL)!=0)==constrained && ((style&WS_VSCROLL)!=0)==constrained,"Wrong scrollbar state.");
    if(!constrained){require(app.scrollX==0 && app.scrollY==0 && app.contentWidth==width && app.contentHeight==height,"Roomy layout changed its client canvas.");}
    checkFocusReachability();
    scrollTo(INT_MAX,INT_MAX);checkLayout();paintCheck();
    scrollBar(SB_VERT,SB_TOP);scrollBar(SB_HORZ,SB_TOP);
    require(app.scrollX==0 && app.scrollY==0,"Top command did not reset offsets.");
    if(constrained){
        scrollBar(SB_VERT,SB_PAGEDOWN);require(app.scrollY>0,"Vertical page command did not scroll.");
        scrollBar(SB_HORZ,SB_LINEDOWN);require(app.scrollX>0,"Horizontal line command did not scroll.");
        scrollTo(0,0);MSG wheel{};wheel.hwnd=app.encodingQuality;wheel.message=WM_MOUSEWHEEL;wheel.wParam=MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA));
        require(scrollWheelMessage(wheel) && app.scrollY>0 && choice(app.encodingQuality)==0,"Closed quality combo wheel changed settings or failed to scroll.");
        wheel.wParam=MAKEWPARAM(MK_SHIFT,static_cast<WORD>(-WHEEL_DELTA));require(scrollWheelMessage(wheel) && app.scrollX>0,"Shift wheel did not scroll horizontally.");
        droppedCombo=app.encodingQuality;require(!scrollWheelMessage(wheel),"An open combo lost wheel ownership.");droppedCombo=nullptr;
        scrollTo(0,0);wheel.message=WM_MOUSEHWHEEL;wheel.wParam=MAKEWPARAM(0,WHEEL_DELTA);require(scrollWheelMessage(wheel) && app.scrollX>0,"Horizontal wheel did not scroll right.");
        const int before=app.scrollX;wheel.wParam=MAKEWPARAM(MK_CONTROL,WHEEL_DELTA);require(!scrollWheelMessage(wheel) && app.scrollX==before,"Ctrl wheel was intercepted.");
        scrollTo(INT_MAX,INT_MAX);SetWindowPos(app.window,nullptr,0,0,MulDiv(1000,dpi,96),MulDiv(740,dpi,96),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);checkLayout();
        require(app.scrollX==0 && app.scrollY==0 && !(GetWindowLongPtrW(app.window,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL)),"Roomy resize left scroll offsets or bars.");
    }
    std::cout<<"PASS layout dpi="<<dpi<<" client_input="<<width<<'x'<<height<<" mode="<<(constrained?"constrained":"roomy")<<" real_window_dpi="<<GetDpiForWindow(app.window)<<'\n';
}
void advancedDisclosure(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    auto styledVisible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    require(!app.advancedExpanded&&!styledVisible(app.encodingMode)&&!styledVisible(app.stopAfter)&&!styledVisible(app.lowDisk)&&SendMessageW(app.advanced,BM_GETCHECK,0,0)==BST_UNCHECKED,"Advanced options were not collapsed by default.");
    const auto collapsedHeight=app.contentHeight;
    for(int i=0;i<6;++i){
        choose(app.stopAfter,i);updateAdvanced();wchar_t label[128]{};GetWindowTextW(app.advanced,label,128);
        require(i==0 || std::wstring(label).find(RecordingLimitLabels[i])!=std::wstring::npos,"Collapsed disclosure hides a configured time limit.");
        std::wstring measured=label;measured.erase(std::remove(measured.begin(),measured.end(),L'&'),measured.end());
        HDC dc=GetDC(app.advanced);auto old=SelectObject(dc,app.font);SIZE extent{};GetTextExtentPoint32W(dc,measured.c_str(),static_cast<int>(measured.size()),&extent);
        SelectObject(dc,old);ReleaseDC(app.advanced,dc);
        require(extent.cx+app.scale(18)<=bounds(app.advanced).right-bounds(app.advanced).left,"Finite-limit disclosure caption truncates at minimum layout width.");
    }
    ownedFocus=app.advanced;windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(app.advancedExpanded&&styledVisible(app.encodingMode)&&styledVisible(app.stopAfter)&&styledVisible(app.lowDisk)&&SendMessageW(app.advanced,BM_GETCHECK,0,0)==BST_CHECKED,"Disclosure did not expose accessible checked state/options.");
    require(app.contentHeight==collapsedHeight+app.scale(280),"Expanded options failed to claim their own layout rows.");
    require(GetNextDlgTabItem(app.window,app.refresh,FALSE)==app.encodingMode&&GetNextDlgTabItem(app.window,app.encodingMode,FALSE)==app.stopAfter&&GetNextDlgTabItem(app.window,app.stopAfter,FALSE)==app.lowDisk,"Expanded native tab order skipped advanced options.");
    checkLayout();checkFocusReachability();scrollTo(INT_MAX,INT_MAX);ownedFocus=app.lowDisk;
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(!app.advancedExpanded&&ownedFocus==app.advanced&&!styledVisible(app.labels[6])&&!styledVisible(app.labels[7]),"Collapsing stranded keyboard focus or labels.");
    require(app.contentHeight==collapsedHeight&&GetNextDlgTabItem(app.window,app.refresh,FALSE)==app.record,"Collapsed row retained blank height or hidden tab stops.");
    checkLayout();checkFocusReachability();
    app.status.state=State::Recording;updateControls();windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(app.advancedExpanded&&IsWindowEnabled(app.advanced)&&!IsWindowEnabled(app.stopAfter)&&!IsWindowEnabled(app.encodingMode)&&!IsWindowEnabled(app.lowDisk),"Recording froze disclosure or allowed advanced edits.");
    app.status={};updateControls();
    std::cout<<"PASS Advanced disclosure dpi="<<dpi<<" default, finite summaries, expansion, focus transfer, native tab order, scroll clamp, active lock\n";
}
void compressionDisclosure(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    const int collapsed=app.contentHeight;require(!visible(app.skipConfigure)&&!visible(app.skipSummary)&&!visible(app.skipDetail),"Collapsed compression controls visible.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);const int off=app.contentHeight;
    require(visible(app.skipConfigure)&&visible(app.skipSummary)&&!visible(app.skipDetail),"Off compression row visibility incorrect.");
    require(GetNextDlgTabItem(app.window,app.lowDisk,FALSE)==app.recoveryMode&&GetNextDlgTabItem(app.window,app.recoveryMode,FALSE)==app.splitEvery&&GetNextDlgTabItem(app.window,app.splitEvery,FALSE)==app.captureCursor&&GetNextDlgTabItem(app.window,app.captureCursor,FALSE)==app.startDelay&&GetNextDlgTabItem(app.window,app.startDelay,FALSE)==app.skipConfigure,"Recovery/split/cursor/delay/compression controls not in native Advanced tab order.");
    app.settings.timeSkip.mode=TimeSkipMode::Quiet;app.settings.timeSkip.multiplier=64;++app.skipRevision;updateControls();layout();
    require(app.contentHeight==off+app.scale(28) && visible(app.skipDetail) && bounds(app.skipDetail).bottom<bounds(app.preview).top,"Enabled compression detail overlaps preview or has wrong height.");
    const unsigned inspections=lapse::uiPersonPackInspections;
    app.settings.timeSkip.mode=TimeSkipMode::NoPersonWithinSchedule;++app.skipRevision;updateControls();layout();
    require(app.contentHeight==off+app.scale(28) && app.skipDetailCaption.find(L"Select camera")!=std::wstring::npos && lapse::uiPersonPackInspections==inspections,
        "Person policy changed base geometry, hid camera scope or inspected its pack from a main refresh.");
    paintCheck();require(drawnText[3].value.find(L"Base interval")!=std::wstring::npos && drawnText[3].value.find(L"1 hour becomes")==std::wstring::npos,"Enabled compression promised a fixed resulting video duration.");
    for(int limit=0;limit<6;++limit){choose(app.stopAfter,limit);updateAdvanced();wchar_t value[200]{};GetWindowTextW(app.advanced,value,200);
        std::wstring measured=value;measured.erase(std::remove(measured.begin(),measured.end(),L'&'),measured.end());HDC dc=GetDC(app.advanced);auto prior=SelectObject(dc,app.font);SIZE size{};
        GetTextExtentPoint32W(dc,measured.c_str(),static_cast<int>(measured.size()),&size);SelectObject(dc,prior);ReleaseDC(app.advanced,dc);
        require(size.cx+app.scale(18)<=bounds(app.advanced).right-bounds(app.advanced).left,"Compression/finite-stop disclosure truncates.");
        require(measured.find(L"64")!=std::wstring::npos && (!limit || measured.find(L"stop")!=std::wstring::npos || measured.find(RecordingLimitShortLabels[limit])!=std::wstring::npos),"Collapsed compression hides its configured multiplier or finite stop.");}
    ownedFocus=app.skipConfigure;windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(app.contentHeight==collapsed && ownedFocus==app.advanced && !visible(app.skipConfigure),"Collapse stranded compression focus or retained height.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);app.status.state=State::Recording;updateControls();require(IsWindowEnabled(app.skipConfigure),"Active policy inspection was disabled.");
    checkLayout();checkFocusReachability();std::cout<<"PASS compression disclosure dpi="<<dpi<<" visibility, exact height, tab/focus, finite summaries and active inspection\n";
}
void watermarkDisclosure(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    const int collapsed=app.contentHeight;require(!visible(app.watermarkConfigure)&&!visible(app.watermarkSummary),"Collapsed watermark row was visible.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);require(visible(app.watermarkConfigure)&&visible(app.watermarkSummary)&&GetNextDlgTabItem(app.window,app.skipConfigure,FALSE)==app.watermarkConfigure,"Watermark Advanced row lost visibility/native tab order.");
    for(bool skip:{false,true})for(bool night:{false,true}){
        app.settings.timeSkip.mode=skip?TimeSkipMode::Quiet:TimeSkipMode::Off;++app.skipRevision;
        choose(app.mode,night?1:0);app.settings.layers=preset(night?Mode::Camera:Mode::Desktop);SendMessageW(app.nightEnabled,BM_SETCHECK,night?BST_CHECKED:BST_UNCHECKED,0);
        app.settings.watermark.enabled=true;configure();updateControls();layout();
        require(bounds(app.watermarkConfigure).top>bounds(skip?app.skipDetail:app.skipConfigure).bottom &&
            (!night || bounds(app.watermarkConfigure).bottom<bounds(app.nightEnabled).top),"Watermark row overlaps conditional compression/Night content.");
        checkLayout();
        auto value=app.advancedCaption;value.erase(std::remove(value.begin(),value.end(),L'&'),value.end());SIZE extent{};HDC dc=GetDC(app.advanced);auto previous=SelectObject(dc,app.font);GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&extent);SelectObject(dc,previous);ReleaseDC(app.advanced,dc);
        require(value.find(L"watermark")!=std::wstring::npos&&extent.cx+app.scale(18)<=bounds(app.advanced).right-bounds(app.advanced).left,"Enabled watermark vanished/truncated in collapsed summary.");
    }
    checkFocusReachability();
    ownedFocus=app.watermarkConfigure;windowProc(app.window,WM_COMMAND,AdvancedToggle,0);require(!visible(app.watermarkConfigure)&&ownedFocus==app.advanced&&app.contentHeight==collapsed,"Collapse retained watermark height or stranded focus.");
    app.status.state=State::Recording;updateControls();windowProc(app.window,WM_COMMAND,AdvancedToggle,0);require(IsWindowEnabled(app.watermarkConfigure),"Active watermark inspection was disabled.");
    std::cout<<"PASS watermark disclosure dpi="<<dpi<<" compact base, conditional row spacing, caption, focus and active inspection\n";
}
void recoveryDisclosure(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    const auto fits=[](HWND child,int padding){
        wchar_t label[256]{};GetWindowTextW(child,label,256);std::wstring measured=label;measured.erase(std::remove(measured.begin(),measured.end(),L'&'),measured.end());
        HDC dc=GetDC(child);auto previous=SelectObject(dc,app.font);SIZE size{};GetTextExtentPoint32W(dc,measured.c_str(),static_cast<int>(measured.size()),&size);
        SelectObject(dc,previous);ReleaseDC(child,dc);require(size.cx+app.scale(padding)<=bounds(child).right-bounds(child).left,"Recovery summary or checkbox text truncates.");
    };
    const auto collapsed=app.contentHeight;
    require(!visible(app.recoveryMode)&&SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==BST_UNCHECKED,"Recovery was not hidden and off by default.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);fits(app.recoveryMode,26);
    require(visible(app.recoveryMode)&&bounds(app.lowDisk).bottom<bounds(app.recoveryMode).top&&bounds(app.recoveryMode).bottom<bounds(app.skipConfigure).top,
        "Recovery row overlaps its adjacent controls.");
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,RecoveryBox,0);
    ownedFocus=app.recoveryMode;windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(app.contentHeight==collapsed&&!visible(app.recoveryMode)&&ownedFocus==app.advanced&&app.advancedCaption.find(L"recovery")!=std::wstring::npos,
        "Recovery changed the collapsed height, hid its opt-in summary or stranded focus.");fits(app.advanced,18);
    app.settings.night.enabled=true;app.settings.timeSkip.mode=TimeSkipMode::Quiet;app.settings.timeSkip.multiplier=64;++app.skipRevision;
    for(int limit=0;limit<6;++limit){choose(app.stopAfter,limit);updateAdvanced();fits(app.advanced,18);
        require(app.advancedCaption.find(L"recovery")!=std::wstring::npos&&app.advancedTooltip.find(L"64")!=std::wstring::npos&&(!limit||app.advancedTooltip.find(L"Stop after")!=std::wstring::npos),
            "Mixed Advanced summary/tooltip lost recovery or the enabled compression/stop policy.");}
    choose(app.encodingMode,3);windowProc(app.window,WM_COMMAND,MAKEWPARAM(EncodingModeBox,CBN_SELCHANGE),0);fits(app.advanced,18);
    require(app.advancedCaption.find(L"check MP4")!=std::wstring::npos&&!IsWindowEnabled(app.record)&&app.advancedTooltip.find(L"requires H.264")!=std::wstring::npos,
        "Collapsed HEVC/recovery incompatibility is not discoverable.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);app.status.state=State::Recording;updateControls();
    require(IsWindowEnabled(app.advanced)&&!IsWindowEnabled(app.recoveryMode),"Active recovery was editable or prevented disclosure.");
    app.status={};updateControls();checkLayout();checkFocusReachability();
    std::cout<<"PASS recovery disclosure dpi="<<dpi<<" default, row/text bounds, compact mixed summaries, HEVC validation, focus/tab access and active lock\n";
}
void nightDisclosure(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    app.monitors={{L"Synthetic display",{0,0,640,360},L"owned-display"}};app.cameras={{L"Synthetic camera",L"owned-camera"}};
    const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    const auto measure=[](HWND child,int padding){
        wchar_t raw[250]{};GetWindowTextW(child,raw,250);std::wstring value=raw;value.erase(std::remove(value.begin(),value.end(),L'&'),value.end());
        HDC dc=GetDC(child);auto prior=SelectObject(dc,app.font);SIZE extent{};GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&extent);SelectObject(dc,prior);ReleaseDC(child,dc);
        require(extent.cx+app.scale(padding)<=bounds(child).right-bounds(child).left,"Night control text truncates at minimum width/DPI.");
    };
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);require(!visible(app.nightEnabled)&&!visible(app.nightDuration),"Desktop exposes irrelevant camera night controls.");
    choose(app.mode,static_cast<int>(Mode::Camera));changeLayout(false);
    require(visible(app.nightEnabled)&&!visible(app.nightDuration)&&!visible(app.nightTarget)&&GetNextDlgTabItem(app.window,app.recoveryMode,FALSE)==app.splitEvery&&GetNextDlgTabItem(app.window,app.splitEvery,FALSE)==app.startDelay&&GetNextDlgTabItem(app.window,app.startDelay,FALSE)==app.skipConfigure&&GetNextDlgTabItem(app.window,app.skipConfigure,FALSE)==app.watermarkConfigure&&GetNextDlgTabItem(app.window,app.watermarkConfigure,FALSE)==app.playbackConfigure&&GetNextDlgTabItem(app.window,app.playbackConfigure,FALSE)==app.nightEnabled,"Camera night opt-in visibility/tab order failed.");
    const auto offHeight=app.contentHeight;
    ownedFocus=app.nightEnabled;SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,NightBox,0);
    require(visible(app.nightDuration)&&visible(app.nightTarget)&&visible(app.nightHint)&&visible(app.nightDetail)&&app.contentHeight==offHeight+app.scale(72),"Night options did not claim exactly their detail row space.");
    require(GetNextDlgTabItem(app.window,app.nightEnabled,FALSE)==app.nightDuration&&GetNextDlgTabItem(app.window,app.nightDuration,FALSE)==app.nightTarget,"Night duration/brightness native tab order failed.");
    measure(app.nightEnabled,26);measure(app.labels[8],0);measure(app.labels[9],0);measure(app.nightTarget,30);
    require(!intersects(bounds(app.nightEnabled),bounds(app.nightDuration))&&!intersects(bounds(app.nightDuration),bounds(app.nightTarget))&&bounds(app.nightDetail).bottom<bounds(app.preview).top,"Night controls overlap or touch preview.");
    for(int i=0;i<6;++i){choose(app.stopAfter,i);updateAdvanced();measure(app.advanced,18);}
    checkLayout();checkFocusReachability();ownedFocus=app.nightTarget;windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(ownedFocus==app.advanced&&!visible(app.nightEnabled)&&!visible(app.nightDuration)&&GetNextDlgTabItem(app.window,app.refresh,FALSE)==app.record,"Collapsing stranded night focus or tab stops.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);choose(app.nightDuration,5);windowProc(app.window,WM_COMMAND,MAKEWPARAM(NightDurationBox,CBN_SELCHANGE),0);measure(app.advanced,18);
    require(!app.nightValidation.empty()&&!IsWindowEnabled(app.record),"Invalid night duration lacks compact validation.");
    ownedFocus=app.nightTarget;SendMessageW(app.nightEnabled,BM_SETCHECK,BST_UNCHECKED,0);windowProc(app.window,WM_COMMAND,NightBox,0);
    require(ownedFocus==app.nightEnabled&&!visible(app.nightTarget)&&app.nightValidation.empty(),"Turning night off stranded focus or retained irrelevant validation.");
    ownedFocus=app.nightEnabled;choose(app.mode,static_cast<int>(Mode::Desktop));changeLayout(false);
    require(ownedFocus==app.advanced&&!visible(app.nightEnabled),"Changing to Desktop stranded hidden night checkbox focus.");
    checkLayout();std::cout<<"PASS night disclosure dpi="<<dpi<<" camera-only visibility, compact captions, focus/tab order, native text extents, validation and scroll layout\n";
}
POINT beginDrag(){
    app.settings.layers={{Source::Desktop,{0,0,1,1}},{Source::Camera,{.2,.3,.3,.3}}};app.selected=-1;
    const auto r=layerRect(app.settings.layers[1]);POINT p{(r.left+r.right)/2,(r.top+r.bottom)/2};
    previewProc(app.preview,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(p.x,p.y));
    require(app.dragging && ownedCapture==app.preview,"Preview drag did not start.");
    previewProc(app.preview,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(p.x+4,p.y+4));return p;
}
void dragChecks(){
    HiddenWindow owned(500,400,96);
    for(int operation=0;operation<3;++operation){
        ownedFocus=nullptr;scrollTo(0,0);const auto p=beginDrag();ownedFocus=nullptr;const auto edit=app.settings.layers[1].rect;
        releases=0;releasedDuringDrag=false;
        if(operation==0)scrollTo(40,40);
        else if(operation==1)SetWindowPos(app.window,nullptr,0,0,520,420,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        else {RECT suggested{0,0,700,550};windowProc(app.window,WM_DPICHANGED,MAKELONG(144,144),reinterpret_cast<LPARAM>(&suggested));}
        require(!app.dragging && !ownedCapture && releases==1 && !releasedDuringDrag,"Origin change did not end owned drag first.");
        previewProc(app.preview,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(p.x+80,p.y+80));
        require(equal(edit,app.settings.layers[1].rect),"A later pointer move reused a stale drag origin.");checkLayout();
    }
    beginDrag();ownedCapture=app.window;scrollTo(0,0);layout();require(ownedCapture==app.window && !app.dragging,"Layout released another window's capture.");ownedCapture=nullptr;
    std::cout<<"PASS drag scroll/resize/DPI cancellation preserves last edit and foreign capture\n";
}
void barDependency(){
    HiddenWindow owned(835,530,96);require((GetWindowLongPtrW(app.window,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL))==(WS_HSCROLL|WS_VSCROLL),"Vertical bar did not induce horizontal bar.");
    SetWindowPos(app.window,nullptr,0,0,820,546,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);require((GetWindowLongPtrW(app.window,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL))==(WS_HSCROLL|WS_VSCROLL),"Horizontal bar did not induce vertical bar.");
    checkLayout();std::cout<<"PASS mutually dependent native scrollbars\n";
}
void dpiAndRouting(){
    HiddenWindow owned(500,400,96);
    scrollTo(60,70);RECT suggestion{0,0,1000,800};
    windowProc(app.window,WM_DPICHANGED,MAKELONG(192,192),reinterpret_cast<LPARAM>(&suggestion));
    require(app.scrollX==120 && app.scrollY==140,"DPI transition did not scale existing offsets.");checkLayout();
    scrollTo(INT_MAX,INT_MAX);suggestion={0,0,1000,740};
    windowProc(app.window,WM_DPICHANGED,MAKELONG(96,96),reinterpret_cast<LPARAM>(&suggestion));
    require(app.scrollX==0 && app.scrollY==0,"DPI transition did not reset offsets when content fit.");
    SetWindowPos(app.window,nullptr,0,0,500,400,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    scrollTo(0,0);ownedFocus=app.mode;nextDialogFocus=app.openFolder;
    MSG message{};message.hwnd=app.mode;message.message=WM_KEYDOWN;message.wParam=VK_TAB;
    dispatchAppMessage(app.window,message);
    const auto target=bounds(app.openFolder),view=client();
    require(target.left>=0 && target.right<=view.right && target.top>=0 && target.bottom<=view.bottom,"Actual loop did not reveal new Tab focus.");
    ownedFocus=app.openFolder;nextDialogFocus=app.encodingQuality;message.message=WM_SYSCHAR;message.wParam=L'q';dispatchAppMessage(app.window,message);
    require(bounds(app.encodingQuality).top>=0,"Actual loop did not reveal mnemonic focus.");
    scrollTo(40,50);const int oldX=app.scrollX,oldY=app.scrollY;
    app.settings.layers={{Source::Desktop,{0,0,1,1}},{Source::Camera,{.2,.3,.3,.3}}};
    const auto layer=layerRect(app.settings.layers[1]);message.hwnd=app.preview;message.message=WM_LBUTTONDOWN;message.wParam=MK_LBUTTON;message.lParam=MAKELPARAM((layer.left+layer.right)/2,(layer.top+layer.bottom)/2);
    dispatchAppMessage(app.window,message);
    require(app.dragging && app.scrollX==oldX && app.scrollY==oldY,"Mouse focus reveal moved the preview after drag start.");
    endLayoutDrag();ownedFocus=nullptr;
    windowProc(app.window,WM_TIMER,1,0);invalidatedWholeParent=false;windowProc(app.window,WM_TIMER,1,0);
    require(!invalidatedWholeParent,"Unchanged status invalidated the parent.");
    std::cout<<"PASS DPI offset scaling/reset, exact keyboard/mouse loop routing and timer invalidation\n";
}
void originalScenarios(){
    {
        HiddenWindow owned(904,701,96);
        require(equal(bounds(app.forward),RECT{758,520,878,547}) && equal(bounds(app.folder),RECT{786,609,878,635}) &&
            equal(bounds(app.finish),RECT{302,647,408,681}) && equal(bounds(app.openFolder),RECT{745,647,878,681}),
            "Original normal-size control geometry changed.");
        require(bounds(app.preview).bottom-bounds(app.preview).top==320,"Original roomy preview height changed.");
        std::cout<<"PASS original normal-size geometry unchanged\n";
    }
    for(int outerWidth:{1366,1024}){
        app.dpi=192;syntheticWork={0,0,outerWidth,688};RECT border{};
        require(AdjustWindowRectExForDpi(&border,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,FALSE,0,192)!=FALSE,"Nonclient calculation failed.");
        HiddenWindow owned(outerWidth-(border.right-border.left),688-(border.bottom-border.top),192);
        MINMAXINFO minimum{};windowProc(app.window,WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&minimum));
        require(minimum.ptMinTrackSize.x==outerWidth && minimum.ptMinTrackSize.y==688,"Work-area cap no longer reaches original scenario.");
        checkLayout();checkFocusReachability();
        std::cout<<"PASS original overlap replay outer="<<outerWidth<<"x688 app_dpi=192 all controls reachable\n";
    }
    syntheticWork={0,0,10000,10000};
}
void customGeometry(int dpi){
    HiddenWindow owned(MulDiv(830,dpi,96),MulDiv(700,dpi,96),dpi);
    SendMessageW(app.interval,CB_RESETCONTENT,0,0);for(int value:CaptureIntervals)add(app.interval,formatDuration(value));
    SendMessageW(app.videoSize,CB_RESETCONTENT,0,0);add(app.videoSize,L"720p");add(app.videoSize,L"1080p");
    app.customIntervalMs=86399999;app.customWidth=4096;app.customHeight=2160;app.customLimitSeconds=INT_MAX;
    app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=true;app.committedInterval=6;app.committedSize=2;app.committedLimit=6;
    customItems();configure();app.advancedExpanded=true;updateControls();layout();
    const auto fits=[&](HWND child,int padding){wchar_t label[256]{};GetWindowTextW(child,label,256);HDC dc=GetDC(child);const auto previous=SelectObject(dc,app.font);SIZE extent{};
        GetTextExtentPoint32W(dc,label,static_cast<int>(std::wcslen(label)),&extent);SelectObject(dc,previous);ReleaseDC(child,dc);
        require(extent.cx+app.scale(padding)<=bounds(child).right-bounds(child).left,"Custom value or explanatory encoding label truncates at minimum width/DPI.");};
    for(HWND child:{app.interval,app.videoSize,app.stopAfter})fits(child,30);
    for(int i=0;i<5;++i){choose(app.encodingMode,i);fits(app.encodingMode,30);}
    fits(app.advanced,18);require(app.advancedTooltip.find(formatDuration(int64_t(INT_MAX)*1000))!=std::wstring::npos,"Collapsed finite-stop tooltip lost the exact maximum value.");
    for(const auto& size: {std::pair<int,int>{1000,1000},{1080,1920},{3840,2160},{48,4096},{4096,48}}){
        app.settings.width=size.first;app.settings.height=size.second;app.settings.layers=preset(Mode::Overlay);app.selected=1;
        layout();RECT canvas{};GetClientRect(app.preview,&canvas);const auto video=app.videoRect;
        require(video.left>=0 && video.top>=0 && video.right<=canvas.right && video.bottom<=canvas.bottom && video.right>video.left && video.bottom>video.top,
            "Custom aspect produced an empty or out-of-preview canvas.");
        const double actual=double(video.right-video.left)/std::max(1L,video.bottom-video.top),target=double(size.first)/size.second;
        require(std::abs(actual-target)<=std::max(0.02,2.0*target/std::max(1L,video.bottom-video.top)),"Custom preview kept the old aspect.");
        const auto layer=layerRect(app.settings.layers[1]);require(layer.left>=video.left && layer.top>=video.top && layer.right<=video.right && layer.bottom<=video.bottom,
            "Custom aspect moved layer handles outside their video canvas.");
    }
    std::cout<<"PASS exact custom summaries, encoder explanations and square/portrait/wide canvas dpi="<<dpi<<'\n';
}
void segmentDisclosure(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    const auto collapsed=app.contentHeight;
    require(!visible(app.splitEvery)&&!visible(app.segmentLabel)&&!selectedSegment(),"Default split control was visible or enabled.");
    const auto fits=[&](HWND child,int padding){wchar_t text[256]{};GetWindowTextW(child,text,256);std::wstring label=text;
        label.erase(std::remove(label.begin(),label.end(),L'&'),label.end());HDC dc=GetDC(child);const auto prior=SelectObject(dc,app.font);SIZE size{};
        GetTextExtentPoint32W(dc,label.c_str(),static_cast<int>(label.size()),&size);SelectObject(dc,prior);ReleaseDC(child,dc);
        require(size.cx+app.scale(padding)<=bounds(child).right-bounds(child).left,"Split label/summary/custom duration truncates.");};
    for(int i=1;i<5;++i){choose(app.splitEvery,i);configure();updateControls();fits(app.advanced,18);
        require(app.advancedCaption.find(L"split")!=std::wstring::npos && app.advancedTooltip.find(formatDuration(int64_t(SegmentDurations[i])*1000))!=std::wstring::npos,
            "Collapsed Advanced hides enabled splitting or its exact duration.");}
    app.customSegmentSeconds=INT_MAX;app.hasCustomSegment=true;app.committedSegment=5;customItems();configure();updateControls();fits(app.advanced,18);
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);fits(app.splitEvery,30);fits(app.segmentLabel,0);fits(app.recoveryMode,26);fits(app.captureCursor,26);
    require(visible(app.splitEvery)&&visible(app.segmentLabel)&&!intersects(bounds(app.splitEvery),bounds(app.recoveryMode)) &&
        !intersects(bounds(app.splitEvery),bounds(app.captureCursor))&&bounds(app.splitEvery).bottom<bounds(app.skipConfigure).top,"Split/recovery/cursor row overlaps adjacent controls.");
    checkLayout();checkFocusReachability();ownedFocus=app.splitEvery;windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(app.contentHeight==collapsed && ownedFocus==app.advanced && !visible(app.splitEvery)&&!visible(app.segmentLabel),"Collapse stranded split focus or increased base height.");
    app.settings.night.enabled=true;app.settings.timeSkip.mode=TimeSkipMode::NoPerson;app.settings.recoveryMode=true;++app.skipRevision;
    choose(app.stopAfter,2);updateAdvanced();fits(app.advanced,18);
    require(app.advancedCaption.find(L"split")!=std::wstring::npos && app.advancedTooltip.find(L"Person checks")!=std::wstring::npos &&
        app.advancedTooltip.find(formatDuration(int64_t(INT_MAX)*1000))!=std::wstring::npos,"Mixed summary lost splitting/optional camera detector facts.");
    app.status.state=State::Recording;app.status.frames=321;app.status.elapsed=123;app.status.completedSegments=2;updateControls();paintCheck();
    require(drawnText[3].value.find(L"321 frames")!=std::wstring::npos && drawnText[3].value.find(L"2 parts saved")!=std::wstring::npos,
        "Saved-parts statistics reset overall frame/time totals or omitted the count.");
    app.status.completedSegments=1;paintCheck();require(drawnText[3].value.find(L"1 part saved")!=std::wstring::npos,"Singular saved-parts count was incorrect.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);require(IsWindowEnabled(app.advanced)&&!IsWindowEnabled(app.splitEvery),"Active splitting was editable or disclosure was locked.");
    std::cout<<"PASS split disclosure/default, exact summaries, row bounds, focus/scroll, cumulative part counts and active lock dpi="<<dpi<<'\n';
}
void startDelayDisclosure(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    const int collapsed=app.contentHeight;
    const auto visible=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    const auto fits=[](HWND child,int margin){
        wchar_t label[256]{};GetWindowTextW(child,label,256);std::wstring text=label;
        text.erase(std::remove(text.begin(),text.end(),L'&'),text.end());
        HDC dc=GetDC(child);const auto previous=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));SIZE extent{};
        GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&extent);SelectObject(dc,previous);ReleaseDC(child,dc);
        require(extent.cx+app.scale(margin)<=bounds(child).right-bounds(child).left,"Self-timer label or selected choice clips.");
    };
    require(!visible(app.startDelay)&&!visible(app.startDelayLabel)&&!visible(app.startDelayHint)&&app.advancedCaption==L"&Advanced"&&!selectedStartDelay(),"Default self-timer adds base clutter or an armed choice.");
    choose(app.startDelay,5);configure();updateControls();fits(app.advanced,18);
    require(app.advancedCaption.find(L"delay 5 min")!=std::wstring::npos&&app.advancedTooltip.find(L"5 minutes")!=std::wstring::npos,"Collapsed disclosure hides selected self-timer.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(visible(app.startDelay)&&visible(app.startDelayLabel)&&visible(app.startDelayHint),"Expanded self-timer row is incomplete.");
    fits(app.startDelay,30);fits(app.startDelayLabel,0);
    const auto delay=bounds(app.startDelay),hint=bounds(app.startDelayHint),label=bounds(app.startDelayLabel);
    require(label.bottom<=delay.top&&!intersects(delay,hint)&&bounds(app.recoveryMode).bottom<label.top&&hint.bottom<=bounds(app.skipConfigure).top,"Self-timer timing row overlaps adjacent options.");
    wchar_t help[256]{};GetWindowTextW(app.startDelayHint,help,256);HDC dc=GetDC(app.startDelayHint);const auto previous=SelectObject(dc,app.smallFont);
    RECT measured{0,0,hint.right-hint.left,0};DrawTextW(dc,help,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,previous);ReleaseDC(app.startDelayHint,dc);
    require(measured.bottom<=hint.bottom-hint.top,"Self-timer explanatory help clips when wrapped.");
    checkLayout();checkFocusReachability();ownedFocus=app.startDelay;windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(app.contentHeight==collapsed&&ownedFocus==app.advanced&&!visible(app.startDelay),"Collapse strands self-timer focus or expands the base window.");
    app.settings.watermark.enabled=true;++app.watermarkRevision;choose(app.splitEvery,4);choose(app.stopAfter,5);SendMessageW(app.captureCursor,BM_SETCHECK,BST_UNCHECKED,0);updateAdvanced();fits(app.advanced,18);
    require(app.advancedCaption.find(L"delay")!=std::wstring::npos,"Mixed collapsed summary loses the next-recording delay.");
    app.encodingValidation=L"Check the MP4 mode";updateAdvanced();require(app.advancedCaption.find(L"check MP4")!=std::wstring::npos,"Self-timer summary displaced existing validation.");
    app.encodingValidation.clear();app.settings.watermark.enabled=false;++app.watermarkRevision;choose(app.startDelay,3);configure();
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    for(auto state:{State::Waiting,State::Starting}){
        app.status.state=state;updateControls();wchar_t caption[64]{};GetWindowTextW(app.finish,caption,64);
        require(std::wstring(caption)==L"Cancel s&tart"&&IsWindowEnabled(app.finish)&&!IsWindowEnabled(app.record)&&!IsWindowEnabled(app.pause)&&!IsWindowEnabled(app.startDelay)&&!IsWindowEnabled(app.stopAfter)&&IsWindowEnabled(app.advanced),"Pending start loses cancellation or unlocks recording options.");
        choose(app.startDelay,5);windowProc(app.window,WM_COMMAND,MAKEWPARAM(StartDelayBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.startDelay));
        require(choice(app.startDelay)==3&&app.settings.startDelaySeconds==30,"Forged pending-start selection changes the frozen delay.");
        fits(app.finish,18);
    }
    app.status.state=State::Recording;updateControls();wchar_t finish[64]{};GetWindowTextW(app.finish,finish,64);require(std::wstring(finish)==L"&Finish","Recording retained the pending-start action caption.");
    std::cout<<"PASS self-timer minimal disclosure, wrapped help, warning priority, frozen options and Cancel start dpi="<<dpi<<'\n';
}
void cursorDisclosure(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    const int collapsed=app.contentHeight;const auto styled=[](HWND child){return (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;};
    require(!styled(app.captureCursor)&&app.advancedCaption==L"&Advanced","Default cursor option expanded or changed the base disclosure.");
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);const int expanded=app.contentHeight;
    SendMessageW(app.captureCursor,BM_SETCHECK,BST_UNCHECKED,0);windowProc(app.window,WM_COMMAND,MAKEWPARAM(CursorBox,BN_CLICKED),reinterpret_cast<LPARAM>(app.captureCursor));
    require(app.contentHeight==expanded&&app.advancedCaption.find(L"cursor off")!=std::wstring::npos,"Cursor selection changed layout height or hid its nondefault summary.");
    ownedFocus=app.captureCursor;windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(app.contentHeight==collapsed&&!styled(app.captureCursor)&&ownedFocus==app.advanced,"Collapse stranded cursor focus or added base height.");
    app.settings.layers=preset(Mode::Camera);updateControls();require(!IsWindowEnabled(app.captureCursor)&&app.advancedCaption.find(L"cursor off")==std::wstring::npos,"Camera-only caption implies a desktop effect or checkbox stays enabled.");
    app.settings.layers=preset(Mode::Desktop);app.settings.watermark.enabled=true;++app.watermarkRevision;app.settings.recoveryMode=true;choose(app.splitEvery,4);choose(app.stopAfter,5);updateAdvanced();
    std::wstring caption=app.advancedCaption;caption.erase(std::remove(caption.begin(),caption.end(),L'&'),caption.end());
    HDC dc=GetDC(app.advanced);const auto previous=SelectObject(dc,app.font);SIZE size{};GetTextExtentPoint32W(dc,caption.c_str(),static_cast<int>(caption.size()),&size);SelectObject(dc,previous);ReleaseDC(app.advanced,dc);
    require(size.cx+app.scale(18)<=bounds(app.advanced).right-bounds(app.advanced).left&&caption.find(L"cursor off")!=std::wstring::npos,"Mixed cursor summary truncates or loses nondefault state.");
    app.encodingValidation=L"Check the MP4 mode";updateAdvanced();require(app.advancedCaption.find(L"check MP4")!=std::wstring::npos&&app.advancedCaption.find(L"cursor off")==std::wstring::npos,"Cursor summary displaced a validation warning.");
    std::cout<<"PASS cursor minimal disclosure, source scope, mixed summary width, warning priority and collapse focus dpi="<<dpi<<'\n';
}
}
int main(){
    try{
        std::cout<<std::unitbuf;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        for(int dpi:{96,144,192,288}){scenario(dpi,false);scenario(dpi,true);advancedDisclosure(dpi);compressionDisclosure(dpi);watermarkDisclosure(dpi);recoveryDisclosure(dpi);nightDisclosure(dpi);customGeometry(dpi);segmentDisclosure(dpi);cursorDisclosure(dpi);startDelayDisclosure(dpi);}
        dragChecks();barDependency();dpiAndRouting();originalScenarios();std::cout<<"PASS hidden scrolling, custom geometry and disclosure cases at four DPIs\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FIXTURE FAILURE: "<<e.what()<<'\n';return 1;}
}

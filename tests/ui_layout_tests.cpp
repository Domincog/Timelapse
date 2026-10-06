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
#include <array>
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
    void finish() {} void cancelDelayedStart() noexcept {} void setStatus(const StatusItem&) {}
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
std::array<SHORT,256> keyState{};
SHORT WINAPI fixtureGetKeyState(int key){return keyState[size_t(key)&255];}
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
    if(paintDC && !(flags&DT_CALCRECT)){
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
bool styled(HWND child){return child && (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;}
// Every control the panel and stage own, in native sibling (Tab) order.
std::vector<HWND> siblingOrder(){
    return {app.reset,app.forward,app.preview,app.record,app.pause,app.finish,app.statusText,app.tabs[0],app.tabs[1],app.tabs[2],
        app.refresh,app.labels[0],app.mode,app.labels[4],app.monitor,app.labels[5],app.camera,app.alsoLabel,app.alsoDesktop,app.alsoCamera,
        app.labels[1],app.interval,app.labels[2],app.videoSize,app.labels[3],app.encodingQuality,app.folder,app.openFolder,app.liveStatusSummary,app.liveStatusSet,app.liveStatusClear,
        app.labels[7],app.stopAfter,app.segmentLabel,app.splitEvery,app.startDelayLabel,app.startDelay,app.lowDisk,app.captureCursor,app.skipConfigure,app.skipSummary,app.skipDetail,
        app.nightEnabled,app.labels[8],app.nightDuration,app.labels[9],app.nightTarget,app.nightHint,app.nightDetail,
        app.labels[6],app.encodingMode,app.encoderConfigure,app.encoderSummary,app.recoveryMode,app.watermarkConfigure,app.watermarkSummary,app.playbackConfigure,app.playbackSummary,app.resetDefaults};
}
std::vector<HWND> tabControls(){
    std::vector<HWND> result;
    for(HWND child:siblingOrder())if(styled(child) && (GetWindowLongPtrW(child,GWL_STYLE)&WS_TABSTOP))result.push_back(child);
    return result;
}
std::vector<HWND> visiblePanel(){
    std::vector<HWND> result;
    for(HWND child:siblingOrder())if(styled(child) && panelControl(child))result.push_back(child);
    return result;
}
int textWidth(HWND child,HFONT font){
    wchar_t raw[256]{};GetWindowTextW(child,raw,256);std::wstring value=raw;
    for(size_t at=value.find(L'&');at!=std::wstring::npos;at=value.find(L'&',at+1))value.erase(at,1);
    HDC dc=GetDC(child);const auto previous=SelectObject(dc,font);SIZE extent{};
    GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&extent);SelectObject(dc,previous);ReleaseDC(child,dc);return extent.cx;
}
void fits(HWND child,int padding,const char* message){require(textWidth(child,app.font)+app.scale(padding)<=bounds(child).right-bounds(child).left,message);}
void click(int id){windowProc(app.window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),0);}
void page(int tab){click(TabCapture+tab);require(app.panelTab==tab,"Tab click did not select its page.");}
struct HiddenWindow {
    HiddenWindow(int width,int height,int dpi){
        app.dpi=dpi;app.scrollX=app.scrollY=app.panelScroll=app.wheelVertical=app.wheelHorizontal=app.wheelPanel=0;
        app.contentWidth=app.contentHeight=0;app.layingOut=app.dragging=false;app.panelTab=CaptureTab;invalidatePanel();
        app.visibleDirty=true;app.controlsUpdated=false;app.hiddenToTray=false;
        app.skipRevision=0;app.skipSummaryRevision=-1;app.skipSummaryCaption.clear();app.skipDetailCaption.clear();app.skipCheckAge=UINT64_MAX;
        app.settings={};app.status={};app.watermarkCheckValid=false;app.watermarkValidation.clear();app.watermarkCaption.clear();app.watermarkRevision=0;app.nightValidation.clear();app.encodingValidation.clear();
        app.encoderCaption.clear();app.playbackCaption.clear();app.hotkeyWarning.clear();app.pauseHotkey=app.stopHotkey=app.statusHotkey=0;
        app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;
        app.hasCustomSegment=false;app.customSegmentSeconds=900;app.committedSegment=0;
        app.committedStartDelay=0;app.waitingRemaining=UINT64_MAX;app.waitingCaption.clear();
        app.committedInterval=2;app.committedSize=app.committedLimit=0;
        ownedFocus=ownedCapture=nullptr;keyState.fill(0);
        app.window=CreateWindowExW(0,L"STATIC",L"Owned hidden scrolling fixture",WS_POPUP|WS_CLIPCHILDREN,0,0,width,height,nullptr,nullptr,nullptr,nullptr);
        require(app.window && !IsWindowVisible(app.window),"Hidden parent creation failed.");
        auto child=[&](const wchar_t* cls,const wchar_t* name,DWORD style,int id){auto w=control(cls,name,style,id);require(w!=nullptr,"Child creation failed.");return w;};
        auto combo=[&](int label,const wchar_t* caption,int id){app.labels[label]=child(L"STATIC",caption,SS_CENTERIMAGE,200+label);auto w=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,id);add(w,L"First");add(w,L"Second");choose(w,0);return w;};
        auto button=[&](const wchar_t* name,int id){return child(L"BUTTON",name,WS_TABSTOP|BS_PUSHBUTTON,id);};
        auto summary=[&](int id){return child(L"STATIC",L"Off",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,id);};
        // Same creation (sibling/Tab) order as the application.
        app.reset=button(L"Reset layout",Reset);app.forward=button(L"Bring forward",Forward);
        app.preview=child(L"STATIC",L"Preview",WS_TABSTOP,Preview);
        app.record=button(L"&Record",Record);app.pause=button(L"&Pause",Pause);app.finish=button(L"&Finish",Finish);
        app.statusText=child(L"STATIC",L"Ready",SS_LEFT|SS_CENTERIMAGE,210);
        for(int i=0;i<PanelTabCount;++i)app.tabs[i]=child(L"BUTTON",PanelTabLabels[i],(i?0:WS_GROUP|WS_TABSTOP)|BS_AUTORADIOBUTTON|BS_PUSHLIKE,TabCapture+i);
        app.refresh=child(L"BUTTON",L"Re&fresh",WS_GROUP|WS_TABSTOP|BS_PUSHBUTTON,Refresh);
        app.mode=combo(0,L"&Source",ModeBox);app.monitor=combo(4,L"&Display",MonitorBox);app.camera=combo(5,L"Ca&mera",CameraBox);
        app.alsoLabel=child(L"STATIC",L"Also save files",SS_NOPREFIX|SS_CENTERIMAGE,214);
        app.alsoDesktop=child(L"BUTTON",L"Desktop",WS_TABSTOP|BS_AUTOCHECKBOX,AlsoDesktopBox);app.alsoCamera=child(L"BUTTON",L"Camera",WS_TABSTOP|BS_AUTOCHECKBOX,AlsoCameraBox);
        app.interval=combo(1,L"Capture &every",IntervalBox);app.videoSize=combo(2,L"Video si&ze",SizeBox);app.encodingQuality=combo(3,L"Video &quality",EncodingQualityBox);
        app.folder=button(L"C&hange...",Folder);app.openFolder=button(L"&Open folder",OpenFolder);
        app.liveStatusSummary=child(L"STATIC",L"",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,StatusSummaryLine);
        app.liveStatusSet=button(L"Set stat&us...",SetStatusButton);app.liveStatusClear=button(L"Clear",ClearStatusButton);
        app.stopAfter=combo(7,L"S&top after",StopAfterBox);
        SendMessageW(app.stopAfter,CB_RESETCONTENT,0,0);for(auto name:RecordingLimitLabels)add(app.stopAfter,name);choose(app.stopAfter,0);
        app.segmentLabel=child(L"STATIC",L"Split files e&very",SS_CENTERIMAGE,211);app.splitEvery=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,SegmentBox);
        for(auto name:SegmentLabels)add(app.splitEvery,name);choose(app.splitEvery,0);
        app.startDelayLabel=child(L"STATIC",L"St&art delay",SS_CENTERIMAGE,212);
        app.startDelay=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,StartDelayBox);
        for(auto label:StartDelayLabels)add(app.startDelay,label);choose(app.startDelay,0);
        app.lowDisk=child(L"BUTTON",L"Stop on &low disk space",WS_TABSTOP|BS_AUTOCHECKBOX,LowDiskBox);SendMessageW(app.lowDisk,BM_SETCHECK,BST_CHECKED,0);
        app.captureCursor=child(L"BUTTON",L"Show des&ktop cursor",WS_TABSTOP|BS_AUTOCHECKBOX,CursorBox);SendMessageW(app.captureCursor,BM_SETCHECK,BST_CHECKED,0);
        app.skipConfigure=button(L"Time &compression...",SkipConfigure);app.skipSummary=summary(SkipSummary);
        app.skipDetail=child(L"STATIC",L"",SS_LEFT|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,SkipDetail);
        app.nightEnabled=child(L"BUTTON",L"&Night camera (software blend)",WS_TABSTOP|BS_AUTOCHECKBOX,NightBox);
        app.nightDuration=combo(8,L"Blend d&uration",NightDurationBox);SendMessageW(app.nightDuration,CB_RESETCONTENT,0,0);for(auto name:NightDurationLabels)add(app.nightDuration,name);choose(app.nightDuration,0);
        app.nightTarget=combo(9,L"Auto &brightness",NightTargetBox);SendMessageW(app.nightTarget,CB_RESETCONTENT,0,0);for(auto name:{L"Dark",L"Balanced",L"Bright"})add(app.nightTarget,name);choose(app.nightTarget,1);
        app.nightHint=child(L"STATIC",L"",SS_LEFT|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,NightHint);app.nightDetail=child(L"STATIC",L"",SS_LEFT|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,NightDetail);
        app.encodingMode=combo(6,L"Encodin&g",EncodingModeBox);SendMessageW(app.encodingMode,CB_RESETCONTENT,0,0);for(auto name:EncodingModeLabels)add(app.encodingMode,name);choose(app.encodingMode,0);
        app.encoderConfigure=button(L"Encoder settings...",EncoderConfigure);app.encoderSummary=summary(EncoderSummary);
        app.recoveryMode=child(L"BUTTON",L"MP4 recover&y mode (H.264)",WS_TABSTOP|BS_AUTOCHECKBOX,RecoveryBox);
        app.watermarkConfigure=button(L"&Watermark...",WatermarkConfigure);app.watermarkSummary=summary(WatermarkSummary);
        app.playbackConfigure=button(L"Playback && shortcuts...",PlaybackConfigure);app.playbackSummary=summary(PlaybackSummary);
        app.resetDefaults=button(L"Reset all settings...",ResetDefaults);
        for(HWND member:siblingOrder())SetWindowPos(member,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetWindowLongPtrW(app.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(fixtureWindowProc));
        app.engine=std::make_unique<FixtureEngine>();
        fonts();configure();updateControls();layout();
    }
    ~HiddenWindow(){
        ownedFocus=ownedCapture=nullptr;keyState.fill(0);SetWindowLongPtrW(app.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(DefWindowProcW));
        DestroyWindow(app.window);app.window=app.preview=app.captureCursor=app.startDelay=app.startDelayLabel=app.playbackConfigure=nullptr;
        for(auto& tab:app.tabs)tab=nullptr;app.encoderSummary=app.playbackSummary=app.resetDefaults=nullptr;app.engine.reset();
    }
};
void checkLayout(){
    const auto view=client();
    require(app.contentWidth>=app.scale(StageMinWidth+PanelWidth) && app.contentHeight>=app.scale(StageMinHeight),"Logical minimum lost.");
    require(app.scrollX>=0 && app.scrollX<=app.contentWidth-view.right && app.scrollY>=0 && app.scrollY<=app.contentHeight-view.bottom,"Offsets exceed viewport range.");
    require(!intersects(bounds(app.forward),bounds(app.tabs[0])) && !intersects(bounds(app.finish),bounds(app.openFolder)),"Stage controls overlap the settings panel.");
    // No two visible panel rows overlap, on any page, and every row stays in the panel.
    const auto panel=visiblePanel();
    for(size_t i=0;i<panel.size();++i)for(size_t j=i+1;j<panel.size();++j)
        if(intersects(bounds(panel[i]),bounds(panel[j]))){
            std::cerr<<"Overlap between control ids "<<GetDlgCtrlID(panel[i])<<" and "<<GetDlgCtrlID(panel[j])<<" on page "<<app.panelTab<<'\n';
            require(false,"Visible settings rows overlap.");}
    for(HWND child:panel){
        const auto r=bounds(child);const int offset=app.panelDocked?app.panelScroll:app.scrollY,height=app.panelDocked?app.panelHeight:app.contentHeight;
        require(r.left+app.scrollX>=app.panelRect.left && r.right+app.scrollX<=app.contentWidth && r.top+offset>=0 && r.bottom+offset<=height,"Settings row escaped its panel.");
    }
    // Labels, checkboxes, option buttons and choices keep their whole text.
    for(HWND label:{app.labels[0],app.labels[1],app.labels[2],app.labels[3],app.labels[4],app.labels[5],app.labels[6],app.labels[7],app.labels[8],app.labels[9],app.segmentLabel,app.startDelayLabel,app.alsoLabel})
        if(styled(label))fits(label,0,"A settings label truncates in its column.");
    for(HWND box:{app.lowDisk,app.captureCursor,app.recoveryMode,app.nightEnabled,app.alsoDesktop,app.alsoCamera})if(styled(box))fits(box,26,"A checkbox label truncates.");
    for(HWND button:{app.skipConfigure,app.watermarkConfigure,app.playbackConfigure,app.encoderConfigure,app.resetDefaults,app.refresh,app.folder,app.openFolder,app.liveStatusSet,app.tabs[0],app.tabs[1],app.tabs[2]}){
        const HFONT font=panelPage(button)<0?app.strongFont:app.font;
        if(styled(button) && textWidth(button,font)+app.scale(18)>bounds(button).right-bounds(button).left){
            std::cerr<<"Caption of control "<<GetDlgCtrlID(button)<<" needs "<<textWidth(button,font)+app.scale(18)<<" of "<<bounds(button).right-bounds(button).left<<" at dpi "<<app.dpi<<'\n';
            require(false,"An option button or tab caption truncates.");}}
    if(styled(app.mode)){HDC textDc=GetDC(app.mode);auto oldFont=SelectObject(textDc,app.font);SIZE labelSize{};
        GetTextExtentPoint32W(textDc,SeparateFilesLabel,static_cast<int>(std::wcslen(SeparateFilesLabel)),&labelSize);SelectObject(textDc,oldFont);ReleaseDC(app.mode,textDc);
        require(labelSize.cx+GetSystemMetricsForDpi(SM_CXVSCROLL,app.dpi)+app.scale(12)<=bounds(app.mode).right-bounds(app.mode).left,"Separate-files label truncates in the selected source control.");}
    const auto preview=bounds(app.preview);
    require(preview.bottom-preview.top>=app.scale(160),"Preview is unusably short.");
    RECT local{};GetClientRect(app.preview,&local);require(equal(app.videoRect,previewVideoRect(local)),"Hit-test geometry was left waiting for paint.");
    for(HWND w:{app.preview,app.record,app.pause,app.finish,app.statusText}){if(!styled(w))continue;const auto r=bounds(w);
        require(r.left+app.scrollX>=0 && r.right+app.scrollX<=app.contentWidth && r.top+app.scrollY>=0 && r.bottom+app.scrollY<=app.contentHeight,"Child escaped its stage.");}
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
    // Inspect the complete canvas even when the real dirty viewport culls
    // off-screen stage text. Scoped WM_PAINT invalidation is covered in tray tests.
    paintWindow(paintDC,RECT{-app.scrollX,-app.scrollY,app.contentWidth-app.scrollX,app.contentHeight-app.scrollY});
    const bool capture=app.panelTab==CaptureTab;
    require(drawnText.size()==size_t(4+app.headingCount+(capture?1:0)),"Unexpected parent painted-text count.");
    for(const auto& draw:drawnText){RECT expected=draw.logical;OffsetRect(&expected,-app.scrollX,-app.scrollY);require(equal(expected,draw.device),"Parent paint origin does not follow scrolling.");}
    if(capture){
        require(drawnText.back().logical.top==app.savePathRect.top-app.panelScroll,"Painted save path uses viewport height.");
        require(drawnText.back().device.bottom+app.scale(6)==bounds(app.folder).top,"Painted save path detached from Change button.");
    }else require(app.savePathRect.right<=app.savePathRect.left,"Save path painted on a page without the save folder.");
    for(int i=0;i<app.headingCount;++i)require(drawnText[size_t(4+i)].value==app.headings[i].text,"Page headings did not paint in order.");
    SelectObject(paintDC,old);DeleteObject(bitmap);DeleteDC(paintDC);paintDC=nullptr;
}
void scenario(int dpi,bool constrained){
    const int width=constrained?MulDiv(500,dpi,96):MulDiv(1000,dpi,96), height=constrained?MulDiv(400,dpi,96):MulDiv(740,dpi,96);
    HiddenWindow owned(width,height,dpi);checkLayout();
    const auto style=GetWindowLongPtrW(app.window,GWL_STYLE);
    require(((style&WS_HSCROLL)!=0)==constrained && ((style&WS_VSCROLL)!=0)==constrained,"Wrong scrollbar state.");
    if(!constrained){
        require(app.scrollX==0 && app.scrollY==0 && app.contentWidth==width && app.contentHeight==height,"Roomy layout changed its client canvas.");
        // The point of pages: at a normal size every page fits with no panel scrolling.
        for(int tab:{RecordingTab,OutputTab,CaptureTab}){page(tab);checkLayout();require(app.panelHeight<=client().bottom && !(GetWindowLongPtrW(app.window,GWL_STYLE)&WS_VSCROLL),"A settings page needs scrolling at a normal window size.");}
    }
    checkFocusReachability();
    scrollTo(INT_MAX,INT_MAX);checkLayout();paintCheck();
    scrollBar(SB_VERT,SB_TOP);scrollBar(SB_HORZ,SB_TOP);
    require(app.scrollX==0 && app.scrollY==0 && app.panelScroll==0,"Top command did not reset offsets.");
    if(constrained){
        scrollBar(SB_VERT,SB_PAGEDOWN);require((app.panelDocked?app.panelScroll:app.scrollY)>0,"Vertical page command did not scroll.");
        scrollBar(SB_HORZ,SB_LINEDOWN);require(app.scrollX>0,"Horizontal line command did not scroll.");
        scrollTo(INT_MAX,0);scrollPanelTo(0);MSG wheel{};wheel.hwnd=app.encodingQuality;wheel.message=WM_MOUSEWHEEL;wheel.wParam=MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA));
        POINT pointer{int(client().right)-app.scale(24),app.scale(60)};ClientToScreen(app.window,&pointer);wheel.lParam=MAKELPARAM(pointer.x,pointer.y);
        require(scrollWheelMessage(wheel) && (app.panelDocked?app.panelScroll:app.scrollY)>0 && choice(app.encodingQuality)==0,"Closed quality combo wheel changed settings or failed to scroll.");
        wheel.wParam=MAKEWPARAM(MK_SHIFT,static_cast<WORD>(-WHEEL_DELTA));require(scrollWheelMessage(wheel) && app.scrollX>0,"Shift wheel did not scroll horizontally.");
        droppedCombo=app.encodingQuality;require(!scrollWheelMessage(wheel),"An open combo lost wheel ownership.");droppedCombo=nullptr;
        scrollTo(0,0);wheel.message=WM_MOUSEHWHEEL;wheel.wParam=MAKEWPARAM(0,WHEEL_DELTA);require(scrollWheelMessage(wheel) && app.scrollX>0,"Horizontal wheel did not scroll right.");
        const int before=app.scrollX;wheel.wParam=MAKEWPARAM(MK_CONTROL,WHEEL_DELTA);require(!scrollWheelMessage(wheel) && app.scrollX==before,"Ctrl wheel was intercepted.");
        // Changing pages starts the new page at its top.
        scrollPanelTo(INT_MAX);page(OutputTab);require(app.panelScroll==0,"A new page kept the previous page's scroll offset.");page(CaptureTab);
        scrollTo(INT_MAX,INT_MAX);SetWindowPos(app.window,nullptr,0,0,MulDiv(1000,dpi,96),MulDiv(740,dpi,96),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);checkLayout();
        require(app.scrollX==0 && app.scrollY==0 && !(GetWindowLongPtrW(app.window,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL)),"Roomy resize left scroll offsets or bars.");
    }
    std::cout<<"PASS layout dpi="<<dpi<<" client_input="<<width<<'x'<<height<<" mode="<<(constrained?"constrained":"roomy")<<" real_window_dpi="<<GetDpiForWindow(app.window)<<'\n';
}
void pages(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    const auto checked=[](int tab){return SendMessageW(app.tabs[tab],BM_GETCHECK,0,0)==BST_CHECKED;};
    require(app.panelTab==CaptureTab && checked(CaptureTab) && !checked(RecordingTab) && !checked(OutputTab),"Capture was not the default page.");
    require(styled(app.mode)&&styled(app.interval)&&styled(app.folder)&&styled(app.liveStatusSet)&&!styled(app.stopAfter)&&!styled(app.lowDisk)&&!styled(app.encodingMode)&&!styled(app.resetDefaults),
        "The Capture page showed rows from other pages or hid its own.");
    require(GetNextDlgTabItem(app.window,app.tabs[CaptureTab],FALSE)==app.refresh && GetNextDlgTabItem(app.window,app.refresh,FALSE)==app.mode,"Capture page Tab order is wrong.");
    const int captureHeight=app.panelHeight;
    ownedFocus=app.mode;page(RecordingTab);
    require(checked(RecordingTab) && !checked(CaptureTab) && styled(app.stopAfter) && styled(app.lowDisk) && styled(app.captureCursor) && styled(app.skipConfigure) && !styled(app.mode) && !styled(app.folder),
        "The Recording page did not replace the Capture rows.");
    require(ownedFocus==app.tabs[CaptureTab] || ownedFocus==app.tabs[RecordingTab],"Focus stayed on a hidden row.");
    require(GetNextDlgTabItem(app.window,app.tabs[RecordingTab],FALSE)==app.stopAfter && GetNextDlgTabItem(app.window,app.stopAfter,FALSE)==app.splitEvery &&
        GetNextDlgTabItem(app.window,app.splitEvery,FALSE)==app.startDelay && GetNextDlgTabItem(app.window,app.startDelay,FALSE)==app.lowDisk &&
        GetNextDlgTabItem(app.window,app.lowDisk,FALSE)==app.captureCursor && GetNextDlgTabItem(app.window,app.captureCursor,FALSE)==app.skipConfigure,"Recording page Tab order is wrong.");
    checkLayout();checkFocusReachability();
    page(OutputTab);
    require(styled(app.encodingMode)&&styled(app.encoderConfigure)&&styled(app.recoveryMode)&&styled(app.watermarkConfigure)&&styled(app.playbackConfigure)&&styled(app.resetDefaults)&&!styled(app.stopAfter),
        "The Output page did not show its rows.");
    require(GetNextDlgTabItem(app.window,app.tabs[OutputTab],FALSE)==app.encodingMode && GetNextDlgTabItem(app.window,app.encodingMode,FALSE)==app.encoderConfigure &&
        GetNextDlgTabItem(app.window,app.encoderConfigure,FALSE)==app.recoveryMode && GetNextDlgTabItem(app.window,app.recoveryMode,FALSE)==app.watermarkConfigure &&
        GetNextDlgTabItem(app.window,app.watermarkConfigure,FALSE)==app.playbackConfigure && GetNextDlgTabItem(app.window,app.playbackConfigure,FALSE)==app.resetDefaults,"Output page Tab order is wrong.");
    checkLayout();checkFocusReachability();
    // Ctrl+Tab and Ctrl+Shift+Tab cycle pages; focus follows only from the panel.
    MSG key{};key.hwnd=app.encodingMode;key.message=WM_KEYDOWN;key.wParam=VK_TAB;
    ownedFocus=app.encodingMode;keyState[VK_CONTROL]=SHRT_MIN;dispatchAppMessage(app.window,key);
    require(app.panelTab==CaptureTab && ownedFocus==app.tabs[CaptureTab],"Ctrl+Tab did not wrap to the Capture page with focus.");
    keyState[VK_SHIFT]=SHRT_MIN;key.hwnd=app.tabs[CaptureTab];dispatchAppMessage(app.window,key);keyState[VK_SHIFT]=0;
    require(app.panelTab==OutputTab && ownedFocus==app.tabs[OutputTab],"Ctrl+Shift+Tab did not go back a page.");
    ownedFocus=app.preview;key.hwnd=app.preview;key.wParam=VK_NEXT;dispatchAppMessage(app.window,key);keyState[VK_CONTROL]=0;
    require(app.panelTab==CaptureTab && ownedFocus==app.preview,"Ctrl+PageDown from the stage moved focus or failed to change page.");
    key.wParam=VK_TAB;dispatchAppMessage(app.window,key);require(app.panelTab==CaptureTab,"Plain Tab changed the settings page.");
    // A mnemonic that reaches a row on another page shows that page.
    ownedFocus=app.stopAfter;revealFocusedControl();require(app.panelTab==RecordingTab && styled(app.stopAfter),"Focus on a hidden page's row did not reveal its page.");
    ownedFocus=app.resetDefaults;revealFocusedControl();require(app.panelTab==OutputTab && styled(app.resetDefaults),"Focus did not reveal the Output page.");
    ownedFocus=nullptr;page(CaptureTab);require(app.panelHeight==captureHeight,"Returning to Capture changed its height.");
    // Marks: changed options are dotted, problems that block Record are red.
    require(!app.tabMarks[CaptureTab] && !app.tabMarks[RecordingTab] && !app.tabMarks[OutputTab],"Default settings marked a page as changed.");
    for(int i=1;i<6;++i){choose(app.stopAfter,i);configure();updateControls();
        require(app.tabMarks[RecordingTab]==1 && app.tabTooltips[RecordingTab].find(L"Stop after "+formatDuration(int64_t(RecordingLimits[i])*1000))!=std::wstring::npos,"A finite stop was not marked on its page.");}
    choose(app.encodingMode,3);SendMessageW(app.recoveryMode,BM_SETCHECK,BST_CHECKED,0);configure();updateControls();
    require(app.tabMarks[OutputTab]==2 && !app.encodingValidation.empty() && app.tabTooltips[OutputTab].find(app.encodingValidation)!=std::wstring::npos && !IsWindowEnabled(app.record),
        "An encoder conflict was not flagged on the Output page.");
    choose(app.encodingMode,0);SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);choose(app.stopAfter,0);configure();updateControls();
    require(!app.tabMarks[OutputTab] && !app.tabMarks[RecordingTab],"Restoring defaults left page marks.");
    // Recording keeps live options editable; size, encoder and reset stay locked.
    app.status.state=State::Recording;updateControls();page(RecordingTab);
    require(IsWindowEnabled(app.tabs[OutputTab]) && IsWindowEnabled(app.stopAfter) && IsWindowEnabled(app.lowDisk) && IsWindowEnabled(app.startDelay) && IsWindowEnabled(app.skipConfigure) &&
        !IsWindowEnabled(app.splitEvery),"Recording locked live options, pages, or allowed turning on splitting.");
    page(OutputTab);require(!IsWindowEnabled(app.encodingMode) && !IsWindowEnabled(app.resetDefaults) && IsWindowEnabled(app.watermarkConfigure) && IsWindowEnabled(app.playbackConfigure),"Recording allowed encoder edits or reset.");
    for(auto state:{State::Starting,State::Finishing}){app.status.state=state;updateControls();page(RecordingTab);
        require(!IsWindowEnabled(app.stopAfter) && !IsWindowEnabled(app.lowDisk) && !IsWindowEnabled(app.startDelay),"Preparation or finishing allowed live edits.");}
    app.status={};updateControls();
    std::cout<<"PASS pages dpi="<<dpi<<" default, exclusive rows, Tab order, Ctrl+Tab/PageDown, mnemonic reveal, marks/tooltips and active locks\n";
}
void compressionRow(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    require(!styled(app.skipConfigure)&&!styled(app.skipSummary)&&!styled(app.skipDetail),"Compression controls visible outside the Recording page.");
    page(RecordingTab);const int off=app.panelHeight;
    require(styled(app.skipConfigure)&&styled(app.skipSummary)&&!styled(app.skipDetail),"Off compression row visibility incorrect.");
    require(bounds(app.skipSummary).left>bounds(app.skipConfigure).right && bounds(app.skipSummary).top==bounds(app.skipConfigure).top,"Compression summary is not beside its button.");
    app.settings.timeSkip.mode=TimeSkipMode::Quiet;app.settings.timeSkip.multiplier=64;++app.skipRevision;updateControls();layout();
    require(app.panelHeight==off+app.scale(16)+app.scale(10) && styled(app.skipDetail) && !intersects(bounds(app.skipDetail),bounds(app.preview)),"Enabled compression detail overlaps preview or has wrong height.");
    require(app.tabMarks[RecordingTab]==1 && app.tabTooltips[RecordingTab].find(L"64")!=std::wstring::npos,"Enabled compression was not marked on its page.");
    const unsigned inspections=lapse::uiPersonPackInspections;
    app.settings.timeSkip.mode=TimeSkipMode::NoPersonWithinSchedule;++app.skipRevision;updateControls();layout();
    require(app.panelHeight==off+app.scale(16)+app.scale(10) && app.skipDetailCaption.find(L"Select camera")!=std::wstring::npos && lapse::uiPersonPackInspections==inspections,
        "Person policy changed base geometry, hid camera scope or inspected its pack from a main refresh.");
    paintCheck();require(drawnText[2].value.find(L"Base interval")!=std::wstring::npos && drawnText[3].value.find(L"Time compression")!=std::wstring::npos,"Enabled compression promised a fixed resulting video duration.");
    checkLayout();checkFocusReachability();
    ownedFocus=app.skipConfigure;page(CaptureTab);require(!styled(app.skipConfigure) && ownedFocus!=app.skipConfigure,"Leaving the page stranded compression focus.");
    page(RecordingTab);app.status.state=State::Recording;updateControls();require(IsWindowEnabled(app.skipConfigure),"Active policy inspection was disabled.");
    app.status={};std::cout<<"PASS compression row dpi="<<dpi<<" page visibility, side summary, exact detail height, marks, focus and active inspection\n";
}
void outputRows(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    page(OutputTab);
    for(const auto& [button,summary]:{std::pair<HWND,HWND>{app.encoderConfigure,app.encoderSummary},{app.watermarkConfigure,app.watermarkSummary},{app.playbackConfigure,app.playbackSummary}})
        require(styled(summary) && bounds(summary).left>bounds(button).right && bounds(summary).top==bounds(button).top,"An option summary is not beside its button.");
    require(bounds(app.encodingMode).bottom<bounds(app.encoderConfigure).top && bounds(app.encoderConfigure).bottom<bounds(app.recoveryMode).top &&
        bounds(app.recoveryMode).bottom<bounds(app.watermarkConfigure).top && bounds(app.watermarkConfigure).bottom<bounds(app.playbackConfigure).top &&
        bounds(app.playbackConfigure).bottom<bounds(app.resetDefaults).top,"Output rows are out of order or overlap.");
    require(app.watermarkCaption==L"Off" && app.encoderCaption==L"Automatic quality" && app.playbackCaption==L"30 fps","Default option summaries are wrong.");
    app.settings.watermark.enabled=true;app.settings.outputFps=60;app.pauseHotkey=static_cast<uint16_t>('P'|(HOTKEYF_CONTROL<<8));
    choose(app.encodingMode,static_cast<int>(EncodingMode::SoftwareAV1));app.settings.encodingOptions.rateControl=EncodingRateControl::ConstantQuality;app.settings.encodingOptions.av1Crf=40;
    configure();updateControls();
    require(app.watermarkCaption==watermarkSummary(app.settings.watermark) && app.encoderCaption==L"Preset 6 · CRF 40" && app.playbackCaption==L"60 fps · 1 shortcut","Changed option summaries are wrong.");
    require(app.tabMarks[OutputTab]==1 && app.tabTooltips[OutputTab].find(L"Watermark")!=std::wstring::npos && app.tabTooltips[OutputTab].find(L"60 fps")!=std::wstring::npos &&
        app.tabTooltips[OutputTab].find(L"SVT-AV1")!=std::wstring::npos,"Changed output options were not marked or listed.");
    app.pauseHotkey=0;app.settings.watermark.enabled=false;app.settings.outputFps=DefaultOutputFps;app.settings.encodingOptions={};choose(app.encodingMode,0);configure();updateControls();
    checkLayout();checkFocusReachability();
    std::cout<<"PASS output rows dpi="<<dpi<<" side summaries, order, exact captions, marks and reachability\n";
}
void recoveryRow(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    require(!styled(app.recoveryMode)&&SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==BST_UNCHECKED,"Recovery was not hidden and off by default.");
    page(OutputTab);fits(app.recoveryMode,26,"Recovery checkbox text truncates.");
    SendMessageW(app.recoveryMode,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,RecoveryBox,0);
    require(app.settings.recoveryMode && app.tabMarks[OutputTab]==1 && app.tabTooltips[OutputTab].find(L"recovery")!=std::wstring::npos,"Recovery opt-in was not marked.");
    choose(app.encodingMode,3);windowProc(app.window,WM_COMMAND,MAKEWPARAM(EncodingModeBox,CBN_SELCHANGE),0);
    require(app.tabMarks[OutputTab]==2 && !IsWindowEnabled(app.record) && app.tabTooltips[OutputTab].find(L"requires H.264")!=std::wstring::npos,"HEVC/recovery incompatibility is not discoverable.");
    page(CaptureTab);require(app.tabMarks[OutputTab]==2,"The warning mark depended on the visible page.");
    app.status.state=State::Recording;updateControls();page(OutputTab);require(!IsWindowEnabled(app.recoveryMode),"Active recovery was editable.");
    app.status={};choose(app.encodingMode,0);SendMessageW(app.recoveryMode,BM_SETCHECK,BST_UNCHECKED,0);configure();updateControls();checkLayout();checkFocusReachability();
    std::cout<<"PASS recovery row dpi="<<dpi<<" default, text bounds, mark, HEVC validation and active lock\n";
}
void nightRows(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    app.monitors={{L"Synthetic display",{0,0,640,360},L"owned-display"}};app.cameras={{L"Synthetic camera",L"owned-camera"}};
    page(RecordingTab);require(!styled(app.nightEnabled)&&!styled(app.nightDuration),"Desktop exposes irrelevant camera night controls.");
    choose(app.mode,static_cast<int>(Mode::Camera));changeLayout(false);
    require(styled(app.nightEnabled)&&!styled(app.nightDuration)&&!styled(app.nightTarget)&&!styled(app.captureCursor)&&GetNextDlgTabItem(app.window,app.skipConfigure,FALSE)==app.nightEnabled,"Camera night opt-in visibility/tab order failed.");
    const auto offHeight=app.panelHeight;
    ownedFocus=app.nightEnabled;SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);windowProc(app.window,WM_COMMAND,NightBox,0);
    require(styled(app.nightDuration)&&styled(app.nightTarget)&&styled(app.nightHint)&&styled(app.nightDetail)&&
        app.panelHeight==offHeight+2*app.scale(36)+app.scale(16)+app.scale(6)+app.scale(16)+app.scale(4),"Night options did not claim exactly their detail row space.");
    require(GetNextDlgTabItem(app.window,app.nightEnabled,FALSE)==app.nightDuration&&GetNextDlgTabItem(app.window,app.nightDuration,FALSE)==app.nightTarget,"Night duration/brightness native tab order failed.");
    require(app.tabMarks[RecordingTab]==1,"Night camera was not marked on its page.");
    checkLayout();checkFocusReachability();
    choose(app.nightDuration,5);windowProc(app.window,WM_COMMAND,MAKEWPARAM(NightDurationBox,CBN_SELCHANGE),0);
    require(!app.nightValidation.empty()&&!IsWindowEnabled(app.record)&&app.tabMarks[RecordingTab]==2,"Invalid night duration lacks a page warning.");
    ownedFocus=app.nightTarget;SendMessageW(app.nightEnabled,BM_SETCHECK,BST_UNCHECKED,0);windowProc(app.window,WM_COMMAND,NightBox,0);
    require(ownedFocus==app.nightEnabled&&!styled(app.nightTarget)&&app.nightValidation.empty(),"Turning night off stranded focus or retained irrelevant validation.");
    ownedFocus=app.nightEnabled;choose(app.mode,static_cast<int>(Mode::Desktop));changeLayout(false);
    require(ownedFocus==app.tabs[RecordingTab]&&!styled(app.nightEnabled),"Changing to Desktop stranded hidden night checkbox focus.");
    checkLayout();std::cout<<"PASS night rows dpi="<<dpi<<" camera-only visibility, focus/tab order, exact height, marks and validation\n";
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
    HiddenWindow owned(1200,900,96);
    const int barW=GetSystemMetricsForDpi(SM_CXVSCROLL,96),barH=GetSystemMetricsForDpi(SM_CYHSCROLL,96);
    const int minimumW=app.scale(StageMinWidth+PanelWidth),panel=app.panelHeight;
    require(panel>app.scale(StageMinHeight)+barH,"Capture page is too short to exercise the scrollbar dependency.");
    // Only the vertical bar's width pushes the content below its minimum width.
    SetWindowPos(app.window,nullptr,0,0,minimumW+barW/2,panel-app.scale(10),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    require((GetWindowLongPtrW(app.window,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL))==(WS_HSCROLL|WS_VSCROLL),"Vertical bar did not induce horizontal bar.");
    // Only the horizontal bar's height pushes the panel past the viewport.
    SetWindowPos(app.window,nullptr,0,0,minimumW-app.scale(20),panel+barH/2,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    require((GetWindowLongPtrW(app.window,GWL_STYLE)&(WS_HSCROLL|WS_VSCROLL))==(WS_HSCROLL|WS_VSCROLL),"Horizontal bar did not induce vertical bar.");
    checkLayout();std::cout<<"PASS mutually dependent native scrollbars\n";
}
void dpiAndRouting(){
    HiddenWindow owned(500,300,96);
    scrollTo(60,70);RECT suggestion{0,0,1000,600};
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
    ownedFocus=app.openFolder;nextDialogFocus=app.stopAfter;message.wParam=L't';dispatchAppMessage(app.window,message);
    require(app.panelTab==RecordingTab && styled(app.stopAfter) && bounds(app.stopAfter).top>=0,"Actual loop did not reveal a mnemonic on another page.");
    page(CaptureTab);
    scrollTo(40,50);const int oldX=app.scrollX,oldY=app.scrollY;
    app.settings.layers={{Source::Desktop,{0,0,1,1}},{Source::Camera,{.2,.3,.3,.3}}};
    const auto layer=layerRect(app.settings.layers[1]);message.hwnd=app.preview;message.message=WM_LBUTTONDOWN;message.wParam=MK_LBUTTON;message.lParam=MAKELPARAM((layer.left+layer.right)/2,(layer.top+layer.bottom)/2);
    dispatchAppMessage(app.window,message);
    require(app.dragging && app.scrollX==oldX && app.scrollY==oldY,"Mouse focus reveal moved the preview after drag start.");
    endLayoutDrag();ownedFocus=nullptr;
    windowProc(app.window,WM_TIMER,1,0);invalidatedWholeParent=false;windowProc(app.window,WM_TIMER,1,0);
    require(!invalidatedWholeParent,"Unchanged status invalidated the parent.");
    std::cout<<"PASS DPI offset scaling/reset, exact keyboard/mouse loop routing, cross-page mnemonic reveal and timer invalidation\n";
}
void originalScenarios(){
    {
        // The default 1040 x 720 window: every page, including a camera
        // collage with night camera, fits without panel scrolling.
        HiddenWindow owned(1024,681,96);
        app.monitors={{L"Synthetic display",{0,0,640,360},L"owned-display"}};app.cameras={{L"Synthetic camera",L"owned-camera"}};
        choose(app.mode,static_cast<int>(Mode::Overlay));changeLayout(false);SendMessageW(app.nightEnabled,BM_SETCHECK,BST_CHECKED,0);configure();updateControls();
        for(int tab:{CaptureTab,RecordingTab,OutputTab}){page(tab);checkLayout();
            require(app.panelHeight<=client().bottom && !(GetWindowLongPtrW(app.window,GWL_STYLE)&WS_VSCROLL),"A page of the default window needs scrolling.");}
        std::cout<<"PASS default window shows every page, including collage and night camera, without scrolling\n";
    }
    {
        HiddenWindow owned(1040,420,96);
        require(app.panelDocked && bounds(app.preview).right<bounds(app.folder).left && bounds(app.finish).bottom<bounds(app.statusText).top,
            "Short-window panel overlaps preview or transport/status.");
        const RECT preview=bounds(app.preview),record=bounds(app.record);
        scrollPanelTo(INT_MAX);require(app.panelScroll>0 && equal(preview,bounds(app.preview)) && equal(record,bounds(app.record)),"Panel scrolling moved the stage.");
        scrollPanelTo(100);RECT suggested{0,0,2080,840};
        windowProc(app.window,WM_DPICHANGED,MAKELONG(192,192),reinterpret_cast<LPARAM>(&suggested));
        require(app.panelDocked && app.panelScroll==200 && app.scrollY==0,"Docked panel offset was lost during DPI scaling.");
        const RECT stage=bounds(app.preview);const int offset=app.panelScroll;
        MSG wheel{};wheel.hwnd=app.encodingQuality;wheel.message=WM_MOUSEWHEEL;wheel.wParam=MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA));
        POINT pointer{app.scale(60),app.scale(100)};ClientToScreen(app.window,&pointer);wheel.lParam=MAKELPARAM(pointer.x,pointer.y);
        const int selection=choice(app.encodingQuality);
        require(scrollWheelMessage(wheel),"Wheel over the stage leaked into a focused closed choice.");
        dispatchAppMessage(app.window,wheel);
        require(app.panelScroll==offset && choice(app.encodingQuality)==selection && equal(stage,bounds(app.preview)),"Wheel over the fixed stage scrolled settings or changed a focused choice.");
        scrollPanelTo(0);pointer={int(client().right)-app.scale(24),app.scale(100)};ClientToScreen(app.window,&pointer);wheel.lParam=MAKELPARAM(pointer.x,pointer.y);
        require(scrollWheelMessage(wheel) && app.panelScroll>0 && equal(stage,bounds(app.preview)),"Wheel over settings failed to scroll only the panel.");
        std::cout<<"PASS short window keeps preview and transport fixed with independent panel scrolling\n";
    }
    for(int outerWidth:{1366,1024}){
        app.dpi=192;syntheticWork={0,0,outerWidth,688};RECT border{};
        require(AdjustWindowRectExForDpi(&border,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,FALSE,0,192)!=FALSE,"Nonclient calculation failed.");
        HiddenWindow owned(outerWidth-(border.right-border.left),688-(border.bottom-border.top),192);
        MINMAXINFO minimum{};windowProc(app.window,WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&minimum));
        require(minimum.ptMinTrackSize.x==outerWidth && minimum.ptMinTrackSize.y==688,"Work-area cap no longer reaches original scenario.");
        for(int tab:{CaptureTab,RecordingTab,OutputTab}){page(tab);checkLayout();checkFocusReachability();}
        std::cout<<"PASS original overlap replay outer="<<outerWidth<<"x688 app_dpi=192 all controls on all pages reachable\n";
    }
    syntheticWork={0,0,10000,10000};
}
void customGeometry(int dpi){
    HiddenWindow owned(MulDiv(830,dpi,96),MulDiv(700,dpi,96),dpi);
    SendMessageW(app.interval,CB_RESETCONTENT,0,0);for(int value:CaptureIntervals)add(app.interval,formatDuration(value));
    SendMessageW(app.videoSize,CB_RESETCONTENT,0,0);add(app.videoSize,L"720p");add(app.videoSize,L"1080p");
    app.customIntervalMs=86399999;app.customWidth=4096;app.customHeight=2160;app.customLimitSeconds=INT_MAX;
    app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=true;app.committedInterval=6;app.committedSize=2;app.committedLimit=6;
    customItems();configure();updateControls();layout();
    for(HWND child:{app.interval,app.videoSize,app.stopAfter})fits(child,30,"Custom value truncates at minimum width/DPI.");
    for(int i=0;i<static_cast<int>(std::size(EncodingModeLabels));++i){choose(app.encodingMode,i);fits(app.encodingMode,30,"Explanatory encoding label truncates at minimum width/DPI.");}
    choose(app.encodingMode,0);configure();updateControls();
    require(app.tabTooltips[RecordingTab].find(formatDuration(int64_t(INT_MAX)*1000))!=std::wstring::npos,"Recording page tooltip lost the exact maximum stop value.");
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
    std::cout<<"PASS exact custom values, encoder explanations and square/portrait/wide canvas dpi="<<dpi<<'\n';
}
void splitRow(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    require(!styled(app.splitEvery)&&!styled(app.segmentLabel)&&!selectedSegment(),"Default split control was visible or enabled.");
    for(int i=1;i<5;++i){choose(app.splitEvery,i);configure();updateControls();
        require(app.tabMarks[RecordingTab]==1 && app.tabTooltips[RecordingTab].find(formatDuration(int64_t(SegmentDurations[i])*1000))!=std::wstring::npos,
            "Recording page hides enabled splitting or its exact duration.");}
    app.customSegmentSeconds=INT_MAX;app.hasCustomSegment=true;app.committedSegment=5;customItems();configure();updateControls();
    page(RecordingTab);fits(app.splitEvery,30,"Custom split duration truncates.");
    require(styled(app.splitEvery)&&styled(app.segmentLabel)&&bounds(app.segmentLabel).right<bounds(app.splitEvery).left&&
        bounds(app.stopAfter).bottom<bounds(app.splitEvery).top&&bounds(app.splitEvery).bottom<bounds(app.startDelay).top,"Split row overlaps adjacent controls.");
    checkLayout();checkFocusReachability();
    ownedFocus=app.splitEvery;page(CaptureTab);require(ownedFocus!=app.splitEvery && !styled(app.splitEvery),"Leaving the page stranded split focus.");
    app.status.state=State::Recording;app.status.frames=321;app.status.elapsed=123;app.status.completedSegments=2;updateControls();paintCheck();
    require(drawnText[3].value.find(L"321 frames")!=std::wstring::npos && drawnText[3].value.find(L"2 parts saved")!=std::wstring::npos,
        "Saved-parts statistics reset overall frame/time totals or omitted the count.");
    app.status.completedSegments=1;paintCheck();require(drawnText[3].value.find(L"1 part saved")!=std::wstring::npos,"Singular saved-parts count was incorrect.");
    page(RecordingTab);require(IsWindowEnabled(app.splitEvery),"An active split length was not editable.");
    app.status.state=State::Finishing;updateControls();require(!IsWindowEnabled(app.splitEvery),"A finishing split length was editable.");
    app.status={};std::cout<<"PASS split row dpi="<<dpi<<" default, exact page marks, row bounds, focus/scroll, cumulative part counts and live length\n";
}
void startDelayRow(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    require(!styled(app.startDelay)&&!styled(app.startDelayLabel)&&!app.tabMarks[RecordingTab]&&!selectedStartDelay(),"Default self-timer adds base clutter or an armed choice.");
    choose(app.startDelay,5);configure();updateControls();
    require(app.tabMarks[RecordingTab]==1&&app.tabTooltips[RecordingTab].find(L"Start delay 5 minutes")!=std::wstring::npos,"Recording page hides the selected self-timer.");
    page(RecordingTab);require(styled(app.startDelay)&&styled(app.startDelayLabel),"Self-timer row is incomplete.");
    fits(app.startDelay,30,"Self-timer choice clips.");
    require(bounds(app.startDelayLabel).right<bounds(app.startDelay).left&&bounds(app.splitEvery).bottom<bounds(app.startDelay).top&&bounds(app.startDelay).bottom<bounds(app.lowDisk).top,"Self-timer row overlaps adjacent options.");
    checkLayout();checkFocusReachability();
    app.encodingValidation=L"Check the MP4 mode";updateControls();
    require(app.tabMarks[OutputTab]==2&&app.tabMarks[RecordingTab]==1,"Self-timer mark and an encoder warning displaced each other.");
    app.encodingValidation.clear();choose(app.startDelay,3);configure();
    for(auto state:{State::Waiting,State::Starting}){
        app.status.state=state;updateControls();wchar_t caption[64]{};GetWindowTextW(app.finish,caption,64);
        require(std::wstring(caption)==L"Cancel s&tart"&&IsWindowEnabled(app.finish)&&!IsWindowEnabled(app.record)&&!IsWindowEnabled(app.pause)&&!IsWindowEnabled(app.startDelay)&&!IsWindowEnabled(app.stopAfter)&&IsWindowEnabled(app.tabs[OutputTab]),
            "Pending start loses cancellation or unlocks recording options.");
        choose(app.startDelay,5);windowProc(app.window,WM_COMMAND,MAKEWPARAM(StartDelayBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.startDelay));
        require(choice(app.startDelay)==3&&app.settings.startDelaySeconds==30,"Forged pending-start selection changes the frozen delay.");
        fits(app.finish,18,"Cancel start caption truncates.");
    }
    app.status.state=State::Recording;updateControls();wchar_t finish[64]{};GetWindowTextW(app.finish,finish,64);require(std::wstring(finish)==L"&Finish","Recording retained the pending-start action caption.");
    app.status={};std::cout<<"PASS self-timer row dpi="<<dpi<<" page mark, bounds, independent warnings, frozen options and Cancel start\n";
}
void cursorRow(int dpi){
    HiddenWindow owned(MulDiv(500,dpi,96),MulDiv(400,dpi,96),dpi);
    require(!styled(app.captureCursor)&&!app.tabMarks[RecordingTab],"Default cursor option was shown on Capture or marked as changed.");
    page(RecordingTab);const int height=app.panelHeight;require(styled(app.captureCursor),"Desktop cursor option hidden on the Recording page.");
    SendMessageW(app.captureCursor,BM_SETCHECK,BST_UNCHECKED,0);windowProc(app.window,WM_COMMAND,MAKEWPARAM(CursorBox,BN_CLICKED),reinterpret_cast<LPARAM>(app.captureCursor));
    require(app.panelHeight==height&&app.tabMarks[RecordingTab]==1&&app.tabTooltips[RecordingTab].find(L"cursor hidden")!=std::wstring::npos,"Cursor selection changed layout height or hid its nondefault state.");
    ownedFocus=app.captureCursor;app.settings.layers=preset(Mode::Camera);updateControls();
    require(!IsWindowEnabled(app.captureCursor)&&!styled(app.captureCursor)&&ownedFocus!=app.captureCursor&&app.tabTooltips[RecordingTab].find(L"cursor hidden")==std::wstring::npos,
        "Camera-only page implies a desktop effect, keeps the checkbox or strands focus.");
    app.settings.layers=preset(Mode::Desktop);updateControls();checkLayout();
    std::cout<<"PASS cursor row dpi="<<dpi<<" page scope, fixed height, mark, source scope and focus\n";
}
}
int main(){
    try{
        std::cout<<std::unitbuf;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        for(int dpi:{96,144,192,288}){scenario(dpi,false);scenario(dpi,true);pages(dpi);compressionRow(dpi);outputRows(dpi);recoveryRow(dpi);nightRows(dpi);customGeometry(dpi);splitRow(dpi);cursorRow(dpi);startDelayRow(dpi);}
        dragChecks();barDependency();dpiAndRouting();originalScenarios();std::cout<<"PASS hidden scrolling, pages, custom geometry and row cases at four DPIs\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FIXTURE FAILURE: "<<e.what()<<'\n';return 1;}
}

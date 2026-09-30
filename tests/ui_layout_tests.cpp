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
    void finish() {}
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
    if(app.advancedExpanded){result.push_back(app.encodingMode);result.push_back(app.stopAfter);}
    for(HWND child:{app.record,app.pause,app.finish,app.folder,app.openFolder,app.reset,app.forward,app.preview})result.push_back(child);
    return result;
}
struct HiddenWindow {
    HiddenWindow(int width,int height,int dpi){
        app.dpi=dpi;app.scrollX=app.scrollY=app.wheelVertical=app.wheelHorizontal=0;
        app.contentWidth=app.contentHeight=0;app.layingOut=app.dragging=app.advancedExpanded=false;app.advancedLimitIndex=app.advancedVisibility=-1;
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
        SendMessageW(app.stopAfter,CB_RESETCONTENT,0,0);for(auto name:RecordingLimitLabels)add(app.stopAfter,name);choose(app.stopAfter,0);
        app.record=button(L"&Record",Record);app.pause=button(L"&Pause",Pause);app.finish=button(L"&Finish",Finish);
        app.folder=button(L"&Change...",Folder);app.openFolder=button(L"&Open folder",OpenFolder);app.reset=button(L"Reset layout",Reset);app.forward=button(L"Bring forward",Forward);
        app.preview=child(L"STATIC",L"Preview",WS_TABSTOP,Preview);app.statusText=child(L"STATIC",L"Ready",SS_LEFT|SS_CENTERIMAGE,210);
        SetWindowLongPtrW(app.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(fixtureWindowProc));
        app.engine=std::make_unique<FixtureEngine>();
        fonts();layout();
    }
    ~HiddenWindow(){
        ownedFocus=ownedCapture=nullptr;SetWindowLongPtrW(app.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(DefWindowProcW));
        DestroyWindow(app.window);app.window=app.preview=nullptr;app.engine.reset();
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
    if(app.advancedExpanded)require(!intersects(bounds(app.encodingMode),bounds(app.stopAfter))&&bounds(app.encodingMode).bottom<bounds(app.preview).top,"Advanced options overlap each other or preview.");
    HDC textDc=GetDC(app.mode);auto oldFont=SelectObject(textDc,app.font);SIZE labelSize{};
    GetTextExtentPoint32W(textDc,SeparateFilesLabel,static_cast<int>(std::wcslen(SeparateFilesLabel)),&labelSize);
    SelectObject(textDc,oldFont);ReleaseDC(app.mode,textDc);
    const RECT modeBounds=bounds(app.mode);
    require(labelSize.cx+GetSystemMetricsForDpi(SM_CXVSCROLL,app.dpi)+app.scale(12)<=modeBounds.right-modeBounds.left,"Separate-files label truncates in the selected source control.");
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
    require(!app.advancedExpanded&&!styledVisible(app.encodingMode)&&!styledVisible(app.stopAfter)&&SendMessageW(app.advanced,BM_GETCHECK,0,0)==BST_UNCHECKED,"Advanced options were not collapsed by default.");
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
    require(app.advancedExpanded&&styledVisible(app.encodingMode)&&styledVisible(app.stopAfter)&&SendMessageW(app.advanced,BM_GETCHECK,0,0)==BST_CHECKED,"Disclosure did not expose accessible checked state/options.");
    require(app.contentHeight==collapsedHeight+app.scale(68),"Expanded options failed to claim their own layout row.");
    require(GetNextDlgTabItem(app.window,app.refresh,FALSE)==app.encodingMode&&GetNextDlgTabItem(app.window,app.encodingMode,FALSE)==app.stopAfter,"Expanded native tab order skipped advanced options.");
    checkLayout();checkFocusReachability();scrollTo(INT_MAX,INT_MAX);ownedFocus=app.stopAfter;
    windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(!app.advancedExpanded&&ownedFocus==app.advanced&&!styledVisible(app.labels[6])&&!styledVisible(app.labels[7]),"Collapsing stranded keyboard focus or labels.");
    require(app.contentHeight==collapsedHeight&&GetNextDlgTabItem(app.window,app.refresh,FALSE)==app.record,"Collapsed row retained blank height or hidden tab stops.");
    checkLayout();checkFocusReachability();
    app.status.state=State::Recording;updateControls();windowProc(app.window,WM_COMMAND,AdvancedToggle,0);
    require(app.advancedExpanded&&IsWindowEnabled(app.advanced)&&!IsWindowEnabled(app.stopAfter)&&!IsWindowEnabled(app.encodingMode),"Recording froze disclosure or allowed advanced edits.");
    app.status={};updateControls();
    std::cout<<"PASS Advanced disclosure dpi="<<dpi<<" default, finite summaries, expansion, focus transfer, native tab order, scroll clamp, active lock\n";
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
    invalidatedWholeParent=false;windowProc(app.window,WM_TIMER,1,0);
    require(invalidatedWholeParent,"Timer invalidates only an unscrolled footer.");
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
}
int main(){
    try{
        std::cout<<std::unitbuf;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        for(int dpi:{96,144,192,288}){scenario(dpi,false);scenario(dpi,true);advancedDisclosure(dpi);}
        dragChecks();barDependency();dpiAndRouting();originalScenarios();std::cout<<"PASS 18 hidden scrolling and disclosure cases\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FIXTURE FAILURE: "<<e.what()<<'\n';return 1;}
}

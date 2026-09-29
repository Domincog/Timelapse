#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <sstream>

using namespace lapse;
namespace {
constexpr COLORREF Ink = RGB(25, 38, 45), Muted = RGB(88, 106, 113), Accent = RGB(0, 116, 113);
constexpr COLORREF Background = RGB(247, 249, 250), Canvas = RGB(21, 28, 34);
enum Id { ModeBox = 100, IntervalBox, QualityBox, MonitorBox, CameraBox, Refresh, Record, Pause, Finish, Folder, OpenFolder, Reset, Forward, Preview };
struct App {
    HWND window{}, preview{}, statusText{}, tooltip{};
    HWND mode{}, interval{}, quality{}, monitor{}, camera{}, refresh{}, record{}, pause{}, finish{}, folder{}, openFolder{}, reset{}, forward{};
    HWND labels[5]{};
    HFONT font{}, titleFont{}, smallFont{};
    HBRUSH background = CreateSolidBrush(Background);
    int dpi = 96, selected = -1, modeIndex = 0;
    bool dragging = false, resizing = false, closeWhenDone = false, inspectUI = false;
    POINT dragStart{};
    Rect dragRect{};
    RECT videoRect{};
    Settings settings;
    Status status;
    std::vector<Monitor> monitors;
    std::vector<CameraDevice> cameras;
    std::unique_ptr<Engine> engine;
    std::wstring preferences;
    int scale(int value) const { return MulDiv(value, dpi, 96); }
    bool active() const { return status.state != State::Idle; }
    ~App() { DeleteObject(font); DeleteObject(titleFont); DeleteObject(smallFont); DeleteObject(background); }
} app;

RECT workArea(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT result{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &result, 0);
    return result;
}
RECT fitWindow(RECT rect, const RECT& work) {
    const LONG width = std::min(rect.right - rect.left, work.right - work.left);
    const LONG height = std::min(rect.bottom - rect.top, work.bottom - work.top);
    const LONG left = std::clamp(rect.left, work.left, work.right - width);
    const LONG top = std::clamp(rect.top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
}
Rect resizeLayer(Rect rect, double dx, double dy) {
    // Keep the top-left corner anchored while resizing against canvas edges.
    rect = constrain(rect);
    rect.w = std::clamp(rect.w + dx, 0.1, std::max(0.1, 1.0 - rect.x));
    rect.h = std::clamp(rect.h + dy, 0.1, std::max(0.1, 1.0 - rect.y));
    return rect;
}
std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) return L"";
    std::wstring value(path); CoTaskMemFree(path); return value;
}
void text(HDC dc, std::wstring value, RECT rect, COLORREF color, HFONT font, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &rect, flags); SelectObject(dc, old);
}
std::wstring timeText(double seconds, bool hours) {
    auto n = static_cast<uint64_t>(std::max(0.0, seconds)); wchar_t value[80];
    if (hours) swprintf_s(value,L"%02llu:%02llu:%02llu",n/3600,n/60%60,n%60);
    else swprintf_s(value,L"%02llu:%02llu",n/60,n%60);
    return value;
}
HWND control(LPCWSTR cls, LPCWSTR name, DWORD style, int id) {
    HWND w = CreateWindowExW(0, cls, name, WS_CHILD | WS_VISIBLE | style, 0,0,10,10, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE); return w;
}
void add(HWND box, const std::wstring& value) { SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str())); }
int choice(HWND box) { return static_cast<int>(SendMessageW(box,CB_GETCURSEL,0,0)); }
void choose(HWND box, int i) { SendMessageW(box, CB_SETCURSEL, i, 0); }
bool hasSource(Source source) { for (auto& l : app.settings.layers) if (l.source == source) return true; return false; }

void configure() {
    static const int intervals[] = {1,2,5,10,30,60};
    app.settings.interval = intervals[std::clamp(choice(app.interval),0,5)];
    app.settings.width = choice(app.quality) == 1 ? 1920 : 1280;
    app.settings.height = choice(app.quality) == 1 ? 1080 : 720;
    int m = choice(app.monitor), c = choice(app.camera);
    if (m >= 0 && m < static_cast<int>(app.monitors.size())) app.settings.monitor = app.monitors[m].bounds;
    app.settings.cameraId = c >= 0 && c < static_cast<int>(app.cameras.size()) ? app.cameras[c].id : L"";
    app.settings.preview = !IsIconic(app.window);
    if (app.engine) app.engine->configure(app.settings);
}
void refreshSources() {
    const auto oldCamera = app.settings.cameraId;
    const int oldMonitor = choice(app.monitor);
    app.monitors = enumerateMonitors();
    std::wstring error;
    app.cameras = enumerateCameras(error);
    SendMessageW(app.monitor,CB_RESETCONTENT,0,0); SendMessageW(app.camera,CB_RESETCONTENT,0,0);
    for (auto& m : app.monitors) add(app.monitor,m.name);
    for (auto& c : app.cameras) add(app.camera,c.name);
    if (app.cameras.empty()) add(app.camera, error.empty() ? L"No camera connected" : L"Camera list unavailable");
    choose(app.monitor,std::clamp(oldMonitor,0,std::max(0,static_cast<int>(app.monitors.size())-1)));
    int cameraIndex = 0;
    for (size_t i=0;i<app.cameras.size();++i) if (app.cameras[i].id == oldCamera) cameraIndex = static_cast<int>(i);
    choose(app.camera,cameraIndex);
    configure();
}
void updateControls() {
    const bool idle = !app.active();
    for (auto control : {app.mode,app.interval,app.quality,app.refresh,app.folder}) EnableWindow(control,idle);
    EnableWindow(app.monitor,idle && hasSource(Source::Desktop));
    EnableWindow(app.camera,idle && hasSource(Source::Camera) && !app.cameras.empty());
    EnableWindow(app.record,idle && (!hasSource(Source::Camera) || !app.cameras.empty()) && (!hasSource(Source::Desktop) || !app.monitors.empty()));
    EnableWindow(app.pause,app.status.state==State::Recording || app.status.state==State::Paused);
    EnableWindow(app.finish,app.status.state==State::Starting || app.status.state==State::Recording || app.status.state==State::Paused);
    SetWindowTextW(app.pause,app.status.state==State::Paused ? L"&Resume" : L"&Pause");
    bool collage = app.settings.layers.size() > 1;
    EnableWindow(app.reset,collage); EnableWindow(app.forward,collage && app.selected>=0);
}
void changeLayout(bool reset) {
    app.modeIndex = choice(app.mode);
    if (reset || app.modeIndex != static_cast<int>(Mode::Custom) || app.settings.layers.size() < 2) app.settings.layers = preset(static_cast<Mode>(app.modeIndex));
    app.selected = -1; configure(); if(app.engine && !app.active())app.engine->refreshSources(); updateControls(); InvalidateRect(app.preview,nullptr,FALSE);
}
void layout() {
    RECT r; GetClientRect(app.window,&r);
    if (IsIconic(app.window)) return;
    const int pad=app.scale(26), gap=app.scale(14), width=std::max(1,static_cast<int>(r.right)-2*pad);
    const int row1=app.scale(86), row2=app.scale(143), ch=app.scale(30), label=app.scale(65);
    const int sourceW=width*39/100, intervalW=width*28/100, qualityW=width-sourceW-intervalW-2*gap;
    auto move=[&](HWND w,int x,int y,int cx,int cy){MoveWindow(w,x,y,cx,cy,TRUE);};
    move(app.labels[0],pad,label,sourceW,app.scale(20)); move(app.mode,pad,row1,sourceW,app.scale(230));
    move(app.labels[1],pad+sourceW+gap,label,intervalW,app.scale(20)); move(app.interval,pad+sourceW+gap,row1,intervalW,app.scale(220));
    move(app.labels[2],pad+sourceW+intervalW+2*gap,label,qualityW,app.scale(20)); move(app.quality,pad+sourceW+intervalW+2*gap,row1,qualityW,app.scale(140));
    const int refreshW=app.scale(92), half=(width-refreshW-2*gap)/2;
    move(app.labels[3],pad,app.scale(121),half,app.scale(20)); move(app.monitor,pad,row2,half,app.scale(220));
    move(app.labels[4],pad+half+gap,app.scale(121),half,app.scale(20)); move(app.camera,pad+half+gap,row2,half,app.scale(220));
    move(app.refresh,r.right-pad-refreshW,row2,refreshW,ch);
    // The preview yields height before recording controls do. This also fits
    // smaller work areas at high DPI without letting the footer overlap it.
    const int previewTop=app.scale(190), previewH=std::max(0,static_cast<int>(r.bottom)-previewTop-app.scale(191));
    move(app.preview,pad,previewTop,width,previewH);
    move(app.reset,r.right-pad-app.scale(240),previewTop+previewH+app.scale(10),app.scale(113),app.scale(27));
    move(app.forward,r.right-pad-app.scale(120),previewTop+previewH+app.scale(10),app.scale(120),app.scale(27));
    move(app.folder,r.right-pad-app.scale(92),r.bottom-app.scale(92),app.scale(92),app.scale(26));
    move(app.statusText,pad,r.bottom-app.scale(124),width,app.scale(25));
    move(app.record,pad,r.bottom-app.scale(54),app.scale(150),app.scale(34));
    move(app.pause,pad+app.scale(160),r.bottom-app.scale(54),app.scale(106),app.scale(34));
    move(app.finish,pad+app.scale(276),r.bottom-app.scale(54),app.scale(106),app.scale(34));
    move(app.openFolder,r.right-pad-app.scale(133),r.bottom-app.scale(54),app.scale(133),app.scale(34));
    InvalidateRect(app.window,nullptr,TRUE);
}
void fonts() {
    DeleteObject(app.font); DeleteObject(app.titleFont); DeleteObject(app.smallFont);
    app.font = CreateFontW(-app.scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    app.titleFont = CreateFontW(-app.scale(25),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    app.smallFont = CreateFontW(-app.scale(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    EnumChildWindows(app.window,[](HWND w,LPARAM p)->BOOL { SendMessageW(w,WM_SETFONT,p,TRUE); return TRUE; },reinterpret_cast<LPARAM>(app.font));
}
void selectFolder() {
    IFileOpenDialog* dialog=nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return;
    dialog->SetOptions(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
    dialog->SetTitle(L"Choose where timelapses are saved");
    IShellItem* current=nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(app.settings.folder.c_str(),nullptr,IID_PPV_ARGS(&current)))) { dialog->SetFolder(current); current->Release(); }
    if (SUCCEEDED(dialog->Show(app.window))) {
        IShellItem* item=nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path=nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) { app.settings.folder=path; CoTaskMemFree(path); configure(); }
            item->Release();
        }
    }
    dialog->Release(); InvalidateRect(app.window,nullptr,FALSE);
}
void preferences(bool save) {
    const auto path=app.preferences.c_str();
    if (!save) {
        wchar_t folder[32768]; GetPrivateProfileStringW(L"Settings",L"Folder",app.settings.folder.c_str(),folder,32768,path); app.settings.folder=folder;
        choose(app.interval,std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Settings",L"Interval",2,path)),0,5));
        choose(app.quality,std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Settings",L"Quality",0,path)),0,1));
        // Launch on desktop: opening the app never silently turns on a camera.
        choose(app.mode,0);
    } else {
        std::error_code ec; std::filesystem::create_directories(std::filesystem::path(app.preferences).parent_path(),ec);
        WritePrivateProfileStringW(L"Settings",L"Folder",app.settings.folder.c_str(),path);
        WritePrivateProfileStringW(L"Settings",L"Interval",std::to_wstring(choice(app.interval)).c_str(),path);
        WritePrivateProfileStringW(L"Settings",L"Quality",std::to_wstring(choice(app.quality)).c_str(),path);
    }
}

RECT layerRect(const Layer& l) {
    int width=app.videoRect.right-app.videoRect.left, height=app.videoRect.bottom-app.videoRect.top;
    return {app.videoRect.left+static_cast<LONG>(l.rect.x*width),app.videoRect.top+static_cast<LONG>(l.rect.y*height),app.videoRect.left+static_cast<LONG>((l.rect.x+l.rect.w)*width),app.videoRect.top+static_cast<LONG>((l.rect.y+l.rect.h)*height)};
}
void customized() {
    app.modeIndex=static_cast<int>(Mode::Custom); choose(app.mode,app.modeIndex);
    configure(); updateControls(); InvalidateRect(app.preview,nullptr,FALSE);
}
LRESULT CALLBACK previewProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(w,&ps); RECT r; GetClientRect(w,&r);
        HDC mem=CreateCompatibleDC(dc); HBITMAP bmp=CreateCompatibleBitmap(dc,std::max(1L,r.right),std::max(1L,r.bottom)); auto old=SelectObject(mem,bmp);
        HBRUSH bg=CreateSolidBrush(Canvas); FillRect(mem,&r,bg); DeleteObject(bg);
        int width=r.right, height=width*9/16;
        if (height>r.bottom) {height=r.bottom;width=height*16/9;}
        app.videoRect={(r.right-width)/2,(r.bottom-height)/2,(r.right+width)/2,(r.bottom+height)/2};
        if(app.status.preview && app.status.preview->valid()) {
            auto& f=*app.status.preview; BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=f.width; info.bmiHeader.biHeight=-f.height; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
            SetStretchBltMode(mem,HALFTONE); SetBrushOrgEx(mem,0,0,nullptr);
            StretchDIBits(mem,app.videoRect.left,app.videoRect.top,width,height,0,0,f.width,f.height,f.pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
        } else {
            RECT hint=app.videoRect; InflateRect(&hint,-app.scale(30),-app.scale(24));
            text(mem,hasSource(Source::Camera)?L"Waiting for camera preview...":L"Preparing desktop preview...",hint,RGB(192,205,212),app.font,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        if(app.settings.layers.size()>1) for(size_t i=0;i<app.settings.layers.size();++i) {
            RECT layer=layerRect(app.settings.layers[i]);
            bool selected=static_cast<int>(i)==app.selected;
            HPEN pen=CreatePen(PS_SOLID,app.scale(selected?2:1),selected?RGB(83,229,205):RGB(160,180,185));
            auto p=SelectObject(mem,pen), b=SelectObject(mem,GetStockObject(HOLLOW_BRUSH));
            Rectangle(mem,layer.left,layer.top,layer.right,layer.bottom);SelectObject(mem,p);SelectObject(mem,b);DeleteObject(pen);
            RECT tag={layer.left+app.scale(5),layer.top+app.scale(4),layer.left+app.scale(82),layer.top+app.scale(25)};
            HBRUSH label=CreateSolidBrush(RGB(23,41,47));FillRect(mem,&tag,label);DeleteObject(label);
            text(mem,app.settings.layers[i].source==Source::Desktop?L"Desktop":L"Camera",tag,RGB(239,248,250),app.smallFont,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if(selected) {RECT handle={layer.right-app.scale(12),layer.bottom-app.scale(12),layer.right,layer.bottom};HBRUSH h=CreateSolidBrush(RGB(83,229,205));FillRect(mem,&handle,h);DeleteObject(h);}
        }
        BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);EndPaint(w,&ps);return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(w); if(app.settings.layers.size()<2) return 0;
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};app.selected=-1;
        for(int i=static_cast<int>(app.settings.layers.size())-1;i>=0;--i) {RECT r=layerRect(app.settings.layers[i]);if(PtInRect(&r,p)){app.selected=i;app.dragStart=p;app.dragRect=app.settings.layers[i].rect;app.resizing=p.x>=r.right-app.scale(18)&&p.y>=r.bottom-app.scale(18);app.dragging=true;SetCapture(w);break;}}
        updateControls();InvalidateRect(w,nullptr,FALSE);return 0;
    }
    case WM_MOUSEMOVE:
        if(app.dragging && app.selected>=0) {
            double dx=double(GET_X_LPARAM(lp)-app.dragStart.x)/std::max(1L,app.videoRect.right-app.videoRect.left),dy=double(GET_Y_LPARAM(lp)-app.dragStart.y)/std::max(1L,app.videoRect.bottom-app.videoRect.top);
            Rect r=app.dragRect;if(app.resizing){r=resizeLayer(r,dx,dy);}else{r.x+=dx;r.y+=dy;}
            app.settings.layers[app.selected].rect=constrain(r);customized();
        } return 0;
    case WM_LBUTTONUP: app.dragging=false;ReleaseCapture();return 0;
    case WM_CAPTURECHANGED: app.dragging=false;return 0;
    case WM_KEYDOWN:
        if(app.settings.layers.size()>1) {
            if(wp==VK_SPACE){app.selected=(app.selected+1)%static_cast<int>(app.settings.layers.size());updateControls();InvalidateRect(w,nullptr,FALSE);return 0;}
            if(app.selected>=0 && (wp==VK_LEFT||wp==VK_RIGHT||wp==VK_UP||wp==VK_DOWN)) {
                Rect& r=app.settings.layers[app.selected].rect;double dx=wp==VK_LEFT?-.01:wp==VK_RIGHT?.01:0,dy=wp==VK_UP?-.01:wp==VK_DOWN?.01:0;
                if(GetKeyState(VK_SHIFT)&0x8000){r=resizeLayer(r,dx,dy);}else{r.x+=dx;r.y+=dy;}r=constrain(r);customized();return 0;
            }
        } break;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

LRESULT CALLBACK windowProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        app.window=w;app.dpi=static_cast<int>(GetDpiForWindow(w));fonts();
        // Native label mnemonics and accessibility names follow sibling order.
        auto combo=[&](int index,const wchar_t* label,int id){
            app.labels[index]=control(L"STATIC",label,0,200+index);
            return control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,id);
        };
        app.mode=combo(0,L"&Source",ModeBox);for(auto s:{L"Desktop",L"Camera",L"Desktop + camera",L"Side by side",L"Custom collage"})add(app.mode,s);
        app.interval=combo(1,L"Capture &every",IntervalBox);for(auto s:{L"1 second",L"2 seconds",L"5 seconds",L"10 seconds",L"30 seconds",L"60 seconds"})add(app.interval,s);
        app.quality=combo(2,L"Video si&ze",QualityBox);add(app.quality,L"720p · smaller file");add(app.quality,L"1080p · more detail");
        app.monitor=combo(3,L"&Display",MonitorBox);app.camera=combo(4,L"Ca&mera",CameraBox);
        auto button=[&](const wchar_t* s,int id){return control(L"BUTTON",s,WS_TABSTOP|BS_PUSHBUTTON,id);};
        app.refresh=button(L"Re&fresh",Refresh);app.record=button(L"●  &Record",Record);app.pause=button(L"&Pause",Pause);app.finish=button(L"&Finish",Finish);app.folder=button(L"&Change...",Folder);app.openFolder=button(L"Open &folder",OpenFolder);app.reset=button(L"Reset layout",Reset);app.forward=button(L"Bring forward",Forward);
        app.preview=control(L"LapsePreview",L"Collage preview. Space selects a layer. Arrow keys move it. Shift and arrow keys resize it.",WS_TABSTOP,Preview);
        app.statusText=control(L"STATIC",app.status.message.c_str(),SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,210);
        SendMessageW(app.statusText,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
        app.tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,w,nullptr,nullptr,nullptr);
        TOOLINFOW tip{sizeof(tip)};tip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tip.hwnd=w;tip.uId=reinterpret_cast<UINT_PTR>(app.statusText);tip.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));SendMessageW(app.tooltip,TTM_SETMAXTIPWIDTH,0,app.scale(520));
        preferences(false);refreshSources();changeLayout(true);
        app.engine=std::make_unique<Engine>();configure();updateControls();layout();SetTimer(w,1,200,nullptr);
        // Keep the controls out of desktop recordings on Windows 10 2004 and later.
        if(!app.inspectUI)SetWindowDisplayAffinity(w,0x00000011);return 0;
    }
    case WM_SIZE: if(app.mode){layout();configure();} return 0;
    case WM_DPICHANGED: {
        app.dpi=HIWORD(wp);fonts();auto suggested=reinterpret_cast<RECT*>(lp);
        const RECT r=fitWindow(*suggested,workArea(MonitorFromRect(suggested,MONITOR_DEFAULTTONEAREST)));
        SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info=reinterpret_cast<MINMAXINFO*>(lp);
        const RECT work=workArea(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST));
        info->ptMinTrackSize={std::min<LONG>(app.scale(830),work.right-work.left),std::min<LONG>(app.scale(630),work.bottom-work.top)};
        return 0;
    }
    case WM_TIMER: {
        auto before=app.status.preview;auto oldMessage=app.status.message;bool oldError=app.status.error;app.status=app.engine->status();updateControls();
        if(oldMessage!=app.status.message)SetWindowTextW(app.statusText,app.status.message.c_str());
        if(oldError!=app.status.error)InvalidateRect(app.statusText,nullptr,TRUE);
        if(before!=app.status.preview)InvalidateRect(app.preview,nullptr,FALSE);
        RECT r;GetClientRect(w,&r);r.top=r.bottom-app.scale(181);InvalidateRect(w,&r,FALSE);
        if(app.closeWhenDone && !app.active()) {
            app.closeWhenDone=false;
            if(app.status.error) {
                // Keep the recovery path visible if saving failed while the
                // user was waiting for Finish and close.
                EnableWindow(w,TRUE);SetForegroundWindow(w);
                MessageBoxW(w,app.status.message.c_str(),L"Timelapse could not finish normally",MB_OK|MB_ICONERROR);
            } else DestroyWindow(w);
        }return 0;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wp),code=HIWORD(wp);
        if(code==CBN_SELCHANGE){if(id==ModeBox)changeLayout(false);else configure();InvalidateRect(w,nullptr,FALSE);return 0;}
        switch(id) {
        case Refresh:refreshSources();app.engine->refreshSources();updateControls();break;
        case Record:configure();app.engine->record();app.status=app.engine->status();updateControls();break;
        case Pause:app.engine->pause();break;
        case Finish:app.engine->finish();break;
        case Folder:selectFolder();break;
        case OpenFolder: {std::error_code ec;std::filesystem::create_directories(app.settings.folder,ec);if(ec)MessageBoxW(w,L"The save folder is unavailable. Choose another folder.",L"Timelapse",MB_OK|MB_ICONERROR);else ShellExecuteW(w,L"open",app.settings.folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;}
        case Reset:app.settings.layers=preset(app.modeIndex==static_cast<int>(Mode::SideBySide)?Mode::SideBySide:Mode::Overlay);choose(app.mode,app.modeIndex==static_cast<int>(Mode::SideBySide)?3:2);changeLayout(false);break;
        case Forward:if(app.selected>=0){auto layer=app.settings.layers[app.selected];app.settings.layers.erase(app.settings.layers.begin()+app.selected);app.settings.layers.push_back(layer);app.selected=static_cast<int>(app.settings.layers.size())-1;customized();}break;
        }return 0;
    }
    case WM_NOTIFY:
        if(reinterpret_cast<NMHDR*>(lp)->code==TTN_GETDISPINFOW){reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText=const_cast<LPWSTR>(app.status.message.c_str());return 0;}break;
    case WM_CTLCOLORSTATIC: SetBkColor(reinterpret_cast<HDC>(wp),Background);SetTextColor(reinterpret_cast<HDC>(wp),reinterpret_cast<HWND>(lp)==app.statusText && app.status.error?RGB(174,53,44):Muted);return reinterpret_cast<LRESULT>(app.background);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;HDC dc=BeginPaint(w,&ps);RECT r;GetClientRect(w,&r);FillRect(dc,&ps.rcPaint,app.background);
        int p=app.scale(26);RECT title={p,app.scale(14),r.right-p,app.scale(53)};text(dc,L"Timelapse",title,Ink,app.titleFont);
        RECT badge={r.right-app.scale(280),app.scale(20),r.right-p,app.scale(49)};
        std::wstring state=app.status.state==State::Recording?L"●  RECORDING":app.status.state==State::Paused?L"Ⅱ  PAUSED":app.status.state==State::Starting?L"PREPARING":app.status.state==State::Finishing?L"SAVING":L"DESKTOP + CAMERA";
        text(dc,state,badge,app.status.state==State::Recording?Accent:Muted,app.smallFont,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        RECT hint={p,r.bottom-app.scale(180),r.right-p-app.scale(251),r.bottom-app.scale(150)};
        text(dc,app.settings.layers.size()>1?L"Drag a layer to move it. Pull its corner to resize.":L"Preview · your recording is saved at 30 fps",hint,Muted,app.smallFont);
        RECT stats={p,r.bottom-app.scale(147),r.right-p,r.bottom-app.scale(123)};
        std::wstring detail;
        if(app.active() || app.status.frames)detail=std::to_wstring(app.status.frames)+L" frames  ·  "+timeText(double(app.status.frames)/30,false)+L" video  ·  "+timeText(app.status.elapsed,true)+L" recording";
        else {wchar_t buf[140];swprintf_s(buf,L"Every %d second%s  ·  1 hour becomes %.0f seconds of video",app.settings.interval,app.settings.interval==1?L"":L"s",120.0/app.settings.interval);detail=buf;}
        text(dc,detail,stats,Ink,app.font);
        RECT path={p,r.bottom-app.scale(92),r.right-p-app.scale(106),r.bottom-app.scale(66)};text(dc,L"Save to: "+app.settings.folder,path,Muted,app.smallFont,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_PATH_ELLIPSIS);
        EndPaint(w,&ps);return 0;
    }
    case WM_CLOSE:
        app.status=app.engine->status();
        if(app.active()){if(MessageBoxW(w,L"Finish the current video and close Timelapse?",L"Finish recording",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK)return 0;app.closeWhenDone=true;app.engine->finish();EnableWindow(w,FALSE);return 0;}
        DestroyWindow(w);return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        // Confirmed session shutdown reaches WM_DESTROY and joins the engine,
        // giving the encoder a chance to finalize before Windows terminates us.
        if(wp)DestroyWindow(w);
        return 0;
    case WM_DESTROY:KillTimer(w,1);preferences(true);app.engine.reset();PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR commandLine,int show) {
    const int cameraHostResult = runCameraHost(commandLine);
    if (cameraHostResult >= 0) return cameraHostResult;
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    HRESULT media=MFStartup(MF_VERSION,MFSTARTUP_LITE);
    if(FAILED(com)||FAILED(media)){MessageBoxW(nullptr,L"Windows media components are unavailable. On Windows N, install the Media Feature Pack, then try again.",L"Timelapse",MB_OK|MB_ICONERROR);if(SUCCEEDED(com))CoUninitialize();return 1;}
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    app.settings.folder=(std::filesystem::path(knownFolder(FOLDERID_Videos))/L"Timelapse").wstring();
    app.preferences=(std::filesystem::path(knownFolder(FOLDERID_LocalAppData))/L"Timelapse"/L"settings.ini").wstring();
    int argumentCount=0;
    if(auto arguments=CommandLineToArgvW(GetCommandLineW(),&argumentCount)) {
        for(int i=1;i<argumentCount;++i)if(std::wcscmp(arguments[i],L"--inspect-ui")==0)app.inspectUI=true;
        LocalFree(arguments);
    }
    WNDCLASSEXW preview{sizeof(preview)};preview.lpfnWndProc=previewProc;preview.hInstance=instance;preview.hCursor=LoadCursorW(nullptr,IDC_ARROW);preview.lpszClassName=L"LapsePreview";RegisterClassExW(&preview);
    WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=windowProc;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);cls.hbrBackground=app.background;cls.lpszClassName=L"TimelapseWindow";RegisterClassExW(&cls);
    app.dpi=static_cast<int>(GetDpiForSystem());
    POINT cursor{};GetCursorPos(&cursor);
    const RECT work=workArea(MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY));
    const int margin=app.scale(16);
    const int width=std::min(app.scale(920),std::max(1,static_cast<int>(work.right-work.left)-2*margin));
    const int height=std::min(app.scale(740),std::max(1,static_cast<int>(work.bottom-work.top)-2*margin));
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"Timelapse",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,instance,nullptr);
    if(!window){MFShutdown();CoUninitialize();return 1;}
    ShowWindow(window,show);UpdateWindow(window);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    MFShutdown();CoUninitialize();return static_cast<int>(msg.wParam);
}

#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <new>
#include <sstream>
#include <fstream>
#include <climits>
#include <system_error>

using namespace lapse;
namespace {
constexpr COLORREF Ink = RGB(25, 38, 45), Muted = RGB(88, 106, 113), Accent = RGB(0, 116, 113);
constexpr COLORREF Background = RGB(247, 249, 250), Canvas = RGB(21, 28, 34);
enum Id { ModeBox = 100, IntervalBox, SizeBox, EncodingQualityBox, MonitorBox, CameraBox, Refresh, Record, Pause, Finish, Folder, OpenFolder, Reset, Forward, Preview };
struct App {
    HWND window{}, preview{}, statusText{}, tooltip{};
    HWND mode{}, interval{}, videoSize{}, encodingQuality{}, monitor{}, camera{}, refresh{}, record{}, pause{}, finish{}, folder{}, openFolder{}, reset{}, forward{};
    HWND labels[6]{};
    HFONT font{}, titleFont{}, smallFont{};
    HBRUSH background = CreateSolidBrush(Background);
    int dpi = 96, selected = -1, modeIndex = 0;
    Mode collagePreset = Mode::Overlay;
    bool dragging = false, resizing = false, closeWhenDone = false, inspectUI = false;
    bool layingOut = false;
    bool startupComplete = false;
    int contentWidth = 0, contentHeight = 0, scrollX = 0, scrollY = 0;
    int wheelVertical = 0, wheelHorizontal = 0;
    POINT dragStart{};
    Rect dragRect{};
    RECT videoRect{};
    Settings settings;
    Status status;
    std::vector<Monitor> monitors;
    std::vector<CameraDevice> cameras;
    std::unique_ptr<Engine> engine;
    std::wstring preferences, selectedMonitorId;
    int scale(int value) const { return MulDiv(value, dpi, 96); }
    bool active() const { return status.state != State::Idle; }
    ~App() { DeleteObject(font); DeleteObject(titleFont); DeleteObject(smallFont); DeleteObject(background); }
} app;

struct MediaRuntime {
    MediaRuntime() = default;
    MediaRuntime(const MediaRuntime&) = delete;
    MediaRuntime& operator=(const MediaRuntime&) = delete;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT media = SUCCEEDED(com) ? MFStartup(MF_VERSION, MFSTARTUP_LITE) : E_UNEXPECTED;
    ~MediaRuntime() {
        // Join the worker before releasing its media runtime, including when
        // startup or message retrieval exits without receiving WM_DESTROY.
        app.engine.reset();
        if (SUCCEEDED(media)) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

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
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, &CoTaskMemFree);
    return SUCCEEDED(result) && owned ? std::wstring(owned.get()) : L"";
}
bool defaultPaths(std::wstring& folder, std::wstring& preferencesPath) {
    const std::filesystem::path videos(knownFolder(FOLDERID_Videos));
    const std::filesystem::path localData(knownFolder(FOLDERID_LocalAppData));
    // Missing directories may be created later, but unresolved Windows profile
    // locations must never become paths relative to the launch directory.
    if (!videos.is_absolute() || !localData.is_absolute()) return false;
    auto newFolder = (videos / L"Timelapse").wstring();
    auto newPreferences = (localData / L"Timelapse" / L"settings.ini").wstring();
    folder.swap(newFolder); preferencesPath.swap(newPreferences);
    return true;
}
void text(HDC dc, std::wstring value, RECT rect, COLORREF color, HFONT font, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &rect, flags | DT_NOPREFIX); SelectObject(dc, old);
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
bool sameSourceId(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;
}
bool hasRequiredSources() {
    const int m=choice(app.monitor), c=choice(app.camera);
    return (!hasSource(Source::Desktop) || (m>=0 && m<static_cast<int>(app.monitors.size()))) &&
        (!hasSource(Source::Camera) || (c>=0 && c<static_cast<int>(app.cameras.size())));
}

void configure() {
    static const int intervals[] = {1,2,5,10,30,60};
    app.settings.interval = intervals[std::clamp(choice(app.interval),0,5)];
    app.settings.width = choice(app.videoSize) == 1 ? 1920 : 1280;
    app.settings.height = choice(app.videoSize) == 1 ? 1080 : 720;
    app.settings.encodingQuality = static_cast<EncodingQuality>(std::clamp(choice(app.encodingQuality),0,2));
    int m = choice(app.monitor), c = choice(app.camera);
    if (m >= 0 && m < static_cast<int>(app.monitors.size())) {
        app.settings.monitor = app.monitors[m].bounds;
        app.selectedMonitorId = app.monitors[m].id;
    } else app.settings.monitor = {};
    app.settings.monitorId = app.selectedMonitorId;
    if (c >= 0 && c < static_cast<int>(app.cameras.size())) app.settings.cameraId = app.cameras[c].id;
    app.settings.preview = !IsIconic(app.window);
    if (app.engine) app.engine->configure(app.settings);
}
void refreshSources() {
    const auto oldCamera = app.settings.cameraId;
    const auto oldMonitor = app.selectedMonitorId;
    app.monitors = enumerateMonitors();
    std::wstring error;
    app.cameras = enumerateCameras(error);
    SendMessageW(app.monitor,CB_RESETCONTENT,0,0); SendMessageW(app.camera,CB_RESETCONTENT,0,0);
    for (auto& m : app.monitors) add(app.monitor,m.name);
    for (auto& c : app.cameras) add(app.camera,c.name);
    int monitorIndex = oldMonitor.empty() && !app.monitors.empty() ? 0 : -1;
    for (size_t i=0;i<app.monitors.size();++i)
        if (sameSourceId(app.monitors[i].id,oldMonitor)) monitorIndex = static_cast<int>(i);
    if (monitorIndex < 0) {
        monitorIndex = static_cast<int>(app.monitors.size());
        add(app.monitor,oldMonitor.empty() ? L"No display available" : L"Selected display unavailable");
    }
    choose(app.monitor,monitorIndex);
    int cameraIndex = oldCamera.empty() && !app.cameras.empty() ? 0 : -1;
    for (size_t i=0;i<app.cameras.size();++i)
        if (sameSourceId(app.cameras[i].id,oldCamera)) cameraIndex = static_cast<int>(i);
    if (cameraIndex < 0) {
        cameraIndex = static_cast<int>(app.cameras.size());
        add(app.camera,!error.empty() ? L"Camera list unavailable"
            : oldCamera.empty() ? L"No camera connected" : L"Selected camera unavailable");
    }
    choose(app.camera,cameraIndex);
    configure();
}
void updateControls() {
    const bool idle = !app.active();
    for (auto control : {app.mode,app.interval,app.videoSize,app.encodingQuality,app.refresh,app.folder}) EnableWindow(control,idle);
    EnableWindow(app.monitor,idle && hasSource(Source::Desktop));
    EnableWindow(app.camera,idle && hasSource(Source::Camera) && !app.cameras.empty());
    EnableWindow(app.record,idle && hasRequiredSources());
    EnableWindow(app.pause,app.status.state==State::Recording || app.status.state==State::Paused);
    EnableWindow(app.finish,app.status.state==State::Starting || app.status.state==State::Recording || app.status.state==State::Paused);
    SetWindowTextW(app.pause,app.status.state==State::Paused ? L"&Resume" : L"&Pause");
    bool collage = app.settings.layers.size() > 1;
    EnableWindow(app.reset,collage); EnableWindow(app.forward,collage && app.selected>=0);
}
void changeLayout(bool reset) {
    app.modeIndex = choice(app.mode);
    if (reset || app.modeIndex != static_cast<int>(Mode::Custom) || app.settings.layers.size() < 2) {
        app.settings.layers = preset(static_cast<Mode>(app.modeIndex));
        // Custom edits retain their preset; a newly seeded collage starts over.
        app.collagePreset = app.modeIndex == static_cast<int>(Mode::SideBySide) ? Mode::SideBySide : Mode::Overlay;
    }
    app.selected = -1; configure(); if(app.engine && !app.active())app.engine->refreshSources(); updateControls(); InvalidateRect(app.preview,nullptr,FALSE);
}
void endLayoutDrag() {
    // Keep the last applied edit, but never reuse a pointer origin after the
    // preview has moved or changed size. Do not release another window's capture.
    app.dragging = false;
    if (GetCapture() == app.preview) ReleaseCapture();
}
RECT previewVideoRect(const RECT& client) {
    int width=client.right, height=width*9/16;
    if(height>client.bottom){height=client.bottom;width=height*16/9;}
    return {(client.right-width)/2,(client.bottom-height)/2,
            (client.right+width)/2,(client.bottom+height)/2};
}
void layout() {
    if (app.layingOut || !app.preview || IsIconic(app.window)) return;
    app.layingOut = true;
    endLayoutDrag();
    RECT r; GetClientRect(app.window,&r);
    // Solve both scroll bars together: either bar can make the other necessary.
    const auto style=GetWindowLongPtrW(app.window,GWL_STYLE);
    const int barW=GetSystemMetricsForDpi(SM_CXVSCROLL,app.dpi);
    const int barH=GetSystemMetricsForDpi(SM_CYHSCROLL,app.dpi);
    const int availableW=r.right+((style&WS_VSCROLL)?barW:0);
    const int availableH=r.bottom+((style&WS_HSCROLL)?barH:0);
    const int minimumW=app.scale(830);
    const int minimumH=app.scale(190)+app.scale(160)+app.scale(191);
    bool horizontal=false, vertical=false;
    for(int i=0;i<3;++i) {
        horizontal=availableW-(vertical?barW:0)<minimumW;
        vertical=availableH-(horizontal?barH:0)<minimumH;
    }
    ShowScrollBar(app.window,SB_HORZ,horizontal);
    ShowScrollBar(app.window,SB_VERT,vertical);
    GetClientRect(app.window,&r);
    const int viewportW=std::max(1L,r.right), viewportH=std::max(1L,r.bottom);
    app.contentWidth=std::max(viewportW,minimumW);
    app.contentHeight=std::max(viewportH,minimumH);
    app.scrollX=std::clamp(app.scrollX,0,app.contentWidth-viewportW);
    app.scrollY=std::clamp(app.scrollY,0,app.contentHeight-viewportH);
    SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};
    scroll.nMax=app.contentWidth-1;scroll.nPage=viewportW;scroll.nPos=app.scrollX;
    SetScrollInfo(app.window,SB_HORZ,&scroll,TRUE);
    scroll.nMax=app.contentHeight-1;scroll.nPage=viewportH;scroll.nPos=app.scrollY;
    SetScrollInfo(app.window,SB_VERT,&scroll,TRUE);
    r.right=app.contentWidth;r.bottom=app.contentHeight;
    const int pad=app.scale(26), gap=app.scale(14), width=std::max(1,static_cast<int>(r.right)-2*pad);
    const int row1=app.scale(86), row2=app.scale(143), ch=app.scale(30), label=app.scale(65);
    // Keep resolution and compression separate without consuming preview height.
    const int available=width-3*gap, sourceW=available*32/100, intervalW=available*22/100, sizeW=available*18/100;
    const int qualityW=available-sourceW-intervalW-sizeW;
    const int intervalX=pad+sourceW+gap, sizeX=intervalX+intervalW+gap, qualityX=sizeX+sizeW+gap;
    auto move=[&](HWND w,int x,int y,int cx,int cy){MoveWindow(w,x-app.scrollX,y-app.scrollY,cx,cy,TRUE);};
    move(app.labels[0],pad,label,sourceW,app.scale(20)); move(app.mode,pad,row1,sourceW,app.scale(230));
    move(app.labels[1],intervalX,label,intervalW,app.scale(20)); move(app.interval,intervalX,row1,intervalW,app.scale(220));
    move(app.labels[2],sizeX,label,sizeW,app.scale(20)); move(app.videoSize,sizeX,row1,sizeW,app.scale(140));
    move(app.labels[3],qualityX,label,qualityW,app.scale(20)); move(app.encodingQuality,qualityX,row1,qualityW,app.scale(160));
    const int refreshW=app.scale(92), half=(width-refreshW-2*gap)/2;
    move(app.labels[4],pad,app.scale(121),half,app.scale(20)); move(app.monitor,pad,row2,half,app.scale(220));
    move(app.labels[5],pad+half+gap,app.scale(121),half,app.scale(20)); move(app.camera,pad+half+gap,row2,half,app.scale(220));
    move(app.refresh,r.right-pad-refreshW,row2,refreshW,ch);
    // The logical canvas retains a usable preview when the viewport is small.
    const int previewTop=app.scale(190), previewH=static_cast<int>(r.bottom)-previewTop-app.scale(191);
    move(app.preview,pad,previewTop,width,previewH);
    RECT previewClient{};GetClientRect(app.preview,&previewClient);
    app.videoRect=previewVideoRect(previewClient);
    move(app.reset,r.right-pad-app.scale(240),previewTop+previewH+app.scale(10),app.scale(113),app.scale(27));
    move(app.forward,r.right-pad-app.scale(120),previewTop+previewH+app.scale(10),app.scale(120),app.scale(27));
    move(app.folder,r.right-pad-app.scale(92),r.bottom-app.scale(92),app.scale(92),app.scale(26));
    move(app.statusText,pad,r.bottom-app.scale(124),width,app.scale(25));
    move(app.record,pad,r.bottom-app.scale(54),app.scale(150),app.scale(34));
    move(app.pause,pad+app.scale(160),r.bottom-app.scale(54),app.scale(106),app.scale(34));
    move(app.finish,pad+app.scale(276),r.bottom-app.scale(54),app.scale(106),app.scale(34));
    move(app.openFolder,r.right-pad-app.scale(133),r.bottom-app.scale(54),app.scale(133),app.scale(34));
    app.layingOut = false;
    InvalidateRect(app.window,nullptr,TRUE);
}
void scrollTo(int x,int y) {
    RECT r;GetClientRect(app.window,&r);
    x=std::clamp(x,0,std::max(0,app.contentWidth-static_cast<int>(r.right)));
    y=std::clamp(y,0,std::max(0,app.contentHeight-static_cast<int>(r.bottom)));
    if(x==app.scrollX && y==app.scrollY)return;
    app.scrollX=x;app.scrollY=y;layout();
}
void scrollBar(int bar,int command) {
    SCROLLINFO info{sizeof(info),SIF_ALL};
    if(!GetScrollInfo(app.window,bar,&info))return;
    int position=info.nPos;
    const int page=std::max(1,static_cast<int>(info.nPage)-app.scale(24));
    switch(command) {
    case SB_LINEUP:position-=app.scale(24);break;
    case SB_LINEDOWN:position+=app.scale(24);break;
    case SB_PAGEUP:position-=page;break;
    case SB_PAGEDOWN:position+=page;break;
    case SB_TOP:position=0;break;
    case SB_BOTTOM:position=info.nMax;break;
    case SB_THUMBTRACK:case SB_THUMBPOSITION:position=info.nTrackPos;break;
    default:return;
    }
    scrollTo(bar==SB_HORZ?position:app.scrollX,bar==SB_VERT?position:app.scrollY);
}
void revealFocusedControl() {
    HWND child=GetFocus();
    if(!child || !IsChild(app.window,child))return;
    while(GetParent(child)!=app.window)child=GetParent(child);
    RECT target{}, viewport{};GetWindowRect(child,&target);GetClientRect(app.window,&viewport);
    MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&target),2);
    OffsetRect(&target,app.scrollX,app.scrollY);
    // Keep a margin only when it fits; a nearly viewport-sized button must
    // remain fully visible instead of being clipped to make room for padding.
    const int marginX=std::clamp(static_cast<int>((viewport.right-(target.right-target.left))/2),0,app.scale(8));
    const int marginY=std::clamp(static_cast<int>((viewport.bottom-(target.bottom-target.top))/2),0,app.scale(8));
    InflateRect(&target,marginX,marginY);
    auto reveal=[](int position,int size,int first,int last) {
        if(first<position || last-first>size)return first;
        return last>position+size?last-size:position;
    };
    scrollTo(reveal(app.scrollX,viewport.right,target.left,target.right),
             reveal(app.scrollY,viewport.bottom,target.top,target.bottom));
}
bool scrollWheelMessage(const MSG& message) {
    if(message.message!=WM_MOUSEWHEEL && message.message!=WM_MOUSEHWHEEL)return false;
    if(GET_KEYSTATE_WPARAM(message.wParam)&MK_CONTROL)return false;
    if(message.hwnd!=app.window && !IsChild(app.window,message.hwnd))return false;
    const bool horizontal=message.message==WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(message.wParam)&MK_SHIFT);
    RECT viewport;GetClientRect(app.window,&viewport);
    if((horizontal?app.contentWidth:app.contentHeight)<=(horizontal?viewport.right:viewport.bottom))return false;
    // Open lists own their wheel input. Closed lists must not change recording
    // settings when the user's wheel gesture is scrolling the surrounding page.
    for(HWND box:{app.mode,app.interval,app.videoSize,app.encodingQuality,app.monitor,app.camera})
        if(SendMessageW(box,CB_GETDROPPEDSTATE,0,0))return false;
    SendMessageW(app.window,message.message,message.wParam,message.lParam);
    return true;
}
void fonts() {
    DeleteObject(app.font); DeleteObject(app.titleFont); DeleteObject(app.smallFont);
    app.font = CreateFontW(-app.scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    app.titleFont = CreateFontW(-app.scale(25),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    app.smallFont = CreateFontW(-app.scale(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    EnumChildWindows(app.window,[](HWND w,LPARAM p)->BOOL { SendMessageW(w,WM_SETFONT,p,TRUE); return TRUE; },reinterpret_cast<LPARAM>(app.font));
}
void selectFolder() {
    try {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return;
        if (FAILED(dialog->SetOptions(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR))) return;
        dialog->SetTitle(L"Choose where timelapses are saved");
        {
            Microsoft::WRL::ComPtr<IShellItem> current;
            if (SUCCEEDED(SHCreateItemFromParsingName(app.settings.folder.c_str(),nullptr,IID_PPV_ARGS(&current)))) dialog->SetFolder(current.Get());
        }
        if (SUCCEEDED(dialog->Show(app.window))) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path=nullptr;
                const HRESULT result=item->GetDisplayName(SIGDN_FILESYSPATH,&path);
                const std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> owned(path,&CoTaskMemFree);
                if (SUCCEEDED(result) && owned) { app.settings.folder=owned.get(); configure(); }
            }
        }
        InvalidateRect(app.window,nullptr,FALSE);
    } catch (const std::bad_alloc&) {
        // Contain allocation failure before returning through the Win32 callback.
        MessageBoxW(app.window,L"Timelapse could not change the save folder. Close other applications, then try again.",L"Timelapse",MB_OK|MB_ICONERROR);
    }
}
bool savePreferences() {
    try {
        std::error_code ec;
        const auto preferencesPath=fileIOPath(app.preferences);
        const std::filesystem::path target(preferencesPath);
        std::filesystem::create_directories(target.parent_path(),ec);
        if(ec)return false;
        // The wide profile API creates ANSI files unless a UTF-16 BOM exists.
        // Preserve existing keys/comments while upgrading the old ANSI format.
        std::ifstream previous(target,std::ios::binary);
        std::string bytes;
        if(previous) {
            previous.seekg(0,std::ios::end);
            const auto length=previous.tellg();
            if(length<0 || length>INT_MAX)return false;
            previous.seekg(0,std::ios::beg);
            bytes.resize(static_cast<size_t>(length));
            if(!previous || (!bytes.empty() && !previous.read(bytes.data(),static_cast<std::streamsize>(bytes.size()))))return false;
        } else if(std::filesystem::exists(target,ec) || ec)return false;
        previous.close();
        if(bytes.size()<2 || static_cast<unsigned char>(bytes[0])!=0xff || static_cast<unsigned char>(bytes[1])!=0xfe) {
            std::wstring unicode(1,L'\ufeff');
            if(!bytes.empty()) {
                const int count=MultiByteToWideChar(CP_ACP,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
                if(!count)return false;
                unicode.resize(static_cast<size_t>(count)+1);
                if(!MultiByteToWideChar(CP_ACP,0,bytes.data(),static_cast<int>(bytes.size()),unicode.data()+1,count))return false;
            }
            bytes.assign(reinterpret_cast<const char*>(unicode.data()),unicode.size()*sizeof(wchar_t));
        }
        if(bytes.size()>MAXDWORD)return false;
        const auto temporary=preferencesPath+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L".tmp";
        const auto interval=std::to_wstring(choice(app.interval)), quality=std::to_wstring(choice(app.videoSize));
        const auto encodingQuality=std::to_wstring(choice(app.encodingQuality));
        struct TemporaryFile {
            const wchar_t* path;
            HANDLE file=INVALID_HANDLE_VALUE;
            bool created=false;
            bool close() {
                const auto handle=file; file=INVALID_HANDLE_VALUE;
                return handle==INVALID_HANDLE_VALUE || CloseHandle(handle)!=FALSE;
            }
            ~TemporaryFile() { close(); if(created)DeleteFileW(path); }
        } pending{temporary.c_str()};
        pending.file=CreateFileW(pending.path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(pending.file==INVALID_HANDLE_VALUE)return false;
        pending.created=true;
        DWORD written=0;
        bool saved=WriteFile(pending.file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) && written==bytes.size();
        if(!pending.close())saved=false;
        if(saved) saved=WritePrivateProfileStringW(L"Settings",L"Folder",app.settings.folder.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"Interval",interval.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"Quality",quality.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"EncodingQuality",encodingQuality.c_str(),pending.path);
        // This cache-flush form returns zero even when successful.
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,pending.path);
        if(!saved)return false;
        pending.file=CreateFileW(pending.path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(pending.file==INVALID_HANDLE_VALUE)return false;
        saved=FlushFileBuffers(pending.file)!=FALSE;
        if(!pending.close())saved=false;
        if(!saved || !MoveFileExW(pending.path,preferencesPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
        pending.created=false;
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,preferencesPath.c_str());
        return true;
    } catch (...) {
        // Saving is best effort during WM_DESTROY; never unwind a window callback.
        return false;
    }
}
void preferences(bool save) {
    if (!save) {
        const auto preferencesPath=fileIOPath(app.preferences);
        const auto path=preferencesPath.c_str();
        wchar_t folder[32768]; GetPrivateProfileStringW(L"Settings",L"Folder",app.settings.folder.c_str(),folder,32768,path); app.settings.folder=folder;
        choose(app.interval,std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Settings",L"Interval",2,path)),0,5));
        choose(app.videoSize,std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Settings",L"Quality",0,path)),0,1));
        choose(app.encodingQuality,std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Settings",L"EncodingQuality",1,path)),0,2));
        // Launch on desktop: opening the app never silently turns on a camera.
        choose(app.mode,0);
    } else if(!savePreferences()) {
        OutputDebugStringW(L"Timelapse could not save preferences.\n");
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
        app.videoRect=previewVideoRect(r);
        const int width=app.videoRect.right-app.videoRect.left, height=app.videoRect.bottom-app.videoRect.top;
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
            const bool arrow=wp==VK_LEFT||wp==VK_RIGHT||wp==VK_UP||wp==VK_DOWN;
            if(app.dragging && (wp==VK_SPACE || (app.selected>=0 && arrow))) {
                // Commit the pointer gesture before changing selection or
                // geometry, so later mouse movement cannot reuse dragRect.
                app.dragging=false;
                if(GetCapture()==w)ReleaseCapture();
            }
            if(wp==VK_SPACE){app.selected=(app.selected+1)%static_cast<int>(app.settings.layers.size());updateControls();InvalidateRect(w,nullptr,FALSE);return 0;}
            if(app.selected>=0 && arrow) {
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
        app.startupComplete=false;
        try {
        app.window=w;app.dpi=static_cast<int>(GetDpiForWindow(w));fonts();
        bool controlsReady=true;
        auto requiredControl=[&](LPCWSTR cls,LPCWSTR name,DWORD style,int id) {
            HWND child=control(cls,name,style,id);
            if(!child)controlsReady=false;
            return child;
        };
        // Native label mnemonics and accessibility names follow sibling order.
        auto combo=[&](int index,const wchar_t* label,int id){
            app.labels[index]=requiredControl(L"STATIC",label,0,200+index);
            return requiredControl(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,id);
        };
        app.mode=combo(0,L"&Source",ModeBox);for(auto s:{L"Desktop",L"Camera",L"Desktop + camera",L"Side by side",L"Custom collage"})add(app.mode,s);
        app.interval=combo(1,L"Capture &every",IntervalBox);for(auto s:{L"1 second",L"2 seconds",L"5 seconds",L"10 seconds",L"30 seconds",L"60 seconds"})add(app.interval,s);
        app.videoSize=combo(2,L"Video si&ze",SizeBox);add(app.videoSize,L"720p");add(app.videoSize,L"1080p");
        app.encodingQuality=combo(3,L"Video &quality",EncodingQualityBox);for(auto s:{L"Smaller file",L"Balanced",L"More detail"})add(app.encodingQuality,s);
        app.monitor=combo(4,L"&Display",MonitorBox);app.camera=combo(5,L"Ca&mera",CameraBox);
        auto button=[&](const wchar_t* s,int id){return requiredControl(L"BUTTON",s,WS_TABSTOP|BS_PUSHBUTTON,id);};
        app.refresh=button(L"Re&fresh",Refresh);app.record=button(L"●  &Record",Record);app.pause=button(L"&Pause",Pause);app.finish=button(L"&Finish",Finish);app.folder=button(L"&Change...",Folder);app.openFolder=button(L"&Open folder",OpenFolder);app.reset=button(L"Reset layout",Reset);app.forward=button(L"Bring forward",Forward);
        app.preview=requiredControl(L"LapsePreview",L"Collage preview. Space selects a layer. Arrow keys move it. Shift and arrow keys resize it.",WS_TABSTOP,Preview);
        app.statusText=requiredControl(L"STATIC",app.status.message.c_str(),SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,210);
        if(!controlsReady) {
            app.mode=nullptr;
            OutputDebugStringW(L"Timelapse could not create its required controls.\n");
            return -1;
        }
        SendMessageW(app.statusText,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
        app.tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,w,nullptr,nullptr,nullptr);
        TOOLINFOW tip{sizeof(tip)};tip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tip.hwnd=w;tip.uId=reinterpret_cast<UINT_PTR>(app.statusText);tip.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));SendMessageW(app.tooltip,TTM_SETMAXTIPWIDTH,0,app.scale(520));
        preferences(false);refreshSources();changeLayout(true);
        try {
            app.engine=std::make_unique<Engine>();
        } catch (const std::system_error&) {
            app.mode=nullptr;
            OutputDebugStringW(L"Timelapse could not start its recording worker.\n");
            return -1;
        }
        configure();updateControls();layout();
        if(!SetTimer(w,1,200,nullptr)) {
            app.mode=nullptr;
            OutputDebugStringW(L"Timelapse could not start its status updates.\n");
            return -1;
        }
        // Keep the controls out of desktop recordings on Windows 10 2004 and later.
        if(!app.inspectUI)SetWindowDisplayAffinity(w,0x00000011);
        app.startupComplete=true;return 0;
        } catch (const std::bad_alloc&) {
            // Ignore resize work while Windows tears down the failed window.
            app.mode=nullptr;
            OutputDebugStringW(L"Timelapse could not initialize its window: insufficient memory.\n");
            return -1;
        }
    }
    case WM_SIZE:
        if(app.mode && !app.layingOut) {
            if(wp!=SIZE_MINIMIZED){layout();revealFocusedControl();}
            configure();
        }
        return 0;
    case WM_VSCROLL:case WM_HSCROLL:
        if(lp)break;
        scrollBar(msg==WM_VSCROLL?SB_VERT:SB_HORZ,LOWORD(wp));return 0;
    case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL: {
        if(GET_KEYSTATE_WPARAM(wp)&MK_CONTROL)break;
        const bool horizontal=msg==WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp)&MK_SHIFT);
        RECT viewport;GetClientRect(w,&viewport);
        const int size=horizontal?viewport.right:viewport.bottom;
        if((horizontal?app.contentWidth:app.contentHeight)<=size)break;
        UINT lines=3;
        SystemParametersInfoW(horizontal?SPI_GETWHEELSCROLLCHARS:SPI_GETWHEELSCROLLLINES,0,&lines,0);
        int& remainder=horizontal?app.wheelHorizontal:app.wheelVertical;
        // Horizontal wheel positive means right; vertical wheel positive means up.
        remainder+=GET_WHEEL_DELTA_WPARAM(wp)*(msg==WM_MOUSEHWHEEL?1:-1);
        const int steps=remainder/WHEEL_DELTA;remainder%=WHEEL_DELTA;
        const int page=std::max(1,size-app.scale(24));
        const int amount=lines==WHEEL_PAGESCROLL?page:
            static_cast<int>(std::min<uint64_t>(uint64_t(lines)*app.scale(20),page));
        scrollTo(app.scrollX+(horizontal?steps*amount:0),app.scrollY+(horizontal?0:steps*amount));
        return 0;
    }
    case WM_DPICHANGED: {
        endLayoutDrag();
        const int dpi=HIWORD(wp);
        app.scrollX=MulDiv(app.scrollX,dpi,app.dpi);app.scrollY=MulDiv(app.scrollY,dpi,app.dpi);
        app.dpi=dpi;fonts();auto suggested=reinterpret_cast<RECT*>(lp);
        const RECT r=fitWindow(*suggested,workArea(MonitorFromRect(suggested,MONITOR_DEFAULTTONEAREST)));
        SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);layout();revealFocusedControl();return 0;
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
        // The footer shares the scrolled logical canvas with its controls.
        InvalidateRect(w,nullptr,FALSE);
        if(app.closeWhenDone && !app.active()) {
            app.closeWhenDone=false;
            if(app.status.recordingFailed) {
                // Keep the recovery path visible if saving failed while the
                // user was waiting for Finish and close.
                EnableWindow(w,TRUE);SetForegroundWindow(w);
                MessageBoxW(w,app.status.message.c_str(),L"Timelapse could not finish normally",MB_OK|MB_ICONERROR);
            } else DestroyWindow(w);
        }return 0;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wp),code=HIWORD(wp);
        if(code==CBN_SELCHANGE){
            if(id==ModeBox)changeLayout(false);
            else {
                if(id==MonitorBox || id==CameraBox) {
                    const HWND box=id==MonitorBox ? app.monitor : app.camera;
                    const int count=static_cast<int>(id==MonitorBox ? app.monitors.size() : app.cameras.size());
                    if(choice(box)>=0 && choice(box)<count)
                        while(SendMessageW(box,CB_GETCOUNT,0,0)>count) SendMessageW(box,CB_DELETESTRING,count,0);
                }
                configure();updateControls();
            }
            InvalidateRect(w,nullptr,FALSE);return 0;
        }
        switch(id) {
        case Refresh:refreshSources();app.engine->refreshSources();updateControls();break;
        case Record:configure();if(hasRequiredSources())app.engine->record();app.status=app.engine->status();updateControls();break;
        case Pause:app.engine->setPaused(app.status.state!=State::Paused);break;
        case Finish:app.engine->finish();break;
        case Folder:selectFolder();break;
        case OpenFolder: {
            std::error_code ec;
            std::filesystem::create_directories(fileIOPath(app.settings.folder),ec);
            if(ec) MessageBoxW(w,L"The save folder is unavailable. Choose another folder.",L"Timelapse",MB_OK|MB_ICONERROR);
            else if(reinterpret_cast<INT_PTR>(ShellExecuteW(w,L"open",app.settings.folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32) {
                const auto message=L"Windows could not open the save folder. Try opening it in File Explorer:\n\n"+app.settings.folder;
                MessageBoxW(w,message.c_str(),L"Timelapse",MB_OK|MB_ICONERROR);
            }
            break;
        }
        case Reset:choose(app.mode,static_cast<int>(app.collagePreset));changeLayout(false);break;
        case Forward:if(app.selected>=0){auto layer=app.settings.layers[app.selected];app.settings.layers.erase(app.settings.layers.begin()+app.selected);app.settings.layers.push_back(layer);app.selected=static_cast<int>(app.settings.layers.size())-1;customized();}break;
        }return 0;
    }
    case WM_NOTIFY:
        if(reinterpret_cast<NMHDR*>(lp)->code==TTN_GETDISPINFOW){reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText=const_cast<LPWSTR>(app.status.message.c_str());return 0;}break;
    case WM_CTLCOLORSTATIC: SetBkColor(reinterpret_cast<HDC>(wp),Background);SetTextColor(reinterpret_cast<HDC>(wp),reinterpret_cast<HWND>(lp)==app.statusText && app.status.error?RGB(174,53,44):Muted);return reinterpret_cast<LRESULT>(app.background);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;HDC dc=BeginPaint(w,&ps);RECT r;GetClientRect(w,&r);FillRect(dc,&ps.rcPaint,app.background);
        SetViewportOrgEx(dc,-app.scrollX,-app.scrollY,nullptr);
        r.right=app.contentWidth;r.bottom=app.contentHeight;
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
    case WM_DESTROY:KillTimer(w,1);if(app.startupComplete)preferences(true);app.startupComplete=false;app.engine.reset();PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
void dispatchAppMessage(HWND window,MSG& msg) {
    const HWND focused=GetFocus();
    if(!scrollWheelMessage(msg) && !IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    // Reveal keyboard targets once. Mouse focus must not relocate a clicked
    // control or the preview after a drag captures its starting point.
    const bool keyboard=msg.message>=WM_KEYFIRST && msg.message<=WM_KEYLAST;
    if(keyboard && (focused!=GetFocus() || msg.message==WM_SYSCHAR ||
       (msg.message==WM_KEYDOWN && msg.wParam==VK_TAB)))revealFocusedControl();
}
int runGui(HINSTANCE instance,int show,bool& windowCreationFailed) {
    const MediaRuntime runtime;
    if(FAILED(runtime.com) || FAILED(runtime.media)) {
        const bool comFailed=FAILED(runtime.com);
        const HRESULT result=comFailed ? runtime.com : runtime.media;
        wchar_t error[256]{};
        swprintf_s(error,L"Windows %s initialization failed (0x%08X).%s",
            comFailed ? L"COM" : L"media",static_cast<unsigned>(result),
            !comFailed && result==E_NOTIMPL ? L" On Windows N, install the Media Feature Pack, then try again." : L"");
        MessageBoxW(nullptr,error,L"Timelapse",MB_OK|MB_ICONERROR);return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    if(!defaultPaths(app.settings.folder,app.preferences)) {
        MessageBoxW(nullptr,L"Windows could not locate your Videos or application data folder. Check that your Windows user profile is available, then reopen Timelapse.",L"Timelapse",MB_OK|MB_ICONERROR);
        return 1;
    }
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
    if(!window){windowCreationFailed=true;return 1;}
    ShowWindow(window,show);UpdateWindow(window);
    MSG msg{};
    BOOL messageResult=0;
    while((messageResult=GetMessageW(&msg,nullptr,0,0))>0) {
        dispatchAppMessage(window,msg);
    }
    if(messageResult<0) {
        const DWORD error=GetLastError();
        const auto diagnostic=L"Timelapse message loop failed: "+errorText(HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE))+L"\n";
        OutputDebugStringW(diagnostic.c_str());return 1;
    }
    return static_cast<int>(msg.wParam);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR commandLine,int show) {
    const int cameraHostResult = runCameraHost(commandLine);
    if (cameraHostResult >= 0) return cameraHostResult;
    try {
        bool windowCreationFailed=false;
        const int result=runGui(instance,show,windowCreationFailed);
        if(windowCreationFailed) {
            // The GUI runtime has already released the worker, media and COM.
            MessageBoxW(nullptr,L"Timelapse could not open its window. Close other applications, then try again.",L"Timelapse",MB_OK|MB_ICONERROR);
        }
        return result;
    } catch (const std::bad_alloc&) {
        // The GUI runtime has already released the worker, media and COM.
        MessageBoxW(nullptr,L"Timelapse ran out of memory. Close other applications, then try again.",L"Timelapse",MB_OK|MB_ICONERROR);
        return 1;
    }
}

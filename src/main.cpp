#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include "config.h"
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
enum Id { ModeBox = 100, IntervalBox, SizeBox, EncodingQualityBox, MonitorBox, CameraBox, Refresh, Record, Pause, Finish, Folder, OpenFolder, Reset, Forward, Preview, EncodingModeBox, AdvancedToggle, StopAfterBox, LowDiskBox, NightBox, NightDurationBox, NightTargetBox, NightHint, NightDetail };
constexpr int RecordingLimits[] = {0,900,3600,14400,28800,86400};
constexpr int CaptureIntervals[] = {1000,2000,5000,10000,30000,60000};
constexpr const wchar_t* RecordingLimitLabels[] = {L"Never",L"15 minutes",L"1 hour",L"4 hours",L"8 hours",L"24 hours"};
constexpr const wchar_t* RecordingLimitShortLabels[] = {L"Never",L"15 min",L"1 hour",L"4 hours",L"8 hours",L"24 hours"};
constexpr int NightDurations[] = {0,1000,2000,5000,10000,30000};
constexpr const wchar_t* NightDurationLabels[] = {L"Auto",L"1 second",L"2 seconds",L"5 seconds",L"10 seconds",L"30 seconds"};
constexpr int NightTargets[] = {64,96,128};
constexpr const wchar_t* EncodingModeLabels[] = {L"Compatible H.264 (default)",L"Efficient H.264 (bitrate target)",L"Hardware H.264 (CPU offload)",L"Hardware HEVC (HEVC player)",L"Quality H.264 (detail)"};
constexpr int SeparateFilesMode = 5;
constexpr wchar_t SeparateFilesLabel[] = L"Desktop + camera (2 files)";
constexpr UINT TrayMessage = WM_APP + 1, ShowExistingMessage = WM_APP + 2;
constexpr UINT TrayShow = 4001, TrayPause = 4002, TrayFinish = 4003, TrayExit = 4004;
constexpr UINT ExitSystemCommand = 0x1000;
constexpr wchar_t InstanceMutexName[] = L"Local\\Timelapse.Application.{DC32D155-1B8D-4880-9902-CE6245D34923}";
constexpr wchar_t SetupMutexName[] = L"Local\\Timelapse.Setup.{DC32D155-1B8D-4880-9902-CE6245D34923}";
struct App {
    HWND window{}, preview{}, statusText{}, tooltip{};
    HWND customDialog{};
    HWND mode{}, interval{}, videoSize{}, encodingQuality{}, encodingMode{}, monitor{}, camera{}, refresh{}, record{}, pause{}, finish{}, folder{}, openFolder{}, reset{}, forward{};
    HWND advanced{}, stopAfter{}, lowDisk{}, nightEnabled{}, nightDuration{}, nightTarget{}, nightHint{}, nightDetail{}, labels[10]{};
    HFONT font{}, titleFont{}, smallFont{};
    HBRUSH background = CreateSolidBrush(Background);
    int dpi = 96, selected = -1, modeIndex = 0;
    Mode collagePreset = Mode::Overlay;
    bool dragging = false, resizing = false, closeWhenDone = false, inspectUI = false;
    bool layingOut = false;
    bool startupComplete = false;
    bool visibleDirty = true, controlsUpdated = false;
    State controlsState = State::Idle;
    bool advancedExpanded = false;
    int advancedLimitIndex = -1, advancedVisibility = -1, advancedNightState = -1, nightVisibility = -1;
    int customIntervalMs=5000, customWidth=1280, customHeight=720, customLimitSeconds=900;
    int committedInterval=2, committedSize=0, committedLimit=0;
    bool hasCustomInterval=false, hasCustomSize=false, hasCustomLimit=false;
    std::wstring advancedCaption, advancedTooltip;
    std::wstring nightValidation, statusCaption, nightHintCaption, nightDetailCaption;
    bool statusCaptionError = false;
    bool hiddenToTray = false, trayRegistered = false, trayNoticeShown = false, trayVersion4 = false;
    UINT taskbarCreated = 0;
    std::wstring trayTooltip;
    State trayState = State::Idle;
    bool trayStateValid = false, trayFailure = false, traySeparate = false;
    HICON trayIcons[5]{};
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
    ~App() { for(auto icon:trayIcons)if(icon)DestroyIcon(icon);DeleteObject(font); DeleteObject(titleFont); DeleteObject(smallFont); DeleteObject(background); }
} app;
void removeTray();

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
        removeTray();
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
int selectedInterval() {
    const int selected=choice(app.interval);
    return app.hasCustomInterval && selected==6 ? app.customIntervalMs : CaptureIntervals[std::clamp(selected,0,5)];
}
int selectedLimit() {
    const int selected=choice(app.stopAfter);
    return app.hasCustomLimit && selected==6 ? app.customLimitSeconds : RecordingLimits[std::clamp(selected,0,5)];
}
std::wstring sizeText(int width,int height) { return std::to_wstring(width)+L" × "+std::to_wstring(height); }
void customItems(HWND box,int presets,bool custom,const std::wstring& value,int selected) {
    while(SendMessageW(box,CB_GETCOUNT,0,0)>presets)SendMessageW(box,CB_DELETESTRING,presets,0);
    if(custom)add(box,value);
    add(box,L"Custom...");choose(box,selected);
}
void customItems() {
    customItems(app.interval,6,app.hasCustomInterval,formatDuration(app.customIntervalMs),app.committedInterval);
    customItems(app.videoSize,2,app.hasCustomSize,sizeText(app.customWidth,app.customHeight),app.committedSize);
    customItems(app.stopAfter,6,app.hasCustomLimit,formatDuration(int64_t(app.customLimitSeconds)*1000,true),app.committedLimit);
}
void normalizeCustomSelections() {
    if(app.hasCustomInterval)for(int i=0;i<6;++i)if(CaptureIntervals[i]==app.customIntervalMs){app.hasCustomInterval=false;app.committedInterval=i;break;}
    if(app.hasCustomSize && ((app.customWidth==1280 && app.customHeight==720) || (app.customWidth==1920 && app.customHeight==1080))){
        app.hasCustomSize=false;app.committedSize=app.customWidth==1280?0:1;
    }
    if(app.hasCustomLimit)for(int i=1;i<6;++i)if(RecordingLimits[i]==app.customLimitSeconds){app.hasCustomLimit=false;app.committedLimit=i;break;}
}
bool hasSource(Source source) { if(app.settings.separateFiles)return true;for (auto& l : app.settings.layers) if (l.source == source) return true; return false; }
bool sameSourceId(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;
}
bool hasRequiredSources() {
    const int m=choice(app.monitor), c=choice(app.camera);
    return (!hasSource(Source::Desktop) || (m>=0 && m<static_cast<int>(app.monitors.size()))) &&
        (!hasSource(Source::Camera) || (c>=0 && c<static_cast<int>(app.cameras.size())));
}
void layout();
int nightRow() { return hasSource(Source::Camera) ? (app.settings.night.enabled?2:1) : 0; }
const std::wstring& statusCaption() {
    return !app.active() && !app.status.error && !app.nightValidation.empty() ? app.nightValidation : app.status.message;
}
bool statusCaptionError() {
    return app.status.error || (!app.active() && !app.nightValidation.empty());
}
void updateStatusText(bool force=false) {
    const auto& caption=statusCaption();const bool error=statusCaptionError();
    if(app.hiddenToTray || IsIconic(app.window)) {
        if(force || caption!=app.statusCaption || error!=app.statusCaptionError)app.visibleDirty=true;
        return;
    }
    if(force || caption!=app.statusCaption){SetWindowTextW(app.statusText,caption.c_str());app.statusCaption=caption;}
    if(error!=app.statusCaptionError){InvalidateRect(app.statusText,nullptr,TRUE);app.statusCaptionError=error;}
}
void updateNightText(bool force=false) {
    if(!app.nightDetail || !app.advancedExpanded || nightRow()!=2)return;
    if(app.hiddenToTray || IsIconic(app.window)){app.visibleDirty=true;return;}
    const std::wstring hint=app.nightValidation.empty()?L"Software frame blending; camera shutter is unchanged. Movement may blur.":app.nightValidation;
    if(force || hint!=app.nightHintCaption){SetWindowTextW(app.nightHint,hint.c_str());app.nightHintCaption=hint;InvalidateRect(app.nightHint,nullptr,TRUE);}
    std::wstring detail=L"Night effect appears during recording; idle preview is unchanged.";
    if(app.status.nightEnabled && app.status.night.samples){
        wchar_t value[240]{};
        swprintf_s(value,L"Last blend: %.1f s · %u camera frames · %.1f× digital gain%s",
            app.status.nightDurationMs/1000.0,app.status.night.samples,app.status.night.appliedGain,
            app.status.night.targetLimited?L" · brightness target limited":L"");
        detail=value;
    } else if(app.status.nightEnabled && app.status.nightWaiting)detail=L"Collecting camera frames for the first full blend...";
    if(force || detail!=app.nightDetailCaption){SetWindowTextW(app.nightDetail,detail.c_str());app.nightDetailCaption=std::move(detail);}
}
void updateAdvanced() {
    if(!app.advanced)return;
    const int selection=selectedLimit();
    const int night=app.settings.night.enabled?(app.nightValidation.empty()?1:2):0;
    if(selection!=app.advancedLimitIndex || night!=app.advancedNightState){
        std::wstring caption=L"&Advanced";
        if(night==2)caption+=L" · check blend";
        else if(night){caption+=L" · night";if(selection)caption+=L", "+(choice(app.stopAfter)<6?std::wstring(RecordingLimitShortLabels[std::clamp(choice(app.stopAfter),0,5)]):formatDuration(int64_t(selection)*1000,true));}
        else if(selection)caption+=L" · stop after "+(choice(app.stopAfter)<6?std::wstring(RecordingLimitLabels[std::clamp(choice(app.stopAfter),0,5)]):formatDuration(int64_t(selection)*1000,true));
        RECT bounds{};GetClientRect(app.advanced,&bounds);
        if(selection && night!=2 && bounds.right>app.scale(40)) {
            HDC dc=GetDC(app.advanced);if(dc){const auto previous=SelectObject(dc,app.font);SIZE size{};
                GetTextExtentPoint32W(dc,caption.c_str(),static_cast<int>(caption.size()),&size);
                SelectObject(dc,previous);ReleaseDC(app.advanced,dc);
                if(size.cx+app.scale(18)>bounds.right)caption=night?L"&Advanced · night + stop":L"&Advanced · timed stop";
            }
        }
        if(caption!=app.advancedCaption){SetWindowTextW(app.advanced,caption.c_str());app.advancedCaption=caption;}
        app.advancedTooltip=L"Show or hide advanced options. Recording options can be changed before recording. Night mode applies only to camera content.";
        if(selection)app.advancedTooltip+=L" Stop after "+formatDuration(int64_t(selection)*1000)+L" of active recording; pauses and startup do not count.";
        app.advancedLimitIndex=selection;app.advancedNightState=night;
    }
    const int visibleNight=app.advancedExpanded?nightRow():0;
    if(app.advancedVisibility==static_cast<int>(app.advancedExpanded) && app.nightVisibility==visibleNight)return;
    app.advancedVisibility=static_cast<int>(app.advancedExpanded);
    app.nightVisibility=visibleNight;
    SendMessageW(app.advanced,BM_SETCHECK,app.advancedExpanded?BST_CHECKED:BST_UNCHECKED,0);
    const auto visible=[](HWND child,bool show){
        if(child && ((GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0)!=show)ShowWindow(child,show?SW_SHOWNA:SW_HIDE);
    };
    for(HWND child:{app.labels[6],app.encodingMode,app.labels[7],app.stopAfter,app.lowDisk})visible(child,app.advancedExpanded);
    if(!visibleNight && app.nightEnabled && GetFocus()==app.nightEnabled)SetFocus(app.advanced);
    visible(app.nightEnabled,visibleNight!=0);
    for(HWND child:{app.labels[8],app.nightDuration,app.labels[9],app.nightTarget,app.nightHint,app.nightDetail}){
        const HWND focused=GetFocus();
        if(visibleNight!=2 && child && (focused==child || (focused && IsChild(child,focused))))SetFocus(visibleNight?app.nightEnabled:app.advanced);
        visible(child,visibleNight==2);
    }
    updateNightText(true);
}

void configure() {
    app.settings.intervalMs = selectedInterval();
    const bool customSize=app.hasCustomSize && choice(app.videoSize)==2;
    app.settings.width = customSize?app.customWidth:choice(app.videoSize)==1?1920:1280;
    app.settings.height = customSize?app.customHeight:choice(app.videoSize)==1?1080:720;
    app.committedInterval=std::clamp(choice(app.interval),0,app.hasCustomInterval?6:5);
    app.committedSize=std::clamp(choice(app.videoSize),0,app.hasCustomSize?2:1);
    app.committedLimit=std::clamp(choice(app.stopAfter),0,app.hasCustomLimit?6:5);
    app.settings.encodingQuality = static_cast<EncodingQuality>(std::clamp(choice(app.encodingQuality),0,2));
    app.settings.encodingMode = static_cast<EncodingMode>(std::clamp(choice(app.encodingMode),0,4));
    app.settings.recordingLimitSeconds = selectedLimit();
    app.settings.stopOnLowDiskSpace = !app.lowDisk || SendMessageW(app.lowDisk,BM_GETCHECK,0,0)!=BST_UNCHECKED;
    app.settings.night.enabled = app.nightEnabled && SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_CHECKED && hasSource(Source::Camera);
    app.settings.night.durationMs = NightDurations[app.nightDuration?std::clamp(choice(app.nightDuration),0,5):0];
    app.settings.night.targetBrightness = NightTargets[app.nightTarget?std::clamp(choice(app.nightTarget),0,2):1];
    app.nightValidation.clear();
    if(app.settings.night.enabled && app.settings.intervalMs<NightMinDurationMs)
        app.nightValidation=L"Night camera needs a capture interval of at least 1 second. Choose a longer interval or turn off Night camera.";
    else if(app.settings.night.enabled && app.settings.night.durationMs>app.settings.intervalMs)
        app.nightValidation=L"Night blend duration must not exceed Capture every. Choose Auto, a shorter blend, or a longer capture interval.";
    int m = choice(app.monitor), c = choice(app.camera);
    if (m >= 0 && m < static_cast<int>(app.monitors.size())) {
        app.settings.monitor = app.monitors[m].bounds;
        app.selectedMonitorId = app.monitors[m].id;
    } else app.settings.monitor = {};
    app.settings.monitorId = app.selectedMonitorId;
    if (c >= 0 && c < static_cast<int>(app.cameras.size())) app.settings.cameraId = app.cameras[c].id;
    app.settings.preview = !app.hiddenToTray && !IsIconic(app.window);
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
    for (auto control : {app.mode,app.interval,app.videoSize,app.encodingQuality,app.encodingMode,app.stopAfter,app.lowDisk,app.nightEnabled,app.nightDuration,app.nightTarget,app.refresh,app.folder}) EnableWindow(control,idle);
    EnableWindow(app.monitor,idle && hasSource(Source::Desktop));
    EnableWindow(app.camera,idle && hasSource(Source::Camera) && !app.cameras.empty());
    EnableWindow(app.record,idle && hasRequiredSources() && app.nightValidation.empty());
    EnableWindow(app.pause,app.status.state==State::Recording || app.status.state==State::Paused);
    EnableWindow(app.finish,app.status.state==State::Starting || app.status.state==State::Recording || app.status.state==State::Paused);
    if(!app.controlsUpdated || (app.controlsState==State::Paused)!=(app.status.state==State::Paused))
        SetWindowTextW(app.pause,app.status.state==State::Paused ? L"&Resume" : L"&Pause");
    bool collage = !app.settings.separateFiles && app.settings.layers.size() > 1;
    EnableWindow(app.reset,collage); EnableWindow(app.forward,collage && app.selected>=0);
    updateAdvanced();
    updateNightText(!app.controlsUpdated);updateStatusText(!app.controlsUpdated);
    app.controlsState=app.status.state;app.controlsUpdated=true;
}
void invalidateCanvas(RECT rect) {
    OffsetRect(&rect,-app.scrollX,-app.scrollY);InvalidateRect(app.window,&rect,FALSE);
}
void applyStatus(Status value,bool force=false) {
    const bool state=app.status.state!=value.state;
    const bool stats=state || app.status.frames!=value.frames ||
        static_cast<uint64_t>(std::max(0.0,app.status.elapsed))!=static_cast<uint64_t>(std::max(0.0,value.elapsed));
    const bool message=app.status.message!=value.message,error=app.status.error!=value.error;
    const bool preview=app.status.preview!=value.preview;
    const bool night=app.status.nightEnabled!=value.nightEnabled || app.status.nightWaiting!=value.nightWaiting ||
        app.status.nightDurationMs!=value.nightDurationMs || app.status.night.samples!=value.night.samples ||
        app.status.night.appliedGain!=value.night.appliedGain || app.status.night.targetLimited!=value.night.targetLimited;
    app.status=std::move(value);
    // Status and tray handling remain live while hidden; only visual work waits.
    if(app.hiddenToTray || IsIconic(app.window)) {
        if(force || stats || message || error || preview || night)app.visibleDirty=true;
        return;
    }
    if(force || app.visibleDirty) {
        updateControls();updateNightText();updateStatusText();
        InvalidateRect(app.window,nullptr,FALSE);InvalidateRect(app.preview,nullptr,FALSE);InvalidateRect(app.statusText,nullptr,TRUE);
        app.visibleDirty=false;return;
    }
    if(!app.controlsUpdated || app.controlsState!=app.status.state)updateControls();
    if(message || error || state)updateStatusText();
    if(night)updateNightText();
    if(preview)InvalidateRect(app.preview,nullptr,FALSE);
    if(state)invalidateCanvas({app.contentWidth-app.scale(280),app.scale(20),app.contentWidth-app.scale(26),app.scale(49)});
    if(stats)invalidateCanvas({app.scale(26),app.contentHeight-app.scale(147),app.contentWidth-app.scale(26),app.contentHeight-app.scale(123)});
}
void changeLayout(bool reset) {
    app.modeIndex = choice(app.mode);
    app.settings.separateFiles = app.modeIndex == SeparateFilesMode;
    if (reset || app.modeIndex != static_cast<int>(Mode::Custom) || app.settings.layers.size() < 2) {
        app.settings.layers = preset(app.settings.separateFiles ? Mode::SideBySide : static_cast<Mode>(app.modeIndex));
        // Custom edits retain their preset; a newly seeded collage starts over.
        app.collagePreset = app.modeIndex == static_cast<int>(Mode::SideBySide) ? Mode::SideBySide : Mode::Overlay;
    }
    app.selected = -1; configure(); if(app.engine && !app.active())app.engine->refreshSources(); updateControls();layout(); InvalidateRect(app.preview,nullptr,FALSE);
}
HICON trayIcon(int state) {
    if(app.trayIcons[state])return app.trayIcons[state];
    // The colored circle remains recognizable at the notification area's size.
    const COLORREF colors[]={RGB(70,100,110),RGB(210,60,50),RGB(220,156,30),RGB(40,126,187),RGB(160,40,40)};
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=16;
    info.bmiHeader.biHeight=-16;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP color=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    const BYTE maskBits[32]{};HBITMAP mask=CreateBitmap(16,16,1,1,maskBits);
    if(color && mask && bits) {
        auto pixels=static_cast<DWORD*>(bits);const auto c=colors[state];
        for(int y=0;y<16;++y)for(int x=0;x<16;++x){const int dx=2*x-15,dy=2*y-15;
            pixels[y*16+x]=dx*dx+dy*dy<=169 ? 0xff000000u|(DWORD(GetRValue(c))<<16)|(DWORD(GetGValue(c))<<8)|GetBValue(c) : 0;}
        ICONINFO icon{};icon.fIcon=TRUE;icon.hbmColor=color;icon.hbmMask=mask;app.trayIcons[state]=CreateIconIndirect(&icon);
    }
    if(color)DeleteObject(color);if(mask)DeleteObject(mask);
    return app.trayIcons[state] ? app.trayIcons[state] : LoadIconW(nullptr,IDI_APPLICATION);
}
bool updateTray(bool addIcon=false) {
    if(!app.trayRegistered && !addIcon)return false;
    if(addIcon && !app.taskbarCreated){app.taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");if(!app.taskbarCreated)return false;}
    if(!addIcon && app.trayStateValid && app.trayState==app.status.state &&
       app.trayFailure==app.status.recordingFailed && app.traySeparate==app.settings.separateFiles)return true;
    const wchar_t* state=app.status.state==State::Recording?L"Recording":app.status.state==State::Paused?L"Paused":
        app.status.state==State::Starting?L"Preparing":app.status.state==State::Finishing?L"Saving":app.status.recordingFailed?L"Recording failed":L"Ready";
    std::wstring tip=L"Timelapse - ";tip+=state;
    if(app.active())tip+=app.settings.separateFiles?L" - desktop + camera files":L"";
    if(!addIcon && tip==app.trayTooltip){
        app.trayState=app.status.state;app.trayFailure=app.status.recordingFailed;app.traySeparate=app.settings.separateFiles;app.trayStateValid=true;
        return true;
    }
    const int icon=app.status.recordingFailed&&!app.active()?4:app.status.state==State::Recording?1:app.status.state==State::Paused?2:app.active()?3:0;
    NOTIFYICONDATAW data{sizeof(data)};data.hWnd=app.window;data.uID=1;data.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
    data.uCallbackMessage=TrayMessage;data.hIcon=trayIcon(icon);wcscpy_s(data.szTip,tip.c_str());
    if(!Shell_NotifyIconW(addIcon?NIM_ADD:NIM_MODIFY,&data)){
        Shell_NotifyIconW(NIM_DELETE,&data);app.trayRegistered=false;app.trayVersion4=false;app.trayTooltip.clear();app.trayStateValid=false;return false;
    }
    app.trayRegistered=true;app.trayTooltip=std::move(tip);
    app.trayState=app.status.state;app.trayFailure=app.status.recordingFailed;app.traySeparate=app.settings.separateFiles;app.trayStateValid=true;
    if(addIcon){data.uVersion=NOTIFYICON_VERSION_4;app.trayVersion4=Shell_NotifyIconW(NIM_SETVERSION,&data)!=FALSE;}
    return true;
}
void removeTray() {
    if(app.trayRegistered){NOTIFYICONDATAW data{sizeof(data)};data.hWnd=app.window;data.uID=1;Shell_NotifyIconW(NIM_DELETE,&data);}
    app.trayRegistered=false;app.trayVersion4=false;app.trayTooltip.clear();app.trayStateValid=false;
}
void showWindow() {
    app.hiddenToTray=false;ShowWindow(app.window,SW_RESTORE);SetForegroundWindow(app.window);
    if(app.engine){applyStatus(app.engine->status(),true);configure();}
}
bool hideToTray() {
    if(app.customDialog)EndDialog(app.customDialog,IDCANCEL);
    applyStatus(app.engine->status());
    if(!updateTray(!app.trayRegistered)) {
        showWindow();
        MessageBoxW(app.window,L"Windows could not add Timelapse to the system tray. The window will stay open so you can control your recording.",L"Timelapse",MB_OK|MB_ICONWARNING);
        return false;
    }
    app.hiddenToTray=true;configure();ShowWindow(app.window,SW_HIDE);
    if(!app.trayNoticeShown) {
        NOTIFYICONDATAW data{sizeof(data)};data.hWnd=app.window;data.uID=1;data.uFlags=NIF_INFO;data.dwInfoFlags=NIIF_INFO;
        wcscpy_s(data.szInfoTitle,L"Timelapse is in the system tray");
        wcscpy_s(data.szInfo,L"Recording continues when this window is closed. Right-click the tray icon to show Timelapse, finish, or exit.");
        Shell_NotifyIconW(NIM_MODIFY,&data);app.trayNoticeShown=true;
    }
    return true;
}
void exitApplication() {
    if(app.closeWhenDone)return;
    if(app.customDialog)EndDialog(app.customDialog,IDCANCEL);
    applyStatus(app.engine->status());
    if(app.active()) {
        if(MessageBoxW(app.window,L"Finish the current recording and exit Timelapse?",L"Finish recording",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK)return;
        app.closeWhenDone=true;app.engine->finish();EnableWindow(app.window,FALSE);
    } else DestroyWindow(app.window);
}
void trayMenu(POINT at={},bool usePoint=false) {
    applyStatus(app.engine->status());
    HMENU menu=CreatePopupMenu();if(!menu){showWindow();return;}
    AppendMenuW(menu,MF_STRING,TrayShow,L"&Show Timelapse");
    const bool pauseAllowed=!app.closeWhenDone&&(app.status.state==State::Recording||app.status.state==State::Paused);
    AppendMenuW(menu,MF_STRING|(pauseAllowed?MF_ENABLED:MF_GRAYED),TrayPause,app.status.state==State::Paused?L"&Resume recording":L"&Pause recording");
    const bool finishAllowed=!app.closeWhenDone&&(pauseAllowed||app.status.state==State::Starting);
    AppendMenuW(menu,MF_STRING|(finishAllowed?MF_ENABLED:MF_GRAYED),TrayFinish,L"&Finish recording");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING|(app.closeWhenDone?MF_GRAYED:MF_ENABLED),TrayExit,L"E&xit Timelapse");
    SetMenuDefaultItem(menu,TrayShow,FALSE);if(!usePoint)GetCursorPos(&at);SetForegroundWindow(app.window);
    const UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,at.x,at.y,0,app.window,nullptr);
    DestroyMenu(menu);PostMessageW(app.window,WM_NULL,0,0);
    if(command)SendMessageW(app.window,WM_COMMAND,command,0);
}
void endLayoutDrag() {
    // Keep the last applied edit, but never reuse a pointer origin after the
    // preview has moved or changed size. Do not release another window's capture.
    app.dragging = false;
    if (GetCapture() == app.preview) ReleaseCapture();
}
RECT previewVideoRect(const RECT& client) {
    const int sourceWidth=std::max(1,app.settings.width),sourceHeight=std::max(1,app.settings.height);
    int width=std::max(1L,client.right), height=std::max(1,int(int64_t(width)*sourceHeight/sourceWidth));
    if(height>client.bottom){height=std::max(1L,client.bottom);width=std::max(1,int(int64_t(height)*sourceWidth/sourceHeight));}
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
    const int night=nightRow();
    const int previewTop=app.scale(app.advancedExpanded?(night==2?372:night==1?300:258):190);
    const int minimumH=previewTop+app.scale(160)+app.scale(191);
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
    const int refreshW=app.scale(92), optionsW=width-refreshW-3*gap;
    const int deviceW=optionsW*31/100, advancedW=optionsW-2*deviceW;
    const int cameraX=pad+deviceW+gap, advancedX=cameraX+deviceW+gap;
    move(app.labels[4],pad,app.scale(121),deviceW,app.scale(20)); move(app.monitor,pad,row2,deviceW,app.scale(220));
    move(app.labels[5],cameraX,app.scale(121),deviceW,app.scale(20)); move(app.camera,cameraX,row2,deviceW,app.scale(220));
    move(app.advanced,advancedX,row2,advancedW,ch);
    const int options=width-2*gap, encodingW=options*40/100, stopW=options*25/100;
    const int stopX=pad+encodingW+gap, diskX=stopX+stopW+gap;
    move(app.labels[6],pad,app.scale(190),encodingW,app.scale(20));move(app.encodingMode,pad,app.scale(211),encodingW,app.scale(190));
    move(app.labels[7],stopX,app.scale(190),stopW,app.scale(20));move(app.stopAfter,stopX,app.scale(211),stopW,app.scale(210));
    move(app.lowDisk,diskX,app.scale(211),options-encodingW-stopW,ch);
    move(app.nightEnabled,pad,app.scale(night==2?278:258),encodingW,ch);
    move(app.labels[8],stopX,app.scale(257),stopW,app.scale(20));move(app.nightDuration,stopX,app.scale(278),stopW,app.scale(210));
    move(app.labels[9],diskX,app.scale(257),options-encodingW-stopW,app.scale(20));move(app.nightTarget,diskX,app.scale(278),options-encodingW-stopW,app.scale(150));
    move(app.nightHint,pad,app.scale(313),width,app.scale(21));move(app.nightDetail,pad,app.scale(337),width,app.scale(21));
    move(app.refresh,r.right-pad-refreshW,row2,refreshW,ch);
    // The logical canvas retains a usable preview when the viewport is small.
    const int previewH=static_cast<int>(r.bottom)-previewTop-app.scale(191);
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
    app.advancedLimitIndex=-1;updateAdvanced();
    app.layingOut = false;
    updateAdvanced();
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
    if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))return;
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
void toggleAdvanced() {
    const HWND focused=GetFocus();
    if(app.advancedExpanded)for(HWND child:{app.encodingMode,app.stopAfter,app.lowDisk,app.nightEnabled,app.nightDuration,app.nightTarget})
        if(child && (focused==child || (focused && IsChild(child,focused)))){SetFocus(app.advanced);break;}
    app.advancedExpanded=!app.advancedExpanded;
    updateAdvanced();layout();revealFocusedControl();
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
    for(HWND box:{app.mode,app.interval,app.videoSize,app.encodingQuality,app.encodingMode,app.stopAfter,app.nightDuration,app.nightTarget,app.monitor,app.camera})
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
    for(HWND child:{app.statusText,app.nightHint,app.nightDetail})if(child)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
}

enum class CustomKind { Interval, Size, Limit };
enum CustomId { CustomFirst=5001, CustomSecond, CustomUnits, CustomHelp, CustomError, CustomFirstLabel, CustomSecondLabel };
struct CustomDraft {
    CustomKind kind=CustomKind::Interval;
    int64_t durationMs=5000;
    int width=1280,height=720,dpi=96,scrollX=0,scrollY=0;
    HFONT font{};
    HWND first{},second{},units{},help{},error{},firstLabel{},secondLabel{},okay{},cancel{};
    bool layingOut=false;
    int scale(int value) const { return MulDiv(value,dpi,96); }
};
struct CustomTemplate {
    DLGTEMPLATE dialog{};
    WORD menu=0,windowClass=0,title=0;
    CustomTemplate() {
        dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;
        dialog.dwExtendedStyle=WS_EX_CONTROLPARENT;dialog.cx=260;dialog.cy=160;
    }
};
std::wstring secondsInput(int64_t milliseconds) {
    std::wstring value=std::to_wstring(milliseconds/1000);
    if(milliseconds%1000){std::wstring fraction=std::to_wstring(1000+milliseconds%1000).substr(1);
        while(fraction.back()==L'0')fraction.pop_back();value+=L"."+fraction;}
    return value;
}
void customLayout(HWND window,CustomDraft& draft) {
    if(draft.layingOut)return;draft.layingOut=true;
    RECT client{};GetClientRect(window,&client);
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    const int barW=GetSystemMetricsForDpi(SM_CXVSCROLL,draft.dpi),barH=GetSystemMetricsForDpi(SM_CYHSCROLL,draft.dpi);
    const int availableW=client.right+((style&WS_VSCROLL)?barW:0),availableH=client.bottom+((style&WS_HSCROLL)?barH:0);
    const int pad=draft.scale(18),gap=draft.scale(14);
    const auto wrapped=[&](HWND child,int width,int minimum){
        if(!child)return minimum;
        wchar_t value[1024]{};GetWindowTextW(child,value,1024);RECT rect{0,0,std::max(1,width),0};
        HDC dc=GetDC(window);if(!dc)return minimum;
        const auto previous=SelectObject(dc,draft.font?draft.font:GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc,value,-1,&rect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,previous);ReleaseDC(window,dc);return std::max(minimum,int(rect.bottom));
    };
    bool horizontal=false,vertical=false;
    int helpHeight=0,errorHeight=0,errorTop=0,buttonTop=0,minimumHeight=0;
    const auto measure=[&](int width){helpHeight=wrapped(draft.help,width-2*pad,draft.scale(54));errorTop=draft.scale(80)+helpHeight+draft.scale(8);
        errorHeight=wrapped(draft.error,width-2*pad,draft.scale(46));buttonTop=errorTop+errorHeight+draft.scale(12);minimumHeight=buttonTop+draft.scale(42);};
    for(int i=0;i<3;++i){horizontal=availableW-(vertical?barW:0)<draft.scale(320);
        measure(std::max(availableW-(vertical?barW:0),draft.scale(320)));vertical=availableH-(horizontal?barH:0)<minimumHeight;}
    ShowScrollBar(window,SB_HORZ,horizontal);ShowScrollBar(window,SB_VERT,vertical);GetClientRect(window,&client);
    const int width=std::max(int(client.right),draft.scale(320));measure(width);const int height=std::max(int(client.bottom),minimumHeight);
    draft.scrollX=std::clamp(draft.scrollX,0,width-int(std::max(1L,client.right)));
    draft.scrollY=std::clamp(draft.scrollY,0,height-int(std::max(1L,client.bottom)));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};info.nMax=width-1;info.nPage=std::max(1L,client.right);info.nPos=draft.scrollX;SetScrollInfo(window,SB_HORZ,&info,TRUE);
    info.nMax=height-1;info.nPage=std::max(1L,client.bottom);info.nPos=draft.scrollY;SetScrollInfo(window,SB_VERT,&info,TRUE);
    const int half=(width-2*pad-gap)/2;
    const auto move=[&](HWND child,int x,int y,int w,int h){if(child)MoveWindow(child,x-draft.scrollX,y-draft.scrollY,w,h,TRUE);};
    move(draft.firstLabel,pad,draft.scale(16),half,draft.scale(20));move(draft.first,pad,draft.scale(38),half,draft.scale(28));
    move(draft.secondLabel,pad+half+gap,draft.scale(16),half,draft.scale(20));
    move(draft.second?draft.second:draft.units,pad+half+gap,draft.scale(38),half,draft.scale(draft.second?28:170));
    move(draft.help,pad,draft.scale(80),width-2*pad,helpHeight);move(draft.error,pad,errorTop,width-2*pad,errorHeight);
    move(draft.okay,width-pad-draft.scale(174),buttonTop,draft.scale(80),draft.scale(28));
    move(draft.cancel,width-pad-draft.scale(80),buttonTop,draft.scale(80),draft.scale(28));
    draft.layingOut=false;
}
void customReveal(HWND window,CustomDraft& draft,HWND child) {
    if(!child || !IsChild(window,child))return;
    RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
    if(bounds.left<0)draft.scrollX+=bounds.left;else if(bounds.right>client.right)draft.scrollX+=bounds.right-client.right;
    if(bounds.top<0)draft.scrollY+=bounds.top;else if(bounds.bottom>client.bottom)draft.scrollY+=bounds.bottom-client.bottom;
    customLayout(window,draft);
}
void customFont(HWND window,CustomDraft& draft) {
    const HFONT previous=draft.font;
    draft.font=CreateFontW(-draft.scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    EnumChildWindows(window,[](HWND child,LPARAM font)->BOOL {SendMessageW(child,WM_SETFONT,font,TRUE);return TRUE;},reinterpret_cast<LPARAM>(draft.font));
    if(previous)DeleteObject(previous);
}
bool validateCustom(CustomDraft& draft,std::wstring& message,HWND& invalid) {
    wchar_t first[96]{},second[96]{};invalid=draft.first;
    if(GetWindowTextLengthW(draft.first)>=96){message=L"Enter a shorter complete number.";return false;}
    GetWindowTextW(draft.first,first,96);
    if(draft.kind==CustomKind::Size){
        int width=0,height=0;
        if(!parsePixelDimension(first,width,message))return false;
        invalid=draft.second;if(GetWindowTextLengthW(draft.second)>=96){message=L"Enter a shorter complete number.";return false;}GetWindowTextW(draft.second,second,96);
        if(!parsePixelDimension(second,height,message))return false;
        if(width<MinVideoDimension || width>MaxVideoDimension || (width&1))invalid=draft.first;
        if(!validateVideoSize(width,height,message))return false;
        draft.width=width;draft.height=height;return true;
    }
    const bool interval=draft.kind==CustomKind::Interval;
    int64_t duration=0;const auto unit=static_cast<DurationUnit>(std::clamp(choice(draft.units),0,interval?2:3));
    if(!parseDuration(first,unit,interval?MinCaptureIntervalMs:1000,interval?int64_t(MaxCaptureIntervalMs):int64_t(INT_MAX)*1000,
                      interval?1:1000,duration,message))return false;
    draft.durationMs=duration;return true;
}
INT_PTR CALLBACK customProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* draft=reinterpret_cast<CustomDraft*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_INITDIALOG){
            draft=reinterpret_cast<CustomDraft*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);app.customDialog=window;
            draft->dpi=static_cast<int>(GetDpiForWindow(window));if(draft->dpi<=0)draft->dpi=app.dpi;
            const bool size=draft->kind==CustomKind::Size,interval=draft->kind==CustomKind::Interval;
            SetWindowTextW(window,size?L"Custom video size":interval?L"Custom capture interval":L"Custom stop time");
            const auto child=[&](const wchar_t* type,const wchar_t* text,DWORD style,int id){return CreateWindowExW(std::wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,
                type,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);};
            draft->firstLabel=child(L"STATIC",size?L"&Width (pixels)":L"&Value",0,CustomFirstLabel);
            draft->first=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,CustomFirst);
            draft->secondLabel=child(L"STATIC",size?L"&Height (pixels)":L"&Units",0,CustomSecondLabel);
            if(size)draft->second=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,CustomSecond);
            else {draft->units=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,CustomUnits);
                for(const auto* unit:{L"Seconds",L"Minutes",L"Hours"})add(draft->units,unit);if(!interval)add(draft->units,L"Days");choose(draft->units,0);}
            draft->help=child(L"STATIC",size?L"Use even dimensions from 48 to 4096 pixels, at most 8,847,360 pixels total. Sources fit inside the video without stretching.":
                interval?L"Choose 0.1 seconds to 24 hours. Decimals use a point and must resolve to whole milliseconds. Playback stays at 30 fps.":
                L"Choose 1 to 2,147,483,647 seconds of active recording. Pauses and initial preparation do not count. Decimals must resolve to whole seconds.",SS_NOPREFIX,CustomHelp);
            draft->error=child(L"STATIC",L"",SS_NOPREFIX,CustomError);
            draft->okay=child(L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK);draft->cancel=child(L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL);
            if(!draft->firstLabel || !draft->secondLabel || !draft->first || !(size?draft->second:draft->units) || !draft->help || !draft->error || !draft->okay || !draft->cancel){EndDialog(window,-1);return TRUE;}
            SendMessageW(draft->first,EM_SETLIMITTEXT,80,0);if(size)SendMessageW(draft->second,EM_SETLIMITTEXT,80,0);
            SetWindowTextW(draft->first,(size?std::to_wstring(draft->width):secondsInput(draft->durationMs)).c_str());
            if(size)SetWindowTextW(draft->second,std::to_wstring(draft->height).c_str());customFont(window,*draft);
            RECT rect{0,0,draft->scale(420),draft->scale(250)};AdjustWindowRectExForDpi(&rect,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),draft->dpi);
            RECT owner{};GetWindowRect(app.window,&owner);OffsetRect(&rect,(owner.left+owner.right-(rect.right-rect.left))/2-rect.left,(owner.top+owner.bottom-(rect.bottom-rect.top))/2-rect.top);
            rect=fitWindow(rect,workArea(MonitorFromWindow(app.window,MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
            customLayout(window,*draft);SetFocus(draft->first);SendMessageW(draft->first,EM_SETSEL,0,-1);return FALSE;
        }
        if(!draft)return FALSE;
        switch(message){
        case WM_SIZE:customLayout(window,*draft);return TRUE;
        case WM_DPICHANGED:{draft->dpi=HIWORD(wp);customFont(window,*draft);RECT rect=*reinterpret_cast<RECT*>(lp);rect=fitWindow(rect,workArea(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);customLayout(window,*draft);customReveal(window,*draft,GetFocus());return TRUE;}
        case WM_HSCROLL:case WM_VSCROLL:{const int bar=message==WM_HSCROLL?SB_HORZ:SB_VERT;SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int position=info.nPos;
            switch(LOWORD(wp)){case SB_LINEUP:position-=draft->scale(24);break;case SB_LINEDOWN:position+=draft->scale(24);break;case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:position=info.nTrackPos;break;case SB_TOP:position=0;break;case SB_BOTTOM:position=info.nMax;break;default:return TRUE;}
            (bar==SB_HORZ?draft->scrollX:draft->scrollY)=position;customLayout(window,*draft);return TRUE;}
        case WM_COMMAND:
            if(LOWORD(wp)==IDCANCEL){EndDialog(window,IDCANCEL);return TRUE;}
            if(LOWORD(wp)==IDOK){
                if(app.active()){EndDialog(window,IDCANCEL);return TRUE;}
                std::wstring error;HWND invalid{};
                if(validateCustom(*draft,error,invalid)){EndDialog(window,IDOK);return TRUE;}
                SetWindowTextW(draft->error,error.c_str());SetFocus(invalid);SendMessageW(invalid,EM_SETSEL,0,-1);customReveal(window,*draft,invalid);return TRUE;
            }
            if(HIWORD(wp)==EN_SETFOCUS || HIWORD(wp)==CBN_SETFOCUS || HIWORD(wp)==BN_SETFOCUS)customReveal(window,*draft,reinterpret_cast<HWND>(lp));
            return FALSE;
        case WM_CTLCOLORSTATIC:if(reinterpret_cast<HWND>(lp)==draft->error){SetTextColor(reinterpret_cast<HDC>(wp),RGB(164,40,40));SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_BTNFACE));return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));}break;
        case WM_CLOSE:EndDialog(window,IDCANCEL);return TRUE;
        case WM_DESTROY:if(draft->font){DeleteObject(draft->font);draft->font=nullptr;}if(app.customDialog==window)app.customDialog=nullptr;return TRUE;
        }
    } catch (...) {EndDialog(window,-1);return TRUE;}
    return FALSE;
}
void commitCustom(const CustomDraft& draft) {
    if(draft.kind==CustomKind::Interval){app.customIntervalMs=static_cast<int>(draft.durationMs);app.hasCustomInterval=true;app.committedInterval=6;
        for(int i=0;i<6;++i)if(CaptureIntervals[i]==draft.durationMs)app.committedInterval=i;}
    else if(draft.kind==CustomKind::Size){app.customWidth=draft.width;app.customHeight=draft.height;app.hasCustomSize=true;
        app.committedSize=draft.width==1280 && draft.height==720?0:draft.width==1920 && draft.height==1080?1:2;}
    else {app.customLimitSeconds=static_cast<int>(draft.durationMs/1000);app.hasCustomLimit=true;app.committedLimit=6;
        for(int i=1;i<6;++i)if(RecordingLimits[i]==app.customLimitSeconds)app.committedLimit=i;}
    normalizeCustomSelections();customItems();configure();updateControls();layout();InvalidateRect(app.preview,nullptr,FALSE);InvalidateRect(app.window,nullptr,FALSE);
}
void editCustom(CustomKind kind) {
    if(app.active() || app.customDialog)return;
    HWND box=kind==CustomKind::Interval?app.interval:kind==CustomKind::Size?app.videoSize:app.stopAfter;
    choose(box,kind==CustomKind::Interval?app.committedInterval:kind==CustomKind::Size?app.committedSize:app.committedLimit);
    CustomDraft draft;draft.kind=kind;draft.durationMs=kind==CustomKind::Interval?app.settings.intervalMs:int64_t(app.settings.recordingLimitSeconds?app.settings.recordingLimitSeconds:900)*1000;
    draft.width=app.settings.width;draft.height=app.settings.height;CustomTemplate resource;
    const auto outcome=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,app.window,customProc,reinterpret_cast<LPARAM>(&draft));
    if(!IsWindow(app.window))return;
    if(outcome==IDOK && !app.active())commitCustom(draft);
    else if(outcome==-1)MessageBoxW(app.window,L"The custom settings dialog could not be opened. Try again.",L"Timelapse",MB_OK|MB_ICONERROR);
    if(!app.closeWhenDone){SetFocus(box);revealFocusedControl();}
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
        const auto encodingMode=std::to_wstring(choice(app.encodingMode));
        const auto recordingLimit=std::to_wstring(std::clamp(choice(app.stopAfter),0,app.hasCustomLimit?6:5));
        const auto customInterval=std::to_wstring(app.customIntervalMs), customWidth=std::to_wstring(app.customWidth),customHeight=std::to_wstring(app.customHeight);
        const auto customLimit=std::to_wstring(app.customLimitSeconds);
        const auto nightDuration=std::to_wstring(NightDurations[app.nightDuration?std::clamp(choice(app.nightDuration),0,5):0]);
        const auto nightTarget=std::to_wstring(NightTargets[app.nightTarget?std::clamp(choice(app.nightTarget),0,2):1]);
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
            WritePrivateProfileStringW(L"Settings",L"EncodingQuality",encodingQuality.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"EncodingMode",encodingMode.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"RecordingLimit",recordingLimit.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"CaptureIntervalMs",customInterval.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"VideoWidth",customWidth.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"VideoHeight",customHeight.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"RecordingLimitSeconds",customLimit.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"StopOnLowDiskSpace",!app.lowDisk || SendMessageW(app.lowDisk,BM_GETCHECK,0,0)!=BST_UNCHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightEnabled",app.nightEnabled && SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_CHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightDurationMs",nightDuration.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightTargetBrightness",nightTarget.c_str(),pending.path);
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
        const int interval=static_cast<int>(GetPrivateProfileIntW(L"Settings",L"Interval",2,path));
        const int videoSize=static_cast<int>(GetPrivateProfileIntW(L"Settings",L"Quality",0,path));
        choose(app.interval,std::clamp(interval,0,5));
        choose(app.videoSize,std::clamp(videoSize,0,1));
        choose(app.encodingQuality,std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Settings",L"EncodingQuality",1,path)),0,2));
        const UINT encodingMode=GetPrivateProfileIntW(L"Settings",L"EncodingMode",0,path);
        choose(app.encodingMode,encodingMode<=4?static_cast<int>(encodingMode):0);
        const UINT recordingLimit=GetPrivateProfileIntW(L"Settings",L"RecordingLimit",0,path);
        choose(app.stopAfter,recordingLimit<=5?static_cast<int>(recordingLimit):0);
        const auto exactInteger=[&](const wchar_t* key,int minimum,int maximum,int& output){
            wchar_t value[48]{};GetPrivateProfileStringW(L"Settings",key,L"",value,48,path);
            int64_t parsed=0;std::wstring error;
            if(!parseDuration(value,DurationUnit::Seconds,int64_t(minimum)*1000,int64_t(maximum)*1000,1000,parsed,error) ||
               std::to_wstring(parsed/1000)!=value)return false;
            output=static_cast<int>(parsed/1000);return true;
        };
        app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;
        app.customIntervalMs=5000;app.customWidth=1280;app.customHeight=720;app.customLimitSeconds=900;
        if(interval==6){app.hasCustomInterval=exactInteger(L"CaptureIntervalMs",MinCaptureIntervalMs,MaxCaptureIntervalMs,app.customIntervalMs);choose(app.interval,app.hasCustomInterval?6:2);}
        if(videoSize==2){std::wstring error;app.hasCustomSize=exactInteger(L"VideoWidth",MinVideoDimension,MaxVideoDimension,app.customWidth) &&
            exactInteger(L"VideoHeight",MinVideoDimension,MaxVideoDimension,app.customHeight) && validateVideoSize(app.customWidth,app.customHeight,error);
            choose(app.videoSize,app.hasCustomSize?2:0);}
        if(recordingLimit==6){app.hasCustomLimit=exactInteger(L"RecordingLimitSeconds",1,INT_MAX,app.customLimitSeconds);choose(app.stopAfter,app.hasCustomLimit?6:0);}
        // Add the custom display row before selecting it: CB_SETCURSEL cannot
        // select an item that does not yet exist in an older preset-only list.
        app.committedInterval=app.hasCustomInterval?6:interval==6?2:std::clamp(interval,0,5);
        app.committedSize=app.hasCustomSize?2:videoSize==2?0:std::clamp(videoSize,0,1);
        app.committedLimit=app.hasCustomLimit?6:recordingLimit<=5?static_cast<int>(recordingLimit):0;
        normalizeCustomSelections();customItems();
        wchar_t stopOnLowDiskSpace[16]{};
        GetPrivateProfileStringW(L"Settings",L"StopOnLowDiskSpace",L"1",stopOnLowDiskSpace,16,path);
        SendMessageW(app.lowDisk,BM_SETCHECK,std::wcscmp(stopOnLowDiskSpace,L"0")==0?BST_UNCHECKED:BST_CHECKED,0);
        wchar_t nightEnabled[16]{};GetPrivateProfileStringW(L"Settings",L"NightEnabled",L"0",nightEnabled,16,path);
        SendMessageW(app.nightEnabled,BM_SETCHECK,std::wcscmp(nightEnabled,L"1")==0?BST_CHECKED:BST_UNCHECKED,0);
        const auto loadNightChoice=[&](const wchar_t* key,HWND box,const auto& values,int fallback){
            const auto defaultValue=std::to_wstring(values[fallback]);wchar_t value[32]{};
            GetPrivateProfileStringW(L"Settings",key,defaultValue.c_str(),value,32,path);
            int selected=fallback;
            for(size_t i=0;i<std::size(values);++i)if(std::to_wstring(values[i])==value){selected=static_cast<int>(i);break;}
            choose(box,selected);
        };
        loadNightChoice(L"NightDurationMs",app.nightDuration,NightDurations,0);
        loadNightChoice(L"NightTargetBrightness",app.nightTarget,NightTargets,1);
        // Launch on desktop: opening the app never silently turns on a camera.
        choose(app.mode,0);
        updateAdvanced();
    } else if(!savePreferences()) {
        OutputDebugStringW(L"Timelapse could not save preferences.\n");
    }
}

RECT layerRect(const Layer& l) {
    int width=app.videoRect.right-app.videoRect.left, height=app.videoRect.bottom-app.videoRect.top;
    return {app.videoRect.left+static_cast<LONG>(l.rect.x*width),app.videoRect.top+static_cast<LONG>(l.rect.y*height),app.videoRect.left+static_cast<LONG>((l.rect.x+l.rect.w)*width),app.videoRect.top+static_cast<LONG>((l.rect.y+l.rect.h)*height)};
}
void customized() {
    app.settings.separateFiles=false;
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
        if(!app.settings.separateFiles && app.settings.layers.size()>1) for(size_t i=0;i<app.settings.layers.size();++i) {
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
        SetFocus(w); if(app.settings.separateFiles || app.settings.layers.size()<2) return 0;
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
        if(!app.settings.separateFiles && app.settings.layers.size()>1) {
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
    if(app.taskbarCreated && msg==app.taskbarCreated) {
        app.trayRegistered=false;app.trayVersion4=false;app.trayTooltip.clear();app.trayStateValid=false;
        if(!updateTray(true) && app.hiddenToTray)showWindow();
        return 0;
    }
    switch(msg) {
    case WM_SYSCOMMAND:if((wp&0xfff0)==ExitSystemCommand){exitApplication();return 0;}break;
    case ShowExistingMessage:showWindow();return 0;
    case TrayMessage: {
        const UINT event=app.trayVersion4?LOWORD(lp):static_cast<UINT>(lp);
        if(event==WM_LBUTTONUP || event==WM_LBUTTONDBLCLK || event==NIN_SELECT || event==NIN_KEYSELECT || event==NIN_BALLOONUSERCLICK)showWindow();
        else if(event==WM_RBUTTONUP || event==WM_CONTEXTMENU){POINT at{GET_X_LPARAM(wp),GET_Y_LPARAM(wp)};trayMenu(at,app.trayVersion4 && !(at.x==-1 && at.y==-1));}
        return 0;
    }
    case WM_CREATE: {
        app.startupComplete=false;app.advancedExpanded=false;app.advancedLimitIndex=app.advancedVisibility=-1;
        app.advancedNightState=app.nightVisibility=-1;app.nightValidation.clear();app.statusCaption.clear();app.statusCaptionError=false;app.nightHintCaption.clear();app.nightDetailCaption.clear();
        app.customDialog=nullptr;app.advancedCaption.clear();app.advancedTooltip.clear();
        app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;
        app.customIntervalMs=5000;app.customWidth=1280;app.customHeight=720;app.customLimitSeconds=900;
        app.committedInterval=2;app.committedSize=app.committedLimit=0;
        app.visibleDirty=true;app.controlsUpdated=false;app.trayStateValid=false;
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
        app.mode=combo(0,L"&Source",ModeBox);for(auto s:{L"Desktop",L"Camera",L"Desktop + camera",L"Side by side",L"Custom collage",SeparateFilesLabel})add(app.mode,s);
        app.interval=combo(1,L"Capture &every",IntervalBox);for(auto s:{L"1 second",L"2 seconds",L"5 seconds",L"10 seconds",L"30 seconds",L"60 seconds"})add(app.interval,s);
        app.videoSize=combo(2,L"Video si&ze",SizeBox);add(app.videoSize,L"720p");add(app.videoSize,L"1080p");
        app.encodingQuality=combo(3,L"Video &quality",EncodingQualityBox);for(auto s:{L"Smaller file",L"Balanced",L"More detail"})add(app.encodingQuality,s);
        app.monitor=combo(4,L"&Display",MonitorBox);app.camera=combo(5,L"Ca&mera",CameraBox);
        auto button=[&](const wchar_t* s,int id){return requiredControl(L"BUTTON",s,WS_TABSTOP|BS_PUSHBUTTON,id);};
        app.advanced=requiredControl(L"BUTTON",L"&Advanced",WS_TABSTOP|BS_AUTOCHECKBOX|BS_PUSHLIKE,AdvancedToggle);
        app.refresh=button(L"Re&fresh",Refresh);
        app.encodingMode=combo(6,L"Encodin&g",EncodingModeBox);
        for(auto label:EncodingModeLabels)add(app.encodingMode,label);
        app.stopAfter=combo(7,L"S&top after",StopAfterBox);for(auto label:RecordingLimitLabels)add(app.stopAfter,label);
        app.lowDisk=requiredControl(L"BUTTON",L"Stop on &low disk space",WS_TABSTOP|BS_AUTOCHECKBOX,LowDiskBox);
        app.nightEnabled=requiredControl(L"BUTTON",L"&Night camera (software blend)",WS_TABSTOP|BS_AUTOCHECKBOX,NightBox);
        app.nightDuration=combo(8,L"Blend d&uration",NightDurationBox);for(auto label:NightDurationLabels)add(app.nightDuration,label);
        app.nightTarget=combo(9,L"Auto &brightness",NightTargetBox);for(auto label:{L"Dark",L"Balanced",L"Bright"})add(app.nightTarget,label);
        app.nightHint=requiredControl(L"STATIC",L"",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,NightHint);
        app.nightDetail=requiredControl(L"STATIC",L"",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,NightDetail);
        app.record=button(L"●  &Record",Record);app.pause=button(L"&Pause",Pause);app.finish=button(L"&Finish",Finish);app.folder=button(L"&Change...",Folder);app.openFolder=button(L"&Open folder",OpenFolder);app.reset=button(L"Reset layout",Reset);app.forward=button(L"Bring forward",Forward);
        app.preview=requiredControl(L"LapsePreview",L"Collage preview. Space selects a layer. Arrow keys move it. Shift and arrow keys resize it.",WS_TABSTOP,Preview);
        app.statusText=requiredControl(L"STATIC",app.status.message.c_str(),SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,210);
        if(!controlsReady) {
            app.mode=nullptr;
            OutputDebugStringW(L"Timelapse could not create its required controls.\n");
            return -1;
        }
        SendMessageW(app.statusText,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
        SendMessageW(app.nightHint,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);SendMessageW(app.nightDetail,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
        app.tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,w,nullptr,nullptr,nullptr);
        TOOLINFOW tip{sizeof(tip)};tip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tip.hwnd=w;tip.uId=reinterpret_cast<UINT_PTR>(app.statusText);tip.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));SendMessageW(app.tooltip,TTM_SETMAXTIPWIDTH,0,app.scale(520));
        tip.uId=reinterpret_cast<UINT_PTR>(app.encodingMode);
        tip.lpszText=const_cast<LPWSTR>(L"Compatible keeps the original software H.264 settings. Efficient uses a bitrate target at every quality level. Quality H.264 and hardware modes use fixed quantization; detailed or changing scenes can make much larger files. Hardware modes require a supported encoder and do not switch to software if unavailable. HEVC playback needs a compatible player or decoder. File size and image quality depend on the scene and encoder.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.mode);
        tip.lpszText=const_cast<LPWSTR>(L"Separate files records full-frame desktop and camera videos together. The side-by-side preview is only for monitoring; each source has its own MP4.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.advanced);
        tip.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.stopAfter);
        tip.lpszText=const_cast<LPWSTR>(L"Finish and save automatically after this much active recording time. Pauses and initial startup do not count. Never records until you choose Finish.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.lowDisk);
        tip.lpszText=const_cast<LPWSTR>(L"Check free space in the save folder and try to finish and save before space runs out. Other programs or sudden disk changes can still cause a recording to fail. Turn this off to record when free space cannot be checked.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.nightEnabled);
        tip.lpszText=const_cast<LPWSTR>(L"Optional software blending and automatic digital brightness for camera recordings. It does not change camera shutter settings. Motion can blur; clipped or missing detail cannot be recovered. Idle preview is unchanged; the effect appears during recording.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.nightDuration);
        tip.lpszText=const_cast<LPWSTR>(L"Auto chooses a blend duration within the capture interval, up to 30 seconds. A manual duration keeps automatic brightness and must not exceed Capture every. Late blends retain their full duration and delay later captures instead of catching up.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.nightTarget);
        tip.lpszText=const_cast<LPWSTR>(L"Automatic camera brightness uses 8-bit brightness references: Dark 64/255, Balanced 96/255, Bright 128/255. This is processed image brightness, not sensor exposure. Gain and highlight limits can leave the target unmet.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.lpszText=LPSTR_TEXTCALLBACKW;
        for(HWND child:{app.nightHint,app.nightDetail}){tip.uId=reinterpret_cast<UINT_PTR>(child);SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
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
            if(app.engine && wp!=SIZE_MINIMIZED && !app.hiddenToTray && app.visibleDirty)
                applyStatus(app.engine->status(),true);
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
        info->ptMinTrackSize={std::min<LONG>(app.scale(830),work.right-work.left),std::min<LONG>(app.scale(600),work.bottom-work.top)};
        return 0;
    }
    case WM_TIMER: {
        if(wp!=1 || !app.engine)return 0;
        const bool oldRecordingFailed=app.status.recordingFailed;applyStatus(app.engine->status());
        if(app.trayRegistered && !updateTray() && app.hiddenToTray)showWindow();
        if(app.closeWhenDone && !app.active()) {
            app.closeWhenDone=false;
            if(app.status.recordingFailed) {
                // Keep the recovery path visible if saving failed while the
                // user was waiting for Finish and close.
                EnableWindow(w,TRUE);showWindow();
                MessageBoxW(w,app.status.message.c_str(),L"Timelapse could not finish normally",MB_OK|MB_ICONERROR);
            } else DestroyWindow(w);
        } else if(app.hiddenToTray && !oldRecordingFailed && app.status.recordingFailed) {
            showWindow();MessageBoxW(w,app.status.message.c_str(),L"Timelapse could not finish normally",MB_OK|MB_ICONERROR);
        }return 0;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wp),code=HIWORD(wp);
        if(code==CBN_SELCHANGE){
            if(app.active() && (id==NightDurationBox || id==NightTargetBox || id==IntervalBox || id==SizeBox || id==StopAfterBox)){
                if(id==IntervalBox)choose(app.interval,app.committedInterval);
                if(id==SizeBox)choose(app.videoSize,app.committedSize);
                if(id==StopAfterBox)choose(app.stopAfter,app.committedLimit);
                return 0;
            }
            const HWND customBox=id==IntervalBox?app.interval:id==SizeBox?app.videoSize:id==StopAfterBox?app.stopAfter:nullptr;
            const int customAction=id==IntervalBox?6+int(app.hasCustomInterval):id==SizeBox?2+int(app.hasCustomSize):6+int(app.hasCustomLimit);
            if(customBox && choice(customBox)==customAction && SendMessageW(customBox,CB_GETCOUNT,0,0)>customAction){
                SendMessageW(customBox,CB_SHOWDROPDOWN,FALSE,0);editCustom(id==IntervalBox?CustomKind::Interval:id==SizeBox?CustomKind::Size:CustomKind::Limit);return 0;
            }
            if(id==ModeBox)changeLayout(false);
            else {
                if(id==MonitorBox || id==CameraBox) {
                    const HWND box=id==MonitorBox ? app.monitor : app.camera;
                    const int count=static_cast<int>(id==MonitorBox ? app.monitors.size() : app.cameras.size());
                    if(choice(box)>=0 && choice(box)<count)
                        while(SendMessageW(box,CB_GETCOUNT,0,0)>count) SendMessageW(box,CB_DELETESTRING,count,0);
                }
                configure();updateControls();if(id==SizeBox)layout();
            }
            InvalidateRect(w,nullptr,FALSE);return 0;
        }
        switch(id) {
        case AdvancedToggle:toggleAdvanced();break;
        case LowDiskBox:if(!app.active())configure();break;
        case NightBox:if(!app.active()){configure();updateControls();layout();revealFocusedControl();}break;
        case TrayShow:showWindow();break;
        case TrayPause:if(!app.closeWhenDone && (app.status.state==State::Recording || app.status.state==State::Paused))app.engine->setPaused(app.status.state!=State::Paused);break;
        case TrayFinish:if(!app.closeWhenDone)app.engine->finish();break;
        case TrayExit:exitApplication();break;
        case Refresh:refreshSources();app.engine->refreshSources();updateControls();break;
        case Record:configure();if(hasRequiredSources() && app.nightValidation.empty())app.engine->record();applyStatus(app.engine->status(),true);break;
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
        case Reset:if(!app.settings.separateFiles){choose(app.mode,static_cast<int>(app.collagePreset));changeLayout(false);}break;
        case Forward:if(!app.settings.separateFiles && app.selected>=0){auto layer=app.settings.layers[app.selected];app.settings.layers.erase(app.settings.layers.begin()+app.selected);app.settings.layers.push_back(layer);app.selected=static_cast<int>(app.settings.layers.size())-1;customized();}break;
        }return 0;
    }
    case WM_NOTIFY:
        if(reinterpret_cast<NMHDR*>(lp)->code==TTN_GETDISPINFOW){
            auto info=reinterpret_cast<NMTTDISPINFOW*>(lp);const auto child=reinterpret_cast<HWND>(info->hdr.idFrom);
            info->lpszText=const_cast<LPWSTR>((child==app.advanced?app.advancedTooltip:child==app.nightHint?app.nightHintCaption:child==app.nightDetail?app.nightDetailCaption:statusCaption()).c_str());return 0;
        }break;
    case WM_CTLCOLORSTATIC: {
        const auto child=reinterpret_cast<HWND>(lp);const bool warning=(child==app.statusText && statusCaptionError()) || (child==app.nightHint && !app.nightValidation.empty());
        SetBkColor(reinterpret_cast<HDC>(wp),Background);SetTextColor(reinterpret_cast<HDC>(wp),warning?RGB(174,53,44):Muted);return reinterpret_cast<LRESULT>(app.background);
    }
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
        text(dc,app.settings.separateFiles?L"Desktop and camera each save to their own MP4.":app.settings.layers.size()>1?L"Drag a layer to move it. Pull its corner to resize.":L"Preview · your recording is saved at 30 fps",hint,Muted,app.smallFont);
        RECT stats={p,r.bottom-app.scale(147),r.right-p,r.bottom-app.scale(123)};
        std::wstring detail;
        if(app.active() || app.status.frames)detail=std::to_wstring(app.status.frames)+L" frames  ·  "+timeText(double(app.status.frames)/30,false)+L" video  ·  "+timeText(app.status.elapsed,true)+L" recording";
        else {wchar_t buf[140];const double videoSeconds=120000.0/std::max(MinCaptureIntervalMs,app.settings.intervalMs);
            swprintf_s(buf,L"  ·  1 hour becomes %.*f seconds of video",videoSeconds<1?3:videoSeconds<10?1:0,videoSeconds);
            detail=L"Every "+formatDuration(app.settings.intervalMs)+buf;}
        text(dc,detail,stats,Ink,app.font);
        RECT path={p,r.bottom-app.scale(92),r.right-p-app.scale(106),r.bottom-app.scale(66)};text(dc,L"Save to: "+app.settings.folder,path,Muted,app.smallFont,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_PATH_ELLIPSIS);
        EndPaint(w,&ps);return 0;
    }
    case WM_CLOSE:
        hideToTray();return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        // Confirmed session shutdown reaches WM_DESTROY and joins the engine,
        // giving the encoder a chance to finalize before Windows terminates us.
        if(wp){if(app.customDialog)EndDialog(app.customDialog,IDCANCEL);DestroyWindow(w);}
        return 0;
    case WM_DESTROY:removeTray();KillTimer(w,1);if(app.startupComplete)preferences(true);app.startupComplete=false;app.engine.reset();PostQuitMessage(0);return 0;
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
    struct InstanceHandle {HANDLE value{};~InstanceHandle(){if(value)CloseHandle(value);}} instanceHandle;
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
        for(int i=1;i<argumentCount;++i){if(std::wcscmp(arguments[i],L"--inspect-ui")==0)app.inspectUI=true;
            else if(std::wcscmp(arguments[i],L"--tray")==0)app.hiddenToTray=true;}
        LocalFree(arguments);
    }
    // One GUI owns both the tray icon and the installed app's running marker.
    // Camera helper processes return before reaching this path.
    auto setupRunning=[] {
        HANDLE setup=OpenMutexW(SYNCHRONIZE,FALSE,SetupMutexName);
        if(setup){CloseHandle(setup);return true;}
        return GetLastError()!=ERROR_FILE_NOT_FOUND;
    };
    auto setupMessage=[] {MessageBoxW(nullptr,L"Timelapse setup is running. Finish installing or uninstalling, then open Timelapse.",L"Timelapse",MB_OK|MB_ICONINFORMATION);};
    if(setupRunning()){setupMessage();return 1;}
    const HANDLE mutex=CreateMutexW(nullptr,FALSE,InstanceMutexName);
    const DWORD mutexError=GetLastError();instanceHandle.value=mutex;
    if(!mutex) {
        MessageBoxW(nullptr,L"Timelapse could not reserve its application instance. Close any other Timelapse window and try again.",L"Timelapse",MB_OK|MB_ICONERROR);return 1;
    }
    if(setupRunning()){setupMessage();return 1;}
    if(mutexError==ERROR_ALREADY_EXISTS) {
        if(app.hiddenToTray)return 0; // Logon startup must not raise an existing recording window.
        const HWND existing=FindWindowW(L"TimelapseWindow",nullptr);DWORD_PTR result=0;
        if(existing && SendMessageTimeoutW(existing,ShowExistingMessage,0,0,SMTO_ABORTIFHUNG,2000,&result))return 0;
        MessageBoxW(nullptr,L"Timelapse is already starting or is busy. Use its system tray icon, or try again in a moment.",L"Timelapse",MB_OK|MB_ICONINFORMATION);return 1;
    }
    app.taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW preview{sizeof(preview)};preview.lpfnWndProc=previewProc;preview.hInstance=instance;preview.hCursor=LoadCursorW(nullptr,IDC_ARROW);preview.lpszClassName=L"LapsePreview";RegisterClassExW(&preview);
    WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=windowProc;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);cls.hbrBackground=app.background;cls.lpszClassName=L"TimelapseWindow";RegisterClassExW(&cls);
    app.dpi=static_cast<int>(GetDpiForSystem());
    POINT cursor{};GetCursorPos(&cursor);
    const RECT work=workArea(MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY));
    const int margin=app.scale(16);
    const int width=std::min(app.scale(920),std::max(1,static_cast<int>(work.right-work.left)-2*margin));
    const int height=std::min(app.scale(680),std::max(1,static_cast<int>(work.bottom-work.top)-2*margin));
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"Timelapse",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,instance,nullptr);
    if(!window){windowCreationFailed=true;return 1;}
    if(const HMENU menu=GetSystemMenu(window,FALSE)){AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,ExitSystemCommand,L"E&xit Timelapse");}
    updateTray(true);
    if(app.hiddenToTray && app.trayRegistered)ShowWindow(window,SW_HIDE);
    else {app.hiddenToTray=false;ShowWindow(window,show==SW_HIDE?SW_SHOWNORMAL:show);configure();}
    UpdateWindow(window);
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

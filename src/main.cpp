#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include "config.h"
#include "hotkeys.h"
#include "time_skip.h"
#include "person_pack.h"
#include "watermark.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>
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
#include <initializer_list>
#include <atomic>
#include <process.h>

using namespace lapse;
namespace {
// Light Fluent-style palette. Controls stay native; the window itself paints
// only flat fills, a few antialiased rounded shapes and text.
constexpr COLORREF Ink = RGB(27, 31, 36), Muted = RGB(92, 101, 110), Subtle = RGB(138, 146, 154);
constexpr COLORREF Accent = RGB(0, 116, 113), AccentSoft = RGB(222, 241, 239);
constexpr COLORREF Background = RGB(243, 244, 246), Panel = RGB(255, 255, 255), Border = RGB(222, 226, 230);
constexpr COLORREF Canvas = RGB(23, 27, 32), Danger = RGB(196, 43, 28), DangerSoft = RGB(253, 233, 231);
constexpr COLORREF Warning = RGB(138, 82, 0), WarningSoft = RGB(255, 243, 205), Info = RGB(0, 95, 184), InfoSoft = RGB(228, 240, 251);
// Logical (96 DPI) geometry. Settings live in a fixed-width panel on the right
// that scrolls on its own; the stage on the left keeps preview and transport.
constexpr int PanelWidth = 328, PanelPad = 20, StagePad = 20, StageMinWidth = 560;
constexpr int HeaderTop = 14, HeaderHeight = 32, PreviewGap = 10, PreviewMinHeight = 160;
constexpr int TransportGap = 14, TransportHeight = 64, StatusGap = 8, StatusHeight = 26, StageBottom = 12;
constexpr int StageMinHeight = HeaderTop + HeaderHeight + PreviewGap + PreviewMinHeight + TransportGap + TransportHeight + StatusGap + StatusHeight + StageBottom;
// Horizontal space the Advanced disclosure reserves beside its caption text:
// left inset, chevron, the gap before it and the right inset.
constexpr int DisclosureChrome = 46;
enum Id { ModeBox = 100, IntervalBox, SizeBox, EncodingQualityBox, MonitorBox, CameraBox, Refresh, Record, Pause, Finish, Folder, OpenFolder, Reset, Forward, Preview, EncodingModeBox, AdvancedToggle, StopAfterBox, LowDiskBox, NightBox, NightDurationBox, NightTargetBox, NightHint, NightDetail, SkipConfigure, SkipSummary, SkipDetail, RecoveryBox, SegmentBox, WatermarkConfigure, WatermarkSummary, StatusDetails, CursorBox, StartDelayBox, PlaybackConfigure };
constexpr int StartDelays[] = {0,5,10,30,60,300};
constexpr const wchar_t* StartDelayLabels[] = {L"None",L"5 seconds",L"10 seconds",L"30 seconds",L"1 minute",L"5 minutes"};
constexpr int RecordingLimits[] = {0,900,3600,14400,28800,86400};
constexpr int CaptureIntervals[] = {1000,2000,5000,10000,30000,60000};
constexpr const wchar_t* RecordingLimitLabels[] = {L"Never",L"15 minutes",L"1 hour",L"4 hours",L"8 hours",L"24 hours"};
constexpr const wchar_t* RecordingLimitShortLabels[] = {L"Never",L"15 min",L"1 hour",L"4 hours",L"8 hours",L"24 hours"};
constexpr int SegmentDurations[] = {0,900,3600,21600,86400};
constexpr const wchar_t* SegmentLabels[] = {L"Never",L"15 minutes",L"1 hour",L"6 hours",L"24 hours"};
constexpr wchar_t SegmentHelp[] = L"Split by active recording time, not video length. Pauses and initial preparation do not count; automatic saving does. Empty periods create no files. Shorter parts add processing and file overhead. Paired desktop/camera files share each part. No crash or power-loss guarantee.";
constexpr int NightDurations[] = {0,1000,2000,5000,10000,30000};
constexpr const wchar_t* NightDurationLabels[] = {L"Auto",L"1 second",L"2 seconds",L"5 seconds",L"10 seconds",L"30 seconds"};
constexpr int NightTargets[] = {64,96,128};
constexpr const wchar_t* EncodingModeLabels[] = {L"Compatible H.264 (default)",L"Efficient H.264 (bitrate target)",L"Hardware H.264 (CPU offload)",L"Hardware HEVC (HEVC player)",L"Quality H.264 (detail)"};
constexpr const wchar_t* SkipModeLabels[] = {L"Off",L"Scene is quiet",L"Inside scheduled ranges",L"Quiet scene + schedule",L"No person detected (camera)",L"No person + schedule (camera)"};
constexpr int SkipMultipliers[] = {2,4,8,16,32,64}, SkipRamps[] = {15,30,60};
constexpr int SeparateFilesMode = 5;
constexpr wchar_t SeparateFilesLabel[] = L"Desktop + camera (2 files)";
constexpr UINT TrayMessage = WM_APP + 1, ShowExistingMessage = WM_APP + 2;
constexpr UINT CancelOwnedWorkMessage = WM_APP + 3;
constexpr UINT TrayShow = 4001, TrayPause = 4002, TrayFinish = 4003, TrayExit = 4004, TrayProgress = 4005;
constexpr UINT ExitSystemCommand = 0x1000;
constexpr int FirstRecordingHotkeyId = 4401, LastRecordingHotkeyId = 4404;
constexpr wchar_t InstanceMutexName[] = L"Local\\Timelapse.Application.{DC32D155-1B8D-4880-9902-CE6245D34923}";
constexpr wchar_t SetupMutexName[] = L"Local\\Timelapse.Setup.{DC32D155-1B8D-4880-9902-CE6245D34923}";
enum class FailureNotice { None, Pending, Presenting, Presented };
enum class OpenFolderPhase { Preparing, Cancelled, Dispatching };
enum class OpenFolderResult { Opened, DirectoryUnavailable, ShellFailure, ComFailure, OutOfMemory, StartFailure, Failed, Cancelled };
struct OpenFolderTask {
    std::wstring folder;
    std::atomic<OpenFolderPhase> phase{OpenFolderPhase::Preparing};
    std::atomic<bool> done{false};
    OpenFolderResult result=OpenFolderResult::Failed;
    bool suppressError=false; // UI-only: a newer recording notice takes priority.
    void cancel() noexcept {auto expected=OpenFolderPhase::Preparing;phase.compare_exchange_strong(expected,OpenFolderPhase::Cancelled);}
};
// Both Explorer actions share one bounded slot, including native cleanup after
// their UI has closed. A blocked provider cannot accumulate replacement jobs.
std::atomic<bool> shellOperationBusy{false};
struct App {
    struct SizeSuggestion {
        int item=-1,width=0,height=0;
        int64_t sourceWidth=0,sourceHeight=0;
        uint64_t generation=0;
        RECT bounds{};
        std::wstring sourceId;
    };
    HWND window{}, preview{}, statusText{}, statusDetails{}, tooltip{};
    HWND customDialog{};
    HWND skipConfigure{},skipSummary{},skipDetail{};
    HWND watermarkConfigure{},watermarkSummary{},playbackConfigure{};
    uint16_t pauseHotkey=0,stopHotkey=0;
    int pauseHotkeyId=0,stopHotkeyId=0,recordedOutputFps=DefaultOutputFps;
    std::wstring hotkeyWarning;
    HWND mode{}, interval{}, videoSize{}, encodingQuality{}, encodingMode{}, monitor{}, camera{}, refresh{}, record{}, pause{}, finish{}, folder{}, openFolder{}, reset{}, forward{};
    HWND advanced{}, stopAfter{}, lowDisk{}, recoveryMode{}, nightEnabled{}, nightDuration{}, nightTarget{}, nightHint{}, nightDetail{}, labels[10]{};
    HWND segmentLabel{}, splitEvery{}, captureCursor{};
    HWND startDelayLabel{}, startDelay{}, startDelayHint{};
    HFONT font{}, titleFont{}, smallFont{}, strongFont{}, headerFont{};
    HBRUSH background = CreateSolidBrush(Background), panelBrush = CreateSolidBrush(Panel);
    HICON appIcons[2]{};
    int dpi = 96, selected = -1, modeIndex = 0;
    Mode collagePreset = Mode::Overlay;
    bool dragging = false, resizing = false, closeWhenDone = false, inspectUI = false;
    bool layingOut = false;
    bool startupComplete = false;
    FailureNotice failureNotice = FailureNotice::None;
    bool trayMenuOpen = false, trayMenuCanceled = false;
    std::shared_ptr<OpenFolderTask> openFolderTask;
    bool shellBusyObserved = false, openFolderBusyShown = false;
    bool visibleDirty = true, controlsUpdated = false;
    State controlsState = State::Idle;
    bool advancedExpanded = false;
    int advancedLimitIndex = -1, advancedVisibility = -1, advancedNightState = -1, advancedRecoveryState = -1, nightVisibility = -1;
    int advancedSegmentSeconds = -1,advancedOutputFps=-1;
    int advancedCursorState = -1, cursorVisibility = -1, advancedDelaySeconds = -1, committedStartDelay = 0;
    uint64_t waitingRemaining = UINT64_MAX;
    std::wstring waitingCaption;
    int customIntervalMs=5000, customWidth=1280, customHeight=720, customLimitSeconds=900;
    int committedInterval=2, committedSize=0, committedLimit=0;
    bool hasCustomInterval=false, hasCustomSize=false, hasCustomLimit=false;
    std::array<SizeSuggestion,2> sizeSuggestions{};
    std::wstring sizeTooltip;
    int customSegmentSeconds=900, committedSegment=0;
    bool hasCustomSegment=false;
    int customNightDurationMs=NightInitialDurationMs, committedNightDuration=0;
    bool hasCustomNightDuration=false;
    std::wstring advancedCaption, advancedTooltip;
    std::wstring skipSummaryCaption,skipDetailCaption;
    int skipRevision=0,advancedSkipRevision=-1,skipSummaryRevision=-1,skipVisibility=-1;
    uint64_t skipCheckAge=UINT64_MAX;
    WatermarkSettings watermarkChecked;
    bool watermarkCheckValid=false;
    int watermarkWidth=0,watermarkHeight=0,watermarkRevision=0,advancedWatermarkRevision=-1;
    std::wstring watermarkValidation,watermarkCaption;
    PersonPackInfo personPack{};
    bool personPackKnown=false;
    std::wstring nightValidation, encodingValidation, statusCaption, nightHintCaption, nightDetailCaption;
    std::wstring cameraListError, statusTooltipCaption;
    bool statusCaptionError = false;
    int statusDetailsVisible = -1;
    bool hiddenToTray = false, trayRegistered = false, trayNoticeShown = false, trayVersion4 = false;
    UINT taskbarCreated = 0;
    std::wstring trayTooltip;
    State trayState = State::Idle;
    bool trayStateValid = false, trayFailure = false, traySeparate = false;
    HICON trayIcons[5]{};
    int contentWidth = 0, contentHeight = 0, scrollX = 0, scrollY = 0;
    int wheelVertical = 0, wheelHorizontal = 0, wheelPanel = 0;
    // Docked: the stage fits the viewport and the window's vertical bar scrolls
    // only the settings panel. Otherwise the whole canvas scrolls (panelScroll=0).
    bool panelDocked = true;
    int panelScroll = 0, panelHeight = 0, advancedTop = 0;
    bool collageTools = false, advancedWarning = false;
    // Painted geometry in logical canvas coordinates, refreshed by layout().
    // Panel-relative rows are stored unscrolled; paint subtracts panelScroll.
    RECT panelRect{}, headerRect{}, previewRect{}, transportRect{}, statsRect{}, statusRect{}, savePathRect{};
    int sectionTops[3]{}, panelRules[6]{}, panelRuleCount = 0;
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
    ~App() {
        if(openFolderTask)openFolderTask->cancel();for(auto icon:trayIcons)if(icon)DestroyIcon(icon);for(auto icon:appIcons)if(icon)DestroyIcon(icon);
        for(HGDIOBJ object:{static_cast<HGDIOBJ>(font),static_cast<HGDIOBJ>(titleFont),static_cast<HGDIOBJ>(smallFont),static_cast<HGDIOBJ>(strongFont),static_cast<HGDIOBJ>(headerFont),
            static_cast<HGDIOBJ>(background),static_cast<HGDIOBJ>(panelBrush)})DeleteObject(object);
    }
} app;
void removeTray();
void unregisterRecordingHotkeys() noexcept {
    if(app.pauseHotkeyId)UnregisterHotKey(app.window,app.pauseHotkeyId);
    if(app.stopHotkeyId)UnregisterHotKey(app.window,app.stopHotkeyId);
    app.pauseHotkeyId=app.stopHotkeyId=0;
}
bool setRecordingHotkeys(uint16_t pause,uint16_t stop,std::wstring& error) {
    if(!validateHotkeys(pause,stop,error))return false;
    struct Binding {uint16_t value;int id;};
    const Binding previous[]={{app.pauseHotkey,app.pauseHotkeyId},{app.stopHotkey,app.stopHotkeyId}};
    Binding next[]={{pause,0},{stop,0}};
    bool created[2]{};
    // Reuse retained combinations, including a swap of the two actions. New
    // combinations use spare ids until the whole draft succeeds, so a conflict
    // never silently disables a user's previous working shortcut.
    for(int action=0;action<2;++action){
        if(!next[action].value)continue;
        for(const auto& old:previous)if(old.id && hotkeyIdentity(old.value)==hotkeyIdentity(next[action].value)){next[action].id=old.id;break;}
        if(next[action].id)continue;
        for(int id=FirstRecordingHotkeyId;id<=LastRecordingHotkeyId;++id){
            if(id==previous[0].id || id==previous[1].id || id==next[0].id || id==next[1].id)continue;
            next[action].id=id;break;
        }
        if(!next[action].id || !RegisterHotKey(app.window,next[action].id,hotkeyRegistrationModifiers(next[action].value),LOBYTE(next[action].value))){
            for(int i=0;i<2;++i)if(created[i])UnregisterHotKey(app.window,next[i].id);
            error=std::wstring(action==0?L"Pause / resume":L"Stop and save")+L" shortcut is unavailable. Windows or another app may be using it. Choose another combination.";
            return false;
        }
        created[action]=true;
    }
    for(const auto& old:previous)if(old.id && old.id!=next[0].id && old.id!=next[1].id)UnregisterHotKey(app.window,old.id);
    app.pauseHotkey=pause;app.stopHotkey=stop;app.pauseHotkeyId=next[0].id;app.stopHotkeyId=next[1].id;
    app.hotkeyWarning.clear();error.clear();return true;
}
void cancelOpenFolder() noexcept;
void refreshOpenFolderControl();
void pollOpenFolder();
void cancelOwnedDialogs() {
    // EndDialog returns before destruction: walk ownership, not the global
    // pointer, so an open range editor and its settings dialog both terminate.
    for(HWND dialog=app.customDialog;dialog && dialog!=app.window;){
        HWND owner=GetWindow(dialog,GW_OWNER);SendMessageW(dialog,CancelOwnedWorkMessage,0,0);EndDialog(dialog,IDCANCEL);dialog=owner;
    }
}

struct MediaRuntime {
    MediaRuntime() = default;
    MediaRuntime(const MediaRuntime&) = delete;
    MediaRuntime& operator=(const MediaRuntime&) = delete;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT media = SUCCEEDED(com) ? MFStartup(MF_VERSION, MFSTARTUP_LITE) : E_UNEXPECTED;
    ~MediaRuntime() {
        cancelOpenFolder();
        // Join the worker before releasing its media runtime, including when
        // startup or message retrieval exits without receiving WM_DESTROY.
        app.engine.reset();
        removeTray();
        unregisterRecordingHotkeys();
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
int textWidth(HDC dc, const wchar_t* value, HFONT font) {
    SIZE size{}; const auto old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, value, static_cast<int>(std::wcslen(value)), &size); SelectObject(dc, old);
    return size.cx;
}
COLORREF blend(COLORREF from, COLORREF to, double amount) {
    amount = std::clamp(amount, 0.0, 1.0);
    const auto channel = [&](int a, int b) { return static_cast<BYTE>(std::lround(a + (b - a) * amount)); };
    return RGB(channel(GetRValue(from), GetRValue(to)), channel(GetGValue(from), GetGValue(to)), channel(GetBValue(from), GetBValue(to)));
}
void solid(HDC dc, const RECT& area, COLORREF color) {
    if (area.right <= area.left || area.bottom <= area.top) return;
    const COLORREF previous = SetDCBrushColor(dc, color);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); SetDCBrushColor(dc, previous);
}
// Antialiased rounded rectangle over a known opaque backdrop. Straight spans are
// plain fills; only the four radius-sized corner squares are shaded per pixel,
// so cost follows the radius, not the area, and no brush or bitmap is retained.
void roundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF backdrop, COLORREF border = CLR_INVALID, int stroke = 0) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    if (border == CLR_INVALID) stroke = 0;
    stroke = std::clamp(stroke, 0, std::min(w, h) / 2);
    radius = std::clamp(std::max(radius, stroke), 0, std::min({w / 2, h / 2, 48}));
    const int s = std::min(stroke, radius);
    if (s) {
        solid(dc, {r.left + radius, r.top, r.right - radius, r.top + s}, border);
        solid(dc, {r.left + radius, r.bottom - s, r.right - radius, r.bottom}, border);
        solid(dc, {r.left, r.top + radius, r.left + s, r.bottom - radius}, border);
        solid(dc, {r.right - s, r.top + radius, r.right, r.bottom - radius}, border);
    }
    solid(dc, {r.left + radius, r.top + s, r.right - radius, r.bottom - s}, fill);
    solid(dc, {r.left + s, r.top + radius, r.left + radius, r.bottom - radius}, fill);
    solid(dc, {r.right - radius, r.top + radius, r.right - s, r.bottom - radius}, fill);
    if (!radius) return;
    std::array<uint32_t, 48 * 48> pixels{};
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = radius; info.bmiHeader.biHeight = -radius;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    for (int corner = 0; corner < 4; ++corner) {
        for (int y = 0; y < radius; ++y) for (int x = 0; x < radius; ++x) {
            // Distance from this corner's arc centre, sampled at pixel centres.
            const double dx = (corner & 1) ? x + .5 : radius - (x + .5), dy = (corner & 2) ? y + .5 : radius - (y + .5);
            const double distance = std::sqrt(dx * dx + dy * dy), outer = std::clamp(radius - distance + .5, 0.0, 1.0);
            const COLORREF color = s ? blend(blend(backdrop, border, outer), fill, std::clamp(radius - s - distance + .5, 0.0, 1.0)) : blend(backdrop, fill, outer);
            pixels[size_t(y) * radius + x] = (uint32_t(GetRValue(color)) << 16) | (uint32_t(GetGValue(color)) << 8) | GetBValue(color);
        }
        StretchDIBits(dc, (corner & 1) ? r.right - radius : r.left, (corner & 2) ? r.bottom - radius : r.top, radius, radius,
            0, 0, radius, radius, pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
}
// Straight-alpha 32-bit icon rendered from signed distances, so edges stay
// smooth at every system icon size without an icon resource or GDI+.
template<class Shade> HICON shadedIcon(int size, Shade shade) {
    size = std::clamp(size, 16, 256);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = size; info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    // Word-aligned 1-bpp rows for the largest size; static, so callbacks never allocate here.
    static const std::array<BYTE, 32 * 256> emptyMask{};
    void* bits = nullptr; HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = color ? CreateBitmap(size, size, 1, 1, emptyMask.data()) : nullptr;
    HICON icon = nullptr;
    if (color && mask && bits) {
        auto pixels = static_cast<uint32_t*>(bits);
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            double alpha = 0; const COLORREF value = shade((x + .5) / size, (y + .5) / size, 1.0 / size, alpha);
            pixels[size_t(y) * size + x] = (uint32_t(std::lround(std::clamp(alpha, 0.0, 1.0) * 255)) << 24) |
                (uint32_t(GetRValue(value)) << 16) | (uint32_t(GetGValue(value)) << 8) | GetBValue(value);
        }
        ICONINFO parts{}; parts.fIcon = TRUE; parts.hbmColor = color; parts.hbmMask = mask; icon = CreateIconIndirect(&parts);
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}
double coverage(double signedDistance, double pixel) { return std::clamp(.5 - signedDistance / pixel, 0.0, 1.0); }
double boxDistance(double x, double y, double cx, double cy, double hx, double hy, double radius) {
    const double qx = std::abs(x - cx) - hx + radius, qy = std::abs(y - cy) - hy + radius;
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
}
double roundedBoxDistance(double x, double y, double half, double radius) { return boxDistance(x, y, .5, .5, half, half, radius); }
double segmentDistance(double x, double y, double ax, double ay, double bx, double by) {
    const double px = x - ax, py = y - ay, vx = bx - ax, vy = by - ay;
    const double t = std::clamp((px * vx + py * vy) / (vx * vx + vy * vy), 0.0, 1.0);
    return std::hypot(px - vx * t, py - vy * t);
}
// Teal tile with a white clock face: recognizable in the taskbar and Alt+Tab.
HICON createAppIcon(int size) {
    return shadedIcon(size, [](double x, double y, double pixel, double& alpha) {
        alpha = coverage(roundedBoxDistance(x, y, .46, .2), pixel);
        const double ring = std::abs(std::hypot(x - .5, y - .5) - .27) - .045;
        const double hands = std::min(segmentDistance(x, y, .5, .5, .5, .32), segmentDistance(x, y, .5, .5, .62, .58)) - .04;
        return blend(blend(RGB(0, 138, 133), RGB(0, 100, 97), y), RGB(255, 255, 255), coverage(std::min(ring, hands), pixel));
    });
}
std::wstring timeText(double seconds, bool hours) {
    auto n = static_cast<uint64_t>(std::max(0.0, seconds)); wchar_t value[80];
    if (hours) swprintf_s(value,L"%02llu:%02llu:%02llu",n/3600,n/60%60,n%60);
    else swprintf_s(value,L"%02llu:%02llu",n/60,n%60);
    return value;
}
uint64_t startDelayRemaining(const Status& status,uint64_t now) noexcept {
    if(status.state!=State::Waiting || !status.startDeadlineTick)return UINT64_MAX;
    const auto milliseconds=status.startDeadlineTick>now?status.startDeadlineTick-now:0;
    return milliseconds/1000+(milliseconds%1000!=0);
}
bool waitingProgressText(const Status& status,wchar_t (&value)[160],uint64_t now=GetTickCount64()) noexcept {
    if(status.state!=State::Waiting)return false;
    const auto seconds=startDelayRemaining(status,now);
    if(seconds==UINT64_MAX)return wcscpy_s(value,L"Waiting to start.")==0;
    if(!seconds)return wcscpy_s(value,L"Waiting to start: preparing shortly.")==0;
    return swprintf_s(value,L"Waiting to start: %02llu:%02llu remaining",seconds/60,seconds%60)>=0;
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
int selectedStartDelay() { return app.startDelay?StartDelays[std::clamp(choice(app.startDelay),0,5)]:0; }
int selectedLimit() {
    const int selected=choice(app.stopAfter);
    return app.hasCustomLimit && selected==6 ? app.customLimitSeconds : RecordingLimits[std::clamp(selected,0,5)];
}
int selectedSegment() {
    const int selected=app.splitEvery?choice(app.splitEvery):0;
    return app.hasCustomSegment && selected==5 ? app.customSegmentSeconds : SegmentDurations[std::clamp(selected,0,4)];
}
int selectedNightDuration() {
    if(!app.nightDuration)return 0;
    const int selected=choice(app.nightDuration);
    const int committed=selected>=0 && selected<=(app.hasCustomNightDuration?6:5)?selected:app.committedNightDuration;
    return app.hasCustomNightDuration && committed==6 ? app.customNightDurationMs : NightDurations[std::clamp(committed,0,5)];
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
    app.sizeSuggestions={};
    customItems(app.stopAfter,6,app.hasCustomLimit,formatDuration(int64_t(app.customLimitSeconds)*1000,true),app.committedLimit);
    customItems(app.splitEvery,5,app.hasCustomSegment,formatDuration(int64_t(app.customSegmentSeconds)*1000,true),app.committedSegment);
    customItems(app.nightDuration,6,app.hasCustomNightDuration,formatDuration(app.customNightDurationMs),app.committedNightDuration);
}
void normalizeCustomSelections() {
    if(app.hasCustomInterval)for(int i=0;i<6;++i)if(CaptureIntervals[i]==app.customIntervalMs){app.hasCustomInterval=false;app.committedInterval=i;break;}
    if(app.hasCustomSize && ((app.customWidth==1280 && app.customHeight==720) || (app.customWidth==1920 && app.customHeight==1080))){
        app.hasCustomSize=false;app.committedSize=app.customWidth==1280?0:1;
    }
    if(app.hasCustomLimit)for(int i=1;i<6;++i)if(RecordingLimits[i]==app.customLimitSeconds){app.hasCustomLimit=false;app.committedLimit=i;break;}
    if(app.hasCustomSegment)for(int i=0;i<5;++i)if(SegmentDurations[i]==app.customSegmentSeconds){app.hasCustomSegment=false;app.committedSegment=i;break;}
    if(app.hasCustomNightDuration)for(int i=0;i<6;++i)if(NightDurations[i]==app.customNightDurationMs){app.hasCustomNightDuration=false;app.committedNightDuration=i;break;}
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
constexpr wchar_t VideoSizeHelp[]=L"Use copies the selected source size once. Fit scales it down to supported even dimensions. Output stays fixed when sources change. Camera input is up to 1280 × 720 for smaller outputs, or 1920 × 1080 for larger outputs if supported. Camera suggestions show the input actually received.";
struct SourceSizeChoice {
    int width=0,height=0;bool fitted=false;const wchar_t* reason=L"";
    int64_t sourceWidth=0,sourceHeight=0;
    uint64_t generation=0;
    RECT bounds{};
    std::wstring sourceId;
};
std::array<SourceSizeChoice,2> sourceSizeChoices() {
    std::array<SourceSizeChoice,2> result{};
    const auto set=[&](size_t index,int64_t width,int64_t height){
        const auto output=sourceVideoDimensions(width,height);auto& value=result[index];
        value.width=output.first;value.height=output.second;value.fitted=width!=output.first || height!=output.second;
        value.sourceWidth=width;value.sourceHeight=height;
        if(!value.width)value.reason=L"Source dimensions cannot fit the supported output limits.";
    };
    const int monitor=choice(app.monitor),camera=choice(app.camera);
    if(hasSource(Source::Desktop)) {
        if(monitor>=0 && monitor<static_cast<int>(app.monitors.size())) {
            // Display topology can change while the dropdown is open. Resolve
            // current bounds on demand without changing the source selection.
            const auto current=enumerateMonitors();const auto& id=app.monitors[monitor].id;
            const auto source=std::find_if(current.begin(),current.end(),[&](const auto& value){return sameSourceId(id,value.id);});
            if(source!=current.end()) { const auto& rect=source->bounds;
                set(0,int64_t(rect.right)-rect.left,int64_t(rect.bottom)-rect.top);result[0].sourceId=source->id;result[0].bounds=rect;
            } else result[0].reason=L"The selected screen is unavailable. Refresh sources to retry.";
        } else result[0].reason=L"Screen size is unavailable. Refresh sources to retry.";
    }
    if(hasSource(Source::Camera)) {
        // configure/Refresh clear the engine's metadata synchronously; do not
        // reuse a potentially older UI timer snapshot after a source change.
        const auto input=app.engine?app.engine->status().cameraInput:app.status.cameraInput;
        if(camera>=0 && camera<static_cast<int>(app.cameras.size()) && sameSourceId(app.settings.cameraId,app.cameras[camera].id) && input.generation && input.width>0 && input.height>0){
            set(1,input.width,input.height);result[1].sourceId=app.cameras[camera].id;result[1].generation=input.generation;
        }
        else result[1].reason=L"Camera input size is unavailable until a current frame arrives.";
    }
    return result;
}
void refreshSizeSuggestions() noexcept {
    if(!app.videoSize)return;
    app.sizeSuggestions={};
    const int first=3+int(app.hasCustomSize); // Keep the existing Custom... index stable.
    while(SendMessageW(app.videoSize,CB_GETCOUNT,0,0)>first)SendMessageW(app.videoSize,CB_DELETESTRING,first,0);
    try {
        const auto choices=sourceSizeChoices();
        for(size_t i=0;i<choices.size();++i)if(choices[i].width) {
            const auto& value=choices[i];
            const std::wstring label=std::wstring(value.fitted?L"Fit ":L"Use ")+(i?L"camera input: ":L"screen: ")+sizeText(value.width,value.height);
            const auto item=SendMessageW(app.videoSize,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(item!=CB_ERR && item!=CB_ERRSPACE)app.sizeSuggestions[i]={static_cast<int>(item),value.width,value.height,value.sourceWidth,value.sourceHeight,value.generation,value.bounds,value.sourceId};
        }
        int width=app.scale(160);HDC dc=GetDC(app.videoSize);
        if(dc){auto old=SelectObject(dc,app.font);wchar_t label[128]{};
            for(int i=0;i<SendMessageW(app.videoSize,CB_GETCOUNT,0,0);++i)if(SendMessageW(app.videoSize,CB_GETLBTEXTLEN,i,0)<128){
                SendMessageW(app.videoSize,CB_GETLBTEXT,i,reinterpret_cast<LPARAM>(label));SIZE measured{};
                if(GetTextExtentPoint32W(dc,label,static_cast<int>(std::wcslen(label)),&measured))width=std::max(width,static_cast<int>(measured.cx)+app.scale(36));
            }
            SelectObject(dc,old);ReleaseDC(app.videoSize,dc);
        }
        SendMessageW(app.videoSize,CB_SETDROPPEDWIDTH,width,0);
    } catch (...) {
        while(SendMessageW(app.videoSize,CB_GETCOUNT,0,0)>first)SendMessageW(app.videoSize,CB_DELETESTRING,first,0);
        app.sizeSuggestions={};
    }
    choose(app.videoSize,app.committedSize);
}
bool currentSizeSuggestion(size_t source,const App::SizeSuggestion& suggestion) noexcept {
    try {
        const auto choices=sourceSizeChoices();const auto& current=choices[source];
        return current.width==suggestion.width && current.height==suggestion.height && current.width>0 &&
            current.sourceWidth==suggestion.sourceWidth && current.sourceHeight==suggestion.sourceHeight &&
            current.generation==suggestion.generation && sameSourceId(current.sourceId,suggestion.sourceId) &&
            EqualRect(&current.bounds,&suggestion.bounds);
    } catch (...) { return false; }
}
const wchar_t* videoSizeTooltip() noexcept {
    try {
        std::wstring text=VideoSizeHelp;const auto choices=sourceSizeChoices();
        for(const auto& value:choices)if(value.reason[0])text+=L"\n"+std::wstring(value.reason);
        app.sizeTooltip=std::move(text);return app.sizeTooltip.c_str();
    } catch (...) { return VideoSizeHelp; }
}
void layout();
bool skipEnabled() { return app.settings.timeSkip.mode!=TimeSkipMode::Off; }
bool skipScheduled(TimeSkipMode mode) { return mode==TimeSkipMode::Manual || mode==TimeSkipMode::QuietWithinSchedule || mode==TimeSkipMode::NoPersonWithinSchedule; }
bool skipAutomatic(TimeSkipMode mode) { return mode==TimeSkipMode::Quiet || mode==TimeSkipMode::QuietWithinSchedule; }
bool skipPerson(TimeSkipMode mode) { return mode==TimeSkipMode::NoPerson || mode==TimeSkipMode::NoPersonWithinSchedule; }
bool skipObserved(TimeSkipMode mode) { return skipAutomatic(mode) || skipPerson(mode); }
TimeSkipMode skipMode(HWND control) { return static_cast<TimeSkipMode>(std::clamp(choice(control),0,static_cast<int>(std::size(SkipModeLabels))-1)); }
std::wstring personAvailability() {
    if(!hasSource(Source::Camera))return L"Select camera content for person checks; recording otherwise uses the normal capture interval.";
    if(!app.personPackKnown)return L"Camera person checks require the optional detector; manage it in Time compression.";
    if(app.personPack.state!=PersonPackState::Ready)return L"Person detector unavailable; recording uses the normal capture interval. Manage it in Time compression.";
    return L"Checks the selected camera, not the desktop. Missing, uncertain or stale checks keep the normal capture interval.";
}
std::wstring skipSummary(const TimeSkipSettings& policy,int intervalMs) {
    if(policy.mode==TimeSkipMode::Off)return L"Off";
    return std::wstring(SkipModeLabels[std::clamp(static_cast<int>(policy.mode),0,static_cast<int>(std::size(SkipModeLabels))-1)])+L" · up to "+std::to_wstring(policy.multiplier)+
        L"× · target up to "+formatDuration(int64_t(intervalMs)*policy.multiplier,true)+L" between frames";
}
void updateSkipText(bool force=false) {
    if(!app.skipSummary || !app.advancedExpanded)return;
    if(app.hiddenToTray || IsIconic(app.window)){app.visibleDirty=true;return;}
    if(force || app.skipSummaryRevision!=app.skipRevision){
        const auto summary=skipSummary(app.settings.timeSkip,app.settings.intervalMs);
        if(summary!=app.skipSummaryCaption){SetWindowTextW(app.skipSummary,summary.c_str());app.skipSummaryCaption=summary;}
        app.skipSummaryRevision=app.skipRevision;
    }
    if(!skipEnabled())return;
    std::wstring detail=L"Recording-time capture spacing; schedules use active time and exclude pauses.";
    if(skipPerson(app.settings.timeSkip.mode))detail=personAvailability();
    const auto& value=app.status.timeSkip;
    if(app.active() && value.enabled){
        if(app.status.state==State::Paused)detail=L"Paused; source checks and the schedule clock are paused.";
        else {
            const wchar_t* reason=value.reason==TimeSkipReason::Quiet?L"Quiet":value.reason==TimeSkipReason::Manual?L"Scheduled":
                value.reason==TimeSkipReason::NoPerson?L"No person detected":value.reason==TimeSkipReason::PersonPresent?L"Person detected":
                value.reason==TimeSkipReason::PersonUncertain?L"Person check uncertain":
                value.reason==TimeSkipReason::Checking?(skipPerson(app.settings.timeSkip.mode)?L"Checking for absence":L"Checking image changes"):
                value.reason==TimeSkipReason::Unavailable?L"Checks unavailable":L"Normal cadence";
            detail=reason;
            if(value.intervalMs>0)detail+=L" · target every "+formatDuration(value.intervalMs,true);
            if(skipObserved(app.settings.timeSkip.mode)){
                const uint64_t now=GetTickCount64();
                app.skipCheckAge=value.lastCheckTick && now>=value.lastCheckTick?(now-value.lastCheckTick)/1000:UINT64_MAX;
                detail+=app.skipCheckAge==UINT64_MAX?L" · waiting for source checks":L" · last source check "+std::to_wstring(app.skipCheckAge)+L" s ago";
                if(value.observationDelayed)detail+=L" · delayed";
            }
            if(value.diagnostic[0])detail+=L" · "+std::wstring(value.diagnostic.data(),wcsnlen_s(value.diagnostic.data(),value.diagnostic.size()));
        }
    }
    if(force || detail!=app.skipDetailCaption){SetWindowTextW(app.skipDetail,detail.c_str());app.skipDetailCaption=std::move(detail);}
}
int nightRow() { return hasSource(Source::Camera) ? (app.settings.night.enabled?2:1) : 0; }
bool cameraListUnavailable() {
    if(app.active() || app.cameraListError.empty() || !hasSource(Source::Camera))return false;
    const int selected=choice(app.camera);
    return selected<0 || selected>=static_cast<int>(app.cameras.size());
}
const std::wstring& statusCaption() {
    if(!app.active() && !app.status.error && !app.encodingValidation.empty())return app.encodingValidation;
    if(!app.active() && !app.status.error && !app.nightValidation.empty())return app.nightValidation;
    if(!app.active() && !app.status.error && !app.watermarkValidation.empty())return app.watermarkValidation;
    if(!app.status.error && !app.status.recordingFailed && app.status.savedPath.empty() && app.status.savedPaths.empty() && cameraListUnavailable())
        return app.cameraListError;
    if(app.status.state==State::Waiting && !app.status.error && !app.status.recordingFailed &&
       app.status.savedPath.empty() && app.status.savedPaths.empty()) {
        const auto now=GetTickCount64();
        const auto remaining=startDelayRemaining(app.status,now);
        if(remaining!=app.waitingRemaining || app.waitingCaption.empty()) {
            wchar_t value[160]{};
            if(waitingProgressText(app.status,value,now)) {
                try { app.waitingCaption=value;app.waitingRemaining=remaining; }
                catch(const std::bad_alloc&) { return app.status.message; }
            }
        }
        if(!app.waitingCaption.empty())return app.waitingCaption;
    }
    return app.status.message;
}
bool statusCaptionError() {
    return app.status.error || (!app.active() && (!app.nightValidation.empty() || !app.encodingValidation.empty() || !app.watermarkValidation.empty() || &statusCaption()==&app.cameraListError));
}
const wchar_t* statusTooltip() noexcept {
    const auto& caption=statusCaption();
    if(!cameraListUnavailable())return caption.c_str();
    // Build only on tooltip demand. Keep the current engine/save/recovery
    // report first; the source-list failure must not replace its details.
    try {
        std::wstring detail=caption;
        if(&caption!=&app.cameraListError)detail+=L"\n\n"+app.cameraListError;
        detail+=L"\nRefresh sources to retry.";
        app.statusTooltipCaption=std::move(detail);
        return app.statusTooltipCaption.c_str();
    } catch (...) {
        return caption.c_str();
    }
}
bool hasStatusDetails() {
    return app.status.error || app.status.recordingFailed || !app.status.savedPath.empty() || !app.status.savedPaths.empty() ||
        (!app.active() && (!app.encodingValidation.empty() || !app.nightValidation.empty() || !app.watermarkValidation.empty())) || cameraListUnavailable();
}
std::wstring statusDetailsSnapshot() {
    const auto& caption=statusCaption();
    const bool outcome=app.status.error || app.status.recordingFailed || !app.status.savedPath.empty() || !app.status.savedPaths.empty();
    const auto& primary=outcome && !app.status.message.empty()?app.status.message:caption;
    std::wstring snapshot;snapshot.reserve(primary.size());
    const auto append=[&](std::wstring_view value){
        for(size_t i=0;i<value.size();++i){if(value[i]==L'\n' && (!i || value[i-1]!=L'\r'))snapshot+=L'\r';snapshot+=value[i];}
    };
    append(primary);
    if(caption!=primary && !caption.empty()){append(L"\n\nCurrent setting:\n");append(caption);}
    if(cameraListUnavailable()) {
        if(app.cameraListError!=primary && app.cameraListError!=caption){append(L"\n\nCamera list:\n");append(app.cameraListError);}
        append(L"\nRefresh sources to retry.");
    }
    return snapshot;
}
void revealFocusedControl();
bool statusDetailsOwnerReady(HWND owner) {
    return owner==app.window && IsWindow(owner) && IsWindowVisible(owner) && IsWindowEnabled(owner) &&
        !app.hiddenToTray && !IsIconic(owner) && !app.closeWhenDone && app.failureNotice!=FailureNotice::Presenting;
}
void restoreStatusFocus(HWND owner,HWND preferred) {
    if(!statusDetailsOwnerReady(owner))return;
    HWND target=nullptr;
    for(HWND child:{preferred,app.openFolder})if(child && IsChild(owner,child) && IsWindowVisible(child) && IsWindowEnabled(child) &&
        (GetWindowLongPtrW(child,GWL_STYLE)&WS_TABSTOP)){target=child;break;}
    SetFocus(target?target:owner);revealFocusedControl();
}
void cancelOpenFolder() noexcept {
    if(app.openFolderTask)app.openFolderTask->cancel();
    app.openFolderTask.reset();
}
void refreshOpenFolderControl() {
    if(app.shellBusyObserved && !shellOperationBusy.load(std::memory_order_acquire))app.shellBusyObserved=false;
    const bool busy=app.openFolderTask || app.shellBusyObserved;
    if(busy==app.openFolderBusyShown)return;
    if(app.hiddenToTray || IsIconic(app.window)){app.visibleDirty=true;return;}
    if(!app.openFolder || !IsWindow(app.openFolder) || GetParent(app.openFolder)!=app.window)return;
    if(busy && GetFocus()==app.openFolder)restoreStatusFocus(app.window,app.advanced);
    SetWindowTextW(app.openFolder,busy?L"Opening...":L"&Open folder");
    EnableWindow(app.openFolder,!busy);app.openFolderBusyShown=busy;
}
OpenFolderResult openFolderPath(OpenFolderTask& task) {
    const auto cancelled=[&]{return task.phase.load()==OpenFolderPhase::Cancelled;};
    if(cancelled())return OpenFolderResult::Cancelled;
    struct Com {
        HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
        ~Com(){if(SUCCEEDED(result))CoUninitialize();}
    } com;
    if(FAILED(com.result))return OpenFolderResult::ComFailure;
    if(cancelled())return OpenFolderResult::Cancelled;
    std::error_code error;
    std::filesystem::create_directories(fileIOPath(task.folder),error);
    if(cancelled())return OpenFolderResult::Cancelled;
    if(error)return OpenFolderResult::DirectoryUnavailable;
    // A dispatched Shell request cannot be revoked. Before this transition,
    // hiding/exiting prevents dispatch even if directory preparation was slow.
    auto expected=OpenFolderPhase::Preparing;
    if(!task.phase.compare_exchange_strong(expected,OpenFolderPhase::Dispatching))return OpenFolderResult::Cancelled;
    SHELLEXECUTEINFOW request{sizeof(request)};
    request.fMask=SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
    request.lpVerb=L"open";request.lpFile=task.folder.c_str();request.nShow=SW_SHOWNORMAL;
    // NOASYNC lets this short-lived STA finish provider dispatch without a
    // message loop. NO_UI suppresses ordinary errors, not security prompts.
    return ShellExecuteExW(&request)?OpenFolderResult::Opened:OpenFolderResult::ShellFailure;
}
unsigned __stdcall openFolderWorker(void* argument) noexcept {
    std::unique_ptr<std::shared_ptr<OpenFolderTask>> owned(static_cast<std::shared_ptr<OpenFolderTask>*>(argument));
    const auto task=*owned;owned.reset();
    try {task->result=openFolderPath(*task);}
    catch(const std::bad_alloc&){task->result=OpenFolderResult::OutOfMemory;}
    catch(...){task->result=OpenFolderResult::Failed;}
    // COM/native resources are gone before another Explorer action can start.
    shellOperationBusy.store(false,std::memory_order_release);
    task->done.store(true,std::memory_order_release);return 0;
}
bool openFolderReportReady() {
    return IsWindow(app.window) && !app.hiddenToTray && !IsIconic(app.window) && IsWindowEnabled(app.window) &&
        !app.customDialog && !app.trayMenuOpen && !app.closeWhenDone &&
        app.failureNotice!=FailureNotice::Pending && app.failureNotice!=FailureNotice::Presenting;
}
void pollOpenFolder() {
    if(!app.openFolderTask && !app.shellBusyObserved)return;
    if(!app.openFolderTask || !app.openFolderTask->done.load(std::memory_order_acquire)){refreshOpenFolderControl();return;}
    const auto result=app.openFolderTask->result;
    const bool discard=result==OpenFolderResult::Opened || result==OpenFolderResult::Cancelled ||
        !IsWindow(app.window) || app.hiddenToTray || app.closeWhenDone || app.openFolderTask->suppressError ||
        app.failureNotice==FailureNotice::Pending || app.failureNotice==FailureNotice::Presenting ||
        app.openFolderTask->folder!=app.settings.folder;
    if(!discard && !openFolderReportReady()){refreshOpenFolderControl();return;}
    // Claim the result before a modal error can reenter, hide, or destroy App's
    // window. The recording report and its acknowledgement remain untouched.
    const auto task=std::move(app.openFolderTask);refreshOpenFolderControl();
    if(discard || !openFolderReportReady())return;
    const wchar_t* message=L"Windows could not open the save folder. Try again.";
    std::wstring detailed;
    if(result==OpenFolderResult::DirectoryUnavailable)message=L"The save folder is unavailable. Choose another folder.";
    else if(result==OpenFolderResult::OutOfMemory)message=L"Not enough memory to open the save folder. Try again.";
    else if(result==OpenFolderResult::StartFailure)message=L"Windows could not start the folder request. Try again.";
    else if(result==OpenFolderResult::ShellFailure){
        try {detailed=L"Windows could not open the save folder. Try opening it in File Explorer:\n\n"+task->folder;message=detailed.c_str();}
        catch(...){/* The fixed message remains safe under allocation failure. */}
    }
    MessageBoxW(app.window,message,L"Timelapse",MB_OK|MB_ICONERROR);
}
void startOpenFolder() {
    if(!IsWindow(app.window) || app.hiddenToTray || IsIconic(app.window) || !IsWindowEnabled(app.window) ||
        app.customDialog || app.closeWhenDone || app.failureNotice==FailureNotice::Presenting)return;
    if(app.openFolderTask){refreshOpenFolderControl();return;}
    bool expected=false;
    if(!shellOperationBusy.compare_exchange_strong(expected,true)){
        app.shellBusyObserved=true;refreshOpenFolderControl();return;
    }
    app.shellBusyObserved=true;
    std::shared_ptr<OpenFolderTask> task;
    std::unique_ptr<std::shared_ptr<OpenFolderTask>> argument;
    try {
        task=std::make_shared<OpenFolderTask>();task->folder=app.settings.folder;
        task->suppressError=app.failureNotice==FailureNotice::Pending || app.failureNotice==FailureNotice::Presenting;
        argument=std::make_unique<std::shared_ptr<OpenFolderTask>>(task);
    } catch(...) {
        shellOperationBusy.store(false,std::memory_order_release);refreshOpenFolderControl();
        if(openFolderReportReady())MessageBoxW(app.window,L"Not enough memory to open the save folder. Try again.",L"Timelapse",MB_OK|MB_ICONERROR);
        return;
    }
    app.openFolderTask=task;
    const uintptr_t thread=_beginthreadex(nullptr,0,openFolderWorker,argument.get(),0,nullptr);
    if(!thread){
        task->result=OpenFolderResult::StartFailure;task->done.store(true,std::memory_order_release);
        shellOperationBusy.store(false,std::memory_order_release);pollOpenFolder();return;
    }
    argument.release();CloseHandle(reinterpret_cast<HANDLE>(thread));refreshOpenFolderControl();
}
void layoutStatusRow() {
    if(app.contentWidth<=0 || app.contentHeight<=0)return;
    // The status message sits under the transport card; Details takes the
    // right end only while a report exists.
    const RECT row=app.statusRect;const int height=row.bottom-row.top,detailsW=app.scale(92);
    const int details=app.statusDetailsVisible==1?detailsW+app.scale(10):0,inset=app.scale(4);
    MoveWindow(app.statusText,row.left+inset-app.scrollX,row.top-app.scrollY,std::max(1,int(row.right-row.left)-details-inset),height,TRUE);
    if(app.statusDetails && GetParent(app.statusDetails)==app.window)
        MoveWindow(app.statusDetails,row.right-detailsW-app.scrollX,row.top-app.scrollY,detailsW,height,TRUE);
}
void updateStatusDetails() {
    if(!app.statusDetails || !IsWindow(app.statusDetails) || GetParent(app.statusDetails)!=app.window)return;
    const bool visible=hasStatusDetails();
    if(app.statusDetailsVisible==static_cast<int>(visible))return;
    app.statusDetailsVisible=static_cast<int>(visible);
    if(!visible && GetFocus()==app.statusDetails)restoreStatusFocus(app.window,app.openFolder);
    ShowWindow(app.statusDetails,visible?SW_SHOWNA:SW_HIDE);
    layoutStatusRow();
}
void updateStatusText(bool force=false) {
    const auto& caption=statusCaption();const bool error=statusCaptionError();
    if(app.hiddenToTray || IsIconic(app.window)) {
        if(force || caption!=app.statusCaption || error!=app.statusCaptionError)app.visibleDirty=true;
        return;
    }
    updateStatusDetails();
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
        swprintf_s(value,L"Last blend: %.1f s · %u camera frames · %.1f× shadow gain%s",
            app.status.nightDurationMs/1000.0,app.status.night.samples,app.status.night.appliedGain,
            app.status.night.targetLimited?L" · brightness target limited":L"");
        detail=value;
    } else if(app.status.nightEnabled && app.status.nightWaiting)detail=L"Collecting camera frames for the first full blend...";
    if(force || detail!=app.nightDetailCaption){SetWindowTextW(app.nightDetail,detail.c_str());app.nightDetailCaption=std::move(detail);}
}
std::wstring watermarkSummary(const WatermarkSettings& value) {
    if(!value.enabled)return L"Off";
    std::wstring text=value.showTime?(value.timeKind==WatermarkTimeKind::ActiveElapsed?L"Active elapsed":L"Recorded local date-time"):L"";
    if(value.showSpeed){if(!text.empty())text+=L" + ";text+=L"target speed";}
    return text;
}
void checkWatermark() {
    if(app.watermarkCheckValid && app.watermarkWidth==app.settings.width && app.watermarkHeight==app.settings.height &&
       sameWatermarkSettings(app.watermarkChecked,app.settings.watermark))return;
    app.watermarkCheckValid=false;app.watermarkValidation.clear();
    if(validateWatermarkSettings(app.settings.watermark,app.watermarkValidation) && app.settings.watermark.enabled){
        WatermarkRenderer renderer;renderer.prepare(app.settings.watermark,app.settings.width,app.settings.height,app.watermarkValidation);
    }
    app.watermarkChecked=app.settings.watermark;app.watermarkWidth=app.settings.width;app.watermarkHeight=app.settings.height;
    app.watermarkCheckValid=true;++app.watermarkRevision;
}
void updateAdvanced() {
    if(!app.advanced)return;
    const int selection=selectedLimit(),segment=selectedSegment(),delay=selectedStartDelay();
    const int night=app.settings.night.enabled?(app.nightValidation.empty()?1:2):0;
    const int recovery=app.encodingValidation.empty()?(app.settings.recoveryMode?1:0):2;
    const int cursor=app.settings.captureCursor?0:hasSource(Source::Desktop)?1:2;
    if(app.advancedOutputFps!=app.settings.outputFps || delay!=app.advancedDelaySeconds || selection!=app.advancedLimitIndex || segment!=app.advancedSegmentSeconds || night!=app.advancedNightState || recovery!=app.advancedRecoveryState || cursor!=app.advancedCursorState || app.advancedSkipRevision!=app.skipRevision || app.advancedWatermarkRevision!=app.watermarkRevision){
        std::wstring caption=L"&Advanced";
        if(recovery==2)caption+=L" · check MP4";
        else if(night==2)caption+=L" · check blend";
        else if(!app.watermarkValidation.empty())caption+=L" · check watermark";
        else if(night){caption+=L" · night";if(selection)caption+=L", "+(choice(app.stopAfter)<6?std::wstring(RecordingLimitShortLabels[std::clamp(choice(app.stopAfter),0,5)]):formatDuration(int64_t(selection)*1000,true));}
        else if(selection)caption+=L" · stop after "+(choice(app.stopAfter)<6?std::wstring(RecordingLimitLabels[std::clamp(choice(app.stopAfter),0,5)]):formatDuration(int64_t(selection)*1000,true));
        if(delay && night!=2 && recovery!=2 && app.watermarkValidation.empty())caption+=L" · delay "+formatDuration(int64_t(delay)*1000,true);
        if(recovery==1 && night!=2 && app.watermarkValidation.empty())caption+=L" · recovery";
        if(skipEnabled() && night!=2 && recovery!=2 && app.watermarkValidation.empty())caption+=L" · "+std::to_wstring(app.settings.timeSkip.multiplier)+L"×";
        if(segment && night!=2 && recovery!=2 && app.watermarkValidation.empty())caption+=L" · split "+formatDuration(int64_t(segment)*1000,true);
        if(app.settings.watermark.enabled && app.watermarkValidation.empty() && night!=2 && recovery!=2)caption+=L" · watermark";
        if(cursor==1 && app.watermarkValidation.empty() && night!=2 && recovery!=2)caption+=L" · cursor off";
        if(app.settings.outputFps!=DefaultOutputFps)caption+=L" · "+std::to_wstring(app.settings.outputFps)+L" fps";
        if(!app.hotkeyWarning.empty())caption=L"&Advanced · check shortcuts";
        RECT bounds{};GetClientRect(app.advanced,&bounds);
        if((delay || selection || segment || skipEnabled() || recovery==1 || app.settings.watermark.enabled || cursor==1 || app.settings.outputFps!=DefaultOutputFps) && app.hotkeyWarning.empty() && night!=2 && recovery!=2 && app.watermarkValidation.empty() && bounds.right>app.scale(40)) {
            HDC dc=GetDC(app.advanced);if(dc){const auto previous=SelectObject(dc,app.font);SIZE size{};
                GetTextExtentPoint32W(dc,caption.c_str(),static_cast<int>(caption.size()),&size);
                SelectObject(dc,previous);ReleaseDC(app.advanced,dc);
                if(size.cx+app.scale(DisclosureChrome)>bounds.right)caption=delay?L"&Advanced · delay "+formatDuration(int64_t(delay)*1000,true):cursor==1?L"&Advanced · cursor off + options":app.settings.watermark.enabled?L"&Advanced · watermark + options":segment?L"&Advanced · split + options":recovery?L"&Advanced · recovery + options":skipEnabled()?L"&Advanced · "+std::wstring(night?L"night/":L"")+
                    (selection?L"stop/":L"")+std::to_wstring(app.settings.timeSkip.multiplier)+L"×":night?L"&Advanced · night + stop":selection?L"&Advanced · timed stop":L"&Advanced · "+std::to_wstring(app.settings.outputFps)+L" fps";
            }
        }
        const bool warning=recovery==2 || night==2 || !app.watermarkValidation.empty() || !app.hotkeyWarning.empty();
        if(caption!=app.advancedCaption){SetWindowTextW(app.advanced,caption.c_str());app.advancedCaption=caption;}
        else if(warning!=app.advancedWarning)InvalidateRect(app.advanced,nullptr,FALSE);
        app.advancedWarning=warning;
        app.advancedTooltip=L"Show or hide advanced options. Recording options can be changed before recording. Night mode applies only to camera content. Show desktop cursor applies only to desktop content.";
        app.advancedTooltip+=L" Playback: "+std::to_wstring(app.settings.outputFps)+L" fps. Global shortcuts can be configured in Playback & shortcuts.";
        if(!app.hotkeyWarning.empty())app.advancedTooltip+=L" "+app.hotkeyWarning;
        if(delay)app.advancedTooltip+=L" After Record, wait "+formatDuration(int64_t(delay)*1000)+L" before preparation. Visible preview continues; Stop after counts active recording time. Sleep cancels the pending start.";
        if(cursor)app.advancedTooltip+=L" The added system cursor is hidden in desktop preview and recordings; pointers drawn into application pixels are unchanged.";
        if(selection)app.advancedTooltip+=L" Stop after "+formatDuration(int64_t(selection)*1000)+L" of active recording; pauses and startup do not count.";
        if(segment)app.advancedTooltip+=L" Split files every "+formatDuration(int64_t(segment)*1000)+L" of active recording. Shorter parts add processing and file overhead.";
        if(skipEnabled())app.advancedTooltip+=L" Time compression: "+skipSummary(app.settings.timeSkip,app.settings.intervalMs)+L".";
        if(skipPerson(app.settings.timeSkip.mode))app.advancedTooltip+=L" Person checks use only selected camera content and require the optional detector. Missing, uncertain or stale checks keep the normal capture interval.";
        if(app.settings.recoveryMode)app.advancedTooltip+=L" MP4 recovery mode (H.264) is on; recent frames can still be lost after interruption.";
        if(!app.encodingValidation.empty())app.advancedTooltip+=L" "+app.encodingValidation;
        if(app.settings.watermark.enabled)app.advancedTooltip+=L" Watermark: "+watermarkSummary(app.settings.watermark)+L". Same placement in both files.";
        if(!app.watermarkValidation.empty())app.advancedTooltip+=L" "+app.watermarkValidation;
        const auto captionWatermark=watermarkSummary(app.settings.watermark);
        if(captionWatermark!=app.watermarkCaption){SetWindowTextW(app.watermarkSummary,captionWatermark.c_str());app.watermarkCaption=captionWatermark;}
        app.advancedWatermarkRevision=app.watermarkRevision;
        app.advancedCursorState=cursor;app.advancedDelaySeconds=delay;
        app.advancedOutputFps=app.settings.outputFps;
        app.advancedLimitIndex=selection;app.advancedSegmentSeconds=segment;app.advancedNightState=night;app.advancedRecoveryState=recovery;app.advancedSkipRevision=app.skipRevision;
    }
    const int visibleNight=app.advancedExpanded?nightRow():0;
    const int visibleSkip=app.advancedExpanded?(skipEnabled()?2:1):0;
    const bool visibleCursor=app.advancedExpanded && hasSource(Source::Desktop);
    if(app.advancedVisibility==static_cast<int>(app.advancedExpanded) && app.nightVisibility==visibleNight && app.skipVisibility==visibleSkip && app.cursorVisibility==int(visibleCursor))return;
    app.cursorVisibility=int(visibleCursor);
    app.advancedVisibility=static_cast<int>(app.advancedExpanded);
    app.nightVisibility=visibleNight;
    app.skipVisibility=visibleSkip;
    SendMessageW(app.advanced,BM_SETCHECK,app.advancedExpanded?BST_CHECKED:BST_UNCHECKED,0);
    const auto visible=[](HWND child,bool show){
        if(child && ((GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0)!=show)ShowWindow(child,show?SW_SHOWNA:SW_HIDE);
    };
    for(HWND child:{app.labels[6],app.encodingMode,app.labels[7],app.stopAfter,app.lowDisk,app.recoveryMode,app.segmentLabel,app.splitEvery,app.startDelayLabel,app.startDelay,app.startDelayHint,app.watermarkConfigure,app.watermarkSummary,app.playbackConfigure})visible(child,app.advancedExpanded);
    if(!visibleCursor && GetFocus()==app.captureCursor)SetFocus(app.advanced);
    visible(app.captureCursor,visibleCursor);
    for(HWND child:{app.skipConfigure,app.skipSummary})visible(child,visibleSkip!=0);
    visible(app.skipDetail,visibleSkip==2);
    if(!visibleNight && app.nightEnabled && GetFocus()==app.nightEnabled)SetFocus(app.advanced);
    visible(app.nightEnabled,visibleNight!=0);
    for(HWND child:{app.labels[8],app.nightDuration,app.labels[9],app.nightTarget,app.nightHint,app.nightDetail}){
        const HWND focused=GetFocus();
        if(visibleNight!=2 && child && (focused==child || (focused && IsChild(child,focused))))SetFocus(visibleNight?app.nightEnabled:app.advanced);
        visible(child,visibleNight==2);
    }
    updateNightText(true);
    updateSkipText(true);
}

void configure() {
    const int previousInterval=app.settings.intervalMs;
    app.settings.intervalMs = selectedInterval();
    if(previousInterval!=app.settings.intervalMs)++app.skipRevision;
    const int sizeChoice=choice(app.videoSize),sizeSelection=sizeChoice>=0 && sizeChoice<=(app.hasCustomSize?2:1)?sizeChoice:app.committedSize;
    const bool customSize=app.hasCustomSize && sizeSelection==2;
    app.settings.width = customSize?app.customWidth:sizeSelection==1?1920:1280;
    app.settings.height = customSize?app.customHeight:sizeSelection==1?1080:720;
    app.committedInterval=std::clamp(choice(app.interval),0,app.hasCustomInterval?6:5);
    app.committedSize=std::clamp(sizeSelection,0,app.hasCustomSize?2:1);
    app.committedLimit=std::clamp(choice(app.stopAfter),0,app.hasCustomLimit?6:5);
    app.settings.encodingQuality = static_cast<EncodingQuality>(std::clamp(choice(app.encodingQuality),0,2));
    app.settings.encodingMode = static_cast<EncodingMode>(std::clamp(choice(app.encodingMode),0,4));
    app.settings.recoveryMode = app.recoveryMode && SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==BST_CHECKED;
    validateEncodingMode(app.settings.encodingMode,app.settings.recoveryMode,app.encodingValidation);
    app.settings.recordingLimitSeconds = selectedLimit();
    if(!app.active())app.committedStartDelay=std::clamp(choice(app.startDelay),0,5);
    app.settings.startDelaySeconds=app.startDelay?StartDelays[app.committedStartDelay]:0;
    app.settings.segmentDurationSeconds = selectedSegment();
    app.committedSegment=app.splitEvery?std::clamp(choice(app.splitEvery),0,app.hasCustomSegment?5:4):0;
    app.settings.stopOnLowDiskSpace = !app.lowDisk || SendMessageW(app.lowDisk,BM_GETCHECK,0,0)!=BST_UNCHECKED;
    app.settings.captureCursor = !app.captureCursor || SendMessageW(app.captureCursor,BM_GETCHECK,0,0)!=BST_UNCHECKED;
    app.settings.night.enabled = app.nightEnabled && SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_CHECKED && hasSource(Source::Camera);
    app.settings.night.durationMs = selectedNightDuration();
    app.committedNightDuration=app.nightDuration?std::clamp(choice(app.nightDuration),0,app.hasCustomNightDuration?6:5):0;
    app.settings.night.targetBrightness = NightTargets[app.nightTarget?std::clamp(choice(app.nightTarget),0,2):1];
    app.nightValidation.clear();
    if(app.settings.night.enabled && app.settings.intervalMs<NightMinDurationMs)
        app.nightValidation=L"Night camera needs a capture interval of at least 1 second. Choose a longer interval or turn off Night camera.";
    else if(app.settings.night.enabled && app.settings.night.durationMs>app.settings.intervalMs)
        app.nightValidation=L"Night blend duration must not exceed Capture every. Choose Auto, a shorter blend, or a longer capture interval.";
    checkWatermark();
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
    app.cameraListError=std::move(error);
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
        add(app.camera,!app.cameraListError.empty() ? L"Camera list unavailable"
            : oldCamera.empty() ? L"No camera connected" : L"Selected camera unavailable");
    }
    choose(app.camera,cameraIndex);
    configure();
}
void updateControls() {
    const bool idle = !app.active();
    for (auto control : {app.mode,app.interval,app.videoSize,app.encodingQuality,app.encodingMode,app.stopAfter,app.splitEvery,app.startDelay,app.lowDisk,app.recoveryMode,app.nightEnabled,app.nightDuration,app.nightTarget,app.refresh,app.folder}) EnableWindow(control,idle);
    EnableWindow(app.monitor,idle && hasSource(Source::Desktop));
    EnableWindow(app.captureCursor,idle && hasSource(Source::Desktop));
    EnableWindow(app.camera,idle && hasSource(Source::Camera) && !app.cameras.empty());
    EnableWindow(app.record,idle && hasRequiredSources() && app.nightValidation.empty() && app.encodingValidation.empty() && app.watermarkValidation.empty());
    EnableWindow(app.pause,app.status.state==State::Recording || app.status.state==State::Paused);
    const auto starting=[](State state){return state==State::Waiting || state==State::Starting;};
    EnableWindow(app.finish,starting(app.status.state) || app.status.state==State::Recording || app.status.state==State::Paused);
    if(!app.controlsUpdated || starting(app.controlsState)!=starting(app.status.state)){
        SetWindowTextW(app.finish,starting(app.status.state)?L"Cancel s&tart":L"&Finish");
        // A disabled combo's label can still consume its mnemonic. Reserve T
        // for cancelling preparation, then restore the idle Stop-after key.
        if(app.labels[7])SetWindowTextW(app.labels[7],starting(app.status.state)?L"Stop after":L"S&top after");
    }
    if(!app.controlsUpdated || (app.controlsState==State::Paused)!=(app.status.state==State::Paused))
        SetWindowTextW(app.pause,app.status.state==State::Paused ? L"&Resume" : L"&Pause");
    bool collage = !app.settings.separateFiles && app.settings.layers.size() > 1;
    EnableWindow(app.reset,collage); EnableWindow(app.forward,collage && app.selected>=0);
    updateAdvanced();
    updateNightText(!app.controlsUpdated);updateSkipText(!app.controlsUpdated);updateStatusText(!app.controlsUpdated);
    refreshOpenFolderControl();
    app.controlsState=app.status.state;app.controlsUpdated=true;
}
void invalidateCanvas(RECT rect) {
    OffsetRect(&rect,-app.scrollX,-app.scrollY);InvalidateRect(app.window,&rect,FALSE);
}
void applyStatus(Status value,bool force=false) {
    // Polling from a menu or lifecycle command must not consume a recording
    // failure before its recovery information has actually been presented.
    if(app.failureNotice!=FailureNotice::Presenting) {
        if(!value.recordingFailed)app.failureNotice=FailureNotice::None;
        else if(app.failureNotice==FailureNotice::None)app.failureNotice=FailureNotice::Pending;
    }
    if(app.openFolderTask && (app.failureNotice==FailureNotice::Pending || app.failureNotice==FailureNotice::Presenting))
        app.openFolderTask->suppressError=true;
    const bool state=app.status.state!=value.state;
    const bool stats=state || app.status.frames!=value.frames || app.status.completedSegments!=value.completedSegments ||
        static_cast<uint64_t>(std::max(0.0,app.status.elapsed))!=static_cast<uint64_t>(std::max(0.0,value.elapsed));
    const bool message=app.status.message!=value.message,error=app.status.error!=value.error;
    const bool failure=app.status.recordingFailed!=value.recordingFailed;
    const bool outcome=failure || app.status.savedPath.empty()!=value.savedPath.empty() ||
        app.status.savedPaths.empty()!=value.savedPaths.empty();
    const bool deadline=app.status.startDeadlineTick!=value.startDeadlineTick;
    const bool preview=app.status.preview!=value.preview;
    const bool night=app.status.nightEnabled!=value.nightEnabled || app.status.nightWaiting!=value.nightWaiting ||
        app.status.nightDurationMs!=value.nightDurationMs || app.status.night.samples!=value.night.samples ||
        app.status.night.appliedGain!=value.night.appliedGain || app.status.night.targetLimited!=value.night.targetLimited;
    const auto& oldSkip=app.status.timeSkip;const auto& newSkip=value.timeSkip;
    const bool skip=state || oldSkip.enabled!=newSkip.enabled || oldSkip.reason!=newSkip.reason || oldSkip.intervalMs!=newSkip.intervalMs ||
        oldSkip.lastCheckTick!=newSkip.lastCheckTick || oldSkip.observationDelayed!=newSkip.observationDelayed || oldSkip.diagnostic!=newSkip.diagnostic;
    app.status=std::move(value);
    // Status and tray handling remain live while hidden; only visual work waits.
    if(app.hiddenToTray || IsIconic(app.window)) {
        if(force || stats || message || error || outcome || preview || night || skip || deadline)app.visibleDirty=true;
        return;
    }
    if(force || app.visibleDirty) {
        updateControls();updateNightText();updateSkipText();updateStatusText();
        InvalidateRect(app.window,nullptr,FALSE);InvalidateRect(app.preview,nullptr,FALSE);InvalidateRect(app.statusText,nullptr,TRUE);
        app.visibleDirty=false;return;
    }
    if(!app.controlsUpdated || app.controlsState!=app.status.state)updateControls();
    if(message || error || outcome || state || deadline || app.status.state==State::Waiting)updateStatusText();
    if(night)updateNightText();
    if(app.advancedExpanded && skipEnabled()){
        const auto tick=app.status.timeSkip.lastCheckTick,now=GetTickCount64();
        const auto age=tick && now>=tick?(now-tick)/1000:UINT64_MAX;
        if(skip || (app.status.state==State::Recording && skipObserved(app.settings.timeSkip.mode) && age!=app.skipCheckAge))updateSkipText();
    }
    if(preview)InvalidateRect(app.preview,nullptr,FALSE);
    // Repaint only the state pill row and the transport statistics.
    if(state || (failure && app.status.state==State::Idle))invalidateCanvas(app.headerRect);
    if(stats)invalidateCanvas(app.statsRect);
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
    // A colored disc with one white glyph stays recognizable at notification-area
    // sizes: ring = ready/busy, dot = recording, bars = paused, mark = failed.
    const COLORREF colors[]={Accent,Danger,RGB(202,131,0),Info,RGB(150,32,32)};
    const COLORREF disc=colors[state];
    app.trayIcons[state]=shadedIcon(GetSystemMetricsForDpi(SM_CXSMICON,static_cast<UINT>(app.dpi)),[state,disc](double x,double y,double pixel,double& alpha){
        const double distance=std::hypot(x-.5,y-.5);alpha=coverage(distance-.48,pixel);
        double glyph=std::abs(distance-.2)-.065;
        if(state==1)glyph=distance-.17;
        else if(state==2)glyph=std::min(boxDistance(x,y,.39,.5,.065,.19,.03),boxDistance(x,y,.61,.5,.065,.19,.03));
        else if(state==4)glyph=std::min(segmentDistance(x,y,.5,.27,.5,.55)-.065,std::hypot(x-.5,y-.72)-.075);
        return blend(disc,RGB(255,255,255),coverage(glyph,pixel));
    });
    return app.trayIcons[state] ? app.trayIcons[state] : LoadIconW(nullptr,IDI_APPLICATION);
}
bool updateTray(bool addIcon=false) {
    if(!app.trayRegistered && !addIcon)return false;
    if(addIcon && !app.taskbarCreated){app.taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");if(!app.taskbarCreated)return false;}
    if(!addIcon && app.trayStateValid && app.trayState==app.status.state &&
       app.trayFailure==app.status.recordingFailed && app.traySeparate==app.settings.separateFiles)return true;
    const wchar_t* state=app.status.state==State::Recording?L"Recording":app.status.state==State::Paused?L"Paused":
        app.status.state==State::Waiting?L"Waiting to start":app.status.state==State::Starting?L"Preparing":app.status.state==State::Finishing?L"Saving":app.status.recordingFailed?L"Recording failed":L"Ready";
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
bool pendingRecordingFailure() {
    return !app.active() && app.status.recordingFailed && app.failureNotice==FailureNotice::Pending;
}
void acknowledgeVisibleFailure() {
    if(pendingRecordingFailure() && !app.hiddenToTray && !IsIconic(app.window) && IsWindowEnabled(app.window))app.failureNotice=FailureNotice::Presented;
}
void showWindow(bool acknowledgeFailure=false) {
    app.hiddenToTray=false;ShowWindow(app.window,SW_RESTORE);SetForegroundWindow(app.window);
    if(app.engine){applyStatus(app.engine->status(),true);configure();}
    if(acknowledgeFailure)acknowledgeVisibleFailure();
}
bool reportRecordingFailure() {
    if(!pendingRecordingFailure())return false;
    std::wstring message;
    const wchar_t* warning=L"Recording could not finish normally. See the main window's status message for recovery details.";
    try {message=app.status.message;warning=message.c_str();}
    catch(const std::bad_alloc&) {} // Keep the full status available if its snapshot cannot be allocated.
    // ShowWindow and the warning run nested message loops. Suppress duplicate
    // notices and owner-closing commands before either can dispatch messages.
    app.failureNotice=FailureNotice::Presenting;
    if(app.trayMenuOpen){app.trayMenuCanceled=true;EndMenu();}
    showWindow();
    if(IsWindow(app.window))MessageBoxW(app.window,warning,L"Timelapse could not finish normally",MB_OK|MB_ICONERROR);
    app.failureNotice=app.status.recordingFailed?FailureNotice::Presented:FailureNotice::None;
    return true;
}
bool hideToTray() {
    if(app.failureNotice==FailureNotice::Presenting)return false;
    cancelOwnedDialogs();
    applyStatus(app.engine->status());
    if(reportRecordingFailure())return false;
    if(!updateTray(!app.trayRegistered)) {
        showWindow();
        MessageBoxW(app.window,L"Windows could not add Timelapse to the system tray. The window will stay open so you can control your recording.",L"Timelapse",MB_OK|MB_ICONWARNING);
        return false;
    }
    cancelOpenFolder();app.hiddenToTray=true;configure();ShowWindow(app.window,SW_HIDE);
    if(!app.trayNoticeShown) {
        NOTIFYICONDATAW data{sizeof(data)};data.hWnd=app.window;data.uID=1;data.uFlags=NIF_INFO;data.dwInfoFlags=NIIF_INFO;
        wcscpy_s(data.szInfoTitle,L"Timelapse is in the system tray");
        wcscpy_s(data.szInfo,app.status.state==State::Waiting?L"Your start timer continues in the system tray. Right-click the tray icon to show Timelapse, cancel the start, or exit.":L"Recording continues when this window is closed. Right-click the tray icon to show Timelapse, finish, or exit.");
        Shell_NotifyIconW(NIM_MODIFY,&data);app.trayNoticeShown=true;
    }
    return true;
}
void exitApplication() {
    if(app.closeWhenDone || app.failureNotice==FailureNotice::Presenting)return;
    cancelOwnedDialogs();
    applyStatus(app.engine->status());
    if(reportRecordingFailure())return;
    if(app.active()) {
        const bool preparing=app.status.state==State::Starting;
        if(app.status.state!=State::Waiting && MessageBoxW(app.window,
            preparing?L"Cancel the pending start and exit Timelapse?":L"Finish the current recording and exit Timelapse?",
            preparing?L"Cancel start":L"Finish recording",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK)return;
        cancelOpenFolder();app.closeWhenDone=true;app.engine->finish();EnableWindow(app.window,FALSE);
    } else {cancelOpenFolder();DestroyWindow(app.window);}
}
bool trayProgressText(const Status& status,wchar_t (&value)[160]) noexcept {
    value[0]=L'\0';
    if(status.state==State::Waiting)return waitingProgressText(status,value);
    const wchar_t* state=nullptr;
    switch(status.state) {
    case State::Recording:state=L"Recording";break;
    case State::Paused:state=L"Paused";break;
    case State::Finishing:state=L"Saving";break;
    default:return false;
    }
    // A menu-opening snapshot: truncate whole seconds, never extrapolate or
    // imply that accepted frames have already been finalized to a file.
    // UINT64_MAX rounds to 2^64 as double, so equality is also out of range.
    if(!std::isfinite(status.elapsed) || status.elapsed<0 ||
       status.elapsed>=static_cast<double>(UINT64_MAX))return false;
    const auto active=static_cast<uint64_t>(status.elapsed);
    const auto fps=static_cast<uint64_t>(std::clamp(app.recordedOutputFps,MinOutputFps,MaxOutputFps));
    if(status.frames<fps)
        return swprintf_s(value,L"%ls: %02llu:%02llu:%02llu active | %llu %ls total",
            state,active/3600,active/60%60,active%60,status.frames,
            status.frames==1?L"frame":L"frames")>=0;
    const auto video=status.frames/fps;
    return swprintf_s(value,L"%ls: %02llu:%02llu:%02llu active | %02llu:%02llu:%02llu video total",
        state,active/3600,active/60%60,active%60,video/3600,video/60%60,video%60)>=0;
}
void trayMenu(POINT at={},bool usePoint=false) {
    if(app.failureNotice==FailureNotice::Presenting || app.trayMenuOpen)return;
    applyStatus(app.engine->status());
    if(app.hiddenToTray && reportRecordingFailure())return;
    HMENU menu=CreatePopupMenu();if(!menu){showWindow();return;}
    wchar_t progress[160]{};
    if(trayProgressText(app.status,progress) &&
       AppendMenuW(menu,MF_STRING|MF_GRAYED,TrayProgress,progress) &&
       !AppendMenuW(menu,MF_SEPARATOR,0,nullptr))
        DeleteMenu(menu,TrayProgress,MF_BYCOMMAND);
    AppendMenuW(menu,MF_STRING,TrayShow,L"&Show Timelapse");
    const bool pauseAllowed=!app.closeWhenDone&&(app.status.state==State::Recording||app.status.state==State::Paused);
    AppendMenuW(menu,MF_STRING|(pauseAllowed?MF_ENABLED:MF_GRAYED),TrayPause,app.status.state==State::Paused?L"&Resume recording":L"&Pause recording");
    const bool starting=app.status.state==State::Waiting || app.status.state==State::Starting;
    const bool finishAllowed=!app.closeWhenDone&&(pauseAllowed||starting);
    AppendMenuW(menu,MF_STRING|(finishAllowed?MF_ENABLED:MF_GRAYED),TrayFinish,starting?L"Cancel s&tart":L"&Finish recording");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING|(app.closeWhenDone?MF_GRAYED:MF_ENABLED),TrayExit,L"E&xit Timelapse");
    SetMenuDefaultItem(menu,TrayShow,FALSE);if(!usePoint)GetCursorPos(&at);SetForegroundWindow(app.window);
    app.trayMenuOpen=true;app.trayMenuCanceled=false;
    const UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,at.x,at.y,0,app.window,nullptr);
    app.trayMenuOpen=false;
    DestroyMenu(menu);PostMessageW(app.window,WM_NULL,0,0);
    // A failure delivered inside the menu loop cancels its original action.
    if(command && !app.trayMenuCanceled)SendMessageW(app.window,WM_COMMAND,command,0);
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
bool shown(HWND child) { return child && (GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0; }
void showControl(HWND child,bool show) {
    if(child && shown(child)!=show)ShowWindow(child,show?SW_SHOWNA:SW_HIDE);
}
// Settings-panel membership decides scrolling, focus reveal and background.
bool panelControl(HWND child) {
    if(!child)return false;
    for(HWND member:{app.labels[0],app.mode,app.labels[4],app.monitor,app.labels[5],app.camera,app.refresh,app.labels[1],app.interval,app.labels[2],app.videoSize,
        app.labels[3],app.encodingQuality,app.folder,app.openFolder,app.advanced,app.labels[6],app.encodingMode,app.recoveryMode,app.labels[7],app.stopAfter,
        app.segmentLabel,app.splitEvery,app.startDelayLabel,app.startDelay,app.startDelayHint,app.lowDisk,app.captureCursor,app.skipConfigure,app.skipSummary,
        app.skipDetail,app.watermarkConfigure,app.watermarkSummary,app.playbackConfigure,app.nightEnabled,app.labels[8],app.nightDuration,app.labels[9],
        app.nightTarget,app.nightHint,app.nightDetail})if(member==child)return true;
    return false;
}
// Device rows appear only for sources the layout uses, and collage tools only
// for editable collages. Hidden controls drop out of Tab order and mnemonics.
void updateSourceRows() {
    const bool desktop=hasSource(Source::Desktop),camera=hasSource(Source::Camera);
    app.collageTools=!app.settings.separateFiles && app.settings.layers.size()>1;
    const HWND focused=GetFocus();
    if(focused && ((!desktop && focused==app.monitor) || (!camera && focused==app.camera)))SetFocus(app.mode);
    if(focused && !app.collageTools && (focused==app.reset || focused==app.forward))SetFocus(app.preview);
    for(HWND child:{app.labels[4],app.monitor})showControl(child,desktop);
    for(HWND child:{app.labels[5],app.camera})showControl(child,camera);
    for(HWND child:{app.reset,app.forward})showControl(child,app.collageTools);
}
int wrappedHeight(HWND child,int width,HFONT font,int minimum) {
    if(!child)return minimum;
    wchar_t value[512]{};GetWindowTextW(child,value,static_cast<int>(std::size(value)));
    HDC dc=GetDC(child);if(!dc)return minimum;
    const auto previous=SelectObject(dc,font?static_cast<HGDIOBJ>(font):GetStockObject(DEFAULT_GUI_FONT));
    RECT measured{0,0,std::max(1,width),0};DrawTextW(dc,value,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    SelectObject(dc,previous);ReleaseDC(child,dc);
    return std::max(minimum,static_cast<int>(measured.bottom));
}
// Lays out the settings panel top to bottom and returns its content height.
// Rows use panel coordinates; offset maps them into the scrolled viewport.
// Hidden rows keep a parked position but add no height.
int placePanel(bool place,int left,int offset) {
    const int pad=app.scale(PanelPad),width=app.scale(PanelWidth)-2*pad;
    const int labelH=app.scale(20),comboTop=app.scale(22),field=app.scale(62),check=app.scale(26),button=app.scale(32);
    const int line=app.scale(16),rowGap=app.scale(12);
    int y=app.scale(14),rules=0;
    const auto put=[&](HWND child,int top,int height,int x=0,int w=0){
        if(place && child)MoveWindow(child,left+pad+x,top-offset,w?w:width,height,TRUE);};
    const auto combo=[&](HWND label,HWND box,int dropped,bool visible=true){
        put(label,y,labelH);put(box,y+comboTop,app.scale(dropped));if(visible)y+=field;};
    const auto rule=[&]{y+=app.scale(4);if(place && rules<static_cast<int>(std::size(app.panelRules)))app.panelRules[rules++]=y;y+=app.scale(16);};
    const auto section=[&](int index){if(place)app.sectionTops[index]=y;y+=app.scale(34);};
    const int refreshW=app.scale(88);
    put(app.refresh,y+app.scale(1),app.scale(28),width-refreshW,refreshW);
    section(0);
    combo(app.labels[0],app.mode,230);
    combo(app.labels[4],app.monitor,220,hasSource(Source::Desktop));
    combo(app.labels[5],app.camera,220,hasSource(Source::Camera));
    rule();section(1);
    combo(app.labels[1],app.interval,220);
    combo(app.labels[2],app.videoSize,140);
    combo(app.labels[3],app.encodingQuality,160);
    rule();section(2);
    if(place)app.savePathRect={app.panelRect.left+pad,y,app.panelRect.left+pad+width,y+app.scale(20)};
    y+=app.scale(26);
    const int changeW=app.scale(104),buttonGap=app.scale(8);
    put(app.folder,y,button,0,changeW);put(app.openFolder,y,button,changeW+buttonGap,width-changeW-buttonGap);
    y+=button;rule();
    if(place)app.advancedTop=y;
    put(app.advanced,y,app.scale(38));y+=app.scale(38);
    // Advanced rows are always positioned so focus and hit-testing never see
    // stale geometry, but they add height only while expanded.
    const int collapsedBottom=y;
    y+=app.scale(14);
    combo(app.labels[6],app.encodingMode,190);
    put(app.recoveryMode,y-app.scale(4),check);y+=check+app.scale(14);
    combo(app.labels[7],app.stopAfter,210);
    combo(app.segmentLabel,app.splitEvery,210);
    put(app.startDelayLabel,y,labelH);put(app.startDelay,y+comboTop,app.scale(210));
    const int hintTop=y+comboTop+app.scale(34),hintH=wrappedHeight(app.startDelayHint,width,app.smallFont,2*line);
    put(app.startDelayHint,hintTop,hintH);y=hintTop+hintH+rowGap;
    put(app.lowDisk,y,check);y+=check+app.scale(4);
    put(app.captureCursor,y,check);if(hasSource(Source::Desktop))y+=check+app.scale(16);
    put(app.skipConfigure,y,button);y+=button+app.scale(6);
    put(app.skipSummary,y,2*line);y+=2*line;
    put(app.skipDetail,y,2*line);if(skipEnabled())y+=2*line;
    y+=rowGap;
    put(app.watermarkConfigure,y,button);y+=button+app.scale(6);
    put(app.watermarkSummary,y,line);y+=line+rowGap;
    put(app.playbackConfigure,y,button);y+=button+app.scale(16);
    const int night=nightRow();
    put(app.nightEnabled,y,check);if(night)y+=check+app.scale(8);
    combo(app.labels[8],app.nightDuration,210,night==2);
    combo(app.labels[9],app.nightTarget,150,night==2);
    put(app.nightHint,y,2*line);if(night==2)y+=2*line+app.scale(2);
    put(app.nightDetail,y,2*line);if(night==2)y+=2*line;
    if(place)app.panelRuleCount=rules;
    return (app.advancedExpanded?y:collapsedBottom)+app.scale(20);
}
constexpr UINT_PTR SavePathTip=1;
void updateSavePathTip() {
    if(!app.tooltip)return;
    TOOLINFOW tip{sizeof(tip)};tip.hwnd=app.window;tip.uId=SavePathTip;tip.rect=app.savePathRect;
    OffsetRect(&tip.rect,-app.scrollX,-(app.scrollY+app.panelScroll));
    SendMessageW(app.tooltip,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&tip));
}
void layout() {
    if (app.layingOut || !app.preview || IsIconic(app.window)) return;
    app.layingOut = true;
    endLayoutDrag();
    updateSourceRows();
    RECT r; GetClientRect(app.window,&r);
    // Solve both scroll bars together: either bar can make the other necessary.
    const auto style=GetWindowLongPtrW(app.window,GWL_STYLE);
    const int barW=GetSystemMetricsForDpi(SM_CXVSCROLL,app.dpi);
    const int barH=GetSystemMetricsForDpi(SM_CYHSCROLL,app.dpi);
    const int availableW=r.right+((style&WS_VSCROLL)?barW:0);
    const int availableH=r.bottom+((style&WS_HSCROLL)?barH:0);
    const int panelW=app.scale(PanelWidth),minimumW=panelW+app.scale(StageMinWidth),stageMinH=app.scale(StageMinHeight);
    app.panelHeight=placePanel(false,0,0);
    bool horizontal=false, vertical=false;
    for(int i=0;i<3;++i) {
        horizontal=availableW-(vertical?barW:0)<minimumW;
        const int viewH=availableH-(horizontal?barH:0);
        // A docked stage always fits; only a taller panel then needs the bar.
        vertical=viewH<stageMinH || app.panelHeight>viewH;
    }
    ShowScrollBar(app.window,SB_HORZ,horizontal);
    ShowScrollBar(app.window,SB_VERT,vertical);
    GetClientRect(app.window,&r);
    const int viewportW=std::max(1L,r.right), viewportH=std::max(1L,r.bottom);
    // Below the stage minimum, fall back to one scrolling canvas with the
    // panel unrolled beside the stage, so every control stays reachable.
    app.panelDocked=viewportH>=stageMinH;
    const int stageH=std::max(viewportH,stageMinH);
    app.contentWidth=std::max(viewportW,minimumW);
    app.contentHeight=app.panelDocked?viewportH:std::max(stageH,app.panelHeight);
    app.scrollX=std::clamp(app.scrollX,0,app.contentWidth-viewportW);
    app.scrollY=app.panelDocked?0:std::clamp(app.scrollY,0,app.contentHeight-viewportH);
    app.panelScroll=app.panelDocked?std::clamp(app.panelScroll,0,std::max(0,app.panelHeight-viewportH)):0;
    SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};
    scroll.nMax=app.contentWidth-1;scroll.nPage=viewportW;scroll.nPos=app.scrollX;
    SetScrollInfo(app.window,SB_HORZ,&scroll,TRUE);
    // Docked, the window's vertical bar sits beside the panel and scrolls only it.
    scroll.nMax=(app.panelDocked?app.panelHeight:app.contentHeight)-1;scroll.nPage=viewportH;
    scroll.nPos=app.panelDocked?app.panelScroll:app.scrollY;
    SetScrollInfo(app.window,SB_VERT,&scroll,TRUE);
    const int panelLeft=app.contentWidth-panelW;
    app.panelRect={panelLeft,0,app.contentWidth,app.contentHeight};
    const int pad=app.scale(StagePad),stageRight=panelLeft-pad,width=std::max(1,stageRight-pad);
    auto move=[&](HWND w,int x,int y,int cx,int cy){MoveWindow(w,x-app.scrollX,y-app.scrollY,cx,cy,TRUE);};
    // Header: state pill and hint, with collage tools at the right end.
    const int headerTop=app.scale(HeaderTop),headerBottom=headerTop+app.scale(HeaderHeight);
    const int toolH=app.scale(30),toolY=headerTop+(app.scale(HeaderHeight)-toolH)/2,forwardW=app.scale(124),resetW=app.scale(112);
    move(app.forward,stageRight-forwardW,toolY,forwardW,toolH);
    move(app.reset,stageRight-forwardW-app.scale(8)-resetW,toolY,resetW,toolH);
    app.headerRect={pad,headerTop,app.collageTools?stageRight-forwardW-resetW-app.scale(20):stageRight,headerBottom};
    // Transport and status hug the bottom; the preview takes the remaining height.
    const int statusTop=stageH-app.scale(StageBottom+StatusHeight);
    app.statusRect={pad,statusTop,stageRight,statusTop+app.scale(StatusHeight)};
    const int transportTop=statusTop-app.scale(StatusGap+TransportHeight);
    app.transportRect={pad,transportTop,stageRight,transportTop+app.scale(TransportHeight)};
    const int previewTop=headerBottom+app.scale(PreviewGap);
    app.previewRect={pad,previewTop,stageRight,transportTop-app.scale(TransportGap)};
    move(app.preview,pad,previewTop,width,app.previewRect.bottom-previewTop);
    RECT previewClient{};GetClientRect(app.preview,&previewClient);
    app.videoRect=previewVideoRect(previewClient);
    const int buttonH=app.scale(40),buttonY=transportTop+(app.scale(TransportHeight)-buttonH)/2,inset=app.scale(12),gap=app.scale(8);
    const int recordW=app.scale(120),pauseW=app.scale(100),finishW=app.scale(112);
    move(app.record,pad+inset,buttonY,recordW,buttonH);
    move(app.pause,pad+inset+recordW+gap,buttonY,pauseW,buttonH);
    move(app.finish,pad+inset+recordW+pauseW+2*gap,buttonY,finishW,buttonH);
    const int statsLeft=pad+inset+recordW+pauseW+finishW+2*gap+app.scale(20);
    app.statsRect={statsLeft,transportTop+app.scale(8),std::max(statsLeft,stageRight-app.scale(16)),app.transportRect.bottom-app.scale(8)};
    layoutStatusRow();
    placePanel(true,panelLeft-app.scrollX,app.scrollY+app.panelScroll);
    updateSavePathTip();
    app.advancedLimitIndex=-1;updateAdvanced();
    app.layingOut = false;
    updateAdvanced();
    InvalidateRect(app.window,nullptr,TRUE);
}
int viewportHeight() { RECT r{};GetClientRect(app.window,&r);return static_cast<int>(r.bottom); }
void scrollTo(int x,int y) {
    RECT r;GetClientRect(app.window,&r);
    x=std::clamp(x,0,std::max(0,app.contentWidth-static_cast<int>(r.right)));
    y=app.panelDocked?0:std::clamp(y,0,std::max(0,app.contentHeight-static_cast<int>(r.bottom)));
    if(x==app.scrollX && y==app.scrollY)return;
    app.scrollX=x;app.scrollY=y;layout();
}
void scrollPanelTo(int y,int x=-1) {
    RECT r;GetClientRect(app.window,&r);
    y=app.panelDocked?std::clamp(y,0,std::max(0,app.panelHeight-static_cast<int>(r.bottom))):0;
    x=x<0?app.scrollX:std::clamp(x,0,std::max(0,app.contentWidth-static_cast<int>(r.right)));
    if(y==app.panelScroll && x==app.scrollX)return;
    app.panelScroll=y;app.scrollX=x;layout();
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
    if(bar==SB_VERT && app.panelDocked)scrollPanelTo(position);
    else scrollTo(bar==SB_HORZ?position:app.scrollX,bar==SB_VERT?position:app.scrollY);
}
void revealFocusedControl() {
    HWND child=GetFocus();
    if(!child || !IsChild(app.window,child))return;
    while(GetParent(child)!=app.window)child=GetParent(child);
    if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))return;
    RECT target{}, viewport{};GetWindowRect(child,&target);GetClientRect(app.window,&viewport);
    MapWindowPoints(nullptr,app.window,reinterpret_cast<POINT*>(&target),2);
    // A docked panel row scrolls the panel; everything else scrolls the canvas.
    const bool panel=app.panelDocked && panelControl(child);
    OffsetRect(&target,app.scrollX,panel?app.panelScroll:app.scrollY);
    // Keep a margin only when it fits; a nearly viewport-sized button must
    // remain fully visible instead of being clipped to make room for padding.
    const int marginX=std::clamp(static_cast<int>((viewport.right-(target.right-target.left))/2),0,app.scale(8));
    const int marginY=std::clamp(static_cast<int>((viewport.bottom-(target.bottom-target.top))/2),0,app.scale(8));
    InflateRect(&target,marginX,marginY);
    auto reveal=[](int position,int size,int first,int last) {
        if(first<position || last-first>size)return first;
        return last>position+size?last-size:position;
    };
    const int x=reveal(app.scrollX,viewport.right,target.left,target.right);
    if(panel)scrollPanelTo(reveal(app.panelScroll,viewport.bottom,target.top,target.bottom),x);
    else scrollTo(x,reveal(app.scrollY,viewport.bottom,target.top,target.bottom));
}
void toggleAdvanced() {
    const HWND focused=GetFocus();
    if(app.advancedExpanded)for(HWND child:{app.encodingMode,app.stopAfter,app.splitEvery,app.lowDisk,app.recoveryMode,app.captureCursor,app.startDelay,app.skipConfigure,app.watermarkConfigure,app.playbackConfigure,app.nightEnabled,app.nightDuration,app.nightTarget})
        if(child && (focused==child || (focused && IsChild(child,focused)))){SetFocus(app.advanced);break;}
    app.advancedExpanded=!app.advancedExpanded;
    updateAdvanced();layout();
    // Bring newly expanded options into view, keeping the toggle on screen.
    if(app.advancedExpanded && app.panelDocked)scrollPanelTo(std::max(app.panelScroll,app.advancedTop-app.scale(12)));
    revealFocusedControl();
}
// Applies one wheel gesture to the panel or canvas. Returns whether that axis
// can scroll; a closed combo then never consumes the gesture as a selection.
bool wheelScroll(UINT message,WPARAM wp,bool overPanel) {
    if(GET_KEYSTATE_WPARAM(wp)&MK_CONTROL)return false;
    const bool horizontal=message==WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp)&MK_SHIFT);
    RECT viewport;GetClientRect(app.window,&viewport);
    const bool panel=!horizontal && app.panelDocked;
    if(panel?(!overPanel || app.panelHeight<=viewport.bottom):(horizontal?app.contentWidth<=viewport.right:app.contentHeight<=viewport.bottom))return false;
    UINT lines=3;
    SystemParametersInfoW(horizontal?SPI_GETWHEELSCROLLCHARS:SPI_GETWHEELSCROLLLINES,0,&lines,0);
    int& remainder=horizontal?app.wheelHorizontal:panel?app.wheelPanel:app.wheelVertical;
    // Horizontal wheel positive means right; vertical wheel positive means up.
    remainder+=GET_WHEEL_DELTA_WPARAM(wp)*(message==WM_MOUSEHWHEEL?1:-1);
    const int steps=remainder/WHEEL_DELTA;remainder%=WHEEL_DELTA;
    const int size=horizontal?viewport.right:viewport.bottom,page=std::max(1,size-app.scale(24));
    const int amount=lines==WHEEL_PAGESCROLL?page:static_cast<int>(std::min<uint64_t>(uint64_t(lines)*app.scale(20),page));
    if(panel)scrollPanelTo(app.panelScroll+steps*amount);
    else scrollTo(app.scrollX+(horizontal?steps*amount:0),app.scrollY+(horizontal?0:steps*amount));
    return true;
}
bool pointInPanel(POINT screen) {
    if(!ScreenToClient(app.window,&screen))return false;
    RECT viewport{};GetClientRect(app.window,&viewport);
    return screen.x>=app.panelRect.left-app.scrollX && screen.x<viewport.right && screen.y>=0 && screen.y<viewport.bottom;
}
bool scrollWheelMessage(const MSG& message) {
    if(message.message!=WM_MOUSEWHEEL && message.message!=WM_MOUSEHWHEEL)return false;
    if(GET_KEYSTATE_WPARAM(message.wParam)&MK_CONTROL)return false;
    if(message.hwnd!=app.window && !IsChild(app.window,message.hwnd))return false;
    // Open lists own their wheel input. Closed lists must not change recording
    // settings when the user's wheel gesture is scrolling the surrounding page.
    bool closedChoice=false;
    for(HWND box:{app.mode,app.interval,app.videoSize,app.encodingQuality,app.encodingMode,app.stopAfter,app.splitEvery,app.startDelay,app.nightDuration,app.nightTarget,app.monitor,app.camera}){
        if(box && SendMessageW(box,CB_GETDROPPEDSTATE,0,0))return false;
        closedChoice=closedChoice || (box && (message.hwnd==box || IsChild(box,message.hwnd)));
    }
    // WM_MOUSEWHEEL can be delivered to the focused combo even when the pointer
    // is over the preview. Route by the gesture's screen point, not that focus.
    const bool overPanel=pointInPanel({GET_X_LPARAM(message.lParam),GET_Y_LPARAM(message.lParam)});
    if(wheelScroll(message.message,message.wParam,overPanel))return true;
    // A wheel over the fixed stage must also be kept away from a focused
    // closed choice, whose default handler would silently change its value.
    return app.panelDocked && !overPanel && closedChoice;
}
void fonts() {
    for(HFONT* font:{&app.font,&app.titleFont,&app.smallFont,&app.strongFont,&app.headerFont}){DeleteObject(*font);*font=nullptr;}
    const auto make=[](int pixels,int weight){return CreateFontW(-app.scale(pixels),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");};
    app.font=make(14,FW_NORMAL);app.titleFont=make(24,FW_SEMIBOLD);app.smallFont=make(12,FW_NORMAL);
    app.strongFont=make(14,FW_SEMIBOLD);app.headerFont=make(12,FW_SEMIBOLD);
    EnumChildWindows(app.window,[](HWND w,LPARAM p)->BOOL { SendMessageW(w,WM_SETFONT,p,TRUE); return TRUE; },reinterpret_cast<LPARAM>(app.font));
    for(HWND child:{app.statusText,app.statusDetails,app.startDelayHint,app.nightHint,app.nightDetail,app.skipSummary,app.skipDetail,app.watermarkSummary})
        if(child)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
}

enum class CustomKind { Interval, Size, Limit, Range, Segment, Night };
enum CustomId { CustomFirst=5001, CustomSecond, CustomUnits, CustomHelp, CustomError, CustomFirstLabel, CustomSecondLabel };
struct CustomDraft {
    CustomKind kind=CustomKind::Interval;
    int64_t durationMs=5000;
    int width=1280,height=720,dpi=96,scrollX=0,scrollY=0,wheelX=0,wheelY=0;
    int startSeconds=0,endSeconds=60;
    HWND previousDialog{};
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
enum class DialogWheel { Pass, Consumed, Scrolled };
int dialogWheelAxis(UINT message,WPARAM wp) {
    return message==WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp)&MK_SHIFT)?SB_HORZ:SB_VERT;
}
int64_t dialogScrollMaximum(const SCROLLINFO& info) {
    return std::max<int64_t>(info.nMin,int64_t(info.nMax)-std::max<int64_t>(0,int64_t(info.nPage)-1));
}
bool dialogWheelRange(HWND window,UINT message,WPARAM wp,SCROLLINFO& info) {
    if(GET_KEYSTATE_WPARAM(wp)&MK_CONTROL)return false;
    return GetScrollInfo(window,dialogWheelAxis(message,wp),&info) && dialogScrollMaximum(info)>info.nMin;
}
DialogWheel dialogWheel(HWND window,CustomDraft& draft,UINT message,WPARAM wp) {
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};
    if(!dialogWheelRange(window,message,wp,info))return DialogWheel::Pass;
    const bool horizontal=dialogWheelAxis(message,wp)==SB_HORZ;
    int& remainder=horizontal?draft.wheelX:draft.wheelY;
    UINT lines=3;SystemParametersInfoW(horizontal?SPI_GETWHEELSCROLLCHARS:SPI_GETWHEELSCROLLLINES,0,&lines,0);
    if(!lines){remainder=0;return DialogWheel::Consumed;}
    remainder+=GET_WHEEL_DELTA_WPARAM(wp)*(message==WM_MOUSEHWHEEL?1:-1);
    const int steps=remainder/WHEEL_DELTA;remainder%=WHEEL_DELTA;
    if(!steps)return DialogWheel::Consumed;
    const int64_t page=std::max<int64_t>(1,int64_t(info.nPage)-draft.scale(24));
    const int64_t amount=lines==WHEEL_PAGESCROLL?page:std::min<int64_t>(int64_t(lines)*draft.scale(24),page);
    const int position=static_cast<int>(std::clamp<int64_t>(int64_t(info.nPos)+int64_t(steps)*amount,info.nMin,dialogScrollMaximum(info)));
    if(position==info.nPos)return DialogWheel::Consumed;
    (horizontal?draft.scrollX:draft.scrollY)=position;
    return DialogWheel::Scrolled;
}
LRESULT CALLBACK dialogComboWheelProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    if((message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) && !SendMessageW(window,CB_GETDROPPEDSTATE,0,0)){
        const HWND parent=GetParent(window);SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};
        if(dialogWheelRange(parent,message,wp,info)){
            // Closed choices scroll the overflowing page, as in the main UI.
            // Open dropdowns keep their native wheel handling.
            SendMessageW(parent,message,wp,lp);return 0;
        }
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,dialogComboWheelProc,id);
    return DefSubclassProc(window,message,wp,lp);
}
bool dialogWheelCombos(std::initializer_list<HWND> boxes) {
    for(HWND box:boxes)if(!SetWindowSubclass(box,dialogComboWheelProc,1,0))return false;
    return true;
}
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
    if(width<=client.right)draft.wheelX=0;if(height<=client.bottom)draft.wheelY=0;
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
constexpr int StatusDetailsText=5401,StatusDetailsFiles=5402,StatusDetailsFilesStatus=5403;
constexpr UINT_PTR StatusDetailsFilesTimer=2;
enum class StatusFilesPhase { Resolving, Cancelled, Selecting };
struct StatusFilesTask {
    std::vector<std::wstring> files;
    std::wstring folder;
    std::atomic<StatusFilesPhase> phase{StatusFilesPhase::Resolving};
    std::atomic<bool> done{false};
    HRESULT result=E_PENDING;
    void cancel() noexcept {auto expected=StatusFilesPhase::Resolving;phase.compare_exchange_strong(expected,StatusFilesPhase::Cancelled);}
};
std::vector<std::wstring> statusDetailsFiles(const Status& status,std::wstring& folder) {
    folder.clear();
    if(status.savedPaths.size()>2)return {};
    auto files=status.savedPaths;
    if(files.empty()&&!status.savedPath.empty())files.push_back(status.savedPath);
    std::wstring parent;
    for(const auto& file:files){
        if(file.empty()||file.find(L'\0')!=std::wstring::npos)return {};
        const std::filesystem::path path(file);
        if(!path.is_absolute()||!path.has_filename()||path.filename()==L"."||path.filename()==L"..")return {};
        const auto current=path.parent_path().wstring();
        if(current.empty()||(!parent.empty()&&CompareStringOrdinal(parent.c_str(),-1,current.c_str(),-1,TRUE)!=CSTR_EQUAL))return {};
        parent=current;
    }
    folder=std::move(parent);return files;
}
HRESULT selectStatusFiles(StatusFilesTask& task) {
    // Shell parsing can block on storage/providers. This worker owns all inputs
    // and resources and never reads App, a window, or the dialog's lifetime.
    if(task.files.empty()||task.files.size()>2||task.folder.empty())return E_INVALIDARG;
    struct Com {HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);~Com(){if(SUCCEEDED(result))CoUninitialize();}} com;
    if(FAILED(com.result))return com.result;
    const auto cancelled=[&]{return task.phase.load()==StatusFilesPhase::Cancelled;};
    if(cancelled())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    struct Pidl {PIDLIST_ABSOLUTE value{};~Pidl(){CoTaskMemFree(value);}} folder,files[2];
    SFGAOF attributes=0;
    HRESULT result=SHParseDisplayName(task.folder.c_str(),nullptr,&folder.value,SFGAO_FILESYSTEM|SFGAO_FOLDER,&attributes);
    if(FAILED(result))return result;
    if(!folder.value||(attributes&(SFGAO_FILESYSTEM|SFGAO_FOLDER))!=(SFGAO_FILESYSTEM|SFGAO_FOLDER))return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    std::array<PCUITEMID_CHILD,2> children{};
    for(size_t i=0;i<task.files.size();++i){
        if(cancelled())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        attributes=0;result=SHParseDisplayName(task.files[i].c_str(),nullptr,&files[i].value,SFGAO_FILESYSTEM|SFGAO_FOLDER,&attributes);
        if(FAILED(result))return result;
        if(!files[i].value||!(attributes&SFGAO_FILESYSTEM)||(attributes&SFGAO_FOLDER))return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        Pidl parent;parent.value=ILCloneFull(files[i].value);
        if(!parent.value)return E_OUTOFMEMORY;
        if(!ILRemoveLastID(parent.value)||!ILIsEqual(parent.value,folder.value))return E_INVALIDARG;
        children[i]=ILFindLastID(files[i].value);
    }
    // Cancellation wins until this transition. An Explorer request already
    // dispatched cannot be revoked by closing the dialog.
    auto expected=StatusFilesPhase::Resolving;
    if(!task.phase.compare_exchange_strong(expected,StatusFilesPhase::Selecting))return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return SHOpenFolderAndSelectItems(folder.value,static_cast<UINT>(task.files.size()),children.data(),0);
}
unsigned __stdcall statusFilesWorker(void* argument) noexcept {
    std::unique_ptr<std::shared_ptr<StatusFilesTask>> owned(static_cast<std::shared_ptr<StatusFilesTask>*>(argument));
    const auto task=*owned;owned.reset();
    try {task->result=selectStatusFiles(*task);}
    catch(const std::bad_alloc&){task->result=E_OUTOFMEMORY;}
    catch(...){task->result=E_FAIL;}
    // All COM/PIDL resources are released before the next request may start.
    shellOperationBusy.store(false,std::memory_order_release);
    task->done.store(true,std::memory_order_release);return 0;
}
struct StatusDetailsDraft : CustomDraft {
    std::wstring snapshot,filesFolder;
    std::vector<std::wstring> files;
    HWND contents{},showFiles{},filesStatus{};
    std::shared_ptr<StatusFilesTask> filesTask;
    bool filesTimer=false,compactHelp=false;
    ~StatusDetailsDraft(){if(filesTask)filesTask->cancel();}
};
constexpr wchar_t StatusDetailsHelp[]=L"Snapshot when opened. Select text, or press Ctrl+A, then Ctrl+C to copy.";
LRESULT CALLBACK statusDetailsEditProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    if((message==WM_KEYDOWN && wp=='A') || (message==WM_CHAR && wp==1)){
        const bool selectAll=(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000);
        if(selectAll){if(message==WM_KEYDOWN)SendMessageW(window,EM_SETSEL,0,-1);return 0;}
    }
    return DefSubclassProc(window,message,wp,lp);
}
void statusDetailsLayout(HWND window,StatusDetailsDraft& draft) {
    if(draft.layingOut)return;draft.layingOut=true;
    RECT client{};GetClientRect(window,&client);
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    const int barW=GetSystemMetricsForDpi(SM_CXVSCROLL,draft.dpi),barH=GetSystemMetricsForDpi(SM_CYHSCROLL,draft.dpi);
    const int availableW=client.right+((style&WS_VSCROLL)?barW:0),availableH=client.bottom+((style&WS_HSCROLL)?barH:0);
    const int pad=draft.scale(18),gap=draft.scale(12),buttonH=draft.scale(28);
    const bool compact=availableW<draft.scale(480);
    const wchar_t* help=compact?L"Snapshot. Ctrl+A, Ctrl+C to copy.":StatusDetailsHelp;
    if(compact!=draft.compactHelp){draft.compactHelp=compact;SetWindowTextW(draft.help,help);}
    HDC dc=GetDC(window);const auto previous=dc?SelectObject(dc,draft.font?draft.font:GetStockObject(DEFAULT_GUI_FONT)):nullptr;
    const auto buttonWidth=[&](const wchar_t* value,int minimum){SIZE size{};
        if(dc)GetTextExtentPoint32W(dc,value,static_cast<int>(std::wcslen(value)),&size);
        return std::max(draft.scale(minimum),int(size.cx)+draft.scale(32));};
    const int closeW=buttonWidth(L"Close",88),showW=draft.showFiles?buttonWidth(L"Show files",112):0;
    const int minimumW=std::max(closeW,showW)+2*pad;
    const auto measure=[&](const wchar_t* value,int inner,int minimum){RECT measured{0,0,std::max(1,inner),0};
        if(dc)DrawTextW(dc,value,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        return std::max(minimum,static_cast<int>(measured.bottom));};
    wchar_t filesMessage[384]{};if(draft.filesStatus)GetWindowTextW(draft.filesStatus,filesMessage,static_cast<int>(std::size(filesMessage)));
    const bool feedback=filesMessage[0]!=0;
    if(draft.filesStatus)ShowWindow(draft.filesStatus,feedback?SW_SHOWNA:SW_HIDE);
    bool horizontal=false,vertical=false,stacked=false;int helpH=0,filesH=0,minimumH=0;
    const auto measureCanvas=[&](int width){const int inner=width-2*pad;
        helpH=measure(help,inner,draft.scale(24));filesH=feedback?measure(filesMessage,inner,draft.scale(24)):0;
        stacked=draft.showFiles&&showW+gap+closeW>inner;
        minimumH=2*pad+helpH+gap+draft.scale(56)+gap+(feedback?filesH+gap:0)+buttonH+(stacked?buttonH+gap:0);};
    for(int pass=0;pass<3;++pass){horizontal=availableW-(vertical?barW:0)<minimumW;
        measureCanvas(std::max(minimumW,availableW-(vertical?barW:0)));vertical=availableH-(horizontal?barH:0)<minimumH;}
    ShowScrollBar(window,SB_HORZ,horizontal);ShowScrollBar(window,SB_VERT,vertical);GetClientRect(window,&client);
    const int width=std::max(minimumW,int(client.right));measureCanvas(width);
    const int height=std::max(minimumH,int(client.bottom)),inner=width-2*pad;
    if(dc){SelectObject(dc,previous);ReleaseDC(window,dc);}
    if(width<=client.right)draft.wheelX=0;if(height<=client.bottom)draft.wheelY=0;
    draft.scrollX=std::clamp(draft.scrollX,0,width-int(std::max(1L,client.right)));
    draft.scrollY=std::clamp(draft.scrollY,0,height-int(std::max(1L,client.bottom)));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};info.nMax=width-1;info.nPage=std::max(1L,client.right);info.nPos=draft.scrollX;SetScrollInfo(window,SB_HORZ,&info,TRUE);
    info.nMax=height-1;info.nPage=std::max(1L,client.bottom);info.nPos=draft.scrollY;SetScrollInfo(window,SB_VERT,&info,TRUE);
    const int closeY=height-pad-buttonH,showY=closeY-(stacked?buttonH+gap:0),actionsY=draft.showFiles?showY:closeY;
    const int footerY=actionsY-gap-filesH,textY=pad+helpH+gap;
    const auto move=[&](HWND child,int x,int y,int w,int h){if(child)MoveWindow(child,x-draft.scrollX,y-draft.scrollY,w,h,TRUE);};
    move(draft.help,pad,pad,inner,helpH);move(draft.contents,pad,textY,inner,(feedback?footerY-gap:actionsY-gap)-textY);
    if(feedback)move(draft.filesStatus,pad,footerY,inner,filesH);
    move(draft.showFiles,pad,showY,showW,buttonH);move(draft.cancel,width-pad-closeW,closeY,closeW,buttonH);
    draft.layingOut=false;
}
void statusDetailsReveal(HWND window,StatusDetailsDraft& draft,HWND child) {
    if(!child||!IsChild(window,child))return;
    RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
    if(bounds.left<0)draft.scrollX+=bounds.left;else if(bounds.right>client.right)draft.scrollX+=bounds.right-client.right;
    if(bounds.top<0)draft.scrollY+=bounds.top;else if(bounds.bottom>client.bottom)draft.scrollY+=bounds.bottom-client.bottom;
    statusDetailsLayout(window,draft);
}
void statusFilesFeedback(HWND window,StatusDetailsDraft& draft,const wchar_t* message) {
    SetWindowTextW(draft.filesStatus,message);statusDetailsLayout(window,draft);statusDetailsReveal(window,draft,GetFocus());
}
void cancelStatusFiles(HWND window,StatusDetailsDraft& draft) noexcept {
    if(draft.filesTask)draft.filesTask->cancel();
    if(draft.filesTimer){KillTimer(window,StatusDetailsFilesTimer);draft.filesTimer=false;}
}
void startStatusFiles(HWND window,StatusDetailsDraft& draft) {
    if(draft.files.empty()||draft.filesTask||!draft.showFiles||!IsWindowEnabled(draft.showFiles))return;
    bool expected=false;
    if(!shellOperationBusy.compare_exchange_strong(expected,true)){
        statusFilesFeedback(window,draft,L"Another file request is finishing. Try again in a moment.");return;
    }
    app.shellBusyObserved=true;refreshOpenFolderControl();
    std::shared_ptr<StatusFilesTask> task;
    std::unique_ptr<std::shared_ptr<StatusFilesTask>> argument;
    try {
        task=std::make_shared<StatusFilesTask>();task->files=draft.files;task->folder=draft.filesFolder;
        argument=std::make_unique<std::shared_ptr<StatusFilesTask>>(task);
    } catch(...) {
        shellOperationBusy.store(false,std::memory_order_release);refreshOpenFolderControl();
        statusFilesFeedback(window,draft,L"Not enough memory to show the files. Try again.");return;
    }
    // Establish completion delivery before launch. Failure starts no worker.
    if(!SetTimer(window,StatusDetailsFilesTimer,150,nullptr)){
        shellOperationBusy.store(false,std::memory_order_release);refreshOpenFolderControl();
        statusFilesFeedback(window,draft,L"Windows could not prepare the file request. Try again.");return;
    }
    draft.filesTimer=true;draft.filesTask=task;
    const uintptr_t thread=_beginthreadex(nullptr,0,statusFilesWorker,argument.get(),0,nullptr);
    if(!thread){
        cancelStatusFiles(window,draft);draft.filesTask.reset();shellOperationBusy.store(false,std::memory_order_release);refreshOpenFolderControl();
        statusFilesFeedback(window,draft,L"Windows could not start the file request. Try again.");return;
    }
    argument.release();CloseHandle(reinterpret_cast<HANDLE>(thread));
    if(GetFocus()==draft.showFiles)SetFocus(draft.cancel);
    EnableWindow(draft.showFiles,FALSE);
    statusFilesFeedback(window,draft,L"Opening in Explorer...");
}
INT_PTR CALLBACK statusDetailsProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* draft=reinterpret_cast<StatusDetailsDraft*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_INITDIALOG){
            draft=reinterpret_cast<StatusDetailsDraft*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);
            draft->previousDialog=app.customDialog;app.customDialog=window;
            draft->dpi=static_cast<int>(GetDpiForWindow(window));if(draft->dpi<=0)draft->dpi=app.dpi;
            SetWindowTextW(window,L"Status details");
            const auto child=[&](const wchar_t* type,const wchar_t* label,DWORD style,int id){return CreateWindowExW(std::wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,
                type,label,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);};
            draft->help=child(L"STATIC",StatusDetailsHelp,SS_NOPREFIX,5400);
            draft->contents=child(L"EDIT",L"",WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,StatusDetailsText);
            if(!draft->files.empty()){
                draft->filesStatus=child(L"STATIC",L"",SS_NOPREFIX,StatusDetailsFilesStatus);
                draft->showFiles=child(L"BUTTON",L"Show &files",WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY,StatusDetailsFiles);
            }
            draft->cancel=child(L"BUTTON",L"Close",WS_TABSTOP|BS_DEFPUSHBUTTON|BS_NOTIFY,IDCANCEL);
            if(!draft->help || !draft->contents || !draft->cancel ||
                (!draft->files.empty()&&(!draft->showFiles||!draft->filesStatus)) ||
                !SetWindowSubclass(draft->contents,statusDetailsEditProc,1,0)){EndDialog(window,-1);return TRUE;}
            SendMessageW(window,DM_SETDEFID,IDCANCEL,0);
            SendMessageW(draft->contents,EM_SETLIMITTEXT,0,0);
            if(draft->snapshot.size()>INT_MAX || !SetWindowTextW(draft->contents,draft->snapshot.c_str()) ||
                static_cast<size_t>(GetWindowTextLengthW(draft->contents))!=draft->snapshot.size()){EndDialog(window,-1);return TRUE;}
            customFont(window,*draft);
            RECT rect{0,0,draft->scale(640),draft->scale(380)};
            AdjustWindowRectExForDpi(&rect,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),draft->dpi);
            RECT owner{};GetWindowRect(GetWindow(window,GW_OWNER),&owner);
            OffsetRect(&rect,(owner.left+owner.right-(rect.right-rect.left))/2-rect.left,(owner.top+owner.bottom-(rect.bottom-rect.top))/2-rect.top);
            rect=fitWindow(rect,workArea(MonitorFromWindow(GetWindow(window,GW_OWNER),MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
            statusDetailsLayout(window,*draft);SetFocus(draft->contents);SendMessageW(draft->contents,EM_SETSEL,0,0);return FALSE;
        }
        if(!draft)return FALSE;
        switch(message){
        case WM_SIZE:statusDetailsLayout(window,*draft);statusDetailsReveal(window,*draft,GetFocus());return TRUE;
        case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:{
            const auto result=dialogWheel(window,*draft,message,wp);
            if(result==DialogWheel::Scrolled)statusDetailsLayout(window,*draft);
            if(result!=DialogWheel::Pass)return TRUE;break;}
        case WM_HSCROLL:case WM_VSCROLL:{
            if(lp)break;
            const int bar=message==WM_HSCROLL?SB_HORZ:SB_VERT;SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int position=info.nPos;
            switch(LOWORD(wp)){case SB_LINEUP:position-=draft->scale(24);break;case SB_LINEDOWN:position+=draft->scale(24);break;case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:position=info.nTrackPos;break;case SB_TOP:position=0;break;case SB_BOTTOM:position=info.nMax;break;default:return TRUE;}
            (bar==SB_HORZ?draft->scrollX:draft->scrollY)=position;statusDetailsLayout(window,*draft);return TRUE;}
        case WM_GETMINMAXINFO:{RECT minimum{0,0,draft->scale(320),draft->scale(220)};
            AdjustWindowRectExForDpi(&minimum,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),draft->dpi);
            const RECT work=workArea(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST));auto* limits=reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMinTrackSize={std::min(minimum.right-minimum.left,work.right-work.left),std::min(minimum.bottom-minimum.top,work.bottom-work.top)};return TRUE;}
        case WM_DPICHANGED:{draft->dpi=HIWORD(wp);customFont(window,*draft);
            RECT rect=*reinterpret_cast<RECT*>(lp);rect=fitWindow(rect,workArea(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
            statusDetailsLayout(window,*draft);statusDetailsReveal(window,*draft,GetFocus());return TRUE;}
        case WM_COMMAND:
            if((LOWORD(wp)==StatusDetailsText&&HIWORD(wp)==EN_SETFOCUS)||
                ((LOWORD(wp)==StatusDetailsFiles||LOWORD(wp)==IDCANCEL)&&HIWORD(wp)==BN_SETFOCUS)){
                if(GetFocus()==reinterpret_cast<HWND>(lp))statusDetailsReveal(window,*draft,reinterpret_cast<HWND>(lp));return TRUE;}
            if(HIWORD(wp)!=BN_CLICKED)break;
            if(LOWORD(wp)==StatusDetailsFiles){startStatusFiles(window,*draft);return TRUE;}
            if(LOWORD(wp)==IDCANCEL || LOWORD(wp)==IDOK){cancelStatusFiles(window,*draft);EndDialog(window,IDCANCEL);return TRUE;}break;
        case WM_TIMER:
            if(wp==StatusDetailsFilesTimer&&draft->filesTask&&draft->filesTask->done.load(std::memory_order_acquire)){
                const HRESULT result=draft->filesTask->result;
                cancelStatusFiles(window,*draft);draft->filesTask.reset();EnableWindow(draft->showFiles,TRUE);
                if(SUCCEEDED(result))statusFilesFeedback(window,*draft,L"Selection requested in Explorer.");
                else {wchar_t error[256]{};swprintf_s(error,L"Could not show files (0x%08X). They may have moved or be unavailable.",static_cast<unsigned>(result));
                    statusFilesFeedback(window,*draft,error);}
                return TRUE;
            }
            break;
        case CancelOwnedWorkMessage:cancelStatusFiles(window,*draft);return TRUE;
        case WM_CTLCOLORSTATIC:if(reinterpret_cast<HWND>(lp)==draft->contents){SetTextColor(reinterpret_cast<HDC>(wp),Ink);
            SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_WINDOW));return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));}break;
        case WM_CLOSE:cancelStatusFiles(window,*draft);EndDialog(window,IDCANCEL);return TRUE;
        case WM_DESTROY:cancelStatusFiles(window,*draft);if(draft->font){DeleteObject(draft->font);draft->font=nullptr;}
            if(app.customDialog==window)app.customDialog=IsWindow(draft->previousDialog)?draft->previousDialog:nullptr;return TRUE;
        }
    } catch(...){if(draft)cancelStatusFiles(window,*draft);EndDialog(window,-1);return TRUE;}
    return FALSE;
}
void showStatusDetails() {
    const HWND owner=app.window,focused=GetFocus();
    if(app.customDialog || !statusDetailsOwnerReady(owner) || !hasStatusDetails())return;
    bool failed=false;
    try {
        // Own all text before entering a nested message loop. The native edit
        // receives this snapshot once, so timers cannot disturb selection.
        StatusDetailsDraft draft;draft.snapshot=statusDetailsSnapshot();draft.files=statusDetailsFiles(app.status,draft.filesFolder);CustomTemplate resource;
        resource.dialog.style|=WS_THICKFRAME;
        failed=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,owner,statusDetailsProc,reinterpret_cast<LPARAM>(&draft))==-1;
    } catch(...) {failed=true;}
    if(!statusDetailsOwnerReady(owner))return;
    if(failed)MessageBoxW(owner,L"Status details could not be opened. The original report is still available in the status line and its tooltip. Try again.",L"Timelapse",MB_OK|MB_ICONERROR);
    restoreStatusFocus(owner,focused);
}
bool validateCustom(CustomDraft& draft,std::wstring& message,HWND& invalid) {
    wchar_t first[96]{},second[96]{};invalid=draft.first;
    if(GetWindowTextLengthW(draft.first)>=96){message=L"Enter a shorter complete number.";return false;}
    GetWindowTextW(draft.first,first,96);
    if(draft.kind==CustomKind::Range){
        int64_t start=0,end=0;
        if(!parseDuration(first,DurationUnit::Seconds,0,int64_t(INT_MAX)*1000,1000,start,message))return false;
        invalid=draft.second;if(GetWindowTextLengthW(draft.second)>=96){message=L"Enter a shorter complete number.";return false;}
        GetWindowTextW(draft.second,second,96);
        if(!parseDuration(second,DurationUnit::Seconds,1000,int64_t(INT_MAX)*1000,1000,end,message))return false;
        if(end<=start){message=L"End after must be later than Start after.";return false;}
        draft.startSeconds=static_cast<int>(start/1000);draft.endSeconds=static_cast<int>(end/1000);return true;
    }
    if(draft.kind==CustomKind::Size){
        int width=0,height=0;
        if(!parsePixelDimension(first,width,message))return false;
        invalid=draft.second;if(GetWindowTextLengthW(draft.second)>=96){message=L"Enter a shorter complete number.";return false;}GetWindowTextW(draft.second,second,96);
        if(!parsePixelDimension(second,height,message))return false;
        if(width<MinVideoDimension || width>MaxVideoDimension || (width&1))invalid=draft.first;
        if(!validateVideoSize(width,height,message))return false;
        draft.width=width;draft.height=height;return true;
    }
    const bool interval=draft.kind==CustomKind::Interval,night=draft.kind==CustomKind::Night;
    int64_t duration=0;const auto unit=static_cast<DurationUnit>(std::clamp(choice(draft.units),0,night?0:interval?2:3));
    if(!parseDuration(first,unit,night?NightMinDurationMs:interval?MinCaptureIntervalMs:1000,
                      night?int64_t(NightMaxDurationMs):interval?int64_t(MaxCaptureIntervalMs):int64_t(INT_MAX)*1000,
                      (interval || night)?1:1000,duration,message))return false;
    draft.durationMs=duration;return true;
}
INT_PTR CALLBACK customProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* draft=reinterpret_cast<CustomDraft*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_INITDIALOG){
            draft=reinterpret_cast<CustomDraft*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);draft->previousDialog=app.customDialog;app.customDialog=window;
            draft->dpi=static_cast<int>(GetDpiForWindow(window));if(draft->dpi<=0)draft->dpi=app.dpi;
            const bool range=draft->kind==CustomKind::Range,size=draft->kind==CustomKind::Size,interval=draft->kind==CustomKind::Interval,night=draft->kind==CustomKind::Night;
            SetWindowTextW(window,range?L"Compression range":size?L"Custom video size":interval?L"Custom capture interval":night?L"Custom Night blend":draft->kind==CustomKind::Segment?L"Custom file split":L"Custom stop time");
            const auto child=[&](const wchar_t* type,const wchar_t* text,DWORD style,int id){return CreateWindowExW(std::wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,
                type,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);};
            draft->firstLabel=child(L"STATIC",range?L"&Start after (seconds)":size?L"&Width (pixels)":L"&Value",0,CustomFirstLabel);
            draft->first=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,CustomFirst);
            draft->secondLabel=child(L"STATIC",range?L"&End after (seconds)":size?L"&Height (pixels)":L"&Units",0,CustomSecondLabel);
            if(size || range)draft->second=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,CustomSecond);
            else {draft->units=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,CustomUnits);
                add(draft->units,L"Seconds");if(!night){add(draft->units,L"Minutes");add(draft->units,L"Hours");if(!interval)add(draft->units,L"Days");}choose(draft->units,0);}
            draft->help=child(L"STATIC",range?L"Use whole active seconds from 0 to 2,147,483,647. Pauses and initial preparation do not count. Start is included; end is excluded. Overlapping or touching ranges merge when you return to the schedule.":size?L"Use even dimensions from 48 to 4096 pixels, at most 8,847,360 pixels total. Sources fit inside the video without stretching.":
                interval?L"Choose 0.1 seconds to 24 hours. Decimals use a point and must resolve to whole milliseconds. Playback uses the frame rate set in Advanced > Playback & shortcuts.":
                night?L"Request a software blend window from 1 to 30 seconds, in whole milliseconds (for example, 1.25). Automatic brightness continues; camera timing can vary. This does not change shutter settings. The duration must not exceed Capture every.":
                draft->kind==CustomKind::Segment?L"Choose 1 to 2,147,483,647 whole seconds of active recording per part. Pauses and initial preparation do not count; automatic saving does. Empty periods create no files. Shorter parts add processing and file overhead.":
                L"Choose 1 to 2,147,483,647 seconds of active recording. Pauses and initial preparation do not count. Decimals must resolve to whole seconds.",SS_NOPREFIX,CustomHelp);
            draft->error=child(L"STATIC",L"",SS_NOPREFIX,CustomError);
            draft->okay=child(L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON|BS_NOTIFY,IDOK);draft->cancel=child(L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY,IDCANCEL);
            if(!draft->firstLabel || !draft->secondLabel || !draft->first || !((size || range)?draft->second:draft->units) || !draft->help || !draft->error || !draft->okay || !draft->cancel){EndDialog(window,-1);return TRUE;}
            if(draft->units && !dialogWheelCombos({draft->units})){EndDialog(window,-1);return TRUE;}
            // Native paste must reach the rejection threshold, never truncate
            // below it into a different valid numeric prefix.
            SendMessageW(draft->first,EM_SETLIMITTEXT,96,0);if(draft->second)SendMessageW(draft->second,EM_SETLIMITTEXT,96,0);
            SetWindowTextW(draft->first,(range?std::to_wstring(draft->startSeconds):size?std::to_wstring(draft->width):secondsInput(draft->durationMs)).c_str());
            if(draft->second)SetWindowTextW(draft->second,std::to_wstring(range?draft->endSeconds:draft->height).c_str());customFont(window,*draft);
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
        case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:{
            const auto result=dialogWheel(window,*draft,message,wp);
            if(result==DialogWheel::Scrolled)customLayout(window,*draft);
            if(result!=DialogWheel::Pass)return TRUE;break;}
        case WM_DPICHANGED:{draft->dpi=HIWORD(wp);customFont(window,*draft);RECT rect=*reinterpret_cast<RECT*>(lp);rect=fitWindow(rect,workArea(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);customLayout(window,*draft);customReveal(window,*draft,GetFocus());return TRUE;}
        case WM_HSCROLL:case WM_VSCROLL:{const int bar=message==WM_HSCROLL?SB_HORZ:SB_VERT;SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int position=info.nPos;
            switch(LOWORD(wp)){case SB_LINEUP:position-=draft->scale(24);break;case SB_LINEDOWN:position+=draft->scale(24);break;case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:position=info.nTrackPos;break;case SB_TOP:position=0;break;case SB_BOTTOM:position=info.nMax;break;default:return TRUE;}
            (bar==SB_HORZ?draft->scrollX:draft->scrollY)=position;customLayout(window,*draft);return TRUE;}
        case WM_COMMAND:{const int id=LOWORD(wp),code=HIWORD(wp);
            if(((id==IDOK || id==IDCANCEL) && code==BN_SETFOCUS) ||
               ((id==CustomFirst || id==CustomSecond) && code==EN_SETFOCUS) || (id==CustomUnits && code==CBN_SETFOCUS)){
                customReveal(window,*draft,reinterpret_cast<HWND>(lp));return TRUE;
            }
            if(LOWORD(wp)==IDCANCEL && HIWORD(wp)==BN_CLICKED){EndDialog(window,IDCANCEL);return TRUE;}
            if(LOWORD(wp)==IDOK && HIWORD(wp)==BN_CLICKED){
                if(app.active()){EndDialog(window,IDCANCEL);return TRUE;}
                std::wstring error;HWND invalid{};
                if(validateCustom(*draft,error,invalid)){EndDialog(window,IDOK);return TRUE;}
                SetWindowTextW(draft->error,error.c_str());SetFocus(invalid);SendMessageW(invalid,EM_SETSEL,0,-1);customReveal(window,*draft,invalid);return TRUE;
            }
            return FALSE;}
        case WM_CTLCOLORSTATIC:if(reinterpret_cast<HWND>(lp)==draft->error){SetTextColor(reinterpret_cast<HDC>(wp),RGB(164,40,40));SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_BTNFACE));return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));}break;
        case WM_CLOSE:EndDialog(window,IDCANCEL);return TRUE;
        case WM_DESTROY:if(draft->font){DeleteObject(draft->font);draft->font=nullptr;}if(app.customDialog==window)app.customDialog=IsWindow(draft->previousDialog)?draft->previousDialog:nullptr;return TRUE;
        }
    } catch (...) {EndDialog(window,-1);return TRUE;}
    return FALSE;
}
void commitCustom(const CustomDraft& draft) {
    if(draft.kind==CustomKind::Interval){app.customIntervalMs=static_cast<int>(draft.durationMs);app.hasCustomInterval=true;app.committedInterval=6;
        for(int i=0;i<6;++i)if(CaptureIntervals[i]==draft.durationMs)app.committedInterval=i;}
    else if(draft.kind==CustomKind::Size){app.customWidth=draft.width;app.customHeight=draft.height;app.hasCustomSize=true;
        app.committedSize=draft.width==1280 && draft.height==720?0:draft.width==1920 && draft.height==1080?1:2;}
    else if(draft.kind==CustomKind::Segment){app.customSegmentSeconds=static_cast<int>(draft.durationMs/1000);app.hasCustomSegment=true;app.committedSegment=5;}
    else if(draft.kind==CustomKind::Night){app.customNightDurationMs=static_cast<int>(draft.durationMs);app.hasCustomNightDuration=true;app.committedNightDuration=6;}
    else {app.customLimitSeconds=static_cast<int>(draft.durationMs/1000);app.hasCustomLimit=true;app.committedLimit=6;
        for(int i=1;i<6;++i)if(RecordingLimits[i]==app.customLimitSeconds)app.committedLimit=i;}
    normalizeCustomSelections();customItems();configure();updateControls();layout();InvalidateRect(app.preview,nullptr,FALSE);InvalidateRect(app.window,nullptr,FALSE);
}
void editCustom(CustomKind kind) {
    if(app.active() || app.customDialog)return;
    HWND box=kind==CustomKind::Interval?app.interval:kind==CustomKind::Size?app.videoSize:kind==CustomKind::Segment?app.splitEvery:kind==CustomKind::Night?app.nightDuration:app.stopAfter;
    choose(box,kind==CustomKind::Interval?app.committedInterval:kind==CustomKind::Size?app.committedSize:kind==CustomKind::Segment?app.committedSegment:kind==CustomKind::Night?app.committedNightDuration:app.committedLimit);
    const int duration=kind==CustomKind::Segment?app.settings.segmentDurationSeconds:app.settings.recordingLimitSeconds;
    CustomDraft draft;draft.kind=kind;draft.durationMs=kind==CustomKind::Interval?app.settings.intervalMs:kind==CustomKind::Night?
        (app.settings.night.durationMs?app.settings.night.durationMs:NightInitialDurationMs):int64_t(duration?duration:900)*1000;
    draft.width=app.settings.width;draft.height=app.settings.height;CustomTemplate resource;
    const auto outcome=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,app.window,customProc,reinterpret_cast<LPARAM>(&draft));
    if(!IsWindow(app.window))return;
    if(outcome==IDOK && !app.active())commitCustom(draft);
    else if(outcome==-1)MessageBoxW(app.window,L"The custom settings dialog could not be opened. Try again.",L"Timelapse",MB_OK|MB_ICONERROR);
    if(!app.closeWhenDone && !app.hiddenToTray){SetFocus(box);revealFocusedControl();}
}
enum SkipId { SkipMode=5101,SkipSpeed,SkipRamp,SkipQuiet,SkipQuietUnits,SkipRanges,SkipAdd,SkipEdit,SkipRemove,SkipRepeat,SkipRepeatUnits,SkipHelp,SkipError,SkipPackInfo,SkipPackManage,SkipFine,SkipSensitivity };
struct SkipDraft : CustomDraft {
    TimeSkipSettings policy;
    bool readOnly=false,repeatCleared=false,packChecked=false,fineExpanded=false;
    int naturalHeight=0;
    HWND mode{},speed{},ramp{},quiet{},quietUnits{},ranges{},add{},edit{},remove{},repeat{},repeatUnits{};
    HWND packInfo{},packManage{},fine{},sensitivity{},sensitivityLabel{};
    HWND labels[8]{};
};
void skipPackInfo(SkipDraft& draft,bool refresh=false) {
    if(!skipPerson(skipMode(draft.mode)))return;
    if(!draft.readOnly && !app.active() && (refresh || !draft.packChecked)) {
        app.personPack=inspectPersonPack();app.personPackKnown=true;draft.packChecked=true;++app.skipRevision;
    }
    std::wstring text=L"Person detector: ";
    text+=!app.personPackKnown?L"not checked":app.personPack.state==PersonPackState::Ready?L"installed":
        app.personPack.state==PersonPackState::Missing?L"not installed":L"unavailable";
    if(!hasSource(Source::Camera))text+=L". Camera content is not selected; recording uses normal cadence.";
    else if(app.personPackKnown && app.personPack.state!=PersonPackState::Ready)text+=L". Recording uses normal cadence until available.";
    SetWindowTextW(draft.packInfo,text.c_str());
    EnableWindow(draft.packManage,!draft.readOnly && !app.active());
}
HWND skipChild(HWND window,const wchar_t* type,const wchar_t* value,DWORD style,int id) {
    return CreateWindowExW((std::wcscmp(type,L"EDIT")==0 || std::wcscmp(type,L"LISTBOX")==0)?WS_EX_CLIENTEDGE:0,
        type,value,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
}
void skipLayout(HWND window,SkipDraft& draft) {
    if(draft.layingOut)return;draft.layingOut=true;
    const auto mode=skipMode(draft.mode);
    const bool enabled=mode!=TimeSkipMode::Off,observed=skipObserved(mode),scheduled=skipScheduled(mode),person=skipPerson(mode),quiet=skipAutomatic(mode);
    const auto show=[](HWND h,bool yes){if(h)ShowWindow(h,yes?SW_SHOWNA:SW_HIDE);};
    for(HWND h:{draft.labels[1],draft.speed,draft.fine})show(h,enabled);
    for(HWND h:{draft.labels[3],draft.quiet,draft.quietUnits})show(h,observed);
    show(draft.labels[7],false);
    for(HWND h:{draft.labels[2],draft.ramp})show(h,enabled&&draft.fineExpanded);
    for(HWND h:{draft.sensitivityLabel,draft.sensitivity})show(h,quiet&&draft.fineExpanded);
    for(HWND h:{draft.labels[4],draft.ranges,draft.add,draft.edit,draft.remove,draft.labels[5],draft.repeat,draft.labels[6],draft.repeatUnits})show(h,scheduled);
    for(HWND h:{draft.packInfo,draft.packManage})show(h,person);
    RECT client{};GetClientRect(window,&client);const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    const int bw=GetSystemMetricsForDpi(SM_CXVSCROLL,draft.dpi),bh=GetSystemMetricsForDpi(SM_CYHSCROLL,draft.dpi);
    const int availableW=client.right+((style&WS_VSCROLL)?bw:0),availableH=client.bottom+((style&WS_HSCROLL)?bh:0);
    const int pad=draft.scale(18),gap=draft.scale(14),minimumWidth=draft.scale(360);
    auto wrap=[&](HWND h,int width,int minimum){wchar_t value[2048]{};GetWindowTextW(h,value,2048);RECT r{0,0,std::max(1,width),0};HDC dc=GetDC(window);auto old=SelectObject(dc,draft.font);DrawTextW(dc,value,-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,old);ReleaseDC(window,dc);return std::max(minimum,int(r.bottom));};
    int packHeight=0,helpHeight=0,errorHeight=0,bodyTop=0,packTop=0,scheduleTop=0,fineTop=0,tuneTop=0,helpTop=0,errorTop=0,buttonTop=0,minHeight=0;
    const int packButtonWidth=draft.scale(146);
    const auto measure=[&](int width){
        bodyTop=draft.scale(84);packTop=bodyTop+(enabled?draft.scale(68):0);
        packHeight=person?wrap(draft.packInfo,width-2*pad-gap-packButtonWidth,draft.scale(28)):0;
        scheduleTop=packTop+(person?packHeight+draft.scale(14):0);
        fineTop=scheduleTop+(scheduled?draft.scale(214):0);
        tuneTop=fineTop+(enabled?draft.scale(42):0);
        helpTop=tuneTop+(enabled&&draft.fineExpanded?draft.scale(62):0);
        helpHeight=wrap(draft.help,width-2*pad,draft.scale(32));
        errorTop=helpTop+helpHeight+draft.scale(10);errorHeight=wrap(draft.error,width-2*pad,draft.scale(20));
        buttonTop=errorTop+errorHeight+draft.scale(8);minHeight=buttonTop+draft.scale(44);
    };
    bool horizontal=false,vertical=false;
    for(int i=0;i<3;++i){horizontal=availableW-(vertical?bw:0)<minimumWidth;measure(std::max(minimumWidth,availableW-(vertical?bw:0)));vertical=availableH-(horizontal?bh:0)<minHeight;}
    ShowScrollBar(window,SB_HORZ,horizontal);ShowScrollBar(window,SB_VERT,vertical);GetClientRect(window,&client);
    const int width=std::max(minimumWidth,int(client.right));measure(width);const int height=std::max(minHeight,int(client.bottom));draft.naturalHeight=minHeight;
    if(width<=client.right)draft.wheelX=0;if(height<=client.bottom)draft.wheelY=0;
    draft.scrollX=std::clamp(draft.scrollX,0,width-int(std::max(1L,client.right)));draft.scrollY=std::clamp(draft.scrollY,0,height-int(std::max(1L,client.bottom)));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};info.nMax=width-1;info.nPage=std::max(1L,client.right);info.nPos=draft.scrollX;SetScrollInfo(window,SB_HORZ,&info,TRUE);
    info.nMax=height-1;info.nPage=std::max(1L,client.bottom);info.nPos=draft.scrollY;SetScrollInfo(window,SB_VERT,&info,TRUE);
    const int half=(width-2*pad-gap)/2,x2=pad+half+gap;
    auto move=[&](HWND h,int x,int y,int w,int heightPixels){MoveWindow(h,x-draft.scrollX,y-draft.scrollY,w,heightPixels,TRUE);};
    move(draft.labels[0],pad,draft.scale(16),width-2*pad,draft.scale(20));move(draft.mode,pad,draft.scale(38),width-2*pad,draft.scale(180));
    move(draft.labels[1],pad,bodyTop,half,draft.scale(20));move(draft.speed,pad,bodyTop+draft.scale(22),half,draft.scale(200));
    move(draft.labels[3],x2,bodyTop,half,draft.scale(20));move(draft.quiet,x2,bodyTop+draft.scale(22),half/2-draft.scale(4),draft.scale(28));move(draft.quietUnits,x2+half/2+draft.scale(4),bodyTop+draft.scale(22),half/2-draft.scale(4),draft.scale(160));
    move(draft.packInfo,pad,packTop,width-2*pad-gap-packButtonWidth,packHeight);move(draft.packManage,width-pad-packButtonWidth,packTop,packButtonWidth,draft.scale(28));
    move(draft.labels[4],pad,scheduleTop,width-2*pad,draft.scale(20));move(draft.ranges,pad,scheduleTop+draft.scale(22),width-2*pad,draft.scale(76));
    move(draft.add,pad,scheduleTop+draft.scale(104),draft.scale(80),draft.scale(28));move(draft.edit,pad+draft.scale(94),scheduleTop+draft.scale(104),draft.scale(80),draft.scale(28));move(draft.remove,pad+draft.scale(188),scheduleTop+draft.scale(104),draft.scale(92),draft.scale(28));
    move(draft.labels[5],pad,scheduleTop+draft.scale(146),half,draft.scale(20));move(draft.repeat,pad,scheduleTop+draft.scale(168),half,draft.scale(28));move(draft.labels[6],x2,scheduleTop+draft.scale(146),half,draft.scale(20));move(draft.repeatUnits,x2,scheduleTop+draft.scale(168),half,draft.scale(160));
    move(draft.fine,pad,fineTop,width-2*pad,draft.scale(28));
    move(draft.labels[2],pad,tuneTop,half,draft.scale(20));move(draft.ramp,pad,tuneTop+draft.scale(22),half,draft.scale(150));
    move(draft.sensitivityLabel,x2,tuneTop,half,draft.scale(20));move(draft.sensitivity,x2,tuneTop+draft.scale(22),half,draft.scale(150));
    move(draft.help,pad,helpTop,width-2*pad,helpHeight);move(draft.error,pad,errorTop,width-2*pad,errorHeight);move(draft.okay,width-pad-draft.scale(174),buttonTop,draft.scale(80),draft.scale(28));move(draft.cancel,width-pad-draft.scale(80),buttonTop,draft.scale(80),draft.scale(28));
    draft.layingOut=false;
}
void skipFitHeight(HWND window,SkipDraft& draft) {
    RECT rect{},client{};GetWindowRect(window,&rect);GetClientRect(window,&client);
    rect.bottom+=draft.naturalHeight-client.bottom;
    rect=fitWindow(rect,workArea(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST)));
    SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
    skipLayout(window,draft);
}
void skipReveal(HWND window,SkipDraft& draft,HWND child) {
    if(!child || !IsChild(window,child))return;RECT bounds{},client{};GetWindowRect(child,&bounds);
    MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
    if(bounds.left<0)draft.scrollX+=bounds.left;else if(bounds.right>client.right)draft.scrollX+=bounds.right-client.right;
    if(bounds.top<0)draft.scrollY+=bounds.top;else if(bounds.bottom>client.bottom)draft.scrollY+=bounds.bottom-client.bottom;skipLayout(window,draft);
}
void skipHelp(SkipDraft& draft) {
    const auto mode=skipMode(draft.mode);
    SetWindowTextW(draft.labels[3],skipPerson(mode)?L"No &person for":L"&Quiet for");
    std::wstring text=mode==TimeSkipMode::Off?L"Keeps your Capture every interval unchanged.":
        mode==TimeSkipMode::Manual?L"Speeds up inside your active-time ranges, even when something moves.":
        skipPerson(mode)?L"Detects people in the camera only, even when other things move. Checks can miss people; missing, uncertain or stale results keep normal speed.":
        L"Detects image changes in either selected source, not people. Visible activity returns to normal speed; brief or small changes can be missed. Unavailable or stale checks keep normal speed.";
    if(mode==TimeSkipMode::QuietWithinSchedule)text+=L" Speeds up only when quiet AND inside a range.";
    if(mode==TimeSkipMode::NoPersonWithinSchedule)text+=L" Speeds up only when no-person checks qualify AND inside a range.";
    if(skipObserved(mode))text+=L" Waiting time excludes pauses.";
    if(skipScheduled(mode))text+=L" Ranges exclude pauses and initial preparation; 0 repeat runs them once.";
    if(draft.fineExpanded && mode!=TimeSkipMode::Off){
        if(skipAutomatic(mode))text+=L"\n\nLow sensitivity ignores more small differences. High reacts to smaller differences but noisy sources may stay at normal speed. Checks are best effort, about once a second.";
        if(skipObserved(mode)){
            text+=skipPerson(mode)?L" Detected people return to normal speed promptly, interrupting the transition.":L" Detected image changes return to normal speed promptly, interrupting the transition.";
            text+=L" Checks pause with recording and continue outside scheduled ranges.";
        }
        text+=L"\n\nTransition rounds to whole saved video frames ("+std::to_wstring(app.settings.outputFps)+L" fps); real wait depends on Capture every. Short ranges may reach a lower speed. No intermediate pictures are generated. Both files share a cadence. Night blends retain their full duration, bounded by the base interval.";
    }
    if(draft.readOnly)text=L"Recording options are frozen for this session. "+text;
    if(draft.repeatCleared)text+=L" The retained repeat was reset to Never because an edited range exceeded it; your ranges are retained.";
    SetWindowTextW(draft.help,text.c_str());
    if(draft.fine){
        std::wstring caption=draft.fineExpanded?L"▾ &Fine tuning":L"▸ &Fine tuning";
        const int ramp=choice(draft.ramp),sensitivity=choice(draft.sensitivity);
        caption+=L" · "+std::wstring(ramp==0?L"0.5 s":ramp==2?L"2 s":ramp==1?L"1 s":L"invalid")+L" transition";
        if(skipAutomatic(mode))caption+=sensitivity==0?L" · Low sensitivity":sensitivity==2?L" · High sensitivity":sensitivity==1?L" · Standard":L" · invalid sensitivity";
        SetWindowTextW(draft.fine,caption.c_str());
    }
}
void skipList(SkipDraft& draft,int selected=-1) {
    SendMessageW(draft.ranges,LB_RESETCONTENT,0,0);
    for(unsigned i=0;i<draft.policy.rangeCount;++i){const auto& range=draft.policy.ranges[i];
        const auto text=formatDuration(int64_t(range.startSeconds)*1000)+L" → "+formatDuration(int64_t(range.endSeconds)*1000);
        SendMessageW(draft.ranges,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
    if(draft.policy.rangeCount)SendMessageW(draft.ranges,LB_SETCURSEL,std::clamp(selected,0,int(draft.policy.rangeCount)-1),0);
    const bool selection=SendMessageW(draft.ranges,LB_GETCURSEL,0,0)!=LB_ERR;
    EnableWindow(draft.add,!draft.readOnly && draft.policy.rangeCount<TimeSkipMaxRanges);
    EnableWindow(draft.edit,!draft.readOnly && selection);EnableWindow(draft.remove,!draft.readOnly && selection);
}
bool skipDuration(HWND edit,HWND units,int64_t minimum,int64_t& result,std::wstring& error) {
    wchar_t value[96]{};if(GetWindowTextLengthW(edit)>=96){error=L"Enter a shorter complete number.";return false;}GetWindowTextW(edit,value,96);
    return parseDuration(value,static_cast<DurationUnit>(std::clamp(choice(units),0,3)),minimum,int64_t(INT_MAX)*1000,1000,result,error);
}
bool skipClearInactiveRepeat(TimeSkipSettings& policy) {
    if(!skipScheduled(policy.mode) && policy.repeatSeconds>0)
        for(unsigned i=0;i<policy.rangeCount;++i)if(policy.ranges[i].endSeconds>policy.repeatSeconds){policy.repeatSeconds=0;return true;}
    return false;
}
bool validateSkip(SkipDraft& draft,TimeSkipSettings& output,std::wstring& error,HWND& invalid) {
    auto policy=draft.policy;policy.mode=skipMode(draft.mode);
    const int speed=choice(draft.speed);if(speed>=0 && speed<6)policy.multiplier=SkipMultipliers[speed];
    const int ramp=choice(draft.ramp),sensitivity=choice(draft.sensitivity);
    if(ramp>=0 && ramp<3)policy.rampFrames=SkipRamps[ramp];
    else if(policy.mode!=TimeSkipMode::Off){invalid=draft.ramp;error=L"Choose a transition duration in Fine tuning.";return false;}
    if(sensitivity>=0 && sensitivity<3)policy.quietSensitivity=static_cast<QuietSensitivity>(sensitivity);
    else if(skipAutomatic(policy.mode)){invalid=draft.sensitivity;error=L"Choose Low, Standard or High sensitivity in Fine tuning.";return false;}
    int64_t value=0;
    if(skipObserved(policy.mode)){invalid=draft.quiet;if(!skipDuration(draft.quiet,draft.quietUnits,1000,value,error))return false;policy.quietAfterMs=value;}
    if(skipScheduled(policy.mode)){invalid=draft.repeat;if(!skipDuration(draft.repeat,draft.repeatUnits,0,value,error))return false;policy.repeatSeconds=static_cast<int>(value/1000);invalid=draft.ranges;}
    skipClearInactiveRepeat(policy);
    if(!normalizeTimeSkipSettings(policy,error))return false;output=policy;return true;
}
void skipRange(HWND window,SkipDraft& draft,bool edit) {
    if(draft.readOnly || app.active())return;
    const int selected=static_cast<int>(SendMessageW(draft.ranges,LB_GETCURSEL,0,0));
    if((edit && (selected<0 || selected>=int(draft.policy.rangeCount))) || (!edit && draft.policy.rangeCount>=TimeSkipMaxRanges))return;
    CustomDraft range;range.kind=CustomKind::Range;
    if(edit){range.startSeconds=draft.policy.ranges[selected].startSeconds;range.endSeconds=draft.policy.ranges[selected].endSeconds;}
    else if(draft.policy.rangeCount){range.startSeconds=draft.policy.ranges[draft.policy.rangeCount-1].endSeconds;
        range.endSeconds=range.startSeconds<=INT_MAX-60?range.startSeconds+60:INT_MAX;}
    CustomTemplate resource;const auto outcome=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,window,customProc,reinterpret_cast<LPARAM>(&range));
    if(!IsWindow(window))return;
    if(outcome==IDOK && !app.active()){
        auto candidate=draft.policy;candidate.mode=TimeSkipMode::Off;candidate.repeatSeconds=0;
        candidate.ranges[edit?selected:candidate.rangeCount++]={range.startSeconds,range.endSeconds};
        std::wstring error;
        if(normalizeTimeSkipSettings(candidate,error)){
            draft.policy.ranges=candidate.ranges;draft.policy.rangeCount=candidate.rangeCount;skipList(draft,edit?selected:int(candidate.rangeCount)-1);
            SetWindowTextW(draft.error,L"Ranges sorted; touching or overlapping ranges merged.");
        } else SetWindowTextW(draft.error,error.c_str());
    } else if(outcome==-1)SetWindowTextW(draft.error,L"The range editor could not be opened. Try again.");
    if(!app.hiddenToTray){SetFocus(draft.ranges);skipReveal(window,draft,draft.ranges);}
}
INT_PTR CALLBACK skipProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* draft=reinterpret_cast<SkipDraft*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_INITDIALOG){
            draft=reinterpret_cast<SkipDraft*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);draft->previousDialog=app.customDialog;app.customDialog=window;
            draft->dpi=static_cast<int>(GetDpiForWindow(window));if(draft->dpi<=0)draft->dpi=app.dpi;SetWindowTextW(window,L"Time compression");
            const auto label=[&](int index,const wchar_t* text){return draft->labels[index]=skipChild(window,L"STATIC",text,0,5200+index);};
            const auto combo=[&](int id){return skipChild(window,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,id);};
            const auto button=[&](const wchar_t* text,int id){return skipChild(window,L"BUTTON",text,WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY,id);};
            label(0,L"Speed up &when");draft->mode=combo(SkipMode);for(auto value:SkipModeLabels)add(draft->mode,value);choose(draft->mode,static_cast<int>(draft->policy.mode));
            label(1,L"Ma&ximum extra speed");draft->speed=combo(SkipSpeed);for(int value:SkipMultipliers)add(draft->speed,std::to_wstring(value)+L"\u00d7");
            choose(draft->speed,1);
            bool foundSpeed=false;for(int i=0;i<6;++i)if(SkipMultipliers[i]==draft->policy.multiplier){choose(draft->speed,i);foundSpeed=true;}
            if(!foundSpeed){add(draft->speed,std::to_wstring(draft->policy.multiplier)+L"\u00d7");choose(draft->speed,6);}
            label(3,L"&Quiet for");draft->quiet=skipChild(window,L"EDIT",secondsInput(draft->policy.quietAfterMs).c_str(),WS_TABSTOP|ES_AUTOHSCROLL,SkipQuiet);
            label(7,L"Wait u&nits");draft->quietUnits=combo(SkipQuietUnits);
            draft->packInfo=skipChild(window,L"STATIC",L"",SS_NOPREFIX,SkipPackInfo);draft->packManage=button(L"Manage &detector...",SkipPackManage);
            label(4,L"Active-time &ranges (start \u2192 end, up to 16)");draft->ranges=skipChild(window,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL,SkipRanges);
            draft->add=button(L"&Add...",SkipAdd);draft->edit=button(L"&Edit...",SkipEdit);draft->remove=button(L"Re&move",SkipRemove);
            label(5,L"Repeat e&very (0 = never)");draft->repeat=skipChild(window,L"EDIT",std::to_wstring(draft->policy.repeatSeconds).c_str(),WS_TABSTOP|ES_AUTOHSCROLL,SkipRepeat);
            label(6,L"&Units");draft->repeatUnits=combo(SkipRepeatUnits);
            for(HWND box:{draft->quietUnits,draft->repeatUnits}){for(auto value:{L"Seconds",L"Minutes",L"Hours",L"Days"})add(box,value);choose(box,0);}
            const auto exactDuration=[&](HWND edit,HWND units,int64_t milliseconds){
                for(int unit=3;unit>=0;--unit){const auto divisor=durationUnitMs(static_cast<DurationUnit>(unit));
                    if(milliseconds>0 && milliseconds%divisor==0){SetWindowTextW(edit,std::to_wstring(milliseconds/divisor).c_str());choose(units,unit);break;}
                }
            };
            exactDuration(draft->quiet,draft->quietUnits,draft->policy.quietAfterMs);
            exactDuration(draft->repeat,draft->repeatUnits,int64_t(draft->policy.repeatSeconds)*1000);
            draft->fine=button(L"Fine tuning",SkipFine);
            label(2,L"&Transition (video time)");draft->ramp=combo(SkipRamp);for(int value:SkipRamps){const int frames=std::max(1,(value*app.settings.outputFps+DefaultOutputFps/2)/DefaultOutputFps);
                add(draft->ramp,secondsInput(int64_t(value)*1000/DefaultOutputFps)+L" s ("+std::to_wstring(frames)+L" frames)");}
            choose(draft->ramp,1);for(int i=0;i<3;++i)if(SkipRamps[i]==draft->policy.rampFrames)choose(draft->ramp,i);
            draft->sensitivityLabel=skipChild(window,L"STATIC",L"Change &sensitivity",0,5210);draft->sensitivity=combo(SkipSensitivity);
            for(auto value:{L"Low",L"Standard (default)",L"High"})add(draft->sensitivity,value);choose(draft->sensitivity,static_cast<int>(draft->policy.quietSensitivity));
            draft->help=skipChild(window,L"STATIC",L"",SS_NOPREFIX,SkipHelp);draft->error=skipChild(window,L"STATIC",L"",SS_NOPREFIX,SkipError);
            draft->okay=skipChild(window,L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON|BS_NOTIFY,IDOK);draft->cancel=button(draft->readOnly?L"Close":L"Cancel",IDCANCEL);
            bool okay=true;for(HWND child:{draft->mode,draft->speed,draft->ramp,draft->quiet,draft->quietUnits,draft->ranges,draft->add,draft->edit,draft->remove,draft->repeat,draft->repeatUnits,draft->packInfo,draft->packManage,draft->fine,draft->sensitivityLabel,draft->sensitivity,draft->help,draft->error,draft->okay,draft->cancel})if(!child)okay=false;
            for(HWND child:draft->labels)if(!child)okay=false;if(!okay){EndDialog(window,-1);return TRUE;}
            if(!dialogWheelCombos({draft->mode,draft->speed,draft->ramp,draft->quietUnits,draft->repeatUnits,draft->sensitivity})){EndDialog(window,-1);return TRUE;}
            for(HWND child:{draft->quiet,draft->repeat})SendMessageW(child,EM_SETLIMITTEXT,96,0);
            for(HWND child:{draft->mode,draft->speed,draft->ramp,draft->quiet,draft->quietUnits,draft->repeat,draft->repeatUnits,draft->packManage,draft->sensitivity,draft->okay})EnableWindow(child,!draft->readOnly);
            if(draft->readOnly){ShowWindow(draft->okay,SW_HIDE);SendMessageW(window,DM_SETDEFID,IDCANCEL,0);}
            skipList(*draft);skipPackInfo(*draft);skipHelp(*draft);customFont(window,*draft);
            RECT rect{0,0,draft->scale(540),draft->scale(skipScheduled(draft->policy.mode)?680:460)};
            AdjustWindowRectExForDpi(&rect,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),draft->dpi);
            RECT owner{};GetWindowRect(app.window,&owner);OffsetRect(&rect,(owner.left+owner.right-(rect.right-rect.left))/2-rect.left,(owner.top+owner.bottom-(rect.bottom-rect.top))/2-rect.top);
            rect=fitWindow(rect,workArea(MonitorFromWindow(app.window,MONITOR_DEFAULTTONEAREST)));SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
            skipLayout(window,*draft);skipFitHeight(window,*draft);SetFocus(draft->readOnly?draft->cancel:draft->mode);skipReveal(window,*draft,GetFocus());return FALSE;
        }
        if(!draft)return FALSE;
        switch(message){
        case WM_SIZE:skipLayout(window,*draft);return TRUE;
        case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:{
            const auto result=dialogWheel(window,*draft,message,wp);
            if(result==DialogWheel::Scrolled)skipLayout(window,*draft);
            if(result!=DialogWheel::Pass)return TRUE;break;}
        case WM_DPICHANGED:{draft->dpi=HIWORD(wp);customFont(window,*draft);RECT rect=*reinterpret_cast<RECT*>(lp);rect=fitWindow(rect,workArea(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);skipLayout(window,*draft);skipReveal(window,*draft,GetFocus());return TRUE;}
        case WM_HSCROLL:case WM_VSCROLL:{const int bar=message==WM_HSCROLL?SB_HORZ:SB_VERT;SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int position=info.nPos;
            switch(LOWORD(wp)){case SB_LINEUP:position-=draft->scale(24);break;case SB_LINEDOWN:position+=draft->scale(24);break;case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:position=info.nTrackPos;break;case SB_TOP:position=0;break;case SB_BOTTOM:position=info.nMax;break;default:return TRUE;}
            (bar==SB_HORZ?draft->scrollX:draft->scrollY)=position;skipLayout(window,*draft);return TRUE;}
        case WM_COMMAND:{const int id=LOWORD(wp),code=HIWORD(wp);
            if(((id==IDOK || id==IDCANCEL || id==SkipAdd || id==SkipEdit || id==SkipRemove || id==SkipPackManage || id==SkipFine) && code==BN_SETFOCUS) ||
               ((id==SkipQuiet || id==SkipRepeat) && code==EN_SETFOCUS) || (id==SkipRanges && code==LBN_SETFOCUS) ||
               ((id==SkipMode || id==SkipSpeed || id==SkipRamp || id==SkipQuietUnits || id==SkipRepeatUnits || id==SkipSensitivity) && code==CBN_SETFOCUS)){
                skipReveal(window,*draft,reinterpret_cast<HWND>(lp));return TRUE;
            }
            if(id==SkipFine && code==BN_CLICKED){draft->fineExpanded=!draft->fineExpanded;skipHelp(*draft);skipLayout(window,*draft);skipFitHeight(window,*draft);SetFocus(draft->fine);skipReveal(window,*draft,draft->fine);return TRUE;}
            if((id==SkipRamp || id==SkipSensitivity) && code==CBN_SELCHANGE){skipHelp(*draft);return TRUE;}
            if(id==IDCANCEL && code==BN_CLICKED){EndDialog(window,IDCANCEL);return TRUE;}
            if(id==SkipPackManage && code==BN_CLICKED){
                if(!draft->readOnly && !app.active() && skipPerson(skipMode(draft->mode))){
                    showPersonPackDialog(window);
                    if(IsWindow(window) && IsWindow(app.window)){skipPackInfo(*draft,true);skipLayout(window,*draft);skipFitHeight(window,*draft);skipReveal(window,*draft,draft->packManage);}
                }
                return TRUE;
            }
            if(id==IDOK && code==BN_CLICKED){if(draft->readOnly || app.active()){EndDialog(window,IDCANCEL);return TRUE;}TimeSkipSettings policy;std::wstring error;HWND invalid=draft->mode;
                if(validateSkip(*draft,policy,error,invalid)){draft->policy=policy;EndDialog(window,IDOK);return TRUE;}
                if(invalid==draft->ramp || invalid==draft->sensitivity){draft->fineExpanded=true;skipHelp(*draft);}
                SetWindowTextW(draft->error,error.c_str());skipLayout(window,*draft);skipFitHeight(window,*draft);SetFocus(invalid);skipReveal(window,*draft,invalid);return TRUE;}
            if((((id==SkipAdd || id==SkipEdit) && code==BN_CLICKED) || (id==SkipRanges && code==LBN_DBLCLK)) && !draft->readOnly){skipRange(window,*draft,id!=SkipAdd);return TRUE;}
            if(id==SkipRemove && code==BN_CLICKED && !draft->readOnly && !app.active()){const int selected=static_cast<int>(SendMessageW(draft->ranges,LB_GETCURSEL,0,0));
                if(selected>=0 && selected<int(draft->policy.rangeCount)){for(unsigned i=selected+1;i<draft->policy.rangeCount;++i)draft->policy.ranges[i-1]=draft->policy.ranges[i];--draft->policy.rangeCount;skipList(*draft,selected);}return TRUE;}
            if(id==SkipMode && HIWORD(wp)==CBN_SELCHANGE){
                auto policy=draft->policy;policy.mode=skipMode(draft->mode);
                if(!draft->readOnly && skipClearInactiveRepeat(policy)){draft->policy.repeatSeconds=0;draft->repeatCleared=true;SetWindowTextW(draft->repeat,L"0");choose(draft->repeatUnits,0);}
                skipPackInfo(*draft);skipHelp(*draft);SetWindowTextW(draft->error,L"");skipLayout(window,*draft);skipFitHeight(window,*draft);skipReveal(window,*draft,draft->mode);return TRUE;}
            if(id==SkipRanges && HIWORD(wp)==LBN_SELCHANGE){const bool selected=SendMessageW(draft->ranges,LB_GETCURSEL,0,0)!=LB_ERR;EnableWindow(draft->edit,!draft->readOnly && selected);EnableWindow(draft->remove,!draft->readOnly && selected);}
            return FALSE;}
        case WM_CTLCOLORSTATIC:if(reinterpret_cast<HWND>(lp)==draft->error){SetTextColor(reinterpret_cast<HDC>(wp),RGB(164,40,40));SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_BTNFACE));return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));}break;
        case WM_CLOSE:EndDialog(window,IDCANCEL);return TRUE;
        case WM_DESTROY:if(draft->font){DeleteObject(draft->font);draft->font=nullptr;}if(app.customDialog==window)app.customDialog=IsWindow(draft->previousDialog)?draft->previousDialog:nullptr;return TRUE;
        }
    } catch(...){EndDialog(window,-1);return TRUE;}return FALSE;
}
void editSkip() {
    if(app.customDialog)return;SkipDraft draft;draft.policy=app.settings.timeSkip;draft.readOnly=app.active();CustomTemplate resource;
    const int priorRevision=app.skipRevision;
    const auto outcome=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,app.window,skipProc,reinterpret_cast<LPARAM>(&draft));
    if(!IsWindow(app.window))return;
    if(outcome==IDOK && !draft.readOnly && !app.active()){app.settings.timeSkip=draft.policy;++app.skipRevision;configure();updateControls();layout();}
    else if(priorRevision!=app.skipRevision)updateControls();
    if(outcome==-1)MessageBoxW(app.window,L"Time compression settings could not be opened. Try again.",L"Timelapse",MB_OK|MB_ICONERROR);
    if(!app.closeWhenDone && !app.hiddenToTray){SetFocus(app.skipConfigure);revealFocusedControl();}
}
enum WatermarkId { MarkEnabled=5301,MarkTime,MarkSpeed,MarkTimeKind,MarkPosition,MarkX,MarkY,MarkSize,MarkPreview };
struct WatermarkDraft : CustomDraft {
    WatermarkSettings policy;
    WatermarkRenderer renderer;
    Frame illustration;
    bool readOnly=false,ready=false;
    int naturalHeight=0;
    HWND enabled{},time{},speed{},timeKind{},position{},x{},y{},size{},preview{},previewLabel{},labels[5]{};
};
int watermarkPosition(const WatermarkSettings& value) {
    if(value.x==0 && value.y==0)return 0;
    if(value.x==10000 && value.y==0)return 1;
    if(value.x==0 && value.y==10000)return 2;
    if(value.x==10000 && value.y==10000)return 3;
    return 4;
}
bool watermarkDraftSettings(WatermarkDraft& draft,WatermarkSettings& value,std::wstring& error,HWND& invalid) {
    value=draft.policy;value.enabled=SendMessageW(draft.enabled,BM_GETCHECK,0,0)==BST_CHECKED;invalid=draft.enabled;
    // Off remains committable after invalid inactive edits without inventing values.
    if(!value.enabled)return validateWatermarkSettings(value,error);
    value.showTime=SendMessageW(draft.time,BM_GETCHECK,0,0)==BST_CHECKED;
    value.showSpeed=SendMessageW(draft.speed,BM_GETCHECK,0,0)==BST_CHECKED;
    if(!value.showTime && !value.showSpeed){invalid=draft.time;error=L"Choose Show time or Show target speed, or turn off the watermark.";return false;}
    const int kind=choice(draft.timeKind),size=choice(draft.size),position=choice(draft.position);
    if(kind>=0 && kind<2)value.timeKind=static_cast<WatermarkTimeKind>(kind);
    else if(value.showTime){invalid=draft.timeKind;error=L"Choose a time value.";return false;}
    if(size<0 || size>2){invalid=draft.size;error=L"Choose a text size.";return false;}
    value.textSize=static_cast<WatermarkTextSize>(size);
    if(position<0 || position>4){invalid=draft.position;error=L"Choose a position.";return false;}
    if(position<4){value.x=position%2?10000:0;value.y=position>=2?10000:0;}
    else {
        const auto coordinate=[&](HWND edit,int& output){wchar_t text[96]{};invalid=edit;
            if(GetWindowTextLengthW(edit)>=96){error=L"Enter a complete position from 0 to 100, with at most two decimal places.";return false;}
            GetWindowTextW(edit,text,96);int64_t parsed=0;std::wstring detail;
            if(!parseDuration(text,DurationUnit::Seconds,0,100000,10,parsed,detail)){error=L"Position must be 0 to 100 percent, with at most two decimal places.";return false;}
            output=static_cast<int>(parsed/10);return true;
        };
        if(!coordinate(draft.x,value.x) || !coordinate(draft.y,value.y))return false;
    }
    invalid=draft.size;return validateWatermarkSettings(value,error);
}
void watermarkLayout(HWND window,WatermarkDraft& draft) {
    if(draft.layingOut)return;draft.layingOut=true;
    const bool enabled=SendMessageW(draft.enabled,BM_GETCHECK,0,0)==BST_CHECKED,custom=enabled && choice(draft.position)==4;
    const auto show=[](HWND child,bool visible){ShowWindow(child,visible?SW_SHOWNA:SW_HIDE);};
    for(HWND child:{draft.time,draft.speed,draft.labels[0],draft.timeKind,draft.labels[1],draft.position,draft.labels[4],draft.size})show(child,enabled);
    for(HWND child:{draft.labels[2],draft.x,draft.labels[3],draft.y})show(child,custom);
    for(HWND child:{draft.enabled,draft.time,draft.speed,draft.position,draft.x,draft.y,draft.size})EnableWindow(child,!draft.readOnly);
    EnableWindow(draft.timeKind,!draft.readOnly && SendMessageW(draft.time,BM_GETCHECK,0,0)==BST_CHECKED);
    RECT client{};GetClientRect(window,&client);const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    const int bw=GetSystemMetricsForDpi(SM_CXVSCROLL,draft.dpi),bh=GetSystemMetricsForDpi(SM_CYHSCROLL,draft.dpi);
    const int availableW=client.right+((style&WS_VSCROLL)?bw:0),availableH=client.bottom+((style&WS_HSCROLL)?bh:0);
    const int pad=draft.scale(18),gap=draft.scale(14),minimumWidth=draft.scale(360);
    const auto wrap=[&](HWND child,int width,int minimum){wchar_t value[1024]{};GetWindowTextW(child,value,1024);RECT rect{0,0,std::max(1,width),0};
        HDC dc=GetDC(window);auto old=SelectObject(dc,draft.font);DrawTextW(dc,value,-1,&rect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,old);ReleaseDC(window,dc);return std::max(minimum,int(rect.bottom));};
    const int previewTop=draft.scale(enabled?(custom?264:202):58),previewHeight=draft.scale(152);
    int helpTop=previewTop+previewHeight+draft.scale(30),helpHeight=0,errorTop=0,errorHeight=0,buttonTop=0,minHeight=0;
    const auto measure=[&](int width){helpHeight=wrap(draft.help,width-2*pad,draft.scale(36));errorTop=helpTop+helpHeight+draft.scale(10);
        errorHeight=wrap(draft.error,width-2*pad,draft.scale(20));buttonTop=errorTop+errorHeight+draft.scale(8);minHeight=buttonTop+draft.scale(44);};
    bool horizontal=false,vertical=false;
    for(int i=0;i<3;++i){horizontal=availableW-(vertical?bw:0)<minimumWidth;measure(std::max(minimumWidth,availableW-(vertical?bw:0)));vertical=availableH-(horizontal?bh:0)<minHeight;}
    ShowScrollBar(window,SB_HORZ,horizontal);ShowScrollBar(window,SB_VERT,vertical);GetClientRect(window,&client);
    const int width=std::max(minimumWidth,int(client.right));measure(width);const int height=std::max(minHeight,int(client.bottom));draft.naturalHeight=minHeight;
    if(width<=client.right)draft.wheelX=0;if(height<=client.bottom)draft.wheelY=0;
    draft.scrollX=std::clamp(draft.scrollX,0,width-int(std::max(1L,client.right)));draft.scrollY=std::clamp(draft.scrollY,0,height-int(std::max(1L,client.bottom)));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};info.nMax=width-1;info.nPage=std::max(1L,client.right);info.nPos=draft.scrollX;SetScrollInfo(window,SB_HORZ,&info,TRUE);
    info.nMax=height-1;info.nPage=std::max(1L,client.bottom);info.nPos=draft.scrollY;SetScrollInfo(window,SB_VERT,&info,TRUE);
    const int half=(width-2*pad-gap)/2,x2=pad+half+gap;
    const auto move=[&](HWND child,int x,int y,int w,int h){MoveWindow(child,x-draft.scrollX,y-draft.scrollY,w,h,TRUE);};
    move(draft.enabled,pad,draft.scale(16),width-2*pad,draft.scale(28));
    move(draft.time,pad,draft.scale(56),half,draft.scale(28));move(draft.speed,x2,draft.scale(56),half,draft.scale(28));
    move(draft.labels[0],pad,draft.scale(92),width-2*pad,draft.scale(20));move(draft.timeKind,pad,draft.scale(114),width-2*pad,draft.scale(140));
    move(draft.labels[1],pad,draft.scale(150),half,draft.scale(20));move(draft.position,pad,draft.scale(172),half,draft.scale(190));
    move(draft.labels[4],x2,draft.scale(150),half,draft.scale(20));move(draft.size,x2,draft.scale(172),half,draft.scale(140));
    move(draft.labels[2],pad,draft.scale(212),half,draft.scale(20));move(draft.x,pad,draft.scale(234),half,draft.scale(28));
    move(draft.labels[3],x2,draft.scale(212),half,draft.scale(20));move(draft.y,x2,draft.scale(234),half,draft.scale(28));
    move(draft.preview,pad,previewTop,width-2*pad,previewHeight);move(draft.previewLabel,pad,previewTop+previewHeight+draft.scale(4),width-2*pad,draft.scale(20));
    move(draft.help,pad,helpTop,width-2*pad,helpHeight);move(draft.error,pad,errorTop,width-2*pad,errorHeight);
    move(draft.okay,width-pad-draft.scale(174),buttonTop,draft.scale(80),draft.scale(28));move(draft.cancel,width-pad-draft.scale(80),buttonTop,draft.scale(80),draft.scale(28));
    draft.layingOut=false;
}
void watermarkFitHeight(HWND window,WatermarkDraft& draft) {
    RECT rect{},client{};GetWindowRect(window,&rect);GetClientRect(window,&client);rect.bottom+=draft.naturalHeight-client.bottom;
    rect=fitWindow(rect,workArea(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST)));
    SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);watermarkLayout(window,draft);
}
void watermarkReveal(HWND window,WatermarkDraft& draft,HWND child) {
    if(!child || !IsChild(window,child))return;RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
    if(bounds.left<0)draft.scrollX+=bounds.left;else if(bounds.right>client.right)draft.scrollX+=bounds.right-client.right;
    if(bounds.top<0)draft.scrollY+=bounds.top;else if(bounds.bottom>client.bottom)draft.scrollY+=bounds.bottom-client.bottom;watermarkLayout(window,draft);
}
void watermarkIllustration(WatermarkDraft& draft) {
    WatermarkSettings value;std::wstring error;HWND invalid{};const bool valid=watermarkDraftSettings(draft,value,error,invalid);
    const auto dimensions=previewDimensions(app.settings.width,app.settings.height);
    draft.illustration.width=dimensions.first;draft.illustration.height=dimensions.second;
    draft.illustration.pixels.resize(size_t(dimensions.first)*dimensions.second*4);
    for(int y=0;y<dimensions.second;++y)for(int x=0;x<dimensions.first;++x){auto* p=draft.illustration.pixels.data()+(size_t(y)*dimensions.first+x)*4;
        p[0]=uint8_t(70+60*x/dimensions.first);p[1]=uint8_t(75+50*y/dimensions.second);p[2]=uint8_t(35+35*x/dimensions.first);p[3]=255;}
    if(valid && draft.renderer.prepare(value,app.settings.width,app.settings.height,error)){
        WatermarkContext context;context.activeMs=192000;context.recordedLocal={2026,9,3,30,12,34,56,0};context.targetIntervalMs=app.settings.intervalMs;context.outputFps=app.settings.outputFps;
        draft.renderer.apply(draft.illustration,context,error);
    }
    SetWindowTextW(draft.error,error.c_str());InvalidateRect(draft.preview,nullptr,FALSE);
}
void watermarkPaint(const DRAWITEMSTRUCT& draw,const WatermarkDraft& draft) {
    FillRect(draw.hDC,&draw.rcItem,GetSysColorBrush(COLOR_3DDKSHADOW));const auto& frame=draft.illustration;
    if(frame.width<=0 || frame.height<=0 || frame.pixels.empty())return;
    const int width=draw.rcItem.right-draw.rcItem.left,height=draw.rcItem.bottom-draw.rcItem.top;
    const double scale=std::min(double(width)/frame.width,double(height)/frame.height);
    const int w=std::max(1,int(frame.width*scale)),h=std::max(1,int(frame.height*scale));
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=frame.width;info.bmiHeader.biHeight=-frame.height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    const int old=SetStretchBltMode(draw.hDC,HALFTONE);POINT origin{};SetBrushOrgEx(draw.hDC,0,0,&origin);
    StretchDIBits(draw.hDC,draw.rcItem.left+(width-w)/2,draw.rcItem.top+(height-h)/2,w,h,0,0,frame.width,frame.height,frame.pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
    SetBrushOrgEx(draw.hDC,origin.x,origin.y,nullptr);SetStretchBltMode(draw.hDC,old);
}
INT_PTR CALLBACK watermarkProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* draft=reinterpret_cast<WatermarkDraft*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_INITDIALOG){
            draft=reinterpret_cast<WatermarkDraft*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);draft->previousDialog=app.customDialog;app.customDialog=window;
            draft->dpi=static_cast<int>(GetDpiForWindow(window));if(draft->dpi<=0)draft->dpi=app.dpi;SetWindowTextW(window,L"Watermark");
            const auto checkbox=[&](const wchar_t* text,int id,bool checked){HWND child=skipChild(window,L"BUTTON",text,WS_TABSTOP|BS_AUTOCHECKBOX|BS_NOTIFY,id);SendMessageW(child,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);return child;};
            const auto combo=[&](int id){return skipChild(window,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,id);};
            const auto label=[&](int i,const wchar_t* text){return draft->labels[i]=skipChild(window,L"STATIC",text,0,5400+i);};
            draft->enabled=checkbox(L"&Enable watermark",MarkEnabled,draft->policy.enabled);
            draft->time=checkbox(L"Show &time",MarkTime,draft->policy.showTime);draft->speed=checkbox(L"Show target &speed",MarkSpeed,draft->policy.showSpeed);
            label(0,L"Time &value");draft->timeKind=combo(MarkTimeKind);for(auto value:{L"Active elapsed (excludes pauses)",L"Recorded local date/time"})add(draft->timeKind,value);choose(draft->timeKind,static_cast<int>(draft->policy.timeKind));
            label(1,L"&Position");draft->position=combo(MarkPosition);for(auto value:{L"Top left",L"Top right",L"Bottom left",L"Bottom right",L"Custom"})add(draft->position,value);choose(draft->position,watermarkPosition(draft->policy));
            label(4,L"Text si&ze");draft->size=combo(MarkSize);for(auto value:{L"Small",L"Medium (default)",L"Large"})add(draft->size,value);choose(draft->size,static_cast<int>(draft->policy.textSize));
            label(2,L"&X position (%)");draft->x=skipChild(window,L"EDIT",secondsInput(int64_t(draft->policy.x)*10).c_str(),WS_TABSTOP|ES_AUTOHSCROLL,MarkX);
            label(3,L"&Y position (%)");draft->y=skipChild(window,L"EDIT",secondsInput(int64_t(draft->policy.y)*10).c_str(),WS_TABSTOP|ES_AUTOHSCROLL,MarkY);
            for(HWND edit:{draft->x,draft->y})SendMessageW(edit,EM_SETLIMITTEXT,96,0);
            draft->preview=skipChild(window,L"STATIC",L"Watermark placement illustration",SS_OWNERDRAW,MarkPreview);
            draft->previewLabel=skipChild(window,L"STATIC",L"Illustration only; values follow your recording.",SS_NOPREFIX,5410);
            draft->help=skipChild(window,L"STATIC",L"",SS_NOPREFIX,5411);draft->error=skipChild(window,L"STATIC",L"",SS_NOPREFIX,5412);
            draft->okay=skipChild(window,L"BUTTON",L"&Apply",WS_TABSTOP|BS_DEFPUSHBUTTON|BS_NOTIFY,IDOK);
            draft->cancel=skipChild(window,L"BUTTON",draft->readOnly?L"Close":L"Cancel",WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY,IDCANCEL);
            for(HWND child:{draft->enabled,draft->time,draft->speed,draft->timeKind,draft->position,draft->x,draft->y,draft->size,draft->preview,draft->previewLabel,draft->help,draft->error,draft->okay,draft->cancel})if(!child){EndDialog(window,-1);return TRUE;}
            for(HWND child:draft->labels)if(!child){EndDialog(window,-1);return TRUE;}
            if(!dialogWheelCombos({draft->timeKind,draft->position,draft->size})){EndDialog(window,-1);return TRUE;}
            std::wstring help=draft->readOnly?L"Options are frozen for this recording. ":L"";
            help+=L"Target speed is total planned playback acceleration, not extra compression or achieved speed. Recorded time is the local clock when a frame is accepted for saving; it may differ from camera exposure time. Clock/time-zone changes can affect it.\n\nBoth files use the same watermark. X/Y place the whole box inside the video; 0% is left/top, 100% right/bottom. Text scales with video size and may increase file size.";
            SetWindowTextW(draft->help,help.c_str());if(draft->readOnly){ShowWindow(draft->okay,SW_HIDE);SendMessageW(window,DM_SETDEFID,IDCANCEL,0);}
            customFont(window,*draft);draft->ready=true;watermarkIllustration(*draft);
            RECT rect{0,0,draft->scale(540),draft->scale(540)};AdjustWindowRectExForDpi(&rect,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),draft->dpi);
            RECT owner{};GetWindowRect(app.window,&owner);OffsetRect(&rect,(owner.left+owner.right-(rect.right-rect.left))/2-rect.left,(owner.top+owner.bottom-(rect.bottom-rect.top))/2-rect.top);
            rect=fitWindow(rect,workArea(MonitorFromWindow(app.window,MONITOR_DEFAULTTONEAREST)));SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
            watermarkLayout(window,*draft);watermarkFitHeight(window,*draft);SetFocus(draft->readOnly?draft->cancel:draft->enabled);watermarkReveal(window,*draft,GetFocus());return FALSE;
        }
        if(draft && message==WM_DESTROY){draft->renderer.reset();if(draft->font){DeleteObject(draft->font);draft->font=nullptr;}if(app.customDialog==window)app.customDialog=IsWindow(draft->previousDialog)?draft->previousDialog:nullptr;return TRUE;}
        if(!draft || !draft->ready)return FALSE;
        switch(message){
        case WM_SIZE:watermarkLayout(window,*draft);return TRUE;
        case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:{
            const auto result=dialogWheel(window,*draft,message,wp);
            if(result==DialogWheel::Scrolled)watermarkLayout(window,*draft);
            if(result!=DialogWheel::Pass)return TRUE;break;}
        case WM_DPICHANGED:{draft->dpi=HIWORD(wp);customFont(window,*draft);RECT rect=*reinterpret_cast<RECT*>(lp);rect=fitWindow(rect,workArea(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);watermarkLayout(window,*draft);watermarkReveal(window,*draft,GetFocus());return TRUE;}
        case WM_DRAWITEM:if(wp==MarkPreview){watermarkPaint(*reinterpret_cast<DRAWITEMSTRUCT*>(lp),*draft);return TRUE;}break;
        case WM_HSCROLL:case WM_VSCROLL:{const int bar=message==WM_HSCROLL?SB_HORZ:SB_VERT;SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int position=info.nPos;
            switch(LOWORD(wp)){case SB_LINEUP:position-=draft->scale(24);break;case SB_LINEDOWN:position+=draft->scale(24);break;case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:position=info.nTrackPos;break;case SB_TOP:position=0;break;case SB_BOTTOM:position=info.nMax;break;default:return TRUE;}
            (bar==SB_HORZ?draft->scrollX:draft->scrollY)=position;watermarkLayout(window,*draft);return TRUE;}
        case WM_COMMAND:{const int id=LOWORD(wp),code=HIWORD(wp);
            if(((id==IDOK || id==IDCANCEL || id==MarkEnabled || id==MarkTime || id==MarkSpeed) && code==BN_SETFOCUS) ||
               ((id==MarkX || id==MarkY) && code==EN_SETFOCUS) || ((id==MarkTimeKind || id==MarkPosition || id==MarkSize) && code==CBN_SETFOCUS)){
                watermarkReveal(window,*draft,reinterpret_cast<HWND>(lp));return TRUE;
            }
            if(id==IDCANCEL && code==BN_CLICKED){EndDialog(window,IDCANCEL);return TRUE;}
            if(id==IDOK && code==BN_CLICKED){if(draft->readOnly || app.active()){EndDialog(window,IDCANCEL);return TRUE;}WatermarkSettings value;std::wstring error;HWND invalid{};
                if(watermarkDraftSettings(*draft,value,error,invalid) && draft->renderer.prepare(value,app.settings.width,app.settings.height,error)){draft->policy=value;EndDialog(window,IDOK);return TRUE;}
                SetWindowTextW(draft->error,error.c_str());watermarkLayout(window,*draft);watermarkFitHeight(window,*draft);SetFocus(invalid);watermarkReveal(window,*draft,invalid);return TRUE;}
            const bool changed=((id==MarkEnabled || id==MarkTime || id==MarkSpeed) && code==BN_CLICKED) ||
                ((id==MarkTimeKind || id==MarkPosition || id==MarkSize) && code==CBN_SELCHANGE) || ((id==MarkX || id==MarkY) && code==EN_CHANGE);
            if(changed && !draft->readOnly && !app.active()){watermarkIllustration(*draft);watermarkLayout(window,*draft);watermarkFitHeight(window,*draft);watermarkReveal(window,*draft,reinterpret_cast<HWND>(lp));return TRUE;}
            return FALSE;}
        case WM_CTLCOLORSTATIC:if(reinterpret_cast<HWND>(lp)==draft->error){SetTextColor(reinterpret_cast<HDC>(wp),RGB(164,40,40));SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_BTNFACE));return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));}break;
        case WM_CLOSE:EndDialog(window,IDCANCEL);return TRUE;
        }
    } catch(...){EndDialog(window,-1);return TRUE;}return FALSE;
}
void editWatermark() {
    if(app.customDialog)return;WatermarkDraft draft;draft.policy=app.settings.watermark;draft.readOnly=app.active();CustomTemplate resource;
    const auto outcome=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,app.window,watermarkProc,reinterpret_cast<LPARAM>(&draft));
    if(!IsWindow(app.window))return;
    if(outcome==IDOK && !draft.readOnly && !app.active()){app.settings.watermark=draft.policy;app.watermarkCheckValid=false;++app.watermarkRevision;configure();updateControls();layout();}
    if(outcome==-1)MessageBoxW(app.window,L"Watermark settings could not be opened. Try again.",L"Timelapse",MB_OK|MB_ICONERROR);
    if(!app.closeWhenDone && !app.hiddenToTray){SetFocus(app.watermarkConfigure);revealFocusedControl();}
}

enum PlaybackId { PlaybackFps=5501,PlaybackPause,PlaybackStop,PlaybackClearPause,PlaybackClearStop };
struct PlaybackDraft : CustomDraft {
    int fps=DefaultOutputFps,naturalHeight=0;
    uint16_t pauseHotkey=0,stopHotkey=0;
    HWND fpsHint{},pauseLabel{},stopLabel{},stop{},clearPause{},clearStop{};
    bool readOnly=false;
};
LRESULT CALLBACK playbackFocusProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    const auto result=DefSubclassProc(window,message,wp,lp);
    if(message==WM_SETFOCUS)SendMessageW(GetParent(window),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(window),EN_SETFOCUS),reinterpret_cast<LPARAM>(window));
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,playbackFocusProc,id);
    return result;
}
void playbackLayout(HWND window,PlaybackDraft& draft) {
    if(draft.layingOut)return;draft.layingOut=true;
    RECT client{};GetClientRect(window,&client);const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    const int bw=GetSystemMetricsForDpi(SM_CXVSCROLL,draft.dpi),bh=GetSystemMetricsForDpi(SM_CYHSCROLL,draft.dpi);
    const int availableW=client.right+((style&WS_VSCROLL)?bw:0),availableH=client.bottom+((style&WS_HSCROLL)?bh:0),pad=draft.scale(18),gap=draft.scale(14);
    const auto wrap=[&](HWND child,int width,int minimum){
        wchar_t value[1024]{};GetWindowTextW(child,value,1024);RECT bounds{0,0,std::max(1,width),0};HDC dc=GetDC(window);
        if(!dc)return minimum;const auto previous=SelectObject(dc,draft.font?draft.font:GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc,value,-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,previous);ReleaseDC(window,dc);return std::max(minimum,int(bounds.bottom));
    };
    int fpsHintHeight=0,pauseTop=0,stopTop=0,helpTop=0,helpHeight=0,errorTop=0,errorHeight=0,buttonTop=0;
    const auto measure=[&](int width){
        fpsHintHeight=wrap(draft.fpsHint,width-2*pad,draft.scale(36));pauseTop=draft.scale(80)+fpsHintHeight+draft.scale(14);stopTop=pauseTop+draft.scale(66);
        helpTop=stopTop+draft.scale(66);helpHeight=wrap(draft.help,width-2*pad,draft.scale(56));errorTop=helpTop+helpHeight+draft.scale(8);
        errorHeight=wrap(draft.error,width-2*pad,draft.scale(36));buttonTop=errorTop+errorHeight+draft.scale(10);draft.naturalHeight=buttonTop+draft.scale(44);
    };
    bool horizontal=false,vertical=false;
    for(int i=0;i<3;++i){horizontal=availableW-(vertical?bw:0)<draft.scale(360);measure(std::max(draft.scale(360),availableW-(vertical?bw:0)));vertical=availableH-(horizontal?bh:0)<draft.naturalHeight;}
    ShowScrollBar(window,SB_HORZ,horizontal);ShowScrollBar(window,SB_VERT,vertical);GetClientRect(window,&client);
    const int width=std::max(draft.scale(360),int(client.right));measure(width);const int height=std::max(draft.naturalHeight,int(client.bottom));
    if(width<=client.right)draft.wheelX=0;if(height<=client.bottom)draft.wheelY=0;
    draft.scrollX=std::clamp(draft.scrollX,0,width-int(std::max(1L,client.right)));draft.scrollY=std::clamp(draft.scrollY,0,height-int(std::max(1L,client.bottom)));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};info.nMax=width-1;info.nPage=std::max(1L,client.right);info.nPos=draft.scrollX;SetScrollInfo(window,SB_HORZ,&info,TRUE);
    info.nMax=height-1;info.nPage=std::max(1L,client.bottom);info.nPos=draft.scrollY;SetScrollInfo(window,SB_VERT,&info,TRUE);
    const auto move=[&](HWND child,int x,int y,int w,int h){MoveWindow(child,x-draft.scrollX,y-draft.scrollY,w,h,TRUE);};
    const int clearWidth=draft.scale(70),fieldWidth=width-2*pad-clearWidth-gap;
    move(draft.firstLabel,pad,draft.scale(16),width-2*pad,draft.scale(20));move(draft.first,pad,draft.scale(38),draft.scale(130),draft.scale(180));
    move(draft.fpsHint,pad,draft.scale(80),width-2*pad,fpsHintHeight);
    move(draft.pauseLabel,pad,pauseTop,width-2*pad,draft.scale(20));move(draft.second,pad,pauseTop+draft.scale(22),fieldWidth,draft.scale(28));move(draft.clearPause,width-pad-clearWidth,pauseTop+draft.scale(22),clearWidth,draft.scale(28));
    move(draft.stopLabel,pad,stopTop,width-2*pad,draft.scale(20));move(draft.stop,pad,stopTop+draft.scale(22),fieldWidth,draft.scale(28));move(draft.clearStop,width-pad-clearWidth,stopTop+draft.scale(22),clearWidth,draft.scale(28));
    move(draft.help,pad,helpTop,width-2*pad,helpHeight);move(draft.error,pad,errorTop,width-2*pad,errorHeight);
    move(draft.okay,width-pad-draft.scale(174),buttonTop,draft.scale(80),draft.scale(28));move(draft.cancel,width-pad-draft.scale(80),buttonTop,draft.scale(80),draft.scale(28));draft.layingOut=false;
}
void playbackReveal(HWND window,PlaybackDraft& draft,HWND child) {
    if(!child || !IsChild(window,child))return;RECT bounds{},client{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(window,&client);
    if(bounds.left<0)draft.scrollX+=bounds.left;else if(bounds.right>client.right)draft.scrollX+=bounds.right-client.right;
    if(bounds.top<0)draft.scrollY+=bounds.top;else if(bounds.bottom>client.bottom)draft.scrollY+=bounds.bottom-client.bottom;playbackLayout(window,draft);
}
void playbackFitHeight(HWND window,PlaybackDraft& draft) {
    RECT bounds{},client{};GetWindowRect(window,&bounds);GetClientRect(window,&client);bounds.bottom+=draft.naturalHeight-client.bottom;
    bounds=fitWindow(bounds,workArea(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST)));
    SetWindowPos(window,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);playbackLayout(window,draft);
}
bool validatePlaybackDraft(PlaybackDraft& draft,std::wstring& error,HWND& invalid) {
    invalid=draft.first;wchar_t fps[96]{};
    if(GetWindowTextLengthW(draft.first)>=96){error=L"Enter a shorter complete whole number of frames per second.";return false;}
    GetWindowTextW(draft.first,fps,96);int parsed=DefaultOutputFps;
    if(!parseOutputFps(fps,parsed,error))return false;
    const auto pause=static_cast<uint16_t>(SendMessageW(draft.second,HKM_GETHOTKEY,0,0)),stop=static_cast<uint16_t>(SendMessageW(draft.stop,HKM_GETHOTKEY,0,0));
    invalid=!validHotkey(pause)?draft.second:draft.stop;
    if(!validateHotkeys(pause,stop,error))return false;
    if(!setRecordingHotkeys(pause,stop,error)){invalid=error.find(L"Pause / resume")==0?draft.second:draft.stop;return false;}
    draft.fps=parsed;draft.pauseHotkey=pause;draft.stopHotkey=stop;return true;
}
INT_PTR CALLBACK playbackProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* draft=reinterpret_cast<PlaybackDraft*>(GetWindowLongPtrW(window,DWLP_USER));
    try {
        if(message==WM_INITDIALOG){
            draft=reinterpret_cast<PlaybackDraft*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);draft->previousDialog=app.customDialog;app.customDialog=window;
            draft->dpi=static_cast<int>(GetDpiForWindow(window));if(draft->dpi<=0)draft->dpi=app.dpi;SetWindowTextW(window,L"Playback & shortcuts");
            const auto child=[&](const wchar_t* cls,const wchar_t* caption,DWORD style,int id){return CreateWindowExW(std::wcscmp(cls,HOTKEY_CLASSW)==0?WS_EX_CLIENTEDGE:0,cls,caption,
                WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);};
            draft->firstLabel=child(L"STATIC",L"Final video &frame rate (fps)",0,0);
            draft->first=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWN|WS_VSCROLL,PlaybackFps);
            for(int fps:{24,25,30,60,120})add(draft->first,std::to_wstring(fps));SetWindowTextW(draft->first,std::to_wstring(draft->fps).c_str());SendMessageW(draft->first,CB_LIMITTEXT,96,0);
            draft->fpsHint=child(L"STATIC",L"Choose a whole number from 1 to 120. Higher fps plays the captured frames faster and makes a shorter video. Capture every stays unchanged.",SS_NOPREFIX,0);
            draft->pauseLabel=child(L"STATIC",L"Global &pause / resume",0,0);draft->second=child(HOTKEY_CLASSW,L"",WS_TABSTOP,PlaybackPause);draft->clearPause=child(L"BUTTON",L"C&lear",WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY,PlaybackClearPause);
            draft->stopLabel=child(L"STATIC",L"Global &stop and save",0,0);draft->stop=child(HOTKEY_CLASSW,L"",WS_TABSTOP,PlaybackStop);draft->clearStop=child(L"BUTTON",L"Cl&ear",WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY,PlaybackClearStop);
            draft->help=child(L"STATIC",draft->readOnly?L"These settings are frozen for the current recording. Shortcuts work while minimized, and are suspended while a Timelapse dialog or menu is open. Stop saves the recording; during preparation it cancels the start.":
                L"Press a combination with Ctrl or Alt in a shortcut field. Alt with a letter also needs Ctrl to keep access keys available. Clear disables it. Shortcuts work while minimized, and are suspended while a Timelapse dialog or menu is open. Stop saves the recording; during preparation it cancels the start.",SS_NOPREFIX,CustomHelp);
            draft->error=child(L"STATIC",app.hotkeyWarning.c_str(),SS_NOPREFIX,CustomError);
            draft->okay=child(L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON|BS_NOTIFY,IDOK);draft->cancel=child(L"BUTTON",draft->readOnly?L"Close":L"Cancel",WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY,IDCANCEL);
            if(!draft->firstLabel || !draft->first || !draft->fpsHint || !draft->pauseLabel || !draft->second || !draft->clearPause || !draft->stopLabel || !draft->stop || !draft->clearStop || !draft->help || !draft->error || !draft->okay || !draft->cancel){EndDialog(window,-1);return TRUE;}
            if(!dialogWheelCombos({draft->first})){EndDialog(window,-1);return TRUE;}
            if(!SetWindowSubclass(draft->second,playbackFocusProc,2,0) || !SetWindowSubclass(draft->stop,playbackFocusProc,2,0)){EndDialog(window,-1);return TRUE;}
            SendMessageW(draft->second,HKM_SETHOTKEY,draft->pauseHotkey,0);SendMessageW(draft->stop,HKM_SETHOTKEY,draft->stopHotkey,0);
            if(draft->readOnly)for(HWND field:{draft->first,draft->second,draft->stop,draft->clearPause,draft->clearStop,draft->okay})EnableWindow(field,FALSE);
            customFont(window,*draft);RECT bounds{0,0,draft->scale(460),draft->scale(400)};
            AdjustWindowRectExForDpi(&bounds,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),draft->dpi);
            RECT owner{};GetWindowRect(app.window,&owner);OffsetRect(&bounds,(owner.left+owner.right-(bounds.right-bounds.left))/2-bounds.left,(owner.top+owner.bottom-(bounds.bottom-bounds.top))/2-bounds.top);
            bounds=fitWindow(bounds,workArea(MonitorFromWindow(app.window,MONITOR_DEFAULTTONEAREST)));SetWindowPos(window,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);
            playbackLayout(window,*draft);playbackFitHeight(window,*draft);SetFocus(draft->readOnly?draft->cancel:draft->first);return FALSE;
        }
        if(!draft)return FALSE;
        switch(message){
        case WM_SIZE:playbackLayout(window,*draft);return TRUE;
        case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:{const auto result=dialogWheel(window,*draft,message,wp);if(result==DialogWheel::Scrolled)playbackLayout(window,*draft);if(result!=DialogWheel::Pass)return TRUE;break;}
        case WM_DPICHANGED:{draft->dpi=HIWORD(wp);customFont(window,*draft);RECT bounds=fitWindow(*reinterpret_cast<RECT*>(lp),workArea(MonitorFromRect(reinterpret_cast<RECT*>(lp),MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);playbackLayout(window,*draft);playbackReveal(window,*draft,GetFocus());return TRUE;}
        case WM_HSCROLL:case WM_VSCROLL:{const int bar=message==WM_HSCROLL?SB_HORZ:SB_VERT;SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int position=info.nPos;
            switch(LOWORD(wp)){case SB_LINEUP:position-=draft->scale(24);break;case SB_LINEDOWN:position+=draft->scale(24);break;case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:position=info.nTrackPos;break;case SB_TOP:position=0;break;case SB_BOTTOM:position=info.nMax;break;default:return TRUE;}
            (bar==SB_HORZ?draft->scrollX:draft->scrollY)=position;playbackLayout(window,*draft);return TRUE;}
        case WM_COMMAND:{const int id=LOWORD(wp),code=HIWORD(wp);
            if(code==BN_SETFOCUS || code==CBN_SETFOCUS || code==EN_SETFOCUS){playbackReveal(window,*draft,reinterpret_cast<HWND>(lp));return TRUE;}
            if(id==IDCANCEL && code==BN_CLICKED){EndDialog(window,IDCANCEL);return TRUE;}
            if(id==IDOK && code==BN_CLICKED){if(draft->readOnly || app.active()){EndDialog(window,IDCANCEL);return TRUE;}
                std::wstring error;HWND invalid{};if(validatePlaybackDraft(*draft,error,invalid)){EndDialog(window,IDOK);return TRUE;}
                SetWindowTextW(draft->error,error.c_str());playbackLayout(window,*draft);playbackFitHeight(window,*draft);SetFocus(invalid);playbackReveal(window,*draft,invalid);return TRUE;}
            if((id==PlaybackClearPause || id==PlaybackClearStop) && code==BN_CLICKED && !draft->readOnly && !app.active()){
                const HWND field=id==PlaybackClearPause?draft->second:draft->stop;SendMessageW(field,HKM_SETHOTKEY,0,0);SetFocus(field);playbackReveal(window,*draft,field);return TRUE;}
            return FALSE;}
        case WM_CTLCOLORSTATIC:if(reinterpret_cast<HWND>(lp)==draft->error){SetTextColor(reinterpret_cast<HDC>(wp),RGB(164,40,40));SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_BTNFACE));return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));}break;
        case WM_CLOSE:EndDialog(window,IDCANCEL);return TRUE;
        case WM_DESTROY:if(draft->font){DeleteObject(draft->font);draft->font=nullptr;}if(app.customDialog==window)app.customDialog=IsWindow(draft->previousDialog)?draft->previousDialog:nullptr;return TRUE;
        }
    } catch(...){EndDialog(window,-1);return TRUE;}return FALSE;
}
bool savePreferences();
void editPlayback() {
    if(app.customDialog)return;PlaybackDraft draft;draft.fps=app.settings.outputFps;draft.pauseHotkey=app.pauseHotkey;draft.stopHotkey=app.stopHotkey;draft.readOnly=app.active();CustomTemplate resource;
    const auto outcome=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,app.window,playbackProc,reinterpret_cast<LPARAM>(&draft));
    if(!IsWindow(app.window))return;
    if(outcome==IDOK && !draft.readOnly && !app.active()){
        app.settings.outputFps=draft.fps;app.advancedOutputFps=-1;configure();updateControls();layout();InvalidateRect(app.window,nullptr,FALSE);
        if(app.startupComplete && !savePreferences())MessageBoxW(app.window,L"Your settings apply for this session, but could not be saved. Check that the settings folder is writable.",L"Timelapse",MB_OK|MB_ICONWARNING);
    }
    if(outcome==-1)MessageBoxW(app.window,L"Playback and shortcut settings could not be opened. Try again.",L"Timelapse",MB_OK|MB_ICONERROR);
    if(!app.closeWhenDone && !app.hiddenToTray){SetFocus(app.playbackConfigure);revealFocusedControl();}
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
constexpr const wchar_t* SkipKeys[]={L"TimeSkipMode",L"TimeSkipMultiplier",L"TimeSkipQuietAfterMs",L"TimeSkipRampFrames",L"TimeSkipRepeatSeconds",L"TimeSkipRanges",L"TimeSkipQuietSensitivity"};
bool skipInteger(std::wstring_view value,int64_t minimum,int64_t maximum,int64_t& result) {
    if(value.empty() || (value.size()>1 && value.front()==L'0'))return false;
    int64_t parsed=0;
    for(wchar_t c:value){if(c<L'0' || c>L'9' || parsed>(maximum-(c-L'0'))/10)return false;parsed=parsed*10+c-L'0';}
    if(parsed<minimum || parsed>maximum)return false;result=parsed;return true;
}
std::array<std::wstring,7> skipValues(const TimeSkipSettings& policy) {
    std::array<std::wstring,7> values{std::to_wstring(static_cast<int>(policy.mode)),std::to_wstring(policy.multiplier),
        std::to_wstring(policy.quietAfterMs),std::to_wstring(policy.rampFrames),std::to_wstring(policy.repeatSeconds),L"",std::to_wstring(static_cast<int>(policy.quietSensitivity))};
    for(unsigned i=0;i<policy.rangeCount;++i){if(i)values[5]+=L";";values[5]+=std::to_wstring(policy.ranges[i].startSeconds)+L":"+std::to_wstring(policy.ranges[i].endSeconds);}
    return values;
}
bool parseSkipValues(const std::array<std::wstring,7>& values,TimeSkipSettings& output) {
    TimeSkipSettings policy;int64_t value=0;
    if(!skipInteger(values[0],0,static_cast<int>(TimeSkipMode::NoPersonWithinSchedule),value))return false;policy.mode=static_cast<TimeSkipMode>(value);
    if(!skipInteger(values[1],2,64,value))return false;policy.multiplier=static_cast<int>(value);
    if(!skipInteger(values[2],1000,int64_t(INT_MAX)*1000,value) || value%1000)return false;policy.quietAfterMs=value;
    if(!skipInteger(values[3],15,60,value))return false;policy.rampFrames=static_cast<int>(value);
    if(!skipInteger(values[4],0,INT_MAX,value))return false;policy.repeatSeconds=static_cast<int>(value);
    if(!skipInteger(values[6],0,2,value))return false;policy.quietSensitivity=static_cast<QuietSensitivity>(value);
    std::wstring_view ranges=values[5];
    while(!ranges.empty()){
        if(policy.rangeCount==TimeSkipMaxRanges)return false;
        const auto end=ranges.find(L';');const auto part=ranges.substr(0,end);const auto colon=part.find(L':');int64_t start=0,stop=0;
        if(colon==std::wstring_view::npos || !skipInteger(part.substr(0,colon),0,INT_MAX,start) || !skipInteger(part.substr(colon+1),1,INT_MAX,stop))return false;
        policy.ranges[policy.rangeCount++]={static_cast<int>(start),static_cast<int>(stop)};
        if(end==std::wstring_view::npos)break;ranges.remove_prefix(end+1);if(ranges.empty())return false;
    }
    std::wstring error;if(!normalizeTimeSkipSettings(policy,error))return false;output=policy;return true;
}
TimeSkipSettings loadSkip(const wchar_t* path) {
    std::array<std::wstring,7> values;bool complete=true;
    for(size_t i=0;i<values.size();++i){wchar_t value[512]{};
        const DWORD count=GetPrivateProfileStringW(L"Settings",SkipKeys[i],i==6?L"1":L"?",value,512,path);
        if(count>=511 || std::wcscmp(value,L"?")==0)complete=false;values[i]=value;}
    TimeSkipSettings policy;if(!complete || !parseSkipValues(values,policy))return {};return policy;
}
constexpr const wchar_t* WatermarkKeys[]={L"WatermarkEnabled",L"WatermarkShowTime",L"WatermarkShowSpeed",L"WatermarkTimeKind",L"WatermarkX",L"WatermarkY",L"WatermarkTextSize"};
std::array<std::wstring,7> watermarkValues(const WatermarkSettings& value) {
    return {value.enabled?L"1":L"0",value.showTime?L"1":L"0",value.showSpeed?L"1":L"0",std::to_wstring(static_cast<int>(value.timeKind)),
        std::to_wstring(value.x),std::to_wstring(value.y),std::to_wstring(static_cast<int>(value.textSize))};
}
bool parseWatermarkValues(const std::array<std::wstring,7>& values,WatermarkSettings& output) {
    int64_t parsed[7]{};constexpr int maximum[]={1,1,1,1,10000,10000,2};
    for(size_t i=0;i<values.size();++i)if(!skipInteger(values[i],0,maximum[i],parsed[i]))return false;
    WatermarkSettings value;value.enabled=parsed[0]!=0;value.showTime=parsed[1]!=0;value.showSpeed=parsed[2]!=0;
    value.timeKind=static_cast<WatermarkTimeKind>(parsed[3]);value.x=static_cast<int>(parsed[4]);value.y=static_cast<int>(parsed[5]);value.textSize=static_cast<WatermarkTextSize>(parsed[6]);
    std::wstring error;if(!validateWatermarkSettings(value,error))return false;output=value;return true;
}
WatermarkSettings loadWatermark(const wchar_t* path) {
    std::array<std::wstring,7> values;
    for(size_t i=0;i<values.size();++i){wchar_t value[32]{};const DWORD count=GetPrivateProfileStringW(L"Settings",WatermarkKeys[i],L"?",value,32,path);
        if(count>=31 || std::wcscmp(value,L"?")==0)return {};values[i]=value;}
    WatermarkSettings value;if(!parseWatermarkValues(values,value))return {};return value;
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
        const int sizeChoice=choice(app.videoSize);
        const auto interval=std::to_wstring(choice(app.interval)), quality=std::to_wstring(sizeChoice>=0 && sizeChoice<=(app.hasCustomSize?2:1)?sizeChoice:app.committedSize);
        const auto encodingQuality=std::to_wstring(choice(app.encodingQuality));
        const auto encodingMode=std::to_wstring(choice(app.encodingMode));
        const auto recordingLimit=std::to_wstring(std::clamp(choice(app.stopAfter),0,app.hasCustomLimit?6:5));
        const auto customInterval=std::to_wstring(app.customIntervalMs), customWidth=std::to_wstring(app.customWidth),customHeight=std::to_wstring(app.customHeight);
        const auto customLimit=std::to_wstring(app.customLimitSeconds);
        const auto segmentDuration=std::to_wstring(selectedSegment());
        const auto startDelay=std::to_wstring(selectedStartDelay());
        const auto outputFps=std::to_wstring(app.settings.outputFps),pauseHotkey=std::to_wstring(app.pauseHotkey),stopHotkey=std::to_wstring(app.stopHotkey);
        std::wstring playbackError;if(!validateOutputFps(app.settings.outputFps,playbackError) || !validateHotkeys(app.pauseHotkey,app.stopHotkey,playbackError))return false;
        auto skipPolicy=app.settings.timeSkip;std::wstring skipError;
        if(!normalizeTimeSkipSettings(skipPolicy,skipError))return false;
        const auto skip=skipValues(skipPolicy);
        if(!validateWatermarkSettings(app.settings.watermark,skipError))return false;
        const auto watermark=watermarkValues(app.settings.watermark);
        const auto nightDuration=std::to_wstring(selectedNightDuration());
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
            WritePrivateProfileStringW(L"Settings",L"SegmentDurationSeconds",segmentDuration.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"StopOnLowDiskSpace",!app.lowDisk || SendMessageW(app.lowDisk,BM_GETCHECK,0,0)!=BST_UNCHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"RecoveryMode",app.recoveryMode && SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==BST_CHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"ShowDesktopCursor",!app.captureCursor || SendMessageW(app.captureCursor,BM_GETCHECK,0,0)!=BST_UNCHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightEnabled",app.nightEnabled && SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_CHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightDurationMs",nightDuration.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightTargetBrightness",nightTarget.c_str(),pending.path);
        for(size_t i=0;saved && i<skip.size();++i)saved=WritePrivateProfileStringW(L"Settings",SkipKeys[i],skip[i].c_str(),pending.path)!=FALSE;
        for(size_t i=0;saved && i<watermark.size();++i)saved=WritePrivateProfileStringW(L"Settings",WatermarkKeys[i],watermark[i].c_str(),pending.path)!=FALSE;
        if(saved)saved=WritePrivateProfileStringW(L"Settings",L"StartDelaySeconds",startDelay.c_str(),pending.path)!=FALSE;
        if(saved)saved=WritePrivateProfileStringW(L"Settings",L"OutputFps",outputFps.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"PauseHotkey",pauseHotkey.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"StopHotkey",stopHotkey.c_str(),pending.path);
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
        app.customSegmentSeconds=900;app.committedSegment=0;app.hasCustomSegment=false;
        app.customNightDurationMs=NightInitialDurationMs;app.committedNightDuration=0;app.hasCustomNightDuration=false;
        int nightDuration=0;
        if(exactInteger(L"NightDurationMs",NightMinDurationMs,NightMaxDurationMs,nightDuration)){
            app.customNightDurationMs=nightDuration;app.committedNightDuration=6;app.hasCustomNightDuration=true;
        }
        int segment=0;
        if(exactInteger(L"SegmentDurationSeconds",0,INT_MAX,segment) && segment){app.customSegmentSeconds=segment;app.committedSegment=5;app.hasCustomSegment=true;}
        int exactSelector=0;
        if(interval==6){app.hasCustomInterval=exactInteger(L"Interval",6,6,exactSelector) && exactInteger(L"CaptureIntervalMs",MinCaptureIntervalMs,MaxCaptureIntervalMs,app.customIntervalMs);choose(app.interval,app.hasCustomInterval?6:2);}
        if(videoSize==2){std::wstring error;app.hasCustomSize=exactInteger(L"Quality",2,2,exactSelector) && exactInteger(L"VideoWidth",MinVideoDimension,MaxVideoDimension,app.customWidth) &&
            exactInteger(L"VideoHeight",MinVideoDimension,MaxVideoDimension,app.customHeight) && validateVideoSize(app.customWidth,app.customHeight,error);
            choose(app.videoSize,app.hasCustomSize?2:0);}
        if(recordingLimit==6){app.hasCustomLimit=exactInteger(L"RecordingLimit",6,6,exactSelector) && exactInteger(L"RecordingLimitSeconds",1,INT_MAX,app.customLimitSeconds);choose(app.stopAfter,app.hasCustomLimit?6:0);}
        // Add the custom display row before selecting it: CB_SETCURSEL cannot
        // select an item that does not yet exist in an older preset-only list.
        app.committedInterval=app.hasCustomInterval?6:interval==6?2:std::clamp(interval,0,5);
        app.committedSize=app.hasCustomSize?2:videoSize==2?0:std::clamp(videoSize,0,1);
        app.committedLimit=app.hasCustomLimit?6:recordingLimit<=5?static_cast<int>(recordingLimit):0;
        normalizeCustomSelections();customItems();
        wchar_t stopOnLowDiskSpace[16]{};
        GetPrivateProfileStringW(L"Settings",L"StopOnLowDiskSpace",L"1",stopOnLowDiskSpace,16,path);
        SendMessageW(app.lowDisk,BM_SETCHECK,std::wcscmp(stopOnLowDiskSpace,L"0")==0?BST_UNCHECKED:BST_CHECKED,0);
        wchar_t recoveryMode[16]{};GetPrivateProfileStringW(L"Settings",L"RecoveryMode",L"0",recoveryMode,16,path);
        SendMessageW(app.recoveryMode,BM_SETCHECK,std::wcscmp(recoveryMode,L"1")==0?BST_CHECKED:BST_UNCHECKED,0);
        wchar_t captureCursor[16]{};GetPrivateProfileStringW(L"Settings",L"ShowDesktopCursor",L"1",captureCursor,16,path);
        SendMessageW(app.captureCursor,BM_SETCHECK,std::wcscmp(captureCursor,L"0")==0?BST_UNCHECKED:BST_CHECKED,0);
        wchar_t nightEnabled[16]{};GetPrivateProfileStringW(L"Settings",L"NightEnabled",L"0",nightEnabled,16,path);
        SendMessageW(app.nightEnabled,BM_SETCHECK,std::wcscmp(nightEnabled,L"1")==0?BST_CHECKED:BST_UNCHECKED,0);
        const auto loadNightChoice=[&](const wchar_t* key,HWND box,const auto& values,int fallback){
            const auto defaultValue=std::to_wstring(values[fallback]);wchar_t value[32]{};
            GetPrivateProfileStringW(L"Settings",key,defaultValue.c_str(),value,32,path);
            int selected=fallback;
            for(size_t i=0;i<std::size(values);++i)if(std::to_wstring(values[i])==value){selected=static_cast<int>(i);break;}
            choose(box,selected);
        };
        loadNightChoice(L"NightTargetBrightness",app.nightTarget,NightTargets,1);
        loadNightChoice(L"StartDelaySeconds",app.startDelay,StartDelays,0);
        app.committedStartDelay=std::clamp(choice(app.startDelay),0,5);
        app.settings.outputFps=DefaultOutputFps;
        exactInteger(L"OutputFps",MinOutputFps,MaxOutputFps,app.settings.outputFps);
        app.recordedOutputFps=app.settings.outputFps;
        int pause=0,stop=0;exactInteger(L"PauseHotkey",0,UINT16_MAX,pause);exactInteger(L"StopHotkey",0,UINT16_MAX,stop);
        if(!validHotkey(static_cast<uint16_t>(pause)))pause=0;if(!validHotkey(static_cast<uint16_t>(stop)) || (pause && hotkeyIdentity(static_cast<uint16_t>(pause))==hotkeyIdentity(static_cast<uint16_t>(stop))))stop=0;
        unregisterRecordingHotkeys();app.pauseHotkey=static_cast<uint16_t>(pause);app.stopHotkey=static_cast<uint16_t>(stop);
        std::wstring shortcutError;if(!setRecordingHotkeys(app.pauseHotkey,app.stopHotkey,shortcutError))app.hotkeyWarning=std::move(shortcutError);
        app.advancedOutputFps=-1;
        app.settings.timeSkip=loadSkip(path);++app.skipRevision;
        app.settings.watermark=loadWatermark(path);app.watermarkCheckValid=false;++app.watermarkRevision;
        // Launch on desktop: opening the app never silently turns on a camera.
        choose(app.mode,0);
        updateAdvanced();
    } else if(!savePreferences()) {
        OutputDebugStringW(L"Timelapse could not save preferences.\n");
    }
}

RECT layerRect(const Layer& l) {
    const LONG width=app.videoRect.right-app.videoRect.left,height=app.videoRect.bottom-app.videoRect.top;
    if(width<=0 || height<=0)return {app.videoRect.left,app.videoRect.top,app.videoRect.left,app.videoRect.top};
    const auto rect=constrain(l.rect);
    const LONG left=std::min(width-1,static_cast<LONG>(rect.x*width)),top=std::min(height-1,static_cast<LONG>(rect.y*height));
    return {app.videoRect.left+left,app.videoRect.top+top,
        app.videoRect.left+std::min(width,std::max(left+1,static_cast<LONG>((rect.x+rect.w)*width))),
        app.videoRect.top+std::min(height,std::max(top+1,static_cast<LONG>((rect.y+rect.h)*height)))};
}
RECT layerGripRect(const RECT& layer,int preferredPixels) {
    const LONG width=layer.right-layer.left,height=layer.bottom-layer.top;
    // A single physical pixel has no distinct body and corner. Keep pointer
    // movement available; Shift+arrows still resize the normalized rectangle.
    if(width<=0 || height<=0 || (width==1 && height==1))return {layer.right,layer.bottom,layer.right,layer.bottom};
    const LONG gripWidth=std::min<LONG>(preferredPixels,std::max(1L,width/3)),gripHeight=std::min<LONG>(preferredPixels,std::max(1L,height/3));
    return {layer.right-gripWidth,layer.bottom-gripHeight,layer.right,layer.bottom};
}
struct StateLook { const wchar_t* label; COLORREF fill, ink; };
StateLook stateLook() {
    switch(app.status.state) {
    case State::Recording:return {L"Recording",DangerSoft,Danger};
    case State::Paused:return {L"Paused",WarningSoft,Warning};
    case State::Waiting:return {L"Waiting to start",InfoSoft,Info};
    case State::Starting:return {L"Preparing",InfoSoft,Info};
    case State::Finishing:return {L"Saving",InfoSoft,Info};
    default:return app.status.recordingFailed?StateLook{L"Stopped",DangerSoft,Danger}:StateLook{L"Ready",RGB(230,232,235),Muted};
    }
}
std::wstring stageHint() {
    if(app.settings.separateFiles)return L"Desktop and camera each save to their own MP4.";
    if(app.settings.layers.size()>1)return L"Drag a layer to move it. Pull its corner to resize.";
    return L"Preview · "+sizeText(app.settings.width,app.settings.height)+L" at "+std::to_wstring(app.settings.outputFps)+L" fps";
}
// Antialiased glyph from a signed-distance function in box pixels; the box is
// small, so this costs a few hundred pixel evaluations per button paint.
template<class Distance> void shade(HDC dc,const RECT& box,COLORREF ink,COLORREF backdrop,Distance distance) {
    const int w=box.right-box.left,h=box.bottom-box.top;
    if(w<=0 || h<=0 || w>48 || h>48)return;
    std::array<uint32_t,48*48> pixels{};
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){const COLORREF color=blend(backdrop,ink,coverage(distance(x+.5,y+.5),1.0));
        pixels[size_t(y)*w+x]=(uint32_t(GetRValue(color))<<16)|(uint32_t(GetGValue(color))<<8)|GetBValue(color);}
    StretchDIBits(dc,box.left,box.top,w,h,0,0,w,h,pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
}
enum class Glyph { None, Dot, Bars, Play, Square, ChevronDown, ChevronUp };
void glyph(HDC dc,Glyph kind,const RECT& box,COLORREF ink,COLORREF backdrop) {
    const double s=box.right-box.left;
    // Convex shapes use the largest outward edge distance of their sides.
    const auto triangle=[s](double x,double y){
        const double points[][2]={{.22*s,.1*s},{.94*s,.5*s},{.22*s,.9*s}};double distance=-1e9;
        for(int i=0;i<3;++i){const auto& a=points[i];const auto& b=points[(i+1)%3];const double dx=b[0]-a[0],dy=b[1]-a[1],length=std::hypot(dx,dy);
            distance=std::max(distance,((x-a[0])*dy-(y-a[1])*dx)/length);}
        return distance;};
    const double stroke=std::max(1.4,s/7);
    switch(kind) {
    case Glyph::Dot:shade(dc,box,ink,backdrop,[s](double x,double y){return std::hypot(x-s/2,y-s/2)-s*.42;});break;
    case Glyph::Bars:shade(dc,box,ink,backdrop,[s](double x,double y){return std::min(boxDistance(x,y,s*.27,s/2,s*.14,s*.44,1),boxDistance(x,y,s*.73,s/2,s*.14,s*.44,1));});break;
    case Glyph::Play:shade(dc,box,ink,backdrop,triangle);break;
    case Glyph::Square:shade(dc,box,ink,backdrop,[s](double x,double y){return boxDistance(x,y,s/2,s/2,s*.4,s*.4,s*.12);});break;
    case Glyph::ChevronDown:case Glyph::ChevronUp:{
        const double top=kind==Glyph::ChevronDown?s*.32:s*.68,tip=kind==Glyph::ChevronDown?s*.68:s*.32;
        shade(dc,box,ink,backdrop,[=](double x,double y){return std::min(segmentDistance(x,y,s*.12,top,s*.5,tip),segmentDistance(x,y,s*.5,tip,s*.88,top))-stroke/2;});break;}
    default:break;
    }
}
enum class ButtonLook { None, Primary, Secondary, Quiet, Disclosure };
ButtonLook buttonLook(HWND button) {
    if(!button)return ButtonLook::None;
    if(button==app.record)return ButtonLook::Primary;
    if(button==app.advanced)return ButtonLook::Disclosure;
    if(button==app.statusDetails)return ButtonLook::Quiet;
    for(HWND member:{app.pause,app.finish,app.refresh,app.folder,app.openFolder,app.reset,app.forward,app.skipConfigure,app.watermarkConfigure,app.playbackConfigure})
        if(member==button)return ButtonLook::Secondary;
    return ButtonLook::None;
}
COLORREF controlBackdrop(HWND child) {
    return child==app.record || child==app.pause || child==app.finish || panelControl(child)?Panel:Background;
}
// Native buttons keep their behavior, state, keyboard and accessibility; only
// their face is painted here, through comctl32 custom draw.
LRESULT drawButton(const NMCUSTOMDRAW& draw) {
    const HWND button=draw.hdr.hwndFrom;const auto look=buttonLook(button);
    if(look==ButtonLook::None || draw.dwDrawStage!=CDDS_PREPAINT)return CDRF_DODEFAULT;
    const HDC dc=draw.hdc;const RECT r=draw.rc;
    const auto state=SendMessageW(button,BM_GETSTATE,0,0),cues=SendMessageW(button,WM_QUERYUISTATE,0,0);
    const bool enabled=IsWindowEnabled(button)!=FALSE && !(draw.uItemState&CDIS_DISABLED);
    const bool pressed=enabled && ((state&BST_PUSHED) || (draw.uItemState&CDIS_SELECTED));
    const bool hot=enabled && ((state&BST_HOT) || (draw.uItemState&CDIS_HOT));
    const bool focused=(draw.uItemState&CDIS_FOCUS) && !(cues&UISF_HIDEFOCUS);
    const COLORREF backdrop=controlBackdrop(button);
    COLORREF fill=Panel,edge=Border,ink=enabled?Ink:Subtle;
    switch(look) {
    case ButtonLook::Primary:
        fill=!enabled?RGB(233,235,238):pressed?RGB(158,33,21):hot?RGB(177,38,25):Danger;edge=fill;ink=enabled?RGB(255,255,255):Subtle;break;
    case ButtonLook::Quiet:
        fill=pressed?RGB(222,227,232):hot?RGB(232,236,240):backdrop;edge=fill;ink=enabled?Accent:Subtle;break;
    default:
        fill=!enabled?blend(backdrop,Panel,.5):pressed?RGB(229,232,236):hot?RGB(242,244,246):Panel;
        edge=enabled?Border:RGB(232,235,238);
        if(look==ButtonLook::Disclosure && enabled && app.advancedWarning)ink=Danger;
        break;
    }
    if(focused)edge=Ink;
    roundRect(dc,r,app.scale(look==ButtonLook::Primary?6:5),fill,backdrop,edge,focused?std::max(2,app.scale(2)):1);
    wchar_t caption[128]{};GetWindowTextW(button,caption,static_cast<int>(std::size(caption)));
    Glyph kind=Glyph::None;
    if(button==app.record)kind=Glyph::Dot;
    else if(button==app.pause)kind=std::wcsstr(caption,L"Resume")?Glyph::Play:Glyph::Bars;
    else if(button==app.finish && !std::wcsstr(caption,L"Cancel"))kind=Glyph::Square;
    else if(look==ButtonLook::Disclosure)kind=(state&BST_CHECKED)?Glyph::ChevronUp:Glyph::ChevronDown;
    const HFONT owned=reinterpret_cast<HFONT>(SendMessageW(button,WM_GETFONT,0,0));
    const auto previousFont=SelectObject(dc,look==ButtonLook::Primary && app.strongFont?app.strongFont:owned?owned:app.font);
    const UINT prefix=(cues&UISF_HIDEACCEL)?DT_HIDEPREFIX:0;
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ink);
    const int icon=kind==Glyph::None?0:app.scale(10),middle=(r.top+r.bottom)/2;
    if(look==ButtonLook::Disclosure) {
        const int inset=app.scale(14);
        RECT label{r.left+inset,r.top,r.right-inset-icon-app.scale(8),r.bottom};
        DrawTextW(dc,caption,-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|prefix);
        glyph(dc,kind,{r.right-inset-icon,middle-icon/2,r.right-inset,middle-icon/2+icon},enabled?Muted:Subtle,fill);
    } else {
        RECT measured{};DrawTextW(dc,caption,-1,&measured,DT_CALCRECT|DT_SINGLELINE|prefix);
        const int gap=app.scale(8),textW=measured.right-measured.left;
        // Drop the glyph rather than clip a translated or longer caption.
        const bool withIcon=icon && textW+icon+gap+app.scale(20)<=r.right-r.left;
        const int content=textW+(withIcon?icon+gap:0);
        int x=r.left+std::max(app.scale(8),static_cast<int>((r.right-r.left-content)/2));
        if(withIcon){glyph(dc,kind,{x,middle-icon/2,x+icon,middle-icon/2+icon},ink,fill);x+=icon+gap;}
        RECT label{x,r.top,r.right-app.scale(6),r.bottom};
        DrawTextW(dc,caption,-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|prefix);
    }
    SelectObject(dc,previousFont);
    return CDRF_SKIPDEFAULT;
}
void paintHeader(HDC dc) {
    const RECT band=app.headerRect;if(band.right<=band.left)return;
    const auto look=stateLook();
    const int height=app.scale(26),top=band.top+(band.bottom-band.top-height)/2,inset=app.scale(11),mark=app.scale(8),space=app.scale(7);
    RECT pill{band.left,top,std::min<LONG>(band.right,band.left+2*inset+mark+space+textWidth(dc,look.label,app.headerFont)),top+height};
    roundRect(dc,pill,height/2,look.fill,Background);
    const int markX=pill.left+inset,markY=top+(height-mark)/2;
    glyph(dc,app.status.state==State::Paused?Glyph::Bars:Glyph::Dot,{markX,markY,markX+mark,markY+mark},look.ink,look.fill);
    text(dc,look.label,{markX+mark+space,pill.top,pill.right,pill.bottom},look.ink,app.headerFont);
    text(dc,stageHint(),{pill.right+app.scale(12),band.top,band.right,band.bottom},Muted,app.smallFont);
}
void paintStats(HDC dc) {
    const RECT r=app.statsRect;if(r.right<=r.left)return;
    std::wstring primary,secondary;HFONT primaryFont=app.strongFont;
    if((app.active() && app.status.state!=State::Waiting) || app.status.frames) {
        primary=timeText(app.status.elapsed,true);primaryFont=app.titleFont;
        secondary=std::to_wstring(app.status.frames)+L" frames  ·  "+timeText(double(app.status.frames)/std::clamp(app.recordedOutputFps,MinOutputFps,MaxOutputFps),false)+L" video";
    } else if(skipEnabled()) {
        primary=L"Base interval "+formatDuration(app.settings.intervalMs);
        secondary=L"Time compression up to "+std::to_wstring(app.settings.timeSkip.multiplier)+L"×";
    } else {
        primary=L"Every "+formatDuration(app.settings.intervalMs);
        wchar_t buffer[96]{};const double videoSeconds=3600000.0/(double(std::max(MinCaptureIntervalMs,app.settings.intervalMs))*app.settings.outputFps);
        swprintf_s(buffer,L"1 hour → %.*f s video",videoSeconds<1?3:videoSeconds<10?1:0,videoSeconds);secondary=buffer;
    }
    if(app.status.completedSegments)secondary+=L"  ·  "+std::to_wstring(app.status.completedSegments)+(app.status.completedSegments==1?L" part saved":L" parts saved");
    const int split=r.top+(r.bottom-r.top)*3/5;
    text(dc,primary,{r.left,r.top,r.right,split},Ink,primaryFont,DT_LEFT|DT_BOTTOM|DT_SINGLELINE|DT_END_ELLIPSIS);
    text(dc,secondary,{r.left,split+app.scale(2),r.right,r.bottom},Muted,app.smallFont,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_END_ELLIPSIS);
}
void paintPanel(HDC dc) {
    const int pad=app.scale(PanelPad),left=app.panelRect.left+pad,right=app.panelRect.right-pad,offset=app.panelScroll;
    static constexpr const wchar_t* Headings[]={L"INPUT",L"OUTPUT",L"SAVE TO"};
    const int spacing=SetTextCharacterExtra(dc,std::max(1,app.scale(1)));
    for(int i=0;i<3;++i){const int top=app.sectionTops[i]-offset;text(dc,Headings[i],{left,top,right-app.scale(100),top+app.scale(30)},Muted,app.headerFont);}
    SetTextCharacterExtra(dc,spacing);
    for(int i=0;i<app.panelRuleCount;++i){const int y=app.panelRules[i]-offset;solid(dc,{left,y,right,y+1},Border);}
    RECT path=app.savePathRect;OffsetRect(&path,0,-offset);
    text(dc,app.settings.folder,path,Ink,app.smallFont,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_PATH_ELLIPSIS);
}
// Paints the parent surface in client coordinates; children are clipped out.
void paintWindow(HDC dc,const RECT& dirty) {
    const int divider=app.panelRect.left-app.scrollX;
    solid(dc,{dirty.left,dirty.top,std::min<LONG>(dirty.right,divider),dirty.bottom},Background);
    solid(dc,{std::max<LONG>(dirty.left,divider),dirty.top,dirty.right,dirty.bottom},Panel);
    solid(dc,{divider,dirty.top,divider+1,dirty.bottom},Border);
    POINT origin{};OffsetViewportOrgEx(dc,-app.scrollX,-app.scrollY,&origin);
    RECT logical=dirty;OffsetRect(&logical,app.scrollX,app.scrollY);
    const auto touches=[&](const RECT& area){RECT overlap{};return IntersectRect(&overlap,&logical,&area)!=FALSE;};
    if(touches(app.headerRect))paintHeader(dc);
    if(touches(app.transportRect)){roundRect(dc,app.transportRect,app.scale(8),Panel,Background,Border,1);paintStats(dc);}
    if(touches(app.panelRect))paintPanel(dc);
    SetViewportOrgEx(dc,origin.x,origin.y,nullptr);
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
        HDC mem=CreateCompatibleDC(dc);
        BITMAPINFO surface{};surface.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);surface.bmiHeader.biWidth=std::max(1L,r.right);
        surface.bmiHeader.biHeight=-std::max(1L,r.bottom);surface.bmiHeader.biPlanes=1;surface.bmiHeader.biBitCount=32;surface.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;HBITMAP bmp=mem?CreateDIBSection(dc,&surface,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
        if(!mem || !bmp){if(bmp)DeleteObject(bmp);if(mem)DeleteDC(mem);FillRect(dc,&r,app.background);EndPaint(w,&ps);return 0;}
        auto old=SelectObject(mem,bmp);
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
            if(selected) {RECT handle=layerGripRect(layer,app.scale(12));HBRUSH h=CreateSolidBrush(RGB(83,229,205));FillRect(mem,&handle,h);DeleteObject(h);}
        }
        // Only the small corner squares need alpha coverage. Blend the finished
        // preview into the stage background, including video and layer handles.
        GdiFlush();
        const int radius=std::min({app.scale(8),int(r.right)/2,int(r.bottom)/2,48});
        auto surfacePixels=static_cast<uint32_t*>(pixels);
        for(int corner=0;corner<4;++corner)for(int y=0;y<radius;++y)for(int x=0;x<radius;++x){
            const double dx=radius-(x+.5),dy=radius-(y+.5),alpha=std::clamp(radius-std::hypot(dx,dy)+.5,0.0,1.0);
            const int px=(corner&1)?r.right-1-x:x,py=(corner&2)?r.bottom-1-y:y;
            auto& pixel=surfacePixels[size_t(py)*surface.bmiHeader.biWidth+px];
            const COLORREF color=blend(Background,RGB((pixel>>16)&255,(pixel>>8)&255,pixel&255),alpha);
            pixel=(uint32_t(GetRValue(color))<<16)|(uint32_t(GetGValue(color))<<8)|GetBValue(color);
        }
        BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);EndPaint(w,&ps);return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(w); if(app.settings.separateFiles || app.settings.layers.size()<2) return 0;
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};app.selected=-1;
        for(int i=static_cast<int>(app.settings.layers.size())-1;i>=0;--i) {RECT r=layerRect(app.settings.layers[i]);if(PtInRect(&r,p)){app.selected=i;app.dragStart=p;app.dragRect=app.settings.layers[i].rect;const RECT grip=layerGripRect(r,app.scale(18));app.resizing=PtInRect(&grip,p)!=FALSE;app.dragging=true;SetCapture(w);break;}}
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
    case ShowExistingMessage:if(app.failureNotice!=FailureNotice::Presenting)showWindow(true);return 0;
    case TrayMessage: {
        if(app.failureNotice==FailureNotice::Presenting)return 0;
        const UINT event=app.trayVersion4?LOWORD(lp):static_cast<UINT>(lp);
        if(event==WM_LBUTTONUP || event==WM_LBUTTONDBLCLK || event==NIN_SELECT || event==NIN_KEYSELECT || event==NIN_BALLOONUSERCLICK)showWindow(true);
        else if(event==WM_RBUTTONUP || event==WM_CONTEXTMENU){POINT at{GET_X_LPARAM(wp),GET_Y_LPARAM(wp)};trayMenu(at,app.trayVersion4 && !(at.x==-1 && at.y==-1));}
        return 0;
    }
    case WM_CREATE: {
        cancelOpenFolder();app.shellBusyObserved=shellOperationBusy.load(std::memory_order_acquire);app.openFolderBusyShown=false;
        app.startupComplete=false;app.advancedExpanded=false;app.advancedLimitIndex=app.advancedVisibility=-1;
        app.advancedSegmentSeconds=-1;
        app.advancedCursorState=app.cursorVisibility=app.advancedDelaySeconds=-1;app.committedStartDelay=0;
        app.waitingRemaining=UINT64_MAX;app.waitingCaption.clear();
        app.failureNotice=FailureNotice::None;
        app.trayMenuOpen=app.trayMenuCanceled=false;
        app.advancedNightState=app.advancedRecoveryState=app.nightVisibility=-1;app.nightValidation.clear();app.encodingValidation.clear();app.statusCaption.clear();app.statusCaptionError=false;app.nightHintCaption.clear();app.nightDetailCaption.clear();
        app.cameraListError.clear();app.statusTooltipCaption.clear();
        app.statusDetailsVisible=-1;
        app.customDialog=nullptr;app.advancedCaption.clear();app.advancedTooltip.clear();
        app.skipRevision=0;app.advancedSkipRevision=app.skipSummaryRevision=app.skipVisibility=-1;app.skipCheckAge=UINT64_MAX;
        app.skipSummaryCaption.clear();app.skipDetailCaption.clear();app.settings.timeSkip={};
        app.personPack={};app.personPackKnown=false;
        app.settings.watermark={};app.watermarkChecked={};app.watermarkCheckValid=false;app.watermarkRevision=0;app.advancedWatermarkRevision=-1;app.watermarkValidation.clear();app.watermarkCaption.clear();
        unregisterRecordingHotkeys();app.pauseHotkey=app.stopHotkey=0;app.hotkeyWarning.clear();app.settings.outputFps=app.recordedOutputFps=DefaultOutputFps;app.advancedOutputFps=-1;
        app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;
        app.customIntervalMs=5000;app.customWidth=1280;app.customHeight=720;app.customLimitSeconds=900;
        app.committedInterval=2;app.committedSize=app.committedLimit=0;
        app.committedSegment=0;app.customSegmentSeconds=900;app.hasCustomSegment=false;
        app.committedNightDuration=0;app.customNightDurationMs=NightInitialDurationMs;app.hasCustomNightDuration=false;
        app.visibleDirty=true;app.controlsUpdated=false;app.trayStateValid=false;
        app.scrollX=app.scrollY=app.panelScroll=app.wheelVertical=app.wheelHorizontal=app.wheelPanel=0;
        app.contentWidth=app.contentHeight=app.panelHeight=app.advancedTop=0;
        app.panelDocked=true;app.collageTools=app.advancedWarning=false;
        try {
        app.window=w;app.dpi=static_cast<int>(GetDpiForWindow(w));fonts();
        // Attribute 35 is DWMWA_CAPTION_COLOR. Older Windows/SDKs simply
        // retain their normal title bar when the attribute is unsupported.
        const COLORREF captionColor=Background;
        DwmSetWindowAttribute(w,35,&captionColor,sizeof(captionColor));
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
        auto button=[&](const wchar_t* s,int id){return requiredControl(L"BUTTON",s,WS_TABSTOP|BS_PUSHBUTTON,id);};
        app.reset=button(L"Reset layout",Reset);app.forward=button(L"Bring forward",Forward);
        app.preview=requiredControl(L"LapsePreview",L"Collage preview. Space selects a layer. Arrow keys move it. Shift and arrow keys resize it.",WS_TABSTOP,Preview);
        app.record=button(L"&Record",Record);app.pause=button(L"&Pause",Pause);app.finish=button(L"&Finish",Finish);
        app.statusText=requiredControl(L"STATIC",app.status.message.c_str(),SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,210);
        app.statusDetails=button(L"Deta&ils...",StatusDetails);
        app.refresh=button(L"Re&fresh",Refresh);
        app.mode=combo(0,L"&Source",ModeBox);for(auto s:{L"Desktop",L"Camera",L"Desktop + camera",L"Side by side",L"Custom collage",SeparateFilesLabel})add(app.mode,s);
        app.monitor=combo(4,L"&Display",MonitorBox);app.camera=combo(5,L"Ca&mera",CameraBox);
        app.interval=combo(1,L"Capture &every",IntervalBox);for(auto s:{L"1 second",L"2 seconds",L"5 seconds",L"10 seconds",L"30 seconds",L"60 seconds"})add(app.interval,s);
        app.videoSize=combo(2,L"Video si&ze",SizeBox);add(app.videoSize,L"720p");add(app.videoSize,L"1080p");
        app.encodingQuality=combo(3,L"Video &quality",EncodingQualityBox);for(auto s:{L"Smaller file",L"Balanced",L"More detail"})add(app.encodingQuality,s);
        app.folder=button(L"C&hange...",Folder);app.openFolder=button(L"&Open folder",OpenFolder);
        app.advanced=requiredControl(L"BUTTON",L"&Advanced",WS_TABSTOP|BS_AUTOCHECKBOX|BS_PUSHLIKE,AdvancedToggle);
        app.encodingMode=combo(6,L"Encodin&g",EncodingModeBox);
        for(auto label:EncodingModeLabels)add(app.encodingMode,label);
        app.recoveryMode=requiredControl(L"BUTTON",L"MP4 recover&y mode (H.264)",WS_TABSTOP|BS_AUTOCHECKBOX,RecoveryBox);
        app.stopAfter=combo(7,L"S&top after",StopAfterBox);for(auto label:RecordingLimitLabels)add(app.stopAfter,label);
        app.segmentLabel=requiredControl(L"STATIC",L"Split files e&very",0,211);
        app.splitEvery=requiredControl(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,SegmentBox);for(auto label:SegmentLabels)add(app.splitEvery,label);
        app.startDelayLabel=requiredControl(L"STATIC",L"Delay ne&xt recording",0,212);
        app.startDelay=requiredControl(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,StartDelayBox);
        for(auto label:StartDelayLabels)add(app.startDelay,label);choose(app.startDelay,0);
        app.startDelayHint=requiredControl(L"STATIC",L"Preparation starts after the delay. Visible preview continues; Stop after counts active time.",SS_LEFT|SS_NOPREFIX,213);
        app.lowDisk=requiredControl(L"BUTTON",L"Stop on &low disk space",WS_TABSTOP|BS_AUTOCHECKBOX,LowDiskBox);
        app.captureCursor=requiredControl(L"BUTTON",L"Show des&ktop cursor",WS_TABSTOP|BS_AUTOCHECKBOX,CursorBox);
        SendMessageW(app.captureCursor,BM_SETCHECK,BST_CHECKED,0);
        app.skipConfigure=button(L"Time &compression...",SkipConfigure);
        app.skipSummary=requiredControl(L"STATIC",L"Off",SS_LEFT|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,SkipSummary);
        app.skipDetail=requiredControl(L"STATIC",L"",SS_LEFT|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,SkipDetail);
        app.watermarkConfigure=button(L"&Watermark...",WatermarkConfigure);
        app.watermarkSummary=requiredControl(L"STATIC",L"Off",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,WatermarkSummary);
        app.playbackConfigure=button(L"Playback && shortcuts...",PlaybackConfigure);
        app.nightEnabled=requiredControl(L"BUTTON",L"&Night camera (software blend)",WS_TABSTOP|BS_AUTOCHECKBOX,NightBox);
        app.nightDuration=combo(8,L"Blend d&uration",NightDurationBox);for(auto label:NightDurationLabels)add(app.nightDuration,label);
        app.nightTarget=combo(9,L"Auto &brightness",NightTargetBox);for(auto label:{L"Dark",L"Balanced",L"Bright"})add(app.nightTarget,label);
        app.nightHint=requiredControl(L"STATIC",L"",SS_LEFT|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,NightHint);
        app.nightDetail=requiredControl(L"STATIC",L"",SS_LEFT|SS_ENDELLIPSIS|SS_NOPREFIX|SS_NOTIFY,NightDetail);
        if(!controlsReady) {
            app.mode=nullptr;
            OutputDebugStringW(L"Timelapse could not create its required controls.\n");
            return -1;
        }
        fonts();
        app.tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,w,nullptr,nullptr,nullptr);
        TOOLINFOW tip{sizeof(tip)};tip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tip.hwnd=w;tip.uId=reinterpret_cast<UINT_PTR>(app.statusText);tip.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));SendMessageW(app.tooltip,TTM_SETMAXTIPWIDTH,0,app.scale(520));
        tip.uId=reinterpret_cast<UINT_PTR>(app.encodingMode);
        tip.lpszText=const_cast<LPWSTR>(L"Compatible keeps the original software H.264 settings. Efficient uses a bitrate target at every quality level. Quality H.264 and hardware modes use fixed quantization; detailed or changing scenes can make much larger files. Hardware modes require a supported encoder and do not switch to software if unavailable. HEVC playback needs a compatible player or decoder. File size and image quality depend on the scene and encoder.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.mode);
        tip.lpszText=const_cast<LPWSTR>(L"Separate files records full-frame desktop and camera videos together. The side-by-side preview is only for monitoring; each source has its own MP4.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.videoSize);tip.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.advanced);
        tip.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.startDelay);
        tip.lpszText=const_cast<LPWSTR>(L"After Record, wait before preparing the recording. Camera startup and Night blending can add time before the first frame. Visible preview continues. Stop after excludes this wait. Closing hides to the tray and keeps the timer; Cancel start, Exit or sleep cancels it.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.playbackConfigure);
        tip.lpszText=const_cast<LPWSTR>(L"Set the final MP4 playback frame rate and optional global shortcuts for pause/resume and stop/save. Available before recording. Shortcuts work while minimized and pause while a Timelapse dialog or menu is open.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.stopAfter);
        tip.lpszText=const_cast<LPWSTR>(L"Finish and save automatically after this much active recording time. Pauses and initial startup do not count. Never records until you choose Finish.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.lowDisk);
        tip.lpszText=const_cast<LPWSTR>(L"Check free space in the save folder and try to finish and save before space runs out. Other programs or sudden disk changes can still cause a recording to fail. Turn this off to record when free space cannot be checked.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.splitEvery);tip.lpszText=const_cast<LPWSTR>(SegmentHelp);
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.captureCursor);
        tip.lpszText=const_cast<LPWSTR>(L"Add the Windows mouse cursor to desktop preview and recordings. This does not remove pointers already drawn into application pixels. Camera video is unchanged. Fixed while recording; quiet-scene checks always ignore the added cursor.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.recoveryMode);
        tip.lpszText=const_cast<LPWSTR>(L"After an unexpected app exit, completed portions may remain playable in the .recording.mp4 file. Recent frames can be lost; very early interruptions may leave no playable video. H.264 only. Larger files and extra processing; some players may not support this format. Finish normally to save. No recovery guarantee after power loss or drive failure.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.nightEnabled);
        tip.lpszText=const_cast<LPWSTR>(L"Optional software blending and automatic digital brightness for camera recordings. It does not change camera shutter settings. Motion can blur; clipped or missing detail cannot be recovered. Idle preview is unchanged; the effect appears during recording.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.nightDuration);
        tip.lpszText=const_cast<LPWSTR>(L"Auto chooses a blend duration within the capture interval, up to 30 seconds. Custom accepts 1 to 30 seconds in whole milliseconds. A manual duration keeps automatic brightness and must not exceed Capture every. Late blends retain their full duration and delay later captures instead of catching up.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.nightTarget);
        tip.lpszText=const_cast<LPWSTR>(L"Automatic camera brightness uses 8-bit brightness references: Dark 64/255, Balanced 96/255, Bright 128/255. This is processed image brightness, not sensor exposure. Gain and highlight limits can leave the target unmet.");
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.lpszText=LPSTR_TEXTCALLBACKW;
        for(HWND child:{app.nightHint,app.nightDetail,app.skipSummary,app.skipDetail}){tip.uId=reinterpret_cast<UINT_PTR>(child);SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
        tip.uFlags=TTF_SUBCLASS;tip.uId=SavePathTip;tip.rect={};tip.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
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
        if(wheelScroll(msg,wp,pointInPanel({GET_X_LPARAM(lp),GET_Y_LPARAM(lp)})))return 0;
        break;
    }
    case WM_DPICHANGED: {
        endLayoutDrag();
        const int dpi=HIWORD(wp);
        app.scrollX=MulDiv(app.scrollX,dpi,app.dpi);app.scrollY=MulDiv(app.scrollY,dpi,app.dpi);
        app.panelScroll=MulDiv(app.panelScroll,dpi,app.dpi);
        app.dpi=dpi;fonts();auto suggested=reinterpret_cast<RECT*>(lp);
        const RECT r=fitWindow(*suggested,workArea(MonitorFromRect(suggested,MONITOR_DEFAULTTONEAREST)));
        SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);layout();revealFocusedControl();return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info=reinterpret_cast<MINMAXINFO*>(lp);
        const RECT work=workArea(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST));
        RECT minimum{0,0,app.scale(StageMinWidth+PanelWidth)+GetSystemMetricsForDpi(SM_CXVSCROLL,app.dpi),app.scale(StageMinHeight)};
        AdjustWindowRectExForDpi(&minimum,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,FALSE,0,app.dpi);
        info->ptMinTrackSize={std::min<LONG>(minimum.right-minimum.left,work.right-work.left),std::min<LONG>(minimum.bottom-minimum.top,work.bottom-work.top)};
        return 0;
    }
    case WM_TIMER: {
        if(wp!=1 || !app.engine)return 0;
        const bool wasHidden=app.hiddenToTray;applyStatus(app.engine->status());
        if(app.trayRegistered && !updateTray() && app.hiddenToTray)showWindow();
        if(app.failureNotice==FailureNotice::Presenting)return 0;
        if(app.closeWhenDone && !app.active()) {
            app.closeWhenDone=false;
            if(app.status.recordingFailed) {
                // Keep the recovery path visible if saving failed while the
                // user was waiting for Finish and close.
                EnableWindow(w,TRUE);if(!reportRecordingFailure())showWindow();
            } else DestroyWindow(w);
        } else if(wasHidden && pendingRecordingFailure())reportRecordingFailure();
        else acknowledgeVisibleFailure();
        pollOpenFolder();
        return 0;
    }
    case WM_HOTKEY: {
        if(!app.engine || app.customDialog || app.trayMenuOpen || app.closeWhenDone || app.failureNotice==FailureNotice::Presenting || !IsWindowEnabled(w))return 0;
        const int id=static_cast<int>(wp);const uint16_t binding=id==app.pauseHotkeyId?app.pauseHotkey:id==app.stopHotkeyId?app.stopHotkey:0;
        if(!binding || HIWORD(lp)!=LOBYTE(binding) || LOWORD(lp)!=(hotkeyRegistrationModifiers(binding)&~MOD_NOREPEAT))return 0;
        applyStatus(app.engine->status());
        return windowProc(w,WM_COMMAND,id==app.pauseHotkeyId?Pause:Finish,0);
    }
    case WM_COMMAND: {
        if(app.failureNotice==FailureNotice::Presenting)return 0;
        const int id=LOWORD(wp),code=HIWORD(wp);
        // SS_NOTIFY gives static labels mouse input for hover tooltips. Their
        // STN_CLICKED value equals BN_CLICKED; never treat them as buttons.
        if(lp && (reinterpret_cast<HWND>(lp)==app.statusText || reinterpret_cast<HWND>(lp)==app.nightHint ||
           reinterpret_cast<HWND>(lp)==app.nightDetail || reinterpret_cast<HWND>(lp)==app.skipSummary || reinterpret_cast<HWND>(lp)==app.skipDetail))return 0;
        if(id==SizeBox && code==CBN_DROPDOWN){refreshSizeSuggestions();return 0;}
        if(code==CBN_SELCHANGE){
            if(app.active() && (id==EncodingModeBox || id==NightDurationBox || id==NightTargetBox || id==IntervalBox || id==SizeBox || id==StopAfterBox || id==SegmentBox || id==StartDelayBox)){
                if(id==EncodingModeBox)choose(app.encodingMode,static_cast<int>(app.settings.encodingMode));
                if(id==IntervalBox)choose(app.interval,app.committedInterval);
                if(id==SizeBox)choose(app.videoSize,app.committedSize);
                if(id==StopAfterBox)choose(app.stopAfter,app.committedLimit);
                if(id==StartDelayBox)choose(app.startDelay,app.committedStartDelay);
                if(id==SegmentBox)choose(app.splitEvery,app.committedSegment);
                if(id==NightDurationBox)choose(app.nightDuration,app.committedNightDuration);
                return 0;
            }
            if(id==SizeBox)for(size_t source=0;source<app.sizeSuggestions.size();++source){const auto& suggestion=app.sizeSuggestions[source];if(suggestion.item>=0 && choice(app.videoSize)==suggestion.item){
                SendMessageW(app.videoSize,CB_SHOWDROPDOWN,FALSE,0);std::wstring error;
                if(currentSizeSuggestion(source,suggestion) && validateVideoSize(suggestion.width,suggestion.height,error)){CustomDraft draft;draft.kind=CustomKind::Size;draft.width=suggestion.width;draft.height=suggestion.height;commitCustom(draft);}
                else choose(app.videoSize,app.committedSize);
                return 0;
            }}
            const HWND customBox=id==IntervalBox?app.interval:id==SizeBox?app.videoSize:id==StopAfterBox?app.stopAfter:id==SegmentBox?app.splitEvery:id==NightDurationBox?app.nightDuration:nullptr;
            const int customAction=id==IntervalBox?6+int(app.hasCustomInterval):id==SizeBox?2+int(app.hasCustomSize):id==SegmentBox?5+int(app.hasCustomSegment):id==NightDurationBox?6+int(app.hasCustomNightDuration):6+int(app.hasCustomLimit);
            if(customBox && choice(customBox)==customAction && SendMessageW(customBox,CB_GETCOUNT,0,0)>customAction){
                SendMessageW(customBox,CB_SHOWDROPDOWN,FALSE,0);editCustom(id==IntervalBox?CustomKind::Interval:id==SizeBox?CustomKind::Size:id==SegmentBox?CustomKind::Segment:id==NightDurationBox?CustomKind::Night:CustomKind::Limit);return 0;
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
        case StatusDetails:if(code==BN_CLICKED)showStatusDetails();break;
        case AdvancedToggle:toggleAdvanced();break;
        case SkipConfigure:editSkip();break;
        case WatermarkConfigure:if(code==BN_CLICKED)editWatermark();break;
        case PlaybackConfigure:if(code==BN_CLICKED)editPlayback();break;
        case LowDiskBox:if(!app.active())configure();break;
        case CursorBox:
            if(code!=BN_CLICKED)break;
            if(app.active() || !hasSource(Source::Desktop))SendMessageW(app.captureCursor,BM_SETCHECK,app.settings.captureCursor?BST_CHECKED:BST_UNCHECKED,0);
            else {configure();updateControls();}
            break;
        case RecoveryBox:
            if(app.active())SendMessageW(app.recoveryMode,BM_SETCHECK,app.settings.recoveryMode?BST_CHECKED:BST_UNCHECKED,0);
            else {configure();updateControls();}
            break;
        case NightBox:if(!app.active()){configure();updateControls();layout();revealFocusedControl();}break;
        case TrayShow:showWindow(true);break;
        case TrayPause:case Pause:if(!app.closeWhenDone && (app.status.state==State::Recording || app.status.state==State::Paused))app.engine->setPaused(app.status.state!=State::Paused);break;
        case TrayFinish:case Finish:if(!app.closeWhenDone && (app.status.state==State::Waiting || app.status.state==State::Starting || app.status.state==State::Recording || app.status.state==State::Paused))app.engine->finish();break;
        case TrayExit:exitApplication();break;
        case Refresh:refreshSources();app.engine->refreshSources();updateControls();break;
        case Record:
            if(app.active())break;
            configure();
            if(hasRequiredSources() && app.nightValidation.empty() && app.encodingValidation.empty() && app.watermarkValidation.empty()) {
                // A long session may end without WM_DESTROY after a crash or
                // power loss. Checkpoint accepted options before capture starts.
                // As on shutdown, preference failure must not prevent recording.
                if(app.startupComplete)preferences(true);
                app.recordedOutputFps=app.settings.outputFps;
                app.engine->record();
            }
            applyStatus(app.engine->status(),true);acknowledgeVisibleFailure();break;
        case Folder:selectFolder();break;
        case OpenFolder:startOpenFolder();break;
        case Reset:if(!app.settings.separateFiles){choose(app.mode,static_cast<int>(app.collagePreset));changeLayout(false);}break;
        case Forward:if(!app.settings.separateFiles && app.selected>=0){auto layer=app.settings.layers[app.selected];app.settings.layers.erase(app.settings.layers.begin()+app.selected);app.settings.layers.push_back(layer);app.selected=static_cast<int>(app.settings.layers.size())-1;customized();}break;
        }return 0;
    }
    case WM_NOTIFY:
        if(reinterpret_cast<NMHDR*>(lp)->code==NM_CUSTOMDRAW && buttonLook(reinterpret_cast<NMHDR*>(lp)->hwndFrom)!=ButtonLook::None)
            return drawButton(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
        if(reinterpret_cast<NMHDR*>(lp)->code==TTN_GETDISPINFOW && reinterpret_cast<NMHDR*>(lp)->idFrom==SavePathTip){
            reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText=const_cast<LPWSTR>(app.settings.folder.c_str());return 0;
        }
        if(reinterpret_cast<NMHDR*>(lp)->code==TTN_GETDISPINFOW){
            auto info=reinterpret_cast<NMTTDISPINFOW*>(lp);const auto child=reinterpret_cast<HWND>(info->hdr.idFrom);
            info->lpszText=const_cast<LPWSTR>(child==app.statusText?statusTooltip():child==app.videoSize?videoSizeTooltip():
                (child==app.advanced?app.advancedTooltip:child==app.nightHint?app.nightHintCaption:child==app.nightDetail?app.nightDetailCaption:
                 child==app.skipSummary?app.skipSummaryCaption:child==app.skipDetail?app.skipDetailCaption:statusCaption()).c_str());return 0;
        }break;
    case WM_CTLCOLORSTATIC: {
        const auto child=reinterpret_cast<HWND>(lp);const bool warning=(child==app.statusText && statusCaptionError()) || (child==app.nightHint && !app.nightValidation.empty());
        bool label=child==app.segmentLabel || child==app.startDelayLabel;
        for(HWND value:app.labels)label=label || child==value;
        const bool panel=panelControl(child);
        SetBkColor(reinterpret_cast<HDC>(wp),panel?Panel:Background);SetTextColor(reinterpret_cast<HDC>(wp),warning?Danger:label?Ink:Muted);
        return reinterpret_cast<LRESULT>(panel?app.panelBrush:app.background);
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        // Double-buffer only the dirty rectangle so timer and stats updates never flicker.
        PAINTSTRUCT ps;HDC dc=BeginPaint(w,&ps);const RECT dirty=ps.rcPaint;
        const int width=dirty.right-dirty.left,height=dirty.bottom-dirty.top;
        HDC memory=width>0 && height>0?CreateCompatibleDC(dc):nullptr;HBITMAP bitmap=memory?CreateCompatibleBitmap(dc,width,height):nullptr;
        if(memory && bitmap){
            const auto previous=SelectObject(memory,bitmap);SetViewportOrgEx(memory,-dirty.left,-dirty.top,nullptr);
            paintWindow(memory,dirty);SetViewportOrgEx(memory,0,0,nullptr);
            BitBlt(dc,dirty.left,dirty.top,width,height,memory,0,0,SRCCOPY);SelectObject(memory,previous);
        } else paintWindow(dc,dirty);
        if(bitmap)DeleteObject(bitmap);
        if(memory)DeleteDC(memory);
        EndPaint(w,&ps);return 0;
    }
    case WM_CLOSE:
        hideToTray();return 0;
    case WM_POWERBROADCAST:
        if(wp==PBT_APMSUSPEND || wp==PBT_APMRESUMEAUTOMATIC || wp==PBT_APMRESUMESUSPEND || wp==PBT_APMRESUMECRITICAL) {
            if(app.engine)app.engine->cancelDelayedStart();
            return TRUE;
        }
        break;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        // Confirmed session shutdown reaches WM_DESTROY and joins the engine,
        // giving the encoder a chance to finalize before Windows terminates us.
        if(wp){cancelOpenFolder();cancelOwnedDialogs();DestroyWindow(w);}
        return 0;
    case WM_DESTROY:cancelOpenFolder();removeTray();unregisterRecordingHotkeys();KillTimer(w,1);if(app.startupComplete)preferences(true);app.startupComplete=false;app.engine.reset();PostQuitMessage(0);return 0;
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
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_HOTKEY_CLASS};InitCommonControlsEx(&controls);
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
    app.dpi=static_cast<int>(GetDpiForSystem());
    app.appIcons[0]=createAppIcon(GetSystemMetricsForDpi(SM_CXICON,app.dpi));
    app.appIcons[1]=createAppIcon(GetSystemMetricsForDpi(SM_CXSMICON,app.dpi));
    WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=windowProc;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hIcon=app.appIcons[0]?app.appIcons[0]:LoadIconW(nullptr,IDI_APPLICATION);cls.hIconSm=app.appIcons[1];cls.hbrBackground=app.background;cls.lpszClassName=L"TimelapseWindow";RegisterClassExW(&cls);
    POINT cursor{};GetCursorPos(&cursor);
    const RECT work=workArea(MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY));
    const int margin=app.scale(16);
    const int width=std::min(app.scale(1040),std::max(1,static_cast<int>(work.right-work.left)-2*margin));
    const int height=std::min(app.scale(720),std::max(1,static_cast<int>(work.bottom-work.top)-2*margin));
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

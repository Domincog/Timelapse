#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include "config.h"
#include "time_skip.h"
#include "person_pack.h"
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
enum Id { ModeBox = 100, IntervalBox, SizeBox, EncodingQualityBox, MonitorBox, CameraBox, Refresh, Record, Pause, Finish, Folder, OpenFolder, Reset, Forward, Preview, EncodingModeBox, AdvancedToggle, StopAfterBox, LowDiskBox, NightBox, NightDurationBox, NightTargetBox, NightHint, NightDetail, SkipConfigure, SkipSummary, SkipDetail, RecoveryBox, SegmentBox };
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
constexpr const wchar_t* SkipModeLabels[] = {L"Off",L"Quiet periods",L"Manual schedule",L"Quiet within schedule",L"No person detected (camera)",L"No person within schedule (camera)"};
constexpr int SkipMultipliers[] = {2,4,8,16,32,64}, SkipRamps[] = {15,30,60};
constexpr int SeparateFilesMode = 5;
constexpr wchar_t SeparateFilesLabel[] = L"Desktop + camera (2 files)";
constexpr UINT TrayMessage = WM_APP + 1, ShowExistingMessage = WM_APP + 2;
constexpr UINT TrayShow = 4001, TrayPause = 4002, TrayFinish = 4003, TrayExit = 4004;
constexpr UINT ExitSystemCommand = 0x1000;
constexpr wchar_t InstanceMutexName[] = L"Local\\Timelapse.Application.{DC32D155-1B8D-4880-9902-CE6245D34923}";
constexpr wchar_t SetupMutexName[] = L"Local\\Timelapse.Setup.{DC32D155-1B8D-4880-9902-CE6245D34923}";
enum class FailureNotice { None, Pending, Presenting, Presented };
struct App {
    HWND window{}, preview{}, statusText{}, tooltip{};
    HWND customDialog{};
    HWND skipConfigure{},skipSummary{},skipDetail{};
    HWND mode{}, interval{}, videoSize{}, encodingQuality{}, encodingMode{}, monitor{}, camera{}, refresh{}, record{}, pause{}, finish{}, folder{}, openFolder{}, reset{}, forward{};
    HWND advanced{}, stopAfter{}, lowDisk{}, recoveryMode{}, nightEnabled{}, nightDuration{}, nightTarget{}, nightHint{}, nightDetail{}, labels[10]{};
    HWND segmentLabel{}, splitEvery{};
    HFONT font{}, titleFont{}, smallFont{};
    HBRUSH background = CreateSolidBrush(Background);
    int dpi = 96, selected = -1, modeIndex = 0;
    Mode collagePreset = Mode::Overlay;
    bool dragging = false, resizing = false, closeWhenDone = false, inspectUI = false;
    bool layingOut = false;
    bool startupComplete = false;
    FailureNotice failureNotice = FailureNotice::None;
    bool trayMenuOpen = false, trayMenuCanceled = false;
    bool visibleDirty = true, controlsUpdated = false;
    State controlsState = State::Idle;
    bool advancedExpanded = false;
    int advancedLimitIndex = -1, advancedVisibility = -1, advancedNightState = -1, advancedRecoveryState = -1, nightVisibility = -1;
    int advancedSegmentSeconds = -1;
    int customIntervalMs=5000, customWidth=1280, customHeight=720, customLimitSeconds=900;
    int committedInterval=2, committedSize=0, committedLimit=0;
    bool hasCustomInterval=false, hasCustomSize=false, hasCustomLimit=false;
    int customSegmentSeconds=900, committedSegment=0;
    bool hasCustomSegment=false;
    std::wstring advancedCaption, advancedTooltip;
    std::wstring skipSummaryCaption,skipDetailCaption;
    int skipRevision=0,advancedSkipRevision=-1,skipSummaryRevision=-1,skipVisibility=-1;
    uint64_t skipCheckAge=UINT64_MAX;
    PersonPackInfo personPack{};
    bool personPackKnown=false;
    std::wstring nightValidation, encodingValidation, statusCaption, nightHintCaption, nightDetailCaption;
    std::wstring cameraListError, statusTooltipCaption;
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
void cancelOwnedDialogs() {
    // EndDialog returns before destruction: walk ownership, not the global
    // pointer, so an open range editor and its settings dialog both terminate.
    for(HWND dialog=app.customDialog;dialog && dialog!=app.window;){
        HWND owner=GetWindow(dialog,GW_OWNER);EndDialog(dialog,IDCANCEL);dialog=owner;
    }
}

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
int selectedSegment() {
    const int selected=app.splitEvery?choice(app.splitEvery):0;
    return app.hasCustomSegment && selected==5 ? app.customSegmentSeconds : SegmentDurations[std::clamp(selected,0,4)];
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
    customItems(app.splitEvery,5,app.hasCustomSegment,formatDuration(int64_t(app.customSegmentSeconds)*1000,true),app.committedSegment);
}
void normalizeCustomSelections() {
    if(app.hasCustomInterval)for(int i=0;i<6;++i)if(CaptureIntervals[i]==app.customIntervalMs){app.hasCustomInterval=false;app.committedInterval=i;break;}
    if(app.hasCustomSize && ((app.customWidth==1280 && app.customHeight==720) || (app.customWidth==1920 && app.customHeight==1080))){
        app.hasCustomSize=false;app.committedSize=app.customWidth==1280?0:1;
    }
    if(app.hasCustomLimit)for(int i=1;i<6;++i)if(RecordingLimits[i]==app.customLimitSeconds){app.hasCustomLimit=false;app.committedLimit=i;break;}
    if(app.hasCustomSegment)for(int i=0;i<5;++i)if(SegmentDurations[i]==app.customSegmentSeconds){app.hasCustomSegment=false;app.committedSegment=i;break;}
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
    if(!app.status.error && !app.status.recordingFailed && app.status.savedPath.empty() && app.status.savedPaths.empty() && cameraListUnavailable())
        return app.cameraListError;
    return app.status.message;
}
bool statusCaptionError() {
    return app.status.error || (!app.active() && (!app.nightValidation.empty() || !app.encodingValidation.empty() || &statusCaption()==&app.cameraListError));
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
    const int selection=selectedLimit(),segment=selectedSegment();
    const int night=app.settings.night.enabled?(app.nightValidation.empty()?1:2):0;
    const int recovery=app.encodingValidation.empty()?(app.settings.recoveryMode?1:0):2;
    if(selection!=app.advancedLimitIndex || segment!=app.advancedSegmentSeconds || night!=app.advancedNightState || recovery!=app.advancedRecoveryState || app.advancedSkipRevision!=app.skipRevision){
        std::wstring caption=L"&Advanced";
        if(recovery==2)caption+=L" · check MP4";
        else if(night==2)caption+=L" · check blend";
        else if(night){caption+=L" · night";if(selection)caption+=L", "+(choice(app.stopAfter)<6?std::wstring(RecordingLimitShortLabels[std::clamp(choice(app.stopAfter),0,5)]):formatDuration(int64_t(selection)*1000,true));}
        else if(selection)caption+=L" · stop after "+(choice(app.stopAfter)<6?std::wstring(RecordingLimitLabels[std::clamp(choice(app.stopAfter),0,5)]):formatDuration(int64_t(selection)*1000,true));
        if(recovery==1 && night!=2)caption+=L" · recovery";
        if(skipEnabled() && night!=2 && recovery!=2)caption+=L" · "+std::to_wstring(app.settings.timeSkip.multiplier)+L"×";
        if(segment && night!=2 && recovery!=2)caption+=L" · split "+formatDuration(int64_t(segment)*1000,true);
        RECT bounds{};GetClientRect(app.advanced,&bounds);
        if((selection || segment || skipEnabled() || recovery==1) && night!=2 && recovery!=2 && bounds.right>app.scale(40)) {
            HDC dc=GetDC(app.advanced);if(dc){const auto previous=SelectObject(dc,app.font);SIZE size{};
                GetTextExtentPoint32W(dc,caption.c_str(),static_cast<int>(caption.size()),&size);
                SelectObject(dc,previous);ReleaseDC(app.advanced,dc);
                if(size.cx+app.scale(18)>bounds.right)caption=segment?L"&Advanced · split + options":recovery?L"&Advanced · recovery + options":skipEnabled()?L"&Advanced · "+std::wstring(night?L"night/":L"")+
                    (selection?L"stop/":L"")+std::to_wstring(app.settings.timeSkip.multiplier)+L"×":night?L"&Advanced · night + stop":L"&Advanced · timed stop";
            }
        }
        if(caption!=app.advancedCaption){SetWindowTextW(app.advanced,caption.c_str());app.advancedCaption=caption;}
        app.advancedTooltip=L"Show or hide advanced options. Recording options can be changed before recording. Night mode applies only to camera content.";
        if(selection)app.advancedTooltip+=L" Stop after "+formatDuration(int64_t(selection)*1000)+L" of active recording; pauses and startup do not count.";
        if(segment)app.advancedTooltip+=L" Split files every "+formatDuration(int64_t(segment)*1000)+L" of active recording. Shorter parts add processing and file overhead.";
        if(skipEnabled())app.advancedTooltip+=L" Time compression: "+skipSummary(app.settings.timeSkip,app.settings.intervalMs)+L".";
        if(skipPerson(app.settings.timeSkip.mode))app.advancedTooltip+=L" Person checks use only selected camera content and require the optional detector. Missing, uncertain or stale checks keep the normal capture interval.";
        if(app.settings.recoveryMode)app.advancedTooltip+=L" MP4 recovery mode (H.264) is on; recent frames can still be lost after interruption.";
        if(!app.encodingValidation.empty())app.advancedTooltip+=L" "+app.encodingValidation;
        app.advancedLimitIndex=selection;app.advancedSegmentSeconds=segment;app.advancedNightState=night;app.advancedRecoveryState=recovery;app.advancedSkipRevision=app.skipRevision;
    }
    const int visibleNight=app.advancedExpanded?nightRow():0;
    const int visibleSkip=app.advancedExpanded?(skipEnabled()?2:1):0;
    if(app.advancedVisibility==static_cast<int>(app.advancedExpanded) && app.nightVisibility==visibleNight && app.skipVisibility==visibleSkip)return;
    app.advancedVisibility=static_cast<int>(app.advancedExpanded);
    app.nightVisibility=visibleNight;
    app.skipVisibility=visibleSkip;
    SendMessageW(app.advanced,BM_SETCHECK,app.advancedExpanded?BST_CHECKED:BST_UNCHECKED,0);
    const auto visible=[](HWND child,bool show){
        if(child && ((GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0)!=show)ShowWindow(child,show?SW_SHOWNA:SW_HIDE);
    };
    for(HWND child:{app.labels[6],app.encodingMode,app.labels[7],app.stopAfter,app.lowDisk,app.recoveryMode,app.segmentLabel,app.splitEvery})visible(child,app.advancedExpanded);
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
    const bool customSize=app.hasCustomSize && choice(app.videoSize)==2;
    app.settings.width = customSize?app.customWidth:choice(app.videoSize)==1?1920:1280;
    app.settings.height = customSize?app.customHeight:choice(app.videoSize)==1?1080:720;
    app.committedInterval=std::clamp(choice(app.interval),0,app.hasCustomInterval?6:5);
    app.committedSize=std::clamp(choice(app.videoSize),0,app.hasCustomSize?2:1);
    app.committedLimit=std::clamp(choice(app.stopAfter),0,app.hasCustomLimit?6:5);
    app.settings.encodingQuality = static_cast<EncodingQuality>(std::clamp(choice(app.encodingQuality),0,2));
    app.settings.encodingMode = static_cast<EncodingMode>(std::clamp(choice(app.encodingMode),0,4));
    app.settings.recoveryMode = app.recoveryMode && SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==BST_CHECKED;
    validateEncodingMode(app.settings.encodingMode,app.settings.recoveryMode,app.encodingValidation);
    app.settings.recordingLimitSeconds = selectedLimit();
    app.settings.segmentDurationSeconds = selectedSegment();
    app.committedSegment=app.splitEvery?std::clamp(choice(app.splitEvery),0,app.hasCustomSegment?5:4):0;
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
    for (auto control : {app.mode,app.interval,app.videoSize,app.encodingQuality,app.encodingMode,app.stopAfter,app.splitEvery,app.lowDisk,app.recoveryMode,app.nightEnabled,app.nightDuration,app.nightTarget,app.refresh,app.folder}) EnableWindow(control,idle);
    EnableWindow(app.monitor,idle && hasSource(Source::Desktop));
    EnableWindow(app.camera,idle && hasSource(Source::Camera) && !app.cameras.empty());
    EnableWindow(app.record,idle && hasRequiredSources() && app.nightValidation.empty() && app.encodingValidation.empty());
    EnableWindow(app.pause,app.status.state==State::Recording || app.status.state==State::Paused);
    EnableWindow(app.finish,app.status.state==State::Starting || app.status.state==State::Recording || app.status.state==State::Paused);
    if(!app.controlsUpdated || (app.controlsState==State::Paused)!=(app.status.state==State::Paused))
        SetWindowTextW(app.pause,app.status.state==State::Paused ? L"&Resume" : L"&Pause");
    bool collage = !app.settings.separateFiles && app.settings.layers.size() > 1;
    EnableWindow(app.reset,collage); EnableWindow(app.forward,collage && app.selected>=0);
    updateAdvanced();
    updateNightText(!app.controlsUpdated);updateSkipText(!app.controlsUpdated);updateStatusText(!app.controlsUpdated);
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
    const bool state=app.status.state!=value.state;
    const bool stats=state || app.status.frames!=value.frames || app.status.completedSegments!=value.completedSegments ||
        static_cast<uint64_t>(std::max(0.0,app.status.elapsed))!=static_cast<uint64_t>(std::max(0.0,value.elapsed));
    const bool message=app.status.message!=value.message,error=app.status.error!=value.error;
    const bool outcome=app.status.recordingFailed!=value.recordingFailed || app.status.savedPath.empty()!=value.savedPath.empty() ||
        app.status.savedPaths.empty()!=value.savedPaths.empty();
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
        if(force || stats || message || error || outcome || preview || night || skip)app.visibleDirty=true;
        return;
    }
    if(force || app.visibleDirty) {
        updateControls();updateNightText();updateSkipText();updateStatusText();
        InvalidateRect(app.window,nullptr,FALSE);InvalidateRect(app.preview,nullptr,FALSE);InvalidateRect(app.statusText,nullptr,TRUE);
        app.visibleDirty=false;return;
    }
    if(!app.controlsUpdated || app.controlsState!=app.status.state)updateControls();
    if(message || error || outcome || state)updateStatusText();
    if(night)updateNightText();
    if(app.advancedExpanded && skipEnabled()){
        const auto tick=app.status.timeSkip.lastCheckTick,now=GetTickCount64();
        const auto age=tick && now>=tick?(now-tick)/1000:UINT64_MAX;
        if(skip || (app.status.state==State::Recording && skipObserved(app.settings.timeSkip.mode) && age!=app.skipCheckAge))updateSkipText();
    }
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
    if(app.closeWhenDone || app.failureNotice==FailureNotice::Presenting)return;
    cancelOwnedDialogs();
    applyStatus(app.engine->status());
    if(reportRecordingFailure())return;
    if(app.active()) {
        if(MessageBoxW(app.window,L"Finish the current recording and exit Timelapse?",L"Finish recording",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK)return;
        app.closeWhenDone=true;app.engine->finish();EnableWindow(app.window,FALSE);
    } else DestroyWindow(app.window);
}
void trayMenu(POINT at={},bool usePoint=false) {
    if(app.failureNotice==FailureNotice::Presenting || app.trayMenuOpen)return;
    applyStatus(app.engine->status());
    if(app.hiddenToTray && reportRecordingFailure())return;
    HMENU menu=CreatePopupMenu();if(!menu){showWindow();return;}
    AppendMenuW(menu,MF_STRING,TrayShow,L"&Show Timelapse");
    const bool pauseAllowed=!app.closeWhenDone&&(app.status.state==State::Recording||app.status.state==State::Paused);
    AppendMenuW(menu,MF_STRING|(pauseAllowed?MF_ENABLED:MF_GRAYED),TrayPause,app.status.state==State::Paused?L"&Resume recording":L"&Pause recording");
    const bool finishAllowed=!app.closeWhenDone&&(pauseAllowed||app.status.state==State::Starting);
    AppendMenuW(menu,MF_STRING|(finishAllowed?MF_ENABLED:MF_GRAYED),TrayFinish,L"&Finish recording");
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
    const int skipHeight=skipEnabled()?80:52;
    const int previewTop=app.scale(app.advancedExpanded?(night==2?372:night==1?300:258)+skipHeight+58:190);
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
    move(app.recoveryMode,pad,app.scale(271),encodingW,ch);
    move(app.segmentLabel,stopX,app.scale(250),width-encodingW-gap,app.scale(20));move(app.splitEvery,stopX,app.scale(271),width-encodingW-gap,app.scale(210));
    move(app.skipConfigure,pad,app.scale(316),app.scale(202),ch);
    move(app.skipSummary,pad+app.scale(216),app.scale(316),width-app.scale(216),ch);
    move(app.skipDetail,pad,app.scale(352),width,app.scale(21));
    move(app.nightEnabled,pad,app.scale((night==2?336:316)+skipHeight),encodingW,ch);
    move(app.labels[8],stopX,app.scale(315+skipHeight),stopW,app.scale(20));move(app.nightDuration,stopX,app.scale(336+skipHeight),stopW,app.scale(210));
    move(app.labels[9],diskX,app.scale(315+skipHeight),options-encodingW-stopW,app.scale(20));move(app.nightTarget,diskX,app.scale(336+skipHeight),options-encodingW-stopW,app.scale(150));
    move(app.nightHint,pad,app.scale(371+skipHeight),width,app.scale(21));move(app.nightDetail,pad,app.scale(395+skipHeight),width,app.scale(21));
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
    if(app.advancedExpanded)for(HWND child:{app.encodingMode,app.stopAfter,app.splitEvery,app.lowDisk,app.recoveryMode,app.skipConfigure,app.nightEnabled,app.nightDuration,app.nightTarget})
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
    for(HWND box:{app.mode,app.interval,app.videoSize,app.encodingQuality,app.encodingMode,app.stopAfter,app.splitEvery,app.nightDuration,app.nightTarget,app.monitor,app.camera})
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
    for(HWND child:{app.statusText,app.nightHint,app.nightDetail,app.skipDetail})if(child)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
}

enum class CustomKind { Interval, Size, Limit, Range, Segment };
enum CustomId { CustomFirst=5001, CustomSecond, CustomUnits, CustomHelp, CustomError, CustomFirstLabel, CustomSecondLabel };
struct CustomDraft {
    CustomKind kind=CustomKind::Interval;
    int64_t durationMs=5000;
    int width=1280,height=720,dpi=96,scrollX=0,scrollY=0;
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
            draft=reinterpret_cast<CustomDraft*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);draft->previousDialog=app.customDialog;app.customDialog=window;
            draft->dpi=static_cast<int>(GetDpiForWindow(window));if(draft->dpi<=0)draft->dpi=app.dpi;
            const bool range=draft->kind==CustomKind::Range,size=draft->kind==CustomKind::Size,interval=draft->kind==CustomKind::Interval;
            SetWindowTextW(window,range?L"Compression range":size?L"Custom video size":interval?L"Custom capture interval":draft->kind==CustomKind::Segment?L"Custom file split":L"Custom stop time");
            const auto child=[&](const wchar_t* type,const wchar_t* text,DWORD style,int id){return CreateWindowExW(std::wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,
                type,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);};
            draft->firstLabel=child(L"STATIC",range?L"&Start after (seconds)":size?L"&Width (pixels)":L"&Value",0,CustomFirstLabel);
            draft->first=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,CustomFirst);
            draft->secondLabel=child(L"STATIC",range?L"&End after (seconds)":size?L"&Height (pixels)":L"&Units",0,CustomSecondLabel);
            if(size || range)draft->second=child(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,CustomSecond);
            else {draft->units=child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,CustomUnits);
                for(const auto* unit:{L"Seconds",L"Minutes",L"Hours"})add(draft->units,unit);if(!interval)add(draft->units,L"Days");choose(draft->units,0);}
            draft->help=child(L"STATIC",range?L"Use whole active seconds from 0 to 2,147,483,647. Pauses and initial preparation do not count. Start is included; end is excluded. Overlapping or touching ranges merge when you return to the schedule.":size?L"Use even dimensions from 48 to 4096 pixels, at most 8,847,360 pixels total. Sources fit inside the video without stretching.":
                interval?L"Choose 0.1 seconds to 24 hours. Decimals use a point and must resolve to whole milliseconds. Playback stays at 30 fps.":
                draft->kind==CustomKind::Segment?L"Choose 1 to 2,147,483,647 whole seconds of active recording per part. Pauses and initial preparation do not count; automatic saving does. Empty periods create no files. Shorter parts add processing and file overhead.":
                L"Choose 1 to 2,147,483,647 seconds of active recording. Pauses and initial preparation do not count. Decimals must resolve to whole seconds.",SS_NOPREFIX,CustomHelp);
            draft->error=child(L"STATIC",L"",SS_NOPREFIX,CustomError);
            draft->okay=child(L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON|BS_NOTIFY,IDOK);draft->cancel=child(L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON|BS_NOTIFY,IDCANCEL);
            if(!draft->firstLabel || !draft->secondLabel || !draft->first || !((size || range)?draft->second:draft->units) || !draft->help || !draft->error || !draft->okay || !draft->cancel){EndDialog(window,-1);return TRUE;}
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
    else {app.customLimitSeconds=static_cast<int>(draft.durationMs/1000);app.hasCustomLimit=true;app.committedLimit=6;
        for(int i=1;i<6;++i)if(RecordingLimits[i]==app.customLimitSeconds)app.committedLimit=i;}
    normalizeCustomSelections();customItems();configure();updateControls();layout();InvalidateRect(app.preview,nullptr,FALSE);InvalidateRect(app.window,nullptr,FALSE);
}
void editCustom(CustomKind kind) {
    if(app.active() || app.customDialog)return;
    HWND box=kind==CustomKind::Interval?app.interval:kind==CustomKind::Size?app.videoSize:kind==CustomKind::Segment?app.splitEvery:app.stopAfter;
    choose(box,kind==CustomKind::Interval?app.committedInterval:kind==CustomKind::Size?app.committedSize:kind==CustomKind::Segment?app.committedSegment:app.committedLimit);
    const int duration=kind==CustomKind::Segment?app.settings.segmentDurationSeconds:app.settings.recordingLimitSeconds;
    CustomDraft draft;draft.kind=kind;draft.durationMs=kind==CustomKind::Interval?app.settings.intervalMs:int64_t(duration?duration:900)*1000;
    draft.width=app.settings.width;draft.height=app.settings.height;CustomTemplate resource;
    const auto outcome=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&resource.dialog,app.window,customProc,reinterpret_cast<LPARAM>(&draft));
    if(!IsWindow(app.window))return;
    if(outcome==IDOK && !app.active())commitCustom(draft);
    else if(outcome==-1)MessageBoxW(app.window,L"The custom settings dialog could not be opened. Try again.",L"Timelapse",MB_OK|MB_ICONERROR);
    if(!app.closeWhenDone){SetFocus(box);revealFocusedControl();}
}
enum SkipId { SkipMode=5101,SkipSpeed,SkipRamp,SkipQuiet,SkipQuietUnits,SkipRanges,SkipAdd,SkipEdit,SkipRemove,SkipRepeat,SkipRepeatUnits,SkipHelp,SkipError,SkipPackInfo,SkipPackManage };
struct SkipDraft : CustomDraft {
    TimeSkipSettings policy;
    bool readOnly=false,repeatCleared=false,packChecked=false;
    int naturalHeight=0;
    HWND mode{},speed{},ramp{},quiet{},quietUnits{},ranges{},add{},edit{},remove{},repeat{},repeatUnits{};
    HWND packInfo{},packManage{};
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
    const bool enabled=mode!=TimeSkipMode::Off,automatic=skipObserved(mode),scheduled=skipScheduled(mode),person=skipPerson(mode);
    const auto visible=[](HWND child,bool show){if(child && ((GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0)!=show)ShowWindow(child,show?SW_SHOWNA:SW_HIDE);};
    for(HWND child:{draft.labels[1],draft.speed,draft.labels[2],draft.ramp})visible(child,enabled);
    for(HWND child:{draft.labels[3],draft.quiet,draft.labels[7],draft.quietUnits})visible(child,automatic);
    for(HWND child:{draft.labels[4],draft.ranges,draft.add,draft.edit,draft.remove,draft.labels[5],draft.repeat,draft.labels[6],draft.repeatUnits})visible(child,scheduled);
    for(HWND child:{draft.packInfo,draft.packManage})visible(child,person);
    RECT client{};GetClientRect(window,&client);const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    const int barW=GetSystemMetricsForDpi(SM_CXVSCROLL,draft.dpi),barH=GetSystemMetricsForDpi(SM_CYHSCROLL,draft.dpi);
    const int availableW=client.right+((style&WS_VSCROLL)?barW:0),availableH=client.bottom+((style&WS_HSCROLL)?barH:0);
    const int pad=draft.scale(18),gap=draft.scale(14),minimumWidth=draft.scale(360);
    const int quietTop=134,scheduleTop=automatic?196:134,repeatTop=scheduleTop+160;
    const int helpTop=!enabled?78:scheduled?repeatTop+68:automatic?200:138;
    const auto wrapped=[&](HWND child,int width,int minimum){wchar_t value[2048]{};GetWindowTextW(child,value,2048);
        RECT r{0,0,std::max(1,width),0};HDC dc=GetDC(window);if(!dc)return minimum;const auto previous=SelectObject(dc,draft.font);
        DrawTextW(dc,value,-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,previous);ReleaseDC(window,dc);return std::max(minimum,int(r.bottom));};
    const int packButtonWidth=draft.scale(146);
    bool horizontal=false,vertical=false;int helpHeight=0,errorHeight=0,errorTop=0,buttonTop=0,minHeight=0,packHeight=0,packExtra=0;
    const auto measure=[&](int width){packHeight=person?wrapped(draft.packInfo,width-2*pad-gap-packButtonWidth,draft.scale(28)):0;
        packExtra=person?packHeight+draft.scale(16):0;
        helpHeight=wrapped(draft.help,width-2*pad,draft.scale(40));errorTop=draft.scale(helpTop)+packExtra+helpHeight+draft.scale(10);
        errorHeight=wrapped(draft.error,width-2*pad,draft.scale(22));buttonTop=errorTop+errorHeight+draft.scale(12);minHeight=buttonTop+draft.scale(44);};
    for(int i=0;i<3;++i){horizontal=availableW-(vertical?barW:0)<minimumWidth;measure(std::max(minimumWidth,availableW-(vertical?barW:0)));vertical=availableH-(horizontal?barH:0)<minHeight;}
    ShowScrollBar(window,SB_HORZ,horizontal);ShowScrollBar(window,SB_VERT,vertical);GetClientRect(window,&client);
    const int width=std::max(minimumWidth,int(client.right));measure(width);const int height=std::max(minHeight,int(client.bottom));
    draft.naturalHeight=minHeight;
    draft.scrollX=std::clamp(draft.scrollX,0,width-int(std::max(1L,client.right)));draft.scrollY=std::clamp(draft.scrollY,0,height-int(std::max(1L,client.bottom)));
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};info.nMax=width-1;info.nPage=std::max(1L,client.right);info.nPos=draft.scrollX;SetScrollInfo(window,SB_HORZ,&info,TRUE);
    info.nMax=height-1;info.nPage=std::max(1L,client.bottom);info.nPos=draft.scrollY;SetScrollInfo(window,SB_VERT,&info,TRUE);
    const int half=(width-2*pad-gap)/2,x2=pad+half+gap;
    const auto move=[&](HWND child,int x,int y,int w,int h){MoveWindow(child,x-draft.scrollX,y-draft.scrollY,w,h,TRUE);};
    move(draft.labels[0],pad,draft.scale(16),width-2*pad,draft.scale(20));move(draft.mode,pad,draft.scale(38),width-2*pad,draft.scale(160));
    move(draft.labels[1],pad,draft.scale(76),half,draft.scale(20));move(draft.speed,pad,draft.scale(98),half,draft.scale(200));
    move(draft.labels[2],x2,draft.scale(76),half,draft.scale(20));move(draft.ramp,x2,draft.scale(98),half,draft.scale(150));
    move(draft.labels[3],pad,draft.scale(quietTop),half,draft.scale(20));move(draft.quiet,pad,draft.scale(quietTop+22),half,draft.scale(28));
    move(draft.labels[7],x2,draft.scale(quietTop),half,draft.scale(20));
    move(draft.quietUnits,x2,draft.scale(quietTop+22),half,draft.scale(150));
    move(draft.packInfo,pad,draft.scale(196),width-2*pad-gap-packButtonWidth,packHeight);
    move(draft.packManage,width-pad-packButtonWidth,draft.scale(196),packButtonWidth,draft.scale(28));
    move(draft.labels[4],pad,draft.scale(scheduleTop)+packExtra,width-2*pad,draft.scale(20));move(draft.ranges,pad,draft.scale(scheduleTop+22)+packExtra,width-2*pad,draft.scale(94));
    move(draft.add,pad,draft.scale(scheduleTop+122)+packExtra,draft.scale(80),draft.scale(28));move(draft.edit,pad+draft.scale(94),draft.scale(scheduleTop+122)+packExtra,draft.scale(80),draft.scale(28));
    move(draft.remove,pad+draft.scale(188),draft.scale(scheduleTop+122)+packExtra,draft.scale(92),draft.scale(28));
    move(draft.labels[5],pad,draft.scale(repeatTop)+packExtra,half,draft.scale(20));move(draft.repeat,pad,draft.scale(repeatTop+22)+packExtra,half,draft.scale(28));
    move(draft.labels[6],x2,draft.scale(repeatTop)+packExtra,half,draft.scale(20));move(draft.repeatUnits,x2,draft.scale(repeatTop+22)+packExtra,half,draft.scale(150));
    move(draft.help,pad,draft.scale(helpTop)+packExtra,width-2*pad,helpHeight);move(draft.error,pad,errorTop,width-2*pad,errorHeight);
    move(draft.okay,width-pad-draft.scale(174),buttonTop,draft.scale(80),draft.scale(28));move(draft.cancel,width-pad-draft.scale(80),buttonTop,draft.scale(80),draft.scale(28));
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
    SetWindowTextW(draft.labels[3],skipPerson(mode)?L"No &person for (active time)":L"&Quiet for (active time)");
    SetWindowTextW(draft.labels[7],skipPerson(mode)?L"Dwell u&nits":L"Quiet u&nits");
    std::wstring text=mode==TimeSkipMode::Off?L"Off keeps the selected Capture every interval throughout the recording.":
        mode==TimeSkipMode::Manual?L"Manual schedule accelerates even when something moves. Ranges use active recording time; pauses and initial preparation do not count. Zero repeat means one schedule. Touching or overlapping ranges merge.":
        skipPerson(mode)?L"Checks only the selected camera. Speeds up after sustained no-person detections, even when other things move. Detected people return to normal speed promptly, interrupting the transition. Checks can miss people; missing, uncertain or stale results keep normal speed. No person checks run while paused. Desktop content is not checked.":
        L"Image changes in either selected source keep normal speed; this does not detect people. Checks are best effort, about once a second; brief or small changes may be missed. Detected activity returns to normal speed promptly, interrupting the transition.";
    if(mode==TimeSkipMode::QuietWithinSchedule)text+=L" Accelerates only when quiet AND inside a range. Checks continue outside ranges. Schedule times exclude pauses and initial preparation; zero repeat means one schedule.";
    if(mode==TimeSkipMode::NoPersonWithinSchedule)text+=L" Accelerates only when absence is qualified AND inside a range. Checks continue outside ranges. Schedule times exclude pauses and initial preparation; zero repeat means one schedule.";
    if(mode!=TimeSkipMode::Off)text+=L" Transitions count saved video frames (30 fps); real recording time depends on the capture interval. Short ranges may reach a lower speed. No intermediate pictures are generated. Both output files share the cadence. Night blends retain their full duration, bounded by the original interval.";
    if(draft.readOnly)text=L"Recording options are frozen for this session. "+text;
    if(draft.repeatCleared)text+=L" The retained repeat was reset to Never because an edited range exceeded it; your ranges are retained.";
    SetWindowTextW(draft.help,text.c_str());
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
    policy.rampFrames=SkipRamps[std::clamp(choice(draft.ramp),0,2)];
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
            label(0,L"&Mode");draft->mode=combo(SkipMode);for(auto value:SkipModeLabels)add(draft->mode,value);choose(draft->mode,static_cast<int>(draft->policy.mode));
            label(1,L"Ma&ximum extra speed");draft->speed=combo(SkipSpeed);for(int value:SkipMultipliers)add(draft->speed,std::to_wstring(value)+L"×");
            label(2,L"&Transition (video time)");draft->ramp=combo(SkipRamp);for(auto value:{L"0.5 s (15 frames)",L"1 s (30 frames)",L"2 s (60 frames)"})add(draft->ramp,value);
            choose(draft->speed,1);choose(draft->ramp,1);
            bool foundSpeed=false;for(int i=0;i<6;++i)if(SkipMultipliers[i]==draft->policy.multiplier){choose(draft->speed,i);foundSpeed=true;}
            if(!foundSpeed){add(draft->speed,std::to_wstring(draft->policy.multiplier)+L"×");choose(draft->speed,6);}
            for(int i=0;i<3;++i)if(SkipRamps[i]==draft->policy.rampFrames)choose(draft->ramp,i);
            label(3,L"&Quiet for (active time)");draft->quiet=skipChild(window,L"EDIT",secondsInput(draft->policy.quietAfterMs).c_str(),WS_TABSTOP|ES_AUTOHSCROLL,SkipQuiet);
            label(7,L"Quiet u&nits");draft->quietUnits=combo(SkipQuietUnits);
            label(4,L"Active-time &ranges (start → end, up to 16)");draft->ranges=skipChild(window,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL,SkipRanges);
            draft->add=button(L"&Add...",SkipAdd);draft->edit=button(L"&Edit...",SkipEdit);draft->remove=button(L"&Remove",SkipRemove);
            label(5,L"Repeat e&very (0 = never)");draft->repeat=skipChild(window,L"EDIT",std::to_wstring(draft->policy.repeatSeconds).c_str(),WS_TABSTOP|ES_AUTOHSCROLL,SkipRepeat);
            label(6,L"&Units");draft->repeatUnits=combo(SkipRepeatUnits);
            for(HWND box:{draft->quietUnits,draft->repeatUnits}){for(auto value:{L"Seconds",L"Minutes",L"Hours",L"Days"})add(box,value);choose(box,0);}
            draft->packInfo=skipChild(window,L"STATIC",L"",SS_NOPREFIX,SkipPackInfo);
            draft->packManage=button(L"Manage &detector...",SkipPackManage);
            draft->help=skipChild(window,L"STATIC",L"",SS_NOPREFIX,SkipHelp);draft->error=skipChild(window,L"STATIC",L"",SS_NOPREFIX,SkipError);
            draft->okay=skipChild(window,L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON|BS_NOTIFY,IDOK);draft->cancel=button(draft->readOnly?L"Close":L"Cancel",IDCANCEL);
            bool okay=true;for(HWND child:{draft->mode,draft->speed,draft->ramp,draft->quiet,draft->quietUnits,draft->ranges,draft->add,draft->edit,draft->remove,draft->repeat,draft->repeatUnits,draft->packInfo,draft->packManage,draft->help,draft->error,draft->okay,draft->cancel})if(!child)okay=false;
            for(HWND child:draft->labels)if(!child)okay=false;if(!okay){EndDialog(window,-1);return TRUE;}
            for(HWND child:{draft->quiet,draft->repeat})SendMessageW(child,EM_SETLIMITTEXT,96,0);
            for(HWND child:{draft->mode,draft->speed,draft->ramp,draft->quiet,draft->quietUnits,draft->repeat,draft->repeatUnits,draft->packManage,draft->okay})EnableWindow(child,!draft->readOnly);
            if(draft->readOnly)ShowWindow(draft->okay,SW_HIDE);skipList(*draft);skipPackInfo(*draft);skipHelp(*draft);customFont(window,*draft);
            RECT rect{0,0,draft->scale(540),draft->scale(skipScheduled(draft->policy.mode)?680:460)};
            AdjustWindowRectExForDpi(&rect,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),draft->dpi);
            RECT owner{};GetWindowRect(app.window,&owner);OffsetRect(&rect,(owner.left+owner.right-(rect.right-rect.left))/2-rect.left,(owner.top+owner.bottom-(rect.bottom-rect.top))/2-rect.top);
            rect=fitWindow(rect,workArea(MonitorFromWindow(app.window,MONITOR_DEFAULTTONEAREST)));SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
            skipLayout(window,*draft);skipFitHeight(window,*draft);SetFocus(draft->readOnly?draft->cancel:draft->mode);skipReveal(window,*draft,GetFocus());return FALSE;
        }
        if(!draft)return FALSE;
        switch(message){
        case WM_SIZE:skipLayout(window,*draft);return TRUE;
        case WM_DPICHANGED:{draft->dpi=HIWORD(wp);customFont(window,*draft);RECT rect=*reinterpret_cast<RECT*>(lp);rect=fitWindow(rect,workArea(MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST)));
            SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);skipLayout(window,*draft);skipReveal(window,*draft,GetFocus());return TRUE;}
        case WM_HSCROLL:case WM_VSCROLL:{const int bar=message==WM_HSCROLL?SB_HORZ:SB_VERT;SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int position=info.nPos;
            switch(LOWORD(wp)){case SB_LINEUP:position-=draft->scale(24);break;case SB_LINEDOWN:position+=draft->scale(24);break;case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:position=info.nTrackPos;break;case SB_TOP:position=0;break;case SB_BOTTOM:position=info.nMax;break;default:return TRUE;}
            (bar==SB_HORZ?draft->scrollX:draft->scrollY)=position;skipLayout(window,*draft);return TRUE;}
        case WM_COMMAND:{const int id=LOWORD(wp),code=HIWORD(wp);
            if(((id==IDOK || id==IDCANCEL || id==SkipAdd || id==SkipEdit || id==SkipRemove || id==SkipPackManage) && code==BN_SETFOCUS) ||
               ((id==SkipQuiet || id==SkipRepeat) && code==EN_SETFOCUS) || (id==SkipRanges && code==LBN_SETFOCUS) ||
               ((id==SkipMode || id==SkipSpeed || id==SkipRamp || id==SkipQuietUnits || id==SkipRepeatUnits) && code==CBN_SETFOCUS)){
                skipReveal(window,*draft,reinterpret_cast<HWND>(lp));return TRUE;
            }
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
constexpr const wchar_t* SkipKeys[]={L"TimeSkipMode",L"TimeSkipMultiplier",L"TimeSkipQuietAfterMs",L"TimeSkipRampFrames",L"TimeSkipRepeatSeconds",L"TimeSkipRanges"};
bool skipInteger(std::wstring_view value,int64_t minimum,int64_t maximum,int64_t& result) {
    if(value.empty() || (value.size()>1 && value.front()==L'0'))return false;
    int64_t parsed=0;
    for(wchar_t c:value){if(c<L'0' || c>L'9' || parsed>(maximum-(c-L'0'))/10)return false;parsed=parsed*10+c-L'0';}
    if(parsed<minimum || parsed>maximum)return false;result=parsed;return true;
}
std::array<std::wstring,6> skipValues(const TimeSkipSettings& policy) {
    std::array<std::wstring,6> values{std::to_wstring(static_cast<int>(policy.mode)),std::to_wstring(policy.multiplier),
        std::to_wstring(policy.quietAfterMs),std::to_wstring(policy.rampFrames),std::to_wstring(policy.repeatSeconds),L""};
    for(unsigned i=0;i<policy.rangeCount;++i){if(i)values[5]+=L";";values[5]+=std::to_wstring(policy.ranges[i].startSeconds)+L":"+std::to_wstring(policy.ranges[i].endSeconds);}
    return values;
}
bool parseSkipValues(const std::array<std::wstring,6>& values,TimeSkipSettings& output) {
    TimeSkipSettings policy;int64_t value=0;
    if(!skipInteger(values[0],0,static_cast<int>(TimeSkipMode::NoPersonWithinSchedule),value))return false;policy.mode=static_cast<TimeSkipMode>(value);
    if(!skipInteger(values[1],2,64,value))return false;policy.multiplier=static_cast<int>(value);
    if(!skipInteger(values[2],1000,int64_t(INT_MAX)*1000,value) || value%1000)return false;policy.quietAfterMs=value;
    if(!skipInteger(values[3],15,60,value))return false;policy.rampFrames=static_cast<int>(value);
    if(!skipInteger(values[4],0,INT_MAX,value))return false;policy.repeatSeconds=static_cast<int>(value);
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
    std::array<std::wstring,6> values;bool complete=true;
    for(size_t i=0;i<values.size();++i){wchar_t value[512]{};
        const DWORD count=GetPrivateProfileStringW(L"Settings",SkipKeys[i],L"?",value,512,path);
        if(count>=511 || std::wcscmp(value,L"?")==0)complete=false;values[i]=value;}
    TimeSkipSettings policy;if(!complete || !parseSkipValues(values,policy))return {};return policy;
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
        const auto segmentDuration=std::to_wstring(selectedSegment());
        auto skipPolicy=app.settings.timeSkip;std::wstring skipError;
        if(!normalizeTimeSkipSettings(skipPolicy,skipError))return false;
        const auto skip=skipValues(skipPolicy);
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
            WritePrivateProfileStringW(L"Settings",L"SegmentDurationSeconds",segmentDuration.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"StopOnLowDiskSpace",!app.lowDisk || SendMessageW(app.lowDisk,BM_GETCHECK,0,0)!=BST_UNCHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"RecoveryMode",app.recoveryMode && SendMessageW(app.recoveryMode,BM_GETCHECK,0,0)==BST_CHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightEnabled",app.nightEnabled && SendMessageW(app.nightEnabled,BM_GETCHECK,0,0)==BST_CHECKED?L"1":L"0",pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightDurationMs",nightDuration.c_str(),pending.path) &&
            WritePrivateProfileStringW(L"Settings",L"NightTargetBrightness",nightTarget.c_str(),pending.path);
        for(size_t i=0;saved && i<skip.size();++i)saved=WritePrivateProfileStringW(L"Settings",SkipKeys[i],skip[i].c_str(),pending.path)!=FALSE;
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
        app.settings.timeSkip=loadSkip(path);++app.skipRevision;
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
            if(selected) {RECT handle=layerGripRect(layer,app.scale(12));HBRUSH h=CreateSolidBrush(RGB(83,229,205));FillRect(mem,&handle,h);DeleteObject(h);}
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
        app.startupComplete=false;app.advancedExpanded=false;app.advancedLimitIndex=app.advancedVisibility=-1;
        app.advancedSegmentSeconds=-1;
        app.failureNotice=FailureNotice::None;
        app.trayMenuOpen=app.trayMenuCanceled=false;
        app.advancedNightState=app.advancedRecoveryState=app.nightVisibility=-1;app.nightValidation.clear();app.encodingValidation.clear();app.statusCaption.clear();app.statusCaptionError=false;app.nightHintCaption.clear();app.nightDetailCaption.clear();
        app.cameraListError.clear();app.statusTooltipCaption.clear();
        app.customDialog=nullptr;app.advancedCaption.clear();app.advancedTooltip.clear();
        app.skipRevision=0;app.advancedSkipRevision=app.skipSummaryRevision=app.skipVisibility=-1;app.skipCheckAge=UINT64_MAX;
        app.skipSummaryCaption.clear();app.skipDetailCaption.clear();app.settings.timeSkip={};
        app.personPack={};app.personPackKnown=false;
        app.hasCustomInterval=app.hasCustomSize=app.hasCustomLimit=false;
        app.customIntervalMs=5000;app.customWidth=1280;app.customHeight=720;app.customLimitSeconds=900;
        app.committedInterval=2;app.committedSize=app.committedLimit=0;
        app.committedSegment=0;app.customSegmentSeconds=900;app.hasCustomSegment=false;
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
        app.recoveryMode=requiredControl(L"BUTTON",L"MP4 recover&y mode (H.264)",WS_TABSTOP|BS_AUTOCHECKBOX,RecoveryBox);
        app.segmentLabel=requiredControl(L"STATIC",L"Split files e&very",0,211);
        app.splitEvery=requiredControl(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,SegmentBox);for(auto label:SegmentLabels)add(app.splitEvery,label);
        app.skipConfigure=button(L"Time &compression...",SkipConfigure);
        app.skipSummary=requiredControl(L"STATIC",L"Off",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,SkipSummary);
        app.skipDetail=requiredControl(L"STATIC",L"",SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,SkipDetail);
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
        SendMessageW(app.skipDetail,WM_SETFONT,reinterpret_cast<WPARAM>(app.smallFont),TRUE);
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
        tip.uId=reinterpret_cast<UINT_PTR>(app.splitEvery);tip.lpszText=const_cast<LPWSTR>(SegmentHelp);
        SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));
        tip.uId=reinterpret_cast<UINT_PTR>(app.recoveryMode);
        tip.lpszText=const_cast<LPWSTR>(L"After an unexpected app exit, completed portions may remain playable in the .recording.mp4 file. Recent frames can be lost; very early interruptions may leave no playable video. H.264 only. Larger files and extra processing; some players may not support this format. Finish normally to save. No recovery guarantee after power loss or drive failure.");
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
        for(HWND child:{app.nightHint,app.nightDetail,app.skipSummary,app.skipDetail}){tip.uId=reinterpret_cast<UINT_PTR>(child);SendMessageW(app.tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tip));}
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
        return 0;
    }
    case WM_COMMAND: {
        if(app.failureNotice==FailureNotice::Presenting)return 0;
        const int id=LOWORD(wp),code=HIWORD(wp);
        if(code==CBN_SELCHANGE){
            if(app.active() && (id==EncodingModeBox || id==NightDurationBox || id==NightTargetBox || id==IntervalBox || id==SizeBox || id==StopAfterBox || id==SegmentBox)){
                if(id==EncodingModeBox)choose(app.encodingMode,static_cast<int>(app.settings.encodingMode));
                if(id==IntervalBox)choose(app.interval,app.committedInterval);
                if(id==SizeBox)choose(app.videoSize,app.committedSize);
                if(id==StopAfterBox)choose(app.stopAfter,app.committedLimit);
                if(id==SegmentBox)choose(app.splitEvery,app.committedSegment);
                return 0;
            }
            const HWND customBox=id==IntervalBox?app.interval:id==SizeBox?app.videoSize:id==StopAfterBox?app.stopAfter:id==SegmentBox?app.splitEvery:nullptr;
            const int customAction=id==IntervalBox?6+int(app.hasCustomInterval):id==SizeBox?2+int(app.hasCustomSize):id==SegmentBox?5+int(app.hasCustomSegment):6+int(app.hasCustomLimit);
            if(customBox && choice(customBox)==customAction && SendMessageW(customBox,CB_GETCOUNT,0,0)>customAction){
                SendMessageW(customBox,CB_SHOWDROPDOWN,FALSE,0);editCustom(id==IntervalBox?CustomKind::Interval:id==SizeBox?CustomKind::Size:id==SegmentBox?CustomKind::Segment:CustomKind::Limit);return 0;
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
        case SkipConfigure:editSkip();break;
        case LowDiskBox:if(!app.active())configure();break;
        case RecoveryBox:
            if(app.active())SendMessageW(app.recoveryMode,BM_SETCHECK,app.settings.recoveryMode?BST_CHECKED:BST_UNCHECKED,0);
            else {configure();updateControls();}
            break;
        case NightBox:if(!app.active()){configure();updateControls();layout();revealFocusedControl();}break;
        case TrayShow:showWindow(true);break;
        case TrayPause:if(!app.closeWhenDone && (app.status.state==State::Recording || app.status.state==State::Paused))app.engine->setPaused(app.status.state!=State::Paused);break;
        case TrayFinish:if(!app.closeWhenDone)app.engine->finish();break;
        case TrayExit:exitApplication();break;
        case Refresh:refreshSources();app.engine->refreshSources();updateControls();break;
        case Record:
            if(app.active())break;
            configure();
            if(hasRequiredSources() && app.nightValidation.empty() && app.encodingValidation.empty()) {
                // A long session may end without WM_DESTROY after a crash or
                // power loss. Checkpoint accepted options before capture starts.
                // As on shutdown, preference failure must not prevent recording.
                if(app.startupComplete)preferences(true);
                app.engine->record();
            }
            applyStatus(app.engine->status(),true);acknowledgeVisibleFailure();break;
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
            info->lpszText=const_cast<LPWSTR>(child==app.statusText?statusTooltip():
                (child==app.advanced?app.advancedTooltip:child==app.nightHint?app.nightHintCaption:child==app.nightDetail?app.nightDetailCaption:
                 child==app.skipSummary?app.skipSummaryCaption:child==app.skipDetail?app.skipDetailCaption:statusCaption()).c_str());return 0;
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
        else if(skipEnabled())detail=L"Base interval "+formatDuration(app.settings.intervalMs)+L" · time compression up to "+std::to_wstring(app.settings.timeSkip.multiplier)+L"×";
        else {wchar_t buf[140];const double videoSeconds=120000.0/std::max(MinCaptureIntervalMs,app.settings.intervalMs);
            swprintf_s(buf,L"  ·  1 hour becomes %.*f seconds of video",videoSeconds<1?3:videoSeconds<10?1:0,videoSeconds);
            detail=L"Every "+formatDuration(app.settings.intervalMs)+buf;}
        if(app.status.completedSegments)detail+=L"  ·  "+std::to_wstring(app.status.completedSegments)+(app.status.completedSegments==1?L" part saved":L" parts saved");
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
        if(wp){cancelOwnedDialogs();DestroyWindow(w);}
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

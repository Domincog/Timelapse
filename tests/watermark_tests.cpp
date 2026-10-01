// Real GDI memory-only rendering; no screen DC, device or application window.
#include "watermark.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
enum class Fault { None, DC, Font, FontSelection, Metrics, BadMetrics, Widths, Bitmap, BitmapSelection, Configure, Measure, Draw, Flush };
struct Owned { HGDIOBJ handle = nullptr; size_t bytes = 0; };
struct Probe {
    Fault fault = Fault::None;
    std::array<Owned, 8> owned{};
    int dcCount = 0, objectCount = 0, createdDC = 0, createdObjects = 0, draws = 0, measures = 0, flushes = 0;
    size_t bytes = 0, peakBytes = 0;
    bool cleanupFailed = false;
    bool hit(Fault selected) { if (fault != selected) return false; fault = Fault::None; return true; }
    void track(HGDIOBJ handle, size_t size) {
        if (!handle) return;
        for (auto& item : owned) if (!item.handle) {
            item = {handle, size}; ++objectCount; ++createdObjects; bytes += size; peakBytes = std::max(bytes, peakBytes); return;
        }
        throw std::runtime_error("Watermark exceeded the owned GDI object bound");
    }
};
thread_local Probe probe;
thread_local bool forbidAllocation = false;
thread_local size_t allocations = 0;
}
void* operator new(std::size_t bytes) {
    if (forbidAllocation) throw std::bad_alloc();
    ++allocations;
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

HDC WINAPI stampCreateDC(HDC source) {
    require(!source, "Watermark requested a source/screen DC");
    if (probe.hit(Fault::DC)) return nullptr;
    HDC dc = CreateCompatibleDC(source);
    if (dc) { ++probe.dcCount; ++probe.createdDC; }
    return dc;
}
HFONT WINAPI stampCreateFont(int h, int w, int escape, int orientation, int weight, DWORD italic,
    DWORD underline, DWORD strike, DWORD charset, DWORD precision, DWORD clip, DWORD quality, DWORD pitch, LPCWSTR face) {
    require(charset == ANSI_CHARSET && quality == ANTIALIASED_QUALITY, "Unexpected font charset or ClearType rendering");
    if (probe.hit(Fault::Font)) return nullptr;
    auto font = CreateFontW(h,w,escape,orientation,weight,italic,underline,strike,charset,precision,clip,quality,pitch,face);
    probe.track(font, 0); return font;
}
HGDIOBJ WINAPI stampSelect(HDC dc, HGDIOBJ object) {
    const auto type = GetObjectType(object);
    if (type == OBJ_FONT && probe.hit(Fault::FontSelection)) return HGDI_ERROR;
    if (type == OBJ_BITMAP && probe.hit(Fault::BitmapSelection)) return HGDI_ERROR;
    return SelectObject(dc, object);
}
BOOL WINAPI stampMetrics(HDC dc, LPTEXTMETRICW metrics) {
    if (probe.hit(Fault::Metrics)) return FALSE;
    const auto result = GetTextMetricsW(dc, metrics);
    if (probe.hit(Fault::BadMetrics)) metrics->tmMaxCharWidth = 100000;
    return result;
}
BOOL WINAPI stampWidths(HDC dc, UINT first, UINT last, LPINT widths) {
    return probe.hit(Fault::Widths) ? FALSE : GetCharWidth32W(dc, first, last, widths);
}
HBITMAP WINAPI stampDib(HDC dc, const BITMAPINFO* info, UINT colors, void** pixels, HANDLE section, DWORD offset) {
    require(!section && !offset && info->bmiHeader.biHeight < 0 && info->bmiHeader.biBitCount == 32, "Unexpected watermark DIB format");
    if (probe.hit(Fault::Bitmap)) return nullptr;
    auto bitmap = CreateDIBSection(dc, info, colors, pixels, section, offset);
    probe.track(bitmap, size_t(info->bmiHeader.biWidth) * size_t(-info->bmiHeader.biHeight) * 4); return bitmap;
}
int WINAPI stampBkMode(HDC dc, int mode) { return probe.hit(Fault::Configure) ? 0 : SetBkMode(dc, mode); }
BOOL WINAPI stampMeasure(HDC dc, LPCWSTR text, int length, LPSIZE extent) {
    ++probe.measures;
    return probe.hit(Fault::Measure) ? FALSE : GetTextExtentPoint32W(dc, text, length, extent);
}
BOOL WINAPI stampDraw(HDC dc, int x, int y, LPCWSTR text, int length) {
    ++probe.draws;
    return probe.hit(Fault::Draw) ? FALSE : TextOutW(dc, x, y, text, length);
}
BOOL WINAPI stampFlush() {
    ++probe.flushes;
    const auto result = GdiFlush(); // Even injected failure drains the real GDI batch.
    return probe.hit(Fault::Flush) ? FALSE : result;
}
BOOL WINAPI stampDelete(HGDIOBJ object) noexcept {
    for (auto& item : probe.owned) if (item.handle == object) {
        const auto result = DeleteObject(object);
        if (result) { --probe.objectCount; probe.bytes -= item.bytes; item = {}; }
        else probe.cleanupFailed = true;
        return result;
    }
    probe.cleanupFailed = true; return FALSE;
}
BOOL WINAPI stampDeleteDC(HDC dc) noexcept {
    const auto result = DeleteDC(dc);
    if (result) --probe.dcCount; else probe.cleanupFailed = true;
    return result;
}

#define CreateCompatibleDC stampCreateDC
#define CreateFontW stampCreateFont
#define SelectObject stampSelect
#define GetTextMetricsW stampMetrics
#define GetCharWidth32W stampWidths
#define CreateDIBSection stampDib
#define SetBkMode stampBkMode
#define GetTextExtentPoint32W stampMeasure
#define TextOutW stampDraw
#define GdiFlush stampFlush
#define DeleteObject stampDelete
#define DeleteDC stampDeleteDC
#include "../src/watermark.cpp"
#undef CreateCompatibleDC
#undef CreateFontW
#undef SelectObject
#undef GetTextMetricsW
#undef GetCharWidth32W
#undef CreateDIBSection
#undef SetBkMode
#undef GetTextExtentPoint32W
#undef TextOutW
#undef GdiFlush
#undef DeleteObject
#undef DeleteDC

namespace {
using namespace lapse;
struct NoAllocation { NoAllocation() { forbidAllocation = true; } ~NoAllocation() { forbidAllocation = false; } };
WatermarkSettings enabled() { WatermarkSettings settings; settings.enabled = true; return settings; }
WatermarkContext context() {
    WatermarkContext value; value.activeMs = 90061000;
    value.recordedLocal = {2026,9,3,30,14,5,6,123}; return value;
}
Frame frame(int width, int height) {
    Frame value{width,height,{}}; value.pixels.resize(size_t(width) * height * 4);
    for (size_t i = 0; i < value.pixels.size(); i += 4) {
        value.pixels[i] = 71; value.pixels[i+1] = 112; value.pixels[i+2] = 153; value.pixels[i+3] = 29;
    }
    return value;
}
void clean() { require(!probe.dcCount && !probe.objectCount && !probe.bytes && !probe.cleanupFailed, "Watermark GDI ownership did not balance"); }
void formatted(const WatermarkSettings& settings, WatermarkContext ctx, const wchar_t* expected) {
    std::array<wchar_t, WatermarkTextCapacity> output{}; std::wstring error;
    require(formatWatermarkText(settings, ctx, output, error) && output == [&] {
        std::array<wchar_t, WatermarkTextCapacity> wanted{}; wcscpy_s(wanted.data(), wanted.size(), expected); return wanted;
    }(), "Unexpected watermark formatting");
}
void formatting() {
    auto settings = enabled(); auto ctx = context();
    formatted(settings,ctx,L"Elapsed 1d 01:01:01\nTarget 150x");
    ctx.activeMs = 999; ctx.targetIntervalMs = 101; formatted(settings,ctx,L"Elapsed 00:00:00\nTarget 3.03x");
    ctx.activeMs = 86399999; ctx.targetIntervalMs = 110; formatted(settings,ctx,L"Elapsed 23:59:59\nTarget 3.3x");
    ctx.activeMs = 999999LL*86400000+86399999; ctx.targetIntervalMs = WatermarkMaxIntervalMs;
    formatted(settings,ctx,L"Elapsed 999999d 23:59:59\nTarget 165888000x");
    ctx.activeMs = (std::numeric_limits<int64_t>::max)(); ctx.targetIntervalMs = WatermarkMaxIntervalMs-1;
    formatted(settings,ctx,L"Elapsed >999999d\nTarget 165887999.97x");
    settings.timeKind = WatermarkTimeKind::RecordedLocal;
    formatted(settings,ctx,L"Recorded 2026-09-30 14:05:06\nTarget 165887999.97x");
    settings.showSpeed = false; ctx.recordedLocal = {2000,2,0,29,23,59,59,999};
    formatted(settings,ctx,L"Recorded 2000-02-29 23:59:59");
    ctx.recordedLocal = {1601,1,0,1,0,0,0,0}; formatted(settings,ctx,L"Recorded 1601-01-01 00:00:00");
    ctx.recordedLocal = {9999,12,0,31,23,59,59,0}; formatted(settings,ctx,L"Recorded 9999-12-31 23:59:59");
    settings.showTime = false; settings.showSpeed = true; ctx.activeMs = -1; ctx.recordedLocal = {}; ctx.targetIntervalMs = 100;
    formatted(settings,ctx,L"Target 3x");
    ctx.outputFps = 1; ctx.targetIntervalMs = 101; formatted(settings,ctx,L"Target 0.101x");
    ctx.outputFps = 24; formatted(settings,ctx,L"Target 2.424x");
    ctx.outputFps = 60; formatted(settings,ctx,L"Target 6.06x");
    ctx.outputFps = 120; ctx.targetIntervalMs = WatermarkMaxIntervalMs-1;
    formatted(settings,ctx,L"Target 663551999.88x");
    ctx.outputFps = 119; formatted(settings,ctx,L"Target 658022399.881x");
    settings.enabled = false; ctx.targetIntervalMs = -1; formatted(settings,ctx,L"");
}
void invalidFormatting() {
    std::wstring error; auto settings = enabled(); auto ctx = context();
    std::array<wchar_t, WatermarkTextCapacity> output{}; output[0] = L'!'; const auto original = output;
    const auto rejected = [&] { require(!formatWatermarkText(settings,ctx,output,error) && !error.empty() && output == original, "Invalid formatting changed output or succeeded"); };
    ctx.activeMs = -1; rejected(); ctx = context();
    for (int64_t interval : {int64_t(-1),int64_t(0),int64_t(99),WatermarkMaxIntervalMs+1,(std::numeric_limits<int64_t>::max)()}) { ctx.targetIntervalMs = interval; rejected(); }
    ctx = context();
    for (int fps : {INT_MIN,-1,0,121,INT_MAX}) { ctx.outputFps = fps; rejected(); }
    ctx = context(); settings.timeKind = WatermarkTimeKind::RecordedLocal;
    for (SYSTEMTIME bad : {SYSTEMTIME{},SYSTEMTIME{1900,2,0,29,0,0,0,0},SYSTEMTIME{2026,4,0,31,0,0,0,0},
        SYSTEMTIME{10000,1,0,1,0,0,0,0},SYSTEMTIME{2026,1,0,1,24,0,0,0},SYSTEMTIME{2026,1,0,1,0,60,0,0},
        SYSTEMTIME{2026,1,0,1,0,0,60,0},SYSTEMTIME{2026,1,0,1,0,0,0,1000}}) { ctx.recordedLocal = bad; rejected(); }
    settings = enabled(); ctx = context();
    for (int coordinate : {-1,10001}) { settings.x = coordinate; rejected(); settings.x = 0; settings.y = coordinate; rejected(); settings.y = 0; }
    settings.timeKind = static_cast<WatermarkTimeKind>(55); rejected(); settings = enabled();
    settings.textSize = static_cast<WatermarkTextSize>(-1); rejected(); settings = enabled();
    settings.showTime = settings.showSpeed = false; rejected();
    require(!validateWatermarkSettings(settings,error), "Empty enabled watermark accepted");
    settings.enabled = false; require(validateWatermarkSettings(settings,error), "Disabled retained empty fields rejected");
    auto other = settings; require(sameWatermarkSettings(settings,other), "Equal watermark settings differ");
    other.x = 12; require(!sameWatermarkSettings(settings,other), "Position equality missed");
}
void offAndCold() {
    clean(); const int dcs = probe.createdDC, objects = probe.createdObjects;
    auto value = frame(48,48); const auto before = value.pixels; std::wstring error; auto ctx = context();
    { NoAllocation noAllocation; WatermarkRenderer renderer;
      require(renderer.apply(value,ctx,error), "Unprepared renderer failed");
      require(renderer.prepare({},0,-1,error), "Off preparation failed");
      require(renderer.apply(value,ctx,error), "Off renderer failed"); renderer.reset(); renderer.reset(); }
    require(before == value.pixels && probe.createdDC == dcs && probe.createdObjects == objects, "Disabled renderer allocated resources or changed pixels"); clean();
}
void pixels(const Frame& value, const RECT& bounds) {
    require(bounds.right > bounds.left && bounds.bottom > bounds.top && bounds.left >= 0 && bounds.top >= 0 && bounds.right <= value.width && bounds.bottom <= value.height,
        "Stamp bounds are invalid");
    size_t bright = 0, dark = 0;
    for (int y = 0; y < value.height; ++y) for (int x = 0; x < value.width; ++x) {
        const auto* p = value.pixels.data() + (size_t(y)*value.width+x)*4;
        if (x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom) {
            require(p[0] == p[1] && p[1] == p[2] && p[3] == 255, "Stamp is not grayscale opaque BGRA");
            bright += p[0] >= 180; dark += p[0] == 20;
        } else require(p[0] == 71 && p[1] == 112 && p[2] == 153 && p[3] == 29, "Watermark modified pixels outside its bounds");
    }
    require(bright > 3 && dark > bright, "Rendered text or dark backing is missing");
}
void geometry() {
    auto settings = enabled(); std::wstring error;
    for (const auto dimensions : {std::pair<int,int>{1280,720},{3840,2160},{1080,1920},{480,360},{4096,48}}) {
        for (auto size : {WatermarkTextSize::Small,WatermarkTextSize::Medium,WatermarkTextSize::Large}) {
            settings.textSize = size;
            // Deliberately thin output cannot hold two large lines; preflight must reject.
            WatermarkRenderer renderer;
            const bool ready = renderer.prepare(settings,dimensions.first,dimensions.second,error);
            if (dimensions.second == 48 && size == WatermarkTextSize::Large) { require(!ready, "Thin output unexpectedly passed full longest-text preflight"); continue; }
            if (dimensions.second == 48 && size == WatermarkTextSize::Medium && !ready) {
                require(error.find(L"does not fit") != std::wstring::npos, "Unexpected thin-output error"); continue;
            }
            if (!ready) std::wcerr << L"Rejected geometry " << dimensions.first << L'x' << dimensions.second << L" size " << int(size) << L": " << error << L'\n';
            require(ready, "Common output geometry rejected");
            for (const auto pos : {std::pair<int,int>{0,0},{10000,0},{0,10000},{10000,10000},{3721,6284}}) {
                settings.x = pos.first; settings.y = pos.second;
                const int creations = probe.createdObjects;
                require(renderer.prepare(settings,dimensions.first,dimensions.second,error) && creations == probe.createdObjects, "Position update rebuilt resources");
                auto value = frame(dimensions.first,dimensions.second);
                require(renderer.apply(value,context(),error), "Geometry stamping failed"); const auto bounds = renderer.lastBounds(); pixels(value,bounds);
                if (!pos.first) require(bounds.left >= 2 && bounds.left <= 21, "Left margin mismatch");
                if (!pos.second) require(bounds.top >= 2 && bounds.top <= 21, "Top margin mismatch");
                if (pos.first == 10000) require(value.width - bounds.right >= 2 && value.width - bounds.right <= 21, "Right margin mismatch");
            }
            renderer.reset(); clean();
        }
    }
    settings = enabled();
    for (const auto dimensions : {std::pair<int,int>{48,48},{48,4096},{1279,720},{0,720},{4096,4096}}) {
        WatermarkRenderer renderer; require(!renderer.prepare(settings,dimensions.first,dimensions.second,error) && !error.empty(), "Invalid or too-small watermark output accepted"); clean();
    }
}
void scalingAndReuse() {
    WatermarkRenderer renderer; auto settings = enabled(); std::wstring error; auto ctx = context();
    require(renderer.prepare(settings,1280,720,error), "Preview renderer prepare failed");
    auto full = frame(1280,720); require(renderer.apply(full,ctx,error), "Full render failed"); const auto fullBounds = renderer.lastBounds();
    const int drawCalls = probe.draws, measureCalls = probe.measures, creationCalls = probe.createdObjects;
    auto preview = frame(640,360); require(renderer.apply(preview,ctx,error), "Preview render failed"); const auto previewBounds = renderer.lastBounds(); pixels(preview,previewBounds);
    require(previewBounds.left == fullBounds.left/2 && previewBounds.top == fullBounds.top/2 &&
        previewBounds.right == (fullBounds.right+1)/2 && previewBounds.bottom == (fullBounds.bottom+1)/2, "Preview geometry did not follow logical full-output bounds");
    require(probe.draws == drawCalls && probe.measures == measureCalls && probe.createdObjects == creationCalls, "Same text/preview rerasterized or recreated resources");
    const size_t before = allocations;
    { NoAllocation noAllocation;
      for (int i=0;i<100;++i) { ctx.activeMs += 1000; ctx.targetIntervalMs = 5000+i; require(renderer.apply(full,ctx,error), "Warmed changing render failed"); }
      require(renderer.prepare(settings,1280,720,error), "Identical warm preparation failed"); }
    require(allocations == before && probe.createdObjects == creationCalls && probe.dcCount == 1 && probe.objectCount == 2, "Warmed renderer allocated or exceeded resource bound");
    renderer.reset(); clean();
}
void longestText() {
    WatermarkRenderer renderer; auto settings = enabled(); auto ctx = context(); std::wstring error;
    for (auto kind : {WatermarkTimeKind::ActiveElapsed,WatermarkTimeKind::RecordedLocal}) {
        settings.timeKind = kind; ctx.activeMs = 999999LL*86400000+86399999;
        ctx.recordedLocal = {9999,12,0,31,23,59,59,999}; ctx.targetIntervalMs = WatermarkMaxIntervalMs-1; ctx.outputFps = 119;
        require(renderer.prepare(settings,1280,720,error), "Longest-text preflight failed");
        auto value = frame(1280,720); require(renderer.apply(value,ctx,error), "Preflight did not reserve the actual longest supported text"); pixels(value,renderer.lastBounds());
    }
    // Largest even shorter edge under the public 4K-area budget. Recorded
    // local text + Large exercises this machine's maximum retained tile.
    settings.textSize = WatermarkTextSize::Large;
    require(renderer.prepare(settings,2974,2974,error), "Maximum shorter-edge preflight failed");
    auto largest = frame(2974,2974); require(renderer.apply(largest,ctx,error), "Maximum text tile render failed"); pixels(largest,renderer.lastBounds());
    require(probe.bytes <= 4*1024*1024, "Prepared tile exceeded its byte bound");
    renderer.reset(); clean();
}
void faults() {
    std::wstring error;
    for (auto fault : {Fault::DC,Fault::Font,Fault::FontSelection,Fault::Metrics,Fault::BadMetrics,Fault::Widths,Fault::Bitmap,Fault::BitmapSelection,Fault::Configure}) {
        WatermarkRenderer renderer; probe.fault = fault;
        require(!renderer.prepare(enabled(),1280,720,error) && !error.empty() && probe.fault == Fault::None, "Injected preparation failure was ignored");
        clean(); require(renderer.prepare(enabled(),1280,720,error), "Renderer did not recover after preparation failure"); renderer.reset(); clean();
    }
    WatermarkRenderer renderer;
    require(renderer.prepare(enabled(),1280,720,error), "Failure renderer preparation failed");
    auto value = frame(1280,720); const auto before = value.pixels; auto ctx = context();
    for (auto fault : {Fault::Measure,Fault::Draw,Fault::Flush}) {
        probe.fault = fault; ctx.activeMs += 1000;
        require(!renderer.apply(value,ctx,error) && !error.empty() && value.pixels == before && probe.fault == Fault::None, "Drawing failure changed frame or succeeded");
    }
    ctx.activeMs = -1; require(!renderer.apply(value,ctx,error) && value.pixels == before, "Context failure modified frame");
    Frame bad; require(!renderer.apply(bad,context(),error) && bad.pixels.empty(), "Invalid frame succeeded");
    require(renderer.apply(value,context(),error), "Renderer did not recover after drawing failure"); pixels(value,renderer.lastBounds());
    const auto previous = renderer.lastBounds(); ctx.activeMs = -1;
    require(!renderer.apply(value,ctx,error), "Context failure after success was ignored");
    const auto unchanged = renderer.lastBounds();
    require(std::memcmp(&previous,&unchanged,sizeof(RECT)) == 0, "Failed render changed last successful bounds");
    renderer.reset(); clean();
    error.reserve(256);
    { NoAllocation noAllocation; require(!renderer.prepare(enabled(),1280,720,error) && !error.empty(), "Allocation failure escaped prepare"); }
    clean();
}
void independentThreads() {
    bool okay[2] = {false,false};
    std::thread a([&] { try { WatermarkRenderer renderer; std::wstring error; auto value=frame(1280,720);
        require(renderer.prepare(enabled(),1280,720,error) && renderer.apply(value,context(),error), "Thread rendering failed"); renderer.reset(); clean(); okay[0]=true; } catch (...) {} });
    std::thread b([&] { try { WatermarkRenderer renderer; std::wstring error; auto value=frame(1280,720);
        require(renderer.prepare(enabled(),1280,720,error) && renderer.apply(value,context(),error), "Thread rendering failed"); renderer.reset(); clean(); okay[1]=true; } catch (...) {} });
    a.join(); b.join(); require(okay[0] && okay[1], "Independent renderers failed or shared thread resources");
}
uint64_t cpuTicks() {
    FILETIME create{},exit{},kernel{},user{}; require(GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernel,&user)!=FALSE,"Process CPU query failed");
    return (uint64_t(kernel.dwHighDateTime)<<32 | kernel.dwLowDateTime)+(uint64_t(user.dwHighDateTime)<<32 | user.dwLowDateTime);
}
void benchmark() {
    std::cout << "[\n"; bool comma=false;
    for (const auto dimensions : {std::pair<int,int>{1280,720},{3840,2160}}) for (bool changing : {false,true}) {
        WatermarkRenderer renderer; std::wstring error; auto settings=enabled(); settings.timeKind=WatermarkTimeKind::RecordedLocal;
        auto value=frame(dimensions.first,dimensions.second); auto ctx=context();
        require(renderer.prepare(settings,value.width,value.height,error) && renderer.apply(value,ctx,error),"Benchmark preparation failed");
        const auto beforeAllocation=allocations; const auto bytes=probe.bytes;
        const int iterations=changing ? 1000 : 10000;
        const auto start=std::chrono::steady_clock::now(); const auto cpu=cpuTicks();
        { NoAllocation noAllocation; for (int i=0;i<iterations;++i) {
            if (changing) { ctx.recordedLocal.wSecond=WORD(i%60); ctx.targetIntervalMs=5000+i; }
            require(renderer.apply(value,ctx,error),"Benchmark rendering failed");
        } }
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        const auto cpuMs=double(cpuTicks()-cpu)/10000;
        const auto renderAllocations=allocations-beforeAllocation;
        if (comma) std::cout << ",\n"; comma=true;
        std::cout << "{\"width\":"<<value.width<<",\"height\":"<<value.height<<",\"changing\":"<<(changing?"true":"false")
            <<",\"iterations\":"<<iterations<<",\"wallMs\":"<<elapsed<<",\"cpuMs\":"<<cpuMs<<",\"tileBytes\":"<<bytes
            <<",\"ownedGdiObjects\":"<<probe.objectCount+probe.dcCount<<",\"cppAllocations\":"<<renderAllocations<<"}";
        renderer.reset(); clean();
    }
    std::cout << "\n]\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc==2 && std::strcmp(argv[1],"--benchmark")==0) { benchmark(); return 0; }
        formatting(); std::cout << "PASS formatting\n";
        invalidFormatting(); std::cout << "PASS invalid contexts and settings\n";
        offAndCold(); std::cout << "PASS cold/off zero resources and mutation\n";
        geometry(); std::cout << "PASS geometry, positions and exact mutation bounds\n";
        scalingAndReuse(); std::cout << "PASS preview mapping, reuse and zero warmed C++ allocations\n";
        longestText(); std::cout << "PASS longest supported text\n";
        faults(); std::cout << "PASS allocation/GDI failures and recovery\n";
        independentThreads(); std::cout << "PASS independent thread ownership\n";
        clean(); std::cout << "PASS retained-tile peak bytes " << probe.peakBytes << "\n"; return 0;
    } catch (const std::exception& error) { forbidAllocation=false; std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

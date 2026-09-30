// Real capture/cache/GDI cleanup with deterministic offscreen pixels only.
#include "capture.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct OwnedSurface { HDC dc = nullptr; HBITMAP bitmap = nullptr; size_t bytes = 0; };
enum class PrepareFailure { None, DC, Dib, Selection };
struct CaptureProbe {
    std::array<OwnedSurface, 3> surfaces{};
    int sourceWidth = 4096, sourceHeight = 2160;
    PrepareFailure failure = PrepareFailure::None;
    unsigned preparing = 0;
    int createdDC = 0, deletedDC = 0, createdDib = 0, deletedDib = 0, copies = 0;
    bool cleanupFailed = false;
    size_t bytes() const { return surfaces[0].bytes + surfaces[1].bytes + surfaces[2].bytes; }
};
thread_local CaptureProbe* probe = nullptr;
thread_local bool forbidAllocation = false;
HDC sourceDC = nullptr;
}
void* operator new(std::size_t bytes) {
    if (forbidAllocation) throw std::bad_alloc();
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

HDC WINAPI cacheGetDC(HWND window) { require(!window && sourceDC && probe, "Unexpected desktop DC request"); return sourceDC; }
int WINAPI cacheReleaseDC(HWND window, HDC dc) { require(!window && dc == sourceDC, "Unexpected desktop DC release"); return 1; }
int WINAPI cacheMetrics(int key) {
    if (key == SM_XVIRTUALSCREEN || key == SM_YVIRTUALSCREEN) return 0;
    if (key == SM_CXVIRTUALSCREEN) return probe->sourceWidth;
    if (key == SM_CYVIRTUALSCREEN) return probe->sourceHeight;
    throw std::runtime_error("Unexpected desktop geometry request");
}
BOOL WINAPI cacheCursor(PCURSORINFO) { throw std::runtime_error("Unexpected physical cursor request"); }
HDC WINAPI cacheCreateDC(HDC source) {
    require(source == sourceDC && probe, "Unexpected compatible DC source");
    if (probe->failure == PrepareFailure::DC) { probe->failure = PrepareFailure::None; return nullptr; }
    for (unsigned i = 0; i < probe->surfaces.size(); ++i) if (!probe->surfaces[i].dc) {
        const auto dc = CreateCompatibleDC(source);
        if (dc) { probe->surfaces[i].dc = dc; probe->preparing = i; ++probe->createdDC; }
        return dc;
    }
    throw std::runtime_error("Desktop cache exceeded its three-surface bound");
}
HBITMAP WINAPI cacheCreateDib(HDC source, const BITMAPINFO* info, UINT mode, void** pixels, HANDLE section, DWORD offset) {
    require(source == sourceDC && probe && info && !section && !offset, "Unexpected DIB allocation");
    auto& owned = probe->surfaces[probe->preparing];
    require(owned.dc && !owned.bitmap && info->bmiHeader.biBitCount == 32 && info->bmiHeader.biHeight < 0,
        "Unexpected desktop DIB ownership or format");
    if (probe->failure == PrepareFailure::Dib) { probe->failure = PrepareFailure::None; return nullptr; }
    const auto bitmap = CreateDIBSection(source, info, mode, pixels, section, offset);
    if (bitmap) {
        owned.bitmap = bitmap;
        owned.bytes = size_t(info->bmiHeader.biWidth) * size_t(-info->bmiHeader.biHeight) * 4;
        ++probe->createdDib;
    }
    return bitmap;
}
HGDIOBJ WINAPI cacheSelectObject(HDC dc, HGDIOBJ object) {
    if (probe && probe->failure == PrepareFailure::Selection) for (const auto& owned : probe->surfaces)
        if (owned.dc == dc && owned.bitmap == object) {
            probe->failure = PrepareFailure::None; return HGDI_ERROR;
        }
    return SelectObject(dc, object);
}
BOOL WINAPI cacheDeleteObject(HGDIOBJ object) noexcept {
    if (probe) for (auto& owned : probe->surfaces) if (owned.bitmap == object) {
        const BOOL removed = DeleteObject(object);
        if (removed) { owned.bitmap = nullptr; owned.bytes = 0; ++probe->deletedDib; }
        else probe->cleanupFailed = true;
        return removed;
    }
    if (probe) probe->cleanupFailed = true;
    return FALSE;
}
BOOL WINAPI cacheDeleteDC(HDC dc) noexcept {
    if (probe) for (auto& owned : probe->surfaces) if (owned.dc == dc) {
        const BOOL removed = DeleteDC(dc);
        if (removed) { owned.dc = nullptr; ++probe->deletedDC; }
        else probe->cleanupFailed = true;
        return removed;
    }
    if (probe) probe->cleanupFailed = true;
    return FALSE;
}
BOOL WINAPI cacheStretch(HDC target, int x, int y, int width, int height, HDC source,
                        int sx, int sy, int sw, int sh, DWORD mode) {
    require(source == sourceDC && probe && x == 0 && y == 0 && sx == 0 && sy == 0 &&
        sw == probe->sourceWidth && sh == probe->sourceHeight &&
        mode == (SRCCOPY | CAPTUREBLT), "Copy escaped the synthetic desktop");
    bool owned = false;
    for (const auto& surface : probe->surfaces) owned |= surface.dc == target;
    require(owned, "Copy used an unowned destination");
    for (int half = 0; half < 2; ++half) {
        const HBRUSH brush = CreateSolidBrush(half ? RGB(210,150,90) : RGB(30,70,110));
        require(brush != nullptr, "Cannot create owned pattern brush");
        const RECT rectangle{half ? width / 2 : 0, 0, half ? width : width / 2, height};
        const int painted = FillRect(target, &rectangle, brush);
        const BOOL deleted = DeleteObject(brush);
        require(painted && deleted, "Owned pattern paint/cleanup failed");
    }
    ++probe->copies;
    return TRUE;
}
#define GetDC cacheGetDC
#define ReleaseDC cacheReleaseDC
#define GetSystemMetrics cacheMetrics
#define GetCursorInfo cacheCursor
#define CreateCompatibleDC cacheCreateDC
#define CreateDIBSection cacheCreateDib
#define SelectObject cacheSelectObject
#define DeleteObject cacheDeleteObject
#define DeleteDC cacheDeleteDC
#define StretchBlt cacheStretch
#include "../src/capture.cpp"
#undef StretchBlt
#undef DeleteDC
#undef DeleteObject
#undef SelectObject
#undef CreateDIBSection
#undef CreateCompatibleDC
#undef GetCursorInfo
#undef GetSystemMetrics
#undef ReleaseDC
#undef GetDC

namespace {
constexpr size_t normalBytes = size_t(4096) * 2160 * 4;
constexpr size_t previewBytes = size_t(640) * 338 * 4;
constexpr size_t observationBytes = size_t(64) * 34 * 4;
void captureSized(int maximumWidth, int maximumHeight, int expectedWidth, int expectedHeight) {
    lapse::Frame frame;
    std::wstring error;
    require(lapse::captureDesktop({0,0,probe->sourceWidth,probe->sourceHeight}, maximumWidth, maximumHeight, false, frame, error) &&
        error.empty() && frame.valid(), "Synthetic desktop capture failed");
    require(frame.width == expectedWidth && frame.height == expectedHeight,
        "Capture geometry changed across cache demand");
    for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x) {
        const auto* pixel = frame.pixels.data() + (size_t(y) * frame.width + x) * 4;
        const bool right = x >= frame.width / 2;
        require(pixel[0] == (right ? 90 : 110) && pixel[1] == (right ? 150 : 70) && pixel[2] == (right ? 210 : 30),
            "Captured RGB pixels changed after release/recreation");
    }
}
void capture(int width, int height) {
    captureSized(width, height, width, width == 64 ? 34 : width == 640 ? 338 : height);
}
void release() noexcept {
    forbidAllocation = true;
    lapse::releaseDesktopCaptureCache();
    forbidAllocation = false;
}
struct ProbeScope {
    CaptureProbe& owned;
    CaptureProbe* previous;
    explicit ProbeScope(CaptureProbe& value) : owned(value), previous(probe) { probe = &owned; }
    ~ProbeScope() { probe = &owned; release(); probe = previous; }
};
void empty(const CaptureProbe& value) {
    require(!value.cleanupFailed && value.bytes() == 0 && value.createdDC == value.deletedDC &&
        value.createdDib == value.deletedDib, "Owned capture resources were not successfully released");
    for (const auto& surface : value.surfaces) require(!surface.dc && !surface.bitmap, "Owned GDI handle survived release");
}
void lifetime() {
    CaptureProbe owned; ProbeScope scope(owned);
    release(); release();
    require(!owned.createdDC && !owned.createdDib, "Cold release allocated desktop resources");
    capture(4096,2160); capture(640,360); capture(64,36);
    require(owned.bytes() == normalBytes + previewBytes + observationBytes && owned.createdDib == 3,
        "Three independent caches were not retained");
    capture(4096,2160); capture(640,360); capture(64,36);
    require(owned.createdDib == 3 && owned.deletedDib == 0, "Active demands failed to reuse their cache");
    release(); empty(owned); release(); empty(owned);
    capture(4096,2160); capture(640,360); capture(64,36);
    require(owned.createdDib == 6 && owned.deletedDib == 3, "Fresh demand did not recreate released caches");
    release(); empty(owned);
    std::cout << "PASS cold/repeated allocation-free release, three caches, exact pixels, active reuse and recreation; bytes="
        << normalBytes + previewBytes + observationBytes << " owned_DIBs=" << owned.createdDib << '/' << owned.deletedDib
        << " owned_DCs=" << owned.createdDC << '/' << owned.deletedDC << '\n';
}
struct Size { int width, height; };
struct Layout { const char* name; Size source, full, preview, fullPixels, previewPixels, observationPixels; };
constexpr Layout layouts[] = {
    {"1080p", {3840,2160}, {1920,1080}, {640,360}, {1920,1080}, {640,360}, {64,36}},
    {"4K", {3840,2160}, {3840,2160}, {640,360}, {3840,2160}, {640,360}, {64,36}},
    {"portrait", {2160,3840}, {1080,1920}, {202,360}, {1080,1920}, {202,359}, {20,36}},
    {"narrow", {3840,2160}, {48,4096}, {4,360}, {48,27}, {4,2}, {64,36}},
    {"ultrawide", {3840,2160}, {4096,48}, {640,8}, {85,48}, {14,8}, {64,36}},
    {"small", {3840,2160}, {320,180}, {640,360}, {320,180}, {640,360}, {64,36}}
};
void capture(Size bounds, Size expected) { captureSized(bounds.width, bounds.height, expected.width, expected.height); }
void workloads() {
    for (const auto& layout : layouts) for (int mode = 0; mode < 4; ++mode) {
        CaptureProbe owned; ProbeScope scope(owned);
        owned.sourceWidth = layout.source.width; owned.sourceHeight = layout.source.height;
        const bool visible = mode < 2, observing = mode == 1;
        if (visible) capture(layout.preview, layout.previewPixels); // Idle preview before Record.
        // Six saved frames interleaved with preview/observation demands. This
        // is a demand sequence, not a wall-clock or CPU benchmark.
        for (int tick = 0; tick < 30; ++tick) {
            if (mode == 3) capture(layout.preview, layout.previewPixels);
            else if (tick % 5 == 0) capture(layout.full, layout.fullPixels);
            else if (visible) capture(layout.preview, layout.previewPixels);
            if (observing) capture({64,36}, layout.observationPixels);
        }
        const int expected = visible ? (observing ? 3 : 2) : 1;
        require(owned.createdDib == expected && owned.deletedDib == 0,
            "Interleaved capture demands reallocated a reusable surface");
        const size_t full = size_t(layout.fullPixels.width) * layout.fullPixels.height * 4;
        require(owned.bytes() <= (mode == 3 ? 0 : full) + size_t(640) * 360 * 4 + size_t(64) * 36 * 4,
            "Extra retained surface exceeded bounded preview/observation sizes");
        release(); empty(owned);
        capture(layout.preview, layout.previewPixels);
        require(owned.bytes() <= size_t(640) * 360 * 4, "Cleanup retained large storage on new preview demand");
        capture(layout.preview, layout.previewPixels);
        require(owned.createdDib == expected + 1, "Preview failed to reuse storage after cleanup");
        release(); empty(owned);
        std::cout << "PASS interleaved " << layout.name << " mode=" << mode << " DIBs=" << expected << '\n';
    }
}
void sizeTransitions() {
    CaptureProbe owned; ProbeScope scope(owned);
    owned.sourceWidth = 3840; owned.sourceHeight = 2160;
    captureSized(640,360,640,360);
    captureSized(3840,2160,3840,2160);
    captureSized(64,36,64,36);
    require(owned.createdDib == 3 && !owned.deletedDib, "Record discarded the existing idle preview");
    captureSized(1920,1080,1920,1080); // Replace only the large surface.
    captureSized(640,360,640,360); captureSized(64,36,64,36);
    require(owned.createdDib == 4 && owned.deletedDib == 1, "Output-size change retired unrelated surfaces");
    captureSized(320,180,320,180); // Two small ordinary sizes remain independent.
    captureSized(1920,1080,1920,1080); captureSized(320,180,320,180); captureSized(64,36,64,36);
    require(owned.createdDib == 5 && owned.deletedDib == 2, "Preview-size change retired the full recording surface");
    release(); empty(owned);
    std::cout << "PASS recording/preview size changes preserve unrelated owned surfaces\n";
}
void preparationFailures() {
    for (const auto fault : {PrepareFailure::DC, PrepareFailure::Dib, PrepareFailure::Selection})
        for (int scenario = 0; scenario < 3; ++scenario) {
            CaptureProbe owned; ProbeScope scope(owned);
            owned.sourceWidth = 3840; owned.sourceHeight = 2160;
            if (scenario > 0) { captureSized(640,360,640,360); captureSized(64,36,64,36); }
            if (scenario == 2) captureSized(3840,2160,3840,2160);
            const int beforeCopies = owned.copies;
            owned.failure = fault;
            lapse::Frame frame; frame.width = frame.height = 1; frame.pixels.resize(4, 127);
            std::wstring error;
            const int width = scenario == 2 ? 320 : 1920, height = scenario == 2 ? 180 : 1080;
            require(!lapse::captureDesktop({0,0,3840,2160}, width, height, false, frame, error) &&
                !error.empty() && !frame.valid() && owned.copies == beforeCopies && owned.failure == PrepareFailure::None,
                "Preparation failure returned stale pixels, copied, or lost its diagnostic");
            require(!owned.cleanupFailed, "Preparation failure could not release partially created objects");
            const int beforeRecovery = owned.createdDib;
            if (scenario == 1) captureSized(640,360,640,360); // Preserved through normal/preview ownership swap.
            if (scenario == 2) captureSized(3840,2160,3840,2160); // Preserved while preview preparation failed.
            if (scenario > 0) captureSized(64,36,64,36);
            require(owned.createdDib == beforeRecovery, "Preparation failure retired unrelated cached pixels");
            captureSized(width,height,width,height);
            require(owned.createdDib == beforeRecovery + 1, "New demand did not recover from preparation failure");
            release(); empty(owned); release(); empty(owned);
        }
    std::cout << "PASS DC/DIB/selection failures invalidate output, preserve other surfaces, and recover with balanced ownership\n";
}
void threadIsolation() {
    CaptureProbe parent, child;
    ProbeScope scope(parent); capture(4096,2160); capture(640,360); capture(64,36);
    std::mutex mutex; std::condition_variable changed;
    bool ready = false, proceed = false;
    std::exception_ptr childFailure, parentFailure;
    std::thread worker([&] {
        ProbeScope childScope(child);
        try {
            release(); capture(4096,2160); capture(640,360); capture(64,36);
            { std::unique_lock<std::mutex> lock(mutex); ready = true; changed.notify_one();
              require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return proceed; }), "Thread isolation gate timed out"); }
            capture(4096,2160); capture(640,360); capture(64,36);
            require(child.createdDib == 3 && child.deletedDib == 0, "Other thread's release invalidated this cache");
        } catch (...) { childFailure = std::current_exception(); }
        release();
        { std::lock_guard<std::mutex> lock(mutex); ready = true; }
        changed.notify_one();
    });
    try {
        { std::unique_lock<std::mutex> lock(mutex);
          require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return ready; }), "Other capture thread did not start"); }
        release(); empty(parent);
    } catch (...) { parentFailure = std::current_exception(); }
    { std::lock_guard<std::mutex> lock(mutex); proceed = true; }
    changed.notify_one(); worker.join();
    release();
    if (parentFailure) std::rethrow_exception(parentFailure);
    if (childFailure) std::rethrow_exception(childFailure);
    empty(parent); empty(child);
    std::cout << "PASS current-thread-only cleanup; second thread retained exact pixels and all reusable surfaces\n";
}
}
int main() {
    sourceDC = CreateCompatibleDC(nullptr);
    if (!sourceDC) return 1;
    int result = 0;
    try { lifetime(); workloads(); sizeTransitions(); preparationFailures(); threadIsolation(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (!DeleteDC(sourceDC)) result = 1;
    return result;
}

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
struct CaptureProbe {
    std::array<OwnedSurface, 2> surfaces{};
    unsigned preparing = 0;
    int createdDC = 0, deletedDC = 0, createdDib = 0, deletedDib = 0, copies = 0;
    bool cleanupFailed = false;
    size_t bytes() const { return surfaces[0].bytes + surfaces[1].bytes; }
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
    if (key == SM_CXVIRTUALSCREEN) return 4096;
    if (key == SM_CYVIRTUALSCREEN) return 2160;
    throw std::runtime_error("Unexpected desktop geometry request");
}
BOOL WINAPI cacheCursor(PCURSORINFO) { throw std::runtime_error("Unexpected physical cursor request"); }
HDC WINAPI cacheCreateDC(HDC source) {
    require(source == sourceDC && probe, "Unexpected compatible DC source");
    for (unsigned i = 0; i < probe->surfaces.size(); ++i) if (!probe->surfaces[i].dc) {
        const auto dc = CreateCompatibleDC(source);
        if (dc) { probe->surfaces[i].dc = dc; probe->preparing = i; ++probe->createdDC; }
        return dc;
    }
    throw std::runtime_error("Desktop cache exceeded its two-surface bound");
}
HBITMAP WINAPI cacheCreateDib(HDC source, const BITMAPINFO* info, UINT mode, void** pixels, HANDLE section, DWORD offset) {
    require(source == sourceDC && probe && info && !section && !offset, "Unexpected DIB allocation");
    auto& owned = probe->surfaces[probe->preparing];
    require(owned.dc && !owned.bitmap && info->bmiHeader.biBitCount == 32 && info->bmiHeader.biHeight < 0,
        "Unexpected desktop DIB ownership or format");
    const auto bitmap = CreateDIBSection(source, info, mode, pixels, section, offset);
    if (bitmap) {
        owned.bitmap = bitmap;
        owned.bytes = size_t(info->bmiHeader.biWidth) * size_t(-info->bmiHeader.biHeight) * 4;
        ++probe->createdDib;
    }
    return bitmap;
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
    require(source == sourceDC && probe && x == 0 && y == 0 && sx == 0 && sy == 0 && sw == 4096 && sh == 2160 &&
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
#define DeleteObject cacheDeleteObject
#define DeleteDC cacheDeleteDC
#define StretchBlt cacheStretch
#include "../src/capture.cpp"
#undef StretchBlt
#undef DeleteDC
#undef DeleteObject
#undef CreateDIBSection
#undef CreateCompatibleDC
#undef GetCursorInfo
#undef GetSystemMetrics
#undef ReleaseDC
#undef GetDC

namespace {
constexpr size_t normalBytes = size_t(4096) * 2160 * 4;
constexpr size_t observationBytes = size_t(64) * 34 * 4;
void capture(int maximumWidth, int maximumHeight) {
    lapse::Frame frame;
    std::wstring error;
    require(lapse::captureDesktop({0,0,4096,2160}, maximumWidth, maximumHeight, false, frame, error) &&
        error.empty() && frame.valid(), "Synthetic desktop capture failed");
    require(frame.width == maximumWidth && frame.height == (maximumWidth == 64 ? 34 : maximumHeight),
        "Capture geometry changed across cache demand");
    for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x) {
        const auto* pixel = frame.pixels.data() + (size_t(y) * frame.width + x) * 4;
        const bool right = x >= frame.width / 2;
        require(pixel[0] == (right ? 90 : 110) && pixel[1] == (right ? 150 : 70) && pixel[2] == (right ? 210 : 30),
            "Captured RGB pixels changed after release/recreation");
    }
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
    capture(4096,2160); capture(64,36);
    require(owned.bytes() == normalBytes + observationBytes && owned.createdDib == 2, "Both independent caches were not retained");
    capture(4096,2160); capture(64,36);
    require(owned.createdDib == 2 && owned.deletedDib == 0, "Active normal/observation demands failed to reuse their cache");
    release(); empty(owned); release(); empty(owned);
    capture(4096,2160); capture(64,36);
    require(owned.createdDib == 4 && owned.deletedDib == 2, "Fresh demand did not recreate released caches");
    release(); empty(owned);
    std::cout << "PASS cold/repeated allocation-free release, both caches, exact pixels, active reuse and recreation; bytes="
        << normalBytes + observationBytes << " owned_DIBs=" << owned.createdDib << '/' << owned.deletedDib
        << " owned_DCs=" << owned.createdDC << '/' << owned.deletedDC << '\n';
}
void threadIsolation() {
    CaptureProbe parent, child;
    ProbeScope scope(parent); capture(4096,2160); capture(64,36);
    std::mutex mutex; std::condition_variable changed;
    bool ready = false, proceed = false;
    std::exception_ptr childFailure, parentFailure;
    std::thread worker([&] {
        ProbeScope childScope(child);
        try {
            release(); capture(4096,2160); capture(64,36);
            { std::unique_lock<std::mutex> lock(mutex); ready = true; changed.notify_one();
              require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return proceed; }), "Thread isolation gate timed out"); }
            capture(4096,2160); capture(64,36);
            require(child.createdDib == 2 && child.deletedDib == 0, "Other thread's release invalidated this cache");
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
    std::cout << "PASS current-thread-only cleanup; second thread retained exact pixels and both reusable surfaces\n";
}
}
int main() {
    sourceDC = CreateCompatibleDC(nullptr);
    if (!sourceDC) return 1;
    int result = 0;
    try { lifetime(); threadIsolation(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (!DeleteDC(sourceDC)) result = 1;
    return result;
}

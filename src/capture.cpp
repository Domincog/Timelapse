#include "capture.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <atomic>

namespace lapse {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD allStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

std::wstring captureError(const wchar_t* context, HRESULT hr) {
    return std::wstring(context) + L" " + errorText(hr);
}

struct MonitorEnumeration {
    std::vector<Monitor> monitors;
    bool failed = false;
};

template<typename Enumerate>
std::wstring monitorIdentityImpl(const wchar_t* deviceName, Enumerate enumerate) {
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{}; device.cb = sizeof(device);
        if (!enumerate(deviceName, index, &device, EDD_GET_DEVICE_INTERFACE_NAME)) break;
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) && device.DeviceID[0])
            return L"monitor:" + std::wstring(device.DeviceID);
    }
    return L"gdi:" + std::wstring(deviceName);
}

const Monitor* findMonitor(const std::vector<Monitor>& monitors, const std::wstring& id) {
    for (const auto& monitor : monitors)
        if (CompareStringOrdinal(monitor.id.c_str(), -1, id.c_str(), -1, TRUE) == CSTR_EQUAL)
            return &monitor;
    return nullptr;
}

template<typename Enumerate, typename Capture>
bool captureMonitorImpl(const std::wstring& id, int maxWidth, int maxHeight, bool cursor,
                        Frame& output, std::wstring& error, Enumerate enumerate, Capture capture) {
    // Keep the reusable pixel allocation, but never expose a rejected capture as valid.
    struct Result {
        Frame& frame;
        bool accepted = false;
        ~Result() { if (!accepted) frame.width = frame.height = 0; }
    } result{output};
    output.width = output.height = 0;
    error.clear();
    if (id.empty()) { error = L"Select a display before starting capture."; return false; }
    const auto before = enumerate();
    const auto* selected = findMonitor(before, id);
    if (!selected) {
        error = L"The selected display is unavailable. Reconnect it or select another display.";
        return false;
    }
    const RECT bounds = selected->bounds;
    if (!capture(bounds, maxWidth, maxHeight, cursor, output, error)) return false;
    const auto after = enumerate();
    const auto* current = findMonitor(after, id);
    if (!current || !EqualRect(&current->bounds, &bounds)) {
        error = L"The selected display changed during capture. Try recording again.";
        return false;
    }
    result.accepted = true;
    return true;
}

struct ScreenDC {
    HDC value = GetDC(nullptr);
    ~ScreenDC() { if (value) ReleaseDC(nullptr, value); }
};

// Reuse output-sized surfaces per capture thread; never allocate a full
// desktop-sized intermediate image just to shrink it for the timelapse.
struct DesktopSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* pixels = nullptr;
    int width = 0, height = 0;
    ~DesktopSurface() { clear(); }
    void clear() noexcept {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr; bitmap = nullptr; previous = nullptr; pixels = nullptr;
        width = height = 0;
    }
    bool prepare(HDC screen, int w, int h) {
        if (dc && width == w && height == h) return true;
        clear();
        dc = CreateCompatibleDC(screen);
        if (!dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w; info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) { clear(); return false; }
        previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) { previous = nullptr; clear(); return false; }
        width = w; height = h;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        return true;
    }
};

struct DesktopSurfaceCache {
    DesktopSurface normal, preview, observation;
    DesktopSurface& select(int width, int height, int maximumWidth, int maximumHeight) noexcept {
        // Requested bounds distinguish observation from an unusually narrow
        // recording or preview whose aspect-fitted pixels happen to be tiny.
        if (maximumWidth <= 64 && maximumHeight <= 36) return observation;
        if (normal.dc && normal.width == width && normal.height == height) return normal;
        if (preview.dc && preview.width == width && preview.height == height) return preview;
        if (width <= 640 && height <= 360) return normal.dc ? preview : normal;
        // Preserve a bounded preview populated before recording starts. Swap
        // raw ownership fields, never copy a destructor-owning Surface object.
        if (normal.dc && normal.width <= 640 && normal.height <= 360) {
            std::swap(normal.dc, preview.dc); std::swap(normal.bitmap, preview.bitmap);
            std::swap(normal.previous, preview.previous); std::swap(normal.pixels, preview.pixels);
            std::swap(normal.width, preview.width); std::swap(normal.height, preview.height);
        }
        return normal;
    }
};
// Keep cold cleanup from initializing a destructor-bearing thread-local cache.
// In particular, finishing a camera-only recording needs no desktop resources.
thread_local DesktopSurfaceCache* existingDesktopCache = nullptr;
DesktopSurfaceCache& desktopSurfaceCache() {
    thread_local DesktopSurfaceCache cache;
    existingDesktopCache = &cache;
    return cache;
}

void drawCursor(HDC dc, const RECT& bounds, int width, int height) {
    CURSORINFO cursor{sizeof(CURSORINFO)};
    if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING)) return;
    struct OwnedIcon {
        HICON value = nullptr;
        ~OwnedIcon() { if (value) DestroyIcon(value); }
    } owned{CopyIcon(cursor.hCursor)};
    ICONINFO icon{};
    if (!owned.value || !GetIconInfo(owned.value, &icon)) return;
    BITMAP bitmap{};
    int nativeWidth = GetSystemMetrics(SM_CXCURSOR), nativeHeight = GetSystemMetrics(SM_CYCURSOR);
    if (GetObjectW(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(bitmap), &bitmap)) {
        nativeWidth = bitmap.bmWidth;
        nativeHeight = icon.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
    }
    const double sx = double(width) / (bounds.right - bounds.left);
    const double sy = double(height) / (bounds.bottom - bounds.top);
    const int x = int(std::lround((double(cursor.ptScreenPos.x) - bounds.left - icon.xHotspot) * sx));
    const int y = int(std::lround((double(cursor.ptScreenPos.y) - bounds.top - icon.yHotspot) * sy));
    DrawIconEx(dc, x, y, owned.value, std::max(1, int(std::lround(nativeWidth * sx))),
               std::max(1, int(std::lround(nativeHeight * sy))), 0, nullptr, DI_NORMAL);
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
    if (icon.hbmMask) DeleteObject(icon.hbmMask);
}

struct ActivationArray {
    IMFActivate** values = nullptr;
    UINT32 count = 0;
    ~ActivationArray() {
        for (UINT32 i = 0; i < count; ++i) if (values[i]) values[i]->Release();
        CoTaskMemFree(values);
    }
};

std::wstring attributeString(IMFAttributes* attributes, REFGUID key) {
    UINT32 length = 0;
    if (FAILED(attributes->GetStringLength(key, &length))) return {};
    std::wstring result(size_t(length) + 1, L'\0');
    if (FAILED(attributes->GetString(key, result.data(), length + 1, &length))) return {};
    result.resize(length);
    return result;
}

struct Format { UINT32 width = 0, height = 0; LONG stride = 0; UINT32 transferFunction = 0; };
std::atomic<uint64_t> nextCameraEpoch{0};
struct CameraState {
    explicit CameraState(CameraResolution value = CameraResolution::Standard720) : resolution(value) {}
    const CameraResolution resolution;
    std::mutex mutex;
    // Cleared under mutex before the owning reader reference is released.
    IMFSourceReader* reader = nullptr;
    bool running = false;
    HRESULT failure = S_OK;
    ComPtr<IMFSample> sample;
    Format format;
    Clock::time_point started{}, received{};
    uint64_t receivedTick = 0;
    CameraSampleInfo info{++nextCameraEpoch};
};

HRESULT readFormat(IMFSourceReader* reader, Format& format, CameraResolution resolution) {
    const auto limits = cameraCaptureLimits(resolution);
    if (!limits.pixelBytes) return E_INVALIDARG;
    ComPtr<IMFMediaType> type;
    HRESULT hr = reader->GetCurrentMediaType(videoStream, &type);
    if (FAILED(hr)) return hr;
    if (!type) return MF_E_INVALIDMEDIATYPE;
    GUID major{}, subtype{};
    hr = type->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr) || major != MFMediaType_Video) return MF_E_INVALIDMEDIATYPE;
    hr = type->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr) || subtype != MFVideoFormat_RGB32) return MF_E_INVALIDMEDIATYPE;
    UINT32 width = 0, height = 0;
    hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr) || !width || !height || width > limits.width || height > limits.height)
        return MF_E_INVALIDMEDIATYPE;
    UINT32 stride = 0;
    hr = type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
    if (FAILED(hr)) {
        LONG computed = 0;
        hr = MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &computed);
        if (FAILED(hr)) return hr;
        stride = UINT32(computed);
    }
    if (std::abs(int64_t(LONG(stride))) < int64_t(width) * 4) return MF_E_INVALIDMEDIATYPE;
    UINT32 transfer = 0;
    type->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transfer);
    format = {width, height, LONG(stride), transfer};
    return S_OK;
}

class BufferLock {
public:
    explicit BufferLock(IMFMediaBuffer* buffer) : buffer_(buffer) {}
    ~BufferLock() {
        if (locked2D_) surface_->Unlock2D();
        else if (locked_) buffer_->Unlock();
    }
    HRESULT lock(LONG defaultStride, UINT32 height) {
        ComPtr<IMF2DBuffer2> extended;
        if (SUCCEEDED(buffer_.As(&extended))) {
            const HRESULT hr = extended->Lock2DSize(MF2DBuffer_LockFlags_Read, &top, &stride, &base_, &length_);
            if (SUCCEEDED(hr)) {
                surface_ = extended; locked2D_ = true;
                return S_OK;
            }
        }
        // Legacy Lock2D exposes no allocation bounds. IMFMediaBuffer::Lock
        // supplies a bounded contiguous representation using the media stride.
        DWORD maximum = 0;
        HRESULT hr = buffer_->Lock(&base_, &maximum, &length_);
        if (FAILED(hr)) return hr;
        locked_ = true;
        if (length_ > maximum) return E_UNEXPECTED;
        stride = defaultStride; top = base_;
        if (stride < 0) {
            const uint64_t offset = uint64_t(-int64_t(stride)) * (height - 1);
            if (offset >= length_) return MF_E_BUFFERTOOSMALL;
            top += size_t(offset);
        }
        return S_OK;
    }
    bool containsRows(UINT32 width, UINT32 height) const {
        if (!top || !height) return false;
        const uint64_t pitch = uint64_t(std::abs(int64_t(stride)));
        const uint64_t rowBytes = uint64_t(width) * 4;
        if (pitch < rowBytes) return false;
        const uint64_t offset = pitch * (height - 1);
        const uintptr_t topAddress = reinterpret_cast<uintptr_t>(top);
        const uintptr_t baseAddress = reinterpret_cast<uintptr_t>(base_);
        if (stride < 0 && offset > topAddress) return false;
        const uintptr_t first = stride < 0 ? topAddress - uintptr_t(offset) : topAddress;
        if (first < baseAddress) return false;
        const uint64_t prefix = first - baseAddress;
        return prefix <= length_ && offset + rowBytes <= uint64_t(length_) - prefix;
    }
    BYTE* top = nullptr;
    LONG stride = 0;
private:
    ComPtr<IMFMediaBuffer> buffer_;
    ComPtr<IMF2DBuffer> surface_;
    BYTE* base_ = nullptr;
    DWORD length_ = 0;
    bool locked_ = false, locked2D_ = false;
};

HRESULT copySample(IMFSample* sample, const Format& format, Frame& output) {
    ComPtr<IMFMediaBuffer> buffer;
    DWORD count = 0;
    HRESULT hr = sample->GetBufferCount(&count);
    if (FAILED(hr)) return hr;
    hr = count == 1 ? sample->GetBufferByIndex(0, &buffer) : sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) return hr;
    BufferLock lock(buffer.Get());
    hr = lock.lock(format.stride, format.height);
    if (FAILED(hr)) return hr;
    if (!lock.containsRows(format.width, format.height)) return MF_E_BUFFERTOOSMALL;
    output.pixels.resize(size_t(format.width) * format.height * 4);
    const size_t rowBytes = size_t(format.width) * 4;
    for (UINT32 y = 0; y < format.height; ++y)
        std::memcpy(output.pixels.data() + size_t(y) * rowBytes, lock.top + ptrdiff_t(y) * lock.stride, rowBytes);
    output.width = int(format.width); output.height = int(format.height);
    return S_OK;
}

class CameraCallback final : public IMFSourceReaderCallback {
public:
    explicit CameraCallback(std::shared_ptr<CameraState> state) : state_(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFSourceReaderCallback)) return E_NOINTERFACE;
        *out = static_cast<IMFSourceReaderCallback*>(this); AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references_);
        if (!remaining) delete this;
        return ULONG(remaining);
    }
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG timestamp, IMFSample* sample) override {
        // A sample can own cleanup objects that report another callback event.
        // Acquire the incoming reference and retire old references outside the
        // state lock, including reset followed by replacement in one callback.
        ComPtr<IMFSample> incoming, retired;
        if (sample && SUCCEEDED(status) && !(flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) incoming = sample;
        UINT32 discontinuity = FALSE;
        if (incoming) incoming->GetUINT32(MFSampleExtension_Discontinuity, &discontinuity);
        ComPtr<IMFSourceReader> next;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (!state_->running || !state_->reader) return S_OK;
            if (SUCCEEDED(status) && (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
                status = MF_E_END_OF_STREAM;
            if (SUCCEEDED(status) && (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) {
                retired.Swap(state_->sample);
                status = readFormat(state_->reader, state_->format, state_->resolution);
                state_->info.epoch = ++nextCameraEpoch;
            }
            if (FAILED(status)) { state_->failure = status; state_->running = false; }
            else {
                if (sample) {
                    state_->sample.Swap(incoming); state_->received = Clock::now();
                    state_->receivedTick = GetTickCount64();
                    if (discontinuity) state_->info.epoch = ++nextCameraEpoch;
                    ++state_->info.sequence;
                    state_->info.receivedTick = state_->receivedTick;
                    state_->info.timestamp100ns = timestamp;
                    state_->info.timestampValid = true;
                    state_->info.discontinuity = discontinuity != FALSE;
                    state_->info.transferFunction = state_->format.transferFunction;
                }
                next = state_->reader;
            }
        }
        // Only a single latest MF sample is kept. Pixel copying happens only
        // when the application consumes a frame, not for every camera callback.
        if (next) {
            status = next->ReadSample(videoStream, 0, nullptr, nullptr, nullptr, nullptr);
            if (FAILED(status)) setError(status);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent* event) override {
        HRESULT status = S_OK;
        if (event && SUCCEEDED(event->GetStatus(&status)) && FAILED(status)) setError(status);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override { return S_OK; }
private:
    void setError(HRESULT status) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (state_->running) { state_->failure = status; state_->running = false; }
    }
    LONG references_ = 1;
    std::shared_ptr<CameraState> state_;
};

HRESULT chooseCameraFormat(IMFSourceReader* reader, CameraResolution resolution) {
    const auto limits = cameraCaptureLimits(resolution);
    if (!limits.pixelBytes) return E_INVALIDARG;
    struct Candidate { ComPtr<IMFMediaType> type; double score; UINT32 width, height, n, d; };
    std::vector<Candidate> candidates;
    for (DWORD index = 0; index < 512; ++index) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(videoStream, index, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) return hr;
        if (!type) continue;
        UINT32 w = 0, h = 0, n = 0, d = 0;
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w < 2 || h < 2 || w > 7680 || h > 4320) continue;
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &n, &d);
        const double fps = d ? double(n) / d : 30.0;
        if (fps <= 0) continue;
        const double sizeCost = 160.0 * std::abs(std::log(double(w) * h / (double(limits.width) * limits.height)));
        const double rateDifference = std::abs(fps - 10.0) * 3.0;
        // Keep the original 720p preference unchanged. In the detail tier,
        // a common 60fps native 1080p type should not lose to 720p solely on
        // frame rate: the output still requests at most 10fps when supported.
        const double rateCost = (resolution == CameraResolution::Standard720 ? rateDifference :
            std::min(rateDifference, 60.0)) + (fps < 5 ? 100.0 : 0.0);
        const double excessCost = w > limits.width || h > limits.height ? 300.0 : 0.0;
        candidates.push_back({std::move(type), sizeCost + rateCost + excessCost, w, h, n, d});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    HRESULT lastError = MF_E_INVALIDMEDIATYPE;
    for (const auto& candidate : candidates) {
        lastError = reader->SetCurrentMediaType(videoStream, nullptr, candidate.type.Get());
        if (FAILED(lastError)) continue;
        ComPtr<IMFMediaType> output;
        lastError = MFCreateMediaType(&output);
        if (FAILED(lastError)) return lastError;
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        const double scale = std::min({1.0, double(limits.width) / candidate.width, double(limits.height) / candidate.height});
        const UINT32 width = std::max(2u, UINT32(candidate.width * scale) & ~1u);
        const UINT32 height = std::max(2u, UINT32(candidate.height * scale) & ~1u);
        MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (candidate.d && uint64_t(candidate.n) < 10ull * candidate.d)
            MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, candidate.n, candidate.d);
        else MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 10, 1);
        lastError = reader->SetCurrentMediaType(videoStream, nullptr, output.Get());
        Format delivered;
        if (SUCCEEDED(lastError)) lastError = readFormat(reader, delivered, resolution);
        if (SUCCEEDED(lastError)) return S_OK;
        output->DeleteItem(MF_MT_FRAME_RATE);
        lastError = reader->SetCurrentMediaType(videoStream, nullptr, output.Get());
        if (SUCCEEDED(lastError)) lastError = readFormat(reader, delivered, resolution);
        if (SUCCEEDED(lastError)) return S_OK;
    }
    return lastError;
}
}

std::vector<Monitor> enumerateMonitors() {
    MonitorEnumeration result;
    const BOOL complete = EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto& result = *reinterpret_cast<MonitorEnumeration*>(data);
        MONITORINFOEXW info{}; info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) { result.failed = true; return FALSE; }
        auto& monitors = result.monitors;
        try {
            std::wstring name = info.szDevice;
            const size_t prefix = name.find(L"DISPLAY");
            if (prefix != std::wstring::npos) name = L"Display " + name.substr(prefix + 7);
            if (info.dwFlags & MONITORINFOF_PRIMARY) name += L" (primary)";
            Monitor entry{name, info.rcMonitor, monitorIdentityImpl(info.szDevice, EnumDisplayDevicesW)};
            if (info.dwFlags & MONITORINFOF_PRIMARY) monitors.insert(monitors.begin(), std::move(entry));
            else monitors.push_back(std::move(entry));
        } catch (...) { result.failed = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    if (!complete || result.failed) return {};
    return std::move(result.monitors);
}

std::vector<CameraDevice> enumerateCameras(std::wstring& error) {
    error.clear();
    std::vector<CameraDevice> result;
    try {
        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        ActivationArray devices;
        if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attributes.Get(), &devices.values, &devices.count);
        if (FAILED(hr)) { error = captureError(L"Cannot list cameras.", hr); return result; }
        for (UINT32 i = 0; i < devices.count; ++i) {
            CameraDevice device{attributeString(devices.values[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME),
                                attributeString(devices.values[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)};
            if (device.name.empty()) device.name = L"Camera " + std::to_wstring(i + 1);
            if (!device.id.empty()) result.push_back(std::move(device));
        }
    } catch (const std::bad_alloc&) { result.clear(); error = L"Not enough memory to list cameras."; }
    return result;
}

void releaseDesktopCaptureCache() noexcept {
    if (auto* cache = existingDesktopCache) {
        cache->normal.clear();
        cache->preview.clear();
        cache->observation.clear();
    }
}

bool captureDesktop(const RECT& bounds, int maxWidth, int maxHeight, bool cursor, Frame& output, std::wstring& error) {
    output.width = output.height = 0;
    error.clear();
    const int64_t sourceWidth = int64_t(bounds.right) - bounds.left;
    const int64_t sourceHeight = int64_t(bounds.bottom) - bounds.top;
    if (sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > INT_MAX || sourceHeight > INT_MAX ||
        maxWidth <= 0 || maxHeight <= 0 || maxWidth > 7680 || maxHeight > 4320) {
        error = L"The desktop capture size is invalid."; return false;
    }
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT intersection{};
    if (!IntersectRect(&intersection, &desktop, &bounds) || !EqualRect(&intersection, &bounds)) {
        error = L"The selected display is no longer available. Select a display again."; return false;
    }
    const double scale = std::min({1.0, double(maxWidth) / sourceWidth, double(maxHeight) / sourceHeight});
    const int width = std::max(1, int(std::lround(sourceWidth * scale)));
    const int height = std::max(1, int(std::lround(sourceHeight * scale)));
    ScreenDC screen;
    // Keep recording, bounded preview (640x360), and observation (64x36)
    // storage reusable. Each acquires GDI objects only when requested.
    auto& cache = desktopSurfaceCache();
    auto& surface = cache.select(width, height, maxWidth, maxHeight);
    if (!screen.value || !surface.prepare(screen.value, width, height)) {
        error = L"Windows could not prepare the desktop capture."; return false;
    }
    if (!StretchBlt(surface.dc, 0, 0, width, height, screen.value, bounds.left, bounds.top,
                    int(sourceWidth), int(sourceHeight), SRCCOPY | CAPTUREBLT)) {
        error = L"Windows could not capture the desktop. The desktop may be locked or unavailable.";
        return false;
    }
    if (cursor) drawCursor(surface.dc, bounds, width, height);
    if (!GdiFlush()) { error = L"Windows could not finish desktop capture."; return false; }
    try {
        output.pixels.resize(size_t(width) * height * 4);
        std::memcpy(output.pixels.data(), surface.pixels, output.pixels.size());
        output.width = width; output.height = height;
        return true;
    } catch (const std::bad_alloc&) { error = L"Not enough memory to capture the desktop."; return false; }
}

bool captureMonitor(const std::wstring& id, int maxWidth, int maxHeight, bool cursor,
                    Frame& output, std::wstring& error) {
    return captureMonitorImpl(id, maxWidth, maxHeight, cursor, output, error,
                              enumerateMonitors, captureDesktop);
}

struct Camera::Impl {
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    std::shared_ptr<CameraState> state;
};
Camera::Camera() : impl_(std::make_unique<Impl>()) {}
Camera::~Camera() { stop(); }

bool Camera::start(const std::wstring& id, std::wstring& error, CameraResolution resolution) {
    stop(); error.clear();
    if (!cameraCaptureLimits(resolution).pixelBytes) { error = L"The camera resolution limit is invalid."; return false; }
    if (id.empty()) { error = L"Select a camera first."; return false; }
    HRESULT hr = S_OK;
    try {
        auto state = std::make_shared<CameraState>(resolution);
        impl_->state = state;
        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 2);
        if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(hr)) hr = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, id.c_str());
        if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attributes.Get(), &impl_->source);
        ComPtr<IMFSourceReaderCallback> callback;
        callback.Attach(new CameraCallback(state));
        attributes.Reset();
        if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes, 3);
        if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(impl_->source.Get(), attributes.Get(), &impl_->reader);
        if (SUCCEEDED(hr)) hr = impl_->reader->SetStreamSelection(allStreams, FALSE);
        if (SUCCEEDED(hr)) hr = impl_->reader->SetStreamSelection(videoStream, TRUE);
        if (SUCCEEDED(hr)) hr = chooseCameraFormat(impl_->reader.Get(), resolution);
        if (SUCCEEDED(hr)) hr = readFormat(impl_->reader.Get(), state->format, state->resolution);
        if (SUCCEEDED(hr)) {
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                state->started = state->received = Clock::now(); state->reader = impl_->reader.Get(); state->running = true;
            }
            hr = impl_->reader->ReadSample(videoStream, 0, nullptr, nullptr, nullptr, nullptr);
        }
    } catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
    if (FAILED(hr)) {
        stop();
        error = captureError(L"Cannot open the camera. Check Windows camera access and close other camera apps.", hr);
        return false;
    }
    return true;
}

void Camera::stop() {
    ComPtr<IMFSample> retired;
    if (impl_->state) {
        std::lock_guard<std::mutex> guard(impl_->state->mutex);
        impl_->state->running = false; impl_->state->reader = nullptr; retired.Swap(impl_->state->sample);
    }
    retired.Reset();
    if (impl_->reader) impl_->reader->Flush(allStreams);
    if (impl_->source) impl_->source->Shutdown();
    impl_->reader.Reset(); impl_->source.Reset(); impl_->state.reset();
}

bool Camera::latest(Frame& output, std::wstring& error) {
    uint64_t receivedTick = 0;
    return latest(output, error, receivedTick);
}

bool Camera::latest(Frame& output, std::wstring& error, uint64_t& receivedTick) {
    CameraSampleInfo info;
    const bool ready = latestNewer(output, error, info, {});
    if (ready) receivedTick = info.receivedTick;
    return ready;
}

bool Camera::latestNewer(Frame& output, std::wstring& error, CameraSampleInfo& info,
                         const CameraSampleInfo& watermark) {
    error.clear();
    if (!impl_->state) { error = L"The camera is not running."; return false; }
    ComPtr<IMFSample> sample;
    Format format;
    {
        std::lock_guard<std::mutex> guard(impl_->state->mutex);
        const auto& state = *impl_->state;
        info = state.info;
        if (FAILED(state.failure)) {
            error = captureError(L"The camera stopped responding. Reconnect it and try again.", state.failure);
            return false;
        }
        // Retiring an old-format sample does not erase its arrival time. Until
        // the first sample, received retains the original activation deadline.
        const auto last = state.received;
        if (Clock::now() - last > std::chrono::seconds(3)) {
            error = L"No fresh camera frame arrived for 3 seconds. Check the camera connection and try again.";
            return false;
        }
        if (!state.sample) return false;
        if (watermark.epoch && watermark.epoch == info.epoch && info.sequence <= watermark.sequence) return false;
        sample = state.sample; format = state.format;
    }
    HRESULT hr = S_OK;
    try { hr = copySample(sample.Get(), format, output); }
    catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
    if (FAILED(hr)) { error = captureError(L"Cannot read the camera frame.", hr); return false; }
    return true;
}
}


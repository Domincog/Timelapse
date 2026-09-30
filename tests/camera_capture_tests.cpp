// All media is synthetic. Include capture.cpp only to exercise private sample
// helpers; no camera enumeration/start, desktop capture, helper, or UI is used.
#include "core.h"
#define private public
#include "capture.h"
#undef private
#ifdef REVIEW_BEFORE
#include "capture.cpp"
#elif defined(REVIEW_CANDIDATE)
#include "capture.candidate.cpp"
#else
#include "../src/capture.cpp"
#endif
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class LegacyBuffer final : public IMFMediaBuffer, public IMF2DBuffer {
public:
    explicit LegacyBuffer(IMFMediaBuffer* buffer) : buffer_(buffer) {
        check(SUCCEEDED(buffer_.As(&surface_)), "synthetic legacy surface exists");
    }
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IMFMediaBuffer)) *out = static_cast<IMFMediaBuffer*>(this);
        else if (id == __uuidof(IMF2DBuffer)) *out = static_cast<IMF2DBuffer*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
    STDMETHODIMP Lock(BYTE** data, DWORD* maximum, DWORD* current) override {
        ++locks; return FAILED(lockStatus) ? lockStatus : buffer_->Lock(data, maximum, current);
    }
    STDMETHODIMP Unlock() override { ++unlocks; return buffer_->Unlock(); }
    STDMETHODIMP GetCurrentLength(DWORD* size) override { return buffer_->GetCurrentLength(size); }
    STDMETHODIMP SetCurrentLength(DWORD size) override { return buffer_->SetCurrentLength(size); }
    STDMETHODIMP GetMaxLength(DWORD* size) override { return buffer_->GetMaxLength(size); }
    STDMETHODIMP Lock2D(BYTE** data, LONG* pitch) override { ++locks2D; return surface_->Lock2D(data, pitch); }
    STDMETHODIMP Unlock2D() override { ++unlocks2D; return surface_->Unlock2D(); }
    STDMETHODIMP GetScanline0AndPitch(BYTE** data, LONG* pitch) override { return surface_->GetScanline0AndPitch(data, pitch); }
    STDMETHODIMP IsContiguousFormat(BOOL* value) override { return surface_->IsContiguousFormat(value); }
    STDMETHODIMP GetContiguousLength(DWORD* size) override { return surface_->GetContiguousLength(size); }
    STDMETHODIMP ContiguousCopyTo(BYTE* data, DWORD size) override { return surface_->ContiguousCopyTo(data, size); }
    STDMETHODIMP ContiguousCopyFrom(const BYTE* data, DWORD size) override { return surface_->ContiguousCopyFrom(data, size); }
    int locks = 0, unlocks = 0, locks2D = 0, unlocks2D = 0;
    HRESULT lockStatus = S_OK;
private:
    std::atomic<ULONG> refs_{1};
    ComPtr<IMFMediaBuffer> buffer_;
    ComPtr<IMF2DBuffer> surface_;
};

void legacySamples() {
    for (BOOL bottomUp : {FALSE, TRUE}) for (DWORD width : {2u, 17u}) {
        constexpr DWORD height = 3;
        const DWORD rowBytes = width * 4;
        const LONG stride = bottomUp ? -LONG(rowBytes) : LONG(rowBytes);
        ComPtr<IMFMediaBuffer> buffer;
        check(SUCCEEDED(MFCreate2DMediaBuffer(width, height, MFVideoFormat_RGB32.Data1, bottomUp, &buffer)), "create real MF legacy buffer");
        ComPtr<IMF2DBuffer> surface;
        check(SUCCEEDED(buffer.As(&surface)), "query real MF legacy surface");
        BYTE* top = nullptr; LONG pitch = 0;
        check(SUCCEEDED(surface->Lock2D(&top, &pitch)), "fill legacy sample through native pitch");
        std::vector<BYTE> expected(size_t(rowBytes) * height);
        for (DWORD y = 0; y < height; ++y) for (DWORD x = 0; x < rowBytes; ++x) {
            const BYTE value = BYTE(19 + y * 73 + x);
            top[int64_t(y) * pitch + x] = value;
            expected[size_t(y) * rowBytes + x] = value;
        }
        check(SUCCEEDED(surface->Unlock2D()), "unlock initialized native surface");
        check(uint64_t(std::abs(int64_t(pitch))) > rowBytes, "fixture actually has native row padding");
        ComPtr<LegacyBuffer> legacy; legacy.Attach(new LegacyBuffer(buffer.Get()));
        {
            lapse::BufferLock lock(static_cast<IMFMediaBuffer*>(legacy.Get()));
            check(SUCCEEDED(lock.lock(stride, height)), "lock bounded contiguous fallback");
            check(!lock.containsRows(width, height + 1), "REGRESSION: legacy sample accepts extra row");
            check(!lock.containsRows(width + 1, height), "legacy sample accepts over-wide row");
            check(lock.containsRows(width, height) && lock.stride == stride, "fallback uses contiguous default stride");
        }
        check(legacy->locks == 1 && legacy->unlocks == 1 && legacy->locks2D == 0, "fallback uses and releases only bounded Lock");
        ComPtr<IMFSample> sample;
        check(SUCCEEDED(MFCreateSample(&sample)), "create real MF sample");
        check(SUCCEEDED(sample->AddBuffer(static_cast<IMFMediaBuffer*>(legacy.Get()))), "attach legacy buffer");
        lapse::Frame output;
        check(SUCCEEDED(lapse::copySample(sample.Get(), {width, height, stride}, output)), "copy valid legacy sample");
        check(output.valid() && output.width == int(width) && output.height == int(height) && output.pixels == expected,
              "contiguous fallback preserves every row and excludes native padding");
        check(legacy->unlocks == 2, "successful sample copy releases lock");
        const lapse::Frame before = output;
        check(FAILED(lapse::copySample(sample.Get(), {width, height + 1, stride}, output)), "reject advertised height beyond sample");
        check(output.width == before.width && output.height == before.height && output.pixels == before.pixels, "rejected geometry preserves output");
        check(legacy->unlocks == 3, "rejected geometry releases acquired lock");
        legacy->lockStatus = E_ACCESSDENIED;
        check(lapse::copySample(sample.Get(), {width, height, stride}, output) == E_ACCESSDENIED, "retain bounded lock failure");
        check(legacy->unlocks == 3 && legacy->locks2D == 0 && legacy->unlocks2D == 0, "failed Lock does not unlock or retry unbounded Lock2D");
        std::cout << "legacy_copy width=" << width << " bottom_up=" << bottomUp << " native_pitch=" << pitch << " copied_stride=" << stride << " passed\n";
    }
}

void modernSamples() {
    for (BOOL bottomUp : {FALSE, TRUE}) {
        ComPtr<IMFMediaBuffer> buffer;
        check(SUCCEEDED(MFCreate2DMediaBuffer(2, 2, MFVideoFormat_RGB32.Data1, bottomUp, &buffer)), "create modern sample");
        ComPtr<IMF2DBuffer> surface; check(SUCCEEDED(buffer.As(&surface)), "query modern surface");
        BYTE* top = nullptr; LONG pitch = 0;
        check(SUCCEEDED(surface->Lock2D(&top, &pitch)), "fill modern surface");
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 8; ++x) top[int64_t(y) * pitch + x] = BYTE(20 + y * 60 + x);
        check(SUCCEEDED(surface->Unlock2D()), "unlock modern surface");
        { lapse::BufferLock lock(buffer.Get());
          check(SUCCEEDED(lock.lock(bottomUp ? -8 : 8, 2)), "lock modern bounded interface");
          check(lock.stride == pitch && lock.containsRows(2, 2) && !lock.containsRows(2, 3), "modern path retains bounded native pitch"); }
        ComPtr<IMFSample> sample; check(SUCCEEDED(MFCreateSample(&sample)), "create modern MF sample");
        check(SUCCEEDED(sample->AddBuffer(buffer.Get())), "attach modern sample buffer");
        lapse::Frame output;
        check(SUCCEEDED(lapse::copySample(sample.Get(), {2, 2, bottomUp ? -8 : 8}, output)) && output.valid(), "copy modern sample");
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 8; ++x)
            check(output.pixels[size_t(y) * 8 + x] == BYTE(20 + y * 60 + x), "modern path preserves row orientation");
    }
    std::cout << "modern_bounded_samples passed\n";
}

void ordinarySamples() {
    for (LONG stride : {12, -12}) {
        ComPtr<IMFMediaBuffer> buffer; check(SUCCEEDED(MFCreateMemoryBuffer(32, &buffer)), "create ordinary padded sample");
        BYTE* base = nullptr; DWORD maximum = 0;
        check(SUCCEEDED(buffer->Lock(&base, &maximum, nullptr)), "fill ordinary memory buffer");
        BYTE* top = stride < 0 ? base + 24 : base;
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 8; ++x) top[int64_t(y) * stride + x] = BYTE(7 + y * 33 + x);
        check(SUCCEEDED(buffer->Unlock()) && SUCCEEDED(buffer->SetCurrentLength(32)), "publish ordinary sample bytes");
        ComPtr<IMFSample> sample; check(SUCCEEDED(MFCreateSample(&sample)), "create ordinary MF sample");
        check(SUCCEEDED(sample->AddBuffer(buffer.Get())), "attach ordinary buffer");
        lapse::Frame output;
        check(SUCCEEDED(lapse::copySample(sample.Get(), {2, 3, stride}, output)) && output.valid(), "copy ordinary padded rows");
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 8; ++x)
            check(output.pixels[size_t(y) * 8 + x] == BYTE(7 + y * 33 + x), "ordinary row orientation preserved");
        check(SUCCEEDED(buffer->SetCurrentLength(31)), "truncate ordinary sample");
        check(FAILED(lapse::copySample(sample.Get(), {2, 3, stride}, output)), "reject truncated ordinary sample");
        check(SUCCEEDED(buffer->Lock(&base, &maximum, nullptr)), "truncation rejection releases memory lock");
        check(SUCCEEDED(buffer->Unlock()), "release verification lock");
    }
    std::cout << "ordinary_padded_samples passed\n";
}

class Reader final : public IMFSourceReader {
public:
    Reader() {
        check(SUCCEEDED(MFCreateMediaType(&type_)), "create synthetic reader type");
        check(SUCCEEDED(type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
              SUCCEEDED(type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
              SUCCEEDED(MFSetAttributeSize(type_.Get(), MF_MT_FRAME_SIZE, 2, 2)) &&
              SUCCEEDED(type_->SetUINT32(MF_MT_DEFAULT_STRIDE, 8)), "set synthetic reader type");
    }
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMFSourceReader)) return E_NOINTERFACE;
        *out = static_cast<IMFSourceReader*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
    STDMETHODIMP GetStreamSelection(DWORD, BOOL*) override { return E_NOTIMPL; }
    STDMETHODIMP SetStreamSelection(DWORD, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP GetNativeMediaType(DWORD, DWORD, IMFMediaType**) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentMediaType(DWORD, IMFMediaType** out) override { return type_.CopyTo(out); }
    STDMETHODIMP SetCurrentMediaType(DWORD, DWORD*, IMFMediaType*) override { return E_NOTIMPL; }
    STDMETHODIMP SetCurrentPosition(REFGUID, REFPROPVARIANT) override { return E_NOTIMPL; }
    STDMETHODIMP ReadSample(DWORD, DWORD, DWORD*, DWORD*, LONGLONG*, IMFSample**) override { ++reads; return S_OK; }
    STDMETHODIMP Flush(DWORD) override { return S_OK; }
    STDMETHODIMP GetServiceForStream(DWORD, REFGUID, REFIID, LPVOID*) override { return E_NOTIMPL; }
    STDMETHODIMP GetPresentationAttribute(DWORD, REFGUID, PROPVARIANT*) override { return E_NOTIMPL; }
    int reads = 0;
private:
    std::atomic<ULONG> refs_{1};
    ComPtr<IMFMediaType> type_;
};

struct Reentry {
    std::mutex mutex;
    std::condition_variable ready;
    std::thread worker;
    bool completed = false, completedDuringRelease = false, failed = false;
    int releases = 0;
    void join() { if (worker.joinable()) worker.join(); }
    ~Reentry() { join(); }
};

class Cleanup final : public IUnknown {
public:
    Cleanup(lapse::CameraCallback* callback, Reentry& state) : callback_(callback), state_(state) {
        check(SUCCEEDED(MFCreateMediaEvent(MEError, GUID_NULL, E_ABORT, nullptr, &event_)), "create actual MF cleanup event");
    }
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = --refs_;
        if (!n) {
            ++state_.releases;
            try {
                state_.worker = std::thread([callback = ComPtr<lapse::CameraCallback>(callback_), event = event_, state = &state_]() mutable {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    bool failed = FAILED(com);
                    if (!failed) {
                        try { failed = FAILED(callback->OnEvent(0, event.Get())); } catch (...) { failed = true; }
                    }
                    event.Reset(); callback.Reset();
                    if (SUCCEEDED(com)) CoUninitialize();
                    { std::lock_guard<std::mutex> lock(state->mutex); state->failed = failed; state->completed = true; }
                    state->ready.notify_all();
                });
                std::unique_lock<std::mutex> lock(state_.mutex);
                state_.completedDuringRelease = state_.ready.wait_for(lock, std::chrono::seconds(2), [&] { return state_.completed; });
            } catch (...) { std::lock_guard<std::mutex> lock(state_.mutex); state_.failed = true; }
            delete this;
        }
        return n;
    }
private:
    std::atomic<ULONG> refs_{1};
    lapse::CameraCallback* callback_;
    Reentry& state_;
    ComPtr<IMFMediaEvent> event_;
};

bool runCase(const char* mode) {
    auto state = std::make_shared<lapse::CameraState>();
    ComPtr<Reader> reader; reader.Attach(new Reader);
    ComPtr<lapse::CameraCallback> callback; callback.Attach(new lapse::CameraCallback(state));
    Reentry reentry;
    lapse::Camera camera;
    camera.impl_->state = state; camera.impl_->reader = reader;
    state->running = true; state->reader = reader.Get(); state->format = {2, 2, 8};
    ComPtr<IMFSample> sample;
    check(SUCCEEDED(MFCreateSample(&sample)), "create actual MF sample");
    ComPtr<Cleanup> cleanup; cleanup.Attach(new Cleanup(callback.Get(), reentry));
    const GUID key{0x1825a1f4, 0x272c, 0x4da2, {0x8b, 0x3f, 0x31, 0x7a, 0x12, 0xfe, 0x99, 0xb0}};
    check(SUCCEEDED(sample->SetUnknown(key, cleanup.Get())), "attach cleanup through real sample attribute store");
    cleanup.Reset(); state->sample = sample; sample.Reset();
    const bool stopping = std::strcmp(mode, "stop") == 0;
    const bool changing = std::strcmp(mode, "format_change") == 0;
    ComPtr<IMFSample> next;
    if (!stopping) check(SUCCEEDED(MFCreateSample(&next)), "create replacement sample");
    if (stopping) camera.stop();
    else callback->OnReadSample(S_OK, 0, changing ? MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED : 0, 0, next.Get());
    reentry.join();
    check(reentry.releases == 1 && reentry.completed && !reentry.failed, "cleanup callback eventually completes without throwing across Release");
    if (stopping) check(!state->running && state->reader == nullptr && state->failure == S_OK, "late cleanup event cannot revive stopped state");
    else check(!state->running && state->failure == E_ABORT && state->sample.Get() == next.Get(), "active cleanup error remains terminal and replacement survives");
    std::cout << "cleanup_mode=" << mode << " callback_completed_before_release_return=" << reentry.completedDuringRelease
              << " callback_completed_after_join=" << reentry.completed << " reads=" << reader->reads << '\n';
    camera.stop();
    return reentry.completedDuringRelease;
}

namespace {
ComPtr<IMFSample> generatedFreshnessSample(int width, int height, BYTE value) {
    const DWORD size = DWORD(width * height * 4);
    ComPtr<IMFMediaBuffer> buffer;
    check(SUCCEEDED(MFCreateMemoryBuffer(size, &buffer)), "create generated freshness buffer");
    BYTE* pixels = nullptr;
    check(SUCCEEDED(buffer->Lock(&pixels, nullptr, nullptr)), "lock generated freshness buffer");
    std::memset(pixels, value, size);
    check(SUCCEEDED(buffer->Unlock()) && SUCCEEDED(buffer->SetCurrentLength(size)), "finish generated freshness buffer");
    ComPtr<IMFSample> sample;
    check(SUCCEEDED(MFCreateSample(&sample)) && SUCCEEDED(sample->AddBuffer(buffer.Get())), "create generated freshness sample");
    return sample;
}
bool freshnessPixels(const lapse::Frame& frame, int width, int height, BYTE value) {
    return frame.valid() && frame.width == width && frame.height == height &&
        std::all_of(frame.pixels.begin(), frame.pixels.end(), [value](BYTE pixel) { return pixel == value; });
}
struct FreshnessCamera {
    std::shared_ptr<lapse::CameraState> state = std::make_shared<lapse::CameraState>();
    ComPtr<Reader> reader;
    ComPtr<lapse::CameraCallback> callback;
    lapse::Camera camera;
    FreshnessCamera() {
        reader.Attach(new Reader);
        callback.Attach(new lapse::CameraCallback(state));
        camera.impl_->state = state; camera.impl_->reader = reader;
        state->reader = reader.Get(); state->running = true; state->format = {2, 2, 8};
        state->started = state->received = lapse::Clock::now();
    }
    void gap() {
        check(SUCCEEDED(callback->OnReadSample(S_OK, 0,
            MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_STREAMTICK, 0, nullptr)),
            "deliver no-sample format/gap callback");
    }
};
void formatChangeFreshness() {
    lapse::Frame frame;
    std::wstring error;
    uint64_t tick = 0;
    {
        FreshnessCamera initial;
        check(!initial.camera.latest(frame, error, tick) && error.empty(), "initial activation permits first-frame warmup");
        // Supply aged owned timestamps instead of waiting three seconds. Empty
        // type changes must not extend the original first-frame deadline.
        const auto expired = lapse::Clock::now() - std::chrono::seconds(4);
        initial.state->started = initial.state->received = expired;
        initial.gap(); initial.gap();
        check(!initial.camera.latest(frame, error, tick) &&
              error.find(L"No fresh camera frame arrived for 3 seconds.") == 0 &&
              initial.state->received == expired && !initial.state->sample && initial.state->receivedTick == 0,
              "repeated no-sample notifications extended the initial activation deadline");
        std::cout << "PASS initial warmup and unchanged first-frame deadline across repeated format gaps\n";
    }
    FreshnessCamera active;
    // Model an established session; the actual callback records receipt of
    // every generated sample. No clock or media API is replaced.
    active.state->started = lapse::Clock::now() - std::chrono::seconds(10);
    auto first = generatedFreshnessSample(2, 2, 73);
    check(SUCCEEDED(active.callback->OnReadSample(S_OK, 0, 0, 0, first.Get())), "deliver first real sample callback");
    check(active.camera.latest(frame, error, tick) && error.empty() && freshnessPixels(frame, 2, 2, 73),
          "healthy established camera did not copy its real sample pixels");
    const auto received = active.state->received;
    const auto firstTick = tick;
    ComPtr<IMFMediaType> changed;
    check(SUCCEEDED(active.reader->GetCurrentMediaType(lapse::videoStream, &changed)) &&
          SUCCEEDED(MFSetAttributeSize(changed.Get(), MF_MT_FRAME_SIZE, 4, 2)) &&
          SUCCEEDED(changed->SetUINT32(MF_MT_DEFAULT_STRIDE, 16)), "change synthetic reader's real media type");
    active.gap();
    check(!active.state->sample && active.state->format.width == 4 && active.state->format.height == 2 &&
          active.state->format.stride == 16 && active.state->running && SUCCEEDED(active.state->failure),
          "format notification must retire only the obsolete-format sample");
    check(!active.camera.latest(frame, error, tick) && error.empty() && active.state->received == received &&
          active.state->receivedTick == firstTick, "fresh prior arrival became stale when its old-format cache was retired");
    active.gap(); active.gap();
    check(!active.camera.latest(frame, error, tick) && error.empty() && active.state->received == received &&
          active.state->receivedTick == firstTick && active.reader->reads == 4,
          "repeated recent format notifications must wait without refreshing the arrival deadline");
    std::cout << "PASS established camera waits across format gaps without returning old-format pixels\n";
    auto second = generatedFreshnessSample(4, 2, 151);
    check(SUCCEEDED(active.callback->OnReadSample(S_OK, 0, 0, 0, second.Get())), "deliver replacement-format sample");
    check(active.camera.latest(frame, error, tick) && error.empty() && freshnessPixels(frame, 4, 2, 151) &&
          active.reader->reads == 5, "replacement-format sample did not resume exact pixel delivery");
    std::cout << "PASS actual callback resumes exact new-format pixel delivery\n";
    const auto expired = lapse::Clock::now() - std::chrono::seconds(4);
    active.state->received = expired;
    const auto lastTick = active.state->receivedTick;
    active.gap(); active.gap();
    check(!active.camera.latest(frame, error, tick) &&
          error.find(L"No fresh camera frame arrived for 3 seconds.") == 0 &&
          active.state->received == expired && active.state->receivedTick == lastTick && !active.state->sample,
          "repeated format changes hid a genuinely stale established camera");
    std::cout << "PASS repeated format gaps retain the established three-second stale-frame limit\n";
}
void sourceSampleIdentity() {
    FreshnessCamera source;
    auto sample = generatedFreshnessSample(2, 2, 42);
    check(SUCCEEDED(source.callback->OnReadSample(S_OK, 0, 0, 0, sample.Get())), "deliver zero-timestamp source sample");
    lapse::Frame frame;
    lapse::CameraSampleInfo first, repeated, second, changed;
    std::wstring error;
    check(source.camera.latestNewer(frame, error, first, {}) && first.epoch && first.sequence == 1 &&
        first.timestampValid && first.timestamp100ns == 0 && freshnessPixels(frame, 2, 2, 42),
        "first camera sample lacks unique source identity or valid zero timestamp");
    ComPtr<IMFMediaBuffer> buffer;
    check(SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer->SetCurrentLength(0)), "make already-consumed sample uncopyable");
    const auto oldPixels = frame.pixels;
    check(!source.camera.latestNewer(frame, error, repeated, first) && error.empty() && repeated.sequence == first.sequence &&
        repeated.receivedTick == first.receivedTick && frame.pixels == oldPixels,
        "repeated retained sample was converted or refreshed its identity");
    check(SUCCEEDED(buffer->SetCurrentLength(16)), "restore owned source sample");
    source.callback->OnReadSample(S_OK, 0, MF_SOURCE_READERF_STREAMTICK, 999, nullptr);
    check(!source.camera.latestNewer(frame, error, repeated, first) && error.empty() && repeated.sequence == first.sequence &&
        repeated.timestamp100ns == 0 && repeated.receivedTick == first.receivedTick,
        "null stream tick manufactured a source sample or timestamp");
    source.callback->OnReadSample(S_OK, 0, 0, 0, sample.Get());
    check(source.camera.latestNewer(frame, error, second, first) && second.sequence == first.sequence + 1 &&
        second.epoch == first.epoch && second.timestampValid && second.timestamp100ns == 0,
        "genuine identical-pixel callback lost its new delivery sequence");
    check(SUCCEEDED(sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE)), "mark source timeline discontinuity");
    source.callback->OnReadSample(S_OK, 0, 0, -100, sample.Get());
    check(source.camera.latestNewer(frame, error, changed, second) && changed.epoch != second.epoch && changed.discontinuity &&
        changed.sequence == second.sequence + 1 && changed.timestamp100ns == -100,
        "source discontinuity failed to invalidate its epoch and retain media time");
    check(SUCCEEDED(sample->SetUINT32(MFSampleExtension_Discontinuity, FALSE)), "clear synthetic discontinuity");
    ComPtr<IMFMediaType> type;
    check(SUCCEEDED(source.reader->GetCurrentMediaType(lapse::videoStream, &type)) &&
        SUCCEEDED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, 7)), "set synthetic transfer provenance");
    source.callback->OnReadSample(S_OK, 0, MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED, 100, sample.Get());
    lapse::CameraSampleInfo transfer;
    check(source.camera.latestNewer(frame, error, transfer, changed) && transfer.epoch != changed.epoch && transfer.transferFunction == 7 &&
        !transfer.discontinuity, "same-size format/color change failed to invalidate the source epoch");
    std::cout << "PASS source epoch/sequence, valid zero timestamps, null ticks, duplicate-copy suppression and transfer provenance\n";
}
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 2;
    const HRESULT media = MFStartup(MF_VERSION);
    if (FAILED(media)) { CoUninitialize(); return 2; }
    int result = 0;
    try {
        legacySamples(); modernSamples(); ordinarySamples();
        for (const char* mode : {"stop", "replace", "format_change"})
            check(runCase(mode), "Sample cleanup could not reenter callback before Release returned");
        formatChangeFreshness();
        sourceSampleIdentity();
        std::cout << "Synthetic native camera buffer and sample-lifetime checks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    MFShutdown(); CoUninitialize(); return result;
}

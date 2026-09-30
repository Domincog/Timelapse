// Owned synthetic source, real child helper and shared-memory transport.
#include "camera_helper_fixture.h"

namespace {
lapse::CameraObservation observe(Harness& h, uint64_t token, uint32_t waitMs = 2000) {
    lapse::CameraObservation output; std::wstring error;
    const uint64_t until = GetTickCount64() + waitMs;
    while (GetTickCount64() < until) {
        if (h.client.observeActivity(token, output, error)) return output;
        if (!error.empty()) { std::wcerr << error << L'\n'; throw std::runtime_error("activity observation failed"); }
        Sleep(15);
    }
    throw std::runtime_error("activity observation did not arrive");
}
void baselineAndCoalescing() {
    Harness h(L"night-live"); h.first();
    const LONG idle = h.control().calls; Sleep(150);
    require(h.control().calls == idle, "disabled observation introduced background capture");
    uint64_t generation = 0;
    h.inspect([&](auto& shared) { generation = shared.generation; });
    const auto first = observe(h, 1);
    require(first.epoch == 1 && first.sequence && first.receivedTick && first.sourceWidth == 64 && first.sourceHeight == 36,
            "observation lost actual source provenance");
    require(first.descriptor.y[0] == 32 && first.descriptor.u[0] == 128 && first.descriptor.v[0] == 128,
            "raw gray source descriptor was not transported");
    h.inspect([&](auto& shared) { require(shared.generation == generation, "small report mutated full pixel publication"); });
    Sleep(60); const auto next = observe(h, 1);
    require(next.sequence > first.sequence, "retained image was relabelled as a distinct observation");
    h.client.cancelActivityObservation(); Sleep(100);
    const LONG stopped = h.control().calls; Sleep(150);
    require(stopped == h.control().calls, "cancelled observer kept polling");

    // Queue both consumers while this thread owns the recursive shared mutex.
    // The helper must satisfy them from one raw conversion when released.
    Sleep(60);
    require(WaitForSingleObject(h.mutex.value, 1000) == WAIT_OBJECT_0, "hold owned sharing mutex");
    lapse::CameraObservation pending; lapse::Frame preview; std::wstring error;
    h.client.observeActivity(2, pending, error);
    const bool queuedObservation = error.empty();
    h.client.latest(preview, error);
    const bool queuedPreview = error.empty();
    const LONG before = h.control().copies;
    ReleaseMutex(h.mutex.value);
    require(queuedObservation && queuedPreview, "queue independent consumers");
    observe(h, 2); h.first();
    require(h.control().copies - before == 1, "same-iteration preview and observation repeated the raw conversion");
    std::cout << "PASS disabled/paused demand, distinct raw provenance, independent pixels and shared conversion\n";
}
void pinnedNight() {
    Harness h(L"night-live"); h.first(); h.begin(100);
    lapse::Frame result; lapse::NightWindowResult facts; std::wstring error;
    const uint64_t until = GetTickCount64() + 4000;
    bool finished = false;
    while (GetTickCount64() < until) {
        const auto raw = observe(h, 4);
        require(raw.descriptor.y[0] == 32, "processed Night brightness leaked into raw observation");
        if (h.client.nightResult(100, result, facts, error)) { finished = true; break; }
        require(error.empty(), "observation interfered with full Night delivery");
        Sleep(80);
    }
    require(finished && facts.exposure.samples >= 3, "Night integration was starved by activity reports");
    const auto pixels = result.pixels;
    Sleep(60); observe(h, 4); h.first(); h.client.cancelActivityObservation();
    lapse::NightWindowResult again;
    const uint64_t deadline = GetTickCount64() + 1000;
    while (!h.client.nightResult(100, result, again, error) && error.empty() && GetTickCount64() < deadline) Sleep(10);
    require(error.empty() && result.pixels == pixels && again.beginTick == facts.beginTick,
            "observation or cancellation replaced the pinned completed Night image");
    std::cout << "PASS simultaneous raw checking, preview and immutable Night result\n";
}
void duplicateDeadlineAndIsolation() {
    {
        Harness h(L"night-duplicate"); h.first(); observe(h, 10);
        const LONG copies = h.control().copies;
        lapse::CameraObservation output; output.epoch = 991;
        std::wstring error; const uint64_t began = GetTickCount64();
        while (GetTickCount64() - began < 3600) {
            require(!h.client.observeActivity(10, output, error), "duplicate sample counted as a fresh check");
            if (!error.empty()) break;
            Sleep(40);
        }
        require(!error.empty() && GetTickCount64() - began < 3400 && output.epoch == 991,
                "duplicate acknowledgements extended deadline or changed caller output");
        require(h.control().copies == copies, "duplicate retry repeatedly converted full source pixels");
    }
    {
        Harness h(L"night-live"); h.first(); InterlockedExchange(&h.control().throwCopy, 1);
        lapse::CameraObservation output; std::wstring error;
        const uint64_t until = GetTickCount64() + 1000;
        while (GetTickCount64() < until && error.empty()) { h.client.observeActivity(11, output, error); Sleep(10); }
        require(!error.empty(), "optional allocation failure was not reported");
        InterlockedExchange(&h.control().throwCopy, 0);
        h.first(); observe(h, 12);
    }
    std::cout << "PASS duplicate suppression, fixed deadline and isolated optional allocation failure\n";
}
void cancellationAndProvenance() {
    Harness h(L"night-live"); h.first(); observe(h, 20); Sleep(60);
    InterlockedExchange(&h.control().readGate, 1);
    lapse::CameraObservation output; std::wstring error;
    require(!h.client.observeActivity(21, output, error) && error.empty(), "gated observation completed unexpectedly");
    const uint64_t until = GetTickCount64() + 1000;
    while (!h.control().readReached && GetTickCount64() < until) Sleep(1);
    require(h.control().readReached, "synthetic raw read did not reach its gate");
    h.inspect([](auto& shared) {
        shared.observationCompleted = shared.observationRequested;
        shared.observationCompletedToken = shared.observationToken;
        shared.observationState = lapse::ObservationState::Ready;
        // The bytes still belong to token20; the forged acknowledgement says21.
    });
    require(!h.client.observeActivity(21, output, error) && !error.empty(),
            "a new acknowledgement relabelled another session's descriptor");
    InterlockedExchange(&h.control().readRelease, 1); InterlockedExchange(&h.control().readGate, 0);
    observe(h, 22);

    TestHandle locked, released;
    locked.value = CreateEventW(nullptr, TRUE, FALSE, nullptr); released.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(locked.value && released.value, "create private contention gates");
    std::thread holder([&] {
        if (WaitForSingleObject(h.mutex.value, 1000) == WAIT_OBJECT_0) {
            SetEvent(locked.value); WaitForSingleObject(released.value, 2000); ReleaseMutex(h.mutex.value);
        }
    });
    const bool held = WaitForSingleObject(locked.value, 1500) == WAIT_OBJECT_0;
    const uint64_t began = GetTickCount64(); h.client.cancelActivityObservation(); const uint64_t elapsed = GetTickCount64() - began;
    SetEvent(released.value); holder.join();
    require(held && elapsed < 100, "observation cancellation waited on the transport mutex");
    const auto resumed = observe(h, 23);
    require(resumed.epoch == 1, "cancelled observer could not restart independently");
    std::cout << "PASS acknowledgement provenance, in-flight replacement and lock-independent cancellation\n";
}
void nightDeliveryPriority() {
    Harness h(L"night-live"); h.first(); h.begin(200);
    lapse::Frame frame; const auto facts = h.completed(200, frame); const auto pixels = frame.pixels;
    InterlockedExchange(&h.control().readGate, 1);
    require(WaitForSingleObject(h.mutex.value, 1000) == WAIT_OBJECT_0, "hold owned mutex for priority requests");
    lapse::CameraObservation report; lapse::NightWindowResult again; std::wstring error;
    h.client.observeActivity(40, report, error);
    const bool observationQueued = error.empty();
    const bool premature = h.client.nightResult(200, frame, again, error);
    const bool nightQueued = error.empty();
    ReleaseMutex(h.mutex.value);
    const uint64_t until = GetTickCount64() + 700;
    bool ready = premature;
    while (!ready && error.empty() && GetTickCount64() < until) {
        ready = h.client.nightResult(200, frame, again, error); Sleep(5);
    }
    InterlockedExchange(&h.control().readRelease, 1); InterlockedExchange(&h.control().readGate, 0);
    h.client.cancelActivityObservation();
    require(observationQueued && nightQueued && ready && error.empty() && frame.pixels == pixels && again.beginTick == facts.beginTick,
            "optional raw conversion blocked an already-complete Night delivery");
    std::cout << "PASS completed Night pixels take priority over a blocked optional source read\n";
}
void nightSampleReuse() {
    Harness h(L"night-live"); h.first(); h.begin(300, 2000);
    bool integrating = false;
    const uint64_t until = GetTickCount64() + 1000;
    while (!integrating && GetTickCount64() < until) {
        h.inspect([&](auto& shared) { integrating = shared.nightState == lapse::NightState::Integrating; });
        if (!integrating) Sleep(2);
    }
    require(integrating, "synthetic Night window did not begin");
    const LONG64 rawAddress = InterlockedCompareExchange64(&h.control().lastFrameAddress, 0, 0);
    InterlockedExchange64(&h.control().expectedFrameAddress, rawAddress);
    const auto first = observe(h, 50);
    Sleep(40); const auto second = observe(h, 50);
    const LONG extra = h.control().differentBufferReads;
    InterlockedExchange64(&h.control().expectedFrameAddress, 0);
    h.client.cancelActivityObservation();
    require(rawAddress && first.sequence < second.sequence && extra == 0,
            "activity requests added a separate raw conversion during Night integration");
    lapse::Frame frame; h.completed(300, frame);
    std::cout << "PASS in-progress Night windows share raw contributions without optional extra reads\n";
}
void checkRawPreviewPixels(const lapse::Frame& frame, bool large) {
    require(frame.width == (large ? 1280 : 64) && frame.height == (large ? 720 : 36), "raw preview geometry changed");
    for (size_t i = 0; i < frame.pixels.size(); i += 4)
        require(frame.pixels[i] == 32 && frame.pixels[i+1] == 32 && frame.pixels[i+2] == 32 && frame.pixels[i+3] == 255,
                "preview did not contain exact unprocessed source pixels");
}
void nightPreviewReuse(bool large, bool integrating, bool observing) {
    Harness h(large ? L"night-720p" : L"night-live"); h.first();
    if (integrating) {
        h.begin(1, 2000);
        bool ready = false; const auto until = GetTickCount64() + 2000;
        while (!ready && GetTickCount64() < until) {
            h.inspect([&](auto& shared) { ready = shared.nightState == lapse::NightState::Integrating; });
            if (!ready) Sleep(1);
        }
        require(ready, "Night watermark did not start a full window");
    }
    require(WaitForSingleObject(h.mutex.value, 1000) == WAIT_OBJECT_0, "hold private transport");
    bool queued = false; LONG before = 0; lapse::Frame frame; std::wstring error;
    try {
        if (!integrating) h.begin(1, 2000);
        if (observing) { lapse::CameraObservation output; h.client.observeActivity(9, output, error); require(error.empty(), "queue optional observation"); }
        queued = !h.client.latest(frame, error) && error.empty();
        // The helper cannot accept these requests until release. Ensure its
        // next integration read is due in the same iteration as the preview.
        if (integrating) Sleep(250);
        before = h.control().copies;
    } catch (...) { ReleaseMutex(h.mutex.value); throw; }
    ReleaseMutex(h.mutex.value);
    require(queued, "preview unexpectedly completed behind held transport");
    bool received = false; const auto until = GetTickCount64() + 1000;
    while (!received && GetTickCount64() < until) {
        received = h.client.latest(frame, error); require(error.empty(), "combined preview failed");
        if (!received) Sleep(1);
    }
    require(received, "combined preview did not arrive"); checkRawPreviewPixels(frame, large);
    const LONG delta = h.control().copies - before;
    h.inspect([](auto& shared) {
        require(shared.publishedRequest == shared.completed && shared.publishedKind == lapse::PixelRequest::Preview &&
            shared.publishedToken == 0, "raw preview lost request ownership");
        require(shared.receivedTick == shared.nightSourceTick, "raw preview replaced the reused source receipt timestamp");
    });
    require(delta == 1, "Preview repeated an already available raw Night conversion");
    if (observing) {
        lapse::CameraObservation output; bool ready = false; const auto end = GetTickCount64() + 500;
        while (!ready && GetTickCount64() < end) { ready = h.client.observeActivity(9, output, error); require(error.empty(), "read shared observation"); if (!ready) Sleep(1); }
        require(ready && output.descriptor.y[0] == 32 && output.sourceWidth == frame.width,
            "observation lost raw provenance or pixels");
    }
    std::cout << "PASS shared raw preview size=" << (large ? "720p" : "64x36") << " integrating=" << integrating
        << " observation=" << observing << " rawCopies=" << delta << " exactPixels=true\n";
    h.client.cancelNight(); h.client.cancelActivityObservation();
}
void nightPreviewFallback(const wchar_t* mode, const wchar_t* diagnostic) {
    Harness h(mode); h.first(); h.begin(1, 2000);
    bool ready = false; const auto until = GetTickCount64() + 2000;
    while (!ready && GetTickCount64() < until) {
        h.inspect([&](auto& shared) { ready = shared.nightState == lapse::NightState::Integrating; });
        if (!ready) Sleep(1);
    }
    require(ready, "fault control did not start Night window");
    require(WaitForSingleObject(h.mutex.value, 1000) == WAIT_OBJECT_0, "hold fault control transport");
    lapse::Frame frame; std::wstring error; bool queued = false; LONG before = 0;
    try { queued = !h.client.latest(frame,error) && error.empty(); Sleep(250); before = h.control().copies; }
    catch (...) { ReleaseMutex(h.mutex.value); throw; }
    ReleaseMutex(h.mutex.value); require(queued, "fault preview was not queued");
    ready = false; const auto end = GetTickCount64() + 1000;
    while (!ready && GetTickCount64() < end) {
        ready = h.client.latest(frame,error); require(error.empty(), "invalid Night raw input contaminated ordinary preview");
        if (!ready) Sleep(1);
    }
    require(ready, "fresh fallback preview missing"); checkRawPreviewPixels(frame,false);
    require(h.control().copies - before == 2, "invalid Night raw frame bypassed normal preview conversion");
    if (diagnostic) require(h.failed(1).find(diagnostic) != std::wstring::npos, "Night lost its independent failure diagnostic");
    h.inspect([](auto& shared) {
        require(shared.publishedKind == lapse::PixelRequest::Preview && shared.publishedToken == 0 &&
            shared.receivedTick <= GetTickCount64() && GetTickCount64() - shared.receivedTick < 1000,
            "fallback preview lost typed publication or actual fresh receipt time");
    });
    h.client.cancelNight();
    std::wcout << L"PASS raw preview fallback " << mode << L" rawCopies=2 exactPreview=true nightFailureChecked="
        << (diagnostic != nullptr) << L'\n';
}
}
int main() {
    const int host = lapse::runCameraHost(nullptr); if (host >= 0) return host;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 2;
    int result = 0;
    try { baselineAndCoalescing(); pinnedNight(); duplicateDeadlineAndIsolation(); cancellationAndProvenance(); nightDeliveryPriority(); nightSampleReuse();
        for (bool large : {false,true}) for (bool integrating : {false,true}) for (bool observing : {false,true})
            nightPreviewReuse(large,integrating,observing);
        nightPreviewFallback(L"night-reuse-stale", L"No fresh");
        nightPreviewFallback(L"night-reuse-shape", L"unsupported");
        nightPreviewFallback(L"night-reuse-future", L"invalid source freshness");
        nightPreviewFallback(L"night-reuse-zero", nullptr); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    CoUninitialize(); return result;
}

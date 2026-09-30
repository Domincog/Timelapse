// Generated camera pixels, actual child process and thumbnail transport only.
#include "camera_helper_fixture.h"

namespace {
std::unique_ptr<lapse::CameraPersonInput> person(Harness& h, uint64_t token) {
    auto input = std::make_unique<lapse::CameraPersonInput>();
    std::wstring error; const auto until = GetTickCount64() + 2000;
    bool first = true;
    while (GetTickCount64() < until) {
        if (h.client.personInput(token, *input, error, first)) return input;
        require(error.empty(), "person input failed"); first = false; Sleep(5);
    }
    throw std::runtime_error("person input timed out");
}
void pixelsAndDemand() {
    Harness h(L"night-live"); h.first();
    const std::wstring name = std::wstring(h.control().name) + L".person";
    TestHandle absent; absent.value = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    require(!absent.value, "disabled camera allocated optional person mapping");
    auto output = std::make_unique<lapse::CameraPersonInput>(); std::wstring error;
    for (int i = 0; i < 10; ++i) require(!h.client.personInput(1, *output, error, false) && error.empty(), "poll created a request");
    uint64_t generation = 0; h.inspect([&](auto& shared) { generation = shared.generation; require(!shared.observationRequested, "poll created work"); });
    const auto first = person(h, 1); const auto& source = first->source;
    require(source.sessionToken == 1 && source.cameraEpoch == 1 && source.sequence && source.receivedTick &&
        source.sourceWidth == 64 && source.sourceHeight == 36 && source.width == 320 && source.height == 180,
        "source identity or full-scene aspect lost");
    require(std::all_of(first->bgr.begin(), first->bgr.begin() + 320 * 180 * 3, [](uint8_t value) { return value == 32; }),
        "BGRA-to-BGR resize corrupted raw pixels");
    h.inspect([&](auto& shared) { require(shared.generation == generation && shared.observationPublishedKind == lapse::ObservationKind::Person,
        "person image changed required pixel slot or lost type"); });
    h.client.cancelActivityObservation(); Sleep(80); const LONG calls = h.control().calls; Sleep(120);
    require(h.control().calls == calls, "cancelled person request kept waking camera");
    lapse::CameraObservation activity;
    const auto until = GetTickCount64() + 1500;
    bool ready = false;
    while (!ready && GetTickCount64() < until) { ready = h.client.observeActivity(2, activity, error); require(error.empty(), "activity afterperson failed"); Sleep(5); }
    require(ready && activity.descriptor.y[0] == 32, "person bytes were reinterpreted as image-change descriptor");
    std::cout << "PASS lazy person mapping, polling without work, exact raw pixels, provenance and mode separation\n";
}
void duplicatePolling() {
    Harness h(L"night-duplicate"); h.first(); person(h, 10);
    auto output = std::make_unique<lapse::CameraPersonInput>(); std::wstring error;
    require(!h.client.personInput(10, *output, error, true) && error.empty(), "duplicate produced result");
    Sleep(50); const LONG calls = h.control().calls;
    for (int i = 0; i < 30; ++i) {
        require(!h.client.personInput(10, *output, error, false) && error.empty(), "duplicate poll completed"); Sleep(5);
    }
    require(h.control().calls == calls, "frequent polling re-signalled duplicate source requests");
    h.client.cancelActivityObservation();
    std::cout << "PASS fixed request cadence is independent from fast completion polling\n";
}
void corruptGeometryAndKind() {
    Harness h(L"night-live"); h.first(); person(h, 20); Sleep(60);
    auto output = std::make_unique<lapse::CameraPersonInput>(); output->source.sequence = 777;
    std::wstring error;
    InterlockedExchange(&h.control().readGate, 1);
    require(!h.client.personInput(21, *output, error), "gated source already returned");
    const auto until = GetTickCount64() + 1000;
    while (!h.control().readReached && GetTickCount64() < until) Sleep(1);
    require(h.control().readReached, "source gate not reached");
    h.inspect([](auto& shared) {
        shared.observationCompleted = shared.observationRequested;
        shared.observationCompletedToken = shared.observationToken;
        shared.observationCompletedKind = lapse::ObservationKind::Activity;
        shared.observationState = lapse::ObservationState::Ready;
    });
    require(!h.client.personInput(21, *output, error, false) && !error.empty() && output->source.sequence == 777,
        "wrong response type exposed retained person data");
    InterlockedExchange(&h.control().readRelease, 1); InterlockedExchange(&h.control().readGate, 0);
    person(h, 22); Sleep(60);
    require(!h.client.personInput(23, *output, error), "new request completed before child runs");
    bool published = false;
    const auto deadline = GetTickCount64() + 1000;
    while (!published && GetTickCount64() < deadline) {
        h.inspect([&](auto& shared) {
            if (shared.observationCompletedToken == 23 && shared.observationState == lapse::ObservationState::Ready) {
                shared.personSource.width = 321; published = true;
            }
        });
        if (!published) Sleep(2);
    }
    require(published, "owned forged response not ready");
    require(!h.client.personInput(23, *output, error, false) && !error.empty() && output->source.sequence == 777,
        "invalid thumbnail dimensions were copied");
    std::cout << "PASS typed reply and geometry rejection leave caller untouched\n";
}
void nightReuse() {
    Harness h(L"night-live"); h.first(); h.begin(90, 2000);
    bool integrating = false; const auto until = GetTickCount64() + 1000;
    while (!integrating && GetTickCount64() < until) {
        h.inspect([&](auto& shared) { integrating = shared.nightState == lapse::NightState::Integrating; }); Sleep(2);
    }
    require(integrating, "Night did not begin");
    const LONG64 address = InterlockedCompareExchange64(&h.control().lastFrameAddress, 0, 0);
    InterlockedExchange64(&h.control().expectedFrameAddress, address);
    auto input = person(h, 30);
    const LONG extra = h.control().differentBufferReads;
    InterlockedExchange64(&h.control().expectedFrameAddress, 0);
    h.client.cancelActivityObservation();
    require(address && !extra && input->bgr[0] == 32, "person checks requested separate Night raw reads or processed pixels");
    lapse::Frame frame; const auto facts = h.completed(90, frame); const auto original = frame.pixels;
    require(facts.exposure.samples >= 3 && frame.pixels[0] != 32, "Night did not finish its full processed image");
    InterlockedExchange(&h.control().readGate, 1);
    require(WaitForSingleObject(h.mutex.value, 1000) == WAIT_OBJECT_0, "hold request mutex");
    std::wstring error; lapse::NightWindowResult again;
    h.client.personInput(31, *input, error); const bool queued = error.empty();
    bool ready = h.client.nightResult(90, frame, again, error); ReleaseMutex(h.mutex.value);
    const auto deadline = GetTickCount64() + 700;
    while (!ready && error.empty() && GetTickCount64() < deadline) { ready = h.client.nightResult(90, frame, again, error); Sleep(5); }
    InterlockedExchange(&h.control().readRelease, 1); InterlockedExchange(&h.control().readGate, 0);
    h.client.cancelActivityObservation();
    require(queued && ready && error.empty() && frame.pixels == original && again.beginTick == facts.beginTick,
        "optional thumbnail delayed or replaced completed Night output");
    std::cout << "PASS Night raw-sample reuse, full windows and completed-result priority\n";
}
}
int main() {
    const int host = lapse::runCameraHost(nullptr); if (host >= 0) return host;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 2;
    int result = 0;
    try { pixelsAndDemand(); duplicatePolling(); corruptGeometryAndKind(); nightReuse(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    CoUninitialize(); return result;
}

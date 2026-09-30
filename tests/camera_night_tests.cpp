#include "camera_helper_fixture.h"
namespace {
lapse::SharedFrame* remapHeader = nullptr;
void mutateOnFullMap(HANDLE, SIZE_T bytes) {
    if (bytes <= sizeof(lapse::SharedFrame)) return;
    remapHeader->resolution = lapse::CameraResolution::Detail1080;
    remapHeader->pixelCapacity = lapse::cameraCaptureLimits(lapse::CameraResolution::Detail1080).pixelBytes;
}
void mappingContract() {
    using namespace lapse;
    require(mappingBytes(CameraResolution::Standard720)==sizeof(SharedFrame)+3686400 &&
        mappingBytes(CameraResolution::Detail1080)==sizeof(SharedFrame)+8294400,
        "Camera mapping does not preserve selected payload size");
    CameraClient invalid; std::wstring error;
    require(!invalid.start(L"night-live",error,static_cast<CameraResolution>(99))&&!error.empty(),"Unknown camera tier launched");
    for(int mode=0;mode<5;++mode) {
        GUID guid{}; require(SUCCEEDED(CoCreateGuid(&guid)),"Create startup mapping identity");
        wchar_t token[40]{};StringFromGUID2(guid,token,40);
        const std::wstring name=std::wstring(mappingPrefix)+token;
        TestHandle mapping,mutex,stop,request,response; TestView view;
        const auto tier=mode==3?CameraResolution::Detail1080:CameraResolution::Standard720;
        const size_t bytes=mappingBytes(tier)-(mode==3?65536:0);
        mapping.value=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(bytes),name.c_str());
        mutex.value=CreateMutexW(nullptr,FALSE,(name+L".mutex").c_str());
        stop.value=CreateEventW(nullptr,TRUE,FALSE,(name+L".stop").c_str());
        request.value=CreateEventW(nullptr,FALSE,FALSE,(name+L".request").c_str());
        response.value=CreateEventW(nullptr,FALSE,FALSE,(name+L".response").c_str());
        require(mapping.value&&mutex.value&&stop.value&&request.value&&response.value,"Create owned startup mapping objects");
        view.value=MapViewOfFile(mapping.value,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(SharedFrame));
        require(view.value!=nullptr,"Map owned fixed header");
        auto& shared=*static_cast<SharedFrame*>(view.value);
        shared.magic=protocolMagic;shared.version=protocolVersion;shared.headerBytes=sizeof(SharedFrame);
        shared.resolution=tier;shared.pixelCapacity=cameraCaptureLimits(tier).pixelBytes;wcscpy_s(shared.id,L"never-activate-device");
        if(mode==0)shared.resolution=static_cast<CameraResolution>(99);
        if(mode==1)++shared.pixelCapacity;
        if(mode==2)--shared.headerBytes;
        struct ClearHook { ~ClearHook(){cameraMappingHook=nullptr;remapHeader=nullptr;} } clear;
        if(mode==4){remapHeader=&shared;cameraMappingHook=mutateOnFullMap;}
        require(hostMain(name.c_str())==(mode==3?4:5),"Malformed/truncated/remap-mutated mapping reached camera activation");
    }
    std::cout<<"PASS tier-sized mapping arithmetic, unknown/header/capacity rejection, actual truncated mapping and remap mutation\n";
}
void acceptedTierMutation() {
    using namespace lapse;
    for(bool changeTier:{false,true}) {
        Harness h(L"night-live");h.first();Frame output;std::wstring error;
        h.inspect([&](auto& shared){if(changeTier){shared.resolution=CameraResolution::Detail1080;shared.pixelCapacity=cameraCaptureLimits(CameraResolution::Detail1080).pixelBytes;}else ++shared.pixelCapacity;});
        require(!h.client.latest(output,error)&&error.find(L"invalid data")!=std::wstring::npos&&output.pixels.empty(),"Parent accepted mutated tier/capacity");
    }
    Harness h(L"night-live");h.first();Frame output;std::wstring error;
    InterlockedExchange(&h.control().readGate,1);
    require(!h.client.latest(output,error)&&error.empty(),"Gated publication unexpectedly completed");
    const auto until=GetTickCount64()+1000;
    while(!h.control().readReached&&GetTickCount64()<until)Sleep(1);
    require(h.control().readReached,"Owned helper did not reach source gate");
    uint64_t generation=0;
    h.inspect([&](auto& shared){generation=shared.generation;shared.resolution=CameraResolution::Detail1080;shared.pixelCapacity=cameraCaptureLimits(CameraResolution::Detail1080).pixelBytes;});
    TestHandle process;process.value=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,h.control().processId);
    require(process.value!=nullptr,"Open owned helper termination handle");
    InterlockedExchange(&h.control().readRelease,1);
    require(WaitForSingleObject(process.value,2000)==WAIT_OBJECT_0,"Helper did not reject changed header before publication");
    DWORD code=0;require(GetExitCodeProcess(process.value,&code)&&code==12,"Helper header mutation has wrong terminal outcome");
    h.inspect([&](auto& shared){require(shared.generation==generation,"Helper copied pixels after its accepted tier changed");});
    std::cout<<"PASS parent and helper retain original tier/capacity after acceptance\n";
}
void highTierNight() {
    Harness h(L"night-1080p",lapse::CameraResolution::Detail1080);h.first();
    h.begin(111);lapse::Frame output;const auto result=h.completed(111,output,true);
    require(output.valid()&&output.width==1920&&output.height==1080&&result.exposure.samples>=2&&
        result.endTick-result.beginTick==1000&&result.lastSampleTick<result.endTick,"1080p night lost geometry/window facts");
    const auto value=output.pixels[0];require(value>32,"1080p blend was raw preview");
    for(size_t p=0;p<output.pixels.size();p+=4)require(output.pixels[p]==value&&output.pixels[p+1]==value&&output.pixels[p+2]==value&&output.pixels[p+3]==255,"1080p blend payload tail or color damaged");
    h.client.cancelNight();h.first();
    std::cout<<"PASS full1080 Night pixels, exact full-window timing and raw preview separation\n";
}
void checkWindow(const lapse::NightWindowResult& result, const lapse::Frame& frame, uint32_t duration) {
    require(frame.valid() && frame.width == 64 && frame.height == 36 && frame.pixels[0] >= 32 && frame.pixels[3] == 255,
        "completed blend pixels are invalid");
    require(result.endTick - result.beginTick == duration && result.firstSampleTick >= result.beginTick &&
        result.lastSampleTick >= result.firstSampleTick && result.lastSampleTick < result.endTick &&
        result.exposure.samples > 0 && result.exposure.samples <= duration / lapse::NightCadenceMs,
        "window duration, observed sample span or contribution count is dishonest");
}
void previewAndStableResult() {
    Harness h(L"night-live"); h.first();
    const LONG baseline = h.control().calls; Sleep(300);
    require(h.control().calls == baseline, "night-off helper gained idle polling");
    h.begin(1); lapse::Frame first, second;
    const auto result = h.completed(1, first, true); checkWindow(result, first, 1000);
    require(!result.timestampFallback && result.exposure.samples >= 3, "healthy unique camera samples lost their timeline");
    h.first(); Sleep(100); h.first();
    const auto reread = h.completed(1, second);
    require(first.pixels == second.pixels && result.exposure.samples == reread.exposure.samples && result.beginTick == reread.beginTick,
        "preview consumed or replaced the immutable completed blend");
    h.client.cancelNight(); Sleep(100); const LONG stopped = h.control().calls; Sleep(300);
    require(h.control().calls == stopped, "cancelled helper retained periodic night work");
    std::cout << "PASS baseline demand, independent preview, unique source count, stable repeated result and idle cancellation\n";
}
void replaceAndCancel() {
    Harness h(L"night-live"); h.first(); h.begin(10, 2000); Sleep(300);
    h.begin(11); lapse::Frame frame; lapse::NightWindowResult result; std::wstring error;
    require(!h.client.nightResult(10, frame, result, error) && error.find(L"replaced") != std::wstring::npos,
        "replaced token consumed another request's result");
    const auto completed = h.completed(11, frame); checkWindow(completed, frame, 1000);
    h.begin(12, 2000); Sleep(300);
    TestHandle locked, released;
    locked.value = CreateEventW(nullptr, TRUE, FALSE, nullptr); released.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(locked.value && released.value, "create owned contention gates");
    std::thread holder([&] {
        const DWORD acquired = WaitForSingleObject(h.mutex.value, 1000);
        if (acquired == WAIT_OBJECT_0) { SetEvent(locked.value); WaitForSingleObject(released.value, 2000); ReleaseMutex(h.mutex.value); }
    });
    const DWORD held = WaitForSingleObject(locked.value, 1500);
    const uint64_t began = GetTickCount64(); h.client.cancelNight(); const uint64_t spent = GetTickCount64() - began;
    Sleep(250); const LONG copies = h.control().calls; Sleep(300); const bool quiet = h.control().calls == copies;
    SetEvent(released.value); holder.join();
    require(held == WAIT_OBJECT_0 && spent < 100 && quiet, "cancellation waited for shared lock or left integration running behind contention");
    require(!h.client.nightResult(12, frame, result, error) && error.find(L"cancelled") != std::wstring::npos,
        "cancelled partial window remained readable");
    h.begin(13); checkWindow(h.completed(13, frame), frame, 1000);
    std::cout << "PASS replacement tokens, lock-independent partial cancellation and fresh resumed window\n";
}
void sourceSemantics() {
    for (const wchar_t* mode : {L"night-constant-time", L"night-regressing-time"}) {
        Harness h(mode); h.first(); h.begin(1); lapse::Frame frame;
        const auto result = h.completed(1, frame); checkWindow(result, frame, 1000);
        require(result.timestampFallback, "unusable camera timestamps were not reported as sequence/receipt fallback");
    }
    {
        Harness h(L"night-duplicate"); h.first(); h.begin(1);
        require(h.failed(1).find(L"No distinct") != std::wstring::npos, "repeated retained source was counted as accumulated light");
        require(h.control().copies <= 2, "duplicate retained sample incurred repeated full pixel copies");
    }
    {
        Harness h(L"night-epoch"); h.first(); h.begin(1);
        require(h.failed(1).find(L"timeline changed") != std::wstring::npos, "source epoch/format change failed to invalidate active window");
    }
    {
        Harness h(L"night-warm"); const uint64_t began = GetTickCount64(); h.begin(1); lapse::Frame frame;
        const auto result = h.completed(1, frame); checkWindow(result, frame, 1000);
        require(result.beginTick - began >= 900, "source warmup was silently included in requested blend duration");
    }
    std::cout << "PASS timestamp fallback, duplicate suppression, epoch invalidation and fresh-watermark warmup\n";
}
void boundedFailures() {
    {
        Harness h(L"night-hang"); h.first(); h.begin(1, 10000);
        const uint64_t began = GetTickCount64();
        require(h.failed(1).find(L"3 seconds") != std::wstring::npos && GetTickCount64() - began < 3800,
            "long integration weakened independent three-second source freshness");
        const uint64_t stopped = GetTickCount64(); h.client.stop();
        require(GetTickCount64() - stopped < 1000, "hung synthetic driver escaped bounded helper shutdown");
    }
    {
        Harness h(L"night-hang"); h.first(); h.begin(1);
        const uint64_t began = GetTickCount64(), until = began + 5000;
        lapse::Frame frame; lapse::NightWindowResult result; std::wstring error;
        while (GetTickCount64() < until) {
            // A deliberately adversarial owned helper heartbeat claims source
            // freshness forever. It must not extend the completion deadline.
            h.inspect([](auto& shared) { if (shared.nightState == lapse::NightState::Integrating) shared.nightSourceTick = GetTickCount64(); });
            require(!h.client.nightResult(1, frame, result, error), "hung helper fabricated completion");
            if (!error.empty()) break;
            Sleep(25);
        }
        require(error.find(L"not delivered within its deadline") != std::wstring::npos && GetTickCount64() - began >= 3900 &&
            GetTickCount64() - began < 4800, "progress extended the fixed absolute window deadline");
    }
    std::cout << "PASS independent source timeout, absolute completion deadline and bounded hung-helper cleanup\n";
}
void slowSourceSuggestion() {
    Harness h(L"night-slow"); h.first(128); h.begin(1, 3000); lapse::Frame frame;
    const auto result = h.completed(1, frame); checkWindow(result, frame, 3000);
    require(result.exposure.samples == 1 && result.exposure.suggestedDurationMs == 6000,
        "bright half-frame-per-second source shrank Auto below a useful two-sample window");
    std::cout << "PASS bright slow source contributes once honestly and recommends a six-second Auto window\n";
}
void completionBoundaries() {
    {
        Harness h(L"night-live"); h.first(); h.begin(1);
        Sleep(4150); // The private result finishes normally, but is collected too late.
        bool completed = false;
        h.inspect([&](auto& shared) { completed = shared.nightState == lapse::NightState::Complete &&
            shared.nightCompletedTick >= shared.nightResult.endTick; });
        require(completed, "late-collection fixture did not produce a real completed private result");
        require(h.failed(1, 1000).find(L"not delivered within its deadline") != std::wstring::npos,
            "already-completed state bypassed the absolute acceptance deadline");
    }
    {
        Harness h(L"night-slow-copy"); h.first(); h.begin(1);
        const uint64_t until = GetTickCount64() + 5000;
        lapse::NightState state = lapse::NightState::Idle;
        while (GetTickCount64() < until) {
            h.inspect([&](auto& shared) { state = shared.nightState; });
            if (state == lapse::NightState::Complete || state == lapse::NightState::Failed) break;
            Sleep(20);
        }
        require(state == lapse::NightState::Failed, "slow conversion promoted stale source facts into a completed result");
        require(h.failed(1, 1000).find(L"3 seconds") != std::wstring::npos,
            "post-conversion source staleness lost its independent error");
    }
    std::cout << "PASS absolute bound applies to completed delivery and slow conversion cannot bypass source freshness\n";
}
void typedPublicationRace() {
    Harness h(L"night-live"); h.first(); h.begin(1); lapse::Frame blend, preview;
    h.completed(1, blend);
    require(blend.pixels[0] != 32, "typed-slot fixture requires distinguishable blended and ordinary pixels");
    h.client.cancelNight();
    InterlockedExchange(&h.control().previewGate, 1); InterlockedExchange(&h.control().previewGap, 1);
    std::wstring error;
    require(!h.client.latest(preview, error) && error.empty(), "gated new preview unexpectedly completed");
    const uint64_t until = GetTickCount64() + 1000;
    while (!InterlockedCompareExchange(&h.control().previewReached, 0, 0) && GetTickCount64() < until) Sleep(1);
    require(h.control().previewReached, "new preview did not enter its no-frame gap");
    uint64_t requested = 0;
    h.inspect([&](auto& shared) {
        requested = shared.requested;
        require(shared.requestedKind == lapse::PixelRequest::Preview && shared.completed < requested,
            "new Preview request must still be pending");
        // Model the exact publication half of the race: an older in-flight
        // Night response commits its already-owned pixels after the new Preview
        // request captured requestedGeneration. The payload remains Night.
        shared.generation += 2;
    });
    InterlockedExchange(&h.control().previewRelease, 1);
    bool acknowledged = false;
    const uint64_t answered = GetTickCount64() + 1000;
    while (!acknowledged && GetTickCount64() < answered) {
        h.inspect([&](auto& shared) { acknowledged = shared.completed == requested; });
        if (!acknowledged) Sleep(1);
    }
    require(acknowledged, "helper did not acknowledge the actual no-frame preview");
    require(!h.client.latest(preview, error) && error.empty(),
        "older Night pixels were delivered as a newer no-frame Preview response");
    InterlockedExchange(&h.control().previewGate, 0); InterlockedExchange(&h.control().previewGap, 0);
    h.first();
    std::cout << "PASS a no-frame preview acknowledgement cannot relabel older in-flight Night pixels\n";
}
}
int main() {
    const int host = lapse::runCameraHost(nullptr); if (host >= 0) return host;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 2;
    int result = 0;
    try {
        mappingContract();acceptedTierMutation();highTierNight();
        previewAndStableResult(); replaceAndCancel(); sourceSemantics(); boundedFailures(); slowSourceSuggestion(); completionBoundaries(); typedPublicationRace();
        std::cout << "Synthetic night helper transport contracts passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    CoUninitialize(); return result;
}

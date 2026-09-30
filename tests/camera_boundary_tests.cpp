// Selective integration of independently reproduced post-window regressions.
#include "camera_helper_fixture.h"
namespace {
void checkWindow(const lapse::NightWindowResult& result, const lapse::Frame& frame, uint32_t duration) {
    require(frame.valid() && frame.width == 64 && frame.height == 36 && frame.pixels[0] >= 32 && frame.pixels[3] == 255,
        "completed blend pixels are invalid");
    require(result.endTick - result.beginTick == duration && result.firstSampleTick >= result.beginTick &&
        result.lastSampleTick >= result.firstSampleTick && result.lastSampleTick < result.endTick &&
        result.exposure.samples > 0 && result.exposure.samples <= duration / lapse::NightCadenceMs,
        "window duration, observed sample span or contribution count is dishonest");
}
void postEndBoundary(const wchar_t* mode) {
    Harness h(mode);h.first();h.begin(1);
    uint64_t begin=0,end=0,first=0,last=0;
    const uint64_t preparedUntil=GetTickCount64()+3000;
    while(GetTickCount64()<preparedUntil){
        h.inspect([&](auto& shared){if(shared.nightState==lapse::NightState::Integrating){begin=shared.nightResult.beginTick;end=shared.nightResult.endTick;first=shared.nightResult.firstSampleTick;last=shared.nightResult.lastSampleTick;}});
        if(first){InterlockedExchange64(&h.control().windowEnd,static_cast<LONG64>(end));InterlockedExchange(&h.control().boundaryArmed,1);break;}
        Sleep(5);
    }
    require(first>=begin&&first<end&&last>=first&&last<end,"fixture did not confirm a real pre-end helper contribution");
    lapse::Frame output;lapse::NightWindowResult result;std::wstring error;
    bool completed=false;const uint64_t until=GetTickCount64()+5000;
    while(GetTickCount64()<until){if(h.client.nightResult(1,output,result,error)){completed=true;break;}if(!error.empty())break;Sleep(10);}
    const uint64_t delivered=static_cast<uint64_t>(InterlockedCompareExchange64(&h.control().boundaryReceived,0,0));
    std::wcout<<mode<<L": begin="<<begin<<L" end="<<end<<L" admittedFirst="<<first<<L" admittedLast="<<last<<L" postEndReceipt="<<delivered<<L" completed="<<completed<<L" resultSamples="<<result.exposure.samples<<L" error="<<error<<L'\n';
    require(h.control().boundaryDelivered&&delivered>=end,"fixture did not deliver the post-end source fact");
    require(completed&&error.empty(),"post-end source fact discarded already admitted pre-end contributions");
    checkWindow(result,output,1000);require(result.lastSampleTick<end,"post-end sample entered the result");
    std::wcout<<L"PASS "<<mode<<L": completed pre-end window ignores post-end source facts\n";
}
void rejectedBoundary(const wchar_t* mode, const wchar_t* expectedError, bool lateUntimed) {
    Harness h(mode); h.first(); h.begin(1);
    uint64_t begin = 0, end = 0, first = 0, last = 0;
    lapse::Frame output; lapse::NightWindowResult result; std::wstring error;
    bool completed = false, armed = false;
    const uint64_t until = GetTickCount64() + 5000;
    while (GetTickCount64() < until) {
        h.inspect([&](auto& shared) {
            if (shared.nightState == lapse::NightState::Integrating) {
                begin = shared.nightResult.beginTick; end = shared.nightResult.endTick;
                first = shared.nightResult.firstSampleTick; last = shared.nightResult.lastSampleTick;
            }
        });
        if (lateUntimed && first && !armed) {
            InterlockedExchange64(&h.control().windowEnd, static_cast<LONG64>(end));
            InterlockedExchange(&h.control().boundaryArmed, 1); armed = true;
        }
        if (h.client.nightResult(1, output, result, error)) { completed = true; break; }
        if (!error.empty()) break;
        Sleep(5);
    }
    const uint64_t delivered = static_cast<uint64_t>(InterlockedCompareExchange64(&h.control().boundaryReceived, 0, 0));
    std::wcout << mode << L": begin=" << begin << L" end=" << end << L" admittedFirst=" << first
        << L" admittedLast=" << last << L" injectedReceiptOrObservation=" << delivered << L" completed=" << completed
        << L" resultSamples=" << result.exposure.samples << L" error=" << error << L'\n';
    require(first >= begin && first < end && last >= first && last < end,
        "negative control did not confirm a real prior in-window contribution");
    require(h.control().boundaryDelivered && (lateUntimed ? delivered >= end : delivered >= begin && delivered < end),
        "negative control did not inject its fact at the intended boundary");
    require(!completed && error.find(expectedError) != std::wstring::npos,
        "negative control lost the expected source-change rejection");
    std::wcout << L"PASS " << mode << L": retained expected source-change rejection\n";
}
}
int main() {
    const int host = lapse::runCameraHost(nullptr); if (host >= 0) return host;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(com)) return 2;
    int result = 0;
    for(const wchar_t* mode:{L"night-post-end-epoch",L"night-post-end-size"}){
        try{postEndBoundary(mode);}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';result=1;}
    }
    for (const wchar_t* mode : {L"night-epoch", L"night-in-window-size", L"night-late-null-epoch"}) {
        try { rejectedBoundary(mode, mode == std::wstring(L"night-in-window-size") ? L"unsupported night blend frame size" : L"timeline changed",
            mode == std::wstring(L"night-late-null-epoch")); }
        catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; result = 1; }
    }
    CoUninitialize(); return result;
}

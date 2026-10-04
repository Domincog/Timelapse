// Optional semantic observations are fully synthetic. Actual encoders and MP4
// decoding verify that absent/failed/slow detection never damages recording.
#include "engine.h"
#include "encoder.h"
#include "camera_host.h"
#include "person_client.h"
#include "capture.h"
#include "engine_segment_decode.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
struct Write { int64_t us; bool secondary; };
std::mutex evidenceMutex;
std::vector<Write> writes;
std::vector<uint64_t> tokens, requestTicks;
std::vector<uint32_t> nightDurations;
std::atomic<unsigned> detectorConstructions{0}, detectorStarts{0}, detectorPolls{0}, detectorSubmits{0}, detectorLive{0};
std::atomic<unsigned> cameraReports{0}, activityCalls{0}, desktopObservations{0}, detectorCancels{0};
std::atomic<int> verdict{1}, sourceMode{0}, detectorMode{0}, startupDelay{0}, inferenceDelay{25}, cameraDelay{25};
std::atomic<lapse::person::Reason> unknownReason{lapse::person::Reason::InsufficientDetail};
std::atomic<bool> throwConstruction{false}, startedBeforeVideo{false}, ambiguousOutput{false}, highScoreAmbiguity{false};
std::atomic<uint64_t> oldReceipt{0};
std::atomic<bool> gateNextWrite{false}, writeGateEntered{false};
std::mutex writeGateMutex;
std::condition_variable writeGateWake;
bool writeGateOpen = false;
void releaseWriteGate() {
    { std::lock_guard<std::mutex> lock(writeGateMutex); writeGateOpen = true; }
    writeGateWake.notify_all();
}
struct ScopedWriteGateRelease { ~ScopedWriteGateRelease() { releaseWriteGate(); } };
void resetEvidence() {
    std::lock_guard<std::mutex> lock(evidenceMutex);
    writes.clear(); tokens.clear(); requestTicks.clear(); nightDurations.clear();
    detectorConstructions=detectorStarts=detectorPolls=detectorSubmits=detectorLive=0;
    cameraReports=activityCalls=desktopObservations=detectorCancels=0;
    verdict=1;sourceMode=detectorMode=startupDelay=0;inferenceDelay=cameraDelay=25;
    unknownReason=lapse::person::Reason::InsufficientDetail;
    throwConstruction=startedBeforeVideo=ambiguousOutput=highScoreAmbiguity=false;oldReceipt=0;
    gateNextWrite = writeGateEntered = false;
    { std::lock_guard<std::mutex> gateLock(writeGateMutex); writeGateOpen = false; }
}
void pixels(lapse::Frame& frame,int width,int height) {
    frame.width=width;frame.height=height;frame.pixels.resize(size_t(width)*height*4);
    for(size_t i=0;i<frame.pixels.size();i+=4){frame.pixels[i]=35;frame.pixels[i+1]=95;frame.pixels[i+2]=165;frame.pixels[i+3]=255;}
}
}
namespace lapse {
class PersonTestEncoder {
    Encoder real_;bool secondary_=false;
public:
    bool open(const std::wstring& path,int width,int height,int fps,std::wstring& error,EncodingQuality quality,EncodingMode mode, bool recoveryMode) {
        secondary_=path.find(L"-camera.recording.mp4")!=std::wstring::npos;
        return real_.open(path,width,height,fps,error,quality,mode, recoveryMode);
    }
    bool write(const Frame& frame,std::wstring& error) {
        {std::lock_guard<std::mutex> lock(evidenceMutex);writes.push_back({std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),secondary_});}
        if (gateNextWrite.exchange(false)) {
            std::unique_lock<std::mutex> lock(writeGateMutex); writeGateEntered = true;
            if (!writeGateWake.wait_for(lock, 5s, [] { return writeGateOpen; })) { error = L"Synthetic write gate timed out."; return false; }
        }
        return real_.write(frame,error);
    }
    bool finish(std::wstring& e){return real_.finish(e);}
    bool finishForPublication(std::wstring& e){return real_.finishForPublication(e);}
    DWORD publish(const std::wstring& p){return real_.publish(p);}
    void releasePublication()noexcept{real_.releasePublication();}
    bool emptyOutputDiscarded() const noexcept { return real_.emptyOutputDiscarded(); }
    uint64_t frames()const{return real_.frames();}
};
}
EXECUTION_STATE WINAPI personPower(EXECUTION_STATE){return ES_CONTINUOUS;}
BOOL WINAPI personSpace(LPCWSTR,PULARGE_INTEGER available,PULARGE_INTEGER,PULARGE_INTEGER){available->QuadPart=1024ULL*1024*1024;return TRUE;}
#define Encoder PersonTestEncoder
#define SetThreadExecutionState personPower
#define GetDiskFreeSpaceExW personSpace
#include "../src/engine.cpp"
#undef GetDiskFreeSpaceExW
#undef SetThreadExecutionState
#undef Encoder

namespace {
using namespace lapse;
using Microsoft::WRL::ComPtr;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void checked(HRESULT code,const char* message){require(SUCCEEDED(code),message);}
template<class Predicate>Status await(Engine& engine,Predicate predicate,int timeout=7000) {
    const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
    do{auto s=engine.status();if(predicate(s))return s;std::this_thread::sleep_for(3ms);}while(std::chrono::steady_clock::now()<until);
    const auto s=engine.status();std::wcerr<<s.message<<L" cadence="<<s.timeSkip.intervalMs<<L" detail="<<s.timeSkip.diagnostic.data()<<L'\n';
    throw std::runtime_error("Person engine condition timed out");
}
Settings configuration(const std::filesystem::path& folder,TimeSkipMode mode=TimeSkipMode::NoPerson) {
    Settings s;s.cameraId=L"person-camera";s.monitorId=L"person-desktop";s.layers=preset(Mode::Camera);
    s.width=160;s.height=120;s.intervalMs=100;s.folder=folder.wstring();s.preview=false;
    s.timeSkip.mode=mode;s.timeSkip.quietAfterMs=1000;s.timeSkip.multiplier=4;s.timeSkip.rampFrames=15;
    if(mode==TimeSkipMode::Manual||mode==TimeSkipMode::NoPersonWithinSchedule){s.timeSkip.rangeCount=1;s.timeSkip.ranges[0]={2,5};}
    return s;
}
Status finish(Engine& e){e.finish();return await(e,[](const Status& s){return s.state==State::Idle;});}
void decode(const Status& s,unsigned outputs=1) {
    require(!s.error&&!s.recordingFailed&&s.frames>0&&s.savedPaths.size()==outputs,"Detector work changed recording outcome");
    constexpr DWORD stream=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    for(const auto& path:s.savedPaths){
        ComPtr<IMFSourceReader> reader;checked(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"Cannot open person-test MP4");
        ComPtr<IMFMediaType> type;checked(MFCreateMediaType(&type),"Cannot create decode type");
        checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Cannot select video");checked(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"Cannot select NV12");
        checked(reader->SetCurrentMediaType(stream,nullptr,type.Get()),"Cannot decode person-test MP4");
        uint64_t count=0;bool ended=false;
        for(uint64_t n=0;n<s.frames+100;++n){DWORD flags=0;LONGLONG timestamp=0;ComPtr<IMFSample> sample;
            checked(reader->ReadSample(stream,0,nullptr,&flags,&timestamp,&sample),"Cannot read person-test MP4");require(!(flags&MF_SOURCE_READERF_ERROR),"MP4 decoder failed");
            if(sample){require(count<s.frames&&std::llabs(timestamp-LONGLONG(count)*10000000/30)<=1,"Person check changed playback timestamps");++count;}
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM){ended=true;break;}
        }
        require(ended&&count==s.frames,"Person check changed MP4 frame count");
    }
}
void boundedCadence() {
    std::lock_guard<std::mutex> lock(evidenceMutex);int64_t last=0;
    for(const auto& w:writes)if(!w.secondary){if(last)require(w.us-last>=98000,"Person transition captured faster than base");last=w.us;}
    for(size_t i=1;i<requestTicks.size();++i)require(requestTicks[i]-requestTicks[i-1]>=990,"Person input requested above one Hz");
}
void offAndNoCamera(const std::filesystem::path& root) {
    for(int i=0;i<3;++i){resetEvidence();Engine e;auto s=configuration(root/std::to_wstring(i),i==0?TimeSkipMode::Off:i==1?TimeSkipMode::Manual:TimeSkipMode::NoPerson);
        if(i==2)s.layers=preset(Mode::Desktop);e.configure(s);e.record();await(e,[](const auto& x){return x.frames>=3;});decode(finish(e));
        require(detectorConstructions==0&&detectorStarts==0&&detectorSubmits==0&&cameraReports==0,"Off/manual/no-camera allocated optional detector");
    }
    std::cout<<"off/manual/no-camera isolation passed\n";
}
void accelerateAndActivity(const std::filesystem::path& root) {
    resetEvidence();Engine e;auto s=configuration(root/L"paired");s.separateFiles=true;s.captureCursor=false;s.timeSkip.uncertainAsAbsent=false;e.configure(s);e.record();
    await(e,[](const auto& x){return x.timeSkip.intervalMs>120&&x.timeSkip.reason==TimeSkipReason::NoPerson;});
    require(!startedBeforeVideo&&detectorStarts==1&&desktopObservations==0&&activityCalls==0,"Person checks did not isolate camera/first admission");
    const auto beforeGeometry=cameraReports.load();sourceMode=7;inferenceDelay=250;
    await(e,[&](const auto& x){return cameraReports>beforeGeometry&&x.timeSkip.intervalMs==100;},2200);
    await(e,[](const auto& x){return x.timeSkip.intervalMs>110&&x.timeSkip.reason==TimeSkipReason::NoPerson;},4200);
    inferenceDelay=25;
    // Person mode and uncertainty policy are session-frozen even if an external caller changes controls.
    s.timeSkip.mode=TimeSkipMode::Off;s.timeSkip.uncertainAsAbsent=true;e.configure(s);verdict=2;
    await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::PersonPresent&&x.timeSkip.intervalMs==100;},2200);
    verdict=0;
    const auto uncertain=await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::PersonUncertain&&x.timeSkip.intervalMs==100&&std::wstring(x.timeSkip.diagnostic.data()).find(L"Too little image detail")!=std::wstring::npos;},2200);
    require(std::wstring(uncertain.timeSkip.diagnostic.data()).find(L"using normal cadence")!=std::wstring::npos,
        "Uncertainty diagnostic used a live setting instead of the frozen opt-out");
    const auto holdUntil=uncertain.timeSkip.lastCheckTick+uint64_t(s.timeSkip.quietAfterMs)+1200;
    while(e.status().timeSkip.lastCheckTick<holdUntil){
        const auto current=e.status();
        require(current.timeSkip.reason==TimeSkipReason::PersonUncertain&&current.timeSkip.intervalMs==100,
            "Opted-out healthy uncertainty accelerated after repeated checks");
        require(GetTickCount64()<holdUntil+2200,"Opted-out uncertainty stopped receiving fresh checks");
        std::this_thread::sleep_for(3ms);
    }
    highScoreAmbiguity=true;
    const auto ambiguous=await(e,[&](const auto& x){return x.timeSkip.reason==TimeSkipReason::PersonUncertain&&x.timeSkip.intervalMs==100&&
        x.timeSkip.lastCheckTick>=holdUntil&&std::wstring(x.timeSkip.diagnostic.data()).find(L"ambiguous")!=std::wstring::npos;},2200);
    require(std::wstring(ambiguous.timeSkip.diagnostic.data()).find(L"using normal cadence")!=std::wstring::npos,
        "Opt-out failed to keep model ambiguity at normal cadence");
    decode(finish(e),2);require(detectorLive==0,"Finished detector retained its resources");boundedCadence();
    std::cout<<"camera-only paired acceleration, Present/Unknown return and frozen opt-out passed\n";
}
void ambiguousAbsenceAndUncertainty(const std::filesystem::path& root) {
    resetEvidence(); verdict=2;
    Engine e; auto s=configuration(root/L"ambiguous-absence");s.timeSkip.quietAfterMs=2000;
    e.configure(s);e.record();
    const auto present=await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::PersonPresent;});
    ambiguousOutput=true;
    const auto checking=await(e,[&](const auto& x){return x.timeSkip.reason==TimeSkipReason::Checking&&x.timeSkip.lastCheckTick>present.timeSkip.lastCheckTick;},2200);
    require(checking.timeSkip.intervalMs==100&&detectorLive==1,"Healthy ambiguous worker was unavailable or skipped absence dwell");
    const auto fullDwellAt=checking.timeSkip.lastCheckTick+uint64_t(s.timeSkip.quietAfterMs);
    while(GetTickCount64()+100<fullDwellAt){
        require(e.status().timeSkip.intervalMs==100,"Subthreshold person scores borrowed earlier presence time for absence dwell");
        std::this_thread::sleep_for(3ms);
    }
    const auto accelerated=await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::NoPerson&&x.timeSkip.intervalMs>=350;},7000);
    require(accelerated.timeSkip.lastCheckTick-checking.timeSkip.lastCheckTick+10>=uint64_t(s.timeSkip.quietAfterMs),
        "Ambiguous empty scene accelerated before a full observed absence dwell");
    await(e,[&](const auto& x){return x.frames>=accelerated.frames+3&&x.timeSkip.reason==TimeSkipReason::NoPerson;},2200);
    {
        std::lock_guard<std::mutex> lock(evidenceMutex);
        require(writes.size()>=4,"No saved frames followed absence acceleration");
        for(size_t i=writes.size()-2;i<writes.size();++i)
            require(writes[i].us-writes[i-1].us>=300000,"Absence changed status without slowing actual saved-frame cadence");
    }
    ambiguousOutput=false;
    const auto returned=await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::PersonPresent&&x.timeSkip.intervalMs==100;},2200);
    verdict=0;
    const auto uncertain=await(e,[&](const auto& x){return x.timeSkip.reason==TimeSkipReason::Checking&&x.timeSkip.lastCheckTick>returned.timeSkip.lastCheckTick;},2200);
    require(uncertain.timeSkip.intervalMs==100&&detectorLive==1&&
        std::wstring(uncertain.timeSkip.diagnostic.data()).find(L"Too little image detail")!=std::wstring::npos&&
        std::wstring(uncertain.timeSkip.diagnostic.data()).find(L"treating uncertainty as no person detected")!=std::wstring::npos,
        "Default low-detail uncertainty was unavailable, hid its policy or skipped its dwell");
    // Changes made during recording must not alter either cadence or diagnostics.
    s.timeSkip.uncertainAsAbsent=false;e.configure(s);
    const auto uncertainDwellAt=uncertain.timeSkip.lastCheckTick+uint64_t(s.timeSkip.quietAfterMs);
    while(GetTickCount64()+100<uncertainDwellAt){
        require(e.status().timeSkip.intervalMs==100,"Unknown result borrowed earlier absence or presence dwell");
        std::this_thread::sleep_for(3ms);
    }
    const auto uncertainAccelerated=await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::NoPersonUncertain&&x.timeSkip.intervalMs>=350;},7000);
    require(uncertainAccelerated.timeSkip.lastCheckTick-uncertain.timeSkip.lastCheckTick+10>=uint64_t(s.timeSkip.quietAfterMs)&&
        std::wstring(uncertainAccelerated.timeSkip.diagnostic.data()).find(L"treating uncertainty as no person detected")!=std::wstring::npos,
        "Default uncertainty skipped full dwell or its frozen diagnostic policy");
    await(e,[&](const auto& x){return x.frames>=uncertainAccelerated.frames+3&&x.timeSkip.reason==TimeSkipReason::NoPersonUncertain;},2200);
    {
        std::lock_guard<std::mutex> lock(evidenceMutex);
        require(writes.size()>=4,"No saved frames followed uncertain-absence acceleration");
        for(size_t i=writes.size()-2;i<writes.size();++i)
            require(writes[i].us-writes[i-1].us>=300000,"Uncertainty changed status without slowing saved-frame cadence");
    }
    highScoreAmbiguity=true;
    const auto ambiguousUncertain=await(e,[&](const auto& x){return x.timeSkip.reason==TimeSkipReason::NoPersonUncertain&&x.timeSkip.intervalMs>=350&&
        x.timeSkip.lastCheckTick>uncertainAccelerated.timeSkip.lastCheckTick&&std::wstring(x.timeSkip.diagnostic.data()).find(L"ambiguous")!=std::wstring::npos;},2200);
    require(std::wstring(ambiguousUncertain.timeSkip.diagnostic.data()).find(L"treating uncertainty as no person detected")!=std::wstring::npos,
        "Default ambiguous uncertainty lost its policy or model detail");
    highScoreAmbiguity=false;
    auto lastFailureTick=ambiguousUncertain.timeSkip.lastCheckTick;
    for(const auto reason:{person::Reason::InvalidInput,person::Reason::ModelFailure,person::Reason::InvalidOutput,person::Reason::StaleSource,person::Reason::None}){
        unknownReason=reason;
        const auto failed=await(e,[&](const auto& x){return x.timeSkip.reason==TimeSkipReason::Unavailable&&x.timeSkip.lastCheckTick>lastFailureTick;},2200);
        require(failed.timeSkip.intervalMs==100&&detectorLive==1&&failed.timeSkip.diagnostic[0],
            "A failed or invalid person result was treated as uncertain absence");
        lastFailureTick=failed.timeSkip.lastCheckTick;
    }
    const auto failedAgain=await(e,[&](const auto& x){return x.timeSkip.lastCheckTick>lastFailureTick;},2200);
    require(failedAgain.timeSkip.reason==TimeSkipReason::Unavailable&&failedAgain.timeSkip.intervalMs==100,
        "Repeated model inference failure qualified absence");
    unknownReason=person::Reason::InsufficientDetail;
    const auto recovered=await(e,[&](const auto& x){return x.timeSkip.reason==TimeSkipReason::Checking&&x.timeSkip.lastCheckTick>failedAgain.timeSkip.lastCheckTick;},2200);
    require(recovered.timeSkip.intervalMs==100&&detectorLive==1,"Fresh healthy uncertainty did not recover after a failed check");
    const auto recoveryDwellAt=recovered.timeSkip.lastCheckTick+uint64_t(s.timeSkip.quietAfterMs);
    while(GetTickCount64()+100<recoveryDwellAt){
        require(e.status().timeSkip.intervalMs==100,"Healthy uncertainty borrowed dwell through failed checks");
        std::this_thread::sleep_for(3ms);
    }
    await(e,[&](const auto& x){return x.timeSkip.reason==TimeSkipReason::NoPersonUncertain&&x.timeSkip.intervalMs>100&&
        x.timeSkip.lastCheckTick-recovered.timeSkip.lastCheckTick+10>=uint64_t(s.timeSkip.quietAfterMs);},4200);
    verdict=2;
    await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::PersonPresent&&x.timeSkip.intervalMs==100;},2200);
    decode(finish(e));boundedCadence();
    std::cout<<"subthreshold and default uncertain absence require full dwell and slow saved cadence; failed checks clear dwell\n";
}
void pauseProvenance(const std::filesystem::path& root) {
    resetEvidence();verdict=0;Engine e;auto s=configuration(root/L"pause");s.timeSkip.quietAfterMs=2000;e.configure(s);e.record();
    await(e,[](const auto& x){return x.timeSkip.intervalMs>110&&x.timeSkip.reason==TimeSkipReason::NoPersonUncertain;});e.setPaused(true);
    const auto paused=await(e,[](const auto& x){return x.state==State::Paused;});const auto calls=detectorPolls.load();
    require(detectorLive==0,"Paused detector retained model resources");std::this_thread::sleep_for(130ms);
    require(detectorPolls==calls&&e.status().elapsed==paused.elapsed,"Hidden pause continued detector work or clock");
    const auto reportsBeforeResume=cameraReports.load(),submitsBeforeResume=detectorSubmits.load();
    oldReceipt=GetTickCount64()-100;sourceMode=6;e.setPaused(false);
    await(e,[&](const auto& x){return x.state==State::Recording&&detectorStarts>=2&&cameraReports>reportsBeforeResume;});
    require(e.status().timeSkip.intervalMs==100&&detectorSubmits==submitsBeforeResume,"Pre-reset sample borrowed pause time");
    {std::lock_guard<std::mutex> lock(evidenceMutex);require(tokens.size()==2&&tokens[0]!=tokens[1],"Resume reused detector token");}
    sourceMode=0;
    const auto recovered=await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::Checking&&x.timeSkip.lastCheckTick>0;},2200);
    const auto freshDwellAt=recovered.timeSkip.lastCheckTick+uint64_t(s.timeSkip.quietAfterMs);
    while(GetTickCount64()+100<freshDwellAt){
        require(e.status().timeSkip.intervalMs==100,"Uncertainty borrowed its pre-pause absence dwell");
        std::this_thread::sleep_for(3ms);
    }
    await(e,[&](const auto& x){return x.timeSkip.reason==TimeSkipReason::NoPersonUncertain&&x.timeSkip.intervalMs>100&&
        x.timeSkip.lastCheckTick-recovered.timeSkip.lastCheckTick+10>=uint64_t(s.timeSkip.quietAfterMs);},4200);
    decode(finish(e));
    std::cout<<"pause cleanup, pre-resume source rejection and fresh uncertainty dwell passed\n";
}
void malformedAndDuplicate(const std::filesystem::path& root) {
    resetEvidence();{Engine e;auto s=configuration(root/L"invalid");sourceMode=1;e.configure(s);e.record();
        for(int mode=1;mode<=4;++mode){sourceMode=mode;const auto before=cameraReports.load();await(e,[&](const auto& x){return cameraReports>before&&x.timeSkip.intervalMs==100;},2400);}
        require(detectorSubmits==0&&detectorStarts==1,"Invalid image reached inference or restarted detector");decode(finish(e));}
    resetEvidence();{Engine e;sourceMode=5;e.configure(configuration(root/L"duplicate"));e.record();
        await(e,[](const auto& x){return cameraReports>=3&&x.timeSkip.intervalMs==100;});require(detectorSubmits==1,"Duplicate source was submitted again");decode(finish(e));}
    std::cout<<"stale/future/session/geometry and duplicate-source rejection passed\n";
}
void failuresLatch(const std::filesystem::path& root) {
    for(int mode=1;mode<=3;++mode){resetEvidence();Engine e;detectorMode=mode;e.configure(configuration(root/std::to_wstring(mode)));e.record();
        await(e,[](const auto& x){return x.frames>=4&&x.timeSkip.reason==TimeSkipReason::Unavailable&&x.timeSkip.diagnostic[0]&&detectorLive==0;});
        const auto starts=detectorStarts.load();e.setPaused(true);await(e,[](const auto& x){return x.state==State::Paused;});e.setPaused(false);
        await(e,[](const auto& x){return x.state==State::Recording&&x.frames>=6;});
        require(detectorStarts==starts,"Hard detector failure retried during same recording");decode(finish(e));
        detectorMode=0;e.record();await(e,[](const auto& x){return x.frames>=2&&detectorStarts>0;});decode(finish(e));require(detectorStarts==starts+1,"New recording did not retry failed detector");
    }
    resetEvidence();{Engine e;throwConstruction=true;e.configure(configuration(root/L"allocation"));e.record();await(e,[](const auto& x){return x.frames>=4&&x.timeSkip.diagnostic[0];});decode(finish(e));}
    std::cout<<"unavailable, wrong-response, startup and allocation fault isolation passed\n";
}
void asynchronousDeadlines(const std::filesystem::path& root) {
    for(bool startup:{false,true}){resetEvidence();Engine e;auto s=configuration(root/(startup?L"slow-start":L"slow-inference"));s.recordingLimitSeconds=1;
        if(startup)startupDelay=5000;else inferenceDelay=5000;e.configure(s);const auto begin=GetTickCount64();e.record();
        const auto done=await(e,[](const auto& x){return x.state==State::Idle&&x.frames>0;},2600);
        require(GetTickCount64()-begin<2000&&done.frames>=6&&detectorLive==0,"Optional pending work delayed recording limit or cleanup");decode(done);
    }
    resetEvidence();{Engine e;inferenceDelay=3150;e.configure(configuration(root/L"late-result"));e.record();
        await(e,[](const auto& x){return detectorSubmits>=1&&x.elapsed>3.6;},5500);
        require(e.status().timeSkip.intervalMs==100&&e.status().timeSkip.lastCheckTick==0,"Completion time made stale source fresh");decode(finish(e));}
    std::cout<<"slow startup/inference deadline priority and source-age freshness passed\n";
}
void scheduledNight(const std::filesystem::path& root) {
    resetEvidence();Engine e;auto s=configuration(root/L"night",TimeSkipMode::NoPersonWithinSchedule);s.intervalMs=1000;s.night.enabled=true;s.night.durationMs=1000;s.separateFiles=true;s.recordingLimitSeconds=4;
    e.configure(s);e.record();await(e,[](const auto& x){return x.state==State::Recording&&detectorSubmits>0&&x.elapsed<2;},4000);
    const auto done=await(e,[](const auto& x){return x.state==State::Idle&&x.frames>0;},7000);decode(done,2);
    require(activityCalls==0&&desktopObservations==0&&detectorLive==0,"Night/person path used activity desktop work or retained process");
    {std::lock_guard<std::mutex> lock(evidenceMutex);require(!nightDurations.empty(),"Night exposure never started");for(auto n:nightDurations)require(n==1000,"Person return truncated Night integration");}
    std::cout<<"scheduled outside-window observation and paired Night finalization passed\n";
}
void splitPresenceContinuity(const std::filesystem::path& root) {
    resetEvidence(); Engine e; auto s=configuration(root/L"split-person");
    s.segmentDurationSeconds=1; s.recordingLimitSeconds=5; s.separateFiles=true;
    e.configure(s); e.record();
    await(e,[](const auto& x){return x.completedSegments>=2&&x.timeSkip.intervalMs>120&&x.timeSkip.reason==TimeSkipReason::NoPerson;},4500);
    require(detectorStarts==1&&detectorLive==1&&activityCalls==0&&desktopObservations==0,
        "File rollover restarted the detector or enabled unrelated observations");
    {std::lock_guard<std::mutex> lock(evidenceMutex);require(tokens.size()==1,"Split reset person evidence token");}
    verdict=2;
    await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::PersonPresent&&x.timeSkip.intervalMs==100;},1800);
    const auto done=await(e,[](const auto& x){return x.state==State::Idle;},4000);
    require(done.elapsed>=5&&done.elapsed<6.5&&detectorStarts==1&&detectorLive==0&&
        done.message.find(L"time limit")!=std::wstring::npos,"Split person session lost clock, limit or cleanup");
    boundedCadence(); split_test::verify(s.folder,done,true);
    std::cout<<"split files preserve absence dwell, detector lifetime, person return and shared playback\n";
}
void splitResumePerson(const std::filesystem::path& root, bool split) {
    resetEvidence(); auto s = configuration(root / (split ? L"resume-split-person" : L"resume-off-person"));
    s.segmentDurationSeconds = split ? 1 : 0;
    s.preview = true; // Keep the source alive; a camera restart must not mask a lost policy reset.
    gateNextWrite = true;
    Engine e; ScopedWriteGateRelease release; e.configure(s); e.record();
    await(e, [](const auto&) { return writeGateEntered.load(); });
    e.setPaused(true); std::this_thread::sleep_for(1100ms); releaseWriteGate();
    const auto paused = await(e, [](const auto& x) { return x.state == State::Paused; });
    require(paused.elapsed >= 1 && paused.completedSegments == 0 && detectorLive == 0,
        "Pause did not retain the overdue part and retire optional detector work");
    e.setPaused(false);
    const auto resumed = await(e, [](const auto& x) { return detectorSubmits >= 2 && x.timeSkip.intervalMs > 100; }, 5000);
    require(detectorStarts == 1 && (!split || resumed.completedSegments > 0) && activityCalls == 0 && desktopObservations == 0,
        "Boundary resume failed to restart one detector or enabled unrelated image observation");
    const auto done = finish(e);
    require(detectorLive == 0, "Boundary-resumed detector survived Finish");
    boundedCadence(); if (split) split_test::verify(s.folder, done, false); else decode(done);
    std::cout << "visible-camera person checks restart after overdue split on Resume; split=" << split << "\n";
}
}

namespace lapse {
struct CameraClient::Impl{bool active=false,pending=false;uint64_t sequence=0,requestTick=0,nightToken=0,nightStart=0;uint32_t duration=0;};
CameraClient::CameraClient():impl_(std::make_unique<Impl>()){}
CameraClient::~CameraClient()=default;
bool CameraClient::start(const std::wstring& id,std::wstring& e, CameraResolution){e.clear();impl_->active=id==L"person-camera";impl_->sequence=0;return impl_->active;}
void CameraClient::stop(){impl_->active=false;impl_->pending=false;}
bool CameraClient::latest(Frame& out,std::wstring& e){e.clear();if(!impl_->active){e=L"Synthetic camera stopped";return false;}pixels(out,160,120);return true;}
bool CameraClient::observeActivity(uint64_t,CameraObservation&,std::wstring& e){++activityCalls;e.clear();return false;}
void CameraClient::cancelActivityObservation()noexcept{impl_->pending=false;}
bool CameraClient::personInput(uint64_t token,CameraPersonInput& out,std::wstring& e,bool requestNew){
    e.clear();if(!impl_->active){e=L"Synthetic camera stopped";return false;}
    const auto now=GetTickCount64();
    if(requestNew){std::lock_guard<std::mutex> lock(evidenceMutex);requestTicks.push_back(now);}
    if(!impl_->pending){if(!requestNew)return false;impl_->pending=true;impl_->requestTick=now;return false;}
    if(now-impl_->requestTick<static_cast<uint64_t>(cameraDelay.load()))return false;
    impl_->pending=false;out.source={token,1,sourceMode==5?1:++impl_->sequence,impl_->requestTick,160,120,320,240};
    if(sourceMode==1)out.source.receivedTick=now-4000;
    if(sourceMode==2)out.source.receivedTick=now+100;
    if(sourceMode==3)++out.source.sessionToken;
    if(sourceMode==4)out.source.width=319;
    if(sourceMode==6)out.source.receivedTick=oldReceipt;
    if(sourceMode==7){out.source.sourceWidth=320;out.source.sourceHeight=240;}
    out.bgr.fill(100);++cameraReports;return true;
}
bool CameraClient::beginNight(uint64_t token,uint32_t duration,const NightSettings&,std::wstring& e){e.clear();impl_->nightToken=token;impl_->nightStart=GetTickCount64();impl_->duration=duration;std::lock_guard<std::mutex> lock(evidenceMutex);nightDurations.push_back(duration);return true;}
bool CameraClient::nightResult(uint64_t token,Frame& out,NightWindowResult& r,std::wstring& e){e.clear();if(token!=impl_->nightToken){e=L"Wrong synthetic Night token";return false;}if(GetTickCount64()<impl_->nightStart+impl_->duration)return false;
    r={};r.beginTick=impl_->nightStart;r.endTick=r.beginTick+impl_->duration;r.firstSampleTick=r.beginTick+100;r.lastSampleTick=r.endTick-100;r.exposure.samples=5;r.exposure.suggestedDurationMs=1000;pixels(out,160,120);return true;}
void CameraClient::cancelNight()noexcept{impl_->nightToken=0;}
bool captureMonitor(const std::wstring&,int w,int h,bool cursor,Frame& out,std::wstring& e){
    if(w==TimeSkipWidth && h==TimeSkipHeight){require(!cursor,"Quiet observation included the desktop cursor");++desktopObservations;}
    e.clear();pixels(out,w,h);return true;
}
struct PersonClient::Impl{bool active=false,pending=false;uint64_t started=0,due=0;person::Source expected;};
PersonClient::PersonClient(){if(throwConstruction.exchange(false))throw std::bad_alloc();impl_=std::make_unique<Impl>();++detectorConstructions;++detectorLive;}
PersonClient::~PersonClient(){--detectorLive;}
bool PersonClient::start(uint64_t token,std::wstring& e){++detectorStarts;e.clear();{std::lock_guard<std::mutex> lock(evidenceMutex);tokens.push_back(token);if(writes.empty())startedBeforeVideo=true;}
    if(detectorMode==3){e=L"Synthetic missing optional pack";return false;}impl_->active=true;impl_->started=GetTickCount64();return true;}
void PersonClient::cancel()noexcept{++detectorCancels;impl_->active=false;impl_->pending=false;}
const wchar_t* PersonClient::diagnostic()const noexcept{return L"Synthetic detector stopped";}
PersonPoll PersonClient::poll(PersonCheckResult& r)noexcept{
    ++detectorPolls;if(!impl_->active||detectorMode==1)return PersonPoll::Unavailable;
    const auto now=GetTickCount64();if(now-impl_->started<static_cast<uint64_t>(startupDelay.load()))return PersonPoll::Pending;
    if(!impl_->pending)return PersonPoll::Ready;if(now<impl_->due)return PersonPoll::Pending;
    impl_->pending=false;r={};r.source=impl_->expected;if(detectorMode==2)++r.source.sessionToken;
    r.output.verdict=static_cast<person::Verdict>(verdict.load());r.output.rawMaxPerson=verdict==2?.8f:.05f;r.output.validMaxPerson=verdict==2?.8f:0;
    r.output.reason=verdict==0?unknownReason.load():person::Reason::None;
    r.sufficientDetail=verdict!=0;
    if(ambiguousOutput){r.output={.22945f,.22945f,person::Verdict::Unknown,person::Reason::Ambiguous};r.sufficientDetail=true;}
    if(highScoreAmbiguity){r.output={.65f,.15f,person::Verdict::Unknown,person::Reason::Ambiguous};r.sufficientDetail=true;}
    return PersonPoll::Complete;
}
bool PersonClient::submit(const CameraPersonInput& input)noexcept{if(!impl_->active||impl_->pending)return false;++detectorSubmits;impl_->expected=input.source;impl_->pending=true;impl_->due=GetTickCount64()+static_cast<uint64_t>(inferenceDelay.load());return true;}
}
void personOnlyRecording(const std::filesystem::path& root) {
    resetEvidence();verdict=2;
    Engine e;auto s=configuration(root/L"person-only",TimeSkipMode::PersonOnly);s.timeSkip.quietAfterMs=1000;
    e.configure(s);e.record();
    const auto present=await(e,[](const auto& x){return x.timeSkip.reason==TimeSkipReason::PersonPresent&&x.frames>=5;});
    require(!present.timeSkip.suspended&&present.timeSkip.intervalMs==100,"A detected person did not keep normal capture");
    verdict=1;
    const auto paused=await(e,[](const auto& x){return x.timeSkip.suspended&&x.timeSkip.reason==TimeSkipReason::NoPerson;},5000);
    size_t before=0;{std::lock_guard<std::mutex> lock(evidenceMutex);before=writes.size();}
    std::this_thread::sleep_for(900ms);
    const auto still=e.status();size_t after=0;{std::lock_guard<std::mutex> lock(evidenceMutex);after=writes.size();}
    require(still.state==State::Recording&&still.timeSkip.suspended&&after==before&&still.frames<=paused.frames+1,
        "Person-only recording saved frames while nobody was detected");
    verdict=2;
    const auto returned=await(e,[](const auto& x){return !x.timeSkip.suspended&&x.timeSkip.reason==TimeSkipReason::PersonPresent;},2500);
    await(e,[&](const auto& x){return x.frames>=returned.frames+3;},2500);
    decode(finish(e));boundedCadence();require(detectorLive==0,"Finished person-only detector retained its resources");
    std::cout<<"person-only recording pauses while nobody is detected and resumes promptly\n";
}
int main(){
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(com))return 1;if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();return 1;}
    const auto root=std::filesystem::current_path()/(L"engine-person-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));int code=0;
    try{offAndNoCamera(root/L"isolation");accelerateAndActivity(root);ambiguousAbsenceAndUncertainty(root);pauseProvenance(root);malformedAndDuplicate(root);failuresLatch(root/L"failures");asynchronousDeadlines(root);scheduledNight(root);splitPresenceContinuity(root);
        splitResumePerson(root,false);splitResumePerson(root,true);personOnlyRecording(root);
        std::filesystem::remove_all(root);std::cout<<"All synthetic person engine contracts passed.\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::wcerr<<L"Retained artifacts: "<<root.wstring()<<L'\n';code=1;}
    MFShutdown();CoUninitialize();return code;
}

// This fixture owns no native desktop capture surface.
namespace lapse { void releaseDesktopCaptureCache() noexcept {} }

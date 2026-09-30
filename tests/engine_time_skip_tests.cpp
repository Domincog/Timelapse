// Recording-time cadence and observation integration. Every source is synthetic;
// owned MP4 files use the real encoder and are decoded before test cleanup.
#include "engine.h"
#include "config.h"
#include "encoder.h"
#include "camera_host.h"
#include "capture.h"
#include "engine_segment_decode.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
struct Write { uint64_t tick; int64_t microseconds; bool secondary; };
struct Window { uint64_t tick; uint32_t duration; };
std::mutex evidenceMutex;
std::vector<Write> writes;
std::vector<Window> windows;
std::atomic<unsigned> starts{0}, captures{0}, observerCaptures{0}, checks{0}, cancels{0}, nightCancels{0}, queries{0}, opens{0};
std::atomic<unsigned> observationReports{0};
std::atomic<int> cameraScene{0}, desktopScene{0}, observationMode{0}, observationDelay{0}, writeDelay{0};
std::atomic<bool> lowSpace{false};
std::atomic<uint64_t> retainedReceipt{0};
std::atomic<bool> gateNextWrite{false}, writeGateEntered{false};
std::mutex writeGateMutex;
std::condition_variable writeGateWake;
bool writeGateOpen = false;
void releaseWriteGate() {
    { std::lock_guard<std::mutex> lock(writeGateMutex); writeGateOpen = true; }
    writeGateWake.notify_all();
}
struct ScopedWriteGateRelease { ~ScopedWriteGateRelease() { releaseWriteGate(); } };
void reset() {
    std::lock_guard<std::mutex> lock(evidenceMutex); writes.clear(); windows.clear();
    starts=captures=observerCaptures=checks=cancels=nightCancels=queries=opens=observationReports=0;
    cameraScene=desktopScene=observationMode=observationDelay=writeDelay=0; lowSpace=false; retainedReceipt=0;
    gateNextWrite = writeGateEntered = false;
    { std::lock_guard<std::mutex> gateLock(writeGateMutex); writeGateOpen = false; }
}
std::vector<Write> submitted() { std::lock_guard<std::mutex> lock(evidenceMutex); return writes; }
std::vector<Window> begun() { std::lock_guard<std::mutex> lock(evidenceMutex); return windows; }
void pixels(lapse::Frame& frame,int width,int height,int scene) {
    frame.width=width;frame.height=height;frame.pixels.resize(size_t(width)*height*4);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        const auto offset=(size_t(y)*width+x)*4;
        const auto value=static_cast<uint8_t>(scene ? ((x/8+y/8)%2?220:30) : 80);
        frame.pixels[offset]=frame.pixels[offset+1]=frame.pixels[offset+2]=value;frame.pixels[offset+3]=255;
    }
}
}
namespace lapse {
class SkipEncoder {
    Encoder real_;
    bool secondary_=false;
public:
    bool open(const std::wstring& path,int width,int height,int fps,std::wstring& error,EncodingQuality quality,EncodingMode mode, bool recoveryMode) {
        ++opens;secondary_=path.find(L"-camera.recording.mp4")!=std::wstring::npos;
        return real_.open(path,width,height,fps,error,quality,mode, recoveryMode);
    }
    bool write(const Frame& frame,std::wstring& error) {
        { std::lock_guard<std::mutex> lock(evidenceMutex); writes.push_back({GetTickCount64(),
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),secondary_}); }
        if (gateNextWrite.exchange(false)) {
            std::unique_lock<std::mutex> lock(writeGateMutex); writeGateEntered = true;
            if (!writeGateWake.wait_for(lock, 5s, [] { return writeGateOpen; })) { error = L"Synthetic write gate timed out."; return false; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(writeDelay.exchange(0)));
        return real_.write(frame,error);
    }
    bool finish(std::wstring& error){return real_.finish(error);}
    bool finishForPublication(std::wstring& error){return real_.finishForPublication(error);}
    DWORD publish(const std::wstring& path){return real_.publish(path);}
    void releasePublication()noexcept{real_.releasePublication();}
    uint64_t frames()const{return real_.frames();}
};
}
EXECUTION_STATE WINAPI skipPower(EXECUTION_STATE){return ES_CONTINUOUS;}
BOOL WINAPI skipSpace(LPCWSTR,PULARGE_INTEGER available,PULARGE_INTEGER,PULARGE_INTEGER){++queries;available->QuadPart=lowSpace?0:1024ULL*1024*1024;return TRUE;}
#define Encoder SkipEncoder
#define SetThreadExecutionState skipPower
#define GetDiskFreeSpaceExW skipSpace
#include "../src/engine.cpp"
#undef GetDiskFreeSpaceExW
#undef SetThreadExecutionState
#undef Encoder

namespace {
using namespace lapse;
using Microsoft::WRL::ComPtr;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void checked(HRESULT value,const char* message){require(SUCCEEDED(value),message);}
template<class Predicate> Status await(Engine& engine,Predicate predicate,int timeout=7000) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
    do {const auto value=engine.status();if(predicate(value))return value;std::this_thread::sleep_for(3ms);}
    while(std::chrono::steady_clock::now()<end);
    const auto value=engine.status();std::wcerr<<value.message<<L" target="<<value.timeSkip.intervalMs<<L" reason="<<int(value.timeSkip.reason)<<L'\n';
    throw std::runtime_error("Timed out waiting for time-skipping engine contract");
}
Settings config(const std::filesystem::path& folder,TimeSkipMode mode=TimeSkipMode::Quiet) {
    Settings value;value.cameraId=L"skip-camera";value.monitorId=L"skip-desktop";value.layers=preset(Mode::Camera);
    value.width=160;value.height=120;value.intervalMs=100;value.folder=folder.wstring();value.preview=false;
    value.timeSkip.mode=mode;value.timeSkip.quietAfterMs=1000;value.timeSkip.rampFrames=15;value.timeSkip.multiplier=4;
    if(mode==TimeSkipMode::Manual||mode==TimeSkipMode::QuietWithinSchedule) {
        value.timeSkip.rangeCount=1;value.timeSkip.ranges[0]={0,60};value.timeSkip.repeatSeconds=60;
    }
    return value;
}
Status finish(Engine& engine){engine.finish();return await(engine,[](const auto& value){return value.state==State::Idle;});}
void spacing(const std::vector<Write>& values,int minimum=98) {
    int64_t last=0;
    for(const auto& value:values)if(!value.secondary){if(last)require(value.microseconds-last>=int64_t(minimum)*1000,"Transition admitted a frame faster than the configured base");last=value.microseconds;}
}
void decode(const Status& result, int width, int height, unsigned count, bool failure = false) {
    require(result.error == failure && result.recordingFailed == failure && result.savedPaths.size() == count && result.frames > 0,
        "Time-skipping recording did not save the expected outputs");
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    for (const auto& path : result.savedPaths) {
        ComPtr<IMFSourceReader> reader; checked(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "Cannot open time-skipping MP4");
        ComPtr<IMFMediaType> type; checked(reader->GetCurrentMediaType(stream, &type), "Cannot inspect time-skipping MP4");
        UINT32 actualWidth = 0, actualHeight = 0;
        checked(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &actualWidth, &actualHeight), "Missing time-skipping geometry");
        require(actualWidth == UINT32(width) && actualHeight == UINT32(height), "Encoded time-skipping dimensions changed");
        checked(MFCreateMediaType(&type), "Cannot create decoded type");
        checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Cannot select video");
        checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Cannot select decoded pixels");
        checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Cannot decode time-skipping MP4");
        uint64_t samples = 0; bool ended = false;
        for (uint64_t attempt = 0; attempt < result.frames + 100; ++attempt) {
            DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
            checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Cannot read time-skipping MP4");
            require(!(flags & MF_SOURCE_READERF_ERROR), "Time-skipping MP4 stream failed");
            if (sample) {
                require(samples < result.frames && std::llabs(timestamp - LONGLONG(samples) * 10000000 / 30) <= 1,
                    "Time-skipping capture changed 30fps playback timestamps");
                ++samples;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
        }
        require(ended && samples == result.frames, "Time-skipping MP4 frame count changed");
    }
}

void validation(const std::filesystem::path& root) {
    std::vector<Settings> invalid;
    auto value=config(L"");value.timeSkip.multiplier=65;invalid.push_back(value);
    value=config(L"");value.timeSkip.quietAfterMs=999;invalid.push_back(value);
    value=config(L"");value.timeSkip.rampFrames=0;invalid.push_back(value);
    value=config(L"",TimeSkipMode::Manual);value.timeSkip.rangeCount=0;invalid.push_back(value);
    value=config(L"",TimeSkipMode::Manual);value.timeSkip.ranges[0]={3,2};invalid.push_back(value);
    unsigned index=0;
    for(auto settings:invalid) {
        reset();settings.folder=(root/std::to_wstring(index++)).wstring();settings.separateFiles=true;
        Engine engine;engine.configure(settings);engine.record();
        const auto saved=await(engine,[](const auto& status){return status.state==State::Idle;});
        require(saved.recordingFailed&&starts==0&&captures==0&&checks==0&&queries==0&&opens==0&&!std::filesystem::exists(settings.folder),
            "Invalid policy reached a source, disk query or file");
    }
    std::cout<<"PASS policy validation precedes all source and file work.\n";
}
void offAndManual(const std::filesystem::path& root) {
    reset();
    {auto settings=config(root/L"off",TimeSkipMode::Off);settings.recordingLimitSeconds=1;
     Engine engine;engine.configure(settings);engine.record();const auto saved=await(engine,[](const auto& s){return s.state==State::Idle;});
     require(checks==0&&observerCaptures==0&&cancels==0&&!saved.timeSkip.enabled,"Off mode requested observer work");decode(saved,160,120,1);}
    reset();auto settings=config(root/L"manual",TimeSkipMode::Manual);
    Engine engine;engine.configure(settings);engine.record();
    const auto accelerated=await(engine,[](const auto& s){return s.timeSkip.intervalMs>=350;});
    require(accelerated.timeSkip.reason==TimeSkipReason::Manual&&checks==0&&observerCaptures==0,"Manual mode scanned activity");
    settings.timeSkip.mode=TimeSkipMode::Off;settings.intervalMs=1000;engine.configure(settings);
    const auto count=accelerated.frames;await(engine,[&](const auto& s){return s.frames>=count+2;});
    require(engine.status().timeSkip.intervalMs>=350,"Live settings changed frozen cadence policy");
    settings.layers.front().rect={0.1,0.1,0.8,0.8};engine.configure(settings);
    await(engine,[](const auto& s){return s.timeSkip.intervalMs<=110;},700);
    engine.setPaused(true);await(engine,[](const auto& s){return s.state==State::Paused;});
    const auto paused=engine.status();std::this_thread::sleep_for(300ms);require(engine.status().frames==paused.frames,"Paused manual session admitted a frame");
    engine.setPaused(false);await(engine,[&](const auto& s){return s.frames>paused.frames;});
    const auto saved=finish(engine);spacing(submitted());decode(saved,160,120,1);
    std::cout<<"PASS Off/manual zero observation work, smooth growth, frozen policy and pause/resume.\n";
}
void pairedActivity(const std::filesystem::path& root) {
    reset();auto settings=config(root/L"paired");settings.separateFiles=true;settings.captureCursor=false;
    Engine engine;engine.configure(settings);engine.record();
    await(engine,[](const auto& s){return s.timeSkip.intervalMs>=300;},9000);
    require(checks>=3&&observerCaptures>0,"Sparse paired admission did not retain independent observations");
    cameraScene=1;
    await(engine,[](const auto& s){return s.timeSkip.intervalMs==100&&s.timeSkip.reason==TimeSkipReason::Checking;},1800);
    const auto afterCamera=engine.status().frames;
    await(engine,[&](const auto& s){return s.frames>=afterCamera+3;},900);
    // A second distinct source event also resets the shared quiet baseline.
    await(engine,[](const auto& s){return s.timeSkip.intervalMs>=150;},4500);
    desktopScene=1;
    await(engine,[](const auto& s){return s.timeSkip.intervalMs==100&&s.timeSkip.reason==TimeSkipReason::Checking&&s.timeSkip.lastCheckTick>0;},1800);
    const auto saved=finish(engine);const auto values=submitted();spacing(values);
    require(values.size()==saved.frames*2,"Separate files did not share admissions");
    for(size_t i=0;i<values.size();i+=2)require(!values[i].secondary&&values[i+1].secondary&&values[i+1].tick-values[i].tick<200,"Pair was rescheduled independently");
    decode(saved,160,120,2);
    require(checks<=unsigned(saved.elapsed)+2&&observerCaptures==checks,"Optional checks exceeded their bounded 1 Hz cadence");
    std::cout<<"Paired observation evidence: active_seconds="<<saved.elapsed<<" frames_per_video="<<saved.frames
        <<" camera_requests="<<checks<<" camera_reports="<<observationReports<<" desktop_small_captures="<<observerCaptures
        <<" disk_queries="<<queries<<'\n';
    std::cout<<"PASS paired quiet detection and activity return share output cadence without oversampling.\n";
}
void unavailableAndPause(const std::filesystem::path& root) {
    reset();auto settings=config(root/L"unavailable");
    Engine engine;engine.configure(settings);engine.record();
    await(engine,[](const auto& s){return s.timeSkip.intervalMs>=300;},9000);
    observationMode=1; // No new report, not fresh quiet evidence.
    await(engine,[](const auto& s){return s.timeSkip.intervalMs==100&&s.timeSkip.reason==TimeSkipReason::Unavailable;},4200);
    const auto unavailable=engine.status();require(!unavailable.error&&unavailable.timeSkip.observationDelayed,"Observer staleness became a recording error or lacked delay status");
    observationMode=2;
    await(engine,[](const auto& s){return std::wstring(s.timeSkip.diagnostic.data()).find(L"allocate")!=std::wstring::npos;},1500);
    require(!engine.status().recordingFailed,"Optional observer exception stopped recording");
    engine.setPaused(true);await(engine,[](const auto& s){return s.state==State::Paused;});
    const auto checkCount=checks.load(), captureCount=captures.load();const auto frames=engine.status().frames;
    std::this_thread::sleep_for(1200ms);require(checks==checkCount&&captures==captureCount&&engine.status().frames==frames,"Paused hidden session continued observer work");
    observationMode=0;engine.setPaused(false);await(engine,[&](const auto& s){return s.frames>frames&&s.timeSkip.reason==TimeSkipReason::Checking;});
    require(engine.status().timeSkip.intervalMs==100&&cancels>0,"Resume reused quiet evidence or failed to cancel observation");
    const auto saved=finish(engine);spacing(submitted());decode(saved,160,120,1);
    std::cout<<"PASS stale/throwing observers restore normal cadence; pause cancels work and resume resets evidence.\n";
}
void resetReceipt(const std::filesystem::path& root) {
    reset();auto settings=config(root/L"reset-receipt");
    Engine engine;engine.configure(settings);engine.record();
    await(engine,[](const auto& s){return s.frames>=2&&checks>=1;});
    engine.setPaused(true);await(engine,[](const auto& s){return s.state==State::Paused;});
    retainedReceipt=GetTickCount64();std::this_thread::sleep_for(150ms);observationMode=3;
    engine.setPaused(false);
    await(engine,[](const auto& s){return s.timeSkip.reason==TimeSkipReason::Unavailable&&s.timeSkip.diagnostic[0];},1400);
    std::this_thread::sleep_for(1100ms);require(engine.status().timeSkip.intervalMs==100,"A retained pre-resume sample qualified as new quiet evidence");
    observationMode=0;
    await(engine,[](const auto& s){return s.timeSkip.reason==TimeSkipReason::Checking;},1400);
    std::this_thread::sleep_for(500ms);require(engine.status().timeSkip.intervalMs==100,"Fresh baseline borrowed quiet dwell from before reset");
    const auto saved=finish(engine);spacing(submitted());decode(saved,160,120,1);
    std::cout<<"PASS pre-resume cached receipts cannot establish quiet evidence or borrow paused time.\n";
}
void scheduleAndLimit(const std::filesystem::path& root) {
    reset();auto settings=config(root/L"finite-schedule",TimeSkipMode::Manual);
    settings.timeSkip.repeatSeconds=0;settings.timeSkip.ranges[0]={0,3};settings.recordingLimitSeconds=4;
    Engine engine;engine.configure(settings);engine.record();
    await(engine,[](const auto& s){return s.timeSkip.intervalMs>100;});
    await(engine,[](const auto& s){return s.elapsed>=3&&s.timeSkip.reason==TimeSkipReason::Normal&&s.timeSkip.intervalMs==100;},4000);
    const auto saved=await(engine,[](const auto& s){return s.state==State::Idle;},2000);
    require(saved.message.find(L"time limit")!=std::wstring::npos&&!saved.error,"Schedule displaced automatic stop outcome");
    spacing(submitted());decode(saved,160,120,1);
    reset();settings=config(root/L"large-interval",TimeSkipMode::Manual);settings.intervalMs=MaxCaptureIntervalMs;
    settings.timeSkip.repeatSeconds=INT_MAX;settings.timeSkip.ranges[0]={0,INT_MAX};settings.timeSkip.multiplier=64;settings.recordingLimitSeconds=1;
    Engine longSession;longSession.configure(settings);longSession.record();
    const auto large=await(longSession,[](const auto& s){return s.frames==1;});require(large.timeSkip.intervalMs>MaxCaptureIntervalMs,"Effective interval was silently capped at one day");
    const auto longSaved=await(longSession,[](const auto& s){return s.state==State::Idle;},2500);require(longSaved.frames==1,"Long interval ignored the stop deadline");decode(longSaved,160,120,1);
    std::cout<<"PASS finite schedule return, stop deadline priority, and effective intervals above 24 hours.\n";
}
void optionalSlowQueryLimit(const std::filesystem::path& root) {
    reset();auto settings=config(root/L"slow-observer");settings.recordingLimitSeconds=1;
    observationDelay=1200;
    Engine engine;engine.configure(settings);engine.record();
    const auto saved=await(engine,[](const auto& s){return s.state==State::Idle;},3500);
    require(saved.frames==1&&saved.message.find(L"time limit")!=std::wstring::npos&&!saved.error,"Slow observation admitted a frame after the active deadline");decode(saved,160,120,1);
    std::cout<<"PASS automatic stop beats a slow optional observation.\n";
}
void splitPolicyContinuity(const std::filesystem::path& root) {
    reset(); auto settings=config(root/L"split-manual",TimeSkipMode::Manual);
    settings.segmentDurationSeconds=1; settings.recordingLimitSeconds=4;
    settings.timeSkip.repeatSeconds=0; settings.timeSkip.ranges[0]={0,3};
    {
        Engine engine; engine.configure(settings); engine.record();
        await(engine,[](const auto& s){return s.completedSegments>=1&&s.timeSkip.intervalMs>100;});
        await(engine,[](const auto& s){return s.elapsed>=3&&s.timeSkip.reason==TimeSkipReason::Normal&&s.timeSkip.intervalMs==100;},4000);
        const auto saved=await(engine,[](const auto& s){return s.state==State::Idle;},2500);
        require(saved.elapsed>=4&&saved.elapsed<5.5&&checks==0&&observerCaptures==0,
            "Split restarted the manual schedule clock or enabled observers");
        spacing(submitted()); split_test::verify(settings.folder,saved,false);
    }
    reset(); settings=config(root/L"split-quiet"); settings.segmentDurationSeconds=1; settings.separateFiles=true;
    {
        Engine engine; engine.configure(settings); engine.record();
        await(engine,[](const auto& s){return s.frames>=1;});
        const auto cancelled=cancels.load();
        const auto accelerated=await(engine,[](const auto& s){return s.completedSegments>=3&&s.timeSkip.intervalMs>=300;},9000);
        require(starts==1&&cancels==cancelled&&checks<=unsigned(accelerated.elapsed)+2,
            "Split restarted source/quiet evidence or oversampled checks");
        cameraScene=1;
        await(engine,[](const auto& s){return s.timeSkip.intervalMs==100&&s.timeSkip.reason==TimeSkipReason::Checking;},2000);
        const auto saved=finish(engine);
        spacing(submitted()); split_test::verify(settings.folder,saved,true);
    }
    std::cout<<"PASS file splits preserve manual schedule, quiet dwell and shared paired cadence.\n";
}
void splitResumeObservation(const std::filesystem::path& root, bool split, bool manual = false) {
    reset(); auto settings = config(root / (manual ? L"resume-split-manual" : split ? L"resume-split-quiet" : L"resume-off-quiet"),
        manual ? TimeSkipMode::Manual : TimeSkipMode::Quiet);
    settings.layers = preset(Mode::Desktop); settings.segmentDurationSeconds = split ? 1 : 0;
    gateNextWrite = true;
    Engine engine; ScopedWriteGateRelease release;
    engine.configure(settings); engine.record();
    await(engine, [](const auto&) { return writeGateEntered.load(); });
    engine.setPaused(true); // Queued before this admitted write crosses the cut.
    std::this_thread::sleep_for(1100ms); releaseWriteGate();
    const auto paused = await(engine, [](const auto& s) { return s.state == State::Paused; });
    require(paused.elapsed >= 1 && paused.completedSegments == 0, "Pause did not retain the overdue first part");
    std::this_thread::sleep_for(100ms);
    require(engine.status().elapsed == paused.elapsed, "Boundary pause consumed active time");
    const auto before = observerCaptures.load(); engine.setPaused(false);
    const auto resumed = await(engine, [&](const auto& s) {
        return s.frames >= 2 && s.timeSkip.intervalMs > 100 && (manual || observerCaptures >= before + 2);
    }, 5000);
    require(!split || resumed.completedSegments > 0, "Resume did not finalize the overdue part");
    require(checks == 0 && starts == 0 && (!manual || observerCaptures == 0), "Desktop/manual resume enabled unrelated camera observation");
    const auto saved = finish(engine); spacing(submitted());
    if (split) split_test::verify(settings.folder, saved, false); else decode(saved, 160, 120, 1);
    std::cout << "PASS Resume restarts desktop observation across overdue split; split=" << split << " manual=" << manual << ".\n";
}
void combinedAndSourceReset(const std::filesystem::path& root) {
    reset();auto settings=config(root/L"combined",TimeSkipMode::QuietWithinSchedule);
    settings.timeSkip.repeatSeconds=0;settings.timeSkip.ranges[0]={3,30};
    Engine engine;engine.configure(settings);engine.record();
    const auto outside=await(engine,[](const auto& s){return s.elapsed>=2&&checks>=2;});
    require(outside.timeSkip.reason==TimeSkipReason::Normal&&outside.timeSkip.intervalMs==100,"Combined mode accelerated outside its range");
    await(engine,[](const auto& s){return s.timeSkip.intervalMs>=150;},3500);
    const auto priorStarts=starts.load();
    settings.layers=preset(Mode::Desktop);engine.configure(settings);
    await(engine,[](const auto& s){return s.timeSkip.intervalMs==100&&s.timeSkip.reason!=TimeSkipReason::Quiet;},700);
    settings.layers=preset(Mode::Camera);engine.configure(settings);
    await(engine,[&](const auto& s){return starts>priorStarts&&s.timeSkip.intervalMs==100&&s.timeSkip.reason==TimeSkipReason::Checking;},1600);
    require(!engine.status().error,"Reopening the same synthetic camera broke recording");
    const auto saved=finish(engine);spacing(submitted());decode(saved,160,120,1);
    std::cout<<"PASS combined eligibility retains outside-window observations; source removal/reopen resets quiet evidence.\n";
}
void nightWindows(const std::filesystem::path& root) {
    reset();auto settings=config(root/L"night",TimeSkipMode::Quiet);settings.intervalMs=1000;settings.night.enabled=true;settings.night.durationMs=1000;
    Engine engine;engine.configure(settings);engine.record();
    await(engine,[](const auto& s){return s.frames>=3&&s.timeSkip.intervalMs>1000;},6500);
    const auto baselineCancels=nightCancels.load();cameraScene=1;
    await(engine,[](const auto& s){return s.timeSkip.reason==TimeSkipReason::Checking&&s.timeSkip.intervalMs==1000;},2000);
    require(nightCancels==baselineCancels,"Activity return truncated an accepted Night window");
    const auto count=engine.status().frames;await(engine,[&](const auto& s){return s.frames>count;},2500);
    const auto saved=finish(engine);const auto values=submitted();const auto integrations=begun();
    require(!integrations.empty()&&checks>=3,"Night recording had no independent raw-source observations");
    for(size_t i=0;i<values.size();++i)require(i<integrations.size()&&integrations[i].duration==1000&&values[i].tick>=integrations[i].tick+1000,"Night exposure was shortened or scaled with time skipping");
    spacing(values,990);decode(saved,160,120,1);
    std::cout<<"PASS Night activity return retains complete base-bounded windows.\n";
}
void nightSourceTransition(const std::filesystem::path& root) {
    reset();auto settings=config(root/L"night-source-transition");settings.layers=preset(Mode::Desktop);
    settings.intervalMs=2000;settings.night.enabled=true;settings.night.durationMs=1000;
    Engine engine;engine.configure(settings);engine.record();
    await(engine,[](const auto& s){return s.frames==1;});
    settings.layers=preset(Mode::Camera);engine.configure(settings);
    await(engine,[](const auto& s){return s.frames>=2;},3500);
    const auto saved=finish(engine);spacing(submitted(),1980);decode(saved,160,120,1);
    require(!begun().empty()&&begun()[0].duration==1000,"Night source transition changed the configured blend");
    std::cout<<"PASS live desktop-to-Night transition preserves base admission spacing and complete exposure.\n";
}
}
namespace lapse {
struct CameraClient::Impl {bool active=false;uint64_t sequence=0,token=0,nightStart=0;uint32_t duration=0;};
CameraClient::CameraClient():impl_(std::make_unique<Impl>()){}
CameraClient::~CameraClient()=default;
bool CameraClient::start(const std::wstring& id,std::wstring& error, CameraResolution){++starts;error.clear();impl_->active=id==L"skip-camera";return impl_->active;}
void CameraClient::stop(){impl_->active=false;impl_->token=0;}
bool CameraClient::latest(Frame& output,std::wstring& error){error.clear();if(!impl_->active){error=L"Synthetic camera stopped.";return false;}pixels(output,160,120,cameraScene);return true;}
bool CameraClient::observeActivity(uint64_t,CameraObservation& output,std::wstring& error){
    ++checks;error.clear();std::this_thread::sleep_for(std::chrono::milliseconds(observationDelay.exchange(0)));
    if(observationMode==1)return false;
    if(observationMode==2)throw std::bad_alloc();
    Frame frame;pixels(frame,160,120,cameraScene);describeTimeSkipFrame(frame,output.descriptor);
    output.epoch=1;output.sequence=++impl_->sequence;output.receivedTick=observationMode==3?retainedReceipt.load():GetTickCount64();output.sourceWidth=160;output.sourceHeight=120;++observationReports;return true;
}
void CameraClient::cancelActivityObservation()noexcept{++cancels;}
bool CameraClient::beginNight(uint64_t token,uint32_t duration,const NightSettings&,std::wstring& error){
    error.clear();impl_->token=token;impl_->nightStart=GetTickCount64();impl_->duration=duration;
    std::lock_guard<std::mutex> lock(evidenceMutex);windows.push_back({impl_->nightStart,duration});return true;
}
bool CameraClient::nightResult(uint64_t token,Frame& output,NightWindowResult& result,std::wstring& error){
    error.clear();if(token!=impl_->token){error=L"Synthetic stale Night token.";return false;}
    if(GetTickCount64()<impl_->nightStart+impl_->duration)return false;
    result={};result.beginTick=impl_->nightStart;result.endTick=impl_->nightStart+impl_->duration;result.firstSampleTick=result.beginTick+100;result.lastSampleTick=result.endTick-100;
    result.exposure.samples=5;result.exposure.suggestedDurationMs=1000;pixels(output,160,120,0);return true;
}
void CameraClient::cancelNight()noexcept{++nightCancels;impl_->token=0;}
bool captureMonitor(const std::wstring& id,int width,int height,bool cursor,Frame& output,std::wstring& error){
    ++captures;
    if(width==TimeSkipWidth && height==TimeSkipHeight){require(!cursor,"Quiet observation included the desktop cursor");++observerCaptures;}
    error.clear();if(id!=L"skip-desktop"){error=L"Unknown synthetic desktop.";return false;}pixels(output,width,height,desktopScene);return true;
}
}
int main(){
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(com))return 1;
    if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();return 1;}
    const auto root=std::filesystem::current_path()/(L"engine-time-skip-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    int result=0;
    try{validation(root);offAndManual(root);pairedActivity(root);unavailableAndPause(root);resetReceipt(root);scheduleAndLimit(root);optionalSlowQueryLimit(root);splitPolicyContinuity(root);
        splitResumeObservation(root,false);splitResumeObservation(root,true);splitResumeObservation(root,true,true);
        combinedAndSourceReset(root);nightWindows(root);nightSourceTransition(root);
        std::filesystem::remove_all(root);std::cout<<"All synthetic time-skipping engine contracts passed.\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';std::wcerr<<L"Artifacts retained at "<<root.wstring()<<L'\n';result=1;}
    MFShutdown();CoUninitialize();return result;
}

#include "engine_person_camera_stub.h"

// This fixture owns no native desktop capture surface.
namespace lapse { void releaseDesktopCaptureCache() noexcept {} }

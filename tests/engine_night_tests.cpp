// Engine admission and lifecycle with synthetic asynchronous camera windows,
// real MP4 encoding/decoding, inert power requests and owned output folders.
#include "engine.h"
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
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
struct Begin { uint64_t token, tick; uint32_t duration; int target; };
std::mutex observationsMutex;
std::vector<Begin> begins;
std::vector<lapse::CameraResolution> cameraResolutions;
std::atomic<unsigned> cameraCloses{0};
std::atomic<unsigned> cancellations{0}, opens{0}, previews{0}, resultPolls{0};
std::atomic<bool> holdResult{false}, failBegin{false}, failResult{false}, lowSpace{false};
std::atomic<int> resultDelayMs{0}, writeDelayMs{0}, suggestedMs{1000};
void reset() {
    std::lock_guard<std::mutex> lock(observationsMutex); begins.clear(); cameraResolutions.clear(); cameraCloses=0;
    cancellations=opens=previews=resultPolls=0;
    holdResult=failBegin=failResult=lowSpace=false;
    resultDelayMs=writeDelayMs=0; suggestedMs=1000;
}
std::vector<Begin> observations() { std::lock_guard<std::mutex> lock(observationsMutex); return begins; }
std::vector<lapse::CameraResolution> resolutions() { std::lock_guard<std::mutex> lock(observationsMutex); return cameraResolutions; }
void pixels(lapse::Frame& f,int width,int height,uint8_t shade) {
    f.width=width;f.height=height;f.pixels.resize(size_t(width)*height*4);
    for(size_t i=0;i<f.pixels.size();i+=4) { f.pixels[i]=f.pixels[i+1]=f.pixels[i+2]=shade;f.pixels[i+3]=255; }
}
}
namespace lapse {
class NightTestEncoder {
    Encoder real_;
public:
    bool open(const std::wstring& path,int width,int height,int fps,std::wstring& error,EncodingQuality q,EncodingMode mode, bool recoveryMode) {
        ++opens;return real_.open(path,width,height,fps,error,q,mode, recoveryMode);
    }
    bool write(const Frame& f,std::wstring& error) {
        std::this_thread::sleep_for(std::chrono::milliseconds(writeDelayMs.exchange(0)));
        return real_.write(f,error);
    }
    bool finishForPublication(std::wstring& error) {return real_.finishForPublication(error);}
    bool finish(std::wstring& error) {return real_.finish(error);}
    DWORD publish(const std::wstring& path) {return real_.publish(path);}
    void releasePublication() noexcept {real_.releasePublication();}
    bool emptyOutputDiscarded() const noexcept { return real_.emptyOutputDiscarded(); }
    uint64_t frames()const {return real_.frames();}
};
}
EXECUTION_STATE WINAPI nightExecutionState(EXECUTION_STATE) {return ES_CONTINUOUS;}
BOOL WINAPI nightDiskSpace(LPCWSTR, PULARGE_INTEGER available, PULARGE_INTEGER, PULARGE_INTEGER) {
    available->QuadPart=lowSpace?0:1024ULL*1024*1024;return TRUE;
}
#define Encoder NightTestEncoder
#define SetThreadExecutionState nightExecutionState
#define GetDiskFreeSpaceExW nightDiskSpace
#include "../src/engine.cpp"
#undef GetDiskFreeSpaceExW
#undef SetThreadExecutionState
#undef Encoder

namespace {
using namespace lapse;
using Microsoft::WRL::ComPtr;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void checked(HRESULT hr,const char* message) {require(SUCCEEDED(hr),message);}
template<class Predicate> Status await(Engine& engine,Predicate predicate,int timeout=7000) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
    do { auto s=engine.status();if(predicate(s))return s;std::this_thread::sleep_for(5ms); }
    while(std::chrono::steady_clock::now()<end);
    std::wcerr<<engine.status().message<<L'\n';throw std::runtime_error("Timed out waiting for night engine");
}
Settings config(const std::filesystem::path& folder,bool separate=false) {
    Settings s;s.cameraId=L"synthetic-night";s.monitorId=L"synthetic-desktop";
    s.layers=preset(Mode::Camera);s.width=320;s.height=240;s.intervalMs = 1000;
    s.folder=folder.wstring();s.preview=false;s.separateFiles=separate;
    s.night.enabled=true;s.night.durationMs=1000;return s;
}
Status finish(Engine& engine) {
    engine.finish();return await(engine,[](const auto& s){return s.state==State::Idle;});
}
void decode(const std::wstring& path,uint64_t expected,uint8_t shade) {
    constexpr DWORD stream=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"Cannot open night MP4");
    ComPtr<IMFMediaType> type;checked(MFCreateMediaType(&type),"Cannot create decoded type");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Cannot set video type");
    checked(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"Cannot set pixel type");
    checked(reader->SetCurrentMediaType(stream,nullptr,type.Get()),"Cannot decode night MP4");
    uint64_t count=0;bool ended=false;
    for(uint64_t attempt=0;attempt<expected+100;++attempt) {
        DWORD flags=0;LONGLONG timestamp=0;ComPtr<IMFSample> sample;
        checked(reader->ReadSample(stream,0,nullptr,&flags,&timestamp,&sample),"Night MP4 read failed");
        require(!(flags&MF_SOURCE_READERF_ERROR),"Night MP4 stream error");
        if(sample) {
            require(count<expected&&std::llabs(timestamp-static_cast<LONGLONG>(count)*10000000/30)<=1,"Night MP4 count or timestamps differ");
            ComPtr<IMFMediaBuffer> buffer;checked(sample->ConvertToContiguousBuffer(&buffer),"Cannot read night pixels");
            BYTE* p=nullptr;DWORD length=0;checked(buffer->Lock(&p,nullptr,&length),"Cannot lock decoded pixels");
            const bool valid=length>=320*240*3/2;
            const int center=valid?p[120*320+160]:0;buffer->Unlock();
            require(valid&&std::abs(center-(16+int(shade)*219/255))<=8,"Video contains raw/incorrect source pixels");
            ++count;
        }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM){ended=true;break;}
    }
    require(ended&&count==expected,"Night MP4 frame count wrong");
}
void verify(const Status& s,uint64_t count,bool separate,bool failure=false) {
    require(s.state==State::Idle&&s.frames==count&&s.recordingFailed==failure,"Night recording outcome wrong");
    require(s.savedPaths.size()==(separate?2u:1u),"Night output paths missing");
    for(const auto& path:s.savedPaths)decode(path,count,path.find(L"-desktop.mp4")!=std::wstring::npos?40:180);
}
void validation(const std::filesystem::path& root) {
    for(int duration:{500,2000}) {
        reset();auto s=config(root/std::to_wstring(duration));s.night.durationMs=duration;
        Engine engine;engine.configure(s);engine.record();
        const auto result=await(engine,[](const auto& value){return value.state==State::Idle;});
        require(result.recordingFailed&&opens==0&&observations().empty()&&!std::filesystem::exists(s.folder),"Invalid night settings created work or files");
    }
    reset();auto s=config(root/L"desktop-ignores-night");s.layers=preset(Mode::Desktop);s.night.durationMs=123;
    Engine engine;engine.configure(s);engine.record();
    await(engine,[](const auto& value){return value.frames==1;});const auto result=finish(engine);
    require(result.cameraInput.width==0&&result.cameraInput.height==0,"Desktop-only recording reported camera dimensions");
    require(!result.recordingFailed&&observations().empty()&&!result.nightEnabled,"Desktop-only session used night controls");
    decode(result.savedPath,1,40);
    std::cout<<"PASS validation before output and desktop-only scope.\n";
}
void preparingPreview(const std::filesystem::path& root) {
    reset();auto s=config(root/L"preparing");s.preview=true;holdResult=true;
    Engine engine;engine.configure(s);engine.record();
    await(engine,[](const auto& value){return value.state==State::Starting&&value.preview&&previews>0&&!observations().empty();});
    std::this_thread::sleep_for(1150ms);const auto waiting=engine.status();
    require(waiting.state==State::Starting&&waiting.frames==0&&waiting.elapsed==0&&opens==0&&
        !std::filesystem::exists(s.folder),"Preview opened a writer or consumed initial blend");
    const auto result=finish(engine);holdResult=false;
    require(!result.recordingFailed&&result.savedPaths.empty()&&cancellations>0&&!result.nightWaiting,"Pending finish did not cancel cleanly");
    std::cout<<"PASS pending preview/finish has no output or active time.\n";
}
void singleSource(const std::filesystem::path& root,Mode mode) {
    reset();auto s=config(root/(mode==Mode::Camera?L"camera-only":L"overlay"));
    s.layers=preset(mode);s.intervalMs = 60000;Engine engine;engine.configure(s);engine.record();
    const auto delivered=await(engine,[](const auto& value){return value.frames==1;});
    require(delivered.cameraInput.width==320&&delivered.cameraInput.height==240&&delivered.cameraInput.generation!=0&&previews==0,
        "Completed Night input geometry was missing or required an extra raw camera read");
    const auto result=finish(engine);
    require(result.cameraInput.width==0&&result.cameraInput.height==0,"Hidden finish retained retired camera dimensions");
    require(!result.recordingFailed&&result.savedPaths.size()==1&&result.frames==1&&result.night.samples==5,
        "Single-source night output lost its completed camera window");
    decode(result.savedPath,1,mode==Mode::Camera?180:40);
    std::cout<<"PASS single camera/overlay recording, mode="<<int(mode)<<".\n";
}
void firstProcessedPreview(const std::filesystem::path& root) {
    reset();auto s=config(root/L"first-processed-preview");s.preview=true;holdResult=true;
    Engine engine;engine.configure(s);engine.record();
    // Hold an already-complete window until a fresh raw preview was just
    // published. Its clock must not delay the first processed camera image.
    await(engine,[](const auto& value){return value.state==State::Starting&&value.preview&&previews>=3;});
    holdResult=false;
    const auto shown=await(engine,[](const auto& value){return value.frames==1&&value.preview&&value.preview->pixels[0]==180;},800);
    require(shown.night.samples==5,"First processed preview lost its admitted window facts");
    verify(finish(engine),1,false);
    std::cout<<"PASS first processed Night window promptly replaces a recent raw preview.\n";
}
void pairedCadence(const std::filesystem::path& root,bool automatic) {
    reset();auto s=config(root/(automatic?L"auto-clamp":L"paired-cadence"),true);s.preview=true;
    s.night.targetBrightness=64;suggestedMs=NightMaxDurationMs;if(automatic)s.night.durationMs=0;
    Engine engine;engine.configure(s);engine.record();
    auto first=await(engine,[](const auto& value){return value.frames==1&&value.preview;});
    const auto rawCalls=previews.load();
    require(first.elapsed<.2&&first.night.samples==5&&first.nightDurationMs==1000&&first.night.suggestedDurationMs==NightMaxDurationMs,"Initial blend counted as active time or lost its dark-scene suggestion");
    s.night.enabled=false;s.night.durationMs=30000;s.night.targetBrightness=128;
    s.width=1920;s.height=1080;
    engine.configure(s);writeDelayMs=80;
    const auto third=await(engine,[](const auto& value){return value.frames>=3;});
    const auto result=finish(engine);verify(result,third.frames,true);
    const auto starts=observations();require(starts.size()>=3,"Missing night windows");
    for(size_t i=0;i<3;++i) {
        require(starts[i].duration==1000&&starts[i].target==64,"Live settings changed frozen night policy or auto interval bound");
        if(i)require(starts[i].token>starts[i-1].token&&starts[i].tick-starts[i-1].tick>=1000&&
            starts[i].tick-starts[i-1].tick<1800,"Equal interval window shortened or skipped a whole slot");
    }
    require(previews==rawCalls,"Active preview replaced processed camera with raw frames");
    require(resolutions()==std::vector<CameraResolution>{CameraResolution::Standard720},"Active output edit renegotiated frozen low camera tier");
    require(resultPolls<=9,"Parent polled long integration at excessive frequency");
    std::cout<<"PASS paired full windows, frozen policy, decoded sources, cadence and auto clamp="<<automatic<<".\n";
}
void limitWindow(const std::filesystem::path& root) {
    reset();auto s=config(root/L"limit-window",true);s.night.durationMs=0;s.intervalMs = 5000;s.recordingLimitSeconds=2;suggestedMs=5000;
    Engine engine;engine.configure(s);const auto started=GetTickCount64();engine.record();
    const auto first=await(engine,[](const auto& value){return value.frames==1;});
    require(GetTickCount64()-started>=3000&&first.elapsed<.2,"Auto initial window missing or included in active limit");
    const auto result=await(engine,[](const auto& value){return value.state==State::Idle;});verify(result,1,true);
    const auto starts=observations();require(starts.size()==2&&starts[0].duration==3000&&starts[1].duration==5000,"Auto suggestion did not schedule complete next window");
    require(result.elapsed>=2&&result.message.find(L"time limit")!=std::wstring::npos&&cancellations>0,"Limit did not cancel incomplete blend successfully");
    require(result.nightDurationMs==3000&&result.night.samples==5&&!result.nightWaiting,"Pending next blend corrupted last completed facts");
    require(resultPolls<=5,"Parent polled before expected completion too frequently");
    std::cout<<"PASS initial Auto startup excluded; deadline cancels longer next blend.\n";
}
void splitDuringWindow(const std::filesystem::path& root) {
    reset(); auto s=config(root/L"split-during-window",true);
    s.intervalMs=2000; s.night.durationMs=2000; s.segmentDurationSeconds=1; s.recordingLimitSeconds=5;
    Engine engine; engine.configure(s); engine.record();
    await(engine,[](const auto& value){return value.frames==1&&value.nightWaiting;});
    const auto cancelled=cancellations.load();
    const auto closed=await(engine,[](const auto& value){return value.completedSegments==1;},2500);
    require(closed.state==State::Recording&&closed.frames==1&&closed.nightWaiting&&cancellations==cancelled,
        "File boundary cancelled a full Night window or ended the session");
    const auto result=await(engine,[](const auto& value){return value.state==State::Idle;},6500);
    require(result.frames>=2&&result.completedSegments>=2&&result.elapsed>=5&&result.elapsed<6.5&&
        result.message.find(L"time limit")!=std::wstring::npos,"Split Night session reset its clock or lost its limit");
    const auto windows=observations();
    require(windows.size()>=result.frames,"Split Night session lost exposure history");
    for(const auto& window:windows)require(window.duration==2000,"File split shortened a Night exposure");
    split_test::verify(s.folder,result,true);
    std::cout<<"PASS deadline-only split preserves in-progress Night windows and paired playback.\n";
}
void pauseWindow(const std::filesystem::path& root) {
    reset();auto s=config(root/L"pause",true);s.preview=true;Engine engine;engine.configure(s);engine.record();
    await(engine,[](const auto& value){return value.frames==1&&observations().size()>=2;});
    engine.setPaused(true);const auto paused=await(engine,[](const auto& value){return value.state==State::Paused;});
    const auto count=observations().size();const auto rawCalls=previews.load();std::this_thread::sleep_for(1100ms);
    require(engine.status().frames==1&&engine.status().elapsed==paused.elapsed&&observations().size()==count,"Pause consumed window or active clock");
    engine.setPaused(false);const auto resumedAt=GetTickCount64();
    await(engine,[](const auto& value){return value.frames==2;});
    require(GetTickCount64()-resumedAt>=1000&&cancellations>0,"Resume reused partial blend");
    require(previews==rawCalls,"Visible pause/resume replaced processed preview with raw camera video");
    verify(finish(engine),2,true);std::cout<<"PASS pause drops partial window; resume starts full fresh window.\n";
}
void lateResult(const std::filesystem::path& root,bool error) {
    reset();auto s=config(root/(error?L"late-error":L"late-result"),true);s.recordingLimitSeconds=2;
    Engine engine;engine.configure(s);engine.record();await(engine,[](const auto& value){return value.frames==1;});
    resultDelayMs=1200;failResult=error;
    const auto result=await(engine,[](const auto& value){return value.state==State::Idle;});verify(result,1,true);
    require(resultDelayMs==0&&result.message.find(L"time limit")!=std::wstring::npos,"Late helper response overrode successful time limit");
    std::cout<<"PASS deadline takes precedence after blocked helper result, error="<<error<<".\n";
}
void retainedFailure(const std::filesystem::path& root,bool disk) {
    reset();auto s=config(root/(disk?L"low-space":L"helper-failure"),true);Engine engine;engine.configure(s);engine.record();
    await(engine,[](const auto& value){return value.frames==1;});
    if(disk)lowSpace=true;else failResult=true;
    const auto result=await(engine,[](const auto& value){return value.state==State::Idle;});verify(result,1,true,true);
    require(result.error&&cancellations>0&&!result.nightWaiting,"Failure did not stop both writers and cancel helper");
    std::cout<<"PASS prior paired frames retained after "<<(disk?"low space":"helper error")<<".\n";
}
void beginFailure(const std::filesystem::path& root) {
    reset();auto s=config(root/L"begin-failure",true);s.intervalMs = 2000;
    Engine engine;engine.configure(s);engine.record();await(engine,[](const auto& value){return value.frames==1;});
    failBegin=true;
    const auto result=await(engine,[](const auto& value){return value.state==State::Idle;});verify(result,1,true,true);
    require(result.message.find(L"Synthetic begin failure")!=std::wstring::npos,"Begin failure diagnostic lost");
    std::cout<<"PASS rejected next window preserves prior paired frames.\n";
}
void shutdownPending(const std::filesystem::path& root) {
    reset();holdResult=true;const auto folder=root/L"shutdown";uint64_t before=0;
    {
        Engine engine;engine.configure(config(folder));engine.record();
        await(engine,[](const auto&){return !observations().empty();});before=GetTickCount64();
    }
    require(GetTickCount64()-before<700&&cancellations>0&&opens==0&&!std::filesystem::exists(folder),"Shutdown waited for whole exposure or created output");
    std::cout<<"PASS shutdown cancels pending blend promptly.\n";
}
void frozenInputTier(const std::filesystem::path& root) {
    reset(); auto s=config(root/L"frozen-detail-tier",true);
    s.width=1920; s.height=1080; s.segmentDurationSeconds=1;
    Engine engine; engine.configure(s); engine.record();
    const auto delivered=await(engine,[](const auto& value){return value.frames==1;});
    require(resolutions()==std::vector<CameraResolution>{CameraResolution::Detail1080},"Large separate Night output did not request high camera tier");
    require(delivered.cameraInput.width==1920&&delivered.cameraInput.height==1080,
        "Completed high-tier Night pixels were not reported as actual input geometry");
    // Direct API edits must not renegotiate the accepted session even though
    // the normal UI locks output size. Hidden pause closes/reopens the helper.
    s.width=320; s.height=240; engine.configure(s); engine.setPaused(true);
    await(engine,[](const auto& value){return value.state==State::Paused&&cameraCloses>0;});
    const auto paused=engine.status(); std::this_thread::sleep_for(100ms);
    require(engine.status().elapsed==paused.elapsed,"Paused high-tier session advanced clock");
    engine.setPaused(false); const auto resumed=await(engine,[](const auto& value){return value.frames>=2;});
    require(resumed.cameraInput.width==1920&&resumed.cameraInput.height==1080&&resumed.cameraInput.generation!=delivered.cameraInput.generation,
        "High-tier hidden resume did not publish freshly verified actual input geometry");
    const auto result=finish(engine);
    const auto tiers=resolutions();
    require(tiers.size()==2&&tiers[0]==CameraResolution::Detail1080&&tiers[1]==CameraResolution::Detail1080,
        "Resume or split adopted live draft output tier");
    split_test::verify(s.folder,result,true);
    for(const auto& item:std::filesystem::directory_iterator(s.folder)) {
        ComPtr<IMFSourceReader> reader; checked(MFCreateSourceReaderFromURL(item.path().c_str(),nullptr,&reader),"Cannot open frozen-tier part");
        ComPtr<IMFMediaType> type; checked(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&type),"Cannot read frozen-tier geometry");
        UINT32 width=0,height=0; checked(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&width,&height),"Missing frozen-tier geometry");
        require(width==1920&&height==1080,"Live output edit changed split recording dimensions");
    }
    // A later recording takes the new small draft, while the completed first
    // session remains independent and the camera still reports actual pixels.
    s.folder=(root/L"next-small-tier").wstring(); s.segmentDurationSeconds=0;
    engine.configure(s); engine.record(); await(engine,[](const auto& value){return value.frames==1;});
    const auto next=finish(engine); verify(next,1,true);
    require(resolutions().back()==CameraResolution::Standard720,"New recording did not adopt the current small output tier");
    std::cout<<"PASS high-tier separate Night session freezes across hidden pause/splits; next session adopts new tier.\n";
}
}
namespace lapse {
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) {
    error = L"Unexpected activity observer in an Off-mode fixture."; return false;
}
void CameraClient::cancelActivityObservation() noexcept {}
struct CameraClient::Impl {bool active=false;uint64_t token=0,start=0;uint32_t duration=0;CameraResolution resolution=CameraResolution::Standard720;};
CameraClient::CameraClient():impl_(std::make_unique<Impl>()){}
CameraClient::~CameraClient()=default;
bool CameraClient::start(const std::wstring& id,std::wstring& error, CameraResolution resolution) {
    {std::lock_guard<std::mutex> lock(observationsMutex);cameraResolutions.push_back(resolution);}
    impl_->resolution=resolution;
    error.clear();impl_->active=id==L"synthetic-night";if(!impl_->active)error=L"Unknown synthetic camera.";return impl_->active;
}
void CameraClient::stop(){if(impl_->active)++cameraCloses;impl_->active=false;impl_->token=0;}
bool CameraClient::latest(Frame& output,std::wstring& error) {
    error.clear();if(!impl_->active){error=L"Synthetic camera stopped.";return false;}
    ++previews;pixels(output,impl_->resolution==CameraResolution::Detail1080?1920:320,impl_->resolution==CameraResolution::Detail1080?1080:240,24);return true;
}
bool CameraClient::beginNight(uint64_t token,uint32_t duration,const NightSettings& settings,std::wstring& error) {
    error.clear();if(failBegin||!impl_->active){error=L"Synthetic begin failure.";return false;}
    impl_->token=token;impl_->start=GetTickCount64();impl_->duration=duration;
    std::lock_guard<std::mutex> lock(observationsMutex);begins.push_back({token,impl_->start,duration,settings.targetBrightness});return true;
}
bool CameraClient::nightResult(uint64_t token,Frame& output,NightWindowResult& result,std::wstring& error) {
    ++resultPolls;error.clear();std::this_thread::sleep_for(std::chrono::milliseconds(resultDelayMs.exchange(0)));
    if(failResult||!impl_->active||token!=impl_->token){error=L"Synthetic result failure.";return false;}
    if(holdResult||GetTickCount64()<impl_->start+impl_->duration)return false;
    result={};result.beginTick=impl_->start;result.endTick=impl_->start+impl_->duration;
    result.firstSampleTick=result.beginTick+100;result.lastSampleTick=result.endTick-100;
    result.exposure.samples=5;result.exposure.appliedGain=4;result.exposure.suggestedDurationMs=suggestedMs;
    pixels(output,impl_->resolution==CameraResolution::Detail1080?1920:320,impl_->resolution==CameraResolution::Detail1080?1080:240,180);return true;
}
void CameraClient::cancelNight() noexcept {++cancellations;impl_->token=0;}
bool captureMonitor(const std::wstring& id,int width,int height,bool,Frame& output,std::wstring& error) {
    error.clear();if(id!=L"synthetic-desktop"){error=L"Unknown synthetic desktop.";return false;}
    pixels(output,width,height,40);return true;
}
}
int main() {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(com))return 1;
    if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();return 1;}
    const auto root=std::filesystem::current_path()/(L"engine-night-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    int result=0;
    try {
        validation(root);preparingPreview(root);firstProcessedPreview(root);singleSource(root,Mode::Camera);singleSource(root,Mode::Overlay);
        pairedCadence(root,false);pairedCadence(root,true);
        limitWindow(root);splitDuringWindow(root);pauseWindow(root);lateResult(root,false);lateResult(root,true);
        retainedFailure(root,false);retainedFailure(root,true);beginFailure(root);shutdownPending(root);frozenInputTier(root);
        std::filesystem::remove_all(root);std::cout<<"Night engine: all synthetic real-encoder scenarios passed.\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';std::wcerr<<L"Artifacts kept at "<<root.wstring()<<L'\n';result=1;}
    MFShutdown();CoUninitialize();return result;
}

#include "engine_person_camera_stub.h"

// This fixture owns no native desktop capture surface.
namespace lapse { void releaseDesktopCaptureCache() noexcept {} }

// Actual Engine/core/capture; owned synthetic memory desktop and fake
// camera/encoder. No app, physical display pixels, device host, or power change.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include "encoder.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
constexpr int tile = 64, height = 36, canvasWidth = 3 * tile;
constexpr uint8_t colorA = 51, colorB = 173, colorC = 227, emptyColor = 9;
struct Display { const wchar_t* name; RECT bounds; uint8_t marker; bool primary; const wchar_t* interfaceId; };
const Display originalA{L"DISPLAY-A", {0,0,64,36}, colorA, true, L"interface-A"};
const Display originalB{L"DISPLAY-B", {64,0,128,36}, colorB, false, L"interface-B"};
const Display originalC{L"DISPLAY-C", {128,0,192,36}, colorC, false, L"interface-C"};
std::array<Display,3> displays{};
size_t displayCount = 0;
RECT virtualBounds{};
HDC syntheticDC = nullptr;
HBITMAP syntheticBitmap = nullptr;
HGDIOBJ originalBitmap = nullptr;
uint8_t* syntheticPixels = nullptr;
enum class DuringCopy { None, MoveSelected, ReplaceSelected, Unrelated };
DuringCopy changeDuringBlt = DuringCopy::None;
bool failEnumeration = false;
int acquired = 0, released = 0, copied = 0, enumerated = 0;
std::filesystem::path outputRoot;
std::mutex frameMutex;
std::condition_variable frameChanged;
std::array<uint8_t,8> recordedMarkers{};
unsigned recordedFrames = 0;
bool blockFirstWrite = false, releaseFirstWrite = false, barrierTimeout = false;
std::mutex copyMutex;
std::condition_variable copyChanged;
bool blockCopy = false, copyReached = false, releaseCopy = false;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void topology(std::initializer_list<Display> next) {
    require(next.size() && next.size() <= displays.size(), "Invalid synthetic topology.");
    displayCount = next.size(); std::copy(next.begin(), next.end(), displays.begin());
    virtualBounds = displays[0].bounds;
    for (size_t i=1;i<displayCount;++i) UnionRect(&virtualBounds,&virtualBounds,&displays[i].bounds);
    require(virtualBounds.left >= 0 && virtualBounds.top == 0 && virtualBounds.right <= canvasWidth && virtualBounds.bottom == height,
        "Synthetic topology exceeds owned bitmap.");
    require(GdiFlush() != FALSE, "Cannot flush owned GDI before replacing pixels.");
    std::fill(syntheticPixels, syntheticPixels + canvasWidth * height * 4, emptyColor);
    for (size_t i=0;i<displayCount;++i) {
        const auto& item=displays[i];
        for (LONG y=item.bounds.top;y<item.bounds.bottom;++y)
            for (LONG x=item.bounds.left;x<item.bounds.right;++x)
                for(int c=0;c<4;++c) syntheticPixels[(size_t(y)*canvasWidth+x)*4+c]=item.marker;
    }
}
struct OwnedDesktop {
    OwnedDesktop() {
        syntheticDC = CreateCompatibleDC(nullptr);
        require(syntheticDC != nullptr, "Cannot allocate synthetic memory DC.");
        BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=canvasWidth; info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;
        syntheticBitmap=CreateDIBSection(syntheticDC,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        require(syntheticBitmap && pixels, "Cannot allocate synthetic desktop pixels.");
        originalBitmap=SelectObject(syntheticDC,syntheticBitmap); syntheticPixels=static_cast<uint8_t*>(pixels);
        require(originalBitmap && originalBitmap != HGDI_ERROR, "Cannot select synthetic desktop bitmap.");
    }
    ~OwnedDesktop() {
        if(syntheticDC && originalBitmap) SelectObject(syntheticDC,originalBitmap);
        if(syntheticBitmap) DeleteObject(syntheticBitmap);
        if(syntheticDC) DeleteDC(syntheticDC);
    }
};
lapse::Monitor selected(const wchar_t* id) {
    const auto list=lapse::enumerateMonitors();
    for(const auto& item:list) if(item.id==id) return item;
    throw std::runtime_error("Missing initial synthetic selection.");
}
void sample(const std::wstring& id, uint8_t marker, lapse::Frame& frame) {
    std::wstring error;
    const bool result=lapse::captureMonitor(id,tile,height,true,frame,error);
    require(result && error.empty() && frame.valid(), "Expected accepted desktop capture.");
    for(size_t i=0;i<frame.pixels.size();i+=4)
        require(frame.pixels[i]==marker && frame.pixels[i+1]==marker && frame.pixels[i+2]==marker,
            "Captured pixels do not match expected synthetic display.");
}
void rejected(const std::wstring& id, lapse::Frame& frame, bool beforeCopy = true) {
    std::wstring error;
    const auto capacity=frame.pixels.capacity(); const auto storage=frame.pixels.data(); const int acquisitionsBefore=acquired;
    require(!lapse::captureMonitor(id,tile,height,true,frame,error) && !error.empty() && !frame.valid(),
        "Invalid monitor sample did not fail with invalid output.");
    require(frame.pixels.capacity()==capacity && frame.pixels.data()==storage,"Failed monitor sample discarded reusable storage.");
    if(beforeCopy)require(acquired==acquisitionsBefore,"Unavailable identity reached desktop copying.");
}
void captureCases() {
    topology({originalA,originalB}); const auto selectedA=selected(L"monitor:interface-A");
    lapse::Frame frame; sample(selectedA.id,colorA,frame);
    const auto storage=frame.pixels.data(); const auto capacity=frame.pixels.capacity();
    topology({originalB,originalA}); sample(selectedA.id,colorA,frame);
    sample(L"MONITOR:INTERFACE-a",colorA,frame);
    require(frame.pixels.data()==storage && frame.pixels.capacity()==capacity,"Stable sampling did not reuse pixel allocation.");
    std::cout<<"PASS identity enumeration, reorder, case matching and stable storage reuse\n";

    topology({{L"DISPLAY-B",{0,0,64,36},colorB,true,L"interface-B"}}); rejected(selectedA.id,frame);
    topology({{L"DISPLAY-A",{0,0,64,36},colorB,true,L"interface-B"}}); rejected(selectedA.id,frame);
    std::cout<<"PASS removal and same-name/same-bounds physical replacement reject before copying\n";

    topology({{L"DISPLAY-RENAMED",{64,0,128,36},colorA,false,L"interface-A"},{L"DISPLAY-B",{0,0,64,36},colorB,true,L"interface-B"}});
    sample(selectedA.id,colorA,frame);
    require(frame.pixels.data()==storage && frame.pixels.capacity()==capacity,"Moved display discarded existing pixel allocation.");
    std::cout<<"PASS moved display and changed GDI name follow the same interface identity\n";

    topology({originalA}); rejected(L"monitor:interface-B",frame);
    topology({originalA,originalC}); rejected(L"monitor:interface-B",frame);
    rejected(L"",frame);
    std::cout<<"PASS missing identity rejects outside bounds, inside virtual-desktop gap, and empty selection\n";

    topology({originalA,originalB}); changeDuringBlt=DuringCopy::MoveSelected; rejected(selectedA.id,frame,false);
    require(changeDuringBlt==DuringCopy::None,"In-flight movement barrier was not reached.");
    topology({originalA,originalB}); changeDuringBlt=DuringCopy::ReplaceSelected; rejected(selectedA.id,frame,false);
    require(changeDuringBlt==DuringCopy::None,"In-flight replacement barrier was not reached.");
    topology({originalA,originalB}); changeDuringBlt=DuringCopy::Unrelated; sample(selectedA.id,colorA,frame);
    require(changeDuringBlt==DuringCopy::None,"Unrelated topology barrier was not reached.");
    std::cout<<"PASS in-flight selected movement/replacement rejected; unrelated movement accepted\n";

    topology({originalA,originalB}); failEnumeration=true; rejected(selectedA.id,frame); failEnumeration=false;
    sample(selectedA.id,colorA,frame);
    require(frame.pixels.data()==storage && frame.pixels.capacity()==capacity,"Failure/recovery lost reusable storage.");
    topology({{L"VIRTUAL-1",{0,0,64,36},colorA,true,L""}}); sample(L"gdi:VIRTUAL-1",colorA,frame);
    std::cout<<"PASS partial enumeration rejected, next call recovers, explicit GDI fallback available\n";
}
template<class Predicate>
lapse::Status awaitStatus(lapse::Engine& engine, Predicate predicate) {
    const auto deadline=std::chrono::steady_clock::now()+4s;
    do { auto status=engine.status(); if(predicate(status))return status; std::this_thread::sleep_for(5ms); }
    while(std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error("Timed out waiting for native Engine status.");
}
struct ReleaseWrite {
    ~ReleaseWrite() { { std::lock_guard<std::mutex> lock(frameMutex); releaseFirstWrite=true; } frameChanged.notify_all(); }
};
void activeCase(bool moved, bool outsideControl, bool pinning = false) {
    topology({originalA,originalB});
    const auto monitor=selected(outsideControl?L"monitor:interface-B":L"monitor:interface-A");
    lapse::Settings cfg; cfg.monitor=monitor.bounds; cfg.monitorId=monitor.id;
    cfg.width=tile*2; cfg.height=height*2; cfg.intervalMs = 1000; cfg.preview=false; cfg.folder=outputRoot.wstring();
    { std::lock_guard<std::mutex> lock(frameMutex); recordedFrames=0; blockFirstWrite=true; releaseFirstWrite=barrierTimeout=false; recordedMarkers.fill(0); }
    {
        lapse::Engine engine; ReleaseWrite unblock;
        engine.configure(cfg); engine.record();
        { std::unique_lock<std::mutex> lock(frameMutex);
          require(frameChanged.wait_for(lock,4s,[]{return recordedFrames==1;}),"First recording frame missing.");
          require(recordedMarkers[0]==(outsideControl?colorB:colorA),"Recording began with the wrong selected display."); }
        // The worker is blocked after frame one. Topology changes below do
        // not call configure. The separate pinning control explicitly changes
        // live configuration while the recording must retain its accepted ID.
        if(pinning) {
            cfg.monitorId=L"monitor:interface-B"; cfg.monitor=originalB.bounds;
            cfg.layers[0].rect.w=.75; cfg.preview=true; engine.configure(cfg);
        } else if(outsideControl) topology({originalA});
        else if(moved) topology({{L"DISPLAY-A",{64,0,128,36},colorA,false,L"interface-A"},{L"DISPLAY-B",{0,0,64,36},colorB,true,L"interface-B"}});
        else topology({{L"DISPLAY-B",{0,0,64,36},colorB,true,L"interface-B"}});
        { std::lock_guard<std::mutex> lock(frameMutex); releaseFirstWrite=true; } frameChanged.notify_all();
        if(outsideControl || (!moved && !pinning)) {
            const auto status=awaitStatus(engine,[](const lapse::Status& s){return s.state==lapse::State::Idle && s.error;});
            require(status.frames==1 && status.message.find(L"selected display")!=std::wstring::npos &&
                status.message.find(L"unavailable")!=std::wstring::npos,
                "Removed display did not stop recording with the expected capture error.");
            { std::lock_guard<std::mutex> lock(frameMutex); require(recordedFrames==1,"Removed display allowed a replacement-frame write."); }
            std::cout<<"PASS actual Engine stops on removal "<<(outsideControl?"outside":"inside")
                <<" remaining virtual bounds; no replacement frame written\n";
        } else {
            const auto status=awaitStatus(engine,[](const lapse::Status& s){return s.frames>=2;});
            require(status.state==lapse::State::Recording && !status.error,"Healthy identified display did not leave recording active.");
            { std::lock_guard<std::mutex> lock(frameMutex); require(recordedFrames==2 && recordedMarkers[1]==colorA,
                "Second encoded frame did not retain selected display A."); }
            engine.finish(); awaitStatus(engine,[](const lapse::Status& s){return s.state==lapse::State::Idle;});
            std::cout<<"PASS actual Engine records A then A "
                <<(pinning?"through live ID/layout/preview updates":"after A moves and B occupies old bounds")<<"\n";
        }
    }
    require(!barrierTimeout,"Recording frame barrier timed out.");
}
uint8_t previewMarker(const lapse::Status& status) {
    if(!status.preview || !status.preview->valid())return 0;
    const auto& frame=*status.preview;
    return frame.pixels[(size_t(frame.height/2)*frame.width+frame.width/2)*4];
}
struct ReleaseCopy {
    ~ReleaseCopy() { { std::lock_guard<std::mutex> lock(copyMutex); releaseCopy=true; blockCopy=false; } copyChanged.notify_all(); }
};
void previewIdentityChange() {
    topology({originalA,originalB});
    lapse::Settings cfg; cfg.monitor=originalA.bounds; cfg.monitorId=L"monitor:interface-A";
    cfg.preview=true;
    lapse::Engine engine; ReleaseCopy unblock;
    engine.configure(cfg);
    awaitStatus(engine,[](const lapse::Status& s){return previewMarker(s)==colorA && !s.error;});
    { std::lock_guard<std::mutex> lock(copyMutex); blockCopy=true; copyReached=releaseCopy=false; }
    cfg.monitorId=L"monitor:interface-B"; // Bounds metadata deliberately unchanged.
    engine.configure(cfg);
    require(!engine.status().preview,"ID-only display selection did not retire old preview immediately.");
    { std::unique_lock<std::mutex> lock(copyMutex);
      require(copyChanged.wait_for(lock,4s,[]{return copyReached;}),"New preview did not reach copy barrier."); }
    require(!engine.status().preview,"Old preview reappeared while changed identity was waiting.");
    { std::lock_guard<std::mutex> lock(copyMutex); releaseCopy=true; blockCopy=false; } copyChanged.notify_all();
    awaitStatus(engine,[](const lapse::Status& s){return previewMarker(s)==colorB && !s.error;});
    require(!barrierTimeout,"Preview copy barrier timed out.");
    std::cout<<"PASS ID-only selection retires old preview before new identified display publishes\n";
}
}

int WINAPI reviewGetSystemMetrics(int index) {
    switch(index) {
    case SM_XVIRTUALSCREEN:return virtualBounds.left; case SM_YVIRTUALSCREEN:return virtualBounds.top;
    case SM_CXVIRTUALSCREEN:return virtualBounds.right-virtualBounds.left;
    case SM_CYVIRTUALSCREEN:return virtualBounds.bottom-virtualBounds.top;
    default:throw std::runtime_error("Unexpected system-metric boundary.");
    }
}
HDC WINAPI reviewGetDC(HWND window) { require(!window && syntheticDC,"Unexpected capture DC request."); ++acquired; return syntheticDC; }
int WINAPI reviewReleaseDC(HWND window,HDC dc) { require(!window && dc==syntheticDC,"Unexpected capture DC release."); ++released; return 1; }
BOOL WINAPI reviewGetCursorInfo(PCURSORINFO info) { require(info!=nullptr,"Missing cursor output."); info->flags=0; return TRUE; }
BOOL WINAPI reviewEnumDisplayMonitors(HDC dc,LPCRECT clip,MONITORENUMPROC callback,LPARAM data) {
    require(!dc && !clip,"Unexpected monitor enumeration filters."); ++enumerated;
    for(size_t i=0;i<displayCount;++i) { RECT bounds=displays[i].bounds;
        if(!callback(reinterpret_cast<HMONITOR>(i+1),nullptr,&bounds,data))return FALSE;
        if(failEnumeration)return FALSE; }
    return TRUE;
}
BOOL WINAPI reviewGetMonitorInfoW(HMONITOR monitor,MONITORINFOEXW* info) {
    const size_t i=reinterpret_cast<size_t>(monitor)-1;
    require(i<displayCount && info && info->cbSize==sizeof(*info),"Unexpected monitor-info lookup.");
    info->rcMonitor=info->rcWork=displays[i].bounds; info->dwFlags=displays[i].primary?MONITORINFOF_PRIMARY:0;
    wcscpy_s(info->szDevice,displays[i].name); return TRUE;
}
BOOL WINAPI reviewEnumDisplayDevicesW(LPCWSTR name,DWORD index,PDISPLAY_DEVICEW info,DWORD flags) {
    require(name && info && info->cb==sizeof(*info) && flags==EDD_GET_DEVICE_INTERFACE_NAME,
        "Unexpected display-interface lookup.");
    if(index)return FALSE;
    for(size_t i=0;i<displayCount;++i)if(wcscmp(name,displays[i].name)==0) {
        if(!displays[i].interfaceId[0])return FALSE;
        info->StateFlags=DISPLAY_DEVICE_ACTIVE; wcscpy_s(info->DeviceID,displays[i].interfaceId); return TRUE;
    }
    return FALSE;
}
BOOL WINAPI reviewStretchBlt(HDC target,int x,int y,int w,int h,HDC source,int sx,int sy,int sw,int sh,DWORD mode) {
    require(source==syntheticDC,"Copy attempted a nonfixture desktop."); ++copied;
    { std::unique_lock<std::mutex> lock(copyMutex);
      if(blockCopy) { copyReached=true; copyChanged.notify_all();
          if(!copyChanged.wait_for(lock,4s,[]{return releaseCopy;}))barrierTimeout=true; } }
    const auto change=changeDuringBlt; changeDuringBlt=DuringCopy::None;
    if(change==DuringCopy::MoveSelected)
        topology({{L"DISPLAY-A",{64,0,128,36},colorA,false,L"interface-A"},{L"DISPLAY-B",{0,0,64,36},colorB,true,L"interface-B"}});
    else if(change==DuringCopy::ReplaceSelected)
        topology({{L"DISPLAY-A",{0,0,64,36},colorB,true,L"interface-B"}});
    else if(change==DuringCopy::Unrelated)
        topology({originalA,{L"DISPLAY-B",{128,0,192,36},colorB,false,L"interface-B"}});
    return StretchBlt(target,x,y,w,h,source,sx,sy,sw,sh,mode);
}
EXECUTION_STATE WINAPI reviewExecutionState(EXECUTION_STATE flags) { return flags; }
namespace lapse {
bool CameraClient::beginNight(uint64_t, uint32_t, const NightSettings&, std::wstring& error) {
    error = L"Unexpected night request in ordinary-mode fixture."; return false;
}
bool CameraClient::nightResult(uint64_t, Frame&, NightWindowResult&, std::wstring& error) {
    error = L"Unexpected night result in ordinary-mode fixture."; return false;
}
void CameraClient::cancelNight() noexcept {}
bool CameraClient::observeActivity(uint64_t, CameraObservation&, std::wstring& error) {
    error = L"Unexpected activity observer in an Off-mode fixture."; return false;
}
void CameraClient::cancelActivityObservation() noexcept {}
struct CameraClient::Impl {};
CameraClient::CameraClient():impl_(std::make_unique<Impl>()){}
CameraClient::~CameraClient()=default;
bool CameraClient::start(const std::wstring&,std::wstring&, CameraResolution) { throw std::runtime_error("Unexpected camera activation."); }
void CameraClient::stop(){}
bool CameraClient::latest(Frame&,std::wstring&) { throw std::runtime_error("Unexpected camera read."); }
struct Encoder::Impl { uint64_t frames=0; bool finished=false; };
Encoder::Encoder():impl_(std::make_unique<Impl>()){}
Encoder::~Encoder()=default;
bool Encoder::open(const std::wstring& path,int,int,int,std::wstring& error,EncodingQuality,EncodingMode,bool, const EncodingOptions& ) {
    require(std::filesystem::path(path).parent_path()==std::filesystem::path(fileIOPath(outputRoot.wstring())),"Synthetic encoder escaped fixture output.");
    impl_->frames=0; impl_->finished=false; error.clear(); return true;
}
bool Encoder::write(const Frame& frame,std::wstring& error) {
    require(frame.valid(),"Invalid synthetic encoded frame.");
    std::unique_lock<std::mutex> lock(frameMutex);
    require(recordedFrames<recordedMarkers.size(),"Unexpected recording duration.");
    recordedMarkers[recordedFrames++]=frame.pixels[(size_t(frame.height/2)*frame.width+frame.width/2)*4];
    ++impl_->frames; frameChanged.notify_all();
    if(blockFirstWrite && recordedFrames==1 && !frameChanged.wait_for(lock,4s,[]{return releaseFirstWrite;}))barrierTimeout=true;
    error.clear(); return true;
}
bool Encoder::finish(std::wstring& error) { impl_->finished=true; error.clear(); return true; }
bool Encoder::finishForPublication(std::wstring& error) { return finish(error); }
bool Encoder::emptyOutputDiscarded() const noexcept { return impl_->finished && !impl_->frames; }
DWORD Encoder::publish(const std::wstring& path) {
    require(std::filesystem::path(path).parent_path()==std::filesystem::path(fileIOPath(outputRoot.wstring())),
        "Synthetic publication escaped fixture folder.");
    return ERROR_SUCCESS; // Deliberately no video file in this display fixture.
}
void Encoder::releasePublication() noexcept {}
uint64_t Encoder::frames()const{return impl_->frames;}
}
int main() {
    try {
        OwnedDesktop desktop;
        outputRoot=std::filesystem::current_path()/(L"topology-output-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(outputRoot),"Cannot create unique empty output root.");
        captureCases(); activeCase(false,false); activeCase(true,false); activeCase(false,true); activeCase(false,false,true);
        previewIdentityChange();
        require(acquired==released,"Actual capture did not balance intercepted DC ownership.");
        require(RemoveDirectoryW(outputRoot.c_str())!=FALSE,"Fixture output root was not empty/removable.");
        std::cout<<"All 11 bounded display identity groups passed; DC acquisitions="<<acquired
            <<" releases="<<released<<" copies="<<copied<<". No physical pixels, camera, video files, or power request.\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<std::endl;
        if(!outputRoot.empty())std::wcerr<<L"Fixture output root retained: "<<outputRoot.wstring()<<std::endl;
        return 1;
    }
}

#include "engine_person_camera_stub.h"

// Real encoders and owned child termination; generated pixels only. No devices.
#include "encoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>

HRESULT WINAPI recoveryCreateSink(IMFByteStream*, IMFMediaType*, IMFMediaType*, IMFMediaSink**);
HRESULT WINAPI recoveryCreateWriter(IMFMediaSink*, IMFAttributes*, IMFSinkWriter**);
#define MFCreateFMPEG4MediaSink recoveryCreateSink
#define MFCreateSinkWriterFromMediaSink recoveryCreateWriter
#include "../src/encoder.cpp"
#undef MFCreateSinkWriterFromMediaSink
#undef MFCreateFMPEG4MediaSink

EXECUTION_STATE WINAPI recoveryExecutionState(EXECUTION_STATE value) { return value; }
#define SetThreadExecutionState recoveryExecutionState
#include "../src/engine.cpp"
#undef SetThreadExecutionState

using Microsoft::WRL::ComPtr;
namespace {
bool failSink = false, failWriter = false, failMarker = false, failFinalize = false;
unsigned sinkCreations = 0, markerCalls = 0, flushCalls = 0;
unsigned engineFailMarkerAt = 0, engineFailWriteAt = 0, engineWriteCalls = 0;
std::atomic<unsigned> engineCaptures{0};
class Writer final : public IMFSinkWriter {
    std::atomic<ULONG> refs_{1};
    ComPtr<IMFSinkWriter> real_;
public:
    explicit Writer(IMFSinkWriter* real) : real_(real) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSinkWriter)) {
            *value = static_cast<IMFSinkWriter*>(this); AddRef(); return S_OK;
        }
        return real_->QueryInterface(iid, value); // Keep real transform verification.
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { const auto n = --refs_; if (!n) delete this; return n; }
    STDMETHODIMP AddStream(IMFMediaType* a, DWORD* b) override { return real_->AddStream(a,b); }
    STDMETHODIMP SetInputMediaType(DWORD a, IMFMediaType* b, IMFAttributes* c) override { return real_->SetInputMediaType(a,b,c); }
    STDMETHODIMP BeginWriting() override { return real_->BeginWriting(); }
    STDMETHODIMP WriteSample(DWORD a, IMFSample* b) override {
        ++engineWriteCalls;
        return engineFailWriteAt && engineWriteCalls==engineFailWriteAt ? E_FAIL : real_->WriteSample(a,b);
    }
    STDMETHODIMP SendStreamTick(DWORD a, LONGLONG b) override { return real_->SendStreamTick(a,b); }
    STDMETHODIMP PlaceMarker(DWORD a, LPVOID b) override { return real_->PlaceMarker(a,b); }
    STDMETHODIMP NotifyEndOfSegment(DWORD a) override {
        ++markerCalls;
        return failMarker || (engineFailMarkerAt && markerCalls==engineFailMarkerAt) ? E_FAIL : real_->NotifyEndOfSegment(a);
    }
    STDMETHODIMP Flush(DWORD a) override { ++flushCalls; return real_->Flush(a); }
    STDMETHODIMP Finalize() override { return failFinalize ? E_FAIL : real_->Finalize(); }
    STDMETHODIMP GetServiceForStream(DWORD a, REFGUID b, REFIID c, LPVOID* d) override { return real_->GetServiceForStream(a,b,c,d); }
    STDMETHODIMP GetStatistics(DWORD a, MF_SINK_WRITER_STATISTICS* b) override { return real_->GetStatistics(a,b); }
};
constexpr int width = 320, height = 240, fps = 30, crashFrames = 31;
constexpr DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::wcerr << lapse::errorText(hr) << L'\n'; throw std::runtime_error(message); }
}
void encoded(bool okay, const std::wstring& error) {
    if (!okay) { std::wcerr << error << L'\n'; throw std::runtime_error("Recovery encoder operation failed"); }
}
lapse::Frame pattern(int index) {
    lapse::Frame frame{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
    for (int y=0; y<height; ++y) for (int x=0; x<width; ++x) {
        auto* p = &frame.pixels[(size_t(y)*width+x)*4];
        if (y<height/2 && x<width/2) p[index%2 ? 0 : 2]=255;
        else if (y<height/2) p[1]=255;
        else if (x<width/2) p[0]=p[1]=p[2]=255;
        p[3]=255;
    }
    return frame;
}
bool hardwareAvailable() {
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video,MFVideoFormat_H264};
    IMFActivate** items=nullptr; UINT32 count=0;
    checked(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,MFT_ENUM_FLAG_HARDWARE|MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,&output,&items,&count),"Enumerate hardware H.264");
    for (UINT32 i=0;i<count;++i) items[i]->Release(); CoTaskMemFree(items);
    return count!=0;
}
std::vector<uint64_t> decode(const std::filesystem::path& path, int expected) {
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"Open recovery output");
    ComPtr<IMFMediaType> native; checked(reader->GetNativeMediaType(video,0,&native),"Read recovery codec");
    GUID codec{}; checked(native->GetGUID(MF_MT_SUBTYPE,&codec),"Read recovery subtype");
    require(codec==MFVideoFormat_H264,"Recovery changed codec");
    require(MFGetAttributeUINT32(native.Get(),MF_MT_VIDEO_PRIMARIES,0)==MFVideoPrimaries_BT709 &&
        MFGetAttributeUINT32(native.Get(),MF_MT_VIDEO_NOMINAL_RANGE,0)==MFNominalRange_16_235,
        "Recovery changed color metadata");
    ComPtr<IMFMediaType> type; checked(MFCreateMediaType(&type),"Create recovery decode type");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Video type");
    checked(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"NV12 type");
    checked(reader->SetCurrentMediaType(video,nullptr,type.Get()),"Decode recovery H.264");
    std::vector<uint64_t> hashes;
    bool ended=false;
    for (int attempt=0;attempt<expected+100;++attempt) {
        DWORD flags=0; LONGLONG time=0; ComPtr<IMFSample> sample;
        checked(reader->ReadSample(video,0,nullptr,&flags,&time,&sample),"Read recovery frame");
        require(!(flags&MF_SOURCE_READERF_ERROR),"Recovery decoder flag");
        if (sample) {
            const auto index=hashes.size();
            require(index<size_t(expected) && std::llabs(time-int64_t(index)*10000000/fps)<=334,
                "Recovery lost or reordered timestamps");
            ComPtr<IMFMediaType> current; checked(reader->GetCurrentMediaType(video,&current),"Read decoded geometry");
            UINT32 w=0,h=0; checked(MFGetAttributeSize(current.Get(),MF_MT_FRAME_SIZE,&w,&h),"Read size");
            require(w>=width && w<width+64 && h>=height && h<height+64,"Recovery changed geometry");
            ComPtr<IMFMediaBuffer> buffer; checked(sample->ConvertToContiguousBuffer(&buffer),"Read pixels");
            ComPtr<IMF2DBuffer> twoD; BYTE* data=nullptr; LONG stride=LONG(w); DWORD bytes=0;
            if (SUCCEEDED(buffer.As(&twoD))) checked(twoD->Lock2D(&data,&stride),"Lock recovery pixels");
            else checked(buffer->Lock(&data,nullptr,&bytes),"Lock linear recovery pixels");
            bool valid=stride>=width && (twoD || bytes>=DWORD(stride)*h*3/2);
            uint64_t hash=1469598103934665603ull;
            if (valid) {
                for (int plane=0;plane<2;++plane) {
                    const auto* base=data+(plane ? size_t(stride)*h : 0);
                    for (int y=0;y<(plane ? height/2 : height);++y) for (int x=0;x<width;++x) {
                        hash^=base[size_t(y)*stride+x]; hash*=1099511628211ull;
                    }
                }
                valid=std::abs(int(data[60*stride+80])-(index%2 ? 32 : 63))<=12 &&
                    std::abs(int(data[60*stride+240])-173)<=12;
            }
            if (twoD) twoD->Unlock2D(); else buffer->Unlock();
            require(valid,"Recovery changed decoded colors or visible pixels"); hashes.push_back(hash);
        }
        if (flags&MF_SOURCE_READERF_ENDOFSTREAM) { ended=true; break; }
    }
    require(ended && hashes.size()==size_t(expected),"Wrong recovered frame count");
    return hashes;
}
std::vector<uint8_t> read(const std::filesystem::path& path) {
    // The active encoder retains DELETE access to its owned object. A reader
    // must share that access; this does not change the encoder's deny-delete guard.
    const HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"Read owned MP4");
    struct Close { HANDLE value; ~Close(){CloseHandle(value);} } close{file};
    LARGE_INTEGER length{};require(GetFileSizeEx(file,&length) && length.QuadPart>=0 &&
        length.QuadPart<16*1024*1024,"Unexpected fixture size");
    std::vector<uint8_t> data(static_cast<size_t>(length.QuadPart)); DWORD count=0;
    require(data.empty() || (ReadFile(file,data.data(),DWORD(data.size()),&count,nullptr) && count==data.size()),"Read full fixture");
    return data;
}
uint32_t be32(const std::vector<uint8_t>& data, size_t p) {
    require(p+4<=data.size(),"Short box field");
    return uint32_t(data[p])<<24|uint32_t(data[p+1])<<16|uint32_t(data[p+2])<<8|data[p+3];
}
struct Box { size_t start,end,payload; uint32_t type; };
constexpr uint32_t four(char a,char b,char c,char d) { return uint32_t(a)<<24|uint32_t(b)<<16|uint32_t(c)<<8|uint32_t(d); }
bool nextBox(const std::vector<uint8_t>& data,size_t& cursor,size_t limit,Box& box) {
    if (cursor>limit || limit-cursor<8) return false;
    uint64_t length=be32(data,cursor); size_t header=8;
    if (length==1) { if (limit-cursor<16) return false; length=(uint64_t(be32(data,cursor+8))<<32)|be32(data,cursor+12); header=16; }
    if (!length) length=limit-cursor;
    if (length<header || length>limit-cursor) return false;
    box={cursor,cursor+size_t(length),cursor+header,be32(data,cursor+4)}; cursor=box.end; return true;
}
unsigned samples(const std::vector<uint8_t>& data,const Box& parent,int depth=0) {
    require(depth<3,"Unexpected fixture nesting"); size_t at=parent.payload; Box box{}; unsigned total=0;
    while (nextBox(data,at,parent.end,box)) {
        if (box.type==four('t','r','a','f')) total+=samples(data,box,depth+1);
        if (box.type==four('t','r','u','n')) { require(box.end-box.payload>=8,"Short sample table"); total+=be32(data,box.payload+4); }
    }
    require(at==parent.end,"Incomplete owned moof"); return total;
}
struct Fragment { size_t begin,payload,end; unsigned count; };
std::vector<Fragment> fragments(const std::vector<uint8_t>& data) {
    std::vector<Fragment> result; size_t at=0; Box box{},pending{}; bool have=false;
    while (nextBox(data,at,data.size(),box)) {
        if (box.type==four('m','o','o','f')) { pending=box;have=true; }
        else if (have && box.type==four('m','d','a','t')) { result.push_back({pending.start,box.payload,box.end,samples(data,pending)});have=false; }
        else have=false;
    }
    return result;
}
void encodeFile(const std::filesystem::path& path,int count,lapse::EncodingMode mode,bool recovery=true) {
    lapse::Encoder encoder; std::wstring error;
    encoded(encoder.open(path.wstring(),width,height,fps,error,lapse::EncodingQuality::Balanced,mode,recovery),error);
    for (int i=0;i<count;++i) encoded(encoder.write(pattern(i),error),error);
    if (!count) require(!encoder.finish(error) && !error.empty() && !std::filesystem::exists(path),"Empty recovery output leaked");
    else { encoded(encoder.finish(error),error); require(encoder.frames()==uint64_t(count),"Accepted count changed"); }
}
void lifecycle(const std::filesystem::path& directory) {
    for (auto mode:{lapse::EncodingMode::Compatible,lapse::EncodingMode::Efficient,lapse::EncodingMode::QualityH264,lapse::EncodingMode::HardwareH264}) {
        if (mode==lapse::EncodingMode::HardwareH264 && !hardwareAvailable()) {
            std::cout<<"Hardware H.264 absent by independent enumeration; skipped.\n"; continue;
        }
        for (int count:{0,1,2,31}) {
            const auto path=directory/(L"mode-"+std::to_wstring(int(mode))+L"-"+std::to_wstring(count)+L".mp4");
            const auto before=markerCalls; encodeFile(path,count,mode);
            require(markerCalls-before==unsigned(count),"Recovery omitted or duplicated marker");
            if (count) { decode(path,count); require(!fragments(read(path)).empty(),"Recovery did not produce fragments"); }
        }
    }
    require(!flushCalls,"Recovery discarded pending samples through writer Flush");
}
void failurePaths(const std::filesystem::path& directory) {
    std::wstring error; lapse::Encoder encoder; const auto invalid=directory/L"invalid.mp4";
    const unsigned before=sinkCreations;
    for (auto mode:{lapse::EncodingMode::HardwareHEVC,lapse::EncodingMode::SoftwareAV1})
        require(!encoder.open(invalid.wstring(),width,height,fps,error,lapse::EncodingQuality::Balanced,
            mode,true) && error.find(L"H.264")!=std::wstring::npos &&
            !std::filesystem::exists(invalid) && sinkCreations==before,"Invalid codec opened recovery output");
    for (bool* flag:{&failSink,&failWriter}) {
        *flag=true; const bool opened=encoder.open(invalid.wstring(),width,height,fps,error,
            lapse::EncodingQuality::Balanced,lapse::EncodingMode::Efficient,true); *flag=false;
        require(!opened && !std::filesystem::exists(invalid),"Recovery setup failed to clean its owned file");
    }
    const auto path=directory/L"marker-error.mp4",published=directory/L"published.mp4";
    encoded(encoder.open(path.wstring(),width,height,fps,error,lapse::EncodingQuality::Balanced,lapse::EncodingMode::Efficient,true),error);
    require(!MoveFileExW(path.c_str(),published.c_str(),0) && GetLastError()==ERROR_SHARING_VIOLATION,"Recovery weakened file ownership");
    failMarker=true; const bool wrote=encoder.write(pattern(0),error); failMarker=false;
    require(!wrote && encoder.frames()==1 && error.find(L"recovery section")!=std::wstring::npos,"Marker failure lost accepted frame");
    encoded(encoder.finishForPublication(error),error);
    { std::ofstream sentinel(published,std::ios::binary); sentinel<<"existing"; }
    require(encoder.publish(published.wstring())!=ERROR_SUCCESS,"Recovery overwrote existing destination");
    encoder.releasePublication(); require(read(published)==std::vector<uint8_t>({'e','x','i','s','t','i','n','g'}),"Existing destination changed");
    decode(path,1);
    require(!encoder.open(path.wstring(),width,height,fps,error,lapse::EncodingQuality::Balanced,lapse::EncodingMode::Efficient,true),"Recovery overwrote existing recording");
    const auto partial=directory/L"finalize-error.mp4";
    encoded(encoder.open(partial.wstring(),width,height,fps,error,lapse::EncodingQuality::Balanced,lapse::EncodingMode::Efficient,true),error);
    encoded(encoder.write(pattern(0),error),error);
    failFinalize=true; const bool finished=encoder.finish(error); failFinalize=false;
    require(!finished && !error.empty() && std::filesystem::exists(partial),"Finalization failure deleted accepted output");
}
bool engineFailure(const std::filesystem::path& directory,unsigned failureAt,bool beforeAcceptance) {
    const unsigned expected=beforeAcceptance?failureAt-1:failureAt;
    const auto folder=directory/(std::wstring(beforeAcceptance?L"engine-write-":L"engine-marker-")+std::to_wstring(failureAt));
    require(std::filesystem::create_directory(folder),"Create owned engine output folder");
    engineFailMarkerAt=beforeAcceptance?0:failureAt;
    engineFailWriteAt=beforeAcceptance?failureAt:0;
    markerCalls=engineWriteCalls=0;engineCaptures=0;
    lapse::Status saved;
    {
        lapse::Engine engine;lapse::Settings settings;
        settings.monitorId=L"recovery-owned-desktop";settings.width=width;settings.height=height;
        settings.folder=folder.wstring();settings.preview=false;settings.intervalMs=100;
        settings.encodingMode=lapse::EncodingMode::Efficient;settings.recoveryMode=true;
        engine.configure(settings);engine.record();
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(8);
        do {
            saved=engine.status();
            if(saved.state==lapse::State::Idle)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while(std::chrono::steady_clock::now()<until);
        require(saved.state==lapse::State::Idle&&saved.error&&saved.recordingFailed,
                "Engine did not preserve the injected writer failure");
        require(saved.message.find(beforeAcceptance?L"Cannot encode the video frame":L"recovery section")!=std::wstring::npos&&
                saved.savedPaths.size()==1&&saved.savedPaths[0]==saved.savedPath&&
                std::filesystem::path(saved.savedPath).parent_path()==folder&&std::filesystem::exists(saved.savedPath),
                "Engine lost failure context or the actual finalized path");
        const auto decoded=decode(saved.savedPath,int(expected));
        require(decoded.size()==expected&&engineWriteCalls==failureAt&&
                markerCalls==(beforeAcceptance?expected:failureAt),"Injected engine write boundary changed");
        settings.preview=true;engine.configure(settings);
        const auto previewUntil=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        lapse::Status idle;
        do {
            idle=engine.status();if(idle.preview)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while(std::chrono::steady_clock::now()<previewUntil);
        require(idle.preview&&idle.state==lapse::State::Idle&&idle.frames==saved.frames&&
                idle.savedPath==saved.savedPath&&idle.savedPaths==saved.savedPaths&&
                idle.message==saved.message&&idle.error&&idle.recordingFailed,
                "Later idle preview changed the retained recording outcome");
    }
    engineFailMarkerAt=engineFailWriteAt=0;
    const bool correct=saved.frames==expected;
    std::cout<<(correct?"PASS ":"FAIL ")<<"actual Engine "<<(beforeAcceptance?"pre-accept write":"accepted marker")
        <<" failure at "<<failureAt<<": decoded="<<expected<<" reported="<<saved.frames
        <<" error/path/idle retained\n";
    return correct;
}
struct Child {
    HANDLE process=nullptr,thread=nullptr,job=nullptr;
    ~Child() { if (job) CloseHandle(job); if (process) { if (WaitForSingleObject(process,0)==WAIT_TIMEOUT) { TerminateProcess(process,99); WaitForSingleObject(process,5000); } CloseHandle(process); } if (thread) CloseHandle(thread); }
};
int heldChild(const wchar_t* path,const wchar_t* eventName) {
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,eventName); if (!ready) return 5;
    lapse::Encoder encoder; std::wstring error;
    encoded(encoder.open(path,width,height,fps,error,lapse::EncodingQuality::Balanced,lapse::EncodingMode::Efficient,true),error);
    for (int i=0;i<crashFrames;++i) { encoded(encoder.write(pattern(i),error),error); Sleep(10); }
    SetEvent(ready); CloseHandle(ready); Sleep(INFINITE); return 6;
}
void interrupted(const std::filesystem::path& directory,const std::vector<uint64_t>& reference) {
    const auto path=directory/L"terminated.recording.mp4";
    const std::wstring eventName=L"Local\\Timelapse.EncoderRecoveryTest."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str()); require(ready!=nullptr,"Create owned child event");
    Child child; child.job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(child.job && SetInformationJobObject(child.job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Create child cleanup job");
    wchar_t executable[32768]{}; require(GetModuleFileNameW(nullptr,executable,32768)!=0,"Read fixture executable");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --hold \""+path.wstring()+L"\" \""+eventName+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    const BOOL created=CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&process);
    child.process=process.hProcess;child.thread=process.hThread;
    if (!created) { CloseHandle(ready); require(false,"Create owned encoding child"); }
    require(AssignProcessToJobObject(child.job,child.process)!=FALSE,"Contain owned encoding child"); ResumeThread(child.thread);
    const DWORD wait=WaitForSingleObject(ready,15000);CloseHandle(ready);require(wait==WAIT_OBJECT_0,"Child did not submit generated frames");
    // Wait only for observable complete sections, not a claimed flush deadline.
    std::vector<Fragment> completed;const auto began=GetTickCount64();
    do { completed=fragments(read(path)); if (!completed.empty()) break; Sleep(20); } while (GetTickCount64()-began<5000);
    require(!completed.empty(),"No completed recovery section became observable");
    require(TerminateProcess(child.process,73)!=FALSE && WaitForSingleObject(child.process,5000)==WAIT_OBJECT_0,"Terminate only owned child");
    completed=fragments(read(path));unsigned count=0;for(const auto& f:completed)count+=f.count;
    require(count>0 && count<=crashFrames,"Invalid interrupted complete-section count");
    const auto actual=decode(path,int(count));
    require(std::equal(actual.begin(),actual.end(),reference.begin()),"Interrupted frames differ from ordinary reference");
}
void tails(const std::filesystem::path& directory,const std::vector<uint64_t>& reference) {
    const auto path=directory/L"tail-source.mp4";encodeFile(path,crashFrames,lapse::EncodingMode::Efficient);
    require(decode(path,crashFrames)==reference,"Finalized recovery changed ordinary decoded pixels");
    const auto bytes=read(path);const auto parts=fragments(bytes);require(parts.size()>=3,"Too few recovery sections");
    unsigned total=0;for(const auto& f:parts)total+=f.count;require(total==crashFrames,"Fragment sample count changed");
    const auto& last=parts.back();const int prefix=int(total-last.count);
    const std::array<std::pair<size_t,int>,4> cuts{{{parts.front().end,int(parts.front().count)},
        {last.begin+4,prefix},{last.payload+(last.end-last.payload)/2,prefix},{last.end,crashFrames}}};
    for(size_t i=0;i<cuts.size();++i) {
        const auto tail=directory/(L"tail-"+std::to_wstring(i)+L".mp4");
        {std::ofstream out(tail,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(cuts[i].first));require(bool(out),"Write owned interrupted prefix");}
        const auto actual=decode(tail,cuts[i].second);
        require(std::equal(actual.begin(),actual.end(),reference.begin()),"Truncated tail altered completed pixels");
    }
}
}
HRESULT WINAPI recoveryCreateSink(IMFByteStream* bytes,IMFMediaType* videoType,IMFMediaType* audio,IMFMediaSink** sink) {
    ++sinkCreations;if(failSink){*sink=nullptr;return E_FAIL;}return MFCreateFMPEG4MediaSink(bytes,videoType,audio,sink);
}
HRESULT WINAPI recoveryCreateWriter(IMFMediaSink* sink,IMFAttributes* attributes,IMFSinkWriter** writer) {
    if(failWriter){*writer=nullptr;return E_FAIL;}
    ComPtr<IMFSinkWriter> real;const auto hr=MFCreateSinkWriterFromMediaSink(sink,attributes,&real);
    if(FAILED(hr))return hr;*writer=new Writer(real.Get());return S_OK;
}
namespace lapse {
struct CameraClient::Impl {};
CameraClient::CameraClient():impl_(std::make_unique<Impl>()){}
CameraClient::~CameraClient()=default;
bool CameraClient::start(const std::wstring&,std::wstring&,CameraResolution){throw std::runtime_error("Unexpected camera activation");}
void CameraClient::stop(){}
bool CameraClient::latest(Frame&,std::wstring&){throw std::runtime_error("Unexpected camera read");}
bool CameraClient::beginNight(uint64_t,uint32_t,const NightSettings&,std::wstring&){throw std::runtime_error("Unexpected Night request");}
bool CameraClient::nightResult(uint64_t,Frame&,NightWindowResult&,std::wstring&){throw std::runtime_error("Unexpected Night result");}
void CameraClient::cancelNight()noexcept{}
bool CameraClient::observeActivity(uint64_t,CameraObservation&,std::wstring&){throw std::runtime_error("Unexpected activity observation");}
void CameraClient::cancelActivityObservation()noexcept{}
bool CameraClient::personInput(uint64_t,CameraPersonInput&,std::wstring&,bool){throw std::runtime_error("Unexpected person input");}
bool captureMonitor(const std::wstring& id,int,int,bool,Frame& output,std::wstring& error){
    if(id!=L"recovery-owned-desktop"){error=L"Synthetic source not configured.";return false;}
    error.clear();output=pattern(int(engineCaptures++));return true;
}
void releaseDesktopCaptureCache()noexcept{}
}
int wmain(int argc,wchar_t** argv) {
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 1;
    if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();return 1;}
    int result=0;
    const auto directory=std::filesystem::current_path()/(L"encoder-recovery-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try {
        if(argc==4 && std::wstring(argv[1])==L"--hold") return heldChild(argv[2],argv[3]);
        require(argc==1,"Unexpected recovery fixture arguments");
        require(std::filesystem::create_directory(directory),"Create owned recovery directory");
        lifecycle(directory); failurePaths(directory);
        const bool first=engineFailure(directory,1,false);
        const bool later=engineFailure(directory,3,false);
        const bool rejected=engineFailure(directory,2,true);
        require(first&&later&&rejected,"Engine terminal frame count differs from real accepted samples");
        const auto ordinary=directory/L"ordinary.mp4"; const auto markers=markerCalls;
        encodeFile(ordinary,crashFrames,lapse::EncodingMode::Efficient,false);
        require(markerCalls==markers && fragments(read(ordinary)).empty(),"Off changed ordinary container path");
        const auto reference=decode(ordinary,crashFrames);
        interrupted(directory,reference);tails(directory,reference);
        require(!flushCalls,"Recovery invoked destructive writer Flush");
        std::filesystem::remove_all(directory);
        std::cout<<"Recovery modes, lifecycle, failures, owned termination and damaged-tail pixel/timestamp checks passed.\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\nArtifacts kept at "<<directory.string()<<'\n';result=1;}
    MFShutdown();CoUninitialize();return result;
}

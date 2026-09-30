// Controlled capture sources with the real encoder and filesystem. A small
// existing destination forces a real rename failure without filling a disk or
// changing permissions, and the retained movie must still decode completely.
#include "engine.h"
#include "encoder.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <new>
#include <cstdlib>
#include <utility>

namespace {
std::atomic<int> candidateGatePhase{0},candidateGateCalls{0};
std::atomic<bool> candidateGateReached{false},candidateGateReleased{false},candidateGateTimeout{false};
std::atomic<bool> publicationDiagnosticFault{false};
void publicationCandidateGate(int phase) {
    int expected=phase;
    if(!candidateGatePhase.compare_exchange_strong(expected,0))return;
    ++candidateGateCalls;candidateGateReached=true;
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(!candidateGateReleased) {
        if(std::chrono::steady_clock::now()>=until){candidateGateTimeout=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

namespace {
enum class SaveFault { None, PathPreparation, MessagePreparation, AfterRename };
std::atomic<SaveFault> saveFault{SaveFault::None};
thread_local int allocationCountdown=0;
std::atomic<int> saveAllocationFailures{0}, saveMoves{0}, savedMoves{0};
std::atomic<bool> afterRenamePending{false}, commitAcknowledged{false}, allocationFreeCommit{false};
}
void* operator new(std::size_t size) {
    if(allocationCountdown>0 && --allocationCountdown==0) { ++saveAllocationFailures; throw std::bad_alloc(); }
    if(void* p=std::malloc(size?size:1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
namespace lapse {
class StorageEncoder {
public:
    bool open(const std::wstring& path,int width,int height,int fps,std::wstring& error,EncodingQuality quality,EncodingMode mode, bool recoveryMode) {
        return real_.open(path,width,height,fps,error,quality,mode, recoveryMode);
    }
    bool write(const Frame& frame,std::wstring& error) { return real_.write(frame,error); }
    bool finish(std::wstring& error) {
        const bool result=real_.finish(error);
        if(result) {
            const auto fault=saveFault.load();
            if(fault==SaveFault::PathPreparation || fault==SaveFault::MessagePreparation) {
                saveFault=SaveFault::None;
                allocationCountdown=fault==SaveFault::PathPreparation?1:2;
            }
        }
        return result;
    }
    bool finishForPublication(std::wstring& error) {
        const bool result=real_.finishForPublication(error);
        if(result) {
            const auto fault=saveFault.load();
            if(fault==SaveFault::PathPreparation || fault==SaveFault::MessagePreparation) {
                saveFault=SaveFault::None;
                allocationCountdown=fault==SaveFault::PathPreparation?1:2;
            }
        }
        return result;
    }
    DWORD publish(const std::wstring& destination) {
        publicationCandidateGate(1);
        ++saveMoves;
        const DWORD result=real_.publish(destination);
        if(result==ERROR_SUCCESS) {
            ++savedMoves;
            if(saveFault==SaveFault::AfterRename) {
                saveFault=SaveFault::None;afterRenamePending=true;allocationCountdown=1;
            }
        }
        publicationCandidateGate(2);
        if(result!=ERROR_SUCCESS && publicationDiagnosticFault.exchange(false)) {
            allocationCountdown=1;
        }
        return result;
    }
    void releasePublication() noexcept { real_.releasePublication(); }
    uint64_t frames() const { return real_.frames(); }
private:
    Encoder real_;
};
}
EXECUTION_STATE WINAPI storageExecutionState(EXECUTION_STATE flags) {
    // Acknowledge closeRecording's final, nonallocating commit. No system power
    // request is changed by this isolated fixture.
    if(flags==ES_CONTINUOUS && afterRenamePending.exchange(false)) {
        allocationFreeCommit=allocationCountdown==1;
        allocationCountdown=0;
        commitAcknowledged=true;
    }
    return ES_CONTINUOUS;
}
#define Encoder StorageEncoder
#define SetThreadExecutionState storageExecutionState
#include "../src/engine.cpp"
#undef SetThreadExecutionState
#undef Encoder

namespace {
using Microsoft::WRL::ComPtr;
std::atomic<int> cameraPolls{0};
constexpr char sentinel[] = "An existing video must never be replaced.";

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<class Predicate>
lapse::Status await(lapse::Engine& engine, Predicate predicate, int timeoutMs = 7000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        auto status = engine.status();
        if (predicate(status)) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    std::wcerr << L"Last engine status: " << engine.status().message << L'\n';
    throw std::runtime_error("Timed out waiting for storage recovery");
}

void checked(HRESULT result, const char* reason) {
    if (FAILED(result)) {
        std::wcerr << lapse::errorText(result) << L'\n';
        throw std::runtime_error(reason);
    }
}

std::filesystem::path recordingPath(const std::filesystem::path& directory) {
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().wstring();
        if (name.find(L".recording.mp4") == std::wstring::npos) continue;
        require(found.empty(), "More than one pending movie before collision test");
        found = entry.path();
    }
    require(!found.empty(), "The active recording has no temporary movie");
    return found;
}

void writeSentinel(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Could not create rename collision fixture");
    DWORD written = 0;
    const bool ok = WriteFile(file, sentinel, sizeof(sentinel) - 1, &written, nullptr) != FALSE;
    CloseHandle(file);
    require(ok && written == sizeof(sentinel) - 1, "Could not write rename collision fixture");
}

void verifySentinel(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "The existing destination disappeared");
    const std::string actual((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(actual == sentinel, "A recording overwrote the existing destination");
}

void verifyVideo(const std::filesystem::path& path, uint64_t expectedFrames) {
    require(expectedFrames > 0, "Cannot verify an empty recorded movie");
    require(std::filesystem::file_size(path) > 0, "Recorded movie is empty");
    // The URL resolver does not accept every extended file path. Open the
    // retained movie itself as a byte stream, without moving or copying it.
    ComPtr<IMFByteStream> file;
    checked(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                        MF_FILEFLAGS_NONE, lapse::fileIOPath(path.wstring()).c_str(), &file),
            "Could not open retained MP4 file");
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromByteStream(file.Get(), nullptr, &reader), "Could not read retained MP4");
    constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE),
            "Could not deselect source streams");
    checked(reader->SetStreamSelection(videoStream, TRUE), "Could not select recorded video");
    ComPtr<IMFMediaType> decodedType;
    checked(MFCreateMediaType(&decodedType), "Could not create decoded video type");
    checked(decodedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Could not configure decoder");
    checked(decodedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Could not request decoded pixels");
    checked(reader->SetCurrentMediaType(videoStream, nullptr, decodedType.Get()),
            "Retained movie has no decodable video stream");
    uint64_t frames = 0;
    bool ended = false;
    for (uint64_t attempt = 0; attempt < expectedFrames + 100; ++attempt) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        checked(reader->ReadSample(videoStream, 0, nullptr, &flags, nullptr, &sample),
                "Retained movie contains an unreadable frame");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Retained movie reported a decode error");
        if (sample) {
            ComPtr<IMFMediaBuffer> pixels;
            checked(sample->ConvertToContiguousBuffer(&pixels), "Decoded frame has no pixels");
            DWORD bytes = 0;
            checked(pixels->GetCurrentLength(&bytes), "Could not read decoded frame size");
            require(bytes >= 320 * 180 * 3 / 2, "Decoded movie frame is incomplete");
            ++frames;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    require(ended && frames == expectedFrames, "Retained movie lost recorded frames");
}
}

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
CameraClient::CameraClient() : impl_(std::make_unique<Impl>()) {}
CameraClient::~CameraClient() = default;
bool CameraClient::start(const std::wstring&, std::wstring& error, CameraResolution) {
    error.clear(); return true;
}
void CameraClient::stop() {}
bool CameraClient::latest(Frame&, std::wstring& error) {
    // Never deliver the first camera frame so cancellation is deterministic.
    error.clear(); ++cameraPolls; return false;
}
bool captureMonitor(const std::wstring& id, int width, int height, bool, Frame& output, std::wstring& error) {
    if (id != L"synthetic-display") { error = L"Unknown synthetic display."; return false; }
    error.clear();
    output = {width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4, 96)};
    return true;
}
}

void publicationCase(const std::filesystem::path& directory,SaveFault fault) {
    saveFault=SaveFault::None; saveAllocationFailures=saveMoves=savedMoves=0;
    afterRenamePending=commitAcknowledged=allocationFreeCommit=false;
    lapse::Engine engine;
    lapse::Settings settings; settings.monitorId = L"synthetic-display";
    settings.width=320; settings.height=180; settings.intervalMs = 60000;
    settings.folder=directory.wstring(); settings.preview=false;
    engine.configure(settings); engine.record();
    await(engine,[](const auto& s){return s.state==lapse::State::Recording&&s.frames==1;});
    const auto temporary=recordingPath(directory);
    auto name=temporary.wstring();
    name.replace(name.rfind(L".recording.mp4"),14,L".mp4");
    const std::filesystem::path finalPath=name;
    saveFault=fault;
    engine.finish();
    const auto completed=await(engine,[&](const auto& s){
        return s.state==lapse::State::Idle &&
            (fault==SaveFault::AfterRename ? commitAcknowledged.load() : saveAllocationFailures.load()==1 && s.error);
    });
    std::cout<<"fault="<<int(fault)<<" allocations_failed="<<saveAllocationFailures<<" moves="<<saveMoves
        <<" saved_moves="<<savedMoves<<" commit_ack="<<commitAcknowledged<<" noalloc_commit="<<allocationFreeCommit
        <<" correct_path="<<(completed.savedPath==finalPath.wstring())<<'\n';
    require(saveMoves==1&&savedMoves==1,"Save retry repeated an already committed rename");
    require(completed.savedPath==finalPath.wstring()&&std::filesystem::is_regular_file(finalPath)&&!std::filesystem::exists(temporary),
            "Save allocation failure advertised an absent or incorrect file");
    require(completed.recordingFailed==(fault!=SaveFault::AfterRename),
            "Recording outcome did not distinguish recovered resource failure from successful publication");
    if(fault==SaveFault::AfterRename)
        require(saveAllocationFailures==0&&allocationFreeCommit&&!completed.error,"Successful rename publication still allocated");
    else {
        require(saveAllocationFailures==1&&completed.error,"Preparation allocation failure was not reported exactly once");
        require(completed.message.find(L"Captured frames were saved.")!=std::wstring::npos,"Recovered save hid its completed movie");
    }
    verifyVideo(finalPath,completed.frames);
    engine.record();
    require(!engine.status().recordingFailed,"Accepted retry retained the prior recording failure");
    await(engine,[](const auto& s){return s.state==lapse::State::Recording&&s.frames==1;});
    engine.finish();
    const auto recovered=await(engine,[](const auto& s){return s.state==lapse::State::Idle;});
    require(!recovered.error&&!recovered.recordingFailed&&!recovered.savedPath.empty()&&recovered.savedPath!=finalPath.wstring(),"Encoder session did not recover");
    verifyVideo(recovered.savedPath,recovered.frames);
    verifyVideo(finalPath,completed.frames);
}
void activeOutputOwnership(const std::filesystem::path& directory) {
    const auto folder = directory / L"active-output-ownership";
    lapse::Settings settings; settings.monitorId = L"synthetic-display";
    settings.width = 320; settings.height = 180; settings.intervalMs = 60000;
    settings.folder = folder.wstring(); settings.preview = false;
    lapse::Engine engine;
    engine.configure(settings); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    const auto temporary = recordingPath(folder), displaced = folder / L"displaced.mp4";
    auto identity = [](const std::filesystem::path& path) {
        HANDLE file = CreateFileW(lapse::fileIOPath(path.wstring()).c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(file != INVALID_HANDLE_VALUE, "Could not inspect actual worker output identity");
        BY_HANDLE_FILE_INFORMATION info{};
        const BOOL queried = GetFileInformationByHandle(file, &info);
        CloseHandle(file);
        require(queried != FALSE, "Could not query actual worker output identity");
        return info;
    };
    const auto original = identity(temporary);
    require(!MoveFileExW(temporary.c_str(), displaced.c_str(), MOVEFILE_WRITE_THROUGH) && GetLastError() == ERROR_SHARING_VIOLATION,
            "Active worker output can be renamed and replaced before publication");
    require(!DeleteFileW(temporary.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION,
            "Active worker output can be deleted before publication");
    engine.finish();
    const auto saved = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(!saved.error && !saved.recordingFailed && saved.frames == 1 && !saved.savedPath.empty() &&
            !std::filesystem::exists(temporary) && !std::filesystem::exists(displaced),
            "Protected worker output was not published normally");
    const auto completed = identity(saved.savedPath);
    require(original.dwVolumeSerialNumber == completed.dwVolumeSerialNumber && original.nFileIndexHigh == completed.nFileIndexHigh &&
            original.nFileIndexLow == completed.nFileIndexLow, "Worker saved a different file than its encoder owned");
    verifyVideo(saved.savedPath, 1);
    std::cout << "Worker ownership: active rename/delete blocked, original identity published, decoded=1.\n";
}

void temporarySuffixBoundary(const std::filesystem::path& directory) {
    const std::wstring name = L"Timelapse-20260929-120000-123-" + std::to_wstring(GetCurrentProcessId());
    const size_t folderLength = MAX_PATH - 2 - 1 - name.size() - 4;
    const auto prefix = (directory / L"save \u65e5 ").wstring();
    require(prefix.size() < folderLength, "Storage fixture root is too long for the path boundary");
    const std::filesystem::path folder(prefix + std::wstring(folderLength - prefix.size(), L'x'));
    require(!std::filesystem::exists(folder), "Boundary folder must be created by the worker");
    lapse::Settings settings; settings.monitorId = L"synthetic-display";
    settings.width = 320; settings.height = 180; settings.intervalMs = 60000;
    settings.folder = folder.wstring(); settings.preview = false;
    lapse::Engine engine;
    engine.configure(settings); engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    const auto temporary = recordingPath(folder);
    const auto temporaryName = temporary.wstring();
    const auto suffix = temporaryName.rfind(L".recording.mp4");
    require(suffix != std::wstring::npos, "Unexpected boundary temporary filename");
    const std::filesystem::path destination(temporaryName.substr(0, suffix) + L".mp4");
    require(folder.wstring().size() < MAX_PATH && destination.wstring().size() == MAX_PATH - 2 &&
            temporaryName.size() >= MAX_PATH, "Actual worker filename did not cross the suffix boundary");
    writeSentinel(destination);
    engine.finish();
    const auto collision = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(collision.error && collision.recordingFailed && collision.frames == 1 && collision.savedPath == temporaryName &&
            collision.savedPath.rfind(L"\\\\?\\", 0) != 0 && collision.message.find(temporaryName) != std::wstring::npos,
            "Boundary collision lost its recording failure or user-visible retained path");
    verifySentinel(destination);
    verifyVideo(lapse::fileIOPath(collision.savedPath), 1);
    require(DeleteFileW(lapse::fileIOPath(temporaryName).c_str()) != FALSE,
            "Could not retire the owned boundary collision movie");
    engine.record();
    await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames == 1; });
    const auto retryTemporary = recordingPath(folder);
    engine.finish();
    const auto completed = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
    require(!completed.error && !completed.recordingFailed && completed.frames == 1 &&
            completed.savedPath.size() == MAX_PATH - 2 && completed.savedPath.rfind(L"\\\\?\\", 0) != 0 &&
            std::filesystem::path(completed.savedPath).parent_path() == folder &&
            completed.savedPath != destination.wstring(),
            "Boundary recording did not publish a successful friendly final path");
    require(GetFileAttributesW(lapse::fileIOPath(retryTemporary.wstring()).c_str()) == INVALID_FILE_ATTRIBUTES,
            "Boundary publication left its temporary file");
    verifyVideo(completed.savedPath, 1);
    verifySentinel(destination);
    std::cout << "Worker suffix boundary: folder=" << folder.wstring().size()
              << " final=" << completed.savedPath.size() << " temporary=" << temporaryName.size()
              << " decoded=2 collision_preserved=1 friendly_paths=1\n";
}

namespace {
struct Identity {
    DWORD volume=0,high=0,low=0;
    bool operator==(const Identity& other)const{return volume==other.volume&&high==other.high&&low==other.low;}
};
Identity identity(const std::filesystem::path& path) {
    HANDLE file=CreateFileW(lapse::fileIOPath(path.wstring()).c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"Cannot open owned file identity");
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok=GetFileInformationByHandle(file,&info);CloseHandle(file);
    require(ok!=FALSE,"Cannot inspect owned file identity");
    return {info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow};
}
struct ReleaseGate {~ReleaseGate(){candidateGateReleased=true;candidateGatePhase=0;publicationDiagnosticFault=false;allocationCountdown=0;}};
void requireBlocked(const std::filesystem::path& from,const std::filesystem::path& to) {
    require(!MoveFileExW(lapse::fileIOPath(from.wstring()).c_str(),lapse::fileIOPath(to.wstring()).c_str(),0)&&
        GetLastError()==ERROR_SHARING_VIOLATION,"Original object lost its rename protection");
}
void workerCase(const std::filesystem::path& root,int kind) {
    const auto folder=root/(L"worker-"+std::to_wstring(kind));
    candidateGateReached=candidateGateReleased=candidateGateTimeout=false;candidateGateCalls=0;
    saveFault=SaveFault::None;saveAllocationFailures=saveMoves=savedMoves=0;
    afterRenamePending=commitAcknowledged=allocationFreeCommit=false;
    const bool afterRename=kind==2,collision=kind>=3;
    std::filesystem::path temporary,finalPath;Identity original;
    {
        lapse::Engine engine;ReleaseGate release;
        lapse::Settings settings;settings.monitorId=L"synthetic-display";settings.width=320;settings.height=180;
        settings.intervalMs = 60000;settings.preview=false;settings.folder=folder.wstring();
        engine.configure(settings);engine.record();
        await(engine,[](const auto& s){return s.state==lapse::State::Recording&&s.frames==1;});
        temporary=recordingPath(folder);original=identity(temporary);
        auto name=temporary.wstring();name.replace(name.rfind(L".recording.mp4"),14,L".mp4");finalPath=name;
        const auto displaced=folder/L"displaced.mp4";
        requireBlocked(temporary,displaced);
        if(collision)writeSentinel(finalPath);
        if(kind==4)publicationDiagnosticFault=true;
        candidateGatePhase=afterRename?2:1;engine.finish();
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!candidateGateReached&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(candidateGateReached&&!candidateGateTimeout,"Worker missed retained publication gate");
        require(engine.status().state==lapse::State::Finishing,"Worker gate missed Finishing state");
        const auto owned=afterRename?finalPath:temporary;
        require(identity(owned)==original,"Gate changed original identity");verifyVideo(owned,1);
        requireBlocked(owned,displaced);
        if(kind==1) {
            HANDLE replacement=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            const DWORD error=GetLastError();if(replacement!=INVALID_HANDLE_VALUE)CloseHandle(replacement);
            require(replacement==INVALID_HANDLE_VALUE&&(error==ERROR_FILE_EXISTS||error==ERROR_ALREADY_EXISTS),
                "Competing replacement unexpectedly acquired the protected name");
        }
        if(afterRename)writeSentinel(temporary);
        candidateGateReleased=true;
        const auto saved=await(engine,[](const auto& s){return s.state==lapse::State::Idle;});
        require(candidateGateCalls==1&&!candidateGateTimeout,"Publication gate was repeated or timed out");
        require(saved.frames==1&&saved.error==collision&&saved.recordingFailed==collision,"Wrong recording outcome");
        require(saved.savedPath==(collision?temporary:finalPath).wstring(),"Wrong saved/retained path");
        require(identity(saved.savedPath)==original,"Advertised output is not originally owned movie");verifyVideo(saved.savedPath,1);
        if(collision)verifySentinel(finalPath);
        if(afterRename)verifySentinel(temporary);
        if(kind==4)require(saveAllocationFailures==1&&saveMoves==2&&savedMoves==0,"Diagnostic retry missed cached publication boundary");
        else require(saveAllocationFailures==0&&saveMoves==1&&savedMoves==(collision?0:1),"Unexpected publication count");
        require(MoveFileExW(saved.savedPath.c_str(),displaced.c_str(),0)!=FALSE,"Committed result retained its ownership guard");
        require(identity(displaced)==original,"Postcommit original object was not retained");verifyVideo(displaced,1);
        std::cout<<"PASS worker kind="<<kind<<" original_identity=1 decoded=2 guard_released=1 allocations="
            <<saveAllocationFailures<<" publish_calls="<<saveMoves<<'\n';
    }
    if(afterRename)verifySentinel(temporary);
    if(collision)verifySentinel(finalPath);
}
void apiCases(const std::filesystem::path& root) {
    const auto folder=root/L"api \u65e5";require(std::filesystem::create_directory(folder),"API folder create failed");
    const auto path=folder/L"temporary.mp4",finalPath=folder/L"final.mp4",moved=folder/L"moved.mp4";
    lapse::Encoder encoder;std::wstring error;
    require(encoder.publish(finalPath.wstring())==ERROR_INVALID_STATE,"Never-open publish was accepted");
    require(encoder.open(path.wstring(),320,180,30,error),"API encoder open failed");
    require(encoder.publish(finalPath.wstring())==ERROR_INVALID_STATE,"Active writer publish was accepted");
    lapse::Frame frame{320,180,std::vector<uint8_t>(320*180*4,96)};
    require(encoder.write(frame,error)&&encoder.finishForPublication(error),"API retained finalization failed");
    const auto original=identity(path);requireBlocked(path,moved);
    require(!encoder.open((folder/L"unexpected.mp4").wstring(),320,180,30,error),"Open discarded a retained publication guard");
    require(!std::filesystem::exists(folder/L"unexpected.mp4"),"Rejected open created output");
    const std::wstring publicationName=finalPath.wstring();
    bool caught=false;allocationCountdown=1;
    try{(void)encoder.publish(publicationName);}catch(const std::bad_alloc&){caught=true;}
    allocationCountdown=0;require(caught,"Publish preparation allocation did not fail");requireBlocked(path,moved);
    require(!std::filesystem::exists(finalPath)&&identity(path)==original,"Preparation failure changed original object");
    require(encoder.publish(finalPath.wstring())==ERROR_SUCCESS,"Publication did not recover after allocation");
    requireBlocked(finalPath,moved);writeSentinel(path);
    require(encoder.publish((folder/L"must-not-retarget.mp4").wstring())==ERROR_SUCCESS,"Repeated publication lost terminal success");
    require(!std::filesystem::exists(folder/L"must-not-retarget.mp4")&&identity(finalPath)==original,"Repeated publication moved original again");
    encoder.releasePublication();verifySentinel(path);verifyVideo(finalPath,1);
    require(MoveFileExW(finalPath.c_str(),moved.c_str(),0)!=FALSE,"Release retained guard");
    require(encoder.finish(error),"Ordinary finish lost cached successful finalization");verifySentinel(path);
    const auto collisionPath=folder/L"collision-temporary.mp4",collisionFinal=folder/L"collision.mp4";
    require(encoder.open(collisionPath.wstring(),320,180,30,error)&&encoder.write(frame,error)&&encoder.finishForPublication(error),"Collision API open failed");
    writeSentinel(collisionFinal);const DWORD collision=encoder.publish(collisionFinal.wstring());
    require(collision!=ERROR_SUCCESS,"Collision overwrote sentinel");verifySentinel(collisionFinal);
    requireBlocked(collisionPath,folder/L"stolen.mp4");
    require(DeleteFileW(collisionFinal.c_str())!=FALSE,"Cannot retire owned sentinel for cached-result check");
    require(encoder.publish(collisionFinal.wstring())==collision&&!std::filesystem::exists(collisionFinal),"Repeated failed publication retried rename");
    require(encoder.finish(error),"Plain finish did not release retained collision");
    require(MoveFileExW(collisionPath.c_str(),collisionFinal.c_str(),0)!=FALSE,"Plain finish kept guard");verifyVideo(collisionFinal,1);
    const auto ordinary=folder/L"ordinary.mp4";
    require(encoder.open(ordinary.wstring(),320,180,30,error)&&encoder.write(frame,error)&&encoder.finish(error),"Standalone finish failed");
    require(encoder.publish((folder/L"invalid-after-plain.mp4").wstring())==ERROR_INVALID_STATE,"Ordinary finish allowed unowned publication");
    const auto empty=folder/L"empty.mp4";require(encoder.open(empty.wstring(),320,180,30,error),"Empty open failed");
    require(!encoder.finishForPublication(error)&&!std::filesystem::exists(empty)&&encoder.publish(finalPath.wstring())==ERROR_INVALID_STATE,
        "Empty retained finish failed its cleanup/invalid-state contract");
    std::cout<<"PASS API invalid states, retained-open refusal, preparation allocation recovery, Unicode, cached success/failure, ordinary finish, empty cleanup.\n";
}
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    int result = 0;
    const auto directory = std::filesystem::current_path() /
        (L"engine-storage-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        require(std::filesystem::create_directory(directory), "Could not create isolated storage test directory");
        {
            lapse::Settings settings; settings.monitorId = L"synthetic-display";
            settings.layers = lapse::preset(lapse::Mode::Desktop);
            settings.width = 320; settings.height = 180; settings.intervalMs = 60000;
            settings.folder = directory.wstring(); settings.preview = false;
            lapse::Engine engine;
            engine.configure(settings); engine.record();
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
            engine.pause();
            const auto paused = await(engine, [](const auto& s) { return s.state == lapse::State::Paused; });
            const auto temporary = recordingPath(directory);
            const auto temporaryName = temporary.wstring();
            const auto suffix = temporaryName.rfind(L".recording.mp4");
            require(suffix != std::wstring::npos, "Unexpected temporary filename");
            const std::filesystem::path destination(temporaryName.substr(0, suffix) + L".mp4");
            writeSentinel(destination);

            engine.finish();
            const auto failure = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
            require(failure.error && failure.recordingFailed, "A real destination collision was reported as success");
            require(failure.savedPath == temporary.wstring(), "Rename failure did not identify the retained playable movie");
            require(failure.message.find(temporary.wstring()) != std::wstring::npos,
                    "Rename failure did not tell the user where the movie was retained");
            require(failure.message.find(L"Partial file:") == std::wstring::npos,
                    "A finalized movie was incorrectly described as a partial file");
            verifySentinel(destination);
            verifyVideo(temporary, paused.frames);

            settings.preview = true; engine.configure(settings);
            await(engine, [&](const auto& s) { return s.preview && s.preview != failure.preview; });
            const auto preview = engine.status();
            require(preview.error && preview.recordingFailed && preview.message == failure.message && preview.savedPath == failure.savedPath,
                    "Successful idle preview erased the save error or retained path");
            engine.refreshSources();
            const auto refreshed = engine.status();
            require(refreshed.recordingFailed && !refreshed.error && refreshed.savedPath == failure.savedPath,
                    "Refreshing sources changed the previous recording outcome or retained path");

            settings.preview = false; engine.configure(settings); engine.record();
            require(!engine.status().recordingFailed, "Accepted retry retained the failed rename outcome");
            require(engine.status().savedPath.empty(), "A new recording retained the previous movie's saved path");
            await(engine, [](const auto& s) { return s.state == lapse::State::Recording && s.frames >= 1; });
            engine.finish();
            const auto recovered = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
            require(!recovered.error && !recovered.recordingFailed && !recovered.savedPath.empty(), "Recording did not recover after rename failure");
            require(recovered.savedPath != temporary.wstring() && recovered.savedPath != destination.wstring(),
                    "Recovered recording reused a preserved movie's path");
            verifyVideo(recovered.savedPath, recovered.frames);
            verifyVideo(temporary, paused.frames);
            verifySentinel(destination);

            settings.layers = lapse::preset(lapse::Mode::Camera);
            settings.cameraId = L"controlled-warmup-camera";
            engine.configure(settings); engine.record();
            await(engine, [](const auto& s) { return s.state == lapse::State::Starting && cameraPolls > 0; });
            engine.finish();
            const auto cancelled = await(engine, [](const auto& s) { return s.state == lapse::State::Idle; });
            require(!cancelled.error && !cancelled.recordingFailed && cancelled.frames == 0 && cancelled.savedPath.empty(),
                    "Cancelling before the first frame reported a prior movie or a save error");
            require(recordingPath(directory) == temporary, "Cancellation left an empty temporary movie");

            settings.layers = lapse::preset(lapse::Mode::Desktop);
            settings.folder = destination.wstring(); // A file cannot be a save directory.
            engine.configure(settings); engine.record();
            const auto rejected = await(engine, [](const auto& s) { return s.state == lapse::State::Idle && s.error; });
            require(rejected.recordingFailed, "Rejected save directory did not retain a recording failure");
            require(rejected.frames == 0 && rejected.savedPath.empty(), "A rejected save directory reported a stale movie");
            require(recordingPath(directory) == temporary, "Failed recording start left an empty temporary movie");
            verifySentinel(destination);
        }
        for (auto fault : {SaveFault::PathPreparation, SaveFault::MessagePreparation, SaveFault::AfterRename}) {
            const auto owned = directory / (L"publication-" + std::to_wstring(int(fault)));
            require(std::filesystem::create_directory(owned), "Could not create publication test directory");
            publicationCase(owned, fault);
        }
        temporarySuffixBoundary(directory);
        activeOutputOwnership(directory);
        for (int kind=0;kind<5;++kind) workerCase(directory,kind);
        apiCases(directory);
        std::filesystem::remove_all(directory);
        std::cout << "Engine storage: real rename collision, retained MP4 decoding, protected destination, sticky error, retry, cancellation and allocation-safe publication passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Artifacts kept at " << directory.wstring() << L'\n';
        result = 1;
    }
    MFShutdown(); CoUninitialize();
    return result;
}

#include "engine_person_camera_stub.h"

// This fixture owns no native desktop capture surface.
namespace lapse { void releaseDesktopCaptureCache() noexcept {} }

// Actual worker and encoder with generated frames and intercepted power APIs.
// Gates exercise paused saving and a real post-Pause allocation failure.
#include "engine.h"
#include "capture.h"
#include "engine_power_hooks.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;
namespace {
enum class SaveFault { None, Preparation, FailureResult, Persistent };
constexpr EXECUTION_STATE desktopPower=ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED;
constexpr EXECUTION_STATE savingPower=ES_CONTINUOUS|ES_SYSTEM_REQUIRED;
thread_local bool worker=false, allFail=false;
thread_local int countdown=0;
std::atomic<int> layerMatches{0}, allocationsFailed{0}, finishes{0};
std::atomic<bool> blockFinish{false}, blockDesktop{false}, powerOverflow{false};
std::atomic<SaveFault> saveFault{SaveFault::None};
std::atomic<EXECUTION_STATE> currentPower{ES_CONTINUOUS};
struct Call { EXECUTION_STATE flags; DWORD thread; };
std::array<Call,64> powerCalls{};
std::array<EXECUTION_STATE,16> finishPower{};
std::mutex logMutex;
size_t callCount=0;
DWORD ownerThread=0;
HANDLE finishEntered, finishRelease, desktopEntered, desktopRelease, faultEntered, faultRelease, completed;
void require(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
void checked(HRESULT hr,const char* text) { require(SUCCEEDED(hr),text); }
void gate(HANDLE entered,HANDLE release) {
    SetEvent(entered);
    if(WaitForSingleObject(release,5000)!=WAIT_OBJECT_0) ExitProcess(78);
}
void awaitEvent(HANDLE event) { require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"Gate was not reached"); }
void releaseAll() { SetEvent(finishRelease); SetEvent(desktopRelease); SetEvent(faultRelease); }
struct ReleaseGates { ~ReleaseGates(){releaseAll();} };
void reset() {
    blockFinish=blockDesktop=powerOverflow=false; saveFault=SaveFault::None;
    layerMatches=allocationsFailed=finishes=0; currentPower=ES_CONTINUOUS;
    { std::lock_guard<std::mutex> lock(logMutex); callCount=0; powerCalls={}; finishPower={}; }
    for(HANDLE handle:{finishEntered,finishRelease,desktopEntered,desktopRelease,faultEntered,faultRelease}) ResetEvent(handle);
}
void allocation(size_t bytes) {
    if(!worker) return;
    bool fail=allFail || (countdown>0 && --countdown==0);
    if(bytes==2*sizeof(lapse::Layer) && layerMatches>0 && --layerMatches==0) {
        ++allocationsFailed; gate(faultEntered,faultRelease); throw std::bad_alloc();
    }
    if(fail) { ++allocationsFailed; throw std::bad_alloc(); }
}
template<class Predicate> lapse::Status await(lapse::Engine& engine,Predicate predicate) {
    const auto until=std::chrono::steady_clock::now()+6s;
    do { auto state=engine.status(); if(predicate(state)) return state; std::this_thread::sleep_for(5ms); }
    while(std::chrono::steady_clock::now()<until);
    throw std::runtime_error("Expected worker state was not reached");
}
#include "engine_power_video.h"
lapse::Settings config(const std::filesystem::path& folder,bool preview=false) {
    lapse::Settings cfg; cfg.monitorId=L"test"; cfg.monitor={0,0,320,240}; cfg.width=320; cfg.height=240;
    cfg.intervalMs = 60000; cfg.preview=preview; cfg.folder=folder.wstring(); return cfg;
}
void start(lapse::Engine& engine,const lapse::Settings& cfg) {
    engine.configure(cfg); engine.record();
    await(engine,[](const auto& s){return s.state==lapse::State::Recording&&s.frames==1;});
    require(currentPower==desktopPower,"Active desktop recording did not hold its usual request");
}
void paused(lapse::Engine& engine) {
    engine.pause();
    await(engine,[](const auto& s){return s.state==lapse::State::Paused&&currentPower==ES_CONTINUOUS;});
}
void decodeDelete(const std::wstring& path,unsigned count) {
    verify(path,count); require(DeleteFileW(path.c_str())!=FALSE,"Owned output cleanup failed");
}
void checkCalls() {
    std::lock_guard<std::mutex> lock(logMutex);
    require(!powerOverflow && callCount>0,"Power log overflowed or remained empty");
    for(size_t i=0;i<callCount;++i) require(powerCalls[i].thread!=ownerThread,"Power request was issued on UI thread");
}
void recoveredRecording(lapse::Engine& engine,const lapse::Settings& cfg) {
    start(engine,cfg); engine.finish();
    const auto saved=await(engine,[](const auto& s){return s.state==lapse::State::Idle;});
    require(!saved.error&&!saved.savedPath.empty()&&currentPower==ES_CONTINUOUS,"Follow-up recording failed or leaked power");
    decodeDelete(saved.savedPath,static_cast<unsigned>(saved.frames));
}
void finishCase(SaveFault fault,bool wasPaused,const std::filesystem::path& folder) {
    reset(); lapse::Engine engine; ReleaseGates release;
    const auto cfg=config(folder); start(engine,cfg); if(wasPaused) paused(engine);
    const auto before=engine.status(); saveFault=fault; blockFinish=true;
    engine.finish(); awaitEvent(finishEntered);
    const auto during=engine.status(); const auto observed=currentPower.load();
    const bool protectedSave=observed==(wasPaused?savingPower:desktopPower);
    SetEvent(finishRelease);
    const auto saved=await(engine,[](const auto& s){return s.state==lapse::State::Idle;});
    require(during.state==lapse::State::Finishing&&saved.frames==before.frames,"Save changed lifecycle/frame count");
    require(currentPower==ES_CONTINUOUS,"Finalization left a power request active");
    require(saved.error==(fault!=SaveFault::None),"Finalization failure ownership changed");
    if(fault==SaveFault::FailureResult) {
        require(saved.savedPath.empty(),"Failed finalization advertised a final path");
        unsigned found=0;
        for(const auto& item:std::filesystem::directory_iterator(folder)) {
            decodeDelete(item.path().wstring(),static_cast<unsigned>(saved.frames)); ++found;
        }
        require(found==1,"Failed finish did not retain one owned partial movie");
    } else decodeDelete(saved.savedPath,static_cast<unsigned>(saved.frames));
    const int saveFinishes=finishes.load(), failureCount=allocationsFailed.load();
    bool allProtected=true;
    for(int i=0;i<saveFinishes;++i) allProtected &= (finishPower[size_t(i)]&ES_SYSTEM_REQUIRED)!=0;
    recoveredRecording(engine,cfg); checkCalls();
    std::cout<<"  paused="<<wasPaused<<" fault="<<int(fault)<<" atFinish=0x"<<std::hex<<observed<<std::dec
             <<" finishCalls="<<saveFinishes<<" failures="<<failureCount<<'\n';
    require(failureCount==(fault==SaveFault::Preparation?1:0),"Unexpected finalization allocation count");
    require(protectedSave&&allProtected,"Saving from Pause lacked system protection before/during finalization");
}
void quitCase(bool persistentFailure,const std::filesystem::path& folder) {
    reset(); auto engine=std::make_unique<lapse::Engine>(); ReleaseGates release;
    start(*engine,config(folder)); paused(*engine);
    const auto count=engine->status().frames;
    saveFault=persistentFailure?SaveFault::Persistent:SaveFault::None; blockFinish=true;
    std::thread shutdown([&]{engine.reset();});
    struct Join {std::thread& thread; ~Join(){releaseAll();if(thread.joinable())thread.join();}} join{shutdown};
    awaitEvent(finishEntered); const auto observed=currentPower.load(); SetEvent(finishRelease); shutdown.join();
    require(currentPower==ES_CONTINUOUS,"Quit leaked power");
    unsigned found=0;
    for(const auto& item:std::filesystem::directory_iterator(folder)) {
        decodeDelete(item.path().wstring(),static_cast<unsigned>(count)); ++found;
    }
    require(found==1,"Quit did not retain exactly one complete recording");
    require(!persistentFailure||allocationsFailed>0,"Persistent save fault did not trigger");
    bool allProtected=true;
    for(int i=0;i<finishes;++i) allProtected &= (finishPower[size_t(i)]&ES_SYSTEM_REQUIRED)!=0;
    checkCalls();
    std::cout<<"  quit persistent="<<persistentFailure<<" atFinish=0x"<<std::hex<<observed<<std::dec
             <<" finishCalls="<<finishes<<" allocationFailures="<<allocationsFailed<<'\n';
    require(observed==savingPower&&allProtected,"Paused Quit finalized without system protection");
}
void pausedCopyAllocation(const std::filesystem::path& folder) {
    reset(); lapse::Engine engine; ReleaseGates release; auto cfg=config(folder,true); start(engine,cfg);
    blockDesktop=true; awaitEvent(desktopEntered);
    cfg.layers.push_back(cfg.layers.front()); engine.configure(cfg); engine.pause(); layerMatches=2;
    SetEvent(desktopRelease); awaitEvent(faultEntered);
    const auto acknowledged=engine.status(); const auto observed=currentPower.load();
    SetEvent(faultRelease);
    await(engine,[](const auto& s){return s.state==lapse::State::Paused&&s.error;});
    await(engine,[](const auto& s){return s.state==lapse::State::Paused&&!s.error&&currentPower==ES_CONTINUOUS;});
    engine.pause();
    await(engine,[](const auto& s){return s.state==lapse::State::Recording&&s.frames==2;});
    require(currentPower==desktopPower,"Resume did not restore desktop recording request");
    engine.finish(); const auto saved=await(engine,[](const auto& s){return s.state==lapse::State::Idle;});
    require(!saved.error&&currentPower==ES_CONTINUOUS,"Resumed save failed or leaked power");
    decodeDelete(saved.savedPath,static_cast<unsigned>(saved.frames)); checkCalls();
    std::cout<<"  after Paused acknowledgement copy bytes="<<2*sizeof(lapse::Layer)<<" flags=0x"<<std::hex<<observed
             <<std::dec<<" failures="<<allocationsFailed<<'\n';
    require(acknowledged.state==lapse::State::Paused&&allocationsFailed==1,"Fault did not hit actual post-Pause session copy");
    require(observed==ES_CONTINUOUS,"Acknowledged Pause retained its recording power through allocation failure");
}
}
void* operator new(size_t bytes){allocation(bytes);if(void* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](size_t bytes){return ::operator new(bytes);}
void operator delete(void* p)noexcept{std::free(p);} void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,size_t)noexcept{std::free(p);} void operator delete[](void* p,size_t)noexcept{std::free(p);}
namespace probe {
void beforeFinish(){
    const auto index=finishes.fetch_add(1); if(index>=int(finishPower.size()))ExitProcess(77);
    finishPower[size_t(index)]=currentPower.load();
    if(blockFinish.exchange(false))gate(finishEntered,finishRelease);
}
bool afterFinish(bool result,std::wstring& error){
    if(!result)return result;
    switch(saveFault.exchange(SaveFault::None)){
    case SaveFault::Preparation: countdown=1;break;
    case SaveFault::Persistent: allFail=true;break;
    case SaveFault::FailureResult:error=L"Synthetic finalization result failure after real writer close.";return false;
    default:break;
    }
    return result;
}
HRESULT WINAPI initialize(LPVOID reserved,DWORD flags){const auto hr=CoInitializeEx(reserved,flags);worker=true;allFail=false;countdown=0;return hr;}
EXECUTION_STATE WINAPI executionState(EXECUTION_STATE flags){
    std::lock_guard<std::mutex> lock(logMutex);const auto prior=currentPower.exchange(flags);
    if(callCount<powerCalls.size())powerCalls[callCount++]={flags,GetCurrentThreadId()};else powerOverflow=true;
    return prior;
}
BOOL WINAPI forbiddenProcess(LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION){ExitProcess(75);}
}
namespace lapse {
struct Camera::Impl{}; Camera::Camera(){ExitProcess(76);} Camera::~Camera()=default;
bool Camera::start(const std::wstring&,std::wstring&){ExitProcess(76);}void Camera::stop(){ExitProcess(76);}
bool Camera::latest(Frame&,std::wstring&){ExitProcess(76);}bool Camera::latest(Frame&,std::wstring&,uint64_t&){ExitProcess(76);}
bool Camera::latestNewer(Frame&,std::wstring&,CameraSampleInfo&,const CameraSampleInfo&){ExitProcess(76);}
bool captureMonitor(const std::wstring& id,int,int,bool,Frame& out,std::wstring& error){if(id!=L"test"){error=L"Unknown synthetic display.";return false;}if(blockDesktop.exchange(false))gate(desktopEntered,desktopRelease);pattern(out);error.clear();return true;}
}
int main(){
    std::cout<<std::unitbuf;ownerThread=GetCurrentThreadId();
    HANDLE* handles[]={&finishEntered,&finishRelease,&desktopEntered,&desktopRelease,&faultEntered,&faultRelease,&completed};
    for(auto ptr:handles){*ptr=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!*ptr)return 80;}
    std::thread watchdog([]{if(WaitForSingleObject(completed,40000)!=WAIT_OBJECT_0)ExitProcess(79);});
    const auto folder=std::filesystem::current_path()/(L"owned-power-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);int failed=0,passed=0;
    const auto run=[&](const char* name,auto action){try{action();++passed;std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cout<<"FAIL "<<name<<": "<<e.what()<<'\n';}};
    if(FAILED(com)||FAILED(MFStartup(MF_VERSION)))return 81;
    run("active desktop Finish retains existing display/system request",[&]{finishCase(SaveFault::None,false,folder);});
    run("paused Finish protects real finalization",[&]{finishCase(SaveFault::None,true,folder);});
    run("paused save preparation exception retains protection through retry and recovery",[&]{finishCase(SaveFault::Preparation,true,folder);});
    run("paused finalization failure releases protection and recovers",[&]{finishCase(SaveFault::FailureResult,true,folder);});
    run("paused Quit protects real finalization",[&]{quitCase(false,folder);});
    run("paused Quit allocation cleanup keeps protection until release",[&]{quitCase(true,folder);});
    run("actual post-Pause session allocation failure cannot retain active power",[&]{pausedCopyAllocation(folder);});
    if(std::filesystem::exists(folder)&&std::filesystem::is_empty(folder))std::filesystem::remove(folder);
    std::cout<<passed<<"/"<<(passed+failed)<<" passed; no actual power API, physical capture or helper process.\n";
    MFShutdown();CoUninitialize();SetEvent(completed);watchdog.join();for(auto ptr:handles)CloseHandle(*ptr);return failed?1:0;
}

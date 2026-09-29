#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
using namespace lapse;
using Microsoft::WRL::ComPtr;
void check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class Predicate> Status await(Engine& e, Predicate predicate, int ms=10000) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    do { auto status=e.status(); if(predicate(status))return status; std::this_thread::sleep_for(std::chrono::milliseconds(20)); } while(std::chrono::steady_clock::now()<deadline);
    std::wcerr << L"Last engine status: " << e.status().message << L"\n";
    throw std::runtime_error("Engine state timeout");
}
void verifyVideo(const Status& status) {
    check(!status.error && !status.savedPath.empty(),"Recording was not saved");
    check(std::filesystem::file_size(status.savedPath)>0,"Saved file is empty");
    ComPtr<IMFSourceReader> reader;
    check(SUCCEEDED(MFCreateSourceReaderFromURL(status.savedPath.c_str(),nullptr,&reader)),"Saved MP4 cannot be opened");
    uint64_t frames=0;
    for(;;) { DWORD flags=0;ComPtr<IMFSample> sample;HRESULT hr=reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,nullptr,&flags,nullptr,&sample);check(SUCCEEDED(hr),"Saved MP4 sample unreadable");if(sample)++frames;if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break; }
    check(frames==status.frames,"Saved MP4 frame count differs from engine count");
}
int wmain() {
    const int cameraHostResult = runCameraHost(nullptr);
    if (cameraHostResult >= 0) return cameraHostResult;
    HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);check(SUCCEEDED(com),"COM failed");check(SUCCEEDED(MFStartup(MF_VERSION)),"MF startup failed");
    int result=0;
    auto folder=std::filesystem::current_path()/(L"engine-test-"+std::to_wstring(GetCurrentProcessId()));
    try {
        auto displays=enumerateMonitors();check(!displays.empty(),"A desktop session is required for engine tests");
        Settings cfg;cfg.monitor=displays.front().bounds;cfg.monitorId=displays.front().id;cfg.interval=1;cfg.width=320;cfg.height=180;cfg.folder=folder.wstring();cfg.preview=false;
        Engine engine;engine.configure(cfg);engine.record();
        await(engine,[](const Status& s){return s.state==State::Recording && s.frames>=2;});
        engine.pause();auto paused=await(engine,[](const Status& s){return s.state==State::Paused;});
        std::this_thread::sleep_for(std::chrono::milliseconds(1250));
        check(engine.status().frames==paused.frames,"Paused recording added a frame");
        check(engine.status().elapsed-paused.elapsed<0.2,"Paused recording accumulated elapsed time");
        engine.pause();await(engine,[&](const Status& s){return s.state==State::Recording && s.frames>paused.frames;});
        engine.finish();auto saved=await(engine,[](const Status& s){return s.state==State::Idle;});verifyVideo(saved);
        const auto firstPath=saved.savedPath;
        engine.record();await(engine,[](const Status& s){return s.frames>=1 && s.state==State::Recording;});engine.finish();saved=await(engine,[](const Status& s){return s.state==State::Idle;});verifyVideo(saved);check(saved.savedPath!=firstPath,"Repeated recording reused a file path");
        cfg.layers=preset(Mode::Camera);cfg.cameraId=L"nonexistent-device-for-test";engine.configure(cfg);engine.record();
        await(engine,[](const Status& s){return s.state==State::Starting;});engine.finish();await(engine,[](const Status& s){return s.state==State::Idle;},3000);
        cfg.layers=preset(Mode::Desktop);cfg.folder=(folder/L"unavailable-file").wstring();
        HANDLE file=CreateFileW(cfg.folder.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);check(file!=INVALID_HANDLE_VALUE,"Cannot create test fixture");CloseHandle(file);
        engine.configure(cfg);engine.record();auto failure=await(engine,[](const Status& s){return s.state==State::Idle && s.error;});
        check(failure.frames==0,"Failed start counted frames");
        std::cout<<"Engine recording, pause, resume, finalization, restart, cancellation, and output-failure checks passed.\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";result=1;}
    MFShutdown();CoUninitialize();
    // Retain test media for inspection; the directory is unique to this test process.
    std::wcout<<L"Test artifacts: "<<folder.wstring()<<L"\n";
    return result;
}

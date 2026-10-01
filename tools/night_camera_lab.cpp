// Explicit opt-in physical-camera capture. Never run by CTest.
// Saves processed camera BGRA locally for repeatable offline Night comparisons.
#include "capture.h"
#include "night.h"
#include <mfapi.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdlib>
#include <algorithm>
#include <climits>

using namespace lapse;
// Drivers can block synchronously even before the first sample. This opt-in
// diagnostic owns its entire process; bound startup, capture and shutdown.
struct Watchdog {
    std::mutex mutex;std::condition_variable changed;bool finished=false;
    std::thread worker{[this]{
        std::unique_lock<std::mutex> lock(mutex);
        if(!changed.wait_for(lock,std::chrono::seconds(50),[this]{return finished;})) {
            std::cerr<<"Camera diagnostic exceeded its 50-second bound\n";std::_Exit(124);
        }
    }};
    ~Watchdog() {{std::lock_guard<std::mutex> lock(mutex);finished=true;}changed.notify_one();worker.join();}
};
static void bmp(const std::filesystem::path& path,const Frame& f) {
    if(std::filesystem::exists(path))throw std::runtime_error("Image destination already exists");
    BITMAPFILEHEADER a{};BITMAPINFOHEADER b{};
    a.bfType=0x4d42;a.bfOffBits=sizeof(a)+sizeof(b);a.bfSize=a.bfOffBits+DWORD(f.pixels.size());
    b.biSize=sizeof(b);b.biWidth=f.width;b.biHeight=-f.height;b.biPlanes=1;b.biBitCount=32;b.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(&a),sizeof(a));out.write(reinterpret_cast<const char*>(&b),sizeof(b));
    out.write(reinterpret_cast<const char*>(f.pixels.data()),f.pixels.size());
    if(!out)throw std::runtime_error("Cannot write image");
}
static int replay(const std::filesystem::path& input,const std::filesystem::path& output,unsigned requested) {
    std::ifstream in(input,std::ios::binary);uint32_t dimensions[2]{};
    in.read(reinterpret_cast<char*>(dimensions),sizeof(dimensions));
    if(!in || !dimensions[0] || !dimensions[1] || dimensions[0]>NightMaxWidth || dimensions[1]>NightMaxHeight)
        throw std::runtime_error("Invalid capture dimensions");
    const size_t bytes=size_t(dimensions[0])*dimensions[1]*4;
    const auto size=std::filesystem::file_size(input);
    if(size<8+bytes || (size-8)%bytes || (size-8)/bytes>NightMaxSamples)
        throw std::runtime_error("Capture must contain 1 to 300 complete BGRA frames");
    const unsigned samples=requested?requested:static_cast<unsigned>((size-8)/bytes);
    if(samples>(size-8)/bytes)throw std::runtime_error("Requested more frames than capture contains");
    Frame frame;frame.width=static_cast<int>(dimensions[0]);frame.height=static_cast<int>(dimensions[1]);frame.pixels.resize(bytes);
    // Keep the enlarged accumulator off the diagnostic thread's stack too.
    auto accumulator=std::make_unique<NightAccumulator>();std::wstring error;NightSettings settings;settings.enabled=true;
    if(!accumulator->prepare(frame.width,frame.height,error)||!accumulator->begin(settings))throw std::runtime_error("Prepare failed");
    for(unsigned n=1;n<=samples;++n) {
        in.read(reinterpret_cast<char*>(frame.pixels.data()),bytes);
        if(!in || accumulator->add(frame,n)!=NightAdd::Added)throw std::runtime_error("Replay read failed");
    }
    NightResult facts;const auto* out=accumulator->finish(facts);if(!out)throw std::runtime_error("Replay produced no image");
    bmp(output,*out);
    std::cout<<"samples="<<facts.samples<<" gain="<<facts.appliedGain<<" input="<<facts.inputBrightness<<" output="<<facts.outputBrightness<<'\n';
    return 0;
}
int wmain(int argc,wchar_t** argv) {
    if((argc==4 || argc==5) && std::wstring(argv[1])==L"--replay-local") {
        try {
            unsigned requested=0;
            if(argc==5){wchar_t* end=nullptr;const long n=std::wcstol(argv[4],&end,10);if(!*argv[4] || *end || n<1 || n>NightMaxSamples)throw std::runtime_error("Sample count must be 1 to 300");requested=static_cast<unsigned>(n);}
            return replay(argv[2],argv[3],requested);
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    }
    if(argc!=3 || std::wstring(argv[1])!=L"--capture-local") {
        std::wcerr<<L"Explicit camera use: night_camera_lab --capture-local NEW_OUTPUT_DIRECTORY\n"
            <<L"Offline, no camera use: night_camera_lab --replay-local FRAMES.bgra NEW_OUTPUT.bmp [SAMPLES]\n";return 2;
    }
    const std::filesystem::path dir=argv[2];
    if(!std::filesystem::create_directory(dir)){std::cerr<<"Output directory must be new\n";return 2;}
    Watchdog watchdog;
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 3;
    if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();return 3;}
    int result=0;
    try {
        std::wstring error;auto devices=enumerateCameras(error);
        std::wcout<<L"Cameras: "<<devices.size()<<L" "<<error<<std::endl;
        if(devices.empty())throw std::runtime_error("No camera available");
        Camera camera;
        std::wcout<<L"Using "<<devices[0].name<<std::endl;
        if(!camera.start(devices[0].id,error)){std::wcerr<<error<<std::endl;throw std::runtime_error("Camera start failed");}
        CameraSampleInfo seen,info;Frame frame;unsigned count=0;
        const uint64_t start=GetTickCount64(),warm=start+5000,end=warm+30000;
        std::ofstream raw(dir/L"frames.bgra",std::ios::binary),meta(dir/L"samples.csv");
        meta<<"sample,sequence,receipt_ms,timestamp_100ns,width,height,transfer\n";
        auto accumulator=std::make_unique<NightAccumulator>();NightSettings settings;settings.enabled=true;
        while(GetTickCount64()<end) {
            if(camera.latestNewer(frame,error,info,seen)) {
                seen=info;
                if(GetTickCount64()>=warm) {
                    if(!count) {
                        if(!accumulator->prepare(frame.width,frame.height,error)||!accumulator->begin(settings))throw std::runtime_error("Prepare failed");
                        bmp(dir/L"first.bmp",frame);
                        const uint32_t header[]={uint32_t(frame.width),uint32_t(frame.height)};
                        raw.write(reinterpret_cast<const char*>(header),sizeof(header));
                    }
                    if(accumulator->add(frame,info.sequence)!=NightAdd::Added)throw std::runtime_error("Sample rejected");
                    raw.write(reinterpret_cast<const char*>(frame.pixels.data()),frame.pixels.size());
                    meta<<++count<<','<<info.sequence<<','<<info.receivedTick-start<<','<<info.timestamp100ns<<','<<frame.width<<','<<frame.height<<','<<info.transferFunction<<'\n';
                }
            } else if(!error.empty()){std::wcerr<<error<<std::endl;throw std::runtime_error("Camera read failed");}
            Sleep(200);
        }
        camera.stop();NightResult facts;auto out=accumulator->finish(facts);
        if(!out||!raw||!meta)throw std::runtime_error("No samples or file write failed");
        bmp(dir/L"processed.bmp",*out);
        std::cout<<"samples="<<facts.samples<<" gain="<<facts.appliedGain<<" input="<<facts.inputBrightness<<" output="<<facts.outputBrightness<<std::endl;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;result=1;}
    MFShutdown();CoUninitialize();return result;
}

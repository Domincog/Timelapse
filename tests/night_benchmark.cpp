// Opt-in CPU/quality experiment. Compare revisions using the same binary
// configuration and this source; never compare default-exposure MSE alone.
#include "night_quality_fixture.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
using namespace lapse;
using Clock=std::chrono::steady_clock;
void require(bool value) {if(!value)throw std::runtime_error("Night benchmark operation failed");}
double cpuMilliseconds() {
    FILETIME created{},exited{},kernel{},user{};require(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)!=0);
    const auto ticks=[](FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return double(ticks(kernel)+ticks(user))/10000;
}
void quality() {
    const auto truth=night_quality::scene();auto sample=truth;
    std::cout<<"mode,noise,samples,gain,brightness,rgb_mse,luma_mse,chroma_mse,line_contrast,reference_line_contrast\n";
    const char* names[]={"independent_color","correlated_color","block_color","luminance"};
    for(bool fixed:{true,false})for(unsigned type=0;type<4;++type)for(unsigned samples:{4u,16u,64u}) {
        NightTuning tuning;tuning.linear=true;tuning.maxGain=64;tuning.fixed=fixed;tuning.fixedGain=8;
        // Default construction is essential: do not override the revision's
        // production cap while measuring default automatic exposure.
        auto accumulator=fixed?std::make_unique<NightAccumulator>(tuning):std::make_unique<NightAccumulator>();
        std::wstring message;NightSettings settings;settings.enabled=true;
        require(accumulator->prepare(truth.width,truth.height,message)&&accumulator->begin(settings));
        for(unsigned n=0;n<samples;++n){night_quality::noisy(truth,sample,n,night_quality::Noise(type));require(accumulator->add(sample,n+1)==NightAdd::Added);}
        NightResult result;const auto* output=accumulator->finish(result);require(output!=nullptr);
        const auto expected=night_quality::reference(truth,result.appliedGain,fixed);
        const auto score=night_quality::metrics(*output,expected),ideal=night_quality::metrics(expected,expected);
        std::cout<<(fixed?"matched8":"automatic")<<','<<names[type]<<','<<samples<<','<<result.appliedGain<<','<<result.outputBrightness<<','
            <<score.rgb<<','<<score.luma<<','<<score.chroma<<','<<score.lineContrast<<','<<ideal.lineContrast<<'\n';
    }
}
void timing() {
    std::cout<<"scene,width,height,samples,add_ms,finish_ms,window_ms,cpu_window_ms,storage_bytes\n";
    for(const auto dimensions:{std::pair<int,int>{1280,720},{1920,1080}})for(bool noisy:{false,true}) {
        NightAccumulator accumulator;std::wstring message;NightSettings settings;settings.enabled=true;
        auto truth=night_quality::scene(dimensions.first,dimensions.second),sample=truth;
        require(accumulator.prepare(truth.width,truth.height,message));
        // Pre-generate the frames so timing excludes test-noise generation.
        std::array<Frame,16> inputs;for(unsigned n=0;n<inputs.size();++n){inputs[n]=sample;if(noisy)night_quality::noisy(truth,inputs[n],n,night_quality::Noise::IndependentColor);}
        std::array<double,7> additions{},finishes{},windows{};NightResult result;double cpuStart=0;
        for(unsigned trial=0;trial<8;++trial) {
            if(trial==1)cpuStart=cpuMilliseconds();
            const auto start=Clock::now();require(accumulator.begin(settings));
            const auto adding=Clock::now();for(unsigned n=0;n<inputs.size();++n)require(accumulator.add(inputs[n],n+1)==NightAdd::Added);
            const auto finishing=Clock::now();require(accumulator.finish(result)!=nullptr);const auto stop=Clock::now();
            if(trial){additions[trial-1]=std::chrono::duration<double,std::milli>(finishing-adding).count()/inputs.size();finishes[trial-1]=std::chrono::duration<double,std::milli>(stop-finishing).count();windows[trial-1]=std::chrono::duration<double,std::milli>(stop-start).count();}
        }
        const double cpu=(cpuMilliseconds()-cpuStart)/7;
        std::sort(additions.begin(),additions.end());std::sort(finishes.begin(),finishes.end());std::sort(windows.begin(),windows.end());
        std::cout<<(noisy?"noisy":"clean")<<','<<truth.width<<','<<truth.height<<",16,"<<additions[3]<<','<<finishes[3]<<','<<windows[3]<<','<<cpu<<','<<accumulator.storageBytes()<<'\n';
    }
}
void confidence() {
    std::cout<<"level,noise,samples,gain,brightness\n";
    for(unsigned level:{4u,8u,12u})for(unsigned type:{0u,1u})for(unsigned samples:{16u,64u}) {
        auto truth=night_quality::scene(128,96),sample=truth;
        for(size_t p=0;p<truth.pixels.size();p+=4)for(unsigned c=0;c<3;++c)truth.pixels[p+c]=uint8_t(level);
        NightAccumulator accumulator;std::wstring message;NightSettings settings;settings.enabled=true;
        require(accumulator.prepare(truth.width,truth.height,message)&&accumulator.begin(settings));
        for(unsigned n=0;n<samples;++n){night_quality::noisy(truth,sample,n,night_quality::Noise(type));require(accumulator.add(sample,n+1)==NightAdd::Added);}
        NightResult result;require(accumulator.finish(result)!=nullptr);
        std::cout<<level<<','<<(type?"held8":"independent")<<','<<samples<<','<<result.appliedGain<<','<<result.outputBrightness<<'\n';
    }
}
void images(const std::string& prefix) {
    // Binary PPM is deliberately simple and lossless. Every pixel is generated
    // by the synthetic fixture; this path never accesses a physical camera.
    const auto save=[&](const Frame& frame,const std::string& suffix) {
        std::ofstream stream(prefix+suffix+".ppm",std::ios::binary);require(bool(stream));
        stream<<"P6\n"<<frame.width<<' '<<frame.height<<"\n255\n";
        for(size_t p=0;p<frame.pixels.size();p+=4){stream.put(char(frame.pixels[p+2]));stream.put(char(frame.pixels[p+1]));stream.put(char(frame.pixels[p]));}
        require(bool(stream));
    };
    const auto truth=night_quality::scene();auto sample=truth;save(truth,"-input-truth");save(night_quality::reference(truth,8,true),"-matched-truth");
    for(bool fixed:{true,false}) {
        NightTuning tuning;tuning.fixed=fixed;tuning.fixedGain=8;
        auto accumulator=fixed?std::make_unique<NightAccumulator>(tuning):std::make_unique<NightAccumulator>();
        std::wstring message;NightSettings settings;settings.enabled=true;
        require(accumulator->prepare(truth.width,truth.height,message)&&accumulator->begin(settings));
        for(unsigned n=0;n<16;++n){night_quality::noisy(truth,sample,n,night_quality::Noise::IndependentColor);require(accumulator->add(sample,n+1)==NightAdd::Added);}
        NightResult result;const auto* output=accumulator->finish(result);require(output!=nullptr);save(*output,fixed?"-matched8":"-automatic");
    }
}
}
int main(int argc,char** argv) {
    try {std::cout<<std::fixed<<std::setprecision(4);if(argc==2&&std::string(argv[1])=="--timing")timing();else if(argc==2&&std::string(argv[1])=="--confidence")confidence();else if(argc==3&&std::string(argv[1])=="--images")images(argv[2]);else quality();return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

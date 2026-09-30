// Pure synthetic image processing. No camera, capture, window or profile I/O.
#include "night.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<unsigned> allocations{0};
using namespace lapse;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
Frame uniform(int w,int h,uint8_t value) {
    Frame frame;frame.width=w;frame.height=h;frame.pixels.resize(size_t(w)*h*4);
    for(size_t p=0;p<frame.pixels.size();p+=4) {
        frame.pixels[p]=frame.pixels[p+1]=frame.pixels[p+2]=value;frame.pixels[p+3]=uint8_t(p);
    }
    return frame;
}
NightSettings enabled() { NightSettings s;s.enabled=true;return s; }
uint32_t random(uint32_t value) {value^=value>>16;value*=0x7feb352d;value^=value>>15;value*=0x846ca68b;return value^(value>>16);}
unsigned gained(unsigned value,double gain) {return night_detail::encoded(static_cast<unsigned>(std::lround(std::min(65535.0,value*gain))));}
double error(const Frame& output,const Frame& truth,double gain) {
    uint64_t sum=0;
    for(size_t p=0;p<output.pixels.size();p+=4)for(unsigned c=0;c<3;++c) {
        const int difference=int(output.pixels[p+c])-int(gained(night_detail::linear(truth.pixels[p+c]),gain));
        sum+=difference*difference;
    }
    return double(sum)/(output.pixels.size()/4*3);
}
Frame amplify(const Frame& input,double gain) {
    Frame result=input;
    for(size_t p=0;p<input.pixels.size();p+=4)for(unsigned c=0;c<3;++c)
        result.pixels[p+c]=uint8_t(gained(night_detail::linear(input.pixels[p+c]),gain));
    return result;
}
void contracts() {
    for(unsigned code=0;code<256;++code)
        require(night_detail::encoded(night_detail::linear(uint8_t(code)))==code,"Transfer tables failed constant-code roundtrip");
    require(night_detail::encoded(0)==0&&night_detail::encoded(65535)==255&&night_detail::encoded(UINT_MAX)==255,"Transfer endpoints overflowed");
    NightAccumulator accumulator;NightResult facts;std::wstring message;auto settings=enabled();
    require(!accumulator.begin(settings)&&!accumulator.finish(facts),"Unprepared accumulator accepted work");
    require(!accumulator.prepare(0,1,message)&&!accumulator.prepare(1281,720,message)&&!accumulator.prepare(1,721,message),"Invalid image bound accepted");
    require(accumulator.prepare(256,2,message)&&accumulator.begin(settings),"Valid image preparation failed");
    auto frame=uniform(256,2,96);auto wrong=uniform(1,1,96);
    require(accumulator.add(wrong,1)==NightAdd::Invalid&&accumulator.add(frame,0)==NightAdd::Invalid,"Invalid sample accepted");
    require(accumulator.add(frame,9)==NightAdd::Added,"First sample rejected");
    require(accumulator.add(frame,9)==NightAdd::Duplicate&&accumulator.add(frame,8)==NightAdd::Duplicate,"Duplicate/regressing source accepted");
    allocations=0;countAllocations=true;
    for(unsigned n=1;n<NightMaxSamples;++n)require(accumulator.add(frame,n+9)==NightAdd::Added,"Bounded sample rejected");
    require(accumulator.add(frame,1000)==NightAdd::Full,"Sample cap did not stop accumulation");
    const auto* result=accumulator.finish(facts);const auto first=facts;
    require(result&&accumulator.finish(facts)==result&&facts.appliedGain==first.appliedGain,"Finish is not idempotent");
    countAllocations=false;require(allocations==0&&facts.samples==NightMaxSamples,"Steady image work allocated or miscounted");
    require(facts.appliedGain==1&&facts.outputBrightness==96&&!facts.targetLimited,"Constant target changed brightness");
    for(size_t p=0;p<result->pixels.size();p+=4)require(result->pixels[p]==96&&result->pixels[p+1]==96&&result->pixels[p+2]==96&&result->pixels[p+3]==255,"Constant color/alpha changed");
    require(accumulator.add(frame,1001)==NightAdd::Invalid,"Finished image mutated");
    auto invalid=settings;invalid.durationMs=999;require(!validNightSettings(invalid)&&!accumulator.begin(invalid),"Short invalid duration accepted");
    invalid=settings;invalid.durationMs=NightMaxDurationMs+1;require(!validNightSettings(invalid),"Unbounded duration accepted");
    invalid=settings;invalid.targetBrightness=0;require(!validNightSettings(invalid),"Zero target accepted");
    invalid=settings;invalid.targetBrightness=256;require(!validNightSettings(invalid),"Unbounded target accepted");
    for(int target:{64,96,128})for(int duration:{0,1000,NightMaxDurationMs}){settings.targetBrightness=target;settings.durationMs=duration;require(validNightSettings(settings),"Supported policy rejected");}
    require(accumulator.begin(enabled())&&!accumulator.finish(facts),"Empty window fabricated a frame");
    std::cout<<"PASS bounds, source uniqueness, all256transfercodes, maximum sums, actual count, alpha and idempotent result.\n";
}
void stability() {
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();NightResult result;
    require(accumulator.prepare(64,32,message),"Controller allocation failed");
    auto frame=uniform(64,32,0);
    const auto render=[&](int level) {
        for(size_t p=0;p<frame.pixels.size();p+=4)frame.pixels[p]=frame.pixels[p+1]=frame.pixels[p+2]=uint8_t(level);
        accumulator.begin(settings);for(unsigned n=1;n<=10;++n)accumulator.add(frame,n);return accumulator.finish(result);
    };
    render(0);require(result.outputBrightness==0&&result.appliedGain==1&&result.targetLimited,"Black was invented/boosted");
    render(7);require(result.appliedGain==1&&result.outputBrightness==7&&result.targetLimited,"Black-floor control amplified nearzero signal");
    render(64);require(std::abs(result.outputBrightness-96)<=1,"Auto target not reached");const double gain=result.appliedGain;
    render(65);require(result.appliedGain==gain&&std::abs(result.outputBrightness-96)<=3,"Deadband did not stabilize small metering changes");
    for(int level:{72,80,88,96,112,128,160}) {render(level);require(std::abs(result.outputBrightness-96)<=5,"Dawn target tracking unstable");}
    render(192);require(result.appliedGain<=0.251,"Abrupt bright cut retained old exposure");
    render(48);require(std::abs(result.outputBrightness-96)<=2,"Abrupt dark cut recovered too slowly");
    accumulator.resetExposure();render(48);const double first=result.appliedGain;
    const size_t storage=accumulator.storageBytes();allocations=0;countAllocations=true;
    for(unsigned window=0;window<1000;++window)render(48);
    countAllocations=false;
    require(result.appliedGain==first&&accumulator.storageBytes()==storage&&allocations==0,"Long stable input drifted/allocated/grew");
    std::cout<<"PASS automatic target, black floor, deadband, dawn/cuts, and1000stable allocation-free windows.\n";
}
void highlightsAndManual() {
    NightAccumulator autoWindow,manualWindow;std::wstring message;auto automatic=enabled(),manual=enabled();manual.durationMs=1000;
    require(autoWindow.prepare(128,128,message)&&manualWindow.prepare(128,128,message),"Highlight prepare failed");
    auto frame=uniform(128,128,30);
    for(int y=0;y<32;++y)for(int x=0;x<32;++x)for(int c=0;c<3;++c)frame.pixels[(size_t(y)*128+x)*4+c]=255;
    autoWindow.begin(automatic);manualWindow.begin(manual);
    for(unsigned n=1;n<=10;++n){autoWindow.add(frame,n);manualWindow.add(frame,n);}
    NightResult a,b;const auto* out=autoWindow.finish(a);const auto* same=manualWindow.finish(b);
    require(out&&same&&out->pixels==same->pixels&&a.appliedGain==b.appliedGain,"Manual duration disabled automatic brightness");
    require(a.targetLimited&&a.appliedGain<1&&out->pixels[0]<=251,"Highlight constraint missing/untruthful");
    require(a.suggestedDurationMs>=NightMinDurationMs&&a.suggestedDurationMs<=NightMaxDurationMs,"Duration suggestion not bounded");
    std::cout<<"PASS highlight headroom, truthful limited target, manual-duration automatic brightness and bounded suggestion.\n";
}
void actualNoiseReduction() {
    constexpr int width=128,height=96;NightAccumulator accumulator;std::wstring message;auto settings=enabled();
    require(accumulator.prepare(width,height,message)&&accumulator.begin(settings),"Noise prepare failed");
    auto truth=uniform(width,height,48),sample=truth,first=truth;
    for(size_t p=0;p<truth.pixels.size();p+=4)for(unsigned c=0;c<3;++c)truth.pixels[p+c]=uint8_t(38+((p/4)/3)%22+c*3);
    for(unsigned n=0;n<10;++n){
        for(size_t p=0;p<sample.pixels.size();p+=4)for(unsigned c=0;c<3;++c){
            const int noise=int(random(uint32_t(p)+n*65537+c*997)%25)-12;
            sample.pixels[p+c]=uint8_t(int(truth.pixels[p+c])+noise);
        }
        if(!n)first=sample;accumulator.add(sample,n+1);
    }
    NightResult result;const auto* image=accumulator.finish(result);require(image,"Noise result missing");
    const auto single=amplify(first,result.appliedGain);
    const double combinedError=error(*image,truth,result.appliedGain),singleError=error(single,truth,result.appliedGain);
    require(combinedError*5<singleError,"Blending did not reduce actual detail/noise error at matched gain");
    require(result.outputBrightness>=91&&result.outputBrightness<=101,"Noise result did not reach target brightness");
    std::cout<<"PASS matched-gain synthetic detail/noise MSE "<<combinedError<<" versus single "<<singleError<<".\n";
}
void periodicMetering() {
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();
    require(accumulator.prepare(128,128,message)&&accumulator.begin(settings),"Periodic image prepare failed");
    auto frame=uniform(128,128,48);
    // The former fixed8x8 grid saw only black while most pixels contained
    // signal, preventing Auto from raising the actual average brightness.
    for(int y=0;y<128;++y)for(int x=0;x<128;x+=8)for(int c=0;c<3;++c)frame.pixels[(size_t(y)*128+x)*4+c]=0;
    for(unsigned n=1;n<=10;++n)accumulator.add(frame,n);NightResult result;
    require(accumulator.finish(result)&&result.inputBrightness>35&&result.appliedGain>3&&std::abs(result.outputBrightness-96)<=1&&!result.targetLimited,
        "Periodic image aliased the brightness meter");
    for(int target:{64,96,128}){
        settings.targetBrightness=target;accumulator.resetExposure();accumulator.begin(settings);
        auto level=uniform(128,128,48);accumulator.add(level,1);accumulator.finish(result);
        require(std::abs(result.outputBrightness-target)<=1&&!result.targetLimited,"Brightness preset failed to track a reachable target");
    }
    std::cout<<"PASS full-image periodic-pattern metering and three reachable brightness presets.\n";
}
void lowSignalStability() {
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();NightResult result;
    require(accumulator.prepare(64,64,message),"Low-signal prepare failed");
    double previous=7;
    for(unsigned window=0;window<40;++window){
        const uint8_t level=uint8_t(7+(window&1));auto frame=uniform(64,64,level);
        accumulator.begin(settings);accumulator.add(frame,1);require(accumulator.finish(result),"Low-signal result missing");
        require(result.appliedGain<=1.001&&std::abs(result.outputBrightness-previous)<=1&&result.targetLimited,
            "Near-black meter threshold caused a discontinuous gain/output jump");
        previous=result.outputBrightness;
    }
    accumulator.resetExposure();previous=0;
    for(unsigned level=0;level<=24;++level){
        auto frame=uniform(64,64,uint8_t(level));accumulator.begin(settings);accumulator.add(frame,1);accumulator.finish(result);
        require(result.outputBrightness>=previous&&result.outputBrightness-previous<=7&&result.appliedGain<=8,
            "Gradual low-signal rise flickered or exceeded bounded gain");
        if(level<=8)require(result.appliedGain==1&&result.outputBrightness==level,"Black floor invented weak signal");
        previous=result.outputBrightness;
    }
    require(result.outputBrightness>24,"Low-signal ceiling disabled useful automatic brightening");
    std::cout<<"PASS alternating near-black boundary and gradual low-signal brightness without a gain discontinuity.\n";
}
}
void* operator new(size_t size) {if(countAllocations)++allocations;if(void* value=std::malloc(size?size:1))return value;throw std::bad_alloc();}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete(void* value,size_t) noexcept {std::free(value);}
int main() {
    try {contracts();stability();highlightsAndManual();actualNoiseReduction();periodicMetering();lowSignalStability();std::cout<<"All night image contracts passed using synthetic frames.\n";return 0;}
    catch(const std::exception& error){countAllocations=false;std::cerr<<error.what()<<'\n';return 1;}
}

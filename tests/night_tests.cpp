// Pure synthetic image processing. No camera, capture, window or profile I/O.
#include "night.h"
#include "night_quality_fixture.h"
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
double error(const Frame& output,const Frame& truth) {
    uint64_t sum=0;
    for(size_t p=0;p<output.pixels.size();p+=4)for(unsigned c=0;c<3;++c) {
        const int difference=int(output.pixels[p+c])-int(truth.pixels[p+c]);
        sum+=difference*difference;
    }
    return double(sum)/(output.pixels.size()/4*3);
}
void contracts() {
    for(unsigned code=0;code<256;++code)
        require(night_detail::encoded(night_detail::linear(uint8_t(code)))==code,"Transfer tables failed constant-code roundtrip");
    require(night_detail::encoded(0)==0&&night_detail::encoded(65535)==255&&night_detail::encoded(UINT_MAX)==255,"Transfer endpoints overflowed");
    NightAccumulator accumulator;NightResult facts;std::wstring message;auto settings=enabled();
    require(!accumulator.begin(settings)&&!accumulator.finish(facts),"Unprepared accumulator accepted work");
    require(!accumulator.prepare(0,1,message)&&!accumulator.prepare(1921,1080,message)&&!accumulator.prepare(1,1081,message),"Invalid image bound accepted");
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
    render(7);require(result.appliedGain>1&&result.outputBrightness>7&&result.targetLimited,
        "Repeated coherent dim signal did not benefit from its lower sample-aware floor");
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
    require(a.targetLimited&&a.appliedGain>4&&out->pixels[0]==255&&out->pixels.back()==255&&
        out->pixels[(size_t(64)*128+64)*4]>=80,"Bright lamp suppressed dark surroundings or lost its endpoint");
    require(a.suggestedDurationMs>=NightMinDurationMs&&a.suggestedDurationMs<=NightMaxDurationMs,"Duration suggestion not bounded");
    std::cout<<"PASS dark surroundings with a bright lamp, truthful limited target, manual-duration automatic brightness and bounded suggestion.\n";
}
void actualNoiseReduction() {
    constexpr int width=128,height=96;NightAccumulator accumulator,cleanWindow,firstWindow;std::wstring message;auto settings=enabled();
    require(accumulator.prepare(width,height,message)&&accumulator.begin(settings)&&
        cleanWindow.prepare(width,height,message)&&cleanWindow.begin(settings)&&
        firstWindow.prepare(width,height,message)&&firstWindow.begin(settings),"Noise prepare failed");
    auto truth=uniform(width,height,48),sample=truth,first=truth;
    for(size_t p=0;p<truth.pixels.size();p+=4)for(unsigned c=0;c<3;++c)truth.pixels[p+c]=uint8_t(38+((p/4)/3)%22+c*3);
    for(unsigned n=0;n<10;++n){
        for(size_t p=0;p<sample.pixels.size();p+=4)for(unsigned c=0;c<3;++c){
            const int noise=int(random(uint32_t(p)+n*65537+c*997)%25)-12;
            sample.pixels[p+c]=uint8_t(int(truth.pixels[p+c])+noise);
        }
        if(!n)first=sample;accumulator.add(sample,n+1);cleanWindow.add(truth,n+1);firstWindow.add(first,n+1);
    }
    NightResult result;const auto* image=accumulator.finish(result);require(image,"Noise result missing");
    NightResult cleanResult,singleResult;const auto* clean=cleanWindow.finish(cleanResult);const auto* single=firstWindow.finish(singleResult);
    require(clean&&single,"Independent clean/single reference failed");
    const double combinedError=error(*image,*clean),singleError=error(*single,*clean);
    require(combinedError*5<singleError,"Blending did not reduce visible detail/noise error against the clean scene");
    require(result.outputBrightness>=91&&result.outputBrightness<=101,"Noise result did not reach target brightness");
    std::cout<<"PASS automatically rendered synthetic detail/noise MSE "<<combinedError<<" versus single "<<singleError<<".\n";
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
        auto level=uniform(128,128,48);for(unsigned n=1;n<=4;++n)accumulator.add(level,n);accumulator.finish(result);
        require(std::abs(result.outputBrightness-target)<=1&&!result.targetLimited,"Brightness preset failed to track a reachable target");
    }
    settings.targetBrightness=192;accumulator.resetExposure();accumulator.begin(settings);
    auto limited=uniform(128,128,48);accumulator.add(limited,1);accumulator.finish(result);
    require(result.appliedGain<=8&&result.outputBrightness<150&&result.targetLimited,
        "Lone-frame gain ceiling or truthful unreachable-target report changed");
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
        require(result.outputBrightness>=previous&&result.outputBrightness-previous<=7&&result.appliedGain<=16,
            "Gradual low-signal rise flickered or exceeded bounded gain");
        if(level<=8)require(result.appliedGain==1&&result.outputBrightness==level,"Black floor invented weak signal");
        previous=result.outputBrightness;
    }
    require(result.outputBrightness>24,"Low-signal ceiling disabled useful automatic brightening");
    std::cout<<"PASS alternating near-black boundary and gradual low-signal brightness without a gain discontinuity.\n";
}
void darkerSignalAndSampling() {
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();NightResult result;
    require(accumulator.prepare(64,64,message),"Dim-signal prepare failed");
    const auto render=[&](uint8_t level,unsigned samples) {
        auto frame=uniform(64,64,level);accumulator.resetExposure();
        require(accumulator.begin(settings),"Dim-signal begin failed");
        for(unsigned n=1;n<=samples;++n)require(accumulator.add(frame,n)==NightAdd::Added,"Dim sample rejected");
        return accumulator.finish(result);
    };
    for(unsigned samples:{1u,2u,4u,8u,16u,64u,NightMaxSamples}) {
        const auto* black=render(0,samples);require(black&&result.appliedGain==1&&result.outputBrightness==0&&result.targetLimited,
            "More samples fabricated light in a completely black scene");
        for(size_t p=0;p<black->pixels.size();p+=4)require(black->pixels[p]==0&&black->pixels[p+1]==0&&black->pixels[p+2]==0&&black->pixels[p+3]==255,
            "Black-frame color or alpha changed");
    }
    render(8,1);require(result.appliedGain==1&&result.outputBrightness==8,"A lone near-black sample bypassed the noise floor");
    double previous=8;
    for(unsigned samples:{2u,4u,8u,16u,64u}) {
        require(render(8,samples)&&result.samples==samples&&result.outputBrightness>=previous&&result.appliedGain<=64,
            "Averaged coherent shadows did not improve monotonically within the gain bound");
        previous=result.outputBrightness;
    }
    // Sixty-four clean contributions support the expanded shadow exposure,
    // while the separate single-frame assertions retain their original cap.
    require(previous>=85,"Well-averaged low-level detail remained too dark to use");
    for(uint8_t level:{uint8_t(24),uint8_t(32)}) {
        require(render(level,16)&&std::abs(result.outputBrightness-96)<=2&&!result.targetLimited&&result.appliedGain>8,
            "Well-averaged darker scene did not reach the available brightness target");
        require(result.suggestedDurationMs>NightMinDurationMs&&result.suggestedDurationMs<=NightMaxDurationMs,
            "Dim exposure demand did not recommend useful bounded averaging");
    }
    render(7,16);require(result.suggestedDurationMs>NightInitialDurationMs,
        "Low-signal gain restriction incorrectly shortened a dark scene's Auto window");
    std::cout<<"PASS black invariance at1..300samples, conservative lone signal, coherent shadow recovery and dark Auto windows.\n";
}
void mixedDetailAndColor() {
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();NightResult result;
    require(accumulator.prepare(128,128,message),"Mixed scene prepare failed");
    auto frame=uniform(128,128,16);
    for(int y=0;y<128;++y)for(int x=0;x<128;++x)for(unsigned c=0;c<3;++c)
        frame.pixels[(size_t(y)*128+x)*4+c]=uint8_t(x<32&&y<32?240:((x+y)&1?24:16));
    require(accumulator.begin(settings),"Mixed scene begin failed");
    for(unsigned n=1;n<=16;++n)accumulator.add(frame,n);
    const auto* image=accumulator.finish(result);require(image,"Mixed scene result failed");
    const auto dark=image->pixels[(size_t(64)*128+64)*4],light=image->pixels[(size_t(64)*128+65)*4];
    require(dark>=50&&light>=dark+15&&result.appliedGain>8,
        "A bright patch suppressed dim scene detail or flattened its contrast");
    require(image->pixels[0]>=240&&image->pixels[0]<255,"Shadow recovery clipped a meaningful unsaturated highlight");
    require(result.outputBrightness>result.inputBrightness&&result.samples==16&&result.targetLimited,
        "Mixed-scene metadata concealed its actual brightness or limited target");

    require(accumulator.prepare(256,16,message),"Color/ramp prepare failed");
    frame=uniform(256,16,16);
    for(unsigned x=0;x<256;++x)for(unsigned c=0;c<3;++c)frame.pixels[x*4+c]=uint8_t(x);
    const std::array<std::array<uint8_t,3>,6> colors{{{{12,24,48}},{{18,36,72}},{{24,48,96}},{{30,100,250}},{{0,32,80}},{{0,0,255}}}};
    for(size_t i=0;i<colors.size();++i)for(unsigned c=0;c<3;++c)frame.pixels[(256+i)*4+c]=colors[i][c];
    accumulator.resetExposure();require(accumulator.begin(settings),"Color/ramp begin failed");
    for(unsigned n=1;n<=16;++n)accumulator.add(frame,n);
    image=accumulator.finish(result);require(image&&result.appliedGain>1,"Color/ramp did not exercise shadow recovery");
    unsigned previous=0;
    for(unsigned x=0;x<256;++x) {
        const auto* p=&image->pixels[x*4];
        require(p[0]>=previous&&p[0]==p[1]&&p[1]==p[2]&&p[3]==255,"Tone curve inverted gray detail or introduced color/alpha");
        previous=p[0];
    }
    require(image->pixels[0]==0&&image->pixels[255*4]==255,"Tone curve lost its black/white endpoint");
    for(size_t i=0;i<colors.size();++i) {
        double lowest=100,highest=0;
        for(unsigned c=0;c<3;++c) {
            const auto input=colors[i][c],output=image->pixels[(256+i)*4+c];
            if(!input)require(!output,"Common-channel tone mapping invented a color component");
            else {
                const double scale=double(night_detail::linear(output))/night_detail::linear(input);
                lowest=std::min(lowest,scale);highest=std::max(highest,scale);
            }
        }
        require(highest<=lowest*1.08,"Shadow/highlight mapping changed linear color ratios beyond quantization");
    }
    std::cout<<"PASS bright-patch shadow contrast, no meaningful highlight clipping, monotone gray ramp and common-scale color ratios.\n";
}
void lowSignalSampleJitter() {
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();NightResult result;
    require(accumulator.prepare(64,64,message),"Sample-jitter prepare failed");
    for(uint8_t level=4;level<=8;++level) {
        auto frame=uniform(64,64,level);accumulator.resetExposure();double previousOutput=0,previousGain=0;
        for(unsigned window=0;window<20;++window) {
            const unsigned samples=4+(window&1);
            require(accumulator.begin(settings),"Sample-jitter begin failed");
            for(unsigned n=1;n<=samples;++n)accumulator.add(frame,n);
            require(accumulator.finish(result)&&result.samples==samples&&result.appliedGain>=1&&result.appliedGain<=16,
                "Low-signal sample jitter exceeded physical contribution/gain bounds");
            if(window)require(std::abs(result.outputBrightness-previousOutput)<=4&&
                std::max(result.appliedGain,previousGain)<=std::min(result.appliedGain,previousGain)*1.251,
                "One extra near-black contribution caused an excessive exposure jump");
            require(result.suggestedDurationMs==NightMaxDurationMs,"Low-signal sample jitter shortened useful dark averaging");
            previousOutput=result.outputBrightness;previousGain=result.appliedGain;
        }
    }
    std::cout<<"PASS bounded near-black variation with alternating4/5contributions and stable dark collection demand.\n";
}
void mixedMeterContinuity() {
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();NightResult result;
    require(accumulator.prepare(128,100,message),"Mixed continuity prepare failed");
    double previous=0;
    for(unsigned patchRows:{49u,50u,51u})for(unsigned patchLevel:{63u,64u,65u}) {
        auto frame=uniform(128,100,24);
        for(size_t p=0;p<size_t(patchRows)*128*4;p+=4)for(unsigned c=0;c<3;++c)frame.pixels[p+c]=uint8_t(patchLevel);
        accumulator.resetExposure();require(accumulator.begin(settings),"Mixed continuity begin failed");
        for(unsigned n=1;n<=16;++n)accumulator.add(frame,n);
        const auto* image=accumulator.finish(result);require(image,"Mixed continuity result failed");
        const double shadow=image->pixels[size_t(80)*128*4];
        require(!previous||std::abs(shadow-previous)<=4,"Small highlight/median changes caused an exposure discontinuity");
        previous=shadow;
    }
    std::cout<<"PASS smooth mixed-scene exposure across63/64/65codes and49/50/51percent bright distributions.\n";
}
void motionAverageContract() {
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();NightResult result;
    require(accumulator.prepare(64,8,message)&&accumulator.begin(settings),"Motion prepare failed");
    auto sample=uniform(64,8,0);
    for(unsigned n=0;n<4;++n) {
        std::fill(sample.pixels.begin(),sample.pixels.end(),uint8_t(0));
        for(unsigned c=0;c<3;++c) {
            sample.pixels[(size_t(3)*64+4+n)*4+c]=64;
            sample.pixels[(size_t(4)*64+40)*4+c]=64;
        }
        require(accumulator.add(sample,100+n)==NightAdd::Added,"Distinct motion sample rejected");
    }
    auto duplicate=uniform(64,8,255);
    require(accumulator.add(duplicate,103)==NightAdd::Duplicate,"Repeated motion sample was counted twice");
    const auto* image=accumulator.finish(result);require(image&&result.samples==4&&result.appliedGain==1,"Sparse motion blend fabricated brightness/count");
    const auto averaged=night_detail::encoded((night_detail::linear(64)+2)/4);
    for(unsigned n=0;n<4;++n)for(unsigned c=0;c<3;++c)
        require(image->pixels[(size_t(3)*64+4+n)*4+c]==averaged,"Moving detail lost its actual linear-light time average");
    for(unsigned c=0;c<3;++c)require(image->pixels[(size_t(4)*64+40)*4+c]==64&&image->pixels[c]==0,
        "Motion averaging changed static detail or invented a trail outside observed positions");
    std::cout<<"PASS honest moving-detail average, static detail, empty surroundings and duplicate suppression.\n";
}
void averagedToneMonotonicity() {
    // All300-frame mixing fractions of every adjacent8-bit code pair expose
    // the fine linear values that a single encoded gray ramp cannot reach.
    constexpr unsigned fractions=NightMaxSamples+1,values=255*fractions;
    NightAccumulator accumulator;std::wstring message;auto settings=enabled();settings.targetBrightness=192;
    require(accumulator.prepare(512,150,message)&&accumulator.begin(settings),"Averaged ramp prepare failed");
    auto sample=uniform(512,150,0);
    for(unsigned n=1;n<=NightMaxSamples;++n) {
        for(unsigned i=0;i<values;++i) {
            const uint8_t value=uint8_t(i/fractions+(n<=i%fractions?1:0));
            sample.pixels[size_t(i)*4]=sample.pixels[size_t(i)*4+1]=sample.pixels[size_t(i)*4+2]=value;
        }
        require(accumulator.add(sample,n)==NightAdd::Added,"Averaged ramp sample rejected");
    }
    NightResult result;const auto* image=accumulator.finish(result);
    require(image&&result.samples==NightMaxSamples&&result.appliedGain>1,"Averaged ramp did not exercise boosted temporal means");
    unsigned previous=0;
    for(unsigned i=0;i<values;++i) {
        const auto* p=&image->pixels[size_t(i)*4];
        require(p[0]>=previous&&p[0]==p[1]&&p[1]==p[2]&&p[3]==255,
            "Quantized common-channel gain inverted adjacent averaged gray detail");
        previous=p[0];
        if(i+1<values)require(p[0]<255,"Tone recovery newly clipped an unsaturated temporal mean");
    }
    require(image->pixels[0]==0&&image->pixels[size_t(values-1)*4]==255,"Averaged tone curve lost endpoints");
    std::cout<<"PASS monotonic76755temporal gray means, preserved endpoints and no newly clipped highlights.\n";
}
void measuredNoiseAndDetail() {
    // Compare the filtered output with the exact *same* temporal mean rendered
    // at the *same* gain. A darker output cannot win this quality comparison.
    const auto truth=night_quality::scene();auto sample=truth;
    for(auto noise:{night_quality::Noise::IndependentColor,night_quality::Noise::BlockColor,
                   night_quality::Noise::CorrelatedColor,night_quality::Noise::Luminance}) {
        const unsigned samples=noise==night_quality::Noise::CorrelatedColor?64:16;
        NightAccumulator accumulator;std::wstring message;NightResult result;
        require(accumulator.prepare(truth.width,truth.height,message),"Scientific fixture prepare failed");
        std::vector<uint32_t> sums(truth.pixels.size()/4*3,0);
        allocations=0;countAllocations=true;
        require(accumulator.begin(enabled()),"Scientific fixture begin failed");
        for(unsigned n=0;n<samples;++n) {
            night_quality::noisy(truth,sample,n,noise);
            for(size_t p=0;p<sample.pixels.size()/4;++p)for(unsigned c=0;c<3;++c)sums[p*3+c]+=night_detail::linear(sample.pixels[p*4+c]);
            require(accumulator.add(sample,n+1)==NightAdd::Added,"Scientific fixture add failed");
        }
        const auto* output=accumulator.finish(result);countAllocations=false;
        require(output&&allocations==0,"Noise estimator or active chroma filter allocated");
        const auto expected=night_quality::reference(truth,result.appliedGain,false);
        const auto unfiltered=night_quality::renderedMean(truth,sums,samples,result.appliedGain);
        const auto filteredScore=night_quality::metrics(*output,expected),rawScore=night_quality::metrics(unfiltered,expected);
        require(result.outputBrightness>=93&&result.outputBrightness<=100,"Measured dark detail did not reach its supported target");
        require(filteredScore.lineContrast>=rawScore.lineContrast*.95,"Chroma suppression erased one-pixel luminance detail");
        if(noise==night_quality::Noise::Luminance) {
            require(error(*output,unfiltered)<.1,"Chroma filter changed a scene carrying only luminance noise");
        } else {
            const double improvement=noise==night_quality::Noise::BlockColor?.96:.80;
            require(filteredScore.rgb<rawScore.rgb*improvement&&filteredScore.chroma<rawScore.chroma*improvement,
                "Measured color noise did not improve at identical brightness");
            require(filteredScore.luma<rawScore.luma*1.10,"Chroma suppression increased visible luminance noise");
        }
        std::cout<<"PASS matched-gain noise "<<int(noise)<<": RGB MSE "<<filteredScore.rgb<<" vs "<<rawScore.rgb
            <<", chroma "<<filteredScore.chroma<<" vs "<<rawScore.chroma<<", line contrast "<<filteredScore.lineContrast<<" vs "<<rawScore.lineContrast<<".\n";
    }
}
void noisyEndpointsAndFlicker() {
    NightAccumulator accumulator;std::wstring message;NightResult result;
    auto truth=night_quality::scene(128,96),sample=truth;double previous=0;
    require(accumulator.prepare(truth.width,truth.height,message),"Noisy endpoints prepare failed");
    const size_t black=(size_t(32)*truth.width+64)*4,white=black+4,nearWhite=black+8;
    for(unsigned window=0;window<12;++window) {
        require(accumulator.begin(enabled()),"Noisy flicker begin failed");
        for(unsigned n=0;n<16;++n) {
            night_quality::noisy(truth,sample,n+window*37,night_quality::Noise::IndependentColor);
            for(unsigned c=0;c<3;++c){sample.pixels[black+c]=0;sample.pixels[white+c]=255;sample.pixels[nearWhite+c]=254;}
            require(accumulator.add(sample,n+1)==NightAdd::Added,"Noisy endpoints add failed");
        }
        const auto* image=accumulator.finish(result);require(image,"Noisy endpoint finish failed");
        for(unsigned c=0;c<3;++c)require(image->pixels[black+c]==0&&image->pixels[white+c]==255&&image->pixels[nearWhite+c]<255,
            "Active chroma reconstruction changed black/white endpoints or newly clipped a highlight");
        if(window)require(std::abs(result.outputBrightness-previous)<=2,"Stationary stochastic scene caused exposure flicker");
        previous=result.outputBrightness;
    }
    std::cout<<"PASS active-filter black/white endpoints, unsaturated highlights and12 independent noisy-window exposure stability.\n";
}
void correlatedEvidenceAndColorEdges() {
    // Repeating each noisy observation eight times gives far less independent
    // evidence even though all source sequence numbers are distinct.
    auto truth=uniform(128,96,8),sample=truth;double gains[2]{},brightness[2]{};
    for(unsigned held=0;held<2;++held) {
        NightAccumulator accumulator;std::wstring message;NightResult result;
        require(accumulator.prepare(128,96,message)&&accumulator.begin(enabled()),"Correlated confidence prepare failed");
        for(unsigned n=0;n<64;++n){night_quality::noisy(truth,sample,n,held?night_quality::Noise::CorrelatedColor:night_quality::Noise::IndependentColor);accumulator.add(sample,n+1);}
        require(accumulator.finish(result)&&result.samples==64,"Correlated confidence sample count changed");
        gains[held]=result.appliedGain;brightness[held]=result.outputBrightness;
    }
    require(gains[0]>40&&brightness[0]>=90&&gains[1]<20&&brightness[1]<=60&&gains[0]>gains[1]*2.5,
        "Held noisy observations were treated as equally strong independent evidence");

    // These two colors have virtually equal encoded guide luminance (8552 vs
    // 8557 before division by256). Luma guidance alone cannot protect this edge.
    for(int y=0;y<truth.height;++y)for(int x=0;x<truth.width;++x) {
        auto* p=&truth.pixels[(size_t(y)*truth.width+x)*4];p[0]=uint8_t(x<64?12:52);p[1]=28;p[2]=uint8_t(x<64?52:37);
    }
    NightAccumulator accumulator;std::wstring message;NightResult result;
    require(accumulator.prepare(128,96,message)&&accumulator.begin(enabled()),"Color-edge prepare failed");
    std::vector<uint32_t> sums(truth.pixels.size()/4*3,0);
    for(unsigned n=0;n<16;++n) {
        night_quality::noisy(truth,sample,n,night_quality::Noise::IndependentColor);
        for(size_t p=0;p<sample.pixels.size()/4;++p)for(unsigned c=0;c<3;++c)sums[p*3+c]+=night_detail::linear(sample.pixels[p*4+c]);
        accumulator.add(sample,n+1);
    }
    const auto* output=accumulator.finish(result);require(output,"Color-edge finish failed");
    const auto raw=night_quality::renderedMean(truth,sums,16,result.appliedGain);
    const auto contrast=[](const Frame& image) {
        double sum=0;
        for(int y=3;y<image.height-3;++y) {
            const auto* left=&image.pixels[(size_t(y)*image.width+63)*4];const auto* right=left+4;
            sum+=int(left[2])-left[0]-int(right[2])+right[0];
        }
        return sum/(image.height-6);
    };
    require(contrast(*output)>=contrast(raw)*.95,"Noise filtering bled a same-luminance color boundary");
    std::cout<<"PASS independent/held64-sample confidence (gains "<<gains[0]<<"/"<<gains[1]<<") and equal-luminance color-edge contrast "<<contrast(*output)<<" vs "<<contrast(raw)<<".\n";
}
void highResolutionStorage() {
    NightAccumulator accumulator; std::wstring message; auto settings=enabled();
    require(accumulator.storageBytes()==0,"Cold accumulator allocated image storage");
    require(accumulator.prepare(1280,720,message)&&accumulator.storageBytes()==14768640,"720p storage changed");
    require(accumulator.prepare(1920,1080,message)&&accumulator.storageBytes()==33212160,"1080p storage exceeds sums+output+three-row guide budget");
    auto input=uniform(1920,1080,96); NightResult facts;
    allocations=0; countAllocations=true;
    require(accumulator.begin(settings)&&accumulator.add(input,1)==NightAdd::Added&&accumulator.add(input,2)==NightAdd::Added,"1080p accumulation failed");
    const auto* output=accumulator.finish(facts);
    countAllocations=false;
    require(output&&output->width==1920&&output->height==1080&&facts.samples==2&&facts.appliedGain==1&&allocations==0,"1080p finish facts or steady allocation changed");
    for(size_t p=0;p<output->pixels.size();p+=4) require(output->pixels[p]==96&&output->pixels[p+1]==96&&output->pixels[p+2]==96&&output->pixels[p+3]==255,"1080p output pixel/alpha mismatch");
    require(!accumulator.prepare(1921,1080,message)&&accumulator.finish(facts)==output,"Over-budget preparation damaged the previous result");
    require(accumulator.prepare(64,36,message)&&accumulator.storageBytes()==38016,"Smaller source retained high-tier image storage");
    std::cout<<"PASS720/1080 exact storage, full output pixels, no warmed allocation and dimension-change retirement.\n";
}
}
void* operator new(size_t size) {if(countAllocations)++allocations;if(void* value=std::malloc(size?size:1))return value;throw std::bad_alloc();}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete(void* value,size_t) noexcept {std::free(value);}
int main() {
    try {contracts();stability();highlightsAndManual();actualNoiseReduction();periodicMetering();lowSignalStability();darkerSignalAndSampling();mixedDetailAndColor();lowSignalSampleJitter();mixedMeterContinuity();motionAverageContract();averagedToneMonotonicity();measuredNoiseAndDetail();noisyEndpointsAndFlicker();correlatedEvidenceAndColorEdges();highResolutionStorage();std::cout<<"All night image contracts passed using synthetic frames.\n";return 0;}
    catch(const std::exception& error){countAllocations=false;std::cerr<<error.what()<<'\n';return 1;}
}

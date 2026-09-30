#include "night.h"
#include <algorithm>
#include <cmath>

namespace lapse {
namespace {
static_assert(uint64_t(NightMaxSamples) * 65535 <= UINT32_MAX, "Night channel sums must remain bounded");
static_assert(uint64_t(NightMaxWidth) * NightMaxHeight * NightMaxSamples * 65535 < UINT64_MAX / 65536,
    "Night full-image totals and luminance weights must remain bounded");
struct Tables {
    std::array<uint16_t,256> forward{};
    std::array<uint8_t,65536> inverse{};
    Tables() noexcept {
        for (unsigned i=0;i<256;++i) {
            const double v=i/255.0;
            const double l=v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);
            forward[i]=static_cast<uint16_t>(std::lround(l*65535));
        }
        for (unsigned i=0;i<65536;++i) {
            const double l=i/65535.0;
            const double v=l<=0.0031308?12.92*l:1.055*std::pow(l,1/2.4)-0.055;
            inverse[i]=static_cast<uint8_t>(std::clamp(std::lround(v*255),0L,255L));
        }
    }
};
const Tables& tables() noexcept { static const Tables value; return value; }
unsigned luminance(unsigned b,unsigned g,unsigned r) noexcept {
    return static_cast<unsigned>((uint64_t(b)*4732+uint64_t(g)*46871+uint64_t(r)*13933+32768)>>16);
}
}
namespace night_detail {
uint16_t linear(uint8_t value) noexcept { return tables().forward[value]; }
uint8_t encoded(unsigned value) noexcept { return tables().inverse[std::min(value,65535u)]; }
}
bool validNightSettings(const NightSettings& s) noexcept {
    return (s.durationMs==0 || (s.durationMs>=NightMinDurationMs && s.durationMs<=NightMaxDurationMs)) &&
        s.targetBrightness>=32 && s.targetBrightness<=192;
}
bool NightAccumulator::prepare(int w,int h,std::wstring& error) {
    error.clear();
    if(w<=0 || h<=0 || w>NightMaxWidth || h>NightMaxHeight) { error=L"Unsupported night frame dimensions.";return false; }
    if(output_.width==w && output_.height==h && output_.valid())return true;
    try {
        std::vector<uint32_t> sums(size_t(w)*h*3);
        Frame output;output.width=w;output.height=h;output.pixels.resize(size_t(w)*h*4);
        sums_.swap(sums);output_=std::move(output);active_=false;finished_=false;resetExposure();
        (void)tables();return true;
    } catch(const std::bad_alloc&) {error=L"Not enough memory for night blending.";return false;}
}
bool NightAccumulator::begin(const NightSettings& settings) noexcept {
    if(!output_.valid() || !validNightSettings(settings))return false;
    settings_=settings;std::fill(sums_.begin(),sums_.end(),0);totals_.fill(0);sequence_=0;count_=0;active_=true;finished_=false;
    return true;
}
NightAdd NightAccumulator::add(const Frame& frame,uint64_t sequence) noexcept {
    if(!active_ || finished_ || !sequence || !frame.valid() || frame.width!=output_.width || frame.height!=output_.height)return NightAdd::Invalid;
    if(sequence<=sequence_)return NightAdd::Duplicate;
    if(count_==NightMaxSamples)return NightAdd::Full;
    const auto& lut=tables().forward;const auto* p=frame.pixels.data();auto* s=sums_.data();
    const size_t pixels=size_t(frame.width)*frame.height;
    if(linear_){
        uint64_t b=0,g=0,r=0;
        for(size_t i=0;i<pixels;++i,p+=4,s+=3){
            const unsigned bv=lut[p[0]],gv=lut[p[1]],rv=lut[p[2]];
            s[0]+=bv;s[1]+=gv;s[2]+=rv;b+=bv;g+=gv;r+=rv;
        }
        totals_[0]+=b;totals_[1]+=g;totals_[2]+=r;
    }
    else for(size_t i=0;i<pixels;++i,p+=4,s+=3){s[0]+=p[0];s[1]+=p[1];s[2]+=p[2];}
    ++count_;sequence_=sequence;return NightAdd::Added;
}
const Frame* NightAccumulator::finish(NightResult& result) noexcept {
    if(!active_ || !count_)return nullptr;
    if(finished_){result=result_;return &output_;}
    const auto& lut=tables().forward;
    unsigned meterCount=0;std::array<unsigned,256> histogram{};
    // Stratified deterministic samples avoid a fixed8-pixel grid aliasing
    // stripes. This sparse histogram only limits highlights; mean metering
    // uses every pixel accumulated above and cannot miss periodic detail.
    for(int by=0;by<output_.height;by+=8)for(int bx=0;bx<output_.width;bx+=8){
        uint32_t hash=uint32_t(by/8)*65537+uint32_t(bx/8)+1;
        hash^=hash>>16;hash*=0x7feb352d;hash^=hash>>15;hash*=0x846ca68b;hash^=hash>>16;
        const int x=bx+std::min(int(hash&7),output_.width-1-bx),y=by+std::min(int((hash>>3)&7),output_.height-1-by);
        const auto* p=&sums_[(size_t(y)*output_.width+x)*3];
        unsigned b=(p[0]+count_/2)/count_,g=(p[1]+count_/2)/count_,r=(p[2]+count_/2)/count_;
        if(!linear_){b=lut[b];g=lut[g];r=lut[r];}
        ++meterCount;++histogram[std::max({b,g,r})>>8];
    }
    const size_t pixels=output_.pixels.size()/4;
    double mean=(double(totals_[0])*4732+double(totals_[1])*46871+double(totals_[2])*13933)/(65536.0*count_*pixels);
    if(!linear_){
        uint64_t meter=0;const auto* p=sums_.data();
        for(size_t i=0;i<pixels;++i,p+=3)meter+=luminance(lut[(p[0]+count_/2)/count_],lut[(p[1]+count_/2)/count_],lut[(p[2]+count_/2)/count_]);
        mean=double(meter)/pixels;
    }
    const double target=lut[settings_.targetBrightness];
    unsigned cumulative=0,percentile=0;const unsigned rank=(meterCount*99+99)/100;
    for(;percentile<255;++percentile){cumulative+=histogram[percentile];if(cumulative>=rank)break;}
    // Keep quantization headroom for 99% of the metered pixel maxima. This is
    // approximate spatial metering, not a guarantee for unmetered highlights.
    const double highlightCap=double(lut[250])/std::max(1u,std::min(65535u,(percentile+1)*256-1));
    // Approach unity continuously near the black floor. A one-code meter
    // fluctuation must not switch weak noisy signal directly from1x to8x.
    const double signalRatio=mean/lut[8];
    const double signalCap=std::clamp(signalRatio*signalRatio,1.0,maxGain_);
    double wanted=std::clamp(target/std::max(1.0,mean),0.25,maxGain_);
    wanted=std::min({wanted,highlightCap,signalCap});
    if(mean<lut[8])wanted=1;
    if(fixed_)gain_=std::clamp(fixedGain_,0.25,8.0);
    else {
        const bool cut=previousMean_>0 && (mean<previousMean_*.4 || mean>previousMean_*2.5);
        if(!metered_ || cut || previousMean_<lut[8])gain_=wanted;
        else if(std::abs(mean*gain_-target)>target*0.05) {
            // Smooth ordinary changes; cut exposure quickly when light rises.
            gain_=std::clamp(wanted,gain_*0.5,gain_*1.25);
        }
        gain_=std::min({gain_,highlightCap,signalCap});
        if(mean<lut[8])gain_=1;
        gain_=std::clamp(gain_,0.25,maxGain_);
    }
    metered_=true;previousMean_=mean;
    const uint32_t gainQ=static_cast<uint32_t>(std::lround(gain_*4096));
    for(unsigned i=0;i<65536;++i)renderLut_[i]=night_detail::encoded(static_cast<unsigned>(std::min<uint64_t>(65535,(uint64_t(i)*gainQ+2048)>>12)));
    auto* d=output_.pixels.data();const auto* s=sums_.data();uint64_t outputB=0,outputG=0,outputR=0;
    for(size_t i=0;i<output_.pixels.size()/4;++i,s+=3,d+=4){
        for(unsigned c=0;c<3;++c){unsigned v=(s[c]+count_/2)/count_;if(!linear_)v=lut[v];d[c]=renderLut_[v];}d[3]=255;
        outputB+=lut[d[0]];outputG+=lut[d[1]];outputR+=lut[d[2]];
    }
    result_.samples=count_;result_.appliedGain=double(gainQ)/4096;
    result_.inputBrightness=night_detail::encoded(static_cast<unsigned>(std::lround(mean)));
    const double finalMean=(double(outputB)*4732+double(outputG)*46871+double(outputR)*13933)/(65536.0*pixels);
    result_.outputBrightness=night_detail::encoded(static_cast<unsigned>(std::lround(finalMean)));
    result_.targetLimited=finalMean<target*.9 || finalMean>target*1.1;
    // A longer average can offset independent noise when gain is raised. This
    // is a bounded suggestion, not a promise of measured scene SNR.
    result_.suggestedDurationMs=std::clamp(static_cast<int>(std::ceil(std::max(1.0,gain_*gain_)*NightCadenceMs/1000))*1000,NightMinDurationMs,NightMaxDurationMs);
    finished_=true;result=result_;return &output_;
}
}

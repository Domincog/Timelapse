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
    const size_t pixels=output_.pixels.size()/4;
    std::array<uint32_t,256> histogram{};
    std::array<uint64_t,256> maxima{},luma{};
    auto* source=sums_.data();
    for(size_t i=0;i<pixels;++i,source+=3){
        unsigned b=(source[0]+count_/2)/count_,g=(source[1]+count_/2)/count_,r=(source[2]+count_/2)/count_;
        if(!linear_){b=lut[b];g=lut[g];r=lut[r];}
        // Normalize the completed sums in place. finish is idempotent, add is
        // rejected afterwards, and begin clears them for the next window.
        // The render pass then needs neither another division nor a buffer.
        source[0]=b;source[1]=g;source[2]=r;
        const unsigned maximum=std::max({b,g,r});
        const unsigned bin=night_detail::encoded(maximum);
        ++histogram[bin];maxima[bin]+=maximum;luma[bin]+=luminance(b,g,r);
    }
    double mean=(double(totals_[0])*4732+double(totals_[1])*46871+double(totals_[2])*13933)/(65536.0*count_*pixels);
    if(!linear_){
        uint64_t meter=0;for(const auto value:luma)meter+=value;
        mean=double(meter)/pixels;
    }
    const double target=lut[settings_.targetBrightness];
    std::array<double,256> meterMax{},meterLuma{};
    double meterPixels=0,meterTotal=0;
    // Smoothly reduce the influence of lamps/screens without a hard spatial
    // mask or percentile threshold. Every pixel contributes, including dark
    // periodic patterns; constant-color scenes retain their exact meter.
    for(unsigned bin=0;bin<256;++bin)if(histogram[bin]){
        meterMax[bin]=double(maxima[bin])/histogram[bin];
        const double relative=meterMax[bin]/lut[96];
        const double weight=1/(1+relative*relative);
        meterPixels+=histogram[bin]*weight;
        meterLuma[bin]=double(luma[bin])*weight;meterTotal+=meterLuma[bin];
    }
    const double exposureMean=meterTotal/meterPixels;
    const auto predicted=[&](double gain) noexcept {
        double total=0;
        for(unsigned bin=0;bin<256;++bin)if(histogram[bin]){
            const double scale=gain<=1?gain:gain/(1+(gain-1)*meterMax[bin]/65535);
            total+=meterLuma[bin]*scale;
        }
        return total/meterPixels;
    };
    // Extra contributions justify a bounded increase in shadow gain and a
    // lower noise floor. This assumes partly independent noise, not measured
    // SNR: an absolute floor remains and single-frame safeguards stay intact.
    const double supportedGain=std::min(maxGain_,8*std::sqrt(double(count_)));
    const double signalFloor=lut[8]/std::sqrt(double(std::min(count_,16u)));
    const double signalRatio=exposureMean/signalFloor;
    const double signalCap=std::clamp(signalRatio*signalRatio,1.0,supportedGain);
    double lower=0.25,upper=supportedGain;
    for(unsigned step=0;step<20;++step){
        const double middle=(lower+upper)*0.5;
        if(predicted(middle)<target)lower=middle;else upper=middle;
    }
    double wanted=std::min((lower+upper)*0.5,signalCap);
    if(exposureMean<signalFloor)wanted=1;
    if(fixed_)gain_=std::clamp(fixedGain_,0.25,8.0);
    else {
        const bool cut=previousMean_>0 && (exposureMean<previousMean_*.4 || exposureMean>previousMean_*2.5);
        if(!metered_ || cut || previousMean_<signalFloor)gain_=wanted;
        else if(std::abs(predicted(gain_)-target)>target*0.05) {
            // Smooth ordinary changes; cut exposure quickly when light rises.
            gain_=std::clamp(wanted,gain_*0.5,gain_*1.25);
        }
        gain_=std::min(gain_,signalCap);
        if(exposureMean<signalFloor)gain_=1;
        gain_=std::clamp(gain_,0.25,supportedGain);
    }
    metered_=true;previousMean_=exposureMean;
    const uint32_t gainQ=static_cast<uint32_t>(std::lround(gain_*4096));
    const double appliedGain=double(gainQ)/4096;
    // White-anchored monotonic curve: g*x/(1+(g-1)*x). Store mapped maxima,
    // not quantized scales: scale rounding can reverse adjacent intensities.
    // Applying one exact ratio to all channels preserves linear RGB ratios.
    for(unsigned i=0;i<65536;++i){
        const double scale=appliedGain<=1||fixed_?appliedGain:appliedGain/(1+(appliedGain-1)*i/65535);
        toneLut_[i]=static_cast<uint16_t>(std::lround(std::min(65535.0,i*scale)));
    }
    auto* d=output_.pixels.data();const auto* s=sums_.data();uint64_t outputB=0,outputG=0,outputR=0;
    const bool linearTone=appliedGain<=1||fixed_;
    for(size_t i=0;i<output_.pixels.size()/4;++i,s+=3,d+=4){
        if(linearTone){
            for(unsigned c=0;c<3;++c)d[c]=night_detail::encoded(toneLut_[s[c]]);
        } else {
            const unsigned maximum=std::max({s[0],s[1],s[2]});
            const uint32_t mapped=toneLut_[maximum];
            const uint8_t ceiling=maximum<65535?254:255;
            // 65535*65535+65535/2 fits uint32_t; the maximum channel needs
            // no division, and zero (including an all-black pixel) stays zero.
            for(unsigned c=0;c<3;++c){
                const unsigned value=s[c]==maximum?mapped:(s[c]*mapped+maximum/2)/maximum;
                d[c]=std::min(ceiling,night_detail::encoded(value));
            }
        }
        d[3]=255;
        outputB+=lut[d[0]];outputG+=lut[d[1]];outputR+=lut[d[2]];
    }
    result_.samples=count_;result_.appliedGain=double(gainQ)/4096;
    result_.inputBrightness=night_detail::encoded(static_cast<unsigned>(std::lround(mean)));
    const double finalMean=(double(outputB)*4732+double(outputG)*46871+double(outputR)*13933)/(65536.0*pixels);
    result_.outputBrightness=night_detail::encoded(static_cast<unsigned>(std::lround(finalMean)));
    result_.targetLimited=finalMean<target*.9 || finalMean>target*1.1;
    // Base collection on illumination demand, even when weak signal limits
    // displayed gain. The darkest scenes must not ask for the shortest window.
    const double durationGain=std::clamp(target/std::max(1.0,exposureMean),1.0,
        std::sqrt(double(NightMaxDurationMs)/NightCadenceMs));
    result_.suggestedDurationMs=std::clamp(static_cast<int>(std::ceil(durationGain*durationGain*NightCadenceMs/1000))*1000,NightMinDurationMs,NightMaxDurationMs);
    finished_=true;result=result_;return &output_;
}
}

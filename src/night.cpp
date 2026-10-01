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
unsigned codeLuminance(const uint8_t* p) noexcept {
    return (unsigned(p[0])*29+unsigned(p[1])*150+unsigned(p[2])*77+128)>>8;
}
uint32_t scramble(uint32_t value) noexcept {
    value^=value>>16;value*=0x7feb352d;value^=value>>15;value*=0x846ca68b;return value^(value>>16);
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
        std::vector<ChromaPixel> rows(size_t(w)*3);
        Frame output;output.width=w;output.height=h;output.pixels.resize(size_t(w)*h*4);
        sums_.swap(sums);filterRows_.swap(rows);output_=std::move(output);active_=false;finished_=false;resetExposure();
        probeCount_=0;
        const unsigned columns=std::min(w,32),lines=std::min(h,32);
        for(unsigned y=0;y<lines;++y)for(unsigned x=0;x<columns;++x) {
            const unsigned left=x*w/columns,right=(x+1)*w/columns,top=y*h/lines,bottom=(y+1)*h/lines;
            const unsigned px=left+scramble(x+y*columns+1)%(right-left);
            const unsigned py=top+scramble(x+y*columns+12345)%(bottom-top);
            probes_[probeCount_++].pixel=size_t(py)*w+px;
        }
        (void)tables();return true;
    } catch(const std::bad_alloc&) {error=L"Not enough memory for night blending.";return false;}
}
bool NightAccumulator::begin(const NightSettings& settings) noexcept {
    if(!output_.valid() || !validNightSettings(settings))return false;
    settings_=settings;std::fill(sums_.begin(),sums_.end(),0);totals_.fill(0);sequence_=0;count_=0;active_=true;finished_=false;
    for(unsigned i=0;i<probeCount_;++i) {const size_t pixel=probes_[i].pixel;probes_[i]={};probes_[i].pixel=pixel;}
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
    const double divisor=1.0/(count_+1);
    for(unsigned i=0;i<probeCount_;++i) {
        auto& probe=probes_[i];const auto* pixel=frame.pixels.data()+probe.pixel*4;
        const double value=luminance(lut[pixel[0]],lut[pixel[1]],lut[pixel[2]]);
        const double delta=value-probe.mean;probe.mean+=delta*divisor;probe.variance+=delta*(value-probe.mean);
        if(count_) {const double change=value-probe.previous;probe.difference+=change*change;}
        probe.previous=value;
        const double light=codeLuminance(pixel),blue=pixel[0]-light,red=pixel[2]-light;
        const double bd=blue-probe.blueMean,rd=red-probe.redMean;
        probe.blueMean+=bd*divisor;probe.redMean+=rd*divisor;
        probe.blueVariance+=bd*(blue-probe.blueMean);probe.redVariance+=rd*(red-probe.redMean);
    }
    ++count_;sequence_=sequence;return NightAdd::Added;
}
NightAccumulator::NoiseEstimate NightAccumulator::estimateNoise() const noexcept {
    NoiseEstimate result;
    if(count_<3 || !probeCount_)return result;
    std::array<double,32*32> variances{};unsigned used=0;
    // Meter the shadows. Trim the most variable quarter so a small moving
    // subject does not make the entire stationary scene appear noisy.
    for(unsigned i=0;i<probeCount_;++i)if(probes_[i].mean<=tables().forward[128])
        variances[used++]=probes_[i].variance;
    if(!used)return result;
    std::sort(variances.begin(),variances.begin()+used);
    const double limit=variances[(used-1)*3/4];
    double variance=0,difference=0,chroma=0,light=0;unsigned accepted=0;
    for(unsigned i=0;i<probeCount_;++i) {
        const auto& probe=probes_[i];
        if(probe.mean>tables().forward[128] || probe.variance>limit)continue;
        variance+=probe.variance;difference+=probe.difference;
        chroma+=(probe.blueVariance+probe.redVariance)*0.5;light+=probe.mean;++accepted;
    }
    if(!accepted)return result;
    // Consecutive differences have variance 2*sigma^2*(1-rho). Positive
    // correlation (camera temporal denoising, slow drift) reduces independent
    // evidence. The bounded AR(1) inflation is deliberately conservative;
    // this is a noise proxy for delivered RGB, not a calibrated sensor SNR.
    const double correlation=variance>0?std::clamp(1-difference/(2*variance),0.0,0.8):0;
    const double inflation=(1+correlation)/(1-correlation);
    const double effective=std::max(1.0,count_/inflation);
    const double divisor=double(accepted)*(count_-1)*effective;
    result.linearError=std::sqrt(std::max(0.0,variance/divisor));
    result.chromaError=std::sqrt(std::max(0.0,chroma/divisor));
    result.chromaLevel=night_detail::encoded(static_cast<unsigned>(std::lround(light/accepted)));
    return result;
}
void NightAccumulator::filterChroma(double noise) noexcept {
    if(noise<=1.25)return;
    // A small joint bilateral chroma filter operates only on the completed
    // blend. Its guide preserves luminance detail; a chroma range term also
    // protects color boundaries with equal luminance. The three-row ring keeps
    // all reads on original pixels. Precomputed guides avoid repeating color
    // conversion for every neighbor (33.75 KiB at the maximum width).
    const double strength=std::min(0.85,(noise*noise-1.25*1.25)/(noise*noise+4));
    const double lumaScale=std::max(3.0,noise*1.5),chromaScale=std::max(4.0,noise*3);
    std::array<unsigned,256> lumaWeights{};
    std::array<unsigned,511> chromaWeights{};
    for(unsigned i=0;i<lumaWeights.size();++i)lumaWeights[i]=static_cast<unsigned>(std::lround(256/(1+i*i/(lumaScale*lumaScale))));
    for(unsigned i=0;i<chromaWeights.size();++i)chromaWeights[i]=static_cast<unsigned>(std::lround(256/(1+i*i/(chromaScale*chromaScale))));
    const int width=output_.width,height=output_.height;const size_t rowBytes=size_t(width)*4;
    const auto loadRow=[&](int y) noexcept {
        auto* row=filterRows_.data()+size_t(y%3)*width;const auto* source=output_.pixels.data()+size_t(y)*rowBytes;
        for(int x=0;x<width;++x,source+=4) {
            const int light=int(codeLuminance(source));
            row[x]={static_cast<uint8_t>(light),source[1],static_cast<int16_t>(int(source[0])-light),static_cast<int16_t>(int(source[2])-light)};
        }
    };
    const auto rounded=[](double value,unsigned ceiling) noexcept {
        return static_cast<uint8_t>(std::clamp(value,0.0,double(ceiling))+0.5);
    };
    loadRow(0);
    for(int y=0;y<height;++y) {
        if(y+1<height)loadRow(y+1);
        const auto* top=filterRows_.data()+size_t(std::max(0,y-1)%3)*width;
        const auto* middle=filterRows_.data()+size_t(y%3)*width;
        const auto* bottom=filterRows_.data()+size_t(std::min(height-1,y+1)%3)*width;
        for(int x=0;x<width;++x) {
            const auto& center=middle[x];const int light=center.light,blue=center.blue,red=center.red;
            const unsigned maximum=std::max({light+blue,int(center.green),light+red});
            if(!maximum || (light+blue==255 && center.green==255 && light+red==255))continue;
            const unsigned ceiling=maximum<255?254:255;
            // Kernel weights sum to16 and range weights are <=256 each:
            // channel sums are bounded by16*256*256*255 < INT32_MAX.
            int32_t blueSum=0,redSum=0;uint32_t weights=0;
            const auto contribute=[&](const ChromaPixel& pixel,unsigned spatial) noexcept {
                const unsigned cd=static_cast<unsigned>(std::max(std::abs(pixel.blue-blue),std::abs(pixel.red-red)));
                const unsigned weight=spatial*lumaWeights[std::abs(pixel.light-light)]*chromaWeights[cd];
                blueSum+=int32_t(weight)*pixel.blue;redSum+=int32_t(weight)*pixel.red;weights+=weight;
            };
            const int left=std::max(0,x-1),right=std::min(width-1,x+1);
            contribute(top[left],1);contribute(top[x],2);contribute(top[right],1);
            contribute(middle[left],2);contribute(center,4);contribute(middle[right],2);
            contribute(bottom[left],1);contribute(bottom[x],2);contribute(bottom[right],1);
            const double b=blue+strength*(double(blueSum)/weights-blue),r=red+strength*(double(redSum)/weights-red);
            auto* destination=output_.pixels.data()+size_t(y)*rowBytes+size_t(x)*4;
            destination[0]=rounded(light+b,ceiling);
            destination[2]=rounded(light+r,ceiling);
            // Apply the chroma change to the original green value instead of
            // reconstructing it from rounded luma: infinitesimal filtering
            // must not create a one-code green offset throughout the image.
            destination[1]=rounded(center.green-(29*(b-blue)+77*(r-red))/150,ceiling);
        }
    }
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
    // Extra contributions justify a bounded increase in shadow gain. Measured
    // temporal uncertainty raises the floor for noisy or correlated sources;
    // an absolute low-code floor and single-frame safeguards remain.
    const double supportedGain=std::min(maxGain_,8*std::sqrt(double(count_)));
    const auto noise=estimateNoise();
    const double signalFloor=std::max(lut[8]/std::sqrt(double(std::min(count_,64u))),2*noise.linearError);
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
    const unsigned noiseLevel=static_cast<unsigned>(noise.chromaLevel);
    const unsigned low=noiseLevel>4?noiseLevel-4:0,high=std::min(noiseLevel+4,255u);
    const double noiseScale=double(night_detail::encoded(toneLut_[lut[high]])-night_detail::encoded(toneLut_[lut[low]]))/(high-low);
    const double renderedNoise=noise.chromaError*noiseScale;
    filterChroma(renderedNoise);
    if(renderedNoise>1.25) {
        // Report the delivered pixels, including the small rounding/clipping
        // differences from chroma reconstruction.
        outputB=outputG=outputR=0;
        for(size_t i=0;i<output_.pixels.size();i+=4) {outputB+=lut[output_.pixels[i]];outputG+=lut[output_.pixels[i+1]];outputR+=lut[output_.pixels[i+2]];}
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

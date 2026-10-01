#pragma once
// Deterministic, synthetic delivered-camera frames. No device or profile I/O.
#include "night.h"
#include <algorithm>
#include <cmath>

namespace night_quality {
inline uint32_t hash(uint32_t x) noexcept {
    x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16);
}
inline lapse::Frame scene(int w=256,int h=160) {
    lapse::Frame f;f.width=w;f.height=h;f.pixels.resize(size_t(w)*h*4);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        auto* p=&f.pixels[(size_t(y)*w+x)*4];
        // Broad texture and one-pixel lines expose spatial over-smoothing.
        const int v=y<h*3/4?12+8*((x/16+y/16)&1):16+8*(x&1);
        p[0]=p[1]=p[2]=uint8_t(v);
        if(y>=h/4&&y<h/2) {p[0]=uint8_t(x<w/2?10:32);p[1]=20;p[2]=uint8_t(x<w/2?32:10);}
        if(x<w/16&&y<h/16)p[0]=p[1]=p[2]=232;
        p[3]=255;
    }
    return f;
}
enum class Noise { IndependentColor, CorrelatedColor, BlockColor, Luminance };
inline void noisy(const lapse::Frame& truth,lapse::Frame& sample,unsigned n,Noise type) {
    const unsigned phase=type==Noise::CorrelatedColor?n/8:n;
    for(int y=0;y<truth.height;++y)for(int x=0;x<truth.width;++x) {
        const size_t p=(size_t(y)*truth.width+x)*4;
        const unsigned coordinate=type==Noise::BlockColor?unsigned((y/4)*(truth.width/4)+x/4):unsigned(p/4);
        for(unsigned c=0;c<3;++c) {
            const unsigned channel=type==Noise::Luminance?0:c;
            const uint32_t seed=coordinate*104729u+phase*130363u+channel*65537u;
            // Two independent discrete uniform terms: zero mean, sigma sqrt(28).
            const int noise=int(hash(seed)%13)+int(hash(seed+0x9e3779b9u)%13)-12;
            sample.pixels[p+c]=uint8_t(std::clamp(int(truth.pixels[p+c])+noise,0,255));
        }
        sample.pixels[p+3]=255;
    }
}
inline lapse::Frame reference(const lapse::Frame& truth,double gain,bool fixed) {
    auto result=truth;
    for(size_t p=0;p<result.pixels.size();p+=4) {
        unsigned v[3];for(unsigned c=0;c<3;++c)v[c]=lapse::night_detail::linear(truth.pixels[p+c]);
        const unsigned maximum=std::max({v[0],v[1],v[2]});
        const double scale=gain<=1||fixed?gain:gain/(1+(gain-1)*maximum/65535.0);
        for(unsigned c=0;c<3;++c)result.pixels[p+c]=lapse::night_detail::encoded(unsigned(std::lround(std::min(65535.0,v[c]*scale))));
    }
    return result;
}
inline lapse::Frame renderedMean(const lapse::Frame& shape,const std::vector<uint32_t>& sums,unsigned count,double gain) {
    auto result=shape;
    for(size_t p=0;p<result.pixels.size()/4;++p) {
        unsigned v[3];for(unsigned c=0;c<3;++c)v[c]=(sums[p*3+c]+count/2)/count;
        const unsigned maximum=std::max({v[0],v[1],v[2]});
        const double scale=gain<=1?gain:gain/(1+(gain-1)*maximum/65535.0);
        const unsigned mapped=unsigned(std::lround(std::min(65535.0,maximum*scale)));
        for(unsigned c=0;c<3;++c)result.pixels[p*4+c]=lapse::night_detail::encoded(v[c]==maximum?mapped:(v[c]*mapped+maximum/2)/maximum);
    }
    return result;
}
struct Metrics {double rgb=0,luma=0,chroma=0,lineContrast=0;};
inline Metrics metrics(const lapse::Frame& output,const lapse::Frame& reference) {
    Metrics m;double even=0,odd=0;unsigned evenN=0,oddN=0;
    for(int y=0;y<output.height;++y)for(int x=0;x<output.width;++x) {
        const size_t p=(size_t(y)*output.width+x)*4;
        double d[3];for(unsigned c=0;c<3;++c){d[c]=double(output.pixels[p+c])-reference.pixels[p+c];m.rgb+=d[c]*d[c];}
        const double l=.0722*d[0]+.7152*d[1]+.2126*d[2];m.luma+=l*l;
        m.chroma+=((d[0]-d[1])*(d[0]-d[1])+(d[2]-d[1])*(d[2]-d[1]))*.5;
        if(y>=output.height*3/4+3&&y+3<output.height&&x>=3&&x+3<output.width) {
            const double value=.0722*output.pixels[p]+.7152*output.pixels[p+1]+.2126*output.pixels[p+2];
            if(x&1){odd+=value;++oddN;}else{even+=value;++evenN;}
        }
    }
    const double pixels=double(output.width)*output.height;
    m.rgb/=pixels*3;m.luma/=pixels;m.chroma/=pixels;
    m.lineContrast=odd/oddN-even/evenN;
    return m;
}
}

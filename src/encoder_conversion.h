#pragma once
#include "core.h"
#include <emmintrin.h>
#include <cstring>

namespace lapse { namespace encoding_detail {
// SSE2 is part of the Windows x64 baseline. Coefficients and rounding match
// the original scalar BT.709 limited-range conversion exactly, including the
// four-pixel chroma average. Each iteration reads exactly two pixels per row.
inline void toNv12(const Frame& frame, BYTE* target) {
    const size_t width=static_cast<size_t>(frame.width),height=static_cast<size_t>(frame.height);
    BYTE* uv=target+width*height;
    const auto zero=_mm_setzero_si128();
    const auto yWeights=_mm_setr_epi16(2032,20127,5983,0,2032,20127,5983,0);
    const auto uWeights=_mm_setr_epi16(28784,-22189,-6596,0,0,0,0,0);
    const auto vWeights=_mm_setr_epi16(-2639,-26145,28784,0,0,0,0,0);
    const auto yRound=_mm_set1_epi32(16384),yOffset=_mm_set1_epi32(16);
    const auto uvRound=_mm_set1_epi32(128*262144+131072);
    const auto luma=[&](__m128i row,BYTE* destination) {
        auto sum=_mm_madd_epi16(row,yWeights);
        sum=_mm_add_epi32(sum,_mm_srli_si128(sum,4));
        sum=_mm_add_epi32(_mm_srli_epi32(_mm_add_epi32(sum,yRound),15),yOffset);
        sum=_mm_shuffle_epi32(sum,_MM_SHUFFLE(2,2,2,0));
        sum=_mm_packs_epi32(sum,zero);
        sum=_mm_packus_epi16(sum,zero);
        const uint16_t packed=static_cast<uint16_t>(_mm_cvtsi128_si32(sum));
        std::memcpy(destination,&packed,sizeof(packed));
    };
    for(size_t y=0;y<height;y+=2)for(size_t x=0;x<width;x+=2) {
        const size_t at=y*width+x;
        const auto top=_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(frame.pixels.data()+at*4)),zero);
        const auto bottom=_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(frame.pixels.data()+(at+width)*4)),zero);
        luma(top,target+at);luma(bottom,target+at+width);
        auto channels=_mm_add_epi16(top,bottom);
        channels=_mm_add_epi16(channels,_mm_srli_si128(channels,8));
        auto u=_mm_madd_epi16(channels,uWeights),v=_mm_madd_epi16(channels,vWeights);
        u=_mm_add_epi32(u,_mm_srli_si128(u,4));v=_mm_add_epi32(v,_mm_srli_si128(v,4));
        auto pair=_mm_unpacklo_epi32(u,v);
        pair=_mm_srli_epi32(_mm_add_epi32(pair,uvRound),18);
        pair=_mm_packs_epi32(pair,zero);pair=_mm_packus_epi16(pair,zero);
        const uint16_t packed=static_cast<uint16_t>(_mm_cvtsi128_si32(pair));
        std::memcpy(uv+(y/2)*width+x,&packed,sizeof(packed));
    }
}
// AV1 consumes planar Y/U/V. Convert straight into that layout, four pixels
// per row at a time, with the same integer coefficients/rounding as toNv12.
// The two-pixel tail never reads past a row for custom even dimensions.
template<bool fourPixels>
inline void i420Block(const BYTE* top, const BYTE* bottom, BYTE* yTop, BYTE* yBottom,
                      BYTE* cb, BYTE* cr) {
    const auto zero=_mm_setzero_si128();
    const auto yWeights=_mm_setr_epi16(2032,20127,5983,0,2032,20127,5983,0);
    const auto uWeights=_mm_setr_epi16(28784,-22189,-6596,0,0,0,0,0);
    const auto vWeights=_mm_setr_epi16(-2639,-26145,28784,0,0,0,0,0);
    const auto load=[](const BYTE* pixels) {
        if constexpr(fourPixels) return _mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels));
        else return _mm_loadl_epi64(reinterpret_cast<const __m128i*>(pixels));
    };
    const auto topBytes=load(top),bottomBytes=load(bottom);
    const auto topLo=_mm_unpacklo_epi8(topBytes,zero),bottomLo=_mm_unpacklo_epi8(bottomBytes,zero);
    const auto topHi=_mm_unpackhi_epi8(topBytes,zero),bottomHi=_mm_unpackhi_epi8(bottomBytes,zero);
    const auto luma=[&](__m128i lo,__m128i hi,BYTE* destination) {
        auto a=_mm_madd_epi16(lo,yWeights),b=_mm_madd_epi16(hi,yWeights);
        a=_mm_add_epi32(a,_mm_srli_si128(a,4));b=_mm_add_epi32(b,_mm_srli_si128(b,4));
        a=_mm_shuffle_epi32(a,_MM_SHUFFLE(2,2,2,0));b=_mm_shuffle_epi32(b,_MM_SHUFFLE(2,2,2,0));
        auto sum=_mm_unpacklo_epi64(a,b);
        sum=_mm_add_epi32(_mm_srli_epi32(_mm_add_epi32(sum,_mm_set1_epi32(16384)),15),_mm_set1_epi32(16));
        sum=_mm_packus_epi16(_mm_packs_epi32(sum,zero),zero);
        const uint32_t packed=static_cast<uint32_t>(_mm_cvtsi128_si32(sum));
        std::memcpy(destination,&packed,fourPixels?4:2);
    };
    luma(topLo,topHi,yTop);luma(bottomLo,bottomHi,yBottom);
    const auto chroma=[&](__m128i a,__m128i b) {
        auto channels=_mm_add_epi16(a,b);
        channels=_mm_add_epi16(channels,_mm_srli_si128(channels,8));
        auto u=_mm_madd_epi16(channels,uWeights),v=_mm_madd_epi16(channels,vWeights);
        u=_mm_add_epi32(u,_mm_srli_si128(u,4));v=_mm_add_epi32(v,_mm_srli_si128(v,4));
        return _mm_unpacklo_epi32(u,v);
    };
    auto pair=_mm_unpacklo_epi64(chroma(topLo,bottomLo),chroma(topHi,bottomHi));
    pair=_mm_srli_epi32(_mm_add_epi32(pair,_mm_set1_epi32(128*262144+131072)),18);
    pair=_mm_packus_epi16(_mm_packs_epi32(pair,zero),zero);
    const uint32_t packed=static_cast<uint32_t>(_mm_cvtsi128_si32(pair));
    const uint16_t u=static_cast<uint16_t>((packed&0xff)|((packed>>8)&0xff00));
    const uint16_t v=static_cast<uint16_t>(((packed>>8)&0xff)|((packed>>16)&0xff00));
    std::memcpy(cb,&u,fourPixels?2:1);std::memcpy(cr,&v,fourPixels?2:1);
}
inline void toI420(const Frame& frame, BYTE* target) {
    const size_t width=static_cast<size_t>(frame.width),height=static_cast<size_t>(frame.height);
    BYTE* cb=target+width*height;
    BYTE* cr=cb+width*height/4;
    for(size_t y=0;y<height;y+=2) {
        size_t x=0;
        for(;x+4<=width;x+=4) {
            const size_t at=y*width+x,chroma=(y/2)*(width/2)+x/2;
            i420Block<true>(frame.pixels.data()+at*4,frame.pixels.data()+(at+width)*4,
                            target+at,target+at+width,cb+chroma,cr+chroma);
        }
        if(x<width) {
            const size_t at=y*width+x,chroma=(y/2)*(width/2)+x/2;
            i420Block<false>(frame.pixels.data()+at*4,frame.pixels.data()+(at+width)*4,
                             target+at,target+at+width,cb+chroma,cr+chroma);
        }
    }
}
}}

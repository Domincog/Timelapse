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
}}

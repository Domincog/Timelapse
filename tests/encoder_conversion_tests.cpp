#include "encoder_conversion.h"
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <cmath>

void scalar(const lapse::Frame& frame,BYTE* target) {
    const size_t width=frame.width,height=frame.height;
    BYTE* uv=target+width*height;
    for(size_t y=0;y<height;y+=2)for(size_t x=0;x<width;x+=2){
        int red=0,green=0,blue=0;
        for(size_t dy=0;dy<2;++dy)for(size_t dx=0;dx<2;++dx){
            const size_t index=(y+dy)*width+x+dx;
            const BYTE* pixel=frame.pixels.data()+index*4;
            const int b=pixel[0],g=pixel[1],r=pixel[2];
            target[index]=static_cast<BYTE>(16+(11966*r+40254*g+4064*b+32768)/65536);
            red+=r;green+=g;blue+=b;
        }
        const size_t chroma=(y/2)*width+x;
        uv[chroma]=static_cast<BYTE>((128*262144-6596*red-22189*green+28784*blue+131072)/262144);
        uv[chroma+1]=static_cast<BYTE>((128*262144+28784*red-26145*green-2639*blue+131072)/262144);
    }
}
uint32_t randomWord(uint32_t& state){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
double cpuMs(){FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);return (double((uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)+double((uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime))/10000.;}
void verify(const lapse::Frame& frame){
    const size_t size=size_t(frame.width)*frame.height*3/2;
    std::vector<BYTE> a(size+32,0x6d),b(size+32,0x6d);scalar(frame,a.data()+16);lapse::encoding_detail::toNv12(frame,b.data()+16);
    if(a!=b)throw std::runtime_error("SSE2 changed output bytes or output bounds");
}
int main(int argc,char**){
    try{
        uint32_t random=0x63fe372a;
        lapse::Frame frame;
        for(const auto dimensions:{std::pair<int,int>{2,2},{16,16},{18,30},{30,18},{34,66},{1280,720},{1920,1080}}){
            frame.width=dimensions.first;frame.height=dimensions.second;frame.pixels.resize(size_t(frame.width)*frame.height*4);
            for(int pass=0;pass<5;++pass){for(auto& byte:frame.pixels)byte=pass<2?BYTE(pass*255):BYTE(randomWord(random));verify(frame);}
        }
        // Every possible RGB triplet in uniform 2x2 blocks checks all rounding
        // boundaries; random mixed blocks above also verify chroma averaging.
        frame.width=512;frame.height=512;frame.pixels.resize(512*512*4);
        for(uint32_t base=0;base<0x1000000;base+=65536){
            for(uint32_t i=0;i<65536;++i){const uint32_t value=base+i;const size_t at=size_t(i/256*2)*512+i%256*2;
                for(size_t dy=0;dy<2;++dy)for(size_t dx=0;dx<2;++dx){auto* pixel=frame.pixels.data()+(at+dy*512+dx)*4;pixel[0]=BYTE(value);pixel[1]=BYTE(value>>8);pixel[2]=BYTE(value>>16);pixel[3]=BYTE(value>>12);}}
            verify(frame);
        }
        std::cout<<"PASS byte-identical for all RGB colors, mixed 2x2 blocks, all alpha, even dimensions, and guarded output\n";
        if(argc>1){
            frame.width=1920;frame.height=1080;frame.pixels.resize(1920*1080*4);for(auto& byte:frame.pixels)byte=BYTE(randomWord(random));
            std::vector<BYTE> output(1920*1080*3/2);constexpr int repeats=500;volatile unsigned checksum=0;
            for(int round=0;round<4;++round){const bool simd=round%2!=0;const auto start=std::chrono::steady_clock::now();const double cpu=cpuMs();
                for(int i=0;i<repeats;++i){if(simd)lapse::encoding_detail::toNv12(frame,output.data());else scalar(frame,output.data());checksum+=output[size_t(i)%output.size()];}
                std::cout<<(simd?"sse2":"scalar")<<",cpu_ms_per_frame="<<(cpuMs()-cpu)/repeats<<",wall_ms_per_frame="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/repeats<<'\n';}
            std::cout<<"checksum="<<checksum<<'\n';
        }
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

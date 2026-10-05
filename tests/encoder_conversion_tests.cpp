#include "encoder_conversion.h"
#include <iostream>
#include <chrono>
#include <algorithm>
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
    std::vector<BYTE> planar(size+32,0x6d),expected=a;
    const size_t luma=size_t(frame.width)*frame.height,plane=luma/4;
    for(size_t i=0;i<plane;++i){expected[16+luma+i]=a[16+luma+2*i];expected[16+luma+plane+i]=a[16+luma+2*i+1];}
    lapse::encoding_detail::toI420(frame,planar.data()+16);
    if(planar!=expected)throw std::runtime_error("Direct I420 changed plane bytes or output bounds");
    std::vector<BYTE> unaligned(size+33,0x6d);lapse::encoding_detail::toI420(frame,unaligned.data()+17);
    if(!std::equal(expected.begin()+16,expected.end()-16,unaligned.begin()+17) ||
       !std::all_of(unaligned.begin(),unaligned.begin()+17,[](BYTE v){return v==0x6d;}) ||
       !std::all_of(unaligned.end()-16,unaligned.end(),[](BYTE v){return v==0x6d;}))
        throw std::runtime_error("Unaligned I420 changed output or bounds");
}
int main(int argc,char**){
    try{
        uint32_t random=0x63fe372a;
        lapse::Frame frame;
        for(const auto dimensions:{std::pair<int,int>{2,2},{4,2},{6,4},{16,16},{18,30},{30,18},{34,66},{1280,720},{1920,1080},{4096,2160}}){
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
        std::cout<<"PASS NV12 and I420 byte-identical for all RGB colors, mixed 2x2 blocks, all alpha, even dimensions, and guarded/unaligned output\n";
        if(argc>1){
            frame.width=1920;frame.height=1080;frame.pixels.resize(1920*1080*4);for(auto& byte:frame.pixels)byte=BYTE(randomWord(random));
            const size_t luma=1920*1080,plane=luma/4;
            std::vector<BYTE> output(luma*3/2),chroma(luma/2);constexpr int repeats=1000;volatile unsigned checksum=0;
            for(int round=0;round<6;++round){const bool direct=round%2!=0;const auto start=std::chrono::steady_clock::now();const double cpu=cpuMs();
                for(int i=0;i<repeats;++i){
                    if(direct)lapse::encoding_detail::toI420(frame,output.data());
                    else{lapse::encoding_detail::toNv12(frame,output.data());for(size_t c=0;c<plane;++c){chroma[c]=output[luma+2*c];chroma[plane+c]=output[luma+2*c+1];}}
                    checksum+=output[size_t(i)%luma]+(direct?output[luma+size_t(i)%plane]:chroma[size_t(i)%plane]);
                }
                std::cout<<(direct?"direct_i420":"nv12_then_planar")<<",cpu_ms_per_frame="<<(cpuMs()-cpu)/repeats<<",wall_ms_per_frame="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/repeats<<'\n';}
            std::cout<<"checksum="<<checksum<<'\n';
        }
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

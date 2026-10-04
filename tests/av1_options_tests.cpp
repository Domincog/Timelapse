#include "av1_encoder.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void encoded(bool value, const std::wstring& error) {
    if (!value) { std::wcerr << error << L'\n'; throw std::runtime_error("SVT-AV1 operation failed"); }
}
void shortClip(int width, int height, int count, const lapse::EncodingOptions& options) {
    std::cout << "Case " << width << 'x' << height << ", frames=" << count << ", preset=" << options.av1Preset
        << ", CRF=" << options.av1Crf << ", rate control=" << int(options.rateControl)
        << ", bitrate=" << options.bitrateKbps << std::endl;
    lapse::Av1Encoder encoder;
    std::wstring error;
    encoded(encoder.open(width,height,30,lapse::EncodingQuality::Balanced,error,options),error);
    require(!encoder.sequenceHeader().empty(),"Missing init-time AV1 sequence header");
    std::vector<uint8_t> nv12(size_t(width)*height*3/2,128);
    lapse::Av1Packet packet;
    bool eos=false;
    encoded(encoder.receive(packet,eos,error),error);
    require(!eos && packet.unit.empty(),"Fresh stream produced a packet without input");
    int received=0;
    auto drain=[&](bool finishing) {
        for (;;) {
            encoded(encoder.receive(packet,eos,error),error);
            if(!packet.unit.empty()) {
                require(packet.pts==received,"Packet timestamps skipped or reordered input");
                require(packet.keyFrame==(received==0),"Unexpected short-clip sync sample");
                ++received;
            }
            if(eos) {require(finishing,"EOS appeared before flush");break;}
            if(packet.unit.empty()) {require(!finishing,"Flush returned without EOS");break;}
        }
    };
    for(int frame=0;frame<count;++frame) {
        for(int y=0;y<height;++y) for(int x=0;x<width;++x)
            nv12[size_t(y)*width+x]=uint8_t(16+(x*3+y*5+frame*23)%219);
        encoded(encoder.encode(nv12.data(),error),error);
        drain(false);
    }
    encoded(encoder.end(error),error);
    encoded(encoder.end(error),error);
    drain(true);
    require(received==count,"Short clip lost a frame retained by lookahead");
    encoded(encoder.receive(packet,eos,error),error);
    require(eos && packet.unit.empty(),"Finished stream produced duplicate data");
    require(!encoder.encode(nv12.data(),error),"Finished stream accepted another frame");
}
}
int main() {
    try {
        lapse::EncodingOptions options;
        options.rateControl=lapse::EncodingRateControl::ConstantQuality;
        options.av1Preset=0; options.av1Crf=1;
        shortClip(48,48,1,options);
        options.av1Preset=11; options.av1Crf=70;
        shortClip(50,66,2,options);
        options.av1Preset=6; options.av1Crf=64;
        shortClip(320,240,2,options);
        options.rateControl=lapse::EncodingRateControl::TargetBitrate;
        options.bitrateKbps=1;
        {
            lapse::Av1Encoder unsupported;
            std::wstring error;
            require(!unsupported.open(48,48,30,lapse::EncodingQuality::Balanced,error,options) &&
                error.find(L"64")!=std::wstring::npos,"Tiny VBR did not explain SVT's AQ restriction");
        }
        shortClip(64,64,1,options);
        options.bitrateKbps=100000;
        shortClip(64,64,1,options);
        std::cout<<"SVT-AV1: preset/CRF/bitrate limits, minimum and nonaligned geometry, short-clip EOS passed.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

#include "encoding_metrics.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>

namespace raw_quality {
inline void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
inline int run(int argc,wchar_t** argv) {
    if(argc!=6){std::cerr<<"Usage: encoding_quality_verifier --raw <nv12_file> <scene:0|1|2|3> <width> <height> <expected_frames>\nTimestamps MUST be checked separately with ffprobe.\n";return 2;}
    try {
        const std::filesystem::path input=argv[1];const int scene=std::stoi(argv[2]),width=std::stoi(argv[3]),height=std::stoi(argv[4]),count=std::stoi(argv[5]);
        require(count>0,"Expected frames must be positive");corpus::Generator generator(scene,width,height);
        const size_t bytes=size_t(width)*height*3/2;
        require(std::filesystem::file_size(input)==bytes*count,"Wrong raw size: frames or dimensions changed");
        std::ifstream reader(input,std::ios::binary);std::vector<uint8_t> pixels(bytes);
        corpus::Quality sum{};double minY=99,minYSsim=1,minTextSsim=1,maxEdge=0,maxRgb=0,yMse=0,uMse=0,vMse=0;
        std::cout<<"frame,y_psnr,u_psnr,v_psnr,block_y_ssim,text_block_ssim,text_edge_mae,text_rgb_mae\n"<<std::fixed<<std::setprecision(7);
        for(int i=0;i<count;++i) {
            reader.read(reinterpret_cast<char*>(pixels.data()),bytes);require(bool(reader),"Cannot read raw frame");
            const auto reference=generator.frame(i);const auto q=corpus::measure(reference,pixels.data(),width,pixels.data()+size_t(width)*height,width,scene<2);
            sum.ySsim+=q.ySsim;sum.textSsim+=q.textSsim;sum.textEdgeMae+=q.textEdgeMae;sum.textRgbMae+=q.textRgbMae;
            yMse+=65025*std::pow(10,-q.yPsnr/10);uMse+=65025*std::pow(10,-q.uPsnr/10);vMse+=65025*std::pow(10,-q.vPsnr/10);
            minY=std::min(minY,q.yPsnr);minYSsim=std::min(minYSsim,q.ySsim);minTextSsim=std::min(minTextSsim,q.textSsim);maxEdge=std::max(maxEdge,q.textEdgeMae);maxRgb=std::max(maxRgb,q.textRgbMae);
            std::cout<<i<<','<<q.yPsnr<<','<<q.uPsnr<<','<<q.vPsnr<<','<<q.ySsim<<','<<q.textSsim<<','<<q.textEdgeMae<<','<<q.textRgbMae<<'\n';
        }
        std::cerr<<std::fixed<<std::setprecision(7)<<"SUMMARY frames="<<count<<" pooled_y_psnr="<<corpus::psnr(yMse/count)<<" pooled_u_psnr="<<corpus::psnr(uMse/count)<<" pooled_v_psnr="<<corpus::psnr(vMse/count)<<" min_y_psnr="<<minY<<" mean_block_y_ssim="<<sum.ySsim/count<<" min_block_y_ssim="<<minYSsim<<" mean_text_block_ssim="<<sum.textSsim/count<<" min_text_block_ssim="<<(scene<2?minTextSsim:std::numeric_limits<double>::quiet_NaN())<<" mean_text_edge_mae="<<sum.textEdgeMae/count<<" max_text_edge_mae="<<maxEdge<<" mean_text_rgb_mae="<<sum.textRgbMae/count<<" max_text_rgb_mae="<<maxRgb<<'\n';
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}return 0;
}
}

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

using Microsoft::WRL::ComPtr;
namespace {
constexpr DWORD stream=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void checked(HRESULT hr,const char* message) {
    if(FAILED(hr)){std::ostringstream out;out<<message<<" (HRESULT 0x"<<std::hex<<unsigned(hr)<<')';throw std::runtime_error(out.str());}
}
int selfCheck() {
    try {
        for(int h:{720,1080}) for(int scene=0;scene<corpus::sceneCount;++scene) {
            const int w=h*16/9;
            corpus::Generator generator(scene,w,h);
            const auto source=generator.frame(0);
            require(source.valid(),"Invalid generated source");
            require(source.pixels==generator.frame(0).pixels,"Nondeterministic corpus");
            require(source.pixels!=generator.frame(45).pixels,"Corpus did not change");
            auto reference=corpus::referenceNv12(source);
            const auto quality=corpus::measure(source,reference.data(),w,reference.data()+size_t(w)*h,w,scene<2);
            require(quality.yPsnr==99&&quality.uPsnr==99&&quality.vPsnr==99&&std::abs(quality.ySsim-1)<1e-12,"Perfect NV12 reference was not perfect");
            require(scene>=2||(quality.textEdgeMae==0&&std::abs(quality.textSsim-1)<1e-12),"Perfect text reference was not perfect");
            for(size_t i=0;i<size_t(w)*h;++i)reference[i]=uint8_t(16+(reference[i]-16)/2);
            for(size_t i=size_t(w)*h;i<reference.size();++i)reference[i]=128;
            const auto damaged=corpus::measure(source,reference.data(),w,reference.data()+size_t(w)*h,w,scene<2);
            require(damaged.yPsnr<30&&damaged.uPsnr<99&&damaged.vPsnr<99&&damaged.ySsim<quality.ySsim,"Metrics ignored luma or chroma damage");
            require(scene>=2||(damaged.textSsim<quality.textSsim&&damaged.textEdgeMae>10&&damaged.textRgbMae>quality.textRgbMae),"Text metrics ignored damage");
        }
        std::cout<<"Encoding corpus and quality metric self-checks passed at 720p and 1080p.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
void saveBmp(const lapse::Frame& f,const std::filesystem::path& path) {
    BITMAPFILEHEADER fh{};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(BITMAPINFOHEADER);
    fh.bfSize=fh.bfOffBits+static_cast<DWORD>(f.pixels.size());
    BITMAPINFOHEADER ih{};ih.biSize=sizeof(ih);ih.biWidth=f.width;ih.biHeight=-f.height;
    ih.biPlanes=1;ih.biBitCount=32;ih.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(&fh),sizeof(fh));
    out.write(reinterpret_cast<const char*>(&ih),sizeof(ih));out.write(reinterpret_cast<const char*>(f.pixels.data()),f.pixels.size());
    require(bool(out),"Could not save comparison image");
}
lapse::Frame rgbFrame(int width,int height,const uint8_t* y,int stride,const uint8_t* uv) {
    lapse::Frame out{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
    auto byte=[](double x){return uint8_t(std::clamp(std::lround(x),0l,255l));};
    for(int j=0;j<height;++j)for(int i=0;i<width;++i) {
        const double yy=(int(y[size_t(j)*stride+i])-16)*255.0/219.0;
        const double cb=(int(uv[size_t(j/2)*stride+(i&~1)])-128)*255.0/224.0;
        const double cr=(int(uv[size_t(j/2)*stride+(i&~1)+1])-128)*255.0/224.0;
        auto* p=&out.pixels[(size_t(j)*width+i)*4];p[0]=byte(yy+1.8556*cb);p[1]=byte(yy-.187324*cb-.468124*cr);p[2]=byte(yy+1.5748*cr);p[3]=255;
    }
    return out;
}
void checkAperture(IMFMediaType* type,REFGUID key,int w,int h) {
    MFVideoArea area{};UINT32 bytes=0;const HRESULT hr=type->GetBlob(key,reinterpret_cast<BYTE*>(&area),sizeof(area),&bytes);
    if(hr==MF_E_ATTRIBUTENOTFOUND)return;checked(hr,"Read display aperture");
    require(bytes==sizeof(area)&&area.OffsetX.value==0&&area.OffsetX.fract==0&&area.OffsetY.value==0&&area.OffsetY.fract==0&&area.Area.cx==w&&area.Area.cy==h,"Unexpected display aperture");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==2&&std::wstring(argv[1])==L"--self-test")return selfCheck();
    if(argc>1&&std::wstring(argv[1])==L"--raw")return raw_quality::run(argc-1,argv+1);
    if(argc<6){std::cerr<<"Usage: encoding_quality_verifier <mp4> <scene:0|1|2|3> <width> <height> <expected_frames> [snapshot_frame]\nOr use --raw <nv12_file> <scene> <width> <height> <frames> after external timestamp validation.\n";return 2;}
    bool com=false,mf=false;int result=0;
    try {
        const std::filesystem::path input=argv[1];const int scene=std::stoi(argv[2]),width=std::stoi(argv[3]),height=std::stoi(argv[4]),expected=std::stoi(argv[5]),snapshot=argc>6?std::stoi(argv[6]):-1;
        require(expected>0,"Expected frames must be positive");corpus::Generator generator(scene,width,height);
        checked(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Initialize COM");com=true;
        checked(MFStartup(MF_VERSION),"Initialize Media Foundation");mf=true;
        ComPtr<IMFSourceReader> reader;checked(MFCreateSourceReaderFromURL(input.c_str(),nullptr,&reader),"Open video");
        PROPVARIANT duration{};
        checked(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&duration),"Read stream duration");
        // The MP4 writer uses fps*1000 media ticks/second. Container duration
        // may round by one such tick even when every decoded frame PTS is exact.
        constexpr LONGLONG durationTolerance=10000000/(corpus::fps*1000)+2;
        const bool validDuration=duration.vt==VT_UI8&&std::llabs(static_cast<LONGLONG>(duration.uhVal.QuadPart)-int64_t(expected)*10000000/corpus::fps)<=durationTolerance;
        PropVariantClear(&duration);require(validDuration,"Unexpected total stream duration");
        checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE),"Deselect streams");
        checked(reader->SetStreamSelection(stream,TRUE),"Select video stream");
        ComPtr<IMFMediaType> type;checked(MFCreateMediaType(&type),"Create output media type");
        checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Select video");checked(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"Select NV12");
        checked(reader->SetCurrentMediaType(stream,nullptr,type.Get()),"Configure decoder");
        ComPtr<IMFMediaType> actual;UINT32 aw=0,ah=0;size_t packedBytes=0;
        auto refreshLayout=[&] {
            actual.Reset();checked(reader->GetCurrentMediaType(stream,&actual),"Read decoder output type");
            GUID subtype{};checked(actual->GetGUID(MF_MT_SUBTYPE,&subtype),"Read decoded pixel format");
            require(subtype==MFVideoFormat_NV12,"Decoder changed away from NV12");
            checked(MFGetAttributeSize(actual.Get(),MF_MT_FRAME_SIZE,&aw,&ah),"Read decoded dimensions");
            require(aw>=UINT32(width)&&ah>=UINT32(height)&&aw<=UINT32(width+64)&&ah<=UINT32(height+64)&&!(aw&1)&&!(ah&1),"Unexpected decoded dimensions");
            checkAperture(actual.Get(),MF_MT_MINIMUM_DISPLAY_APERTURE,width,height);
            checkAperture(actual.Get(),MF_MT_GEOMETRIC_APERTURE,width,height);
            packedBytes=size_t(aw)*ah*3/2;
        };
        refreshLayout();
        std::cerr<<"Decoded dimensions="<<aw<<'x'<<ah<<", visible="<<width<<'x'<<height<<", bytes="<<std::filesystem::file_size(input)<<'\n';
        int count=0;bool ended=false;corpus::Quality sum{};
        double minY=99,minYSsim=1,minTextSsim=1,maxEdge=0,maxRgb=0,yMse=0,uMse=0,vMse=0;
        std::cout<<"frame,timestamp,y_psnr,u_psnr,v_psnr,block_y_ssim,text_block_ssim,text_edge_mae,text_rgb_mae\n"<<std::fixed<<std::setprecision(7);
        for(int attempt=0;attempt<expected+100;++attempt) {
            DWORD flags=0;LONGLONG timestamp=0;ComPtr<IMFSample> sample;
            checked(reader->ReadSample(stream,0,nullptr,&flags,&timestamp,&sample),"Decode sample");
            require(!(flags&MF_SOURCE_READERF_ERROR),"Decoder reported error");
            // H.264 can announce 1088 coded rows only after the first packet.
            // Refresh and validate its visible crop before locating UV pixels.
            if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                refreshLayout();std::cerr<<"Decoder layout changed to "<<aw<<'x'<<ah<<'\n';
            }
            if(sample) {
                require(count<expected,"Extra decoded frames");require(std::llabs(timestamp-int64_t(count)*10000000/corpus::fps)<=1,"Frame timestamp drift or reordering");
                ComPtr<IMFMediaBuffer> buffer;checked(sample->ConvertToContiguousBuffer(&buffer),"Read decoded buffer");
                ComPtr<IMF2DBuffer> twoD;std::vector<uint8_t> packed;
                if(SUCCEEDED(buffer.As(&twoD))) {
                    DWORD length=0;checked(twoD->GetContiguousLength(&length),"Read contiguous buffer length");
                    require(length==packedBytes,"Unexpected contiguous NV12 length");packed.resize(length);
                    checked(twoD->ContiguousCopyTo(packed.data(),length),"Copy contiguous NV12");
                } else {
                    BYTE* data=nullptr;DWORD length=0;checked(buffer->Lock(&data,nullptr,&length),"Lock contiguous NV12");
                    const int stride=int(MFGetAttributeUINT32(actual.Get(),MF_MT_DEFAULT_STRIDE,aw));
                    const bool valid=stride==int(aw)&&length==packedBytes;
                    if(valid)packed.assign(data,data+length);buffer->Unlock();require(valid,"Unsupported non-2D padded decoder layout");
                }
                const auto reference=generator.frame(count);const auto* uv=packed.data()+size_t(aw)*ah;
                const auto q=corpus::measure(reference,packed.data(),aw,uv,aw,scene<2);
                sum.yPsnr+=q.yPsnr;sum.uPsnr+=q.uPsnr;sum.vPsnr+=q.vPsnr;sum.ySsim+=q.ySsim;sum.textSsim+=q.textSsim;sum.textEdgeMae+=q.textEdgeMae;sum.textRgbMae+=q.textRgbMae;
                yMse+=65025*std::pow(10,-q.yPsnr/10);uMse+=65025*std::pow(10,-q.uPsnr/10);vMse+=65025*std::pow(10,-q.vPsnr/10);
                minY=std::min(minY,q.yPsnr);minYSsim=std::min(minYSsim,q.ySsim);minTextSsim=std::min(minTextSsim,q.textSsim);maxEdge=std::max(maxEdge,q.textEdgeMae);maxRgb=std::max(maxRgb,q.textRgbMae);
                std::cout<<count<<','<<timestamp<<','<<q.yPsnr<<','<<q.uPsnr<<','<<q.vPsnr<<','<<q.ySsim<<','<<q.textSsim<<','<<q.textEdgeMae<<','<<q.textRgbMae<<'\n';
                if(count==snapshot) {
                    auto path=input;path+=L".reference.bmp";saveBmp(reference,path);path=input;path+=L".decoded.bmp";saveBmp(rgbFrame(width,height,packed.data(),aw,uv),path);
                }
                ++count;
            }
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM){ended=true;break;}
        }
        require(ended&&count==expected,"Lost frames or no end of stream");
        std::cerr<<std::fixed<<std::setprecision(7)<<"SUMMARY frames="<<count<<" pooled_y_psnr="<<corpus::psnr(yMse/count)<<" pooled_u_psnr="<<corpus::psnr(uMse/count)<<" pooled_v_psnr="<<corpus::psnr(vMse/count)<<" min_y_psnr="<<minY<<" mean_block_y_ssim="<<sum.ySsim/count<<" min_block_y_ssim="<<minYSsim<<" mean_text_block_ssim="<<sum.textSsim/count<<" min_text_block_ssim="<<(scene<2?minTextSsim:std::numeric_limits<double>::quiet_NaN())<<" mean_text_edge_mae="<<sum.textEdgeMae/count<<" max_text_edge_mae="<<maxEdge<<" mean_text_rgb_mae="<<sum.textRgbMae/count<<" max_text_rgb_mae="<<maxRgb<<'\n';
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    if(mf)MFShutdown();if(com)CoUninitialize();return result;
}

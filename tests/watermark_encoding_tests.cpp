// Generated frames only: exercise the real text renderer, MP4 writers, and decoder.
#include "watermark.h"
#include "encoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
constexpr int width = 1280, height = 720, frameCount = 12, fps = 30;
constexpr DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checked(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::wcerr << lapse::errorText(hr) << L'\n'; throw std::runtime_error(message); }
}
void operation(bool value, const std::wstring& error) {
    if (!value) { std::wcerr << error << L'\n'; throw std::runtime_error("Watermarked media operation failed"); }
}
lapse::Frame background() {
    lapse::Frame result{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
    for (size_t i=0;i<result.pixels.size();i+=4) {
        result.pixels[i]=result.pixels[i+1]=result.pixels[i+2]=72;
        result.pixels[i+3]=255;
    }
    return result;
}
lapse::WatermarkContext context(int index) {
    lapse::WatermarkContext value;
    value.activeMs=86400000LL+index*5000;
    value.recordedLocal.wYear=2026;value.recordedLocal.wMonth=10;value.recordedLocal.wDay=1;
    value.recordedLocal.wHour=23;value.recordedLocal.wMinute=58;value.recordedLocal.wSecond=static_cast<WORD>(index);
    value.targetIntervalMs=index%2 ? 20000 : 5137;
    return value;
}
void bitmap(const std::filesystem::path& path, const lapse::Frame& frame) {
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);
    file.bfSize=file.bfOffBits+static_cast<DWORD>(frame.pixels.size());
    info.biSize=sizeof(info);info.biWidth=frame.width;info.biHeight=-frame.height;
    info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));
    output.write(reinterpret_cast<const char*>(&info),sizeof(info));
    output.write(reinterpret_cast<const char*>(frame.pixels.data()),static_cast<std::streamsize>(frame.pixels.size()));
    require(bool(output),"Could not save owned bitmap evidence");
}
double verify(const std::filesystem::path& path, lapse::WatermarkRenderer& renderer, bool recovery, bool keep) {
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"Open generated watermarked video");
    ComPtr<IMFMediaType> type;checked(MFCreateMediaType(&type),"Create decoder type");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Set decoder video type");
    checked(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"Set decoder NV12 type");
    checked(reader->SetCurrentMediaType(video,nullptr,type.Get()),"Decode watermarked video");
    ComPtr<IMFMediaType> actual;checked(reader->GetCurrentMediaType(video,&actual),"Read decoder geometry");
    UINT32 w=0,h=0;checked(MFGetAttributeSize(actual.Get(),MF_MT_FRAME_SIZE,&w,&h),"Read decoded size");
    require(w>=width && h>=height,"Decoded watermark canvas shrank");
    const LONG defaultStride=static_cast<LONG>(MFGetAttributeUINT32(actual.Get(),MF_MT_DEFAULT_STRIDE,w));
    int seen=0;bool ended=false;double worst=99;int64_t maximumTimestampError=0;
    for(int attempt=0;attempt<frameCount+100;++attempt) {
        DWORD flags=0;LONGLONG timestamp=0;ComPtr<IMFSample> sample;
        checked(reader->ReadSample(video,0,nullptr,&flags,&timestamp,&sample),"Read watermarked frame");
        require(!(flags&MF_SOURCE_READERF_ERROR),"Decoder reported watermark frame error");
        if(sample) {
            // The fragmented muxer uses 30,000 container ticks/sec. Its decoded
            // times can round by one container tick, as in encoder_recovery_tests.
            const int64_t tolerance=recovery ? 10000000/(fps*1000)+1 : 1;
            maximumTimestampError=std::max(maximumTimestampError,std::llabs(timestamp-int64_t(seen)*10000000/fps));
            require(seen<frameCount && std::llabs(timestamp-int64_t(seen)*10000000/fps)<=tolerance,
                "Watermark changed frame count, order, or presentation time");
            auto expected=background();std::wstring error;
            operation(renderer.apply(expected,context(seen),error),error);
            const RECT box=renderer.lastBounds();
            require(box.left>=0 && box.top>=0 && box.right<=width && box.bottom<=height &&
                box.right>box.left && box.bottom>box.top,"Invalid watermark comparison rectangle");
            ComPtr<IMFMediaBuffer> buffer;checked(sample->ConvertToContiguousBuffer(&buffer),"Read decoded pixels");
            ComPtr<IMF2DBuffer> twoD;BYTE* data=nullptr;LONG stride=defaultStride;DWORD length=0;
            if(SUCCEEDED(buffer.As(&twoD))) checked(twoD->Lock2D(&data,&stride),"Lock decoded watermark surface");
            else checked(buffer->Lock(&data,nullptr,&length),"Lock decoded watermark buffer");
            const bool valid=stride>=width && (twoD || (stride>0 && size_t(length)>=size_t(stride)*height));
            uint64_t squared=0,pixels=0,bright=0,retainedBright=0;
            lapse::Frame decoded;
            if(keep && seen==frameCount-1)decoded=background();
            if(valid) {
                for(int y=box.top;y<box.bottom;++y)for(int x=box.left;x<box.right;++x) {
                    const int original=expected.pixels[(size_t(y)*width+x)*4];
                    const int reference=16+int(std::lround(original*219.0/255.0));
                    const int observed=data[size_t(y)*stride+x],difference=observed-reference;
                    squared+=uint64_t(difference*difference);++pixels;
                    if(original>=220){++bright;if(observed>=170)++retainedBright;}
                }
                if(decoded.valid())for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
                    const auto gray=static_cast<uint8_t>(std::clamp((int(data[size_t(y)*stride+x])-16)*255/219,0,255));
                    auto* pixel=&decoded.pixels[(size_t(y)*width+x)*4];pixel[0]=pixel[1]=pixel[2]=gray;
                }
            }
            if(twoD)twoD->Unlock2D();else buffer->Unlock();
            require(valid,"Invalid decoded stride or buffer length");
            require(bright>100 && retainedBright*100>=bright*90,"Encoded text lost its bright glyph strokes");
            const double psnr=squared ? 10*std::log10(255.0*255.0*double(pixels)/double(squared)) : 99;
            worst=std::min(worst,psnr);
            require(psnr>=27,"Watermark region lost too much detail during encoding");
            if(decoded.valid()){auto image=path;image.replace_extension(L"bmp");bitmap(image,decoded);}
            ++seen;
        }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM){ended=true;break;}
    }
    require(ended && seen==frameCount,"Watermarked video lost frames");
    std::cout<<"maximum timestamp rounding="<<maximumTimestampError<<" (100 ns units); ";
    return worst;
}
}
int main(int argc,char**) {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(com))return 1;
    if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();return 1;}
    const auto directory=std::filesystem::current_path()/
        (L"watermark-media-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    int result=0;
    try {
        require(std::filesystem::create_directory(directory),"Could not create owned media directory");
        for(int mode=0;mode<3;++mode)for(int clock=0;clock<2;++clock) {
            lapse::WatermarkSettings settings;settings.enabled=true;
            settings.timeKind=clock ? lapse::WatermarkTimeKind::RecordedLocal : lapse::WatermarkTimeKind::ActiveElapsed;
            settings.x=clock ? 0 : 10000;settings.y=clock ? 0 : 10000;
            lapse::WatermarkRenderer renderer;std::wstring error;
            operation(renderer.prepare(settings,width,height,error),error);
            const auto path=directory/(std::to_wstring(mode)+L"-"+std::to_wstring(clock)+L".mp4");
            lapse::Encoder encoder;
            operation(encoder.open(path.wstring(),width,height,fps,error,lapse::EncodingQuality::Compact,
                mode==1 ? lapse::EncodingMode::Efficient : lapse::EncodingMode::Compatible,mode==2),error);
            for(int i=0;i<frameCount;++i){auto frame=background();operation(renderer.apply(frame,context(i),error),error);operation(encoder.write(frame,error),error);}
            operation(encoder.finish(error),error);
            const double psnr=verify(path,renderer,mode==2,argc>1);
            std::cout<<"mode="<<mode<<" clock="<<clock<<" frames="<<frameCount<<" minimum watermark luma PSNR="<<psnr<<" dB\n";
        }
        if(argc>1)std::wcout<<L"Artifacts kept at "<<directory.wstring()<<L'\n';
        else std::filesystem::remove_all(directory);
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';std::wcout<<L"Artifacts kept at "<<directory.wstring()<<L'\n';result=1;}
    MFShutdown();CoUninitialize();return result;
}

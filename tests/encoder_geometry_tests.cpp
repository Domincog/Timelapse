// Actual MP4 encoding/decoding of generated pixels; no capture devices.
#include "encoder.h"
#include "config.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

HANDLE WINAPI geometryCreateFile(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
// Count our owned-file admission separately from Media Foundation internals.
#define CreateFileW geometryCreateFile
#include "../src/encoder.cpp"
#undef CreateFileW
namespace { unsigned fileAttempts=0; }
HANDLE WINAPI geometryCreateFile(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,
                                DWORD creation,DWORD flags,HANDLE templateFile) {
    ++fileAttempts;
    return CreateFileW(path,access,share,security,creation,flags,templateFile);
}

namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD video=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr int fps=30;
void require(bool okay,const char* message) { if(!okay) throw std::runtime_error(message); }
void check(HRESULT result,const char* message) {
    if(FAILED(result)) { std::wcerr<<lapse::errorText(result)<<L'\n';throw std::runtime_error(message); }
}
void encoded(bool okay,const std::wstring& error) {
    if(!okay) { std::wcerr<<error<<L'\n';throw std::runtime_error("Geometry encoding failed"); }
}
std::array<int,3> pixel(int x,int y,int width,int height,int index) {
    std::array<int,3> p{};
    if(y<height/2) { if(x<width/2)p[index==1?0:2]=255;else p[1]=255; }
    else if(x<width/2)p={255,255,255};else p={32,32,32};
    // Distinct final two rows/columns expose crop and padded-stride errors.
    if(y>=height-2)p={0,255,255};
    if(x>=width-2)p={255,255,0};
    return p;
}
void pattern(lapse::Frame& frame,int index) {
    for(int y=0;y<frame.height;++y)for(int x=0;x<frame.width;++x) {
        const auto color=pixel(x,y,frame.width,frame.height,index);
        auto* p=&frame.pixels[(size_t(y)*frame.width+x)*4];
        for(int c=0;c<3;++c)p[c]=static_cast<BYTE>(color[c]);
        p[3]=123; // Input alpha must not affect encoded color.
    }
}
void checkPixels(const BYTE* y,int stride,const BYTE* uv,int width,int height,int index) {
    const int xs[]={width/4,width*3/4,width-2,width-1};
    const int ys[]={height/4,height*3/4,height-2,height-1};
    for(int row:ys)for(int column:xs) {
        const auto p=pixel(column,row,width,height,index);
        const int expectedY=16+((p[0]*2032+p[1]*20127+p[2]*5983+16384)>>15);
        require(std::abs(int(y[size_t(row)*stride+column])-expectedY)<=20,"Decoded luma/orientation/edge mismatch");
        std::array<int,3> sum{};
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
            const auto source=pixel((column&~1)+dx,(row&~1)+dy,width,height,index);
            for(int c=0;c<3;++c)sum[c]+=source[c];
        }
        const int expectedU=(128*262144+131072+sum[0]*28784-sum[1]*22189-sum[2]*6596)>>18;
        const int expectedV=(128*262144+131072-sum[0]*2639-sum[1]*26145+sum[2]*28784)>>18;
        const auto at=size_t(row/2)*stride+(column&~1);
        require(std::abs(int(uv[at])-expectedU)<=25&&std::abs(int(uv[at+1])-expectedV)<=25,
                "Decoded chroma/UV offset/edge mismatch");
    }
}
bool checkAperture(IMFMediaType* type,REFGUID key,int width,int height) {
    MFVideoArea area{};UINT32 bytes=0;
    const auto result=type->GetBlob(key,reinterpret_cast<BYTE*>(&area),sizeof(area),&bytes);
    if(result==MF_E_ATTRIBUTENOTFOUND)return false;
    check(result,"Read display aperture");
    require(bytes==sizeof(area)&&area.OffsetX.value==0&&area.OffsetX.fract==0&&
        area.OffsetY.value==0&&area.OffsetY.fract==0&&area.Area.cx==width&&area.Area.cy==height,
        "Visible display crop changed");
    return true;
}
void verify(const std::filesystem::path& path,int width,int height,int expected,REFGUID codec) {
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"Open geometry video");
    ComPtr<IMFMediaType> native;check(reader->GetNativeMediaType(video,0,&native),"Read geometry native type");
    GUID subtype{};check(native->GetGUID(MF_MT_SUBTYPE,&subtype),"Read geometry codec");
    require(subtype==codec,"Geometry codec changed");
    UINT32 nativeWidth=0,nativeHeight=0;
    check(MFGetAttributeSize(native.Get(),MF_MT_FRAME_SIZE,&nativeWidth,&nativeHeight),"Read native geometry");
    require(nativeWidth>=unsigned(width)&&nativeHeight>=unsigned(height)&&
        nativeWidth<unsigned(width+64)&&nativeHeight<unsigned(height+64),"Native visible geometry mismatch");
    const bool nativeMinimum=checkAperture(native.Get(),MF_MT_MINIMUM_DISPLAY_APERTURE,width,height);
    const bool nativeGeometric=checkAperture(native.Get(),MF_MT_GEOMETRIC_APERTURE,width,height);
    require((nativeWidth==unsigned(width)&&nativeHeight==unsigned(height))||nativeMinimum||nativeGeometric,
            "Padded native size lacks the requested visible crop");
    require(MFGetAttributeUINT32(native.Get(),MF_MT_VIDEO_PRIMARIES,0)==MFVideoPrimaries_BT709&&
        MFGetAttributeUINT32(native.Get(),MF_MT_TRANSFER_FUNCTION,0)==MFVideoTransFunc_709&&
        MFGetAttributeUINT32(native.Get(),MF_MT_YUV_MATRIX,0)==MFVideoTransferMatrix_BT709&&
        MFGetAttributeUINT32(native.Get(),MF_MT_VIDEO_NOMINAL_RANGE,0)==MFNominalRange_16_235,
        "Geometry color metadata changed");
    PROPVARIANT duration{};
    check(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&duration),"Read duration");
    const bool durationOkay=duration.vt==VT_UI8&&std::llabs(static_cast<LONGLONG>(duration.uhVal.QuadPart)-
        int64_t(expected)*10000000/fps)<=10000000/(fps*1000)+2;
    PropVariantClear(&duration);require(durationOkay,"Geometry duration mismatch");
    ComPtr<IMFMediaType> requested;check(MFCreateMediaType(&requested),"Create decode type");
    check(requested->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Set major type");
    check(requested->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"Set NV12");
    check(reader->SetCurrentMediaType(video,nullptr,requested.Get()),"Configure geometry decode");
    ComPtr<IMFMediaType> current;UINT32 codedWidth=0,codedHeight=0;
    auto layout=[&] {
        current.Reset();check(reader->GetCurrentMediaType(video,&current),"Read decoded type");
        check(MFGetAttributeSize(current.Get(),MF_MT_FRAME_SIZE,&codedWidth,&codedHeight),"Read coded geometry");
        require(codedWidth>=unsigned(width)&&codedHeight>=unsigned(height)&&
            codedWidth<unsigned(width+64)&&codedHeight<unsigned(height+64)&&!(codedWidth&1)&&!(codedHeight&1),
            "Unexpected coded geometry");
        const bool minimum=checkAperture(current.Get(),MF_MT_MINIMUM_DISPLAY_APERTURE,width,height);
        const bool geometric=checkAperture(current.Get(),MF_MT_GEOMETRIC_APERTURE,width,height);
        require((codedWidth==unsigned(width)&&codedHeight==unsigned(height))||minimum||geometric,
                "Padded decoded size lacks the requested visible crop");
    };
    layout();int count=0;bool ended=false;
    for(int attempt=0;attempt<expected+100;++attempt) {
        DWORD flags=0;LONGLONG timestamp=0;ComPtr<IMFSample> sample;
        check(reader->ReadSample(video,0,nullptr,&flags,&timestamp,&sample),"Decode geometry frame");
        require(!(flags&MF_SOURCE_READERF_ERROR),"Geometry decoder error");
        if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)layout();
        if(sample) {
            require(count<expected&&std::llabs(timestamp-int64_t(count)*10000000/fps)<=1,
                    "Geometry frame count or timestamp mismatch");
            ComPtr<IMFMediaBuffer> buffer;check(sample->ConvertToContiguousBuffer(&buffer),"Get geometry pixels");
            ComPtr<IMF2DBuffer> twoD;std::vector<BYTE> packed;
            const size_t expectedBytes=size_t(codedWidth)*codedHeight*3/2;
            if(SUCCEEDED(buffer.As(&twoD))) {
                DWORD bytes=0;check(twoD->GetContiguousLength(&bytes),"Get packed length");
                require(bytes==expectedBytes,"Unexpected packed NV12 geometry");packed.resize(bytes);
                check(twoD->ContiguousCopyTo(packed.data(),bytes),"Copy packed geometry pixels");
            } else {
                BYTE* data=nullptr;DWORD bytes=0;check(buffer->Lock(&data,nullptr,&bytes),"Lock geometry pixels");
                const bool valid=bytes==expectedBytes&&MFGetAttributeUINT32(current.Get(),MF_MT_DEFAULT_STRIDE,codedWidth)==codedWidth;
                if(valid)packed.assign(data,data+bytes);buffer->Unlock();require(valid,"Unsupported flat decode stride");
            }
            // UV follows coded height, including decoder padding/crop rows.
            checkPixels(packed.data(),int(codedWidth),packed.data()+size_t(codedWidth)*codedHeight,width,height,count);
            ++count;
        }
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM){ended=true;break;}
    }
    require(ended&&count==expected,"Geometry lost final frames");
}
void exercise(const std::filesystem::path& directory,int width,int height,lapse::EncodingMode mode,int count=3) {
    const auto name=std::to_wstring(width)+L"x"+std::to_wstring(height)+L"-"+std::to_wstring(int(mode))+L"-"+std::to_wstring(count);
    const auto path=directory/(name+L".mp4");std::wstring error;
    lapse::Encoder encoder;encoded(encoder.open(path.wstring(),width,height,fps,error,lapse::EncodingQuality::Balanced,mode),error);
    lapse::Frame frame{width,height,std::vector<BYTE>(size_t(width)*height*4)};
    for(int i=0;i<count;++i){pattern(frame,i);encoded(encoder.write(frame,error),error);}
    const bool finished=encoder.finish(error);
    if(count){encoded(finished,error);verify(path,width,height,count,mode==lapse::EncodingMode::SoftwareAV1?MFVideoFormat_AV1:MFVideoFormat_H264);}
    else require(!finished&&!error.empty()&&!std::filesystem::exists(path),"Empty custom recording must report no frames and remove its owned output");
    std::wcout<<L"PASS "<<name<<L" visible geometry, color edges, frame timing and finalization\n";
}
// The Windows AV1 decoder is an optional Store extension; encoding never uses it.
bool av1Decoder() {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video,MFVideoFormat_AV1};IMFActivate** list=nullptr;UINT32 count=0;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,MFT_ENUM_FLAG_ALL&~MFT_ENUM_FLAG_FIELDOFUSE,&input,nullptr,&list,&count),"AV1 decoder enumeration failed");
    for(UINT32 i=0;i<count;++i)list[i]->Release();
    CoTaskMemFree(list);return count!=0;
}
void invalid(const std::filesystem::path& directory) {
    std::wstring error;lapse::Encoder encoder;const auto path=directory/L"invalid.mp4";
    for(auto size:{std::pair<int,int>{0,48},{46,48},{48,46},{49,48},{48,49},{4098,48},{48,4098},
                  {4096,4096},{4096,2162},{2162,4096},{INT_MAX,48}}) {
        fileAttempts=0;
        require(!encoder.open(path.wstring(),size.first,size.second,fps,error)&&!error.empty()&&
            fileAttempts==0&&!std::filesystem::exists(path),"Invalid custom size touched output storage");
    }
    std::cout<<"PASS invalid dimensions and pixel budget rejected before file creation\n";
}
}
int main() {
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 1;
    if(FAILED(MFStartup(MF_VERSION))){CoUninitialize();return 1;}
    const auto owner=std::filesystem::current_path();
    const auto directory=owner/(L"encoder-geometry-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    int result=0;
    try {
        require(directory.parent_path()==owner&&std::filesystem::create_directory(directory),"Create owned geometry directory");
        invalid(directory);
        for(auto size:{std::pair<int,int>{48,48},{638,478},{478,638},{1080,1080},{720,1280}})
            exercise(directory,size.first,size.second,lapse::EncodingMode::Compatible);
        exercise(directory,4096,2160,lapse::EncodingMode::Efficient);
        exercise(directory,2160,3840,lapse::EncodingMode::Efficient);
        exercise(directory,638,478,lapse::EncodingMode::QualityH264);
        exercise(directory,638,478,lapse::EncodingMode::Efficient,1);
        exercise(directory,48,48,lapse::EncodingMode::Compatible,0);
        exercise(directory,48,48,lapse::EncodingMode::SoftwareAV1,0);
        if(av1Decoder()) {
            for(auto size:{std::pair<int,int>{48,48},{638,478},{478,638},{4096,2160},{2160,3840}})
                exercise(directory,size.first,size.second,lapse::EncodingMode::SoftwareAV1);
        } else std::cout<<"SKIP AV1 geometry pixels: the optional Windows AV1 decoder is not installed\n";
        std::filesystem::remove_all(directory);
        std::cout<<"All synthetic encoder geometry contracts passed.\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';result=1;}
    MFShutdown();CoUninitialize();return result;
}

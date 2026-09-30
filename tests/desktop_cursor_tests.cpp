// Synthetic cursor ownership, real offscreen pixels and explicit cleanup; no physical input.
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <cstring>

BOOL WINAPI probeCursorInfo(PCURSORINFO);
BOOL WINAPI probeIconInfo(HICON,PICONINFO);
BOOL WINAPI probeDrawIcon(HDC,int,int,HICON,int,int,UINT,HBRUSH,UINT);
int WINAPI probeMetrics(int);
HDC WINAPI rejectDesktopDC(HWND);
int WINAPI probeReleaseDC(HWND,HDC);
BOOL WINAPI probeStretchBlt(HDC,int,int,int,int,HDC,int,int,int,int,DWORD);
HICON WINAPI probeCopyIcon(HICON);
BOOL WINAPI probeDestroyIcon(HICON);
BOOL WINAPI probeDeleteObject(HGDIOBJ);
#define GetCursorInfo probeCursorInfo
#define GetIconInfo probeIconInfo
#define DrawIconEx probeDrawIcon
#define GetSystemMetrics probeMetrics
#define GetDC rejectDesktopDC
#define ReleaseDC probeReleaseDC
#define StretchBlt probeStretchBlt
#define CopyIcon probeCopyIcon
#define DestroyIcon probeDestroyIcon
#define DeleteObject probeDeleteObject
#include "../src/capture.cpp"
#undef DeleteObject
#undef DestroyIcon
#undef CopyIcon
#undef StretchBlt
#undef ReleaseDC
#undef GetDC
#undef GetSystemMetrics
#undef DrawIconEx
#undef GetIconInfo
#undef GetCursorInfo

namespace {
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
bool cleanupFailed=false;
struct Surface {
    HDC dc=nullptr;
    HBITMAP bitmap=nullptr;
    HGDIOBJ previous=nullptr;
    BYTE* pixels=nullptr;
    int width,height;
    Surface(int w,int h):width(w),height(h) {
        dc=CreateCompatibleDC(nullptr);require(dc!=nullptr,"Owned DC creation failed");
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        require(bitmap&&pixels,"Owned bitmap creation failed");
        previous=SelectObject(dc,bitmap);require(previous&&previous!=HGDI_ERROR,"Owned selection failed");
        fill(20,40,60);
    }
    void fill(BYTE b,BYTE g,BYTE r) {
        for(int i=0;i<width*height;++i){pixels[4*i]=b;pixels[4*i+1]=g;pixels[4*i+2]=r;pixels[4*i+3]=255;}
    }
    ~Surface(){
        if(previous&&SelectObject(dc,previous)!=bitmap)cleanupFailed=true;
        if(bitmap&&!DeleteObject(bitmap))cleanupFailed=true;
        if(dc&&!DeleteDC(dc))cleanupFailed=true;
    }
};
struct OwnedCursor {
    HCURSOR value=nullptr;
    OwnedCursor() {
        Surface color(4,4);color.fill(0,0,255);
        BYTE maskBits[8]{};
        HBITMAP mask=CreateBitmap(4,4,1,1,maskBits);require(mask!=nullptr,"Owned mask creation failed");
        require(SelectObject(color.dc,color.previous)==color.bitmap,"Cannot deselect owned cursor bitmap");color.previous=nullptr;
        ICONINFO icon{};icon.fIcon=FALSE;icon.xHotspot=1;icon.yHotspot=2;
        icon.hbmMask=mask;icon.hbmColor=color.bitmap;
        value=reinterpret_cast<HCURSOR>(CreateIconIndirect(&icon));require(DeleteObject(mask)!=FALSE,"Owned mask cleanup failed");
        require(value!=nullptr,"Owned nonshared cursor creation failed");
    }
    void retire(){require(value&&DestroyCursor(value)!=FALSE,"Owned cursor retirement failed");value=nullptr;}
    ~OwnedCursor(){if(value&&!DestroyCursor(value))cleanupFailed=true;}
};
OwnedCursor* activeCursor=nullptr;
HDC expectedDC=nullptr;
bool retireAfterMetadata=false;
int metadataCalls=0,retirements=0,drawCalls=0;
DWORD retiredGdiObjects=0;
HICON copiedCursor=nullptr;
HBITMAP metadataMask=nullptr,metadataColor=nullptr;
int copyCalls=0,copyCreated=0,copyDestroyed=0;
bool failCopy=false,failMetadata=false;
BOOL drawResult=FALSE;
DWORD drawError=0;
int cursorQueries=0,cursorMetrics=0,expectedOffset=4,expectedExtent=4;
HDC ownedSourceDC=nullptr,captureTargetDC=nullptr;
HBITMAP captureBitmap=nullptr;
int sourceAcquired=0,sourceReleased=0,captureCopies=0,captureBitmapsDeleted=0;
bool caseRun(bool retire) {
    Surface target(16,16);OwnedCursor cursor;
    activeCursor=&cursor;expectedDC=target.dc;retireAfterMetadata=retire;
    metadataCalls=retirements=drawCalls=0;retiredGdiObjects=0;drawResult=FALSE;drawError=0;
    copyCalls=copyCreated=copyDestroyed=0;failCopy=failMetadata=false;
    const DWORD gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    lapse::drawCursor(target.dc,{-100,200,-84,216},16,16);
    require(GdiFlush()!=FALSE,"Owned rendering flush failed");
    require(metadataCalls==1&&drawCalls==1&&retirements==int(retire),"Expected cursor boundary was not reached once");
    int mismatch=0;
    for(int y=0;y<16;++y)for(int x=0;x<16;++x){
        const bool red=x>=4&&x<8&&y>=4&&y<8;
        const BYTE* p=target.pixels+(y*16+x)*4;
        if(p[0]!=(red?0:20)||p[1]!=(red?0:40)||p[2]!=(red?255:60))++mismatch;
    }
    const DWORD gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    // Account separately for native bitmap resources destroyed with the owned
    // source cursor; production must release its metadata copies as well.
    require(gdiBefore==gdiAfter+retiredGdiObjects,"Production cursor metadata leaked a GDI object");
    require(!copiedCursor&&!metadataMask&&!metadataColor&&copyCreated==copyDestroyed,
            "Production cursor copies did not release their owned handles");
    const bool passed=drawResult&&mismatch==0;
    std::cout<<(passed?"PASS ":"FAIL ")<<(retire?"owner retires after metadata":"healthy borrowed cursor")
        <<": metadata="<<metadataCalls<<" retired="<<retirements<<" draw="<<drawResult
        <<" draw_error="<<drawError<<" mismatched_pixels="<<mismatch
        <<" gdi_before="<<gdiBefore<<" gdi_after="<<gdiAfter<<" source_gdi_retired="<<retiredGdiObjects
        <<" copied="<<copyCreated<<" destroyed="<<copyDestroyed<<'\n';
    activeCursor=nullptr;expectedDC=nullptr;
    return passed;
}
void earlyFailure(bool copying) {
    Surface target(16,16);OwnedCursor cursor;
    activeCursor=&cursor;expectedDC=target.dc;retireAfterMetadata=false;
    metadataCalls=retirements=drawCalls=copyCalls=copyCreated=copyDestroyed=0;
    failCopy=copying;failMetadata=!copying;
    lapse::drawCursor(target.dc,{-100,200,-84,216},16,16);
    require(GdiFlush()!=FALSE,"Owned failure rendering flush failed");
    require(copyCalls==1&&metadataCalls==int(!copying)&&drawCalls==0&&cursor.value,
            "Early-failure boundary or source ownership changed");
    require(!copiedCursor&&!metadataMask&&!metadataColor&&copyCreated==int(!copying)&&copyDestroyed==copyCreated,
            "Early failure leaked or destroyed the wrong cursor");
    for(int i=0;i<16*16;++i)require(target.pixels[4*i]==20&&target.pixels[4*i+1]==40&&target.pixels[4*i+2]==60,
            "Early failure changed owned destination pixels");
    std::cout<<"PASS "<<(copying?"CopyIcon failure":"GetIconInfo failure after copied handle")
        <<": copied="<<copyCreated<<" destroyed="<<copyDestroyed<<" original_retained=1 draws=0\n";
    activeCursor=nullptr;expectedDC=nullptr;failCopy=failMetadata=false;
}
void captureToggle(int extent) {
    Surface source(16,16);OwnedCursor cursor;
    activeCursor=&cursor;ownedSourceDC=source.dc;retireAfterMetadata=false;
    expectedOffset=expectedExtent=extent/4;
    cursorQueries=cursorMetrics=metadataCalls=drawCalls=copyCalls=copyCreated=copyDestroyed=0;
    sourceAcquired=sourceReleased=captureCopies=captureBitmapsDeleted=0;
    lapse::Frame output;std::wstring error;
    const BYTE* storage=nullptr;
    const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(bool show:{true,false,true}) {
        const int queries=cursorQueries,metrics=cursorMetrics,metadata=metadataCalls;
        const int copies=copyCalls,draws=drawCalls,destroyed=copyDestroyed;
        require(lapse::captureDesktop({-100,200,-84,216},extent,extent,show,output,error)&&
                error.empty()&&output.valid()&&output.width==extent&&output.height==extent,
                "Owned on/off desktop capture failed or changed geometry");
        if(storage)require(storage==output.pixels.data(),"Cursor toggle discarded reusable output storage");
        storage=output.pixels.data();
        for(int y=0;y<extent;++y)for(int x=0;x<extent;++x) {
            const bool red=show&&x>=extent/4&&x<extent/2&&y>=extent/4&&y<extent/2;
            const auto* pixel=output.pixels.data()+(size_t(y)*extent+x)*4;
            require(pixel[0]==(red?0:20)&&pixel[1]==(red?0:40)&&pixel[2]==(red?255:60),
                    "Cursor toggle left a ghost or changed unrelated RGB pixels");
        }
        require(cursorQueries==queries+int(show)&&cursorMetrics==metrics+2*int(show)&&
                metadataCalls==metadata+int(show)&&copyCalls==copies+int(show)&&
                drawCalls==draws+int(show)&&copyDestroyed==destroyed+int(show),
                "Off capture performed cursor API work or On omitted expected work");
        require(!copiedCursor&&!metadataMask&&!metadataColor,"Capture retained cursor resources");
    }
    lapse::releaseDesktopCaptureCache();
    require(!captureBitmap&&captureBitmapsDeleted==1&&sourceAcquired==3&&sourceReleased==3&&captureCopies==3,
            "Capture did not reuse/release its owned surface or balance the borrowed source DC");
    require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==before,"Capture toggle leaked a GDI resource");
    std::cout<<"PASS actual capture on/off/on "<<extent<<"x"<<extent
        <<": exact RGB, no ghost, Off cursor calls=0, one reused DIB released\n";
    activeCursor=nullptr;ownedSourceDC=captureTargetDC=expectedDC=nullptr;
    expectedOffset=expectedExtent=4;
}
}
BOOL WINAPI probeCursorInfo(PCURSORINFO info) {
    ++cursorQueries;
    require(activeCursor&&activeCursor->value&&info&&info->cbSize==sizeof(CURSORINFO),"Unexpected cursor query");
    info->flags=CURSOR_SHOWING;info->hCursor=activeCursor->value;info->ptScreenPos={-95,206};return TRUE;
}
BOOL WINAPI probeIconInfo(HICON value,PICONINFO info) {
    ++metadataCalls;
    if(failMetadata){SetLastError(ERROR_INVALID_CURSOR_HANDLE);return FALSE;}
    const BOOL result=GetIconInfo(value,info);
    require(result!=FALSE,"Actual cursor metadata lookup failed");
    require(!metadataMask&&!metadataColor,"Metadata copies were retained from an earlier case");
    metadataMask=info->hbmMask;metadataColor=info->hbmColor;
    // Model the original cursor owner replacing/freeing its nonshared handle
    // after metadata is copied. This cursor was never selected as a real cursor.
    if(retireAfterMetadata){
        const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        activeCursor->retire();++retirements;
        const DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        require(before>=after,"Retiring source cursor unexpectedly acquired GDI resources");
        retiredGdiObjects=before-after;
    }
    return result;
}
BOOL WINAPI probeDrawIcon(HDC dc,int x,int y,HICON icon,int w,int h,UINT frame,HBRUSH brush,UINT flags) {
    require(dc==expectedDC&&x==expectedOffset&&y==expectedOffset&&w==expectedExtent&&h==expectedExtent&&frame==0&&!brush&&flags==DI_NORMAL,
            "Cursor hotspot, rectangle or owned target is wrong");
    ++drawCalls;SetLastError(ERROR_SUCCESS);
    drawResult=DrawIconEx(dc,x,y,icon,w,h,frame,brush,flags);drawError=GetLastError();return drawResult;
}
int WINAPI probeMetrics(int index) {
    if(ownedSourceDC) {
        if(index==SM_XVIRTUALSCREEN)return -100;
        if(index==SM_YVIRTUALSCREEN)return 200;
        if(index==SM_CXVIRTUALSCREEN||index==SM_CYVIRTUALSCREEN)return 16;
    }
    require(index==SM_CXCURSOR||index==SM_CYCURSOR,"Unexpected desktop metrics query");++cursorMetrics;return 32;
}
HDC WINAPI rejectDesktopDC(HWND window){
    require(!window&&ownedSourceDC,"Physical desktop access forbidden");++sourceAcquired;return ownedSourceDC;
}
int WINAPI probeReleaseDC(HWND window,HDC dc){
    require(!window&&dc==ownedSourceDC,"Unexpected source DC release");++sourceReleased;return 1;
}
BOOL WINAPI probeStretchBlt(HDC target,int x,int y,int w,int h,HDC source,int sx,int sy,int sw,int sh,DWORD flags){
    require(source==ownedSourceDC&&source&&x==0&&y==0&&w==h&&(w==16||w==8)&&
            sx==-100&&sy==200&&sw==16&&sh==16&&flags==(SRCCOPY|CAPTUREBLT),
            "Capture escaped the owned source geometry");
    const auto bitmap=static_cast<HBITMAP>(GetCurrentObject(target,OBJ_BITMAP));
    require(bitmap&&(!captureTargetDC||(captureTargetDC==target&&captureBitmap==bitmap)),
            "Cursor toggle recreated the capture surface");
    captureTargetDC=expectedDC=target;captureBitmap=bitmap;++captureCopies;
    return StretchBlt(target,x,y,w,h,source,0,0,sw,sh,flags);
}
HICON WINAPI probeCopyIcon(HICON value) {
    ++copyCalls;
    require(!copiedCursor&&activeCursor&&value==activeCursor->value,"Unexpected source cursor copy");
    if(failCopy){SetLastError(ERROR_INVALID_CURSOR_HANDLE);return nullptr;}
    copiedCursor=CopyIcon(value);require(copiedCursor&&copiedCursor!=value,"Actual cursor copy failed or aliased its owner");
    ++copyCreated;return copiedCursor;
}
BOOL WINAPI probeDestroyIcon(HICON value) {
    require(value&&value==copiedCursor,"Production destroyed an unowned cursor");
    const BOOL result=DestroyIcon(value);require(result!=FALSE,"Actual copied-cursor cleanup failed");
    copiedCursor=nullptr;++copyDestroyed;return result;
}
BOOL WINAPI probeDeleteObject(HGDIOBJ value) {
    if(value&&value==captureBitmap) {
        const BOOL result=DeleteObject(value);require(result!=FALSE,"Capture bitmap cleanup failed");
        captureBitmap=nullptr;++captureBitmapsDeleted;return result;
    }
    require(value&&(value==metadataMask||value==metadataColor),"Unexpected production GDI deletion");
    const BOOL result=DeleteObject(value);require(result!=FALSE,"Actual metadata bitmap cleanup failed");
    if(value==metadataMask)metadataMask=nullptr;if(value==metadataColor)metadataColor=nullptr;return result;
}
int main(){
    try {
        std::cout<<std::unitbuf;
        { Surface initializeGdi(1,1); }
        const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        int passed=0;passed+=caseRun(false);passed+=caseRun(true);
        earlyFailure(true);earlyFailure(false);
        captureToggle(16);captureToggle(8);
        const DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        // DrawIconEx may initialize cached process GDI resources. Assert actual
        // owned handle deletions, and use the broader count only as a diagnostic.
        require(!cleanupFailed,"Owned fixture resource cleanup failed");
        std::cout<<passed<<"/2 lifetime cases passed"<<"; 2/2 early-failure controls; 2/2 capture toggle cases passed"
            <<"; fixture GDI="<<before<<" -> "<<after<<"; no real cursor/display/input used.\n";
        return passed==2?0:1;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
}

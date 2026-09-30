#pragma once
#include "core.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

// Owned, synthetic content only. No capture APIs or user data. Real Windows
// fonts make the screen scenes materially different from pseudo-text bars.
namespace corpus {
constexpr int frameCount = 120, fps = 30, sceneCount = 4;
inline const char* sceneName(int scene) {
    static const char* names[] = {"static_screen", "scrolling_screen", "camera_motion", "frequent_cuts"};
    return scene >= 0 && scene < sceneCount ? names[scene] : "invalid";
}
struct Region { int x, y, w, h; };
inline Region textRegion(int width, int height) { return {252, 100, width - 280, height - 184}; }
inline uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16);
}
inline int noise(int x, int y, int seed) {
    return int(hash(uint32_t(x) * 0x9e3779b9u ^ uint32_t(y) * 0x85ebca6bu ^ uint32_t(seed)) & 255);
}
inline int smoothNoise(int x, int y, int scale, int seed) {
    const int ix = x / scale, iy = y / scale, dx = x % scale, dy = y % scale;
    return ((noise(ix,iy,seed)*(scale-dx)+noise(ix+1,iy,seed)*dx)*(scale-dy) +
            (noise(ix,iy+1,seed)*(scale-dx)+noise(ix+1,iy+1,seed)*dx)*dy)/(scale*scale);
}
class Surface {
    HDC dc_ = nullptr; HBITMAP bitmap_ = nullptr; HGDIOBJ old_ = nullptr; BYTE* pixels_ = nullptr;
    int width_, height_;
public:
    Surface(int w, int h) : width_(w), height_(h) {
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w; info.bmiHeader.biHeight=-h; info.bmiHeader.biPlanes=1;
        info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        dc_=CreateCompatibleDC(nullptr);
        bitmap_=CreateDIBSection(dc_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr,0);
        if (!dc_ || !bitmap_ || !pixels_) { if(bitmap_)DeleteObject(bitmap_); if(dc_)DeleteDC(dc_); throw std::runtime_error("Cannot create corpus DIB"); }
        old_=SelectObject(dc_,bitmap_); SetBkMode(dc_,TRANSPARENT);
    }
    ~Surface() { SelectObject(dc_,old_); DeleteObject(bitmap_); DeleteDC(dc_); }
    Surface(const Surface&)=delete; Surface& operator=(const Surface&)=delete;
    void rect(int x,int y,int w,int h,COLORREF color) {
        const RECT r{x,y,x+w,y+h}; const HBRUSH brush=CreateSolidBrush(color); FillRect(dc_,&r,brush); DeleteObject(brush);
    }
    void text(int x,int y,const std::wstring& str,int size,COLORREF color,bool mono=false) {
        HFONT font=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,
            mono?L"Consolas":L"Segoe UI");
        if(!font)throw std::runtime_error("Cannot create corpus font");
        const auto old=SelectObject(dc_,font); SetTextColor(dc_,color);
        TextOutW(dc_,x,y,str.c_str(),static_cast<int>(str.size())); SelectObject(dc_,old); DeleteObject(font);
    }
    lapse::Frame frame() const {
        GdiFlush(); lapse::Frame f{width_,height_,std::vector<uint8_t>(size_t(width_)*height_*4)};
        std::memcpy(f.pixels.data(),pixels_,f.pixels.size());
        for(size_t i=3;i<f.pixels.size();i+=4)f.pixels[i]=255; return f;
    }
};
inline lapse::Frame screen(int width,int height,bool dark) {
    Surface s(width,height);
    const auto bg=dark?RGB(28,31,37):RGB(249,250,252);
    const auto fg=dark?RGB(221,224,230):RGB(29,34,42);
    s.rect(0,0,width,height,bg); s.rect(0,0,width,44,RGB(26,32,45));
    s.text(20,11,L"SYNTHETIC WORKSPACE  /  Compression evaluation",17,RGB(235,240,250));
    s.rect(0,44,224,height-44,dark?RGB(36,40,48):RGB(232,237,244));
    s.text(18,70,L"PROJECT EXPLORER",13,fg);
    const wchar_t* files[]={L"Overview",L"src",L"  encoder.cpp",L"  settings.cpp",L"  preview.cpp",L"tests",L"  timing_tests.cpp",L"  quality_tests.cpp",L"Release notes",L"README.md"};
    for(int i=0;i<10;++i)s.text(18,105+i*28,files[i],14,fg);
    s.text(252,62,dark?L"Reviewing the dark theme":L"Reviewing compression and small text",21,fg);
    const std::array<COLORREF,6> colors=dark?std::array<COLORREF,6>{fg,RGB(234,139,115),RGB(110,178,250),RGB(131,200,120),RGB(222,154,222),fg}
        :std::array<COLORREF,6>{fg,RGB(187,36,34),RGB(28,91,202),RGB(15,128,54),RGB(153,44,165),fg};
    const wchar_t* lines[]={L"const auto frame = capture.next();  // timestamp 00:15:30.125",L"if (frame.valid()) writer.append(frame);",L"Small colored text: iIl1 | O0o  [] {} <>  .,;: /\\ 0123456789",L"The quick brown fox jumps over the lazy dog. 0123456789",L"Precision matters: x = 1.00025; duration = 333333 ticks;",L"Status: saved successfully. Background work remains idle."};
    for(int row=0,y=102;y<height-70;++row,y+=26) {
        const int size=10+(row%4)*2;
        s.text(252,y,std::to_wstring(row+1),12,dark?RGB(127,135,149):RGB(115,126,139),true);
        s.text(290,y,lines[row%6],size,colors[row%6],row%6<3);
    }
    const int cx=width-274,cy=height-152;
    s.rect(cx,cy,246,66,bg);
    for(int i=0;i<100;++i)s.rect(cx+i*2,cy+56-(i*13%49),1,1+i*13%49,colors[i%5]);
    s.text(cx,cy-20,L"Fine chart strokes / color edges",11,fg);
    s.rect(0,height-30,width,30,RGB(26,32,45));
    s.text(18,height-24,L"Synthetic content | Segoe UI / Consolas | 10, 12, 14, 16 px",12,RGB(215,225,240));
    return s.frame();
}
class Generator {
    int scene_,width_,height_;
    std::array<lapse::Frame,2> screens_;
    std::array<lapse::Frame,3> landscapes_;
    static uint8_t byte(int value) { return uint8_t(std::clamp(value,0,255)); }
    lapse::Frame landscape(int variant) {
        const int w=width_*2,h=height_; lapse::Frame f{w,h,std::vector<uint8_t>(size_t(w)*h*4)};
        for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
            auto* p=&f.pixels[(size_t(y)*w+x)*4];
            const int broad=smoothNoise(x,y,64,37+variant*101)-128;
            const int medium=smoothNoise(x,y,16,53+variant*99)-128;
            const int fine=noise(x,y,71+variant*91)-128;
            int r,g,b;
            if(y<h*2/5) { r=105+y*45/h+broad/9;g=158+y*45/h+broad/8;b=202+y*30/h+broad/7; }
            else { r=86+broad/3+medium/4+fine/9;g=112+broad/3+medium/3+fine/7;b=59+broad/5+medium/5+fine/10; }
            // Repeating distant structures provide high contrast and fine edges.
            const int building=(x+variant*139)%491;
            if(building<156 && y>h*2/7 && y<h*3/5) {
                r=154+medium/7;g=131+medium/8;b=107+medium/9;
                if(building%29<14 && (y-h*2/7)%35<18) {r=45;g=62;b=73;}
            }
            // Tree crowns and textured ground are nonuniform and multiscale.
            const int dx=(x+variant*97)%317-158,dy=y-h/2;
            if(dx*dx+dy*dy<110*110 && y<h*2/3) {r=41+medium/4+fine/10;g=77+broad/5+medium/3+fine/8;b=32+medium/5;}
            if(variant==1)std::swap(r,b);
            if(variant==2){r+=17;g-=13;b-=9;}
            p[0]=byte(b);p[1]=byte(g);p[2]=byte(r);p[3]=255;
        }
        return f;
    }
public:
    Generator(int scene,int width,int height):scene_(scene),width_(width),height_(height) {
        if(scene<0||scene>=sceneCount||width<640||width>4096||height<360||height>4096||(width&1)||(height&1))throw std::invalid_argument("Invalid corpus dimensions or scene");
        if(scene<2){screens_[0]=screen(width,height,false);screens_[1]=screen(width,height,true);}
        else for(int i=0;i<3;++i)landscapes_[i]=landscape(i);
    }
    lapse::Frame frame(int index) const {
        if(index<0)throw std::invalid_argument("Invalid corpus index");
        // Sparse real-world captures can change substantially every frame.
        // Scene 3 samples the same deterministic landscape 41 steps apart,
        // alternating scene cuts, large pans and noise without changing the
        // established pixels of scenes 0, 1 or 2.
        if(scene_==3) {
            if(index>(std::numeric_limits<int>::max)()/41)throw std::invalid_argument("Corpus index is too large");
            index*=41;
        }
        if(scene_<2) {
            const int variant=scene_==0?0:(index/40)%2;
            auto f=screens_[variant];
            if(scene_==1) {
                const int start=96,end=height_-56, span=end-start,offset=(index*17)%span;
                for(int y=start;y<end;++y) {
                    const auto* src=screens_[variant].pixels.data()+(size_t(start+(y-start+offset)%span)*width_+242)*4;
                    auto* dst=f.pixels.data()+(size_t(y)*width_+242)*4;
                    std::memcpy(dst,src,size_t(width_-254)*4);
                }
            }
            // Tiny status/caret changes; all other pixels remain exactly static.
            const int cx=width_-70, cy=height_-23;
            for(int y=cy;y<cy+12;++y)for(int x=cx;x<cx+32;++x) {
                auto* p=&f.pixels[(size_t(y)*width_+x)*4]; p[0]=uint8_t(80+(index/15)%2*90);p[1]=178;p[2]=64;
            }
            if((index/10)%2==0)for(int y=height_-90;y<height_-74;++y) {
                auto* p=&f.pixels[(size_t(y)*width_+534)*4];p[0]=p[1]=p[2]=variant?230:30;
            }
            return f;
        }
        const int variant=(index/40)%3,shift=(index*11)%(width_-1);
        const auto& src=landscapes_[variant];
        lapse::Frame f{width_,height_,std::vector<uint8_t>(size_t(width_)*height_*4)};
        for(int y=0;y<height_;++y)for(int x=0;x<width_;++x) {
            const auto* p=&src.pixels[(size_t(y)*src.width+x+shift)*4];auto* q=&f.pixels[(size_t(y)*width_+x)*4];
            const int n=int(hash(uint32_t(x)+uint32_t(y)*uint32_t(width_)+uint32_t(index)*0x9e3779b9u)%7)-3;
            const int drift=(index%40)/10-1;
            for(int c=0;c<3;++c)q[c]=byte(int(p[c])+n+drift);q[3]=255;
        }
        return f;
    }
};
// Convenience API. For measured runs construct Generator once outside timing.
inline lapse::Frame render(int scene,int index,int width,int height) {
    thread_local std::map<std::tuple<int,int,int>,std::unique_ptr<Generator>> cache;
    const auto key=std::make_tuple(scene,width,height);auto& generator=cache[key];
    if(!generator)generator=std::make_unique<Generator>(scene,width,height);
    return generator->frame(index);
}
inline std::vector<uint8_t> referenceNv12(const lapse::Frame& f) {
    const int w=f.width,h=f.height;
    if(!f.valid()||(w&1)||(h&1))throw std::invalid_argument("Invalid reference frame");
    std::vector<uint8_t> result(size_t(w)*h*3/2);auto* uv=result.data()+size_t(w)*h;
    for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2) {
        int r=0,g=0,b=0;
        for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
            const size_t k=size_t(y+dy)*w+x+dx;const auto* p=f.pixels.data()+k*4;
            result[k]=uint8_t(16+(11966*p[2]+40254*p[1]+4064*p[0]+32768)/65536);
            r+=p[2];g+=p[1];b+=p[0];
        }
        const size_t k=size_t(y/2)*w+x;
        uv[k]=uint8_t((128*262144-6596*r-22189*g+28784*b+131072)/262144);
        uv[k+1]=uint8_t((128*262144+28784*r-26145*g-2639*b+131072)/262144);
    }
    return result;
}
}

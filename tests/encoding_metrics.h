#pragma once
#include "encoding_corpus.h"
#include <limits>

namespace corpus {
struct Quality {
    double yPsnr=0,uPsnr=0,vPsnr=0,ySsim=0,textSsim=0,textEdgeMae=0,textRgbMae=0;
};
inline double psnr(double mse) { return mse==0?99:10*std::log10(255.0*255.0/mse); }
// Local, non-overlapping 8x8 SSIM. Population moments, K1=.01/K2=.03,
// L=255. This is explicitly block SSIM, not Gaussian-window/MS-SSIM/VMAF.
inline double blockSsim(const uint8_t* a,int aStride,const uint8_t* b,int bStride,Region roi) {
    double total=0;size_t blocks=0;
    for(int y=roi.y;y<roi.y+roi.h;y+=8)for(int x=roi.x;x<roi.x+roi.w;x+=8) {
        double sa=0,sb=0,saa=0,sbb=0,sab=0;int n=0;
        for(int dy=0;dy<8&&y+dy<roi.y+roi.h;++dy)for(int dx=0;dx<8&&x+dx<roi.x+roi.w;++dx) {
            const double av=a[size_t(y+dy)*aStride+x+dx],bv=b[size_t(y+dy)*bStride+x+dx];
            sa+=av;sb+=bv;saa+=av*av;sbb+=bv*bv;sab+=av*bv;++n;
        }
        const double ma=sa/n,mb=sb/n,va=std::max(0.0,saa/n-ma*ma),vb=std::max(0.0,sbb/n-mb*mb),cov=sab/n-ma*mb;
        constexpr double c1=6.5025,c2=58.5225;
        total+=((2*ma*mb+c1)*(2*cov+c2))/((ma*ma+mb*mb+c1)*(va+vb+c2));++blocks;
    }
    return total/blocks;
}
// Decode buffer planes MUST be passed separately: padded decoder height means
// the UV plane is not necessarily at y + stride*visibleHeight. Caller owns the
// IMF2DBuffer lock and checks actual coded-height/current buffer size.
inline Quality measure(const lapse::Frame& source,const uint8_t* decodedY,int yStride,
                       const uint8_t* decodedUv,int uvStride,bool includeText) {
    const int w=source.width,h=source.height;
    if(!decodedY||!decodedUv||yStride<w||uvStride<w)throw std::invalid_argument("Invalid decoded planes");
    const auto ref=referenceNv12(source);const auto* ry=ref.data();const auto* ruv=ry+size_t(w)*h;
    double yError=0,uError=0,vError=0;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {const double d=int(ry[size_t(y)*w+x])-int(decodedY[size_t(y)*yStride+x]);yError+=d*d;}
    for(int y=0;y<h/2;++y)for(int x=0;x<w;x+=2) {
        const double du=int(ruv[size_t(y)*w+x])-int(decodedUv[size_t(y)*uvStride+x]);
        const double dv=int(ruv[size_t(y)*w+x+1])-int(decodedUv[size_t(y)*uvStride+x+1]);uError+=du*du;vError+=dv*dv;
    }
    Quality q;q.yPsnr=psnr(yError/(double(w)*h));q.uPsnr=psnr(uError/(double(w)*h/4));q.vPsnr=psnr(vError/(double(w)*h/4));
    q.ySsim=blockSsim(ry,w,decodedY,yStride,{0,0,w,h});
    if(!includeText){q.textSsim=q.textEdgeMae=q.textRgbMae=std::numeric_limits<double>::quiet_NaN();return q;}
    const auto roi=textRegion(w,h);q.textSsim=blockSsim(ry,w,decodedY,yStride,roi);
    double edgeError=0,rgbError=0;size_t edges=0,pixels=0;
    for(int y=roi.y;y<roi.y+roi.h;++y)for(int x=roi.x;x<roi.x+roi.w;++x) {
        const int a=ry[size_t(y)*w+x],b=decodedY[size_t(y)*yStride+x];
        // Restrict gradient-error scoring to actual reference edges. Blank UI
        // background cannot drown out distorted small glyphs in this metric.
        for(int axis=0;axis<2;++axis) {
            const int dx=axis==0?1:0,dy=axis==0?0:1;
            if(x+dx>=roi.x+roi.w||y+dy>=roi.y+roi.h)continue;
            const int ga=int(ry[size_t(y+dy)*w+x+dx])-a;
            if(std::abs(ga)<12)continue;
            const int gb=int(decodedY[size_t(y+dy)*yStride+x+dx])-b;edgeError+=std::abs(ga-gb);++edges;
        }
        const auto* p=&source.pixels[(size_t(y)*w+x)*4];
        // End-to-end RGB text ROI error includes unavoidable NV12 4:2:0 loss.
        // Nearest 2x2 chroma reconstruction is explicit and identical per mode.
        const int uvx=x&~1;const double yy=(b-16)*255.0/219.0;
        const double cb=(int(decodedUv[size_t(y/2)*uvStride+uvx])-128)*255.0/224.0;
        const double cr=(int(decodedUv[size_t(y/2)*uvStride+uvx+1])-128)*255.0/224.0;
        const double rr=std::clamp(yy+1.5748*cr,0.0,255.0),gg=std::clamp(yy-0.187324*cb-0.468124*cr,0.0,255.0),bb=std::clamp(yy+1.8556*cb,0.0,255.0);
        rgbError+=std::abs(rr-p[2])+std::abs(gg-p[1])+std::abs(bb-p[0]);++pixels;
    }
    q.textEdgeMae=edges?edgeError/edges:0;q.textRgbMae=rgbError/(pixels*3);return q;
}
}

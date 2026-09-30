// Reuse the actual-handler fixture and its 26 existing regressions. All native
// windows are owned and hidden; capture/focus/shift are fixture-only substitutes.
#include <windows.h>
namespace {
bool inspectGripPaint=false;
RECT paintedGrip{};
int paintedGrips=0;
int WINAPI fixtureFillRect(HDC dc,const RECT* rect,HBRUSH brush);
}
#define FillRect fixtureFillRect
#define main collageRegressionMain
#include "ui_collage_tests.cpp"
#undef main
#undef FillRect

namespace {
int WINAPI fixtureFillRect(HDC dc,const RECT* rect,HBRUSH brush) {
    LOGBRUSH description{};
    if(inspectGripPaint&&GetObjectW(brush,sizeof(description),&description)==sizeof(description)&&description.lbColor==RGB(83,229,205)){
        paintedGrip=*rect;++paintedGrips;
    }
    return FillRect(dc,rect,brush);
}
bool equalPixels(const RECT& a, const RECT& b) { return EqualRect(&a,&b)!=FALSE; }
void pixels(const RECT& r) { std::cout << '[' << r.left << ',' << r.top << ',' << r.right << ',' << r.bottom << ']'; }
struct GeometryCase {
    const char* label;
    int outputWidth, outputHeight, clientWidth, clientHeight;
    Rect layer;
};
const GeometryCase projectionCases[] = {
    {"portrait48x4096-min",48,4096,800,450,{.2,.12,.1,.1}},
    {"landscape4096x48-min",4096,48,800,450,{.2,.12,.1,.1}},
    {"portrait-right-bottom",48,4096,800,450,{.9,.9,.1,.1}},
    {"landscape-right-bottom",4096,48,800,450,{.9,.9,.1,.1}},
    {"ordinary1280x720",1280,720,800,450,{.2,.3,.4,.3}},
    {"portrait1080x1920",1080,1920,800,450,{.2,.3,.1,.1}},
    {"square512",512,512,800,450,{.2,.3,.1,.1}},
    {"one-physical-pixel",48,4096,1,1,{.2,.12,.1,.1}},
    {"two-by-two-physical",512,512,6,6,{0,0,.4,.4}}
};
const GeometryCase gestureCases[] = {
    {"portrait-thin-min",48,4096,800,450,{.2,.12,.1,.1}},
    // This is nonempty even on the baseline; at 288 DPI the fixed 54px grip
    // covers its complete 1x45 rectangle. This independently proves grip failure.
    {"portrait-nonempty-grip",48,4096,800,450,{.2,.12,.3,.1}},
    {"landscape-thin-min",4096,48,800,450,{.2,.12,.1,.1}},
    {"landscape-nonempty-grip",4096,48,800,450,{.2,.2,.1,.2}},
    {"ordinary1280x720",1280,720,800,450,{.2,.3,.4,.3}},
    {"portrait1080x1920",1080,1920,800,450,{.2,.3,.1,.1}},
    {"square512",512,512,800,450,{.2,.3,.1,.1}},
    {"one-physical-pixel",48,4096,1,1,{.2,.12,.1,.1}},
    {"two-by-two-physical",512,512,6,6,{0,0,.4,.4}}
};
RECT prepare(const GeometryCase& geometry, int dpi) {
    reset();
    app.dpi=dpi;
    app.hasCustomSize=true;app.customWidth=geometry.outputWidth;app.customHeight=geometry.outputHeight;
    choose(app.videoSize,2);app.settings.width=geometry.outputWidth;app.settings.height=geometry.outputHeight;
    app.settings.separateFiles=false;app.settings.layers[1].rect=geometry.layer;
    require(SetWindowPos(app.preview,nullptr,0,0,geometry.clientWidth,geometry.clientHeight,
        SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"Could not size owned hidden preview.");
    RECT client{};require(GetClientRect(app.preview,&client)!=FALSE,"Missing owned preview client.");
    const auto layers=app.settings.layers;
    const auto fitted=previewVideoRect(client);
    // Independent fixture expectations for the unchanged exact-aspect fit.
    const int fittedWidth=geometry.clientWidth==1?1:geometry.clientWidth==6?6:
        geometry.outputWidth==48?5:geometry.outputWidth==4096?800:geometry.outputWidth==1280?800:
        geometry.outputWidth==1080?253:450;
    const int fittedHeight=geometry.clientHeight==1?1:geometry.clientHeight==6?6:
        geometry.outputWidth==4096?9:450;
    const RECT expectedCanvas={(geometry.clientWidth-fittedWidth)/2,(geometry.clientHeight-fittedHeight)/2,
        (geometry.clientWidth+fittedWidth)/2,(geometry.clientHeight+fittedHeight)/2};
    require(equalPixels(fitted,expectedCanvas),"Exact output aspect fit changed.");
    // Exercise the actual paint path as well as its geometry helper. No cached
    // frame, camera, engine, physical input or real focus/capture is involved.
    InvalidateRect(app.preview,nullptr,FALSE);
    previewProc(app.preview,WM_PAINT,0,0);
    require(equalPixels(app.videoRect,fitted),"Paint changed the exact fitted output canvas.");
    require(sameLayers(app.settings.layers,layers),"Projection mutated normalized layer data.");
    require(!IsWindowVisible(app.window)&&!IsWindowVisible(app.preview),"Fixture became visible.");
    return fitted;
}
bool projectionCase(const GeometryCase& geometry,int dpi) {
    const auto canvas=prepare(geometry,dpi);
    const auto normalized=app.settings.layers;
    const auto bounds=layerRect(app.settings.layers[1]);
    const bool nonempty=bounds.left<bounds.right&&bounds.top<bounds.bottom;
    const bool inside=bounds.left>=canvas.left&&bounds.top>=canvas.top&&bounds.right<=canvas.right&&bounds.bottom<=canvas.bottom;
    // Native selection must find the camera even if the baseline projection is
    // empty. Use its nominal first pixel so the baseline actually dispatches.
    const int x=bounds.left,y=bounds.top;
    previewProc(app.preview,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    const bool selected=app.selected==1&&app.dragging&&ownedCapture==app.preview;
    const bool unchanged=sameLayers(app.settings.layers,normalized)&&equalPixels(app.videoRect,canvas);
    previewProc(app.preview,WM_LBUTTONUP,0,0);
    const bool correct=nonempty&&inside&&selected&&unchanged&&!app.dragging&&!ownedCapture;
    std::cout << (correct?"PASS ":"FAIL ") << "projection " << geometry.label << " dpi=" << dpi << " canvas=";
    pixels(canvas);std::cout << " layer=";pixels(bounds);
    std::cout << " nonempty=" << nonempty << " selected=" << selected << '\n';
    return correct;
}
bool gestureCase(const GeometryCase& geometry,int dpi,bool corner) {
    const auto canvas=prepare(geometry,dpi);
    const auto original=app.settings.layers[1].rect;
    const auto bounds=layerRect(app.settings.layers[1]);
    const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    // Choose the first of the two central pixels when an even-sized box has no
    // single physical center. 2x2 has a top-left move point and bottom-right
    // resize point, but its floor((left+right)/2) midpoint is that corner.
    const int x=corner?bounds.right-1:bounds.left+std::max(0,width-1)/2;
    const int y=corner?bounds.bottom-1:bounds.top+std::max(0,height-1)/2;
    const bool physicalOnePixel=width==1&&height==1;
    const bool wantResize=corner&&!physicalOnePixel;
    previewProc(app.preview,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    const bool admitted=app.selected==1&&app.dragging&&ownedCapture==app.preview;
    const bool classified=admitted&&app.resizing==wantResize;
    bool boundedPaint=false;
    if(admitted){
        paintedGrips=0;inspectGripPaint=true;
        InvalidateRect(app.preview,nullptr,FALSE);previewProc(app.preview,WM_PAINT,0,0);
        inspectGripPaint=false;
        const int gripWidth=paintedGrip.right-paintedGrip.left,gripHeight=paintedGrip.bottom-paintedGrip.top;
        boundedPaint=paintedGrips==1&&paintedGrip.left>=bounds.left&&paintedGrip.top>=bounds.top&&
            paintedGrip.right<=bounds.right&&paintedGrip.bottom<=bounds.bottom&&
            gripWidth<=std::max(1,width/3)&&gripHeight<=std::max(1,height/3)&&
            (physicalOnePixel?(gripWidth==0&&gripHeight==0):(gripWidth>0&&gripHeight>0));
    }
    // One physical pixel cannot distinguish body and corner. Its pointer edits
    // move the layer, and Shift+arrow remains an explicit resize route.
    const int dx=1,dy=1;
    previewProc(app.preview,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+dx,y+dy));
    Rect expected=original;
    const double normalizedX=double(dx)/(canvas.right-canvas.left),normalizedY=double(dy)/(canvas.bottom-canvas.top);
    if(wantResize){expected.w=std::clamp(expected.w+normalizedX,.1,std::max(.1,1.0-expected.x));expected.h=std::clamp(expected.h+normalizedY,.1,std::max(.1,1.0-expected.y));}
    else{expected.x+=normalizedX;expected.y+=normalizedY;}
    expected=constrain(expected);
    const bool edited=same(app.settings.layers[1].rect,expected)&&same(app.settings.layers[0].rect,{0,0,1,1});
    const bool canvasPreserved=equalPixels(app.videoRect,canvas)&&app.settings.width==geometry.outputWidth&&app.settings.height==geometry.outputHeight;
    previewProc(app.preview,WM_LBUTTONUP,0,0);
    bool keyboard=true;
    if(physicalOnePixel&&admitted){
        const auto pointer=app.settings.layers[1].rect;
        // The +1px move fills this canvas and clamps x at its right boundary.
        // Move left first so Shift+Right must produce a real width increase.
        previewProc(app.preview,WM_KEYDOWN,VK_LEFT,0);
        Rect keyExpected=pointer;keyExpected.x-=.01;keyExpected=constrain(keyExpected);
        keyboard=same(app.settings.layers[1].rect,keyExpected);
        shiftDown=true;previewProc(app.preview,WM_KEYDOWN,VK_RIGHT,0);shiftDown=false;
        keyExpected.w=std::clamp(keyExpected.w+.01,.1,std::max(.1,1.0-keyExpected.x));
        keyboard=keyboard&&same(app.settings.layers[1].rect,constrain(keyExpected))&&app.settings.layers[1].rect.w>pointer.w+.005;
    }
    const bool correct=admitted&&classified&&boundedPaint&&edited&&canvasPreserved&&keyboard&&!app.dragging&&!ownedCapture;
    std::cout << (correct?"PASS ":"FAIL ") << (corner?"corner ":"center ") << geometry.label << " dpi=" << dpi << " layer=";
    pixels(bounds);std::cout << " camera=" << admitted << " resize=" << app.resizing << " expected_resize=" << wantResize << " bounded_paint=" << boundedPaint << " normalized=";
    print(app.settings.layers[1].rect);std::cout << '\n';
    return correct;
}
int runThin(const std::string& selection) {
    HiddenFixture fixture;
    int passed=0,total=0;
    for(int dpi:{96,144,192,288}) {
        if(selection=="all"||selection=="projection") for(const auto& geometry:projectionCases){++total;passed+=projectionCase(geometry,dpi);}
        if(selection=="all"||selection=="gestures") for(const auto& geometry:gestureCases) for(bool corner:{false,true}){++total;passed+=gestureCase(geometry,dpi,corner);}
        if(selection=="grip") {++total;passed+=gestureCase(gestureCases[1],dpi,false);}
    }
    require(!IsWindowVisible(app.window)&&!IsWindowVisible(app.preview)&&!ownedCapture,"Fixture escaped hidden scope.");
    std::cout << "Thin native layer " << selection << ": " << passed << '/' << total << " passed.\n";
    return passed==total?0:1;
}
}
int main(int argc,char** argv) {
    std::cout << std::unitbuf;
    const std::string selection=argc>1?argv[1]:"all";
    try {
        if(selection=="regressions")return collageRegressionMain();
        require(selection=="all"||selection=="projection"||selection=="gestures"||selection=="grip","Unknown fixture selection.");
        const int thin=runThin(selection);
        if(selection=="all"){
            // Recreate the original regression fixture's default settings after
            // the custom-size proof; its own window is freshly constructed.
            app.settings={};app.hasCustomSize=app.hasCustomInterval=app.hasCustomLimit=false;
            return thin?thin:collageRegressionMain();
        }
        return thin;
    } catch(const std::exception& error){std::cerr << "HARNESS ERROR: " << error.what() << '\n';return 2;}
}

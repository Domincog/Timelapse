// Includes the actual native handler with an inert engine. Only owned hidden
// controls are created. Focus, capture and modifier state never reach the user.
#include "engine.h"
#include "capture.h"
#include "camera_host.h"
#include <mfapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace lapse {
class FixtureEngine {
public:
    void configure(const Settings&) {}
    void refreshSources() {}
    void record() {}
    void pause() {}
    void setPaused(bool) {}
    void finish() {}
    Status status() { return {}; }
};
std::vector<Monitor> enumerateMonitors() { throw std::runtime_error("Unexpected device enumeration."); }
std::vector<CameraDevice> enumerateCameras(std::wstring&) { throw std::runtime_error("Unexpected camera enumeration."); }
int runCameraHost(const wchar_t*) { throw std::runtime_error("Unexpected application entry."); }
}
namespace {
HWND ownedCapture = nullptr;
bool shiftDown = false, releasedWhileDragging = false;
int releases = 0;
HWND WINAPI fixtureSetFocus(HWND) { return nullptr; }
HWND WINAPI fixtureSetCapture(HWND window) { const auto before = ownedCapture; ownedCapture = window; return before; }
HWND WINAPI fixtureGetCapture() { return ownedCapture; }
BOOL WINAPI fixtureReleaseCapture();
SHORT WINAPI fixtureGetKeyState(int key) { return key == VK_SHIFT && shiftDown ? -32768 : 0; }
}
#define Engine FixtureEngine
#define SetFocus fixtureSetFocus
#define SetCapture fixtureSetCapture
#define GetCapture fixtureGetCapture
#define ReleaseCapture fixtureReleaseCapture
#define GetKeyState fixtureGetKeyState
// The reject-entry sentinel intentionally makes the GUI entry unreachable.
#pragma warning(push)
#pragma warning(disable: 4702)
#include "ui_person_pack_stub.h"
#include "../src/main.cpp"
#pragma warning(pop)
#undef Engine
#undef SetFocus
#undef SetCapture
#undef GetCapture
#undef ReleaseCapture
#undef GetKeyState

namespace {
BOOL WINAPI fixtureReleaseCapture() {
    ++releases;
    releasedWhileDragging |= app.dragging;
    const auto previous = ownedCapture;
    ownedCapture = nullptr;
    if (previous == app.preview) previewProc(app.preview, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
bool same(const Rect& a, const Rect& b) {
    return std::abs(a.x-b.x)<1e-8 && std::abs(a.y-b.y)<1e-8 && std::abs(a.w-b.w)<1e-8 && std::abs(a.h-b.h)<1e-8;
}
void print(const Rect& rect) { std::cout << '(' << rect.x << ',' << rect.y << ',' << rect.w << ',' << rect.h << ')'; }
struct HiddenFixture {
    HiddenFixture() {
        app.dpi = 96;
        app.window = CreateWindowExW(0, L"STATIC", L"Native collage owned fixture", WS_OVERLAPPED,
            0, 0, 920, 720, nullptr, nullptr, nullptr, nullptr);
        require(app.window != nullptr, "Could not create hidden parent.");
        WNDCLASSW cls{}; cls.lpfnWndProc = previewProc; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"NativeCollageReviewPreview";
        require(RegisterClassW(&cls) != 0, "Could not register owned preview class.");
        app.preview = CreateWindowExW(0, cls.lpszClassName, L"", WS_CHILD | WS_TABSTOP,
            0, 0, 800, 450, app.window, nullptr, cls.hInstance, nullptr);
        require(app.preview != nullptr, "Could not create hidden preview.");
        auto combo = [&]() {
            HWND handle = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
                0, 0, 100, 100, app.window, nullptr, nullptr, nullptr);
            require(handle != nullptr, "Could not create hidden combo.");
            for (int i=0; i<6; ++i) add(handle, std::to_wstring(i));
            choose(handle, 0); return handle;
        };
        app.mode=combo(); app.interval=combo(); app.videoSize=combo(); app.encodingQuality=combo();
        app.monitor=combo(); app.camera=combo();
        require(!IsWindowVisible(app.window) && !IsWindowVisible(app.preview), "Fixture became visible.");
    }
    ~HiddenFixture() {
        ownedCapture = nullptr;
        if (app.window) DestroyWindow(app.window);
        app.window = app.preview = nullptr;
        UnregisterClassW(L"NativeCollageReviewPreview", GetModuleHandleW(nullptr));
    }
};
void reset() {
    app.videoRect = {0, 0, 800, 450};
    app.settings.layers = {{Source::Desktop,{0,0,1,1}}, {Source::Camera,{.20,.30,.40,.30}}};
    app.modeIndex = static_cast<int>(Mode::Custom); choose(app.mode, app.modeIndex);
    app.selected = -1; app.dragging = app.resizing = false;
    shiftDown = releasedWhileDragging = false; ownedCapture = nullptr; releases = 0;
}
struct Gesture { int x, y; };
Gesture begin(bool resize) {
    reset();
    const auto bounds = layerRect(app.settings.layers[1]);
    Gesture result{resize ? bounds.right-1 : (bounds.left+bounds.right)/2,
                   resize ? bounds.bottom-1 : (bounds.top+bounds.bottom)/2};
    previewProc(app.preview, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(result.x,result.y));
    require(app.selected==1 && app.dragging && app.resizing==resize && ownedCapture==app.preview,
            "Gesture did not capture the camera.");
    previewProc(app.preview, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(result.x+40,result.y+18));
    return result;
}
bool freshDrag(const Rect& edited) {
    const auto bounds = layerRect(app.settings.layers[1]);
    const int x=(bounds.left+bounds.right)/2, y=(bounds.top+bounds.bottom)/2;
    previewProc(app.preview, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x,y));
    previewProc(app.preview, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x+8,y+9));
    const bool valid = app.selected==1 && same(app.settings.layers[1].rect,
        {edited.x+.01,edited.y+.02,edited.w,edited.h});
    previewProc(app.preview, WM_LBUTTONUP, 0, 0);
    return valid && !app.dragging && !ownedCapture;
}
bool spaceCase(bool resize) {
    const auto start=begin(resize);
    const auto camera=app.settings.layers[1].rect;
    MSG message{}; message.hwnd=app.preview; message.message=WM_KEYDOWN; message.wParam=VK_SPACE;
    const bool consumed=IsDialogMessageW(app.window,&message)!=FALSE;
    if (!consumed) DispatchMessageW(&message);
    require(app.selected==0, "Space did not reach the actual handler via dialog routing.");
    const bool stopped=!app.dragging && !ownedCapture && releases==1 && !releasedWhileDragging;
    previewProc(app.preview,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(start.x+80,start.y+36));
    bool correct=stopped && same(app.settings.layers[0].rect,{0,0,1,1}) && same(app.settings.layers[1].rect,camera);
    std::cout << (correct?"PASS ":"FAIL ") << (resize?"resize":"move") << " + Space: routed=1 desktop=";
    print(app.settings.layers[0].rect); std::cout << "\n";
    if (correct) correct=freshDrag(camera);
    return correct;
}
bool arrowCase(bool resize, bool shift, WPARAM key) {
    const auto start=begin(resize);
    const auto pointer=app.settings.layers[1].rect;
    shiftDown=true; previewProc(app.preview,WM_KEYDOWN,VK_SHIFT,0);
    require(app.dragging && ownedCapture==app.preview && same(app.settings.layers[1].rect,pointer),
            "Shift alone changed the gesture.");
    shiftDown=shift; previewProc(app.preview,WM_KEYDOWN,key,0); shiftDown=false;
    Rect keyboard=pointer;
    const double dx=key==VK_LEFT?-.01:key==VK_RIGHT?.01:0, dy=key==VK_UP?-.01:key==VK_DOWN?.01:0;
    if (shift) { keyboard.w+=dx; keyboard.h+=dy; } else { keyboard.x+=dx; keyboard.y+=dy; }
    require(same(app.settings.layers[1].rect,keyboard), "Key did not initially edit the last pointer rectangle.");
    const bool stopped=!app.dragging && !ownedCapture && releases==1 && !releasedWhileDragging;
    previewProc(app.preview,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(start.x+40,start.y+18));
    const bool repeatedPreserved=same(app.settings.layers[1].rect,keyboard);
    previewProc(app.preview,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(start.x+80,start.y+36));
    bool correct=stopped && repeatedPreserved && same(app.settings.layers[1].rect,keyboard) && same(app.settings.layers[0].rect,{0,0,1,1});
    std::cout << (correct?"PASS ":"FAIL ") << (resize?"resize":"move") << " + " << (shift?"Shift+":"")
        << "arrow " << key << ": keyboard="; print(keyboard); std::cout << " after_mouse="; print(app.settings.layers[1].rect); std::cout << '\n';
    if (correct) correct=freshDrag(keyboard);
    return correct;
}
bool ownershipCase() {
    begin(false); ownedCapture=app.window;
    previewProc(app.preview,WM_KEYDOWN,VK_SPACE,0);
    const bool correct=!app.dragging && ownedCapture==app.window && releases==0;
    std::cout << (correct?"PASS":"FAIL") << " foreign capture is preserved while the gesture ends\n";
    ownedCapture=nullptr; return correct;
}
bool sameLayers(const std::vector<Layer>& a, const std::vector<Layer>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i=0; i<a.size(); ++i)
        if (a[i].source != b[i].source || !same(a[i].rect,b[i].rect)) return false;
    return true;
}
void selectMode(Mode mode) {
    choose(app.mode,static_cast<int>(mode));
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(ModeBox,CBN_SELCHANGE),reinterpret_cast<LPARAM>(app.mode));
}
void editPreset() {
    previewProc(app.preview,WM_KEYDOWN,VK_SPACE,0);
    require(app.selected==0,"Space did not select the first preset layer.");
    const auto before=app.settings.layers;
    shiftDown=true;
    previewProc(app.preview,WM_KEYDOWN,VK_LEFT,0);
    shiftDown=false;
    require(app.modeIndex==static_cast<int>(Mode::Custom) && choice(app.mode)==static_cast<int>(Mode::Custom) &&
            !sameLayers(app.settings.layers,before),"Arrow did not customize the preset.");
}
bool resetTo(Mode expected, const char* label) {
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(Reset,BN_CLICKED),0);
    const bool correct=sameLayers(app.settings.layers,preset(expected)) &&
        app.modeIndex==static_cast<int>(expected) && choice(app.mode)==static_cast<int>(expected) && app.selected==-1;
    std::cout << (correct?"PASS ":"FAIL ") << label << ": mode=" << app.modeIndex << " layers=";
    for (const auto& layer:app.settings.layers) { std::cout << static_cast<int>(layer.source); print(layer.rect); }
    std::cout << '\n';
    return correct;
}
bool resetPresetCase(Mode mode, bool edit) {
    reset(); selectMode(mode);
    require(sameLayers(app.settings.layers,preset(mode)),"Source choice did not install its preset.");
    if (edit) editPreset();
    return resetTo(mode,mode==Mode::SideBySide?(edit?"edited SideBySide reset":"untouched SideBySide reset"):
        "edited Overlay reset after previous SideBySide");
}
bool explicitCustomCase() {
    reset(); selectMode(Mode::SideBySide); selectMode(Mode::Custom);
    require(sameLayers(app.settings.layers,preset(Mode::SideBySide)),"Custom selection replaced an existing collage.");
    editPreset();
    return resetTo(Mode::SideBySide,"explicit Custom keeps existing SideBySide preset");
}
bool newCustomCase(Mode single) {
    reset(); selectMode(Mode::SideBySide); selectMode(single); selectMode(Mode::Custom);
    require(sameLayers(app.settings.layers,preset(Mode::Overlay)),"New Custom did not seed the default overlay.");
    editPreset();
    return resetTo(Mode::Overlay,single==Mode::Desktop?"Desktop to new Custom resets to Overlay":
        "Camera to new Custom resets to Overlay");
}
bool reorderedPresetCase() {
    reset(); selectMode(Mode::SideBySide);
    previewProc(app.preview,WM_KEYDOWN,VK_SPACE,0);
    windowProc(app.window,WM_COMMAND,MAKEWPARAM(Forward,BN_CLICKED),0);
    require(app.modeIndex==static_cast<int>(Mode::Custom) && app.settings.layers[0].source==Source::Camera &&
            app.settings.layers[1].source==Source::Desktop,"Bring forward did not customize the layer order.");
    return resetTo(Mode::SideBySide,"reordered SideBySide resets geometry and layer order");
}
}

int main() {
    std::cout << std::unitbuf;
    try {
        HiddenFixture fixture;
        require((previewProc(app.preview,WM_GETDLGCODE,0,0)&DLGC_WANTARROWS)!=0, "Preview does not request arrow keys.");
        int passed=0, total=0;
        for (bool resize : {false,true}) {
            ++total; passed+=spaceCase(resize);
            for (bool shift : {false,true}) for (WPARAM key : {VK_LEFT,VK_RIGHT,VK_UP,VK_DOWN}) {
                ++total; passed+=arrowCase(resize,shift,key);
            }
        }
        ++total; passed+=ownershipCase();
        ++total; passed+=resetPresetCase(Mode::SideBySide,false);
        ++total; passed+=resetPresetCase(Mode::SideBySide,true);
        ++total; passed+=resetPresetCase(Mode::Overlay,true);
        ++total; passed+=explicitCustomCase();
        ++total; passed+=newCustomCase(Mode::Desktop);
        ++total; passed+=newCustomCase(Mode::Camera);
        ++total; passed+=reorderedPresetCase();
        require(!IsWindowVisible(app.window)&&!IsWindowVisible(app.preview)&&!ownedCapture, "Fixture escaped hidden scope.");
        std::cout << "Native collage keyboard handoff: " << passed << '/' << total << " passed.\n";
        return passed==total?0:1;
    } catch(const std::exception& error) { std::cerr << "HARNESS ERROR: " << error.what() << '\n'; return 2; }
}

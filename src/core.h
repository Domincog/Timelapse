#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <limits>

namespace lapse {
// All frames are tightly packed, top-down BGRA (alpha ignored), sRGB.
struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
    bool valid() const {
        return width > 0 && height > 0 &&
            size_t(width) <= (std::numeric_limits<size_t>::max)() / 4 / size_t(height) &&
            pixels.size() == size_t(width) * size_t(height) * 4;
    }
};
struct Rect { double x = 0, y = 0, w = 1, h = 1; };
enum class Source { Desktop, Camera };
enum class Mode { Desktop, Camera, Overlay, SideBySide, Custom };
struct Layer { Source source; Rect rect; };
std::vector<Layer> preset(Mode mode);
Rect constrain(Rect rect);
// Fits the complete source within the layer; uncovered space is dark.
// Later layers cover earlier layers. On failure, output is unchanged.
// Output uses even dimensions, up to 1920 x 1080.
bool compose(const Frame* desktop, const Frame* camera, const std::vector<Layer>& layers,
             int width, int height, Frame& output, std::wstring& error);
std::wstring errorText(HRESULT hr);
}

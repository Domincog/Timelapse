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
enum class EncodingQuality { Compact, Balanced, Detail };
// Values are saved in preferences; append new modes only.
enum class EncodingMode { Compatible, Efficient, HardwareH264, HardwareHEVC, QualityH264, SoftwareAV1 };
struct Layer { Source source; Rect rect; };
std::vector<Layer> preset(Mode mode);
Rect constrain(Rect rect);
// Fits the complete source within the layer; uncovered space is dark.
// Later layers cover earlier layers. On failure, output is unchanged.
// Primitive output uses even dimensions 2..4096 with at most 8,847,360 pixels.
// Recording has a separate 48-pixel minimum for ordinary codec playback.
bool compose(const Frame* desktop, const Frame* camera, const std::vector<Layer>& layers,
             int width, int height, Frame& output, std::wstring& error);
std::wstring errorText(HRESULT hr);
// Prepare ordinary absolute paths for Windows file I/O without changing their
// user-visible spelling. Relative and device paths retain their existing form.
std::wstring fileIOPath(const std::wstring& path);
}

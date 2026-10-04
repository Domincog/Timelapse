#pragma once
#include "core.h"
#include <memory>

namespace lapse {
// Shared drawing for text burned into video frames (watermark, status feed).
// Nothing draws a box: legibility comes from a dark halo (tight outline plus
// soft drop shadow) derived from the ink, so overlays read on any content.
struct OverlayColor { uint8_t r = 255, g = 255, b = 255; };
// Fixed-capacity tile with a premultiplied colour ink layer over a dark halo
// alpha layer. All buffers are owned after reserve(); drawing, halo and
// compositing never allocate. Single-thread owned.
class OverlayCanvas {
public:
    OverlayCanvas() noexcept = default;
    OverlayCanvas(const OverlayCanvas&) = delete;
    OverlayCanvas& operator=(const OverlayCanvas&) = delete;
    // Keeps a larger existing reservation. On failure the canvas is released.
    bool reserve(int maxWidth, int maxHeight, std::wstring& error);
    void release() noexcept;
    bool reserved() const noexcept { return capacityWidth_ > 0; }
    // Start drawing a width x height tile (within the reservation); clears
    // everything the previous tile touched. Invalid sizes leave it empty.
    bool begin(int width, int height) noexcept;
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    // Coverage from a white-on-black BGRA raster (channel average), placed at
    // x/y and clipped to the tile. Each tile pixel averages sample x sample
    // raster pixels (sourceHeight rows exist). Opacity is 0..255.
    void coverage(const uint8_t* bgra, int strideBytes, int maskWidth, int maskHeight, int sample, int sourceHeight,
                  int x, int y, OverlayColor color, int opacity) noexcept;
    // 4x4 supersampled shapes. Ring sweeps clockwise from 12 o'clock in turns;
    // a sweep of 1 or more draws the whole ring. Lines have round caps.
    void disc(double cx, double cy, double radius, OverlayColor color, int opacity) noexcept;
    void ring(double cx, double cy, double radius, double thickness, double startTurn, double sweepTurns,
              OverlayColor color, int opacity) noexcept;
    void line(double x0, double y0, double x1, double y1, double thickness, OverlayColor color, int opacity) noexcept;
    // Dark backdrop strip in the halo layer whose alpha falls linearly from
    // `alpha` at x=solidX to zero at x=clearX (either direction).
    void fade(int solidX, int clearX, int top, int bottom, int alpha) noexcept;
    // Halo = max(existing, outlineAlpha * dilated ink, shadowAlpha * blurred
    // ink moved down by offsetY). Dilation/blur radii are in pixels.
    void halo(int outlineRadius, int outlineAlpha, int blurRadius, int shadowAlpha, int offsetY) noexcept;
    // Darken by the halo, then lay the ink over it. The tile is placed at
    // left/top of a logical outputWidth x outputHeight image; a smaller frame
    // (disposable preview) is area-sampled. Only touched pixels change and
    // their alpha becomes 255. Bounds are in frame pixels, empty if nothing
    // was drawn. On failure the frame is unchanged.
    bool composite(Frame& frame, int left, int top, int outputWidth, int outputHeight, RECT& bounds) const noexcept;
    // Tile-local rectangle of everything drawn since begin(); empty if none.
    RECT touched() const noexcept { return touched_; }
private:
    void paint(int x, int y, int alpha, OverlayColor color) noexcept;
    void mark(int left, int top, int right, int bottom) noexcept;
    template<class Inside> void shape(double left, double top, double right, double bottom, OverlayColor color, int opacity, Inside inside) noexcept;
    std::unique_ptr<uint8_t[]> ink_, halo_, first_, second_, third_;
    int capacityWidth_ = 0, capacityHeight_ = 0, width_ = 0, height_ = 0;
    RECT touched_{};
};
// One GDI memory DC, font and one-line white-on-black raster. Uses DrawTextW,
// so user text gets Windows font fallback and complex-script shaping. Text is
// drawn at twice the size and averaged down, which avoids GDI's hinted,
// stair-stepped antialiasing. Public sizes are in output pixels.
class OverlayFont {
public:
    OverlayFont() noexcept = default;
    ~OverlayFont();
    OverlayFont(const OverlayFont&) = delete;
    OverlayFont& operator=(const OverlayFont&) = delete;
    // pixelHeight is the em height. maxWidth bounds every later raster.
    bool prepare(const wchar_t* face, int pixelHeight, int maxWidth, std::wstring& error);
    void reset() noexcept;
    bool ready() const noexcept { return dc_ != nullptr; }
    int lineHeight() const noexcept { return lineHeight_; }
    // Distance from the line top to the visual middle of capital letters.
    int capCenter() const noexcept { return capCenter_; }
    int maxWidth() const noexcept { return maxWidth_; }
    // Natural single-line advance width, which may exceed maxWidth.
    bool measure(const wchar_t* text, int length, int& width) noexcept;
    // Draw one line into the raster, ending with an ellipsis if it is wider
    // than width (clamped to maxWidth). drawnWidth is the advance actually used.
    bool raster(const wchar_t* text, int length, int width, int& drawnWidth) noexcept;
    // Copy the last raster's coverage into a canvas.
    void blit(OverlayCanvas& canvas, int x, int y, OverlayColor color, int opacity) const noexcept;
private:
    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldFont_ = nullptr, oldBitmap_ = nullptr;
    uint8_t* pixels_ = nullptr;
    int lineHeight_ = 0, capCenter_ = 0, maxWidth_ = 0, rasterWidth_ = 0, dirtyWidth_ = 0, sourceHeight_ = 0;
};
// Halo radii and strengths scaled from a text em height.
struct OverlayHalo { int outline = 1, outlineAlpha = 0, blur = 1, shadowAlpha = 0, offset = 1, pad = 4; };
OverlayHalo overlayHalo(int pixelHeight) noexcept;
}

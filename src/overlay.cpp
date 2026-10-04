#include "overlay.h"
#include "config.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace lapse {
namespace {
constexpr int64_t MaxCanvasPixels = int64_t(MaxVideoDimension) * MaxVideoDimension;
constexpr double OverlayPi = 3.14159265358979323846;
bool overlayFail(std::wstring& error, const wchar_t* message) { error = message; return false; }
bool overlaySelected(HGDIOBJ object) noexcept { return object && object != HGDI_ERROR; }
bool emptyRect(const RECT& rect) noexcept { return rect.right <= rect.left || rect.bottom <= rect.top; }
uint8_t scaled(int value, int alpha) noexcept { return static_cast<uint8_t>((value * alpha + 127) / 255); }
}
OverlayHalo overlayHalo(int pixelHeight) noexcept {
    OverlayHalo halo;
    const int h = std::max(1, pixelHeight);
    halo.outline = std::max(1, (h + 9) / 18);
    halo.outlineAlpha = 175;
    halo.blur = std::max(1, h / 8);
    halo.shadowAlpha = 185;
    halo.offset = std::max(1, h / 22);
    halo.pad = halo.outline + 2 * halo.blur + halo.offset + 1;
    return halo;
}

bool OverlayCanvas::reserve(int maxWidth, int maxHeight, std::wstring& error) {
    error.clear();
    if (maxWidth <= 0 || maxHeight <= 0 || int64_t(maxWidth) * maxHeight > MaxCanvasPixels) {
        release(); return overlayFail(error, L"The overlay tile size is invalid.");
    }
    if (maxWidth <= capacityWidth_ && maxHeight <= capacityHeight_) return true;
    release();
    try {
        const size_t pixels = size_t(maxWidth) * size_t(maxHeight);
        std::unique_ptr<uint8_t[]> ink(new uint8_t[pixels * 4]()), halo(new uint8_t[pixels]()),
            first(new uint8_t[pixels]()), second(new uint8_t[pixels]()), third(new uint8_t[pixels]());
        ink_ = std::move(ink); halo_ = std::move(halo); first_ = std::move(first); second_ = std::move(second); third_ = std::move(third);
        capacityWidth_ = maxWidth; capacityHeight_ = maxHeight;
        return true;
    } catch (const std::bad_alloc&) { release(); return overlayFail(error, L"There is not enough memory to prepare the text overlay."); }
}
void OverlayCanvas::release() noexcept {
    ink_.reset(); halo_.reset(); first_.reset(); second_.reset(); third_.reset();
    capacityWidth_ = capacityHeight_ = width_ = height_ = 0; touched_ = {};
}
bool OverlayCanvas::begin(int width, int height) noexcept {
    if (capacityWidth_ > 0 && !emptyRect(touched_)) {
        for (int y = touched_.top; y < touched_.bottom; ++y) {
            const size_t row = size_t(y) * capacityWidth_;
            std::memset(ink_.get() + (row + touched_.left) * 4, 0, size_t(touched_.right - touched_.left) * 4);
            std::memset(halo_.get() + row + touched_.left, 0, size_t(touched_.right - touched_.left));
        }
    }
    touched_ = {};
    if (width <= 0 || height <= 0 || width > capacityWidth_ || height > capacityHeight_) { width_ = height_ = 0; return false; }
    width_ = width; height_ = height; return true;
}
void OverlayCanvas::mark(int left, int top, int right, int bottom) noexcept {
    left = std::max(left, 0); top = std::max(top, 0); right = std::min(right, width_); bottom = std::min(bottom, height_);
    if (right <= left || bottom <= top) return;
    if (emptyRect(touched_)) { touched_ = {left, top, right, bottom}; return; }
    touched_.left = std::min<LONG>(touched_.left, left); touched_.top = std::min<LONG>(touched_.top, top);
    touched_.right = std::max<LONG>(touched_.right, right); touched_.bottom = std::max<LONG>(touched_.bottom, bottom);
}
void OverlayCanvas::paint(int x, int y, int alpha, OverlayColor color) noexcept {
    auto* p = ink_.get() + (size_t(y) * capacityWidth_ + x) * 4;
    const int keep = 255 - alpha;
    p[0] = static_cast<uint8_t>((color.b * alpha + p[0] * keep + 127) / 255);
    p[1] = static_cast<uint8_t>((color.g * alpha + p[1] * keep + 127) / 255);
    p[2] = static_cast<uint8_t>((color.r * alpha + p[2] * keep + 127) / 255);
    p[3] = static_cast<uint8_t>(alpha + (p[3] * keep + 127) / 255);
}
void OverlayCanvas::coverage(const uint8_t* bgra, int strideBytes, int maskWidth, int maskHeight, int sample, int sourceHeight,
                             int x, int y, OverlayColor color, int opacity) noexcept {
    if (!bgra || !width_ || maskWidth <= 0 || maskHeight <= 0 || opacity <= 0 || sample < 1 || sample > 4 || sourceHeight <= 0) return;
    opacity = std::min(opacity, 255);
    const int left = std::max(0, -x), top = std::max(0, -y);
    const int right = std::min(maskWidth, width_ - x), bottom = std::min(maskHeight, height_ - y);
    if (right <= left || bottom <= top) return;
    const int samples = sample * sample * 3;
    int inkLeft = right, inkTop = bottom, inkRight = left, inkBottom = top;
    for (int j = top; j < bottom; ++j) {
        for (int i = left; i < right; ++i) {
            int sum = 0;
            for (int v = 0; v < sample; ++v) {
                const int row = std::min(j * sample + v, sourceHeight - 1);
                const auto* s = bgra + size_t(row) * strideBytes + size_t(i) * sample * 4;
                for (int u = 0; u < sample; ++u, s += 4) sum += s[0] + s[1] + s[2];
            }
            if (!sum) continue;
            const int cover = (sum + samples / 2) / samples;
            const int alpha = (cover * opacity + 127) / 255;
            if (!alpha) continue;
            paint(x + i, y + j, alpha, color);
            inkLeft = std::min(inkLeft, i); inkRight = std::max(inkRight, i + 1);
            inkTop = std::min(inkTop, j); inkBottom = std::max(inkBottom, j + 1);
        }
    }
    // Only what actually received ink grows the halo and composite area.
    if (inkRight > inkLeft) mark(x + inkLeft, y + inkTop, x + inkRight, y + inkBottom);
}
template<class Inside>
void OverlayCanvas::shape(double left, double top, double right, double bottom, OverlayColor color, int opacity, Inside inside) noexcept {
    if (!width_ || opacity <= 0) return;
    opacity = std::min(opacity, 255);
    const int x0 = std::max(0, static_cast<int>(std::floor(left))), y0 = std::max(0, static_cast<int>(std::floor(top)));
    const int x1 = std::min(width_, static_cast<int>(std::ceil(right))), y1 = std::min(height_, static_cast<int>(std::ceil(bottom)));
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        int hits = 0;
        for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i)
            hits += inside(x + (i + 0.5) / 4, y + (j + 0.5) / 4) ? 1 : 0;
        if (hits) paint(x, y, (hits * opacity + 8) / 16, color);
    }
    mark(x0, y0, x1, y1);
}
void OverlayCanvas::disc(double cx, double cy, double radius, OverlayColor color, int opacity) noexcept {
    if (!(radius > 0)) return;
    const double squared = radius * radius;
    shape(cx - radius, cy - radius, cx + radius, cy + radius, color, opacity,
        [&](double x, double y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= squared; });
}
void OverlayCanvas::ring(double cx, double cy, double radius, double thickness, double startTurn, double sweepTurns,
                         OverlayColor color, int opacity) noexcept {
    if (!(radius > 0) || !(thickness > 0) || !(sweepTurns > 0)) return;
    const double outer = radius + thickness / 2, half = thickness / 2;
    const bool whole = sweepTurns >= 1;
    shape(cx - outer, cy - outer, cx + outer, cy + outer, color, opacity, [&](double x, double y) {
        const double dx = x - cx, dy = y - cy;
        if (std::abs(std::sqrt(dx * dx + dy * dy) - radius) > half) return false;
        if (whole) return true;
        double turn = std::atan2(dx, -dy) / (2 * OverlayPi);
        turn -= startTurn; turn -= std::floor(turn);
        return turn <= sweepTurns;
    });
}
void OverlayCanvas::line(double x0, double y0, double x1, double y1, double thickness, OverlayColor color, int opacity) noexcept {
    if (!(thickness > 0)) return;
    const double half = thickness / 2, dx = x1 - x0, dy = y1 - y0, length = dx * dx + dy * dy;
    shape(std::min(x0, x1) - half, std::min(y0, y1) - half, std::max(x0, x1) + half, std::max(y0, y1) + half, color, opacity,
        [&](double x, double y) {
            double t = length > 0 ? ((x - x0) * dx + (y - y0) * dy) / length : 0;
            t = std::clamp(t, 0.0, 1.0);
            const double ex = x - (x0 + t * dx), ey = y - (y0 + t * dy);
            return ex * ex + ey * ey <= half * half;
        });
}
void OverlayCanvas::fade(int solidX, int clearX, int top, int bottom, int alpha) noexcept {
    if (!width_ || alpha <= 0 || solidX == clearX) return;
    alpha = std::min(alpha, 255);
    const int x0 = std::max(0, std::min(solidX, clearX)), x1 = std::min(width_, std::max(solidX, clearX));
    const int y0 = std::max(0, top), y1 = std::min(height_, bottom);
    if (x1 <= x0 || y1 <= y0) return;
    const int span = std::abs(clearX - solidX);
    for (int x = x0; x < x1; ++x) {
        const int value = static_cast<int>(int64_t(alpha) * (span - std::abs(x - solidX)) / span);
        if (value <= 0) continue;
        for (int y = y0; y < y1; ++y) {
            auto& h = halo_[size_t(y) * capacityWidth_ + x];
            h = static_cast<uint8_t>(std::max<int>(h, value));
        }
    }
    mark(x0, y0, x1, y1);
}
void OverlayCanvas::halo(int outlineRadius, int outlineAlpha, int blurRadius, int shadowAlpha, int offsetY) noexcept {
    if (!width_ || emptyRect(touched_)) return;
    outlineRadius = std::clamp(outlineRadius, 0, 64); blurRadius = std::clamp(blurRadius, 0, 128);
    offsetY = std::clamp(offsetY, -64, 64);
    outlineAlpha = std::clamp(outlineAlpha, 0, 255); shadowAlpha = std::clamp(shadowAlpha, 0, 255);
    const int extra = outlineRadius + 2 * blurRadius + std::abs(offsetY) + 1;
    const int x0 = std::max(0, int(touched_.left) - extra), y0 = std::max(0, int(touched_.top) - extra);
    const int x1 = std::min(width_, int(touched_.right) + extra), y1 = std::min(height_, int(touched_.bottom) + extra);
    const size_t stride = size_t(capacityWidth_);
    const auto ink = [&](int x, int y) -> int {
        return x < x0 || x >= x1 || y < y0 || y >= y1 ? 0 : ink_[(size_t(y) * stride + x) * 4 + 3];
    };
    // Outline: square dilation of the ink alpha into second_.
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        int value = 0;
        for (int i = -outlineRadius; i <= outlineRadius && value < 255; ++i) value = std::max(value, ink(x + i, y));
        first_[size_t(y) * stride + x] = static_cast<uint8_t>(value);
    }
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        int value = 0;
        for (int j = -outlineRadius; j <= outlineRadius && value < 255; ++j) {
            const int yy = y + j;
            if (yy >= y0 && yy < y1) value = std::max<int>(value, first_[size_t(yy) * stride + x]);
        }
        second_[size_t(y) * stride + x] = static_cast<uint8_t>(value);
    }
    // Shadow: two separable box passes over the offset ink alpha into third_.
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) third_[size_t(y) * stride + x] = static_cast<uint8_t>(ink(x, y - offsetY));
    const int window = 2 * blurRadius + 1;
    for (int pass = 0; pass < 2 && blurRadius > 0; ++pass) {
        for (int y = y0; y < y1; ++y) {
            int sum = 0;
            const auto value = [&](int x) -> int { return x < x0 || x >= x1 ? 0 : third_[size_t(y) * stride + x]; };
            for (int i = -blurRadius; i <= blurRadius; ++i) sum += value(x0 + i);
            for (int x = x0; x < x1; ++x) {
                first_[size_t(y) * stride + x] = static_cast<uint8_t>((sum + window / 2) / window);
                sum += value(x + blurRadius + 1) - value(x - blurRadius);
            }
        }
        for (int x = x0; x < x1; ++x) {
            int sum = 0;
            const auto value = [&](int y) -> int { return y < y0 || y >= y1 ? 0 : first_[size_t(y) * stride + x]; };
            for (int j = -blurRadius; j <= blurRadius; ++j) sum += value(y0 + j);
            for (int y = y0; y < y1; ++y) {
                third_[size_t(y) * stride + x] = static_cast<uint8_t>((sum + window / 2) / window);
                sum += value(y + blurRadius + 1) - value(y - blurRadius);
            }
        }
    }
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        const size_t i = size_t(y) * stride + x;
        const int value = std::max({int(halo_[i]), (second_[i] * outlineAlpha + 127) / 255, (third_[i] * shadowAlpha + 127) / 255});
        halo_[i] = static_cast<uint8_t>(value);
    }
    mark(x0, y0, x1, y1);
}
bool OverlayCanvas::composite(Frame& frame, int left, int top, int outputWidth, int outputHeight, RECT& bounds) const noexcept {
    if (!frame.valid() || frame.width > MaxVideoDimension || frame.height > MaxVideoDimension ||
        outputWidth <= 0 || outputHeight <= 0 || outputWidth > MaxVideoDimension || outputHeight > MaxVideoDimension) return false;
    bounds = {};
    if (!width_ || emptyRect(touched_)) return true;
    const int lx0 = std::max(0, left + int(touched_.left)), ly0 = std::max(0, top + int(touched_.top));
    const int lx1 = std::min(outputWidth, left + int(touched_.right)), ly1 = std::min(outputHeight, top + int(touched_.bottom));
    if (lx1 <= lx0 || ly1 <= ly0) return true;
    const int64_t fw = frame.width, fh = frame.height;
    const int fx0 = static_cast<int>(int64_t(lx0) * fw / outputWidth), fy0 = static_cast<int>(int64_t(ly0) * fh / outputHeight);
    const int fx1 = std::min<int>(frame.width, static_cast<int>((int64_t(lx1) * fw + outputWidth - 1) / outputWidth));
    const int fy1 = std::min<int>(frame.height, static_cast<int>((int64_t(ly1) * fh + outputHeight - 1) / outputHeight));
    if (fx1 <= fx0 || fy1 <= fy0) return true;
    const bool exact = frame.width == outputWidth && frame.height == outputHeight;
    const size_t stride = size_t(capacityWidth_);
    for (int fy = fy0; fy < fy1; ++fy) {
        int sy0 = static_cast<int>(int64_t(fy) * outputHeight / fh) - top;
        int sy1 = exact ? sy0 + 1 : std::max(sy0 + 1, static_cast<int>(int64_t(fy + 1) * outputHeight / fh) - top);
        const int64_t rows = sy1 - sy0;
        sy0 = std::max(sy0, int(touched_.top)); sy1 = std::min(sy1, int(touched_.bottom));
        if (sy1 <= sy0) continue;
        auto* target = frame.pixels.data() + (size_t(fy) * frame.width) * 4;
        for (int fx = fx0; fx < fx1; ++fx) {
            int sx0 = static_cast<int>(int64_t(fx) * outputWidth / fw) - left;
            int sx1 = exact ? sx0 + 1 : std::max(sx0 + 1, static_cast<int>(int64_t(fx + 1) * outputWidth / fw) - left);
            const int64_t area = rows * (sx1 - sx0);
            sx0 = std::max(sx0, int(touched_.left)); sx1 = std::min(sx1, int(touched_.right));
            if (sx1 <= sx0) continue;
            int64_t sum[5]{};
            for (int sy = sy0; sy < sy1; ++sy) for (int sx = sx0; sx < sx1; ++sx) {
                const size_t i = size_t(sy) * stride + sx;
                const auto* p = ink_.get() + i * 4;
                sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3]; sum[4] += halo_[i];
            }
            if (!sum[3] && !sum[4]) continue;
            const int b = static_cast<int>((sum[0] + area / 2) / area), g = static_cast<int>((sum[1] + area / 2) / area);
            const int r = static_cast<int>((sum[2] + area / 2) / area), a = static_cast<int>((sum[3] + area / 2) / area);
            const int h = static_cast<int>((sum[4] + area / 2) / area);
            auto* d = target + size_t(fx) * 4;
            for (int c = 0; c < 3; ++c) {
                const int shaded = scaled(d[c], 255 - h);
                d[c] = static_cast<uint8_t>(std::min(255, (shaded * (255 - a) + 127) / 255 + (c == 0 ? b : c == 1 ? g : r)));
            }
            d[3] = 255;
        }
    }
    bounds = {fx0, fy0, fx1, fy1};
    return true;
}

OverlayFont::~OverlayFont() { reset(); }
void OverlayFont::reset() noexcept {
    if (dc_ && oldFont_) SelectObject(dc_, oldFont_);
    if (dc_ && oldBitmap_) SelectObject(dc_, oldBitmap_);
    if (font_) DeleteObject(font_);
    if (bitmap_) DeleteObject(bitmap_);
    if (dc_) DeleteDC(dc_);
    dc_ = nullptr; font_ = nullptr; bitmap_ = nullptr; oldFont_ = oldBitmap_ = nullptr; pixels_ = nullptr;
    lineHeight_ = capCenter_ = maxWidth_ = rasterWidth_ = dirtyWidth_ = sourceHeight_ = 0;
}
namespace {
constexpr int FontSample = 2;
}
bool OverlayFont::prepare(const wchar_t* face, int pixelHeight, int maxWidth, std::wstring& error) {
    error.clear(); reset();
    if (!face || pixelHeight < 4 || pixelHeight > 512 || maxWidth <= 0 || maxWidth > 2 * MaxVideoDimension)
        return overlayFail(error, L"The overlay font size is invalid.");
    const auto failed = [&](const wchar_t* message) { reset(); return overlayFail(error, message); };
    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_) return failed(L"Windows could not create the overlay drawing context.");
    font_ = CreateFontW(-pixelHeight * FontSample, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, face);
    if (!font_) return failed(L"Windows could not create the overlay font.");
    const auto oldFont = SelectObject(dc_, font_);
    if (!overlaySelected(oldFont)) return failed(L"Windows could not select the overlay font.");
    oldFont_ = oldFont;
    TEXTMETRICW metrics{};
    if (!GetTextMetricsW(dc_, &metrics) || metrics.tmHeight <= 0 || metrics.tmHeight > 2048 || metrics.tmAscent <= 0 ||
        metrics.tmAscent > metrics.tmHeight || metrics.tmInternalLeading < 0 || metrics.tmInternalLeading >= metrics.tmAscent)
        return failed(L"Windows returned unsupported overlay font metrics.");
    sourceHeight_ = metrics.tmHeight;
    lineHeight_ = (metrics.tmHeight + FontSample - 1) / FontSample;
    // Capital height is about 70% of the em; centre icons on it, not the line.
    const int em = metrics.tmHeight - metrics.tmInternalLeading;
    capCenter_ = std::max(1, (static_cast<int>(metrics.tmAscent) - (em * 35 + 50) / 100) / FontSample);
    if (size_t(maxWidth) * FontSample * size_t(sourceHeight_) * 4 > size_t(64) * 1024 * 1024)
        return failed(L"The overlay text line is too large.");
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = maxWidth * FontSample; info.bmiHeader.biHeight = -sourceHeight_;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap_ || !pixels) return failed(L"Windows could not allocate the overlay text line.");
    pixels_ = static_cast<uint8_t*>(pixels);
    const auto oldBitmap = SelectObject(dc_, bitmap_);
    if (!overlaySelected(oldBitmap)) return failed(L"Windows could not select the overlay text line.");
    oldBitmap_ = oldBitmap;
    if (!SetBkMode(dc_, TRANSPARENT) || SetTextColor(dc_, RGB(255, 255, 255)) == CLR_INVALID)
        return failed(L"Windows could not configure overlay text drawing.");
    maxWidth_ = maxWidth;
    std::memset(pixels_, 0, size_t(maxWidth_) * FontSample * sourceHeight_ * 4);
    return true;
}
bool OverlayFont::measure(const wchar_t* text, int length, int& width) noexcept {
    if (!dc_ || !text || length < 0) return false;
    if (!length) { width = 0; return true; }
    RECT rect{0, 0, 0, 0};
    if (!DrawTextW(dc_, const_cast<wchar_t*>(text), length, &rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX)) return false;
    width = (std::max(0, static_cast<int>(rect.right)) + FontSample - 1) / FontSample;
    return true;
}
bool OverlayFont::raster(const wchar_t* text, int length, int width, int& drawnWidth) noexcept {
    if (!dc_ || !text || length < 0) return false;
    // A previous draw may still be queued; flush before touching the pixels.
    if (!GdiFlush()) return false;
    const size_t stride = size_t(maxWidth_) * FontSample * 4;
    for (int y = 0; y < sourceHeight_ && dirtyWidth_; ++y) std::memset(pixels_ + size_t(y) * stride, 0, size_t(dirtyWidth_) * FontSample * 4);
    dirtyWidth_ = rasterWidth_ = 0;
    width = std::clamp(width, 0, maxWidth_);
    if (!length || !width) { drawnWidth = 0; return true; }
    int natural = 0;
    if (!measure(text, length, natural)) return false;
    RECT rect{0, 0, width * FontSample, sourceHeight_};
    dirtyWidth_ = width;
    if (!DrawTextW(dc_, const_cast<wchar_t*>(text), length, &rect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_LEFT | DT_TOP) || !GdiFlush())
        return false;
    rasterWidth_ = width;
    drawnWidth = std::min(natural, width);
    return true;
}
void OverlayFont::blit(OverlayCanvas& canvas, int x, int y, OverlayColor color, int opacity) const noexcept {
    if (pixels_ && rasterWidth_)
        canvas.coverage(pixels_, maxWidth_ * FontSample * 4, rasterWidth_, lineHeight_, FontSample, sourceHeight_, x, y, color, opacity);
}
}

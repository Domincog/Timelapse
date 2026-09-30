#include "watermark.h"
#include "config.h"
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <new>

namespace lapse {
namespace {
constexpr size_t MaxTileBytes = 4 * 1024 * 1024;
bool fail(std::wstring& error, const wchar_t* message) { error = message; return false; }
bool validSettings(const WatermarkSettings& settings) noexcept {
    return settings.timeKind >= WatermarkTimeKind::ActiveElapsed && settings.timeKind <= WatermarkTimeKind::RecordedLocal &&
        settings.textSize >= WatermarkTextSize::Small && settings.textSize <= WatermarkTextSize::Large &&
        settings.x >= 0 && settings.x <= 10000 && settings.y >= 0 && settings.y <= 10000 &&
        (!settings.enabled || settings.showTime || settings.showSpeed);
}
bool validDate(const SYSTEMTIME& value) noexcept {
    if (value.wYear < 1601 || value.wYear > 9999 || value.wMonth < 1 || value.wMonth > 12 ||
        value.wHour > 23 || value.wMinute > 59 || value.wSecond > 59 || value.wMilliseconds > 999) return false;
    constexpr unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = value.wYear % 4 == 0 && (value.wYear % 100 != 0 || value.wYear % 400 == 0);
    return value.wDay >= 1 && value.wDay <= days[value.wMonth - 1] + (value.wMonth == 2 && leap ? 1u : 0u);
}
bool selected(HGDIOBJ object) noexcept { return object && object != HGDI_ERROR; }
}
bool validateWatermarkSettings(const WatermarkSettings& settings, std::wstring& error) {
    error.clear();
    if (!validSettings(settings)) return fail(error, L"Choose a valid watermark time, size and position, with at least one field enabled.");
    return true;
}
bool sameWatermarkSettings(const WatermarkSettings& a, const WatermarkSettings& b) noexcept {
    return a.enabled == b.enabled && a.showTime == b.showTime && a.showSpeed == b.showSpeed &&
        a.timeKind == b.timeKind && a.x == b.x && a.y == b.y && a.textSize == b.textSize;
}
bool formatWatermarkText(const WatermarkSettings& settings, const WatermarkContext& context,
                         std::array<wchar_t, WatermarkTextCapacity>& output, std::wstring& error) {
    error.clear();
    if (!validSettings(settings)) return fail(error, L"The watermark settings are invalid.");
    std::array<wchar_t, WatermarkTextCapacity> text{};
    if (!settings.enabled) { output = text; return true; }
    size_t used = 0;
    if (settings.showTime) {
        int written = 0;
        if (settings.timeKind == WatermarkTimeKind::ActiveElapsed) {
            if (context.activeMs < 0) return fail(error, L"The watermark active recording time is invalid.");
            const int64_t seconds = context.activeMs / 1000, days = seconds / 86400;
            const auto hour = static_cast<unsigned>(seconds / 3600 % 24);
            const auto minute = static_cast<unsigned>(seconds / 60 % 60), second = static_cast<unsigned>(seconds % 60);
            if (days > 999999) written = swprintf_s(text.data(), text.size(), L"Elapsed >999999d");
            else if (days) written = swprintf_s(text.data(), text.size(), L"Elapsed %lldd %02u:%02u:%02u", days, hour, minute, second);
            else written = swprintf_s(text.data(), text.size(), L"Elapsed %02u:%02u:%02u", hour, minute, second);
        } else {
            if (!validDate(context.recordedLocal)) return fail(error, L"The watermark recorded local date or time is invalid.");
            const auto& t = context.recordedLocal;
            written = swprintf_s(text.data(), text.size(), L"Recorded %04u-%02u-%02u %02u:%02u:%02u",
                unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond));
        }
        if (written <= 0) return fail(error, L"Windows could not format the watermark time.");
        used = static_cast<size_t>(written);
    }
    if (settings.showSpeed) {
        if (context.targetIntervalMs < MinCaptureIntervalMs || context.targetIntervalMs > WatermarkMaxIntervalMs)
            return fail(error, L"The watermark target capture interval is invalid.");
        if (used) text[used++] = L'\n';
        // interval * 30 / 1000 is exactly interval * 3 / 100. The accepted
        // interval bound makes the integer product safe and preserves decimals.
        const int64_t hundredths = context.targetIntervalMs * 3;
        const int64_t whole = hundredths / 100;
        const unsigned fraction = static_cast<unsigned>(hundredths % 100);
        const int written = !fraction ? swprintf_s(text.data() + used, text.size() - used, L"Target %lldx", whole) :
            fraction % 10 == 0 ? swprintf_s(text.data() + used, text.size() - used, L"Target %lld.%ux", whole, fraction / 10) :
            swprintf_s(text.data() + used, text.size() - used, L"Target %lld.%02ux", whole, fraction);
        if (written <= 0) return fail(error, L"Windows could not format the watermark target speed.");
    }
    output = text; return true;
}

struct WatermarkRenderer::Impl {
    WatermarkSettings settings;
    int outputWidth = 0, outputHeight = 0, fontHeight = 0, lineHeight = 0, padding = 0, gap = 0, margin = 0;
    int tileWidth = 0, tileHeight = 0, drawnWidth = 0, drawnHeight = 0;
    HDC dc = nullptr;
    HFONT font = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldFont = nullptr, oldBitmap = nullptr;
    uint8_t* pixels = nullptr;
    std::array<wchar_t, WatermarkTextCapacity> text{};
    bool rasterized = false;
    ~Impl() {
        if (dc && oldFont) SelectObject(dc, oldFont);
        if (dc && oldBitmap) SelectObject(dc, oldBitmap);
        if (font) DeleteObject(font);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    bool raster(const std::array<wchar_t, WatermarkTextCapacity>& next, std::wstring& error) {
        if (rasterized && text == next) return true;
        rasterized = false;
        const wchar_t* lines[] = {next.data(), nullptr};
        int lengths[] = {0, 0}, count = 1, maximum = 0;
        const auto split = std::wcschr(next.data(), L'\n');
        if (split) { lengths[0] = static_cast<int>(split - next.data()); lines[1] = split + 1; count = 2; }
        else lengths[0] = static_cast<int>(std::wcslen(next.data()));
        if (count == 2) lengths[1] = static_cast<int>(std::wcslen(lines[1]));
        for (int i = 0; i < count; ++i) {
            SIZE extent{};
            if (!GetTextExtentPoint32W(dc, lines[i], lengths[i], &extent) || extent.cx <= 0 || extent.cy > lineHeight)
                return fail(error, L"Windows could not measure the watermark text.");
            maximum = std::max(maximum, static_cast<int>(extent.cx));
        }
        const int width = maximum + padding * 2, height = count * lineHeight + (count - 1) * gap + padding * 2;
        if (width > tileWidth || height > tileHeight) return fail(error, L"The watermark text exceeds its prepared bounds.");
        // Previous successful rasterization was flushed before any CPU access.
        // Fill only the bounded tile; the recording frame is still untouched.
        for (size_t i = 0; i < size_t(tileWidth) * tileHeight; ++i) {
            pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 20; pixels[i * 4 + 3] = 255;
        }
        bool drawn = true;
        for (int i = 0; i < count; ++i)
            if (!TextOutW(dc, padding, padding + i * (lineHeight + gap), lines[i], lengths[i])) drawn = false;
        const bool flushed = GdiFlush() != FALSE;
        if (!drawn || !flushed) return fail(error, L"Windows could not draw the watermark text.");
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            auto* p = pixels + (size_t(y) * tileWidth + x) * 4;
            // Explicit grayscale and alpha avoid ClearType/color fringes or
            // undefined GDI alpha leaking into the video frame.
            const auto gray = static_cast<uint8_t>((unsigned(p[0]) + p[1] + p[2] + 1) / 3);
            p[0] = p[1] = p[2] = gray; p[3] = 255;
        }
        text = next; drawnWidth = width; drawnHeight = height; rasterized = true; return true;
    }
};
WatermarkRenderer::WatermarkRenderer() noexcept = default;
WatermarkRenderer::~WatermarkRenderer() = default;
void WatermarkRenderer::reset() noexcept { impl_.reset(); lastBounds_ = {}; }
RECT WatermarkRenderer::lastBounds() const noexcept { return lastBounds_; }
bool WatermarkRenderer::prepare(const WatermarkSettings& settings, int width, int height, std::wstring& error) {
    error.clear();
    if (!validateWatermarkSettings(settings, error)) { reset(); return false; }
    if (!settings.enabled) { reset(); return true; }
    if (width < MinVideoDimension || height < MinVideoDimension || width > MaxVideoDimension || height > MaxVideoDimension ||
        (width & 1) || (height & 1) || int64_t(width) * height > MaxVideoPixels) {
        reset(); return fail(error, L"Choose a supported video size before enabling the watermark.");
    }
    if (impl_ && impl_->outputWidth == width && impl_->outputHeight == height && sameWatermarkSettings(impl_->settings, settings)) return true;
    // A position-only change reuses all measured/rasterized GDI state.
    if (impl_ && impl_->outputWidth == width && impl_->outputHeight == height) {
        auto previous = impl_->settings; previous.x = settings.x; previous.y = settings.y;
        if (sameWatermarkSettings(previous, settings)) { impl_->settings = settings; lastBounds_ = {}; return true; }
    }
    reset();
    try {
        auto prepared = std::make_unique<Impl>();
        prepared->settings = settings; prepared->outputWidth = width; prepared->outputHeight = height;
        const int edge = std::min(width, height);
        const int scale = settings.textSize == WatermarkTextSize::Small ? 48 : settings.textSize == WatermarkTextSize::Medium ? 36 : 28;
        const int minimum = settings.textSize == WatermarkTextSize::Small ? 12 : settings.textSize == WatermarkTextSize::Medium ? 16 : 20;
        prepared->fontHeight = std::max(minimum, edge / scale);
        prepared->padding = std::max(2, prepared->fontHeight / 5); prepared->gap = std::max(1, prepared->fontHeight / 6);
        prepared->margin = std::max(2, edge / 100);
        prepared->dc = CreateCompatibleDC(nullptr);
        if (!prepared->dc) return fail(error, L"Windows could not create the watermark drawing context.");
        prepared->font = CreateFontW(-prepared->fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (!prepared->font) return fail(error, L"Windows could not create the watermark font.");
        const auto oldFont = SelectObject(prepared->dc, prepared->font);
        if (!selected(oldFont)) return fail(error, L"Windows could not select the watermark font.");
        prepared->oldFont = oldFont;
        TEXTMETRICW metrics{};
        if (!GetTextMetricsW(prepared->dc, &metrics) || metrics.tmHeight <= 0 || metrics.tmHeight > 512 ||
            metrics.tmMaxCharWidth <= 0 || metrics.tmMaxCharWidth > 512 || metrics.tmOverhang < 0 || metrics.tmOverhang > 512)
            return fail(error, L"Windows returned unsupported watermark font metrics.");
        prepared->lineHeight = metrics.tmHeight;
        // tmMaxCharWidth can include wide non-ASCII glyphs in the selected
        // font. All formatted labels are ASCII: bound their actual advances,
        // without rejecting small canvases because of unrelated glyphs.
        std::array<int, 95> widths{};
        if (!GetCharWidth32W(prepared->dc, 32, 126, widths.data()))
            return fail(error, L"Windows could not measure the watermark alphabet.");
        const int characterWidth = *std::max_element(widths.begin(), widths.end());
        if (characterWidth <= 0 || characterWidth > 512)
            return fail(error, L"Windows returned unsupported watermark character widths.");
        const int longestTime = settings.timeKind == WatermarkTimeKind::RecordedLocal ? 28 : 24;
        const int longest = std::max(settings.showTime ? longestTime : 0, settings.showSpeed ? 20 : 0);
        prepared->tileWidth = longest * characterWidth + metrics.tmOverhang + prepared->padding * 2;
        const int lines = int(settings.showTime) + int(settings.showSpeed);
        prepared->tileHeight = lines * prepared->lineHeight + (lines - 1) * prepared->gap + prepared->padding * 2;
        if (prepared->tileWidth > width - prepared->margin * 2 || prepared->tileHeight > height - prepared->margin * 2 ||
            size_t(prepared->tileWidth) * prepared->tileHeight * 4 > MaxTileBytes)
            return fail(error, L"The watermark does not fit this video size. Choose smaller text, fewer fields, or a larger video size.");
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = prepared->tileWidth; info.bmiHeader.biHeight = -prepared->tileHeight;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        prepared->bitmap = CreateDIBSection(prepared->dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!prepared->bitmap || !pixels) return fail(error, L"Windows could not allocate the watermark text tile.");
        prepared->pixels = static_cast<uint8_t*>(pixels);
        const auto oldBitmap = SelectObject(prepared->dc, prepared->bitmap);
        if (!selected(oldBitmap)) return fail(error, L"Windows could not select the watermark text tile.");
        prepared->oldBitmap = oldBitmap;
        if (!SetBkMode(prepared->dc, TRANSPARENT) || SetTextColor(prepared->dc, RGB(255,255,255)) == CLR_INVALID ||
            SetTextAlign(prepared->dc, TA_LEFT | TA_TOP | TA_NOUPDATECP) == GDI_ERROR)
            return fail(error, L"Windows could not configure watermark text drawing.");
        impl_ = std::move(prepared); return true;
    } catch (const std::bad_alloc&) { return fail(error, L"There is not enough memory to prepare the watermark."); }
}
bool WatermarkRenderer::apply(Frame& frame, const WatermarkContext& context, std::wstring& error) {
    error.clear();
    if (!impl_) { lastBounds_ = {}; return true; }
    if (!frame.valid() || frame.width > MaxVideoDimension || frame.height > MaxVideoDimension ||
        int64_t(frame.width) * frame.height > MaxVideoPixels)
        return fail(error, L"The watermark output frame is invalid.");
    std::array<wchar_t, WatermarkTextCapacity> text{};
    if (!formatWatermarkText(impl_->settings, context, text, error) || !impl_->raster(text, error)) return false;
    const auto& state = *impl_;
    const int left = state.margin + static_cast<int>(int64_t(state.outputWidth - state.margin * 2 - state.drawnWidth) * state.settings.x / 10000);
    const int top = state.margin + static_cast<int>(int64_t(state.outputHeight - state.margin * 2 - state.drawnHeight) * state.settings.y / 10000);
    const RECT bounds{static_cast<LONG>(int64_t(left) * frame.width / state.outputWidth),
        static_cast<LONG>(int64_t(top) * frame.height / state.outputHeight),
        static_cast<LONG>((int64_t(left + state.drawnWidth) * frame.width + state.outputWidth - 1) / state.outputWidth),
        static_cast<LONG>((int64_t(top + state.drawnHeight) * frame.height + state.outputHeight - 1) / state.outputHeight)};
    const int drawnWidth = bounds.right - bounds.left, drawnHeight = bounds.bottom - bounds.top;
    if (bounds.left < 0 || bounds.top < 0 || bounds.right > frame.width || bounds.bottom > frame.height || drawnWidth <= 0 || drawnHeight <= 0)
        return fail(error, L"The watermark placement is outside the video frame.");
    // All fallible work is complete. Only the exact bounded destination tile
    // is touched, using center-sampled scaling for disposable previews.
    for (int y = 0; y < drawnHeight; ++y) {
        const int sy = static_cast<int>((int64_t(y) * 2 + 1) * state.drawnHeight / (int64_t(drawnHeight) * 2));
        auto* target = frame.pixels.data() + (size_t(bounds.top + y) * frame.width + bounds.left) * 4;
        const auto* source = state.pixels + size_t(sy) * state.tileWidth * 4;
        if (drawnWidth == state.drawnWidth && drawnHeight == state.drawnHeight) {
            std::memcpy(target, source, size_t(drawnWidth) * 4); continue;
        }
        for (int x = 0; x < drawnWidth; ++x) {
            const int sx = static_cast<int>((int64_t(x) * 2 + 1) * state.drawnWidth / (int64_t(drawnWidth) * 2));
            target[x * 4] = source[sx * 4]; target[x * 4 + 1] = source[sx * 4 + 1];
            target[x * 4 + 2] = source[sx * 4 + 2]; target[x * 4 + 3] = 255;
        }
    }
    lastBounds_ = bounds; return true;
}
}

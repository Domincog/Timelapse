#include "watermark.h"
#include "config.h"
#include "overlay.h"
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <new>

namespace lapse {
namespace {
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
        if (context.outputFps < MinOutputFps || context.outputFps > MaxOutputFps)
            return fail(error, L"The watermark playback FPS is invalid.");
        if (used) text[used++] = L'\n';
        // Millisecond capture cadence times integer playback fps gives an exact
        // speed with at most three decimal places. The accepted bounds keep the
        // product below INT64_MAX, including accelerated time-compression rates.
        const int64_t thousandths = context.targetIntervalMs * context.outputFps;
        const int64_t whole = thousandths / 1000;
        const unsigned fraction = static_cast<unsigned>(thousandths % 1000);
        const int written = !fraction ? swprintf_s(text.data() + used, text.size() - used, L"Target %lldx", whole) :
            fraction % 100 == 0 ? swprintf_s(text.data() + used, text.size() - used, L"Target %lld.%ux", whole, fraction / 100) :
            fraction % 10 == 0 ? swprintf_s(text.data() + used, text.size() - used, L"Target %lld.%02ux", whole, fraction / 10) :
            swprintf_s(text.data() + used, text.size() - used, L"Target %lld.%03ux", whole, fraction);
        if (written <= 0) return fail(error, L"Windows could not format the watermark target speed.");
    }
    output = text; return true;
}

struct WatermarkRenderer::Impl {
    WatermarkSettings settings;
    int outputWidth = 0, outputHeight = 0, margin = 0, gap = 0, tileWidth = 0, tileHeight = 0;
    // The first line uses the larger semibold face; a second line is smaller.
    OverlayFont fonts[2];
    OverlayCanvas canvas;
    OverlayHalo halo;
    std::array<wchar_t, WatermarkTextCapacity> text{};
    bool rasterized = false;
    bool raster(const std::array<wchar_t, WatermarkTextCapacity>& next, std::wstring& error) {
        if (rasterized && text == next) return true;
        rasterized = false;
        const wchar_t* lines[] = {next.data(), nullptr};
        int lengths[] = {0, 0}, widths[] = {0, 0}, count = 1;
        const auto split = std::wcschr(next.data(), L'\n');
        if (split) { lengths[0] = static_cast<int>(split - next.data()); lines[1] = split + 1; count = 2; }
        else lengths[0] = static_cast<int>(std::wcslen(next.data()));
        if (count == 2) lengths[1] = static_cast<int>(std::wcslen(lines[1]));
        int widest = 0, height = halo.pad * 2;
        for (int i = 0; i < count; ++i) {
            if (!fonts[i].raster(lines[i], lengths[i], fonts[i].maxWidth(), widths[i]) || widths[i] <= 0)
                return fail(error, L"Windows could not draw the watermark text.");
            widest = std::max(widest, widths[i]); height += fonts[i].lineHeight() + (i ? gap : 0);
        }
        const int width = widest + halo.pad * 2;
        if (!canvas.begin(width, height)) return fail(error, L"The watermark text exceeds its prepared bounds.");
        // Align lines toward the nearer horizontal edge so a corner placement
        // reads cleanly without any backing box.
        const int align = settings.x <= 3333 ? 0 : settings.x >= 6667 ? 2 : 1;
        int y = halo.pad;
        for (int i = 0; i < count; ++i) {
            const int x = halo.pad + (align == 0 ? 0 : align == 2 ? widest - widths[i] : (widest - widths[i]) / 2);
            fonts[i].blit(canvas, x, y, i ? OverlayColor{232, 236, 242} : OverlayColor{246, 248, 250}, i ? 225 : 255);
            y += fonts[i].lineHeight() + gap;
        }
        canvas.halo(halo.outline, halo.outlineAlpha, halo.blur, halo.shadowAlpha, halo.offset);
        text = next; tileWidth = width; tileHeight = height; rasterized = true; return true;
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
    // A position-only change reuses fonts and the canvas; only alignment redraws.
    if (impl_ && impl_->outputWidth == width && impl_->outputHeight == height) {
        auto previous = impl_->settings; previous.x = settings.x; previous.y = settings.y;
        if (sameWatermarkSettings(previous, settings)) { impl_->settings = settings; impl_->rasterized = false; lastBounds_ = {}; return true; }
    }
    reset();
    try {
        auto prepared = std::make_unique<Impl>();
        prepared->settings = settings; prepared->outputWidth = width; prepared->outputHeight = height;
        const int edge = std::min(width, height);
        const int scale = settings.textSize == WatermarkTextSize::Small ? 48 : settings.textSize == WatermarkTextSize::Medium ? 36 : 28;
        const int minimum = settings.textSize == WatermarkTextSize::Small ? 12 : settings.textSize == WatermarkTextSize::Medium ? 16 : 20;
        const int primary = std::max(minimum, edge / scale), secondary = std::max(minimum * 3 / 4, primary * 3 / 4);
        prepared->halo = overlayHalo(primary);
        prepared->margin = std::max(2, edge / 60); prepared->gap = std::max(0, primary / 12);
        const int count = int(settings.showTime) + int(settings.showSpeed);
        for (int i = 0; i < count; ++i)
            if (!prepared->fonts[i].prepare(i ? L"Segoe UI" : L"Segoe UI Semibold", i ? secondary : primary, width, error)) return false;
        // Preflight the widest text each line can show: every digit position
        // holds this font's widest digit.
        const auto longest = [&](OverlayFont& font, const wchar_t* pattern, int& result) {
            wchar_t widestDigit = L'0'; int best = -1;
            for (wchar_t digit = L'0'; digit <= L'9'; ++digit) {
                int advance = 0; if (!font.measure(&digit, 1, advance)) return false;
                if (advance > best) { best = advance; widestDigit = digit; }
            }
            wchar_t sample[48]{}; size_t n = 0;
            for (; pattern[n] && n + 1 < std::size(sample); ++n) sample[n] = pattern[n] == L'#' ? widestDigit : pattern[n];
            return font.measure(sample, static_cast<int>(n), result);
        };
        int widest = 0, lines = 0, tall = prepared->halo.pad * 2;
        const wchar_t* time = settings.timeKind == WatermarkTimeKind::RecordedLocal ? L"Recorded ####-##-## ##:##:##" : L"Elapsed ######d ##:##:##";
        for (const wchar_t* pattern : {settings.showTime ? time : nullptr, settings.showSpeed ? L"Target #########.###x" : nullptr}) {
            if (!pattern) continue;
            int measured = 0;
            if (!longest(prepared->fonts[lines], pattern, measured)) return fail(error, L"Windows could not measure the watermark text.");
            widest = std::max(widest, measured); tall += prepared->fonts[lines].lineHeight() + (lines ? prepared->gap : 0); ++lines;
        }
        if (widest > width - prepared->margin * 2 || tall - prepared->halo.pad * 2 > height - prepared->margin * 2)
            return fail(error, L"The watermark does not fit this video size. Choose smaller text, fewer fields, or a larger video size.");
        for (int i = 0; i < lines; ++i)
            if (!prepared->fonts[i].prepare(i ? L"Segoe UI" : L"Segoe UI Semibold", i ? secondary : primary, widest + 2, error)) return false;
        if (!prepared->canvas.reserve(widest + prepared->halo.pad * 2 + 2, tall, error)) return false;
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
    auto& state = *impl_;
    // Place the visible text (inside the halo padding) within the margins.
    const int pad = state.halo.pad, innerWidth = state.tileWidth - pad * 2, innerHeight = state.tileHeight - pad * 2;
    const int left = state.margin - pad + static_cast<int>(int64_t(state.outputWidth - state.margin * 2 - innerWidth) * state.settings.x / 10000);
    const int top = state.margin - pad + static_cast<int>(int64_t(state.outputHeight - state.margin * 2 - innerHeight) * state.settings.y / 10000);
    RECT bounds{};
    if (!state.canvas.composite(frame, left, top, state.outputWidth, state.outputHeight, bounds))
        return fail(error, L"The watermark placement is outside the video frame.");
    lastBounds_ = bounds; return true;
}
}

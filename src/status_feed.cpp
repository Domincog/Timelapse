#include "status_feed.h"
#include "config.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <new>

namespace lapse {
namespace {
constexpr double StatusPi = 3.14159265358979323846;
constexpr OverlayColor Ink{246, 248, 250}, Soft{232, 236, 242}, Blue{124, 196, 255}, Green{91, 228, 155}, Amber{255, 181, 71};
bool statusFail(std::wstring& error, const wchar_t* message) { error = message; return false; }
double ease(double t) noexcept { t = std::clamp(t, 0.0, 1.0); return 1 - (1 - t) * (1 - t) * (1 - t); }
double progress(uint64_t frame, uint64_t from, int span) noexcept {
    return frame <= from ? 0.0 : std::min(1.0, double(frame - from) / std::max(1, span));
}
size_t textLength(const std::array<wchar_t, StatusTextCapacity>& text) noexcept {
    size_t length = 0;
    while (length < text.size() && text[length]) ++length;
    return length;
}
void copyText(std::array<wchar_t, StatusTextCapacity>& output, const wchar_t* text) noexcept {
    output.fill(L'\0');
    for (size_t i = 0; text && text[i] && i + 1 < output.size(); ++i) output[i] = text[i];
}
OverlayColor iconColor(StatusIcon icon) noexcept {
    switch (icon) {
    case StatusIcon::Stopwatch: case StatusIcon::Break: return Blue;
    case StatusIcon::Timer: return Green;
    case StatusIcon::Overtime: return Amber;
    default: return Soft;
    }
}
}
bool validateStatusFeedSettings(const StatusFeedSettings& settings, std::wstring& error) {
    error.clear();
    if (settings.corner < StatusCorner::TopLeft || settings.corner > StatusCorner::BottomRight ||
        settings.textSize < StatusTextSize::Small || settings.textSize > StatusTextSize::Large ||
        settings.style < StatusStyle::Shadow || settings.style > StatusStyle::EdgeFade)
        return statusFail(error, L"Choose a valid status corner, text size and style.");
    return true;
}
bool sameStatusFeedSettings(const StatusFeedSettings& a, const StatusFeedSettings& b) noexcept {
    return a.corner == b.corner && a.textSize == b.textSize && a.style == b.style;
}
void setStatusText(StatusItem& item, const wchar_t* text) noexcept {
    item.text.fill(L'\0');
    if (!text) return;
    std::array<wchar_t, StatusTextCapacity> folded{};
    size_t length = 0;
    for (size_t i = 0; text[i] && length < size_t(StatusMaxTextLength); ++i)
        folded[length++] = std::iswcntrl(text[i]) ? L' ' : text[i];
    size_t first = 0;
    while (first < length && std::iswspace(folded[first])) ++first;
    while (length > first && std::iswspace(folded[length - 1])) --length;
    for (size_t i = first; i < length; ++i) item.text[i - first] = folded[i];
}
bool validateStatusItem(const StatusItem& item, std::wstring& error) {
    error.clear();
    if (item.kind < StatusKind::None || item.kind > StatusKind::Timer) return statusFail(error, L"Choose a valid status type.");
    if (item.kind == StatusKind::None) return true;
    const size_t length = textLength(item.text);
    if (length >= item.text.size()) return statusFail(error, L"The status text is not terminated.");
    if (!length) return statusFail(error, L"Type what you are doing, or clear the status.");
    for (size_t i = 0; i < length; ++i) if (std::iswcntrl(item.text[i])) return statusFail(error, L"Status text must be one line.");
    if (std::iswspace(item.text[0]) || std::iswspace(item.text[length - 1])) return statusFail(error, L"Status text has surrounding spaces.");
    if (item.kind == StatusKind::Timer) {
        if (item.durationMs < 1000 || item.durationMs > StatusMaxTimerMs || item.durationMs % 1000)
            return statusFail(error, L"Timer length must be whole seconds from 1 second to 600 minutes.");
        if (item.breakMs && (item.breakMs < 1000 || item.breakMs > StatusMaxTimerMs || item.breakMs % 1000))
            return statusFail(error, L"Break length must be whole seconds from 1 second to 600 minutes.");
    } else if (item.durationMs || item.breakMs) return statusFail(error, L"Only a timer has a length.");
    return true;
}
void formatStatusClock(int64_t milliseconds, bool overtime, std::array<wchar_t, 16>& output) noexcept {
    constexpr int64_t Cap = 999LL * 3600 + 59 * 60 + 59;
    const int64_t seconds = std::min(std::max<int64_t>(milliseconds, 0) / 1000, Cap);
    const auto hours = static_cast<unsigned>(seconds / 3600), minutes = static_cast<unsigned>(seconds / 60 % 60);
    const auto rest = static_cast<unsigned>(seconds % 60);
    output.fill(L'\0');
    if (hours) swprintf_s(output.data(), output.size(), L"%ls%u:%02u:%02u", overtime ? L"+" : L"", hours, minutes, rest);
    else swprintf_s(output.data(), output.size(), L"%ls%u:%02u", overtime ? L"+" : L"", static_cast<unsigned>(seconds / 60), rest);
}
StatusView resolveStatus(const StatusItem& item, uint64_t nowTick) noexcept {
    StatusView view;
    view.sequence = item.sequence;
    if (item.kind == StatusKind::None || !textLength(item.text)) return view;
    view.visible = true; view.label = item.text;
    const int64_t elapsed = nowTick > item.startTick ? static_cast<int64_t>(std::min<uint64_t>(nowTick - item.startTick, uint64_t(INT64_MAX / 2))) : 0;
    switch (item.kind) {
    case StatusKind::Note: view.icon = StatusIcon::Note; break;
    case StatusKind::Stopwatch:
        view.icon = StatusIcon::Stopwatch;
        formatStatusClock(elapsed, false, view.value);
        view.fraction = double(elapsed % 3600000) / 3600000;
        break;
    case StatusKind::Timer: {
        const int64_t work = std::max<int64_t>(item.durationMs, 1000);
        if (item.breakMs > 0) {
            const int64_t cycle = work + item.breakMs, within = elapsed % cycle;
            const bool onBreak = within >= work;
            view.phase = uint64_t(elapsed / cycle) * 2 + (onBreak ? 1 : 0);
            const int64_t length = onBreak ? item.breakMs : work, remaining = (onBreak ? cycle : work) - within;
            view.icon = onBreak ? StatusIcon::Break : StatusIcon::Timer;
            if (onBreak) copyText(view.label, L"Break");
            formatStatusClock(remaining + 999, false, view.value);
            view.fraction = double(remaining) / double(length);
        } else if (elapsed < work) {
            view.icon = StatusIcon::Timer;
            formatStatusClock(work - elapsed + 999, false, view.value);
            view.fraction = double(work - elapsed) / double(work);
        } else {
            view.icon = StatusIcon::Overtime;
            formatStatusClock(elapsed - work, true, view.value);
        }
        break; }
    default: view.visible = false; break;
    }
    return view;
}

void StatusFeed::reset(int outputFps) noexcept {
    count_ = 0;
    const int fps = std::clamp(outputFps, MinOutputFps, MaxOutputFps);
    const auto frames = [&](int milliseconds) { return std::max(1, (fps * milliseconds + 500) / 1000); };
    enter_ = frames(300); slide_ = frames(270); strike_ = frames(400); hold_ = frames(1200); fade_ = frames(2000);
}
uint64_t StatusFeed::settleFrames() const noexcept { return uint64_t(strike_) + hold_ + fade_ + slide_ + enter_; }
bool StatusFeed::faded(const Entry& entry, uint64_t frame) const noexcept {
    return entry.ended && frame >= entry.endFrame && frame - entry.endFrame >= uint64_t(hold_) + fade_;
}
void StatusFeed::advance(const StatusItem& item, uint64_t nowTick, uint64_t frame) noexcept {
    const auto view = resolveStatus(item, nowTick);
    Entry* top = count_ ? &entries_[0] : nullptr;
    const bool active = top && !top->ended;
    if (active && view.visible && view.sequence == top->view.sequence && view.phase == top->view.phase) { top->view = view; return; }
    if (active) {
        // A finished block of a repeating timer reads as complete, not as the
        // last second that happened to be captured.
        if (view.sequence == top->view.sequence && view.phase != top->view.phase &&
            (top->view.icon == StatusIcon::Timer || top->view.icon == StatusIcon::Break)) {
            formatStatusClock(0, false, top->view.value); top->view.fraction = 0;
        }
        top->ended = true; top->endFrame = frame;
    }
    if (view.visible) {
        const size_t kept = std::min(count_, entries_.size() - 1);
        for (size_t i = kept; i > 0; --i) entries_[i] = entries_[i - 1];
        entries_[0] = {view, frame, 0, false};
        count_ = kept + 1;
    }
    while (count_ && faded(entries_[count_ - 1], frame)) --count_;
}
size_t StatusFeed::layout(uint64_t frame, Rows& rows) const noexcept {
    size_t visible = 0;
    for (size_t i = 0; i < count_ && visible < rows.size(); ++i) {
        const auto& entry = entries_[i];
        double slot = 0;
        for (size_t j = 0; j < i; ++j) slot += ease(progress(frame, entries_[j].startFrame, slide_));
        const double enter = ease(progress(frame, entry.startFrame, enter_));
        double opacity = enter, strike = 0;
        if (entry.ended) {
            const double age = frame > entry.endFrame ? double(frame - entry.endFrame) : 0.0;
            strike = ease(age / strike_);
            opacity *= age < hold_ ? 1 - 0.35 * strike : 0.65 * (1 - ease((age - hold_) / fade_));
        }
        // Older lines beyond three hurry out when a fourth newer line arrives.
        if (i >= 3) opacity *= 1 - progress(frame, entries_[i - 3].startFrame, enter_);
        const int alpha = static_cast<int>(std::lround(std::clamp(opacity, 0.0, 1.0) * 255));
        if (alpha <= 0) continue;
        rows[visible++] = {entry.view, alpha, slot, enter, strike};
    }
    return visible;
}

void StatusFeedRenderer::reset() noexcept {
    label_.reset(); value_.reset(); canvas_.release();
    prepared_ = fits_ = false; lastBounds_ = {};
    outputWidth_ = outputHeight_ = 0;
}
bool StatusFeedRenderer::prepare(const StatusFeedSettings& settings, int width, int height, std::wstring& error) {
    error.clear();
    if (!validateStatusFeedSettings(settings, error)) { reset(); return false; }
    if (width < MinVideoDimension || height < MinVideoDimension || width > MaxVideoDimension || height > MaxVideoDimension ||
        (width & 1) || (height & 1) || int64_t(width) * height > MaxVideoPixels) {
        reset(); return statusFail(error, L"Choose a supported video size before showing a status.");
    }
    if (prepared_ && outputWidth_ == width && outputHeight_ == height && sameStatusFeedSettings(settings_, settings)) return true;
    // Corner and style changes keep the fonts; only placement and tile change.
    const bool sameText = prepared_ && outputWidth_ == width && outputHeight_ == height && settings_.textSize == settings.textSize;
    if (!sameText) reset();
    try {
        const int edge = std::min(width, height);
        const int scale = settings.textSize == StatusTextSize::Small ? 40 : settings.textSize == StatusTextSize::Medium ? 30 : 23;
        const int minimum = settings.textSize == StatusTextSize::Small ? 12 : settings.textSize == StatusTextSize::Medium ? 14 : 16;
        const int em = std::max(minimum, edge / scale);
        em_ = em; halo_ = overlayHalo(em);
        margin_ = std::max(halo_.pad + 2, edge / 36);
        icon_ = std::max(8, em * 19 / 20); gap_ = std::max(3, em * 9 / 20); fadeLength_ = em * 4;
        if (!sameText) {
            if (!value_.prepare(L"Segoe UI", em, width, error)) { reset(); return false; }
            wchar_t widest = L'0'; int best = -1;
            for (wchar_t digit = L'0'; digit <= L'9'; ++digit) {
                int advance = 0; if (!value_.measure(&digit, 1, advance)) { reset(); return statusFail(error, L"Windows could not measure the status text."); }
                if (advance > best) { best = advance; widest = digit; }
            }
            wchar_t sample[] = L"+000:00:00";
            for (auto& c : sample) if (c == L'0') c = widest;
            if (!value_.measure(sample, static_cast<int>(std::wcslen(sample)), valueWidth_)) { reset(); return statusFail(error, L"Windows could not measure the status text."); }
            rowHeight_ = value_.lineHeight() * 21 / 20;
            if (!value_.prepare(L"Segoe UI", em, valueWidth_ + em, error)) { reset(); return false; }
        }
        settings_ = settings; outputWidth_ = width; outputHeight_ = height; prepared_ = true; lastBounds_ = {};
        // Labels get up to 45% of the width; the clock and icon need the rest.
        const int fixed = margin_ * 2 + icon_ + gap_ * 2 + valueWidth_;
        labelWidth_ = std::min(width * 45 / 100, width - fixed);
        fits_ = labelWidth_ >= em * 3 && margin_ * 2 + rowHeight_ * 2 <= height;
        if (!fits_) { label_.reset(); canvas_.release(); return true; }
        if (!sameText || !label_.ready() || label_.maxWidth() != labelWidth_)
            if (!label_.prepare(L"Segoe UI Semibold", em, labelWidth_, error)) { reset(); return false; }
        tileWidth_ = std::min(width, margin_ + icon_ + gap_ * 2 + labelWidth_ + valueWidth_ + halo_.pad + em +
            (settings.style == StatusStyle::EdgeFade ? fadeLength_ : 0));
        tileHeight_ = std::min(height, margin_ + rowHeight_ * int(StatusFeedMaxRows + 1) + halo_.pad);
        if (!canvas_.reserve(tileWidth_, tileHeight_, error)) { reset(); return false; }
        return true;
    } catch (const std::bad_alloc&) { reset(); return statusFail(error, L"There is not enough memory to prepare the status overlay."); }
}
bool StatusFeedRenderer::apply(Frame& frame, const StatusFeed& feed, uint64_t index, std::wstring& error) {
    error.clear();
    if (!prepared_ || !fits_) { lastBounds_ = {}; return true; }
    if (!frame.valid() || frame.width > MaxVideoDimension || frame.height > MaxVideoDimension)
        return statusFail(error, L"The status overlay frame is invalid.");
    StatusFeed::Rows rows{};
    const size_t count = feed.layout(index, rows);
    if (!count) { lastBounds_ = {}; return true; }
    if (!canvas_.begin(tileWidth_, tileHeight_)) return statusFail(error, L"The status overlay tile is invalid.");
    const bool right = settings_.corner == StatusCorner::TopRight || settings_.corner == StatusCorner::BottomRight;
    const bool bottom = settings_.corner == StatusCorner::BottomLeft || settings_.corner == StatusCorner::BottomRight;
    const int em = em_;
    for (size_t i = 0; i < count; ++i) {
        const auto& row = rows[i];
        const double offset = (bottom ? 1 : -1) * 0.35 * rowHeight_ * (1 - row.enter);
        const double top = bottom ? tileHeight_ - margin_ - (row.slot + 1) * rowHeight_ + offset : margin_ + row.slot * rowHeight_ + offset;
        const int textTop = static_cast<int>(std::lround(top)) + (rowHeight_ - value_.lineHeight()) / 2;
        int labelWidth = 0, valueWidth = 0;
        const int labelLength = static_cast<int>(textLength(row.view.label));
        int valueLength = 0;
        while (valueLength < int(row.view.value.size()) && row.view.value[size_t(valueLength)]) ++valueLength;
        if (!label_.raster(row.view.label.data(), labelLength, labelWidth_, labelWidth) ||
            (valueLength && !value_.raster(row.view.value.data(), valueLength, value_.maxWidth(), valueWidth)))
            return statusFail(error, L"Windows could not draw the status text.");
        const int width = icon_ + gap_ + labelWidth + (valueLength ? gap_ + valueWidth : 0);
        const int x = right ? tileWidth_ - margin_ - width : margin_;
        const double centre = textTop + label_.capCenter();
        const auto color = iconColor(row.view.icon);
        if (settings_.style == StatusStyle::EdgeFade) {
            const int bandTop = static_cast<int>(std::lround(top)) + rowHeight_ / 10, bandBottom = static_cast<int>(std::lround(top)) + rowHeight_ * 9 / 10;
            canvas_.fade(right ? tileWidth_ : 0, right ? x - fadeLength_ : x + width + fadeLength_, bandTop, bandBottom, 150 * row.opacity / 255);
            const double tick = std::max(2.0, em / 7.0);
            canvas_.line(right ? tileWidth_ - tick / 2 : tick / 2, bandTop + tick / 2, right ? tileWidth_ - tick / 2 : tick / 2, bandBottom - tick / 2, tick, color, row.opacity);
        }
        const double cx = x + icon_ / 2.0, radius = icon_ * 0.38, stroke = std::max(1.5, icon_ * 0.13);
        switch (row.view.icon) {
        case StatusIcon::Note: canvas_.disc(cx, centre, icon_ * 0.17, color, row.opacity); break;
        case StatusIcon::Stopwatch: {
            const double r = icon_ * 0.33, cy = centre + icon_ * 0.07, turn = row.view.fraction * 2 * StatusPi;
            const double crown = cy - r - stroke * 0.5;
            canvas_.ring(cx, cy, r, stroke, 0, 1, color, row.opacity);
            canvas_.line(cx, crown, cx, crown - icon_ * 0.09, stroke, color, row.opacity);
            canvas_.line(cx - icon_ * 0.1, crown - icon_ * 0.12, cx + icon_ * 0.1, crown - icon_ * 0.12, stroke, color, row.opacity);
            canvas_.disc(cx, cy, stroke * 0.7, color, row.opacity);
            canvas_.line(cx, cy, cx + std::sin(turn) * r * 0.62, cy - std::cos(turn) * r * 0.62, stroke * 0.8, color, row.opacity);
            break; }
        case StatusIcon::Timer: case StatusIcon::Break:
            canvas_.ring(cx, centre, radius, stroke, 0, 1, Ink, row.opacity * 70 / 255);
            canvas_.ring(cx, centre, radius, stroke, 0, std::clamp(row.view.fraction, 0.0, 1.0), color, row.opacity);
            break;
        case StatusIcon::Overtime: canvas_.ring(cx, centre, radius, stroke, 0, 1, color, row.opacity); break;
        default: break;
        }
        const int labelX = x + icon_ + gap_;
        label_.blit(canvas_, labelX, textTop, Ink, row.opacity);
        if (valueLength) value_.blit(canvas_, labelX + labelWidth + gap_, textTop,
            row.view.icon == StatusIcon::Overtime ? Amber : Soft, row.opacity * 230 / 255);
        if (row.strike > 0) {
            const double length = (labelWidth + (valueLength ? gap_ + valueWidth : 0) + em * 0.3) * row.strike;
            canvas_.line(labelX - em * 0.15, centre, labelX - em * 0.15 + length, centre, std::max(1.5, em * 0.085), Ink, row.opacity);
        }
    }
    canvas_.halo(halo_.outline, halo_.outlineAlpha, halo_.blur, halo_.shadowAlpha, halo_.offset);
    RECT bounds{};
    if (!canvas_.composite(frame, right ? outputWidth_ - tileWidth_ : 0, bottom ? outputHeight_ - tileHeight_ : 0, outputWidth_, outputHeight_, bounds))
        return statusFail(error, L"The status overlay placement is outside the video frame.");
    lastBounds_ = bounds; return true;
}
}

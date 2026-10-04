#pragma once
#include "core.h"
#include "overlay.h"
#include <algorithm>
#include <array>

namespace lapse {
// A short "what I'm doing" line burned into the video. Replacing it strikes
// the old line through, slides it under the new one and fades it out, like a
// game event feed. Animation is timed in saved video frames, so it looks the
// same at every capture interval; stopwatch and timer values use real time.
constexpr int StatusMaxTextLength = 60;
constexpr size_t StatusTextCapacity = StatusMaxTextLength + 1;
constexpr int64_t StatusMaxTimerMs = 600LL * 60000;
constexpr size_t StatusFeedMaxRows = 4;
// Values are saved in preferences; append new values only.
enum class StatusKind { None, Note, Stopwatch, Timer };
enum class StatusCorner { TopLeft, TopRight, BottomLeft, BottomRight };
enum class StatusTextSize { Small, Medium, Large };
enum class StatusStyle { Shadow, EdgeFade };
struct StatusFeedSettings {
    StatusCorner corner = StatusCorner::TopLeft;
    StatusTextSize textSize = StatusTextSize::Medium;
    StatusStyle style = StatusStyle::Shadow;
};
bool validateStatusFeedSettings(const StatusFeedSettings&, std::wstring& error);
bool sameStatusFeedSettings(const StatusFeedSettings&, const StatusFeedSettings&) noexcept;
// What the user set. A new sequence is a new status even with the same text.
// Kind None clears the status. A timer with breakMs alternates work blocks of
// durationMs with breaks of breakMs until the status changes.
struct StatusItem {
    uint64_t sequence = 0;
    StatusKind kind = StatusKind::None;
    std::array<wchar_t, StatusTextCapacity> text{};
    // GetTickCount64 when set; it keeps counting through sleep.
    uint64_t startTick = 0;
    int64_t durationMs = 0, breakMs = 0;
};
// Text is trimmed by callers; it must be nonempty, one line and terminated.
bool validateStatusItem(const StatusItem&, std::wstring& error);
// Copies at most StatusMaxTextLength characters, folding control characters
// (including line breaks) to spaces and trimming surrounding whitespace.
void setStatusText(StatusItem&, const wchar_t* text) noexcept;
enum class StatusIcon { None, Note, Stopwatch, Timer, Break, Overtime };
// One displayed line at one moment.
struct StatusView {
    bool visible = false;
    // Changes whenever the line should be replaced: a new status or the next
    // block of a repeating timer.
    uint64_t sequence = 0, phase = 0;
    StatusIcon icon = StatusIcon::None;
    std::array<wchar_t, StatusTextCapacity> label{};
    std::array<wchar_t, 16> value{};
    // Timer: remaining fraction 0..1. Stopwatch: position of the minute hand.
    double fraction = 0;
};
StatusView resolveStatus(const StatusItem&, uint64_t nowTick) noexcept;
// Whole seconds as m:ss or h:mm:ss, capped at 999:59:59. Negative is zero.
void formatStatusClock(int64_t milliseconds, bool overtime, std::array<wchar_t, 16>& output) noexcept;
// Feed history for one recording, advanced once per admitted frame.
class StatusFeed {
public:
    struct Row {
        StatusView view;
        int opacity = 0;      // 0..255 after every fade
        double slot = 0;      // rows from the corner, eased
        double enter = 1;     // 0..1 slide/fade-in of a new line
        double strike = 0;    // 0..1 strike-through progress
    };
    using Rows = std::array<Row, StatusFeedMaxRows + 1>;
    void reset(int outputFps) noexcept;
    // Record the status shown on video frame `frame` (frames never go back).
    void advance(const StatusItem&, uint64_t nowTick, uint64_t frame) noexcept;
    // Rows to draw on `frame`, newest first; returns how many are visible.
    size_t layout(uint64_t frame, Rows& rows) const noexcept;
    bool empty() const noexcept { return !count_; }
    // The line currently being shown (not yet replaced); false if none.
    bool current(StatusView& view) const noexcept {
        if (!count_ || entries_[0].ended) return false;
        view = entries_[0].view; return true;
    }
    // Frames until a replaced line has fully faded.
    uint64_t settleFrames() const noexcept;
    // Frames until a change has finished entering, sliding and striking.
    uint64_t transitionFrames() const noexcept { return uint64_t(std::max({enter_, slide_, strike_})); }
private:
    struct Entry { StatusView view; uint64_t startFrame = 0, endFrame = 0; bool ended = false; };
    std::array<Entry, StatusFeedMaxRows + 2> entries_{};
    size_t count_ = 0;
    int enter_ = 9, slide_ = 8, strike_ = 12, hold_ = 36, fade_ = 60;
    bool faded(const Entry&, uint64_t frame) const noexcept;
};
class StatusFeedRenderer {
public:
    StatusFeedRenderer() noexcept = default;
    StatusFeedRenderer(const StatusFeedRenderer&) = delete;
    StatusFeedRenderer& operator=(const StatusFeedRenderer&) = delete;
    // Prepares fonts and the tile for this output size. A video too small for
    // the feed succeeds with fits() false; apply then draws nothing.
    bool prepare(const StatusFeedSettings&, int outputWidth, int outputHeight, std::wstring& error);
    bool prepared() const noexcept { return prepared_; }
    bool fits() const noexcept { return fits_; }
    // Draw `frame` of the feed onto a full-size or smaller (preview) frame.
    // On failure the frame is unchanged. No allocation once prepared.
    bool apply(Frame&, const StatusFeed&, uint64_t frame, std::wstring& error);
    void reset() noexcept;
    RECT lastBounds() const noexcept { return lastBounds_; }
private:
    StatusFeedSettings settings_;
    OverlayFont label_, value_;
    OverlayCanvas canvas_;
    OverlayHalo halo_;
    int outputWidth_ = 0, outputHeight_ = 0, margin_ = 0, rowHeight_ = 0, icon_ = 0, gap_ = 0, labelWidth_ = 0, valueWidth_ = 0;
    int tileWidth_ = 0, tileHeight_ = 0, fadeLength_ = 0, em_ = 0;
    bool prepared_ = false, fits_ = false;
    RECT lastBounds_{};
};
}

// Status feed model, formatting and real GDI rendering on memory frames only.
#include "status_feed.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
thread_local bool forbidAllocation = false;
}
void* operator new(std::size_t bytes) {
    if (forbidAllocation) throw std::bad_alloc();
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) {
    if (forbidAllocation) throw std::bad_alloc();
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace lapse;
struct NoAllocation { NoAllocation() { forbidAllocation = true; } ~NoAllocation() { forbidAllocation = false; } };
constexpr uint64_t Base = 10'000'000;
StatusItem item(uint64_t sequence, StatusKind kind, const wchar_t* text, uint64_t start = Base, int64_t duration = 0, int64_t breakMs = 0) {
    StatusItem value; value.sequence = sequence; value.kind = kind; setStatusText(value, text);
    value.startTick = start; value.durationMs = duration; value.breakMs = breakMs; return value;
}
std::wstring text(const std::array<wchar_t, 16>& value) { return value.data(); }
std::wstring text(const std::array<wchar_t, StatusTextCapacity>& value) { return value.data(); }
Frame frame(int width, int height) {
    Frame value{width, height, {}}; value.pixels.resize(size_t(width) * height * 4);
    for (size_t i = 0; i < value.pixels.size(); i += 4) { value.pixels[i] = 90; value.pixels[i + 1] = 100; value.pixels[i + 2] = 110; value.pixels[i + 3] = 255; }
    return value;
}
bool original(const Frame& value, int x, int y) {
    const auto* p = value.pixels.data() + (size_t(y) * value.width + x) * 4;
    return p[0] == 90 && p[1] == 100 && p[2] == 110;
}
void settingsAndText() {
    std::wstring error; StatusFeedSettings settings;
    require(validateStatusFeedSettings(settings, error), "Default status settings rejected");
    for (int corner = 0; corner < 4; ++corner) for (int size = 0; size < 3; ++size) for (int style = 0; style < 2; ++style) {
        settings = {static_cast<StatusCorner>(corner), static_cast<StatusTextSize>(size), static_cast<StatusStyle>(style)};
        require(validateStatusFeedSettings(settings, error), "Valid status settings rejected");
    }
    settings = {}; settings.corner = static_cast<StatusCorner>(4); require(!validateStatusFeedSettings(settings, error) && !error.empty(), "Invalid corner accepted");
    settings = {}; settings.textSize = static_cast<StatusTextSize>(-1); require(!validateStatusFeedSettings(settings, error), "Invalid size accepted");
    settings = {}; settings.style = static_cast<StatusStyle>(2); require(!validateStatusFeedSettings(settings, error), "Invalid style accepted");
    StatusFeedSettings a, b; require(sameStatusFeedSettings(a, b), "Equal settings differ"); b.style = StatusStyle::EdgeFade; require(!sameStatusFeedSettings(a, b), "Style equality missed");

    StatusItem value;
    setStatusText(value, L"  Shower\r\nthen\tbreakfast  "); require(text(value.text) == L"Shower  then breakfast", "Text was not folded to one trimmed line");
    setStatusText(value, std::wstring(80, L'x').c_str()); require(text(value.text).size() == size_t(StatusMaxTextLength), "Text was not capped at 60 characters");
    setStatusText(value, nullptr); require(text(value.text).empty(), "Null text was not empty");
    setStatusText(value, L"Café ☕ 日本語"); require(text(value.text) == L"Café ☕ 日本語", "Unicode text changed");

    require(validateStatusItem({}, error), "Cleared status rejected");
    require(validateStatusItem(item(1, StatusKind::Note, L"First task done!!"), error), "Note rejected");
    require(validateStatusItem(item(1, StatusKind::Stopwatch, L"Shower"), error), "Stopwatch rejected");
    require(validateStatusItem(item(1, StatusKind::Timer, L"Work on Essay", Base, 3600000), error), "Timer rejected");
    require(validateStatusItem(item(1, StatusKind::Timer, L"Deep work", Base, 1500000, 300000), error), "Repeating timer rejected");
    require(!validateStatusItem(item(1, StatusKind::Note, L"   "), error) && !error.empty(), "Empty note accepted");
    require(!validateStatusItem(item(1, StatusKind::Note, L"x", Base, 1000), error), "Note with a length accepted");
    for (int64_t bad : {int64_t(0), int64_t(999), int64_t(1500), StatusMaxTimerMs + 1000})
        require(!validateStatusItem(item(1, StatusKind::Timer, L"x", Base, bad), error), "Invalid timer length accepted");
    require(!validateStatusItem(item(1, StatusKind::Timer, L"x", Base, 60000, 500), error), "Invalid break length accepted");
    auto bad = item(1, StatusKind::Note, L"x"); bad.text[0] = L'\n'; require(!validateStatusItem(bad, error), "Control character accepted");
    bad = item(1, StatusKind::Note, L"x"); bad.text.fill(L'a'); require(!validateStatusItem(bad, error), "Unterminated text accepted");
    bad.kind = static_cast<StatusKind>(9); require(!validateStatusItem(bad, error), "Invalid kind accepted");
}
void clocks() {
    std::array<wchar_t, 16> out{};
    const auto check = [&](int64_t ms, bool over, const wchar_t* expected) { formatStatusClock(ms, over, out); require(text(out) == expected, "Unexpected clock text"); };
    check(0, false, L"0:00"); check(-5, false, L"0:00"); check(59999, false, L"0:59"); check(60000, false, L"1:00");
    check(3599999, false, L"59:59"); check(3600000, false, L"1:00:00"); check(62000, true, L"+1:02");
    check(int64_t(1000) * 3600 * 2000, false, L"999:59:59"); check(INT64_MAX, true, L"+999:59:59");

    auto view = resolveStatus(item(3, StatusKind::Note, L"First task done!!"), Base + 5000);
    require(view.visible && view.icon == StatusIcon::Note && text(view.label) == L"First task done!!" && text(view.value).empty() && view.sequence == 3, "Note view wrong");
    view = resolveStatus(item(4, StatusKind::Stopwatch, L"Shower"), Base + 18 * 60000 + 4500);
    require(view.icon == StatusIcon::Stopwatch && text(view.value) == L"18:04" && std::abs(view.fraction - (18 * 60000 + 4500) / 3600000.0) < 1e-9, "Stopwatch view wrong");
    view = resolveStatus(item(4, StatusKind::Stopwatch, L"Shower"), Base - 100);
    require(text(view.value) == L"0:00", "A clock running backwards produced negative time");
    const auto timer = item(5, StatusKind::Timer, L"Work on Essay", Base, 3600000);
    view = resolveStatus(timer, Base); require(view.icon == StatusIcon::Timer && text(view.value) == L"1:00:00" && view.fraction == 1, "Timer start wrong");
    view = resolveStatus(timer, Base + 1); require(text(view.value) == L"1:00:00", "Timer must round remaining time up");
    view = resolveStatus(timer, Base + 1000); require(text(view.value) == L"59:59", "Timer second wrong");
    view = resolveStatus(timer, Base + 3599001); require(text(view.value) == L"0:01", "Last timer second wrong");
    view = resolveStatus(timer, Base + 3600000); require(view.icon == StatusIcon::Overtime && text(view.value) == L"+0:00" && view.phase == 0, "Timer end wrong");
    view = resolveStatus(timer, Base + 3600000 + 192000); require(text(view.value) == L"+3:12" && text(view.label) == L"Work on Essay", "Overtime wrong");
    const auto blocks = item(6, StatusKind::Timer, L"Deep work", Base, 1500000, 300000);
    view = resolveStatus(blocks, Base + 1499999); require(view.icon == StatusIcon::Timer && view.phase == 0 && text(view.value) == L"0:01", "Work block wrong");
    view = resolveStatus(blocks, Base + 1500000); require(view.icon == StatusIcon::Break && view.phase == 1 && text(view.label) == L"Break" && text(view.value) == L"5:00", "Break block wrong");
    view = resolveStatus(blocks, Base + 1800000); require(view.icon == StatusIcon::Timer && view.phase == 2 && text(view.label) == L"Deep work" && text(view.value) == L"25:00", "Second work block wrong");
    require(!resolveStatus({}, Base).visible, "Cleared status visible");
}
void feedTiming() {
    StatusFeed feed; feed.reset(30); StatusFeed::Rows rows{};
    require(feed.empty() && !feed.layout(0, rows), "New feed not empty");
    const auto a = item(1, StatusKind::Stopwatch, L"Shower");
    feed.advance(a, Base, 0);
    require(!feed.layout(0, rows), "A new line must start fully transparent");
    require(feed.layout(9, rows) == 1 && rows[0].opacity == 255 && rows[0].slot == 0 && rows[0].strike == 0, "Line did not finish entering in 0.3 s");
    for (uint64_t f = 1; f < 100; ++f) feed.advance(a, Base + f * 5000, f);
    require(text(rows[0].view.value) != L"8:15" && feed.layout(99, rows) == 1 && text(rows[0].view.value) == L"8:15", "Live stopwatch did not update per frame");
    const auto b = item(2, StatusKind::Timer, L"Work on Essay", Base + 600000, 3600000);
    feed.advance(b, Base + 600000, 100);
    require(feed.layout(100, rows) == 1 && text(rows[0].view.label) == L"Shower" && rows[0].strike == 0, "Old line changed before its first replaced frame");
    feed.advance(b, Base + 600000 + 30000, 106);
    require(feed.layout(106, rows) == 2 && text(rows[0].view.label) == L"Work on Essay" && rows[1].slot > 0.9 && rows[1].strike > 0.6, "Replacement did not slide and strike the old line");
    require(text(rows[1].view.value) == L"8:15", "Struck line did not keep its last value");
    feed.advance(b, Base + 600000, 112);
    require(feed.layout(112, rows) == 2 && rows[1].strike == 1 && rows[1].opacity == 166 && rows[1].slot == 1, "Struck line did not settle dimmed");
    require(feed.layout(100 + 36 + 30, rows) == 2 && rows[1].opacity < 166 && rows[1].opacity > 0, "Struck line did not fade");
    feed.advance(b, Base + 600000, 196);
    require(feed.layout(196, rows) == 1 && text(rows[0].view.label) == L"Work on Essay", "Faded line was not removed after 1.2 s hold and 2 s fade");
    // Clearing strikes the current line out with nothing new above it.
    feed.advance({}, Base, 200);
    require(feed.layout(212, rows) == 1 && rows[0].strike == 1 && rows[0].slot == 0, "Clear did not strike the line");
    feed.advance({}, Base, 296); require(feed.empty(), "Cleared feed retained a faded line");
    // Same text with a new sequence is a new status.
    feed.advance(item(7, StatusKind::Note, L"Same"), Base, 300); feed.advance(item(8, StatusKind::Note, L"Same"), Base, 320);
    require(feed.layout(330, rows) == 2, "A new sequence with the same text was merged");
    // Rapid changes keep at most three older lines; the rest hurry out.
    for (uint64_t i = 0; i < 6; ++i) feed.advance(item(20 + i, StatusKind::Note, L"Rapid"), Base, 400 + i);
    require(feed.layout(400 + 5 + 30, rows) <= 4, "Rapid changes left more than four lines");
    // Repeating timers replace the work block with the break at its exact end.
    StatusFeed blocks; blocks.reset(30);
    const auto deep = item(30, StatusKind::Timer, L"Deep work", Base, 60000, 30000);
    blocks.advance(deep, Base + 59000, 0); blocks.advance(deep, Base + 61000, 1);
    require(blocks.layout(20, rows) == 2 && text(rows[0].view.label) == L"Break" && text(rows[1].view.value) == L"0:00" && rows[1].view.fraction == 0, "Finished block did not read 0:00");
    // Frame-based timing scales with playback FPS.
    StatusFeed fast; fast.reset(60); fast.advance(a, Base, 0);
    require(fast.layout(9, rows) == 1 && rows[0].opacity < 255 && fast.layout(18, rows) == 1 && rows[0].opacity == 255, "Animation did not scale with FPS");
    require(fast.settleFrames() == 2 * feed.settleFrames(), "Settle frames did not scale with FPS");
    StatusFeed one; one.reset(1); one.advance(a, Base, 0); require(one.layout(1, rows) == 1 && rows[0].opacity == 255, "1 fps feed animation wrong");
}
void renderer() {
    std::wstring error; StatusFeed feed; feed.reset(30);
    feed.advance(item(1, StatusKind::Stopwatch, L"Shower"), Base + 600000, 0);
    feed.advance(item(2, StatusKind::Timer, L"Work on Essay", Base + 600000, 3600000), Base + 600000, 30);
    for (int corner = 0; corner < 4; ++corner) for (int style = 0; style < 2; ++style) for (int size = 0; size < 3; ++size) {
        StatusFeedSettings settings{static_cast<StatusCorner>(corner), static_cast<StatusTextSize>(size), static_cast<StatusStyle>(style)};
        StatusFeedRenderer r; require(r.prepare(settings, 1280, 720, error) && r.prepared() && r.fits(), "Status renderer preparation failed");
        auto value = frame(1280, 720); require(r.apply(value, feed, 50, error), "Status render failed");
        const auto bounds = r.lastBounds();
        require(bounds.right > bounds.left && bounds.bottom > bounds.top && bounds.left >= 0 && bounds.top >= 0 && bounds.right <= 1280 && bounds.bottom <= 720, "Status bounds invalid");
        const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
        require(right ? bounds.right > 1280 - 64 && bounds.left > 640 : bounds.left < 64 && bounds.right < 640, "Status feed left its horizontal corner");
        require(bottom ? bounds.bottom > 720 - 64 && bounds.top > 360 : bounds.top < 64 && bounds.bottom < 360, "Status feed left its vertical corner");
        size_t bright = 0, dark = 0, green = 0;
        for (int y = 0; y < 720; ++y) for (int x = 0; x < 1280; ++x) {
            const bool inside = x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
            if (!inside) { require(original(value, x, y), "Status feed changed pixels outside its bounds"); continue; }
            const auto* p = value.pixels.data() + (size_t(y) * 1280 + x) * 4;
            bright += p[0] > 220 && p[1] > 220 && p[2] > 220; dark += p[0] < 40 && p[1] < 40 && p[2] < 40;
            green += p[1] > 180 && p[2] < 140 && p[0] < 190;
        }
        require(bright > 50 && dark > 50 && green > 5, "Status text, halo or timer ring missing");
        auto preview = frame(640, 360); require(r.apply(preview, feed, 50, error), "Status preview failed");
        const auto small = r.lastBounds();
        require(small.left == bounds.left / 2 && small.top == bounds.top / 2 && small.right == (bounds.right + 1) / 2 && small.bottom == (bounds.bottom + 1) / 2, "Preview bounds did not follow full-size bounds");
    }
    // Overtime is amber; Unicode and very long text render inside the bounds.
    StatusFeed over; over.reset(30);
    over.advance(item(3, StatusKind::Timer, L"Café ☕ 日本語 and a very long status text that must be shortened", Base, 60000), Base + 200000, 0);
    StatusFeedRenderer r; require(r.prepare({}, 1280, 720, error), "Default renderer failed");
    auto value = frame(1280, 720); require(r.apply(value, over, 40, error), "Overtime render failed");
    size_t amber = 0; const auto bounds = r.lastBounds();
    for (int y = bounds.top; y < bounds.bottom; ++y) for (int x = bounds.left; x < bounds.right; ++x) {
        const auto* p = value.pixels.data() + (size_t(y) * 1280 + x) * 4; amber += p[2] > 200 && p[1] > 120 && p[1] < 210 && p[0] < 120;
    }
    require(amber > 10 && bounds.right <= 1280 * 2 / 3, "Overtime colour missing or long text escaped its width");
    // Empty feeds and unprepared renderers leave frames untouched.
    StatusFeed empty; empty.reset(30); auto untouched = frame(640, 360); const auto before = untouched.pixels;
    require(r.apply(untouched, empty, 10, error) && untouched.pixels == before && !r.lastBounds().right, "Empty feed drew");
    StatusFeedRenderer cold; require(cold.apply(untouched, feed, 50, error) && untouched.pixels == before, "Unprepared renderer drew");
    Frame invalid; require(!r.apply(invalid, feed, 50, error) && !error.empty(), "Invalid frame accepted");
    // A tiny video prepares but cannot show the feed.
    StatusFeedRenderer tiny; require(tiny.prepare({}, 48, 48, error) && tiny.prepared() && !tiny.fits(), "Tiny output should prepare without fitting");
    auto little = frame(48, 48); const auto littleBefore = little.pixels;
    require(tiny.apply(little, feed, 50, error) && little.pixels == littleBefore, "Tiny output drew a feed");
    for (const auto size : {std::pair<int,int>{47, 48}, {1279, 720}, {4096, 4096}, {0, 720}})
        require(!tiny.prepare({}, size.first, size.second, error) && !error.empty() && !tiny.prepared(), "Invalid output size accepted");
    StatusFeedSettings invalidSettings; invalidSettings.corner = static_cast<StatusCorner>(7);
    require(!tiny.prepare(invalidSettings, 1280, 720, error), "Invalid settings prepared");
}
void warmAndScaled() {
    std::wstring error; error.reserve(256);
    StatusFeedRenderer r; StatusFeed feed; feed.reset(30);
    for (const auto size : {std::pair<int,int>{1280, 720}, {1920, 1080}, {1080, 1920}, {3840, 2160}}) {
        require(r.prepare({}, size.first, size.second, error), "Scaled preparation failed");
        auto value = frame(size.first, size.second);
        const auto timer = item(4, StatusKind::Timer, L"Work on Essay", Base, 3600000);
        feed.reset(30); feed.advance(item(3, StatusKind::Stopwatch, L"Shower"), Base, 0); feed.advance(timer, Base, 20);
        require(r.apply(value, feed, 21, error), "Warm-up render failed");
        const auto start = std::chrono::steady_clock::now();
        { NoAllocation noAllocation;
          for (uint64_t f = 22; f < 82; ++f) { feed.advance(timer, Base + f * 5000, f); require(r.apply(value, feed, f, error), "Warm render failed"); }
          require(r.prepare({}, size.first, size.second, error), "Identical warm preparation failed"); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 60;
        std::cout << "  " << size.first << "x" << size.second << ": " << ms << " ms per animated frame\n";
        require(ms < 250, "Status rendering is too slow for live capture");
    }
    // Corner/style changes keep the fonts; a size change rebuilds them.
    StatusFeedSettings moved; moved.corner = StatusCorner::BottomRight; moved.style = StatusStyle::EdgeFade;
    require(r.prepare(moved, 3840, 2160, error), "Corner change failed");
    auto value = frame(3840, 2160); require(r.apply(value, feed, 81, error) && r.lastBounds().right == 3840, "Moved feed did not draw at its new corner");
}
}
int main() {
    try {
        settingsAndText(); std::cout << "PASS settings, one-line text folding and status validation\n";
        clocks(); std::cout << "PASS clocks, stopwatch, countdown rounding, overtime and repeating blocks\n";
        feedTiming(); std::cout << "PASS frame-timed enter, strike, slide, hold, fade, clear and FPS scaling\n";
        renderer(); std::cout << "PASS corners, styles, sizes, halo, colours, preview mapping and tiny/invalid outputs\n";
        warmAndScaled(); std::cout << "PASS zero warmed allocations and bounded cost up to 4K\n";
        return 0;
    } catch (const std::exception& error) { forbidAllocation = false; std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

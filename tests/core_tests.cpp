#include "core.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include "error_text_fixture.h"

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
bool nearly(double a, double b) { return std::abs(a - b) < 0.000001; }
lapse::Frame solid(int width, int height, uint8_t blue, uint8_t green, uint8_t red) {
    lapse::Frame result{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for (size_t i = 0; i < result.pixels.size(); i += 4) {
        result.pixels[i] = blue;
        result.pixels[i + 1] = green;
        result.pixels[i + 2] = red;
        result.pixels[i + 3] = 17; // Source alpha must never make video transparent.
    }
    return result;
}
bool pixel(const lapse::Frame& frame, int x, int y, uint8_t b, uint8_t g, uint8_t r) {
    if (!frame.valid() || x < 0 || y < 0 || x >= frame.width || y >= frame.height) return false;
    const size_t offset = (size_t(y) * frame.width + x) * 4;
    return frame.pixels[offset] == b && frame.pixels[offset + 1] == g &&
           frame.pixels[offset + 2] == r && frame.pixels[offset + 3] == 255;
}
bool darkPixel(const lapse::Frame& frame, int x, int y) {
    if (!frame.valid() || x < 0 || y < 0 || x >= frame.width || y >= frame.height) return false;
    const size_t offset = (size_t(y) * frame.width + x) * 4;
    return frame.pixels[offset] <= 40 && frame.pixels[offset + 1] <= 40 &&
           frame.pixels[offset + 2] <= 40 && frame.pixels[offset + 3] == 255;
}
void filePaths() {
    using lapse::fileIOPath;
    check(fileIOPath(L"C:\\Videos\\clip.mp4") == L"\\\\?\\C:\\Videos\\clip.mp4",
          "absolute drive paths use extended I/O spelling");
    check(fileIOPath(L"C:/Save Folder/../Videos/\u65e5\u672c.mp4") == L"\\\\?\\C:\\Videos\\\u65e5\u672c.mp4",
          "ordinary absolute paths normalize separators and dot segments before prefixing");
    check(fileIOPath(L"\\\\server\\share\\folder\\..\\clip.mp4") == L"\\\\?\\UNC\\server\\share\\clip.mp4",
          "absolute UNC paths retain their server/share in extended spelling");
    for (const auto* path : {L"", L"relative\\clip.mp4", L"C:clip.mp4", L"\\clip.mp4",
                            L"\\\\?\\C:\\Videos\\clip.mp4", L"\\\\?\\UNC\\server\\share\\clip.mp4", L"\\\\.\\pipe\\owned"}) {
        check(fileIOPath(path) == path, "relative, already extended and device paths retain their spelling");
    }
}
void geometry() {
    using namespace lapse;
    auto rect = constrain({-2, 3, -1, 4});
    check(nearly(rect.x, 0) && nearly(rect.y, 0) && nearly(rect.w, 0.1) && nearly(rect.h, 1),
          "negative sizes and off-canvas positions are constrained");
    rect = constrain({0.99, 0.8, 0.3, 0.4});
    check(nearly(rect.x, 0.7) && nearly(rect.y, 0.6), "right and bottom edges stay on canvas");
    rect = constrain({std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()});
    check(nearly(rect.x, 0) && nearly(rect.y, 0) && nearly(rect.w, 1) && nearly(rect.h, 1),
          "non-finite geometry gets safe finite defaults");
    const auto desktop = preset(Mode::Desktop);
    const auto camera = preset(Mode::Camera);
    check(desktop.size() == 1 && desktop[0].source == Source::Desktop && nearly(desktop[0].rect.w, 1),
          "desktop preset fills canvas");
    check(camera.size() == 1 && camera[0].source == Source::Camera && nearly(camera[0].rect.h, 1),
          "camera preset fills canvas");
    for (auto mode : {Mode::Overlay, Mode::SideBySide, Mode::Custom}) {
        const auto layers = preset(mode);
        check(layers.size() == 2 && layers[0].source == Source::Desktop && layers[1].source == Source::Camera,
              "combined presets contain both sources in back-to-front order");
    }
    check(preset(static_cast<Mode>(100)).empty(), "unknown preset does not silently select a source");
}
void composition() {
    using namespace lapse;
    auto desktop = solid(4, 4, 0, 0, 240);
    auto camera = solid(4, 4, 240, 0, 0);
    Frame result;
    std::wstring error = L"old error";
    check(compose(&desktop, nullptr, preset(Mode::Desktop), 4, 4, result, error), "desktop-only composition");
    check(error.empty() && result.valid() && pixel(result, 0, 0, 0, 0, 240) &&
          pixel(result, 3, 3, 0, 0, 240), "full-frame colors, alpha, and cleared error");
    check(compose(nullptr, &camera, preset(Mode::Camera), 8, 4, result, error), "camera-only composition");
    check(darkPixel(result, 0, 1) && pixel(result, 2, 1, 240, 0, 0) &&
          pixel(result, 5, 1, 240, 0, 0) && darkPixel(result, 7, 1),
          "complete square camera is centered with side letterbox bars");
    check(compose(&desktop, &camera, preset(Mode::SideBySide), 8, 4, result, error), "side-by-side composition");
    check(pixel(result, 0, 0, 0, 0, 240) && pixel(result, 3, 3, 0, 0, 240) &&
          pixel(result, 4, 0, 240, 0, 0) && pixel(result, 7, 3, 240, 0, 0),
          "side-by-side has an exact seam and correct sources");
    std::vector<Layer> layers{{Source::Desktop, {}}, {Source::Camera, {0.5, 0.5, 0.5, 0.5}}};
    check(compose(&desktop, &camera, layers, 8, 8, result, error), "custom overlay composition");
    check(pixel(result, 3, 4, 0, 0, 240) && pixel(result, 4, 4, 240, 0, 0), "front layer covers back layer");
    std::reverse(layers.begin(), layers.end());
    check(compose(&desktop, &camera, layers, 8, 8, result, error) && pixel(result, 7, 7, 0, 0, 240),
          "reversing layers reverses stacking order");
    camera = solid(4, 2, 240, 0, 0);
    layers = {{Source::Desktop, {}}, {Source::Camera, {0.5, 0.5, 0.5, 0.5}}};
    check(compose(&desktop, &camera, layers, 8, 8, result, error), "wide camera over square desktop");
    check(darkPixel(result, 4, 4) && pixel(result, 4, 5, 240, 0, 0) &&
          darkPixel(result, 4, 7), "foreground letterbox is opaque and preserves source aspect");
    auto patterned = solid(2, 2, 0, 0, 0);
    patterned.pixels[0] = 200;
    patterned.pixels[6] = 200;
    patterned.pixels[9] = 200;
    check(compose(&patterned, nullptr, preset(Mode::Desktop), 2, 2, result, error), "identity pixel composition");
    check(pixel(result, 0, 0, 200, 0, 0) && pixel(result, 1, 0, 0, 0, 200) &&
          pixel(result, 0, 1, 0, 200, 0) && pixel(result, 1, 1, 0, 0, 0),
          "BGRA channels and top-down row order are preserved");
    check(compose(&patterned, nullptr, preset(Mode::Desktop), 4, 4, result, error) &&
          pixel(result, 1, 1, 113, 38, 38), "bilinear scaling blends all four source pixels");
    check(compose(&patterned, nullptr, preset(Mode::Desktop), 4, 4, patterned, error) &&
          pixel(patterned, 1, 1, 113, 38, 38), "output can safely alias the source");
    layers = {{Source::Desktop, {0.99, 0.99, 0.1, 0.1}}};
    check(compose(&desktop, nullptr, layers, 2, 2, result, error) && pixel(result, 1, 1, 0, 0, 240),
          "tiny constrained layer still paints a pixel without exceeding frame bounds");
}
void failuresAndLimits() {
    using namespace lapse;
    auto desktop = solid(2, 2, 0, 230, 0);
    auto output = solid(2, 2, 1, 2, 3);
    const auto original = output.pixels;
    std::wstring error;
    const auto layers = preset(Mode::Desktop);
    for (auto dimensions : {std::pair<int, int>{0, 4}, {-2, 4}, {3, 4}, {4, 3}, {1922, 1080}, {1920, 1082}}) {
        check(!compose(&desktop, nullptr, layers, dimensions.first, dimensions.second, output, error) &&
              !error.empty(), "invalid dimensions fail with an explanation");
        check(output.pixels == original && output.width == 2 && output.height == 2,
              "failed composition preserves the previous output");
    }
    check(!compose(nullptr, nullptr, layers, 4, 4, output, error) && error.find(L"desktop") != std::wstring::npos,
          "missing desktop is an explicit failure");
    check(!compose(&desktop, nullptr, preset(Mode::Overlay), 4, 4, output, error) &&
          error.find(L"camera") != std::wstring::npos, "missing required camera is an explicit failure");
    Frame malformed{4, 4, {0, 1, 2}};
    check(!compose(&malformed, nullptr, layers, 4, 4, output, error), "truncated source buffer is rejected");
    malformed = {-2, 2, std::vector<uint8_t>(16)};
    check(!malformed.valid(), "negative source dimensions cannot validate");
    malformed = {(std::numeric_limits<int>::max)(), (std::numeric_limits<int>::max)(), {}};
    check(!malformed.valid(), "huge source dimensions cannot validate an empty buffer");
    check(!compose(&desktop, nullptr, {}, 4, 4, output, error), "empty layout is rejected");
    check(!compose(&desktop, nullptr, {{static_cast<Source>(100), {}}}, 4, 4, output, error),
          "unknown source is rejected");
    check(compose(&desktop, nullptr, layers, 1920, 1080, output, error) && output.valid(),
          "maximum supported video dimensions compose successfully");
    check(errorText(E_ACCESSDENIED).find(L"80070005") != std::wstring::npos,
          "HRESULT diagnostics include a searchable hexadecimal code");
    check(errorText(static_cast<HRESULT>(0xA1234567)).find(L"A1234567") != std::wstring::npos,
          "unknown HRESULT still produces a useful diagnostic");
}
void errorFormattingAndAllocationCleanup() {
    coreTestError = {};
    coreTestError.enabled = true;
    const std::pair<std::wstring, std::wstring> cases[] = {
        {L"Synthetic failure.\r\n  ", L"Synthetic failure. (0x80070005)"},
        {L"\u65e5\u672c %1!s! %2\r\n", L"\u65e5\u672c %1!s! %2 (0x80070005)"},
        {L"First\nsecond\t \r\n", L"First\nsecond\t (0x80070005)"},
        {L" \r\n ", L"The operation failed (0x80070005)"}
    };
    for (const auto& c : cases) {
        coreTestError.message = c.first;
        check(lapse::errorText(E_ACCESSDENIED) == c.second,
              "error text retains Unicode/inserts and trims only trailing CR/LF/spaces");
        check(!coreTestError.allocation && coreTestError.allocations == coreTestError.frees && !coreTestError.mismatchedFrees,
              "successful error formatting releases each message allocation exactly once");
    }
    coreTestError.failLookup = true;
    const int allocationsBefore = coreTestError.allocations;
    check(lapse::errorText(static_cast<HRESULT>(0xA1234567)) == L"The operation failed (0xA1234567)" &&
          coreTestError.allocations == allocationsBefore && coreTestError.code == 0xA1234567,
          "failed message lookup preserves native fallback text and exact HRESULT without allocating a buffer");
    coreTestError.failLookup = false;
    coreTestError.message.assign(1000, L'x');
    coreTestError.failCopy = true;
    const int freesBefore = coreTestError.frees;
    const int faultAllocationsBefore = coreTestError.allocations;
    bool caught = false;
    try { (void)lapse::errorText(E_OUTOFMEMORY); } catch (const std::bad_alloc&) { caught = true; }
    coreTestFailNextAllocation = false;
    std::cout << "error_text_copy_fault: caught=" << caught
              << " buffers_allocated=" << coreTestError.allocations - faultAllocationsBefore
              << " buffers_freed=" << coreTestError.frees - freesBefore
              << " buffer_retained=" << (coreTestError.allocation != nullptr) << '\n';
    check(caught && coreTestError.allocations == faultAllocationsBefore + 1 &&
          coreTestError.frees == freesBefore + 1 && !coreTestError.allocation && !coreTestError.mismatchedFrees,
          "failed message string allocation releases its FormatMessage buffer exactly once while unwinding");
    // Keep the fixture safe when replaying this regression against old source.
    if (coreTestError.allocation) { ::LocalFree(coreTestError.allocation); coreTestError.allocation = nullptr; }
    coreTestError.failCopy = false;
    check(lapse::errorText(E_OUTOFMEMORY) == coreTestError.message + L" (0x8007000E)" && !coreTestError.allocation,
          "error formatting recovers after a one-shot allocation failure");
    check(coreTestError.argumentsValid && coreTestError.code == static_cast<DWORD>(E_OUTOFMEMORY),
          "error formatting uses system messages with ignored inserts and the complete HRESULT");
    coreTestError.enabled = false;
}

}
int main() {
    filePaths();
    geometry();
    composition();
    failuresAndLimits();
    errorFormattingAndAllocationCleanup();
    if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
    std::cout << "All core checks passed\n";
    return 0;
}


#include "core.h"
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>

namespace lapse {
namespace {
constexpr uint8_t background = 20;
constexpr int maxWidth = 1920;
constexpr int maxHeight = 1080;

struct PixelRect { int left, top, right, bottom; };
struct Sample { int first, second; unsigned weight; };

PixelRect pixelsFor(Rect rect, int width, int height) {
    rect = constrain(rect);
    const int left = std::min(width - 1, static_cast<int>(std::lround(rect.x * width)));
    const int top = std::min(height - 1, static_cast<int>(std::lround(rect.y * height)));
    return {left, top,
        std::min(width, std::max(left + 1, static_cast<int>(std::lround((rect.x + rect.w) * width)))),
        std::min(height, std::max(top + 1, static_cast<int>(std::lround((rect.y + rect.h) * height))))};
}

void clearRect(Frame& frame, const PixelRect& rect) {
    for (int y = rect.top; y < rect.bottom; ++y) {
        auto* pixel = frame.pixels.data() + (size_t(y) * frame.width + rect.left) * 4;
        for (int x = rect.left; x < rect.right; ++x, pixel += 4) {
            pixel[0] = pixel[1] = pixel[2] = background;
            pixel[3] = 255;
        }
    }
}

Sample sampleFor(int position, int sourceSize, int targetSize) {
    // Pixel centers keep both the first and last edges inside the source.
    const double location = std::clamp((position + 0.5) * sourceSize / targetSize - 0.5,
                                       0.0, double(sourceSize - 1));
    const int first = static_cast<int>(location);
    return {first, std::min(first + 1, sourceSize - 1),
            static_cast<unsigned>(std::lround((location - first) * 256))};
}

void draw(const Frame& source, Frame& target, const PixelRect& bounds,
          std::vector<Sample>& horizontal) {
    const int boxWidth = bounds.right - bounds.left;
    const int boxHeight = bounds.bottom - bounds.top;
    const double scale = std::min(double(boxWidth) / source.width, double(boxHeight) / source.height);
    const int width = std::clamp(static_cast<int>(std::lround(source.width * scale)), 1, boxWidth);
    const int height = std::clamp(static_cast<int>(std::lround(source.height * scale)), 1, boxHeight);
    const int left = bounds.left + (boxWidth - width) / 2;
    const int top = bounds.top + (boxHeight - height) / 2;
    if (width == source.width && height == source.height) {
        // Desktop capture normally already matches the output size. Avoid
        // resampling those pixels while still normalizing ignored source alpha.
        for (int y = 0; y < height; ++y) {
            const auto* input = source.pixels.data() + size_t(y) * source.width * 4;
            auto* output = target.pixels.data() + (size_t(top + y) * target.width + left) * 4;
            for (int x = 0; x < width; ++x, input += 4, output += 4) {
                output[0] = input[0];
                output[1] = input[1];
                output[2] = input[2];
                output[3] = 255;
            }
        }
        return;
    }
    for (int x = 0; x < width; ++x) horizontal[x] = sampleFor(x, source.width, width);

    // Fixed-point bilinear interpolation avoids floating-point work per channel.
    for (int y = 0; y < height; ++y) {
        const auto vertical = sampleFor(y, source.height, height);
        const auto* firstRow = source.pixels.data() + size_t(vertical.first) * source.width * 4;
        const auto* secondRow = source.pixels.data() + size_t(vertical.second) * source.width * 4;
        auto* dest = target.pixels.data() + (size_t(top + y) * target.width + left) * 4;
        for (int x = 0; x < width; ++x, dest += 4) {
            const auto sample = horizontal[x];
            const size_t first = size_t(sample.first) * 4;
            const size_t second = size_t(sample.second) * 4;
            for (int c = 0; c < 3; ++c) {
                const unsigned upper = firstRow[first + c] * (256 - sample.weight) +
                                       firstRow[second + c] * sample.weight;
                const unsigned lower = secondRow[first + c] * (256 - sample.weight) +
                                       secondRow[second + c] * sample.weight;
                dest[c] = static_cast<uint8_t>((upper * (256 - vertical.weight) +
                                               lower * vertical.weight + 32768) >> 16);
            }
            dest[3] = 255;
        }
    }
}
}

std::vector<Layer> preset(Mode mode) {
    switch (mode) {
    case Mode::Desktop: return {{Source::Desktop, {0, 0, 1, 1}}};
    case Mode::Camera: return {{Source::Camera, {0, 0, 1, 1}}};
    case Mode::SideBySide:
        return {{Source::Desktop, {0, 0, 0.5, 1}}, {Source::Camera, {0.5, 0, 0.5, 1}}};
    case Mode::Overlay:
    case Mode::Custom:
        return {{Source::Desktop, {0, 0, 1, 1}}, {Source::Camera, {0.68, 0.68, 0.30, 0.30}}};
    }
    return {};
}

Rect constrain(Rect rect) {
    rect.w = std::clamp(std::isfinite(rect.w) ? rect.w : 1.0, 0.1, 1.0);
    rect.h = std::clamp(std::isfinite(rect.h) ? rect.h : 1.0, 0.1, 1.0);
    rect.x = std::clamp(std::isfinite(rect.x) ? rect.x : 0.0, 0.0, 1.0 - rect.w);
    rect.y = std::clamp(std::isfinite(rect.y) ? rect.y : 0.0, 0.0, 1.0 - rect.h);
    return rect;
}

bool compose(const Frame* desktop, const Frame* camera, const std::vector<Layer>& layers,
             int width, int height, Frame& output, std::wstring& error) {
    error.clear();
    if (width < 2 || height < 2 || width > maxWidth || height > maxHeight || width % 2 || height % 2) {
        error = L"Video dimensions must be even numbers from 2 to 1920 wide and 2 to 1080 high.";
        return false;
    }
    if (layers.empty()) {
        error = L"Add a desktop or camera layer before recording.";
        return false;
    }
    for (const auto& layer : layers) {
        if (layer.source != Source::Desktop && layer.source != Source::Camera) {
            error = L"The layout contains an unknown capture source.";
            return false;
        }
        const Frame* source = layer.source == Source::Desktop ? desktop : camera;
        if (!source || !source->valid()) {
            error = layer.source == Source::Desktop ?
                L"The desktop frame is unavailable. Check the selected display." :
                L"The camera frame is unavailable. Check that the camera is connected and accessible.";
            return false;
        }
    }
    try {
        // Usually retain the output allocation between frames. A temporary makes
        // in-place composition safe when the caller also passes output as a source.
        Frame temporary;
        Frame& target = (&output == desktop || &output == camera) ? temporary : output;
        std::vector<Sample> horizontal(static_cast<size_t>(width));
        target.pixels.resize(size_t(width) * height * 4);
        target.width = width;
        target.height = height;
        clearRect(target, {0, 0, width, height});
        for (const auto& layer : layers) {
            const Frame& source = *(layer.source == Source::Desktop ? desktop : camera);
            const auto bounds = pixelsFor(layer.rect, width, height);
            // Each layer owns its full rectangle, including its letterbox bars.
            clearRect(target, bounds);
            draw(source, target, bounds, horizontal);
        }
        if (&target != &output) output = std::move(temporary);
        return true;
    } catch (const std::bad_alloc&) {
        error = L"There is not enough memory to prepare the video frame.";
    } catch (const std::length_error&) {
        error = L"The video frame is too large to prepare.";
    }
    return false;
}

std::wstring fileIOPath(const std::wstring& path) {
    const std::filesystem::path filename(path);
    if (!filename.is_absolute() || path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\\\.\\", 0) == 0)
        return path;
    // Dot segments and forward slashes must be resolved before adding the
    // extended prefix, which disables ordinary Win32 path normalization.
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!required || required > 32768) return path;
    std::wstring normalized(required, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), required, normalized.data(), nullptr);
    if (!length || length >= required) return path;
    normalized.resize(length);
    return normalized.rfind(L"\\\\", 0) == 0
        ? L"\\\\?\\UNC\\" + normalized.substr(2) : L"\\\\?\\" + normalized;
}

std::wstring errorText(HRESULT hr) {
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
    wchar_t* message = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, static_cast<DWORD>(hr), 0,
                                       reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    const std::unique_ptr<wchar_t, decltype(&LocalFree)> owned(message, &LocalFree);
    std::wstring result;
    if (count && owned) result.assign(owned.get(), count);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
        result.pop_back();
    if (result.empty()) result = L"The operation failed";
    return result + L" (" + code + L")";
}
}

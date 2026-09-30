// Bounded adaptation of Tencent/ncnn pixel resize fixed-point math.
// Copyright 2018 Tencent. SPDX-License-Identifier: BSD-3-Clause.
// Original pinned source: ncnn e54f7b1f88434e1d844ea0551b880a1cfb079ce1,
// src/mat_pixel_resize.cpp. See ../third-party/ncnn-LICENSE.txt.
// Changes: BGRA input with alpha ignored, fixed cache, no row allocations,
// explicit byte/geometry limits, and bounded aspect-preserving BGR output.
#include "person_pixels.h"
#include <algorithm>
#include <cmath>

namespace person_pixels {
namespace {
void coefficients(int source, int target, std::array<int, side>& offsets,
                  std::array<int16_t, side * 2>& weights) noexcept {
    const double scale = double(source) / target;
    for (int at = 0; at < target; ++at) {
        float fraction = static_cast<float>((at + 0.5) * scale - 0.5);
        int low = static_cast<int>(std::floor(fraction));
        fraction -= low;
        if (low < 0) { low = 0; fraction = 0; }
        if (low >= source - 1) { low = source - 2; fraction = 1; }
        offsets[at] = low;
        weights[2 * at] = static_cast<int16_t>((1.0f - fraction) * 2048.0f + 0.5f);
        weights[2 * at + 1] = static_cast<int16_t>(fraction * 2048.0f + 0.5f);
    }
}
}
bool prepare(const uint8_t* bgra, size_t bytes, int width, int height,
    uint8_t* bgr, size_t outputBytes, Geometry& geometry, Cache& cache) noexcept {
    if (!bgra || !bgr || width < 2 || height < 2 || width > 8192 || height > 8192 ||
        bytes < size_t(width) * height * 4) return false;
    const int outWidth = width >= height ? side : std::max(1, int(int64_t(side) * width / height));
    const int outHeight = height >= width ? side : std::max(1, int(int64_t(side) * height / width));
    if (outputBytes < size_t(outWidth) * outHeight * 3) return false;
    if (cache.sourceWidth != width || cache.sourceHeight != height) {
        coefficients(width, outWidth, cache.x, cache.ax);
        coefficients(height, outHeight, cache.y, cache.ay);
        cache.width = outWidth; cache.height = outHeight;
        cache.sourceWidth = width; cache.sourceHeight = height;
    }
    for (int y = 0; y < outHeight; ++y) {
        const size_t row = size_t(cache.y[y]) * width * 4;
        const int b0 = cache.ay[2 * y], b1 = cache.ay[2 * y + 1];
        for (int x = 0; x < outWidth; ++x) {
            const size_t pixel = row + size_t(cache.x[x]) * 4;
            const int a0 = cache.ax[2 * x], a1 = cache.ax[2 * x + 1];
            for (int channel = 0; channel < 3; ++channel) {
                const size_t p = pixel + channel, below = p + size_t(width) * 4;
                const int horizontal0 = (bgra[p] * a0 + bgra[p + 4] * a1) >> 4;
                const int horizontal1 = (bgra[below] * a0 + bgra[below + 4] * a1) >> 4;
                const int output = (((b0 * horizontal0) >> 16) + ((b1 * horizontal1) >> 16) + 2) >> 2;
                bgr[(size_t(y) * outWidth + x) * 3 + channel] = static_cast<uint8_t>(output);
            }
        }
    }
    geometry = {outWidth, outHeight}; return true;
}
}

#pragma once
#include <cstddef>
#include <cstdint>

namespace lapse {
enum class CameraResolution : uint32_t { Standard720 = 0, Detail1080 = 1 };
struct CameraCaptureLimits {
    uint32_t width, height;
    size_t pixelBytes;
};
constexpr CameraCaptureLimits cameraCaptureLimits(CameraResolution resolution) noexcept {
    switch (resolution) {
    case CameraResolution::Standard720: return {1280, 720, 3686400};
    case CameraResolution::Detail1080: return {1920, 1080, 8294400};
    default: return {0, 0, 0};
    }
}
constexpr CameraResolution cameraResolutionForOutput(int width, int height) noexcept {
    return width > 1280 || height > 720 ? CameraResolution::Detail1080 : CameraResolution::Standard720;
}
}

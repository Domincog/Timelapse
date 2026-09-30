#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace person_pixels {
constexpr int side = 320;
constexpr size_t capacity = size_t(side) * side * 3;
struct Geometry { int width = 0, height = 0; };
struct Cache {
    int sourceWidth = 0, sourceHeight = 0, width = 0, height = 0;
    std::array<int, side> x{}, y{};
    std::array<int16_t, side * 2> ax{}, ay{};
};
// Input is packed BGRA, output packed BGR without padding.
// Input and output must be disjoint. Full scene aspect is preserved with longest
// side 320. No allocation, camera,
// runtime dependency or global state. Invalid input leaves output untouched.
bool prepare(const uint8_t* bgra, size_t bytes, int width, int height,
    uint8_t* bgr, size_t outputBytes, Geometry&, Cache&) noexcept;
}

#pragma once
#include "core.h"
#include <array>

namespace lapse {
constexpr int NightMaxWidth = 1920, NightMaxHeight = 1080;
constexpr int NightMinDurationMs = 1000, NightMaxDurationMs = 30000;
constexpr int NightInitialDurationMs = 3000;
constexpr unsigned NightMaxSamples = 300;
constexpr int NightCadenceMs = 200;
struct NightSettings {
    bool enabled = false;
    int durationMs = 0;
    int targetBrightness = 96;
};
bool validNightSettings(const NightSettings&) noexcept;
struct NightResult {
    uint32_t samples = 0;
    // Gain at the black end of the tone curve; brighter pixels receive less.
    double appliedGain = 1, inputBrightness = 0, outputBrightness = 0;
    int suggestedDurationMs = NightInitialDurationMs;
    bool targetLimited = false;
};
enum class NightAdd { Added, Duplicate, Invalid, Full };
#ifdef NIGHT_IMAGE_LAB
struct NightTuning { bool linear = true; double maxGain = 4; bool fixed = false; double fixedGain = 1; };
#endif
class NightAccumulator {
public:
    NightAccumulator() noexcept = default;
#ifdef NIGHT_IMAGE_LAB
    explicit NightAccumulator(NightTuning tuning) noexcept : linear_(tuning.linear), maxGain_(tuning.maxGain), fixed_(tuning.fixed), fixedGain_(tuning.fixedGain) {}
#endif
    bool prepare(int width, int height, std::wstring& error);
    bool begin(const NightSettings&) noexcept;
    NightAdd add(const Frame&, uint64_t sequence) noexcept;
    const Frame* finish(NightResult&) noexcept;
    void resetExposure() noexcept { gain_ = 1; previousMean_ = 0; metered_ = false; }
    size_t storageBytes() const noexcept { return sums_.capacity() * sizeof(uint32_t) + output_.pixels.capacity(); }
private:
    bool linear_ = true;
    double maxGain_ = 16;
    bool fixed_ = false;
    double fixedGain_ = 1;
    NightSettings settings_;
    Frame output_;
    std::vector<uint32_t> sums_;
    std::array<uint16_t, 65536> toneLut_{};
    std::array<uint64_t, 3> totals_{};
    uint64_t sequence_ = 0;
    unsigned count_ = 0;
    double gain_ = 1, previousMean_ = 0;
    bool active_ = false, finished_ = false, metered_ = false;
    NightResult result_;
};
namespace night_detail {
uint16_t linear(uint8_t value) noexcept;
uint8_t encoded(unsigned value) noexcept;
}
}

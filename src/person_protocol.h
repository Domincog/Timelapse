#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lapse::person {
// This POD wire format is shared only with the separately downloaded worker.
// All metadata and counters are protected by the named transport mutex.
inline constexpr uint32_t Magic = 0x50534E54;
inline constexpr uint32_t ProtocolVersion = 1;
inline constexpr uint32_t ModelRevision = 1;
inline constexpr uint32_t PreprocessingRevision = 1;
inline constexpr uint32_t InputSide = 320;
inline constexpr size_t MaxBgrBytes = size_t(InputSide) * InputSide * 3;
inline constexpr uint32_t MaxSourceDimension = 8192;
inline constexpr uint64_t SourceFreshnessMs = 3000;
inline constexpr float AbsentThreshold = 0.10f;
inline constexpr float PresentThreshold = 0.25f;

enum class Phase : uint32_t { Starting, Ready, Failed };
enum class Verdict : uint32_t { Unknown, QualifiedAbsent, Present };
enum class Reason : uint32_t {
    None, Ambiguous, InvalidInput, ModelFailure, InvalidOutput, StaleSource, InsufficientDetail
};
struct Source {
    uint64_t sessionToken = 0, cameraEpoch = 0, sequence = 0, receivedTick = 0;
    uint32_t sourceWidth = 0, sourceHeight = 0;
    uint32_t width = 0, height = 0; // Packed, unpadded BGR thumbnail geometry.
};
struct Output {
    float rawMaxPerson = 0, validMaxPerson = 0;
    Verdict verdict = Verdict::Unknown;
    Reason reason = Reason::None;
};
using Input = std::array<uint8_t, MaxBgrBytes>;
struct Shared {
    uint32_t magic = Magic, version = ProtocolVersion, structBytes = 0;
    uint32_t modelRevision = ModelRevision, preprocessingRevision = PreprocessingRevision;
    Phase phase = Phase::Starting;
    uint64_t requested = 0, published = 0;
    Source inputSource{}, outputSource{};
    uint32_t inputBytes = 0, reserved = 0;
    Output output{};
    Input input{};
};
inline bool validGeometry(const Source& source) noexcept {
    if (source.sourceWidth < 2 || source.sourceHeight < 2 ||
        source.sourceWidth > MaxSourceDimension || source.sourceHeight > MaxSourceDimension) return false;
    const uint32_t w = source.sourceWidth >= source.sourceHeight ? InputSide :
        (uint32_t((uint64_t(InputSide) * source.sourceWidth) / source.sourceHeight) ?
         uint32_t((uint64_t(InputSide) * source.sourceWidth) / source.sourceHeight) : 1);
    const uint32_t h = source.sourceHeight >= source.sourceWidth ? InputSide :
        (uint32_t((uint64_t(InputSide) * source.sourceHeight) / source.sourceWidth) ?
         uint32_t((uint64_t(InputSide) * source.sourceHeight) / source.sourceWidth) : 1);
    return source.width == w && source.height == h;
}
inline bool sameSource(const Source& a, const Source& b) noexcept {
    return a.sessionToken == b.sessionToken && a.cameraEpoch == b.cameraEpoch &&
        a.sequence == b.sequence && a.receivedTick == b.receivedTick &&
        a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight &&
        a.width == b.width && a.height == b.height;
}
inline bool validHeader(const Shared& shared) noexcept {
    return shared.magic == Magic && shared.version == ProtocolVersion && shared.structBytes == sizeof(Shared) &&
        shared.modelRevision == ModelRevision && shared.preprocessingRevision == PreprocessingRevision &&
        shared.phase <= Phase::Failed && shared.published <= shared.requested && !shared.reserved;
}
static_assert(sizeof(Source) == 48 && sizeof(Output) == 16, "Fixed person wire layout");
static_assert(sizeof(Shared) == 307360, "Fixed person mapping size");
static_assert(std::is_standard_layout_v<Shared> && std::is_trivially_copyable_v<Shared>, "POD person transport");
}

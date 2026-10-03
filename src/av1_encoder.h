#pragma once
#include "core.h"

namespace lapse {
// Software AV1 encoding with the pinned libaom encoder for one fixed-size,
// 8-bit 4:2:0 BT.709 limited-range stream. There is no lookahead: every
// accepted frame immediately yields exactly one shown temporal unit, so
// written samples and frame counts stay one-to-one as in the H.264 modes.
// Use from one thread. After any failed encode() the stream is unusable and
// later frames are refused, because libaom may already reference that frame.
class Av1Encoder {
public:
    Av1Encoder();
    ~Av1Encoder();
    Av1Encoder(const Av1Encoder&) = delete;
    Av1Encoder& operator=(const Av1Encoder&) = delete;

    // Width and height must be even and valid for recording.
    bool open(int width, int height, int fps, EncodingQuality quality, std::wstring& error);
    // The sequence header OBU for the MP4 av1C box; available after open().
    const std::vector<uint8_t>& sequenceHeader() const noexcept;
    // nv12 is tightly packed: width x height luma, then interleaved chroma.
    // On success, unit holds one complete temporal unit without temporal
    // delimiter or padding OBUs, as stored in MP4 samples.
    bool encode(const uint8_t* nv12, std::vector<uint8_t>& unit, bool& keyFrame, std::wstring& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

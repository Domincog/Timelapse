#pragma once
#include "core.h"

namespace lapse {
// A complete shown temporal unit in display order, including any hidden
// reference pictures. SVT-AV1 outputs DTS=PTS. Delimiter/padding OBUs are
// removed to conform to the AV1 MP4 binding.
struct Av1Packet {
    std::vector<uint8_t> unit;
    int64_t pts = 0;
    bool keyFrame = false;
};
// Bundled SVT-AV1, fixed-size 8-bit 4:2:0 BT.709 limited range. Use from one
// thread. encode() copies input into SVT's lookahead queue. Drain receive()
// after submitting frames; call end() and receive() through EOS at stop.
// Any failed operation permanently stops the stream.
class Av1Encoder {
public:
    Av1Encoder();
    ~Av1Encoder();
    Av1Encoder(const Av1Encoder&) = delete;
    Av1Encoder& operator=(const Av1Encoder&) = delete;

    bool open(int width, int height, int fps, EncodingQuality quality, std::wstring& error,
              const EncodingOptions& options = {});
    // The sequence header OBU for the MP4 av1C box; available after open().
    const std::vector<uint8_t>& sequenceHeader() const noexcept;
    // Tightly packed NV12: width x height luma, then interleaved chroma.
    bool encode(const uint8_t* nv12, std::wstring& error);
    // Tightly packed I420: luma followed by separate U and V quarter planes.
    // Avoids NV12 rearrangement; input may be reused when this call returns.
    bool encodeI420(const uint8_t* i420, std::wstring& error);
    bool end(std::wstring& error);
    // Nonblocking before end(), blocking after it. Empty unit means no packet
    // is ready. EOS can accompany the final nonempty unit.
    bool receive(Av1Packet& packet, bool& eos, std::wstring& error);

private:
    bool encodePlanes(const uint8_t* y, const uint8_t* u, const uint8_t* v, std::wstring& error);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

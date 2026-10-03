#include "av1_encoder.h"
#include <aom/aom_encoder.h>
#include <aom/aomcx.h>
#include <algorithm>
#include <utility>
#include <new>

namespace lapse {
namespace {
// Settings measured on the synthetic encoding corpus; see DEVELOPMENT.md.
// Speed 6 is the fastest good-quality preset (faster speeds are the same in
// this usage). Four threads keep frame latency moderate without occupying a
// whole machine. A 1080p desktop keyframe can outweigh minutes of unchanged
// frames, so keyframes are ten playback seconds apart.
constexpr int encoderSpeed = 6;
constexpr unsigned maximumThreads = 4;
constexpr unsigned keyframeSeconds = 10;
constexpr int obuSequenceHeader = 1, obuTemporalDelimiter = 2, obuPadding = 15;

// Constant-quality levels. Balanced matches or exceeds the H.264 Balanced
// modes' measured detail on every corpus scene with much smaller files.
unsigned quantizerLevel(EncodingQuality quality) {
    return quality == EncodingQuality::Compact ? 32 : quality == EncodingQuality::Detail ? 16 : 24;
}

std::wstring widen(const char* text) {
    std::wstring result;
    for (; text && *text; ++text) result += static_cast<wchar_t>(static_cast<unsigned char>(*text));
    return result;
}

bool fail(const aom_codec_ctx_t* codec, aom_codec_err_t result, const wchar_t* stage, std::wstring& error) {
    error = stage;
    if (result == AOM_CODEC_MEM_ERROR) {
        error += L": not enough memory.";
        return false;
    }
    error += L": " + widen(aom_codec_err_to_string(result));
    const char* detail = codec ? aom_codec_error_detail(codec) : nullptr;
    if (detail && *detail) error += L" (" + widen(detail) + L")";
    return false;
}

// Measures one OBU at the start of data. libaom always writes size fields;
// reject anything else rather than guess where a following OBU begins.
bool nextObu(const uint8_t* data, size_t size, size_t& length, int& type) {
    if (!size || (data[0] & 0x80) || !(data[0] & 0x02)) return false;
    type = (data[0] >> 3) & 0x0f;
    size_t at = (data[0] & 0x04) ? 2 : 1;
    uint64_t payload = 0;
    for (int byte = 0; ; ++byte) {
        if (byte == 8 || at >= size) return false;
        payload |= uint64_t(data[at] & 0x7f) << (7 * byte);
        if (!(data[at++] & 0x80)) break;
    }
    if (payload > size - at) return false;
    length = at + static_cast<size_t>(payload);
    return true;
}

// The first sequence header OBU in a temporal unit, or none.
std::pair<const uint8_t*, size_t> sequenceHeaderIn(const std::vector<uint8_t>& unit) {
    for (size_t at = 0, length = 0; at < unit.size(); at += length) {
        int type = 0;
        if (!nextObu(unit.data() + at, unit.size() - at, length, type)) break;
        if (type == obuSequenceHeader) return {unit.data() + at, length};
    }
    return {nullptr, 0};
}

struct Codec {
    aom_codec_ctx_t context{};
    bool initialized = false;
    Codec() = default;
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    ~Codec() { if (initialized) aom_codec_destroy(&context); }
};

bool start(Codec& codec, unsigned width, unsigned height, int fps, EncodingQuality quality, std::wstring& error) {
    aom_codec_iface_t* const encoder = aom_codec_av1_cx();
    aom_codec_enc_cfg_t config{};
    aom_codec_err_t result = aom_codec_enc_config_default(encoder, &config, AOM_USAGE_GOOD_QUALITY);
    if (result != AOM_CODEC_OK) return fail(nullptr, result, L"Cannot configure the AV1 encoder", error);
    config.g_w = width;
    config.g_h = height;
    config.g_timebase = {1, fps};
    config.g_threads = std::clamp<unsigned>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1, maximumThreads);
    config.g_profile = 0;
    config.g_bit_depth = AOM_BITS_8;
    config.g_input_bit_depth = 8;
    config.g_pass = AOM_RC_ONE_PASS;
    // No lookahead: each frame is output by the call that accepts it.
    config.g_lag_in_frames = 0;
    config.rc_end_usage = AOM_Q;
    config.kf_mode = AOM_KF_AUTO;
    config.kf_min_dist = 0;
    config.kf_max_dist = keyframeSeconds * static_cast<unsigned>(fps);
    result = aom_codec_enc_init(&codec.context, encoder, &config, 0);
    if (result != AOM_CODEC_OK) return fail(&codec.context, result, L"Cannot start the AV1 encoder", error);
    codec.initialized = true;
    aom_codec_ctx_t* const context = &codec.context;
    result = aom_codec_control(context, AOME_SET_CPUUSED, encoderSpeed);
    if (result == AOM_CODEC_OK) result = aom_codec_control(context, AOME_SET_CQ_LEVEL, quantizerLevel(quality));
    if (result == AOM_CODEC_OK) result = aom_codec_control(context, AV1E_SET_ROW_MT, 1u);
    // Detect anti-aliased desktop text for palette and intra block copy at
    // keyframes. Camera-like content encodes identically either way.
    if (result == AOM_CODEC_OK) result = aom_codec_control(context, AV1E_SET_SCREEN_CONTENT_DETECTION_MODE,
                                                           static_cast<int>(AOM_SCREEN_DETECTION_ANTIALIASING_AWARE));
    // Match the BT.709 limited-range NV12 conversion shared with H.264.
    if (result == AOM_CODEC_OK) result = aom_codec_control(context, AV1E_SET_COLOR_PRIMARIES, static_cast<int>(AOM_CICP_CP_BT_709));
    if (result == AOM_CODEC_OK) result = aom_codec_control(context, AV1E_SET_TRANSFER_CHARACTERISTICS, static_cast<int>(AOM_CICP_TC_BT_709));
    if (result == AOM_CODEC_OK) result = aom_codec_control(context, AV1E_SET_MATRIX_COEFFICIENTS, static_cast<int>(AOM_CICP_MC_BT_709));
    if (result == AOM_CODEC_OK) result = aom_codec_control(context, AV1E_SET_COLOR_RANGE, static_cast<int>(AOM_CR_STUDIO_RANGE));
    if (result != AOM_CODEC_OK) return fail(context, result, L"Cannot configure the AV1 encoder", error);
    return true;
}

// Encodes one frame into exactly one temporal unit, without temporal
// delimiter or padding OBUs. Leaves unit empty on failure.
bool encodeUnit(Codec& codec, unsigned width, unsigned height, aom_codec_pts_t pts, const uint8_t* nv12,
                std::vector<uint8_t>& unit, bool& keyFrame, std::wstring& error) {
    unit.clear();
    keyFrame = false;
    aom_image_t image{};
    if (!aom_img_wrap(&image, AOM_IMG_FMT_NV12, width, height, 1, const_cast<uint8_t*>(nv12))) {
        error = L"Cannot prepare the frame for AV1 encoding.";
        return false;
    }
    const aom_codec_err_t result = aom_codec_encode(&codec.context, &image, pts, 1, 0);
    if (result != AOM_CODEC_OK) return fail(&codec.context, result, L"Cannot encode the AV1 frame", error);
    int frames = 0;
    aom_codec_iter_t iterator = nullptr;
    try {
        while (const aom_codec_cx_pkt_t* packet = aom_codec_get_cx_data(&codec.context, &iterator)) {
            if (packet->kind != AOM_CODEC_CX_FRAME_PKT) continue;
            ++frames;
            keyFrame = keyFrame || (packet->data.frame.flags & AOM_FRAME_IS_KEY) != 0;
            const auto* data = static_cast<const uint8_t*>(packet->data.frame.buf);
            for (size_t remaining = packet->data.frame.sz, length = 0; remaining; data += length, remaining -= length) {
                int type = 0;
                if (!data || !nextObu(data, remaining, length, type)) {
                    error = L"The AV1 encoder produced an unsupported bitstream.";
                    unit.clear();
                    return false;
                }
                // The AV1 ISO base media file format binding excludes these from samples.
                if (type != obuTemporalDelimiter && type != obuPadding) unit.insert(unit.end(), data, data + length);
            }
        }
    } catch (const std::bad_alloc&) {
        error = L"Cannot store the encoded AV1 frame: not enough memory.";
        unit.clear();
        return false;
    }
    if (frames != 1 || unit.empty()) {
        error = L"The AV1 encoder did not produce exactly one video frame.";
        unit.clear();
        return false;
    }
    return true;
}
}

struct Av1Encoder::Impl {
    Codec codec;
    // Any failure may leave libaom holding a reference frame the caller never
    // stored, so later frames could depend on missing data. Refuse them.
    bool failed = false;
    unsigned width = 0, height = 0;
    aom_codec_pts_t pts = 0;
    std::vector<uint8_t> sequence;
};

Av1Encoder::Av1Encoder() = default;
Av1Encoder::~Av1Encoder() = default;

bool Av1Encoder::open(int width, int height, int fps, EncodingQuality quality, std::wstring& error) {
    error.clear();
    if (impl_) {
        error = L"The AV1 encoder is already open.";
        return false;
    }
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || fps <= 0) {
        error = L"The AV1 encoder needs even video dimensions and a positive frame rate.";
        return false;
    }
    if (quality != EncodingQuality::Compact && quality != EncodingQuality::Balanced && quality != EncodingQuality::Detail) {
        error = L"Choose a valid video quality.";
        return false;
    }
    auto candidate = std::make_unique<Impl>();
    candidate->width = static_cast<unsigned>(width);
    candidate->height = static_cast<unsigned>(height);
    // libaom finalizes some sequence-level tools from speed, size and thread
    // settings while encoding its first frame, so its init-time global header
    // can disagree with the bitstream. Take the final header from a separate
    // short-lived instance with identical settings, before the real one starts.
    {
        Codec primer;
        if (!start(primer, candidate->width, candidate->height, fps, quality, error)) return false;
        std::vector<uint8_t> flat(size_t(width) * height * 3 / 2, 128), unit;
        bool keyFrame = false;
        if (!encodeUnit(primer, candidate->width, candidate->height, 0, flat.data(), unit, keyFrame, error)) return false;
        const auto header = sequenceHeaderIn(unit);
        if (!keyFrame || !header.first) {
            error = L"Cannot read the AV1 sequence header.";
            return false;
        }
        candidate->sequence.assign(header.first, header.first + header.second);
    }
    if (!start(candidate->codec, candidate->width, candidate->height, fps, quality, error)) return false;
    impl_ = std::move(candidate);
    return true;
}

const std::vector<uint8_t>& Av1Encoder::sequenceHeader() const noexcept {
    static const std::vector<uint8_t> none;
    return impl_ ? impl_->sequence : none;
}

bool Av1Encoder::encode(const uint8_t* nv12, std::vector<uint8_t>& unit, bool& keyFrame, std::wstring& error) {
    error.clear();
    unit.clear();
    keyFrame = false;
    if (!impl_ || !nv12) {
        error = L"The AV1 encoder is not open.";
        return false;
    }
    if (impl_->failed) {
        error = L"The AV1 encoder stopped after an earlier error.";
        return false;
    }
    impl_->failed = true;
    if (!encodeUnit(impl_->codec, impl_->width, impl_->height, impl_->pts, nv12, unit, keyFrame, error)) return false;
    if (keyFrame) {
        // av1C promises this exact header; never write a stream contradicting it.
        const auto header = sequenceHeaderIn(unit);
        if (!header.first || !std::equal(header.first, header.first + header.second,
                                         impl_->sequence.begin(), impl_->sequence.end())) {
            error = L"The AV1 sequence header changed unexpectedly.";
            unit.clear();
            keyFrame = false;
            return false;
        }
    }
    ++impl_->pts;
    impl_->failed = false;
    return true;
}
}

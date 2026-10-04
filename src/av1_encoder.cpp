#include "av1_encoder.h"
#include "config.h"
#include <EbSvtAv1Enc.h>
#include <algorithm>
#include <new>

namespace lapse {
namespace {
constexpr unsigned keyframeSeconds = 10;
constexpr int obuSequenceHeader = 1, obuTemporalDelimiter = 2, obuPadding = 15;
int qualityCrf(EncodingQuality quality) {
    switch (quality) {
    case EncodingQuality::ExtraSmall: return 44;
    case EncodingQuality::Compact: return 36;
    case EncodingQuality::Detail: return 24;
    default: return 32;
    }
}
bool fail(EbErrorType result, const wchar_t* stage, std::wstring& error) {
    error = stage;
    if (result == EB_ErrorInsufficientResources) error += L": not enough memory.";
    else if (result == EB_ErrorBadParameter) error += L": unsupported SVT-AV1 settings.";
    else error += L" (SVT-AV1 error " + std::to_wstring(static_cast<uint32_t>(result)) + L").";
    return false;
}
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
struct OutputBuffer {
    EbBufferHeaderType* value = nullptr;
    ~OutputBuffer() { if (value) svt_av1_enc_release_out_buffer(&value); }
};
}
struct Av1Encoder::Impl {
    EbComponentType* codec = nullptr;
    bool initialized = false, failed = false, ending = false, eos = false, priming = false;
    unsigned width = 0, height = 0;
    int64_t submitted = 0, received = 0;
    std::vector<uint8_t> sequence, chroma;
    ~Impl() {
        if (initialized) svt_av1_enc_deinit(codec);
        if (codec) svt_av1_enc_deinit_handle(codec);
    }
};
Av1Encoder::Av1Encoder() = default;
Av1Encoder::~Av1Encoder() = default;

bool Av1Encoder::open(int width, int height, int fps, EncodingQuality quality, std::wstring& error,
                      const EncodingOptions& options) {
    error.clear();
    if (impl_) { error = L"The AV1 encoder is already open."; return false; }
    if (!validateVideoSize(width, height, error) || !validateOutputFps(fps, error) ||
        !validateEncodingOptions(EncodingMode::SoftwareAV1, options, error)) return false;
    if (quality != EncodingQuality::Compact && quality != EncodingQuality::Balanced &&
        quality != EncodingQuality::Detail && quality != EncodingQuality::ExtraSmall) {
        error = L"Choose a valid video quality."; return false;
    }
    // SVT disables adaptive quantization below 64 pixels, and its VBR
    // implementation requires it. Preserve tiny quality-based recordings,
    // but reject this unsupported combination before creating an output file.
    if (options.rateControl == EncodingRateControl::TargetBitrate && (width < 64 || height < 64)) {
        error = L"SVT-AV1 target bitrate needs video dimensions of at least 64 x 64. "
            L"Choose a larger video size or use automatic quality / CRF.";
        return false;
    }
    auto start = [&](std::unique_ptr<Impl>& candidate) {
        candidate = std::make_unique<Impl>();
        candidate->width = static_cast<unsigned>(width);
        candidate->height = static_cast<unsigned>(height);
        candidate->chroma.resize(size_t(width) * height / 2);
        EbSvtAv1EncConfiguration config{};
        EbErrorType result = svt_av1_enc_init_handle(&candidate->codec, &config);
        if (result != EB_ErrorNone) return fail(result, L"Cannot create the AV1 encoder", error);
        config.source_width = candidate->width;
        config.source_height = candidate->height;
        config.frame_rate_numerator = static_cast<unsigned>(fps);
        config.frame_rate_denominator = 1;
        config.encoder_bit_depth = 8;
        config.encoder_color_format = EB_YUV420;
        config.profile = MAIN_PROFILE;
        config.enc_mode = static_cast<int8_t>(options.av1Preset);
        // SVT 4.2 supports random access in its single-worker mode. Retain
        // lookahead and temporal tools while bounding frame/worker pools for
        // the recorder, including simultaneous source outputs.
        config.level_of_parallelism = 1;
        // A 16-frame random-access mini-GOP retains bidirectional prediction,
        // TPL and temporal filtering while keeping 1080p lookahead pools
        // suitable for a recorder. The upstream 32-frame default exceeds the
        // app's memory budget even in its single-worker mode.
        config.hierarchical_levels = 4;
        config.intra_period_length = static_cast<int32_t>(keyframeSeconds * fps - 1);
        config.intra_refresh_type = SVT_AV1_KF_REFRESH;
        config.color_primaries = EB_CICP_CP_BT_709;
        config.transfer_characteristics = EB_CICP_TC_BT_709;
        config.matrix_coefficients = EB_CICP_MC_BT_709;
        config.color_range = EB_CR_STUDIO_RANGE;
        // SVT implements CRF 64..70 as base QP plus extended offset. Use its
        // parameter parser instead of incorrectly assigning these to config.qp.
        const int crf = options.rateControl == EncodingRateControl::ConstantQuality ? options.av1Crf : qualityCrf(quality);
        result = svt_av1_enc_parse_parameter(&config, "crf", std::to_string(crf).c_str());
        if (options.rateControl == EncodingRateControl::TargetBitrate) {
            config.rate_control_mode = SVT_AV1_RC_MODE_VBR;
            config.target_bit_rate = static_cast<uint32_t>(options.bitrateKbps) * 1000;
        }
        if (result == EB_ErrorNone) result = svt_av1_enc_set_parameter(candidate->codec, &config);
        if (result != EB_ErrorNone) return fail(result, L"Cannot configure the AV1 encoder", error);
        result = svt_av1_enc_init(candidate->codec);
        if (result != EB_ErrorNone) return fail(result, L"Cannot start the AV1 encoder", error);
        candidate->initialized = true;
        EbBufferHeaderType* header = nullptr;
        result = svt_av1_enc_stream_header(candidate->codec, &header);
        struct HeaderRelease {
            EbBufferHeaderType* value;
            ~HeaderRelease() { if (value) svt_av1_enc_stream_header_release(value); }
        } release{header};
        if (result != EB_ErrorNone) return fail(result, L"Cannot read the AV1 sequence header", error);
        if (header && header->p_buffer) {
            for (size_t at = 0, length = 0; at < header->n_filled_len; at += length) {
                int type = 0;
                if (!nextObu(header->p_buffer + at, header->n_filled_len - at, length, type)) break;
                if (type == obuSequenceHeader) {
                    candidate->sequence.assign(header->p_buffer + at, header->p_buffer + at + length); break;
                }
            }
        }
        if (candidate->sequence.empty()) { error = L"Cannot read the AV1 sequence header."; return false; }
        return true;
    };
    // SVT derives some sequence-level tools on its first frame, after the
    // init-time stream_header API. Obtain the exact final header with one
    // identical short-lived encoder. Finish and destroy it before starting
    // the recording instance so the large lookahead pools never overlap.
    std::vector<uint8_t> sequence;
    {
        Av1Encoder primer;
        if (!start(primer.impl_)) return false;
        primer.impl_->priming = true;
        std::vector<uint8_t> flat(size_t(width) * height * 3 / 2, 128);
        if (!primer.encode(flat.data(), error) || !primer.end(error)) return false;
        Av1Packet packet;
        bool eos = false;
        int received = 0;
        do {
            if (!primer.receive(packet, eos, error)) return false;
            if (!packet.unit.empty()) {
                if (!packet.keyFrame || packet.pts != 0 || ++received != 1) {
                    error = L"Cannot read the final AV1 sequence header."; return false;
                }
            }
        } while (!eos);
        if (received != 1) { error = L"Cannot read the final AV1 sequence header."; return false; }
        sequence = primer.sequenceHeader();
    }
    std::unique_ptr<Impl> candidate;
    if (!start(candidate)) return false;
    candidate->sequence = std::move(sequence);
    impl_ = std::move(candidate);
    return true;
}
const std::vector<uint8_t>& Av1Encoder::sequenceHeader() const noexcept {
    static const std::vector<uint8_t> none;
    return impl_ ? impl_->sequence : none;
}
bool Av1Encoder::encode(const uint8_t* nv12, std::wstring& error) {
    error.clear();
    if (!impl_ || !nv12) { error = L"The AV1 encoder is not open."; return false; }
    if (impl_->failed || impl_->ending) { error = L"The AV1 encoder has stopped."; return false; }
    impl_->failed = true;
    const size_t luma = size_t(impl_->width) * impl_->height, plane = luma / 4;
    // SVT copies planar input during send_picture(), so both caller NV12
    // storage and this chroma scratch buffer can be reused immediately.
    for (size_t i = 0; i < plane; ++i) {
        impl_->chroma[i] = nv12[luma + 2 * i];
        impl_->chroma[plane + i] = nv12[luma + 2 * i + 1];
    }
    EbSvtIOFormat image{};
    image.luma = const_cast<uint8_t*>(nv12);
    image.cb = impl_->chroma.data(); image.cr = impl_->chroma.data() + plane;
    image.y_stride = impl_->width; image.cb_stride = image.cr_stride = impl_->width / 2;
    EbBufferHeaderType input{};
    input.size = sizeof(input); input.p_buffer = reinterpret_cast<uint8_t*>(&image);
    input.n_filled_len = static_cast<uint32_t>(luma * 3 / 2);
    input.pts = impl_->submitted; input.pic_type = EB_AV1_INVALID_PICTURE;
    const EbErrorType result = svt_av1_enc_send_picture(impl_->codec, &input);
    if (result != EB_ErrorNone) return fail(result, L"Cannot submit the AV1 frame", error);
    ++impl_->submitted; impl_->failed = false;
    return true;
}
bool Av1Encoder::end(std::wstring& error) {
    error.clear();
    if (!impl_) { error = L"The AV1 encoder is not open."; return false; }
    if (impl_->failed) { error = L"The AV1 encoder stopped after an earlier error."; return false; }
    if (impl_->ending) return true;
    impl_->failed = true;
    EbBufferHeaderType input{};
    input.size = sizeof(input); input.flags = EB_BUFFERFLAG_EOS;
    const EbErrorType result = svt_av1_enc_send_picture(impl_->codec, &input);
    if (result != EB_ErrorNone) return fail(result, L"Cannot finish the AV1 stream", error);
    impl_->ending = true; impl_->failed = false;
    return true;
}
bool Av1Encoder::receive(Av1Packet& packet, bool& eos, std::wstring& error) {
    error.clear(); packet.unit.clear(); packet.pts = 0; packet.keyFrame = false; eos = false;
    if (!impl_) { error = L"The AV1 encoder is not open."; return false; }
    if (impl_->failed) { error = L"The AV1 encoder stopped after an earlier error."; return false; }
    if (impl_->eos) { eos = true; return true; }
    impl_->failed = true;
    OutputBuffer output;
    const EbErrorType result = svt_av1_enc_get_packet(impl_->codec, &output.value, impl_->ending ? 1 : 0);
    if (result == EB_NoErrorEmptyQueue && !impl_->ending) { impl_->failed = false; return true; }
    if (result != EB_ErrorNone) return fail(result, L"Cannot receive the AV1 frame", error);
    if (!output.value) { error = L"The AV1 encoder produced no packet."; return false; }
    eos = (output.value->flags & EB_BUFFERFLAG_EOS) != 0;
    packet.pts = output.value->pts; packet.keyFrame = output.value->pic_type == EB_AV1_KEY_PICTURE;
    bool sequenceFound = false;
    try {
        for (size_t at = 0, length = 0; at < output.value->n_filled_len; at += length) {
            int type = 0;
            const uint8_t* data = output.value->p_buffer + at;
            if (!nextObu(data, output.value->n_filled_len - at, length, type)) {
                error = L"The AV1 encoder produced an unsupported bitstream."; packet.unit.clear(); return false;
            }
            if (type == obuSequenceHeader) {
                sequenceFound = true;
                if (impl_->priming && impl_->received == 0) impl_->sequence.assign(data, data + length);
                else if (length != impl_->sequence.size() || !std::equal(data, data + length, impl_->sequence.begin())) {
                    error = L"The AV1 sequence header changed unexpectedly."; packet.unit.clear(); return false;
                }
            }
            if (type != obuTemporalDelimiter && type != obuPadding) packet.unit.insert(packet.unit.end(), data, data + length);
        }
    } catch (const std::bad_alloc&) {
        error = L"Cannot store the encoded AV1 frame: not enough memory."; packet.unit.clear(); return false;
    }
    if (!packet.unit.empty()) {
        if (packet.pts != impl_->received || (packet.keyFrame && !sequenceFound)) {
            error = L"The AV1 encoder produced an unexpected frame order or keyframe."; packet.unit.clear(); return false;
        }
        ++impl_->received;
    } else if (!eos) { error = L"The AV1 encoder produced an empty video frame."; return false; }
    if (eos && (!impl_->ending || impl_->received != impl_->submitted)) {
        error = L"The AV1 encoder did not finish every submitted frame."; packet.unit.clear(); return false;
    }
    impl_->eos = eos; impl_->failed = false;
    return true;
}
}

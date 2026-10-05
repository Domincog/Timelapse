# AV1 frame preparation optimization

Version 0.41.2 change validated on 2026-10-05. The recorder now converts BGRA directly
to the planar I420 input required by SVT-AV1. The SSE2 conversion processes four
pixels from each row at a time, with an exact two-pixel tail for custom even
widths. It preserves the existing integer BT.709 coefficients, rounding and
four-pixel chroma average. H.264/HEVC retain their existing NV12 conversion.

This removes the recording path's separate NV12-to-planar pass and its chroma
scratch allocation (1,036,800 bytes per 1920 x 1080 encoder). The NV12 AV1 API
remains available; it allocates scratch only when actually used. Header priming
also uses planar input. SVT copies input before submission returns, so the
recorder can reuse the conversion buffer while lookahead retains frames.

Presets, CRF, bitrate, threading, lookahead, keyframes, temporal filtering and
all other AV1 compression settings are unchanged.

## Measurements

The unchanged baseline benchmark executable was frozen before editing source.
Baseline and optimized encoders used the same bundled SVT-AV1 4.2.0 Release
library. Each clip contains 90 synthetic 1920 x 1080 frames at 30 playback FPS,
with Balanced quality, preset 6 and automatic CRF 32. Three runs per variant
per scene alternate execution order. The source corpus is generated before
measurement. CPU totals include encoder/header setup, submission, flush and
MP4 finalization; they exclude capture, preview, UI and corpus generation.

| Synthetic scene | Baseline mean CPU | Optimized mean CPU | Reduction | File bytes, both variants |
| --- | ---: | ---: | ---: | ---: |
| Static screen | 4,385.42 ms | 4,307.29 ms | 1.8% | 101,317 |
| Camera motion | 11,828.13 ms | 11,281.25 ms | 4.6% | 1,581,396 |

In the isolated conversion benchmark, three alternating 1,000-frame rounds per
variant averaged 2.729 ms CPU/frame for NV12 plus planar rearrangement and
1.807 ms for direct I420: 33.8% less frame preparation CPU.

These are local measurements, not a promised percentage for other content or
computers. Individual full-encode timings varied, including one optimized
screen run that used more CPU than its paired baseline. Compression remains
the main cost, so overall savings are substantially smaller than conversion
savings.

## Validation

- All 16,777,216 RGB colors in uniform 2 x 2 blocks, random mixed blocks, alpha
  independence, tiny/even/tail dimensions through 4096 x 2160, unaligned output
  and output guards match the existing scalar conversion byte-for-byte.
- NV12 and I420 API comparisons produce identical AV1 packet bytes, timestamps
  and keyframe flags across short/nonaligned inputs, asynchronous lookahead,
  multiple keyframes and target-bitrate encoding. Input buffers are overwritten
  immediately after submission to verify copy ownership.
- All six full-encode baseline/optimized pairs have identical file lengths,
  packet SHA-256 hashes, timing, flags, codec configuration and color metadata.
- All 90 decoded frames in each of the two first-run scene pairs have identical
  hashes, independently decoded with FFmpeg. Whole MP4 hashes differ because
  recording creation timestamps differ.
- Release build succeeded; `encoder_conversion_tests`, `av1_options_tests`,
  `encoder_modes_tests` and `encoder_geometry_tests` passed (4/4, 21.17 seconds).

The local validation workspace retains the frozen baseline executable, raw
measurements, comparison/verification scripts, packet reports and decoded
hashes. These generated artifacts are not included in the public source ZIP.
The public comparison results above describe the tested encoding behavior;
the release also validates the packaged source and executable before publishing.

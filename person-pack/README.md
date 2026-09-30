# Optional person worker

This independently built Windows x64 worker embeds the pinned NanoDet-m320
model and a reduced ncnn CPU runtime. It is outside the ordinary recorder build.
The recorder verifies the versioned executable's exact size and SHA-256 before
launching it. A separate child process contains optional inference failures.

The source camera frame is resized to full-scene BGR with the longest edge 320,
using the pinned ncnn fixed-point bilinear transform. Its unpadded geometry and
original camera dimensions are both transmitted. The worker adds centered zero
padding to 320 by 320 and applies the original BGR mean/normalization. Output
boxes are projected and clipped using original source dimensions.

All 168,000 class scores and 67,200 distance values must be finite and all heads
must have their exact shapes. A valid projected person box scoring at least 0.25
means Present. Only a maximum person score at most 0.10 across **all** raw anchors
can qualify absence. Intermediate results are Unknown. Qualifying absence also
requires mean luma 12 through 243 and P95 minus P05 at least 16 on the unpadded
input. This simple gate rejects blank/low-detail frames; it does not detect every
occluded lens. Present results are preserved by that gate.

The model can still miss people, especially small, obscured, unusual or dark
subjects. Its output is a sampling hint. It is not an occupancy guarantee.

## Build

Install MSVC x64 C++ build tools, Windows SDK, CMake and PowerShell. Download the
two official archives below, then run `build.ps1` with their local paths and an
output directory. The script verifies hashes and creates a fresh private build
directory; it does not download dependencies or change the main app build.

```powershell
./build.ps1 -NcnnArchive C:/downloads/ncnn-20260526-full-source.zip `
  -ModelArchive C:/downloads/ncnn-nanodet-m.zip -OutputDirectory C:/build/person
```

- [ncnn full source, 20260526](https://github.com/Tencent/ncnn/releases/download/20260526/ncnn-20260526-full-source.zip):
  SHA-256 `754659d6fe65545cf2ef4483ffb84526fea631f8764c44b150f1601d0fb4004b`.
  Commit `e54f7b1f88434e1d844ea0551b880a1cfb079ce1`.
- [NanoDet-m model, v0.4.0](https://github.com/RangiLyu/nanodet/releases/download/v0.4.0/ncnn-nanodet-m.zip):
  SHA-256 `2181ff5091e70b5eb39b5b245f1694c92bca536523c6ee11be81ccda61691392`.
  Commit `c63d7cb0b9bfb2e742d9cc8d3fb26eb00df3221e`.

CMake also checks each embedded model file's exact hash. Stored FP16 weights use
float arithmetic; no Vulkan, OpenCV, INT8, BF16, OpenMP or inference threads are
included. The x64 SSE2 baseline retains runtime selection of supported AVX/FMA/
F16C/AVX2 kernels. Old non-AVX hardware remains a separate compatibility test.

## Licenses

`TimelapsePersonWorker.exe --licenses` (or the downloaded revision's filename
with `--licenses`) emits the embedded attribution and full licenses. The complete
[attribution](NOTICE.txt), [NanoDet license](NanoDet-LICENSE.txt) and
[ncnn licenses](ncnn-LICENSE.txt) are retained inside the distributed executable;
preserve them when redistributing, and retain source notices in source distributions. The
Apache-2.0 NanoDet project license and ncnn BSD/third-party notices are included
verbatim. The official model archive contains no separate license file.

`--person-host Local\Timelapse.Person.<random GUID>` is an internal protocol.
The fixed mapping layout is `src/person_protocol.h`. The worker opens `.mutex`,
`.stop`, and `.request` named objects supplied by the parent. Runtime work runs
outside the mutex, one request at a time; lock retries retain the request/result
without repeating inference. The parent owns freshness, absolute watchdogs,
nonblocking cancellation, process/job containment and immutable pack admission.
No caller-selected model path, DLL path, network access or physical capture is
accepted by this worker.

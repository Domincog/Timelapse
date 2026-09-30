// Diagnostic-only ABI, not present in the distributed worker.
#include "../model.h"
#include <mat.h>
#include <algorithm>
#include <memory>
using namespace lapse::person;
extern "C" __declspec(dllexport) int NanoSelfTest() { return 0; }
extern "C" __declspec(dllexport) int NanoCreate(const char*, const char*, void** out) {
    if (!out) return 1; *out = nullptr;
    try { auto detector = std::make_unique<Detector>(); if (!detector->initialize()) return 2; *out = detector.release(); return 0; }
    catch (...) { return 3; }
}
extern "C" __declspec(dllexport) void NanoDestroy(void* handle) { delete static_cast<Detector*>(handle); }
extern "C" __declspec(dllexport) int NanoDetect(void* handle, const uint8_t* bgr, int width, int height,
    Box* boxes, int capacity, Report* report) {
    if (!handle || !bgr || !boxes || !report || capacity < 2100 || width < 2 || height < 2 || width > 8192 || height > 8192) return 1;
    Source source{}; source.sourceWidth = uint32_t(width); source.sourceHeight = uint32_t(height);
    source.width = width >= height ? 320 : std::max(1, int(int64_t(320) * width / height));
    source.height = height >= width ? 320 : std::max(1, int(int64_t(320) * height / width));
    Input resized{};
    ncnn::resize_bilinear_c3(bgr, width, height, resized.data(), int(source.width), int(source.height));
    Output output;
    return static_cast<Detector*>(handle)->infer(resized.data(), size_t(source.width) * source.height * 3,
        source, output, report, boxes, size_t(capacity)) ? 0 : 2;
}
extern "C" __declspec(dllexport) int NanoDetectThumb(void* handle, const uint8_t* bgr, size_t bytes,
    const Source* source, Output* output, Box* boxes, int capacity, Report* report) {
    if (!handle || !source || !output || !boxes || capacity < 2100 || !report) return 1;
    return static_cast<Detector*>(handle)->infer(bgr, bytes, *source, *output, report, boxes, size_t(capacity)) ? 0 : 2;
}

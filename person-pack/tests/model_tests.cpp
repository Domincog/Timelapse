#include "../model.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace lapse::person;
namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void policy() {
    Report report{}; report.finiteClassValues = 168000; report.finiteDistanceValues = 67200;
    report.maxPersonHeadScore = .10f;
    require(classify(report).verdict == Verdict::QualifiedAbsent, "exact absence boundary");
    report.maxPersonHeadScore = .10001f;
    require(classify(report).verdict == Verdict::Unknown, "ambiguous raw anchor blocks absence");
    report.maxPersonHeadScore = report.maxValidPersonScore = .25f;
    require(classify(report).verdict == Verdict::Present, "exact presence boundary");
    report.maxPersonHeadScore = std::numeric_limits<float>::quiet_NaN();
    require(classify(report).verdict == Verdict::Unknown, "nonfinite cannot qualify");
    report.maxPersonHeadScore = .4f; --report.finiteClassValues;
    require(classify(report).verdict == Verdict::Unknown, "all anchor health required");
    Input input{};
    for (int value : {0, 12, 128, 243, 255}) {
        input.fill(uint8_t(value)); require(!sufficientDetail(input.data(), input.size()), "uniform detail rejected");
    }
    for (size_t i = 0; i < input.size(); ++i) input[i] = (i / 3) % 2 ? 112 : 128;
    require(sufficientDetail(input.data(), input.size()), "spread 16 accepted");
    for (size_t i = 0; i < input.size(); ++i) input[i] = (i / 3) % 2 ? 113 : 128;
    require(!sufficientDetail(input.data(), input.size()), "spread 15 rejected");
    require(!sufficientDetail(input.data(), input.size() - 1), "non-pixel byte count rejected");
    for (size_t i = 0; i < input.size(); ++i) input[i] = (i / 3) % 2 ? 4 : 20;
    require(sufficientDetail(input.data(), input.size()), "mean 12 accepted");
    for (size_t i = 0; i < input.size(); ++i) input[i] = (i / 3) % 2 ? 3 : 19;
    require(!sufficientDetail(input.data(), input.size()), "mean below 12 rejected");
}
void geometry() {
    Source source{}; source.sourceWidth = 1920; source.sourceHeight = 1080; source.width = 320; source.height = 180;
    require(validGeometry(source), "valid aspect geometry");
    Box box{10, 249.3f, 20, 249.6f, .4f, 0};
    require(projectBox(box, source) && box.y1 > box.y0 && box.y1 < 1080, "narrow edge box retained in original geometry");
    Source thumbnail = source; thumbnail.sourceWidth = 320; thumbnail.sourceHeight = 180;
    Box old{10, 249.3f, 20, 249.6f, .4f, 0};
    require(!projectBox(old, thumbnail), "fixture exposes thumbnail-dimension clipping regression");
    ++source.height; require(!validGeometry(source), "incorrect thumbnail shape rejected");
    Shared shared{}; shared.structBytes = sizeof(shared); require(validHeader(shared), "fixed wire header");
    ++shared.preprocessingRevision; require(!validHeader(shared), "wrong preprocessing revision rejected");
}
void embedded() {
    Detector detector; require(detector.initialize(), "embedded pinned model loads");
    Source source{}; source.sourceWidth = source.sourceHeight = source.width = source.height = 320;
    Input input{}; Output output; Report report;
    require(detector.infer(input.data(), input.size(), source, output, &report), "blank inference completes");
    require(output.verdict == Verdict::Unknown && output.reason == Reason::InsufficientDetail, "black frame cannot qualify absence");
    require(report.finiteClassValues == 168000 && report.finiteDistanceValues == 67200, "all embedded heads validated");
    require(!detector.infer(input.data(), input.size() - 1, source, output), "short input rejected");
    require(output.verdict == Verdict::Unknown && output.reason == Reason::InvalidInput, "short input remains unknown");
}
}
int main() {
    try { policy(); geometry(); embedded(); std::puts("PASS: conservative thresholds, detail boundaries, original-source edge geometry, protocol, embedded model and bounded inputs"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}

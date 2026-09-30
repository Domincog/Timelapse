#pragma once
#include "../src/person_protocol.h"
#include <memory>

namespace lapse::person {
struct Box { float x0, y0, x1, y1, score; int label; };
struct Report {
    float maxPersonHeadScore = 0, maxValidPersonScore = 0;
    int resizedWidth = 0, resizedHeight = 0, padLeft = 0, padTop = 0;
    int finiteClassValues = 0, finiteDistanceValues = 0, candidates = 0;
    int invalidClipped = 0, kept = 0, personKept = 0;
};
// Owns only the pinned embedded model. No external model or DLL path is accepted.
class Detector {
public:
    Detector();
    ~Detector();
    bool initialize() noexcept;
    bool infer(const uint8_t* bgr, size_t bytes, const Source&, Output&,
               Report* diagnostic = nullptr, Box* boxes = nullptr, size_t capacity = 0) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Output classify(const Report&) noexcept;
bool sufficientDetail(const uint8_t* bgr, size_t bytes) noexcept;
bool projectBox(Box&, const Source&) noexcept;
}

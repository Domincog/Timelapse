#pragma once
#include "core.h"

namespace lapse {
// Use from one thread with COM and Media Foundation initialized. Call finish()
// before MFShutdown to finalize the MP4 and receive any final write errors.
class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Creates a new file; existing files are never overwritten.
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error);
    bool write(const Frame& frame, std::wstring& error);
    bool finish(std::wstring& error);
    uint64_t frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

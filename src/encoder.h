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

    // Creates a new file; existing files are never overwritten. Requires delete
    // access for owned-file cleanup and denies rename/deletion until ordinary
    // finish(), or until the explicit publication sequence below releases it.
    bool open(const std::wstring& path, int width, int height, int fps, std::wstring& error,
              EncodingQuality quality = EncodingQuality::Balanced);
    bool write(const Frame& frame, std::wstring& error);
    bool finish(std::wstring& error);
    // Worker publication keeps the original object protected through status
    // commit. publish() attempts one same-volume, non-replacing handle rename.
    // Preparation failures can retry; a native attempt's Win32 result is cached.
    // Release only after reporting its path; a retained guard prevents open().
    // Plain finish() and destruction also release any retained guard.
    bool finishForPublication(std::wstring& error);
    DWORD publish(const std::wstring& destination);
    void releasePublication() noexcept;
    uint64_t frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

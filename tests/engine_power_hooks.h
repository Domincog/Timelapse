#pragma once
#include "encoder.h"
#include <windows.h>
namespace probe {
void beforeFinish();
bool afterFinish(bool, std::wstring&);
HRESULT WINAPI initialize(LPVOID, DWORD);
EXECUTION_STATE WINAPI executionState(EXECUTION_STATE);
BOOL WINAPI forbiddenProcess(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
}
namespace lapse {
class PowerEncoder {
    Encoder real_;
public:
    bool open(const std::wstring& path, int width, int height, int fps,
              std::wstring& error, EncodingQuality quality, EncodingMode mode, bool recoveryMode) { return real_.open(path,width,height,fps,error,quality,mode, recoveryMode); }
    bool write(const Frame& frame,std::wstring& error) { return real_.write(frame,error); }
    bool finish(std::wstring& error) { probe::beforeFinish(); return probe::afterFinish(real_.finish(error),error); }
    bool finishForPublication(std::wstring& error) { probe::beforeFinish(); return probe::afterFinish(real_.finishForPublication(error),error); }
    DWORD publish(const std::wstring& path) { return real_.publish(path); }
    void releasePublication() noexcept { real_.releasePublication(); }
    bool emptyOutputDiscarded() const noexcept { return real_.emptyOutputDiscarded(); }
    uint64_t frames() const { return real_.frames(); }
};
}

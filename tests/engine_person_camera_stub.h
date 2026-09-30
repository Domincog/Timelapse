#pragma once
#include "camera_host.h"
// Other engine fixtures exercise pre-person modes. Their existing synthetic
// camera must never obtain a real optional thumbnail or open hardware.
namespace lapse {
bool CameraClient::personInput(uint64_t, CameraPersonInput&, std::wstring& error, bool) {
    error = L"Person thumbnails are unavailable in this synthetic source.";
    return false;
}
}

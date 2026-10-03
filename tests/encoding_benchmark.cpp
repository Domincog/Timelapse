#include "encoder.h"
#include "encoding_corpus.h"
#include <mfapi.h>
#include <psapi.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <cwchar>

// Optional, owned-corpus benchmark. It records encoding resources only; pair
// each MP4 with encoding_quality_verifier or verify-encoding-quality.ps1.
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void encoded(bool value, const std::wstring& error) {
    if (!value) { std::wcerr << error << L'\n'; throw std::runtime_error("Encoding failed"); }
}
int integer(const wchar_t* value) {
    wchar_t* end = nullptr; const long parsed = std::wcstol(value, &end, 10);
    require(value != end && *end == L'\0' && parsed >= 0 && parsed <= 100000, "Invalid numeric argument");
    return static_cast<int>(parsed);
}
uint64_t processCpu() {
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "GetProcessTimes failed");
    return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
        ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}
uint64_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        sizeof(counters)) != FALSE, "GetProcessMemoryInfo failed");
    return counters.PrivateUsage;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 8) {
        std::cerr << "Usage: encoding_benchmark <compatible|efficient|quality-h264|hardware-h264|hardware-hevc|av1> "
            "<compact|balanced|detail> <scene:0-3> <width> <height> <frames> <unused-output.mp4>\n"
            "Example: encoding_benchmark efficient balanced 0 1920 1080 180 screen.mp4\n"
            "Pre-renders all source frames (180 x 1080p uses about 1.4 GiB) outside encoding measurements.\n"
            "Run encoding_quality_verifier or verify-encoding-quality.ps1 on every output for fidelity/timing QA.\n";
        return 1;
    }
    bool com = false, mf = false; int result = 0;
    try {
        const std::wstring modeName = argv[1], qualityName = argv[2];
        lapse::EncodingMode mode;
        if (modeName == L"compatible") mode = lapse::EncodingMode::Compatible;
        else if (modeName == L"efficient") mode = lapse::EncodingMode::Efficient;
        else if (modeName == L"quality-h264") mode = lapse::EncodingMode::QualityH264;
        else if (modeName == L"hardware-h264") mode = lapse::EncodingMode::HardwareH264;
        else if (modeName == L"hardware-hevc") mode = lapse::EncodingMode::HardwareHEVC;
        else if (modeName == L"av1") mode = lapse::EncodingMode::SoftwareAV1;
        else throw std::runtime_error("Invalid encoding mode");
        lapse::EncodingQuality quality;
        if (qualityName == L"compact") quality = lapse::EncodingQuality::Compact;
        else if (qualityName == L"balanced") quality = lapse::EncodingQuality::Balanced;
        else if (qualityName == L"detail") quality = lapse::EncodingQuality::Detail;
        else throw std::runtime_error("Invalid encoding quality");
        const int scene = integer(argv[3]), width = integer(argv[4]), height = integer(argv[5]), count = integer(argv[6]);
        require(scene < corpus::sceneCount && width >= 640 && height >= 360 && width <= 4096 && height <= 4096 &&
            !(width & 1) && !(height & 1) && count > 0 && count <= 1000, "Invalid scene, dimensions or frame count");
        require(uint64_t(width) * height * 4 * count <= uint64_t(2) * 1024 * 1024 * 1024,
            "Pre-rendered corpus exceeds 2 GiB; reduce frame count or dimensions");
        require(!std::filesystem::exists(std::filesystem::path(argv[7])), "Choose an unused output path");
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM startup failed"); com = true;
        require(SUCCEEDED(MFStartup(MF_VERSION)), "Media Foundation startup failed"); mf = true;
        corpus::Generator generator(scene, width, height);
        std::vector<lapse::Frame> frames; frames.reserve(count);
        for (int i = 0; i < count; ++i) frames.push_back(generator.frame(i));
        lapse::Encoder encoder; std::wstring error;
        const auto initialPrivate = privateBytes(); auto peakPrivate = initialPrivate;
        const auto cpuStart = processCpu(); const auto start = std::chrono::steady_clock::now();
        encoded(encoder.open(argv[7], width, height, corpus::fps, error, quality, mode), error);
        const auto cpuOpened = processCpu(); peakPrivate = std::max(peakPrivate, privateBytes());
        for (const auto& frame : frames) {
            encoded(encoder.write(frame, error), error); peakPrivate = std::max(peakPrivate, privateBytes());
        }
        const auto cpuSubmitted = processCpu(); encoded(encoder.finish(error), error);
        const auto end = std::chrono::steady_clock::now(); const auto cpuFinished = processCpu();
        peakPrivate = std::max(peakPrivate, privateBytes());
        require(encoder.frames() == uint64_t(count), "Submitted frame count changed");
        const auto bytes = std::filesystem::file_size(std::filesystem::path(argv[7]));
        std::wcout << L"mode,quality,scene,width,height,frames,file_bytes,encode_cpu_ms,encode_wall_ms,"
            L"sampled_encoder_private_mib,setup_cpu_ms,submit_cpu_ms,finalize_cpu_ms\n"
            << std::fixed << std::setprecision(6) << modeName << L',' << qualityName << L',' << scene << L',' << width << L','
            << height << L',' << count << L',' << bytes << L',' << double(cpuFinished - cpuStart) / 10000.0 << L','
            << std::chrono::duration<double, std::milli>(end - start).count() << L','
            << double(peakPrivate - initialPrivate) / 1048576.0 << L',' << double(cpuOpened - cpuStart) / 10000.0 << L','
            << double(cpuSubmitted - cpuOpened) / 10000.0 << L',' << double(cpuFinished - cpuSubmitted) / 10000.0 << L'\n';
        std::wcerr << L"Encoded " << argv[7] << L"; decoded quality/timestamps require the separate verifier.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (mf) MFShutdown(); if (com) CoUninitialize(); return result;
}

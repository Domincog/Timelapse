#pragma once
#include "core.h"
#include <mutex>
#include <thread>
#include <condition_variable>

namespace lapse {
enum class State { Idle, Starting, Recording, Paused, Finishing };
struct Settings {
    RECT monitor{};
    std::wstring cameraId;
    std::vector<Layer> layers = preset(Mode::Desktop);
    int interval = 5;
    int width = 1280, height = 720;
    std::wstring folder;
    bool preview = true;
};
struct Status {
    State state = State::Idle;
    uint64_t frames = 0;
    double elapsed = 0;
    std::wstring message = L"Choose a source, then record.";
    std::wstring savedPath;
    bool error = false;
    std::shared_ptr<const Frame> preview;
};
class Engine {
public:
    Engine();
    ~Engine();
    void configure(const Settings& settings);
    void refreshSources();
    void record();
    void pause();
    void finish();
    Status status();
private:
    void run();
    std::mutex mutex_;
    std::condition_variable wake_;
    Settings settings_;
    Status status_;
    bool quit_ = false, start_ = false, stop_ = false, togglePause_ = false, retrySources_ = false;
    std::thread worker_;
};
}

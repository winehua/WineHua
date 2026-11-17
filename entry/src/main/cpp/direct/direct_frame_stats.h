#pragma once
#include <cstdint>

namespace winehua::direct {
// A queued image can be redrawn for UI changes. Count it only once, after a
// successful output present, and retain it across skipped/failed attempts.
struct DirectFrameAcceptance {
    bool pending = false;
    void Acquired() { pending = true; }
    bool Presented(bool success, bool visible) {
        if (!pending || !success || !visible) return false;
        pending = false;
        return true;
    }
};

class DirectFrameRate {
public:
    void Record() { ++frames_; }
    bool Sample(uint64_t nowUs, double& fps) {
        if (!started_) { started_ = true; startedUs_ = nowUs; return false; }
        if (nowUs < startedUs_) { startedUs_ = nowUs; frames_ = 0; return false; }
        const uint64_t elapsed = nowUs - startedUs_;
        if (elapsed < 1000000) return false;
        fps = frames_ * 1000000.0 / elapsed;
        frames_ = 0;
        startedUs_ = nowUs;
        return true;
    }
private:
    bool started_ = false;
    uint64_t startedUs_ = 0, frames_ = 0;
};
}

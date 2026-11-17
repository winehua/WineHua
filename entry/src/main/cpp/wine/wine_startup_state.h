#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace winehua {
struct WineStartupSnapshot {
    uint64_t generation = 0;
    bool active = false;
    std::string state = "idle";
    std::string phase;
};

// Readiness is retained state, not a one-shot callback. A cancelled worker
// keeps ownership until it has unwound; a retry cannot race its late spawns.
class WineStartupState {
public:
    using Clock = std::chrono::steady_clock;
    uint64_t Begin(Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (value_.active) return 0;
        ++value_.generation;
        value_.active = true;
        value_.state = "starting";
        value_.phase = "graphics";
        deadline_ = now + std::chrono::seconds(150);
        return value_.generation;
    }
    bool Current(uint64_t generation, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation && value_.generation == generation && value_.active &&
               value_.state != "cancelled" && now < deadline_;
    }
    bool Publish(uint64_t generation, const std::string& state, const std::string& phase) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (value_.generation != generation || !value_.active || value_.state == "cancelled") return false;
        value_.state = state;
        value_.phase = phase;
        return true;
    }
    bool Cancel(uint64_t expectedGeneration = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expectedGeneration && value_.generation != expectedGeneration) return false;
        value_.state = "cancelled";
        return true;
    }
    void Finish(uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (value_.generation != generation) return;
        value_.active = false;
        if (value_.state == "starting") value_.state = "failed";
    }
    WineStartupSnapshot Snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }
private:
    std::mutex mutex_;
    WineStartupSnapshot value_;
    Clock::time_point deadline_;
};
} // namespace winehua

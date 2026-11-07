#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace winehua {

struct LivePerformanceSample {
    double clientFps = 0, displayFps = 0;
    std::string gpu = "initializing";
};

class LivePerformance {
public:
    static LivePerformance& Instance() { static LivePerformance stats; return stats; }
    static uint64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void Present() { presents_.fetch_add(1, std::memory_order_relaxed); }
    void SetGpu(const char* name) {
        if (!name) return;
        std::lock_guard<std::mutex> lock(mutex_); gpu_ = name;
    }
    void Commit(uint64_t source) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = sources_[source]; ++entry.frames; entry.lastMs = NowMs();
    }
    LivePerformanceSample Sample() {
        const uint64_t now = NowMs();
        std::lock_guard<std::mutex> lock(mutex_);
        if (now - sampledMs_ < 500) return cached_;
        const uint64_t elapsed = now - sampledMs_;
        const uint64_t presented = presents_.load(std::memory_order_relaxed);
        cached_.displayFps = sampledMs_ ? (presented - sampledPresents_) * 1000.0 / elapsed : 0;
        cached_.clientFps = 0;
        for (auto it = sources_.begin(); it != sources_.end();) {
            auto& entry = it->second;
            if (sampledMs_) cached_.clientFps = std::max(cached_.clientFps,
                (entry.frames - entry.sampledFrames) * 1000.0 / elapsed);
            entry.sampledFrames = entry.frames;
            if (now - entry.lastMs > 10000) it = sources_.erase(it); else ++it;
        }
        cached_.gpu = gpu_;
        sampledMs_ = now; sampledPresents_ = presented;
        return cached_;
    }
private:
    struct Source { uint64_t frames = 0, sampledFrames = 0, lastMs = 0; };
    std::mutex mutex_;
    std::unordered_map<uint64_t, Source> sources_;
    std::atomic<uint64_t> presents_{0};
    uint64_t sampledMs_ = 0, sampledPresents_ = 0;
    std::string gpu_ = "initializing";
    LivePerformanceSample cached_;
};

} // namespace winehua

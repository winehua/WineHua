#pragma once
#include <cstdint>

namespace winehua {

// A NativeImage consumer can briefly retain all producer buffers when hidden
// or resized. Never wait forever while holding the presenter mutex, and never
// rebuild an allocation-failed EGL window on every game frame.
constexpr int32_t kSurfaceBufferTimeoutMs = 16;
constexpr uint64_t kSurfaceRecoveryDelayNs = 250000000;

class SurfaceQueueRecovery {
public:
    void Request(uint64_t nowNs) {
        pending_ = true;
        retryAfterNs_ = nowNs + kSurfaceRecoveryDelayNs;
    }
    bool Pending() const { return pending_; }
    bool Deferred(uint64_t nowNs) const { return pending_ && nowNs < retryAfterNs_; }
    uint64_t RetryAfterNs() const { return retryAfterNs_; }
    void Reset() { pending_ = false; retryAfterNs_ = 0; }
private:
    bool pending_ = false;
    uint64_t retryAfterNs_ = 0;
};

} // namespace winehua

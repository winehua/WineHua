#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace winehua {

// Runtime control belongs to the host, independently of Wine's child env.
inline std::atomic<uint64_t> gFrameLoopDiagnosticState{0};
inline void SetFrameLoopDiagnostics(bool enabled) {
    uint64_t state = gFrameLoopDiagnosticState.load(std::memory_order_relaxed);
    while ((state & 1) != static_cast<uint64_t>(enabled) &&
           !gFrameLoopDiagnosticState.compare_exchange_weak(state,
               ((state + 2) & ~uint64_t{1}) | static_cast<uint64_t>(enabled),
               std::memory_order_relaxed)) {}
}
inline uint64_t FrameLoopDiagnosticState() {
    return gFrameLoopDiagnosticState.load(std::memory_order_relaxed);
}

// Fixed storage; quantiles are upper bounds, rounded up to 250 us. Overflow
// uses the observed maximum as its upper bound, rather than hiding long stalls.
struct LoopTimingHistogram {
    static constexpr uint64_t kBucketUs = 250;
    static constexpr size_t kBuckets = 1025;
    std::array<uint64_t, kBuckets> bins{};
    uint64_t count = 0, sumUs = 0, maxUs = 0;
    void Add(uint64_t us) {
        ++count;
        sumUs += us;
        maxUs = std::max(maxUs, us);
        const uint64_t bucket = us / kBucketUs + (us % kBucketUs != 0);
        ++bins[std::min<uint64_t>(bucket, kBuckets - 1)];
    }
    uint64_t UpperPercentile(unsigned percentile) const {
        if (!count) return 0;
        const uint64_t rank = std::max<uint64_t>(1,
            (count * std::min(percentile, 100u) + 99) / 100);
        uint64_t seen = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            seen += bins[i];
            if (seen >= rank)
                return i == kBuckets - 1 ? maxUs : std::min<uint64_t>(i * kBucketUs, maxUs);
        }
        return maxUs;
    }
};

inline thread_local LoopTimingHistogram* gTakeLockWaitSink = nullptr;
class ScopedTakeLockSink {
    LoopTimingHistogram* previous_;
public:
    explicit ScopedTakeLockSink(LoopTimingHistogram* sink) : previous_(gTakeLockWaitSink) {
        gTakeLockWaitSink = sink;
    }
    ~ScopedTakeLockSink() { gTakeLockWaitSink = previous_; }
    ScopedTakeLockSink(const ScopedTakeLockSink&) = delete;
    ScopedTakeLockSink& operator=(const ScopedTakeLockSink&) = delete;
};
class TakeLockTimer {
    LoopTimingHistogram* sink_ = gTakeLockWaitSink;
    std::chrono::steady_clock::time_point start_;
public:
    TakeLockTimer() {
        if (sink_) start_ = std::chrono::steady_clock::now();
    }
    void Acquired() {
        if (sink_) sink_->Add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_).count()));
    }
};

struct ProducerDiagnosticSnapshot {
    uint64_t shmTop = 0, shmSub = 0, other = 0, unmap = 0;
    uint64_t bytes = 0, commitUs = 0, callbacks = 0;
};
struct ProducerDiagnosticCounters {
    std::atomic<uint64_t> shmTop{0}, shmSub{0}, other{0}, unmap{0};
    std::atomic<uint64_t> bytes{0}, commitUs{0}, callbacks{0};
    ProducerDiagnosticSnapshot Snapshot() const {
        return {shmTop.load(std::memory_order_relaxed), shmSub.load(std::memory_order_relaxed),
            other.load(std::memory_order_relaxed), unmap.load(std::memory_order_relaxed),
            bytes.load(std::memory_order_relaxed), commitUs.load(std::memory_order_relaxed),
            callbacks.load(std::memory_order_relaxed)};
    }
};
inline ProducerDiagnosticCounters gProducerDiagnostics;
inline void NoteProducerCommit(bool shm, bool sub, bool unmap, uint64_t bytes,
                               uint64_t durationUs, uint64_t callbacks) {
    auto& counter = unmap ? gProducerDiagnostics.unmap : !shm ? gProducerDiagnostics.other :
        sub ? gProducerDiagnostics.shmSub : gProducerDiagnostics.shmTop;
    counter.fetch_add(1, std::memory_order_relaxed);
    gProducerDiagnostics.bytes.fetch_add(bytes, std::memory_order_relaxed);
    gProducerDiagnostics.commitUs.fetch_add(durationUs, std::memory_order_relaxed);
    gProducerDiagnostics.callbacks.fetch_add(callbacks, std::memory_order_relaxed);
}

struct FrameLoopDiagnosticWindow {
    static constexpr uint64_t kWindowUs = 2000000;
    uint64_t state = 0, startedUs = 0, lastPresentUs = 0;
    uint64_t loops = 0, noFrame = 0, skipped = 0, presents = 0, failed = 0;
    uint64_t cpuFrames = 0, zcFrames = 0, directFrames = 0, geometryFrames = 0;
    uint64_t vsyncRequests = 0, vsyncTimeouts = 0, vsyncErrors = 0, fallbacks = 0;
    LoopTimingHistogram take, work, wait, lockWait, presentGap;
    ProducerDiagnosticSnapshot producerStart;

    bool Active() const { return (state & 1) != 0; }
    void ResetWindow(uint64_t nowUs, const ProducerDiagnosticSnapshot& source) {
        startedUs = nowUs;
        loops = noFrame = skipped = presents = failed = 0;
        cpuFrames = zcFrames = directFrames = geometryFrames = 0;
        vsyncRequests = vsyncTimeouts = vsyncErrors = fallbacks = 0;
        take = {}; work = {}; wait = {}; lockWait = {}; presentGap = {};
        producerStart = source;
    }
    bool Sync(uint64_t controlState, uint64_t nowUs) {
        if (controlState != state) {
            state = controlState;
            lastPresentUs = 0;
            ResetWindow(nowUs, gProducerDiagnostics.Snapshot());
        }
        return Active();
    }
    void NoteTake(uint64_t us, bool haveFrame, bool cpu, bool zc, bool direct, bool geometry) {
        if (!Active()) return;
        ++loops;
        if (!haveFrame) ++noFrame;
        cpuFrames += cpu; zcFrames += zc; directFrames += direct; geometryFrames += geometry;
        take.Add(us);
    }
    void NoteWork(uint64_t us) { if (Active()) work.Add(us); }
    void NoteWait(uint64_t us) { if (Active()) wait.Add(us); }
    void NotePresent(bool success, uint64_t nowUs) {
        if (!Active()) return;
        if (!success) { ++failed; return; }
        ++presents;
        if (lastPresentUs && nowUs >= lastPresentUs) presentGap.Add(nowUs - lastPresentUs);
        lastPresentUs = nowUs;
    }
    bool Due(uint64_t nowUs) const {
        return Active() && nowUs >= startedUs && nowUs - startedUs >= kWindowUs;
    }
    void MaybePublish(uint32_t rendererId, uint32_t rootId, uint64_t nowUs);
};

} // namespace winehua

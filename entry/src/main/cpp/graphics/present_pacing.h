#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace winehua {

inline constexpr uint64_t kDirectPresentWarmupFrames = 24;
inline constexpr uint64_t kDirectFirstAcquireTimeoutNs = 100000000;
inline constexpr uint64_t kDirectQueueAcquireTimeoutNs = 0;
inline constexpr uint64_t kDefaultPresentFramePeriodNs = 16666667;
inline constexpr uint64_t kMinPresentFramePeriodNs = 4000000;
inline constexpr uint64_t kMaxPresentFramePeriodNs = 33333333;
inline constexpr uint64_t kPresentDispatchLeadNs = 500000;

inline uint64_t NormalizePresentFramePeriodNs(uint64_t framePeriodNs)
{
    if (!framePeriodNs) return kDefaultPresentFramePeriodNs;
    return std::clamp(framePeriodNs, kMinPresentFramePeriodNs,
                      kMaxPresentFramePeriodNs);
}

inline uint64_t PresentPacingPeriodNs(uint64_t displayPeriodNs)
{
    return displayPeriodNs >
            kMinPresentFramePeriodNs + kPresentDispatchLeadNs
        ? displayPeriodNs - kPresentDispatchLeadNs
        : kMinPresentFramePeriodNs;
}

// A NativeImage queue is drained by the display-paced consumer. Dispatching
// its producer early on every frame eventually overtakes that consumer and
// leaves callbacks for buffers already coalesced by UpdateSurfaceImage.
// Match the consumer period; deadline-led direct/Vulkan paths keep the lead.
inline uint64_t QueuePresentPacingPeriodNs(uint64_t displayPeriodNs)
{
    return NormalizePresentFramePeriodNs(displayPeriodNs);
}

struct PresentPacingDecision {
    bool presentNow = true;
    uint64_t nextDeadlineNs = 0;
};

inline uint64_t SaturatingDeadlineNs(uint64_t baseNs, uint64_t periodNs)
{
    if (periodNs > std::numeric_limits<uint64_t>::max() - baseNs)
        return std::numeric_limits<uint64_t>::max();
    return baseNs + periodNs;
}

inline PresentPacingDecision EvaluatePresentPacing(uint64_t nowNs,
                                                   uint64_t lastPresentNs,
                                                   uint64_t periodNs)
{
    if (!lastPresentNs || !periodNs) return {};
    const uint64_t deadlineNs = SaturatingDeadlineNs(lastPresentNs, periodNs);
    if (nowNs >= deadlineNs) return {};
    return {false, deadlineNs};
}

inline uint64_t NextPresentDeadlineNs(uint64_t presentNs, uint64_t periodNs)
{
    return periodNs ? SaturatingDeadlineNs(presentNs, periodNs) : 0;
}

// Queue-full/fence-timeout is a successful deferred present. Always return a
// future deadline so the guest cannot spin uncapped after no pixels were
// published, matching the Direct NativeWindow reference branch contract.
inline uint64_t RetryPresentDeadlineNs(uint64_t nowNs,
                                      uint64_t lastPresentNs,
                                      uint64_t periodNs)
{
    if (!periodNs) return 0;
    const uint64_t lastDeadline = lastPresentNs
        ? SaturatingDeadlineNs(lastPresentNs, periodNs) : 0;
    if (lastDeadline > nowNs) return lastDeadline;
    return SaturatingDeadlineNs(nowNs, periodNs);
}

// EGL cannot reliably distinguish a full, temporarily unconsumed NativeImage
// queue from allocation failure. Keep the failure visible, but do not let it
// erase pacing and turn a hidden surface into an unbounded producer loop.
// 50 ms is also the existing Guest vtest deadline clamp; no protocol change.
class GlPresentFailureBackoff {
public:
    uint64_t Fail(uint64_t nowNs, uint64_t periodNs)
    {
        if (failures_ < 5) ++failures_;
        const uint64_t base = NormalizePresentFramePeriodNs(periodNs);
        const uint64_t delay = std::min<uint64_t>(50000000, base << (failures_ - 1));
        deadlineNs_ = SaturatingDeadlineNs(nowNs, delay);
        return deadlineNs_;
    }
    uint64_t PendingDeadline(uint64_t nowNs) const
    {
        return deadlineNs_ > nowNs ? deadlineNs_ : 0;
    }
    void Reset() { failures_ = 0; deadlineNs_ = 0; }

private:
    uint32_t failures_ = 0;
    uint64_t deadlineNs_ = 0;
};

inline bool DirectPresentUsesGuestDeadline(uint64_t framesPresented)
{
    return framesPresented >= kDirectPresentWarmupFrames;
}

inline uint64_t DirectPresentAcquireTimeoutNs(uint64_t framesPresented)
{
    return DirectPresentUsesGuestDeadline(framesPresented)
        ? kDirectQueueAcquireTimeoutNs : kDirectFirstAcquireTimeoutNs;
}

} // namespace winehua

#pragma once

#include <array>
#include <cstdint>

// Owned by one xdg_toplevel resource, so recreating the role publishes its
// initial limits again. This only filters notifications, not request handling.
class SizeLimitsEventCache {
public:
    bool ShouldPublish(uint32_t toplevelId, int32_t minW, int32_t minH,
                       int32_t maxW, int32_t maxH)
    {
        if (!toplevelId) return false;
        const std::array<int32_t, 4> limits{minW, minH, maxW, maxH};
        if (toplevelId_ == toplevelId && limits_ == limits) return false;
        toplevelId_ = toplevelId;
        limits_ = limits;
        return true;
    }

private:
    uint32_t toplevelId_ = 0;
    std::array<int32_t, 4> limits_{};
};

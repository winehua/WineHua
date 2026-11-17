#pragma once
#include <native_vsync/native_vsync.h>
#include <cstdint>
#include <cstring>

namespace winehua {

// Shared product hint; each consumer still uses the actual granted period.
inline int RequestDesktopRefreshRange(OH_NativeVSync* vsync)
{
    OH_NativeVSync_ExpectedRateRange range{60, 120, 120};
    return OH_NativeVSync_SetExpectedFrameRateRange(vsync, &range);
}

// Vulkan FIFO already paces output acquisition. Observe the display cadence
// without waiting for a second tick, then give that period to GPU producers.
class DisplayCadence {
public:
    DisplayCadence() = default;
    DisplayCadence(const DisplayCadence&) = delete;
    DisplayCadence& operator=(const DisplayCadence&) = delete;
    ~DisplayCadence() { if (vsync_) OH_NativeVSync_Destroy(vsync_); }

    void Initialize()
    {
        if (vsync_) return;
        const char name[] = "WineHuaVulkanDesktop";
        vsync_ = OH_NativeVSync_Create(name, sizeof(name) - 1);
        if (vsync_) RequestDesktopRefreshRange(vsync_);
    }
    bool Refresh(uint64_t nowUs)
    {
        if (!vsync_) return false;
        // GetPeriod is refreshed by RequestFrame callbacks, not by polling
        // alone. The callback has no renderer pointer or Vulkan work, so it
        // cannot race the renderer's teardown or issue queue operations.
        OH_NativeVSync_RequestFrame(vsync_, [](long long, void*) {}, nullptr);
        if (queried_ && nowUs - lastQueryUs_ < 100000) return false;
        queried_ = true;
        lastQueryUs_ = nowUs;
        long long period = 0;
        if (OH_NativeVSync_GetPeriod(vsync_, &period) != 0 ||
            period < 1000000 || period > 1000000000) return false;
        const auto value = static_cast<uint64_t>(period);
        const auto delta = value > periodNs_ ? value - periodNs_ : periodNs_ - value;
        if (delta < 500000) return false;
        periodNs_ = value;
        return true;
    }
    uint64_t PeriodNs() const { return periodNs_; }

private:
    OH_NativeVSync* vsync_ = nullptr;
    uint64_t periodNs_ = 16666667;
    uint64_t lastQueryUs_ = 0;
    bool queried_ = false;
};

} // namespace winehua

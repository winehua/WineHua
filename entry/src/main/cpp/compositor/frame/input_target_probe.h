#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace winehua {
// Actual pointer-event history only. Passive hit-tests never update this value.
struct LastInputTargetSnapshot {
    uint32_t toplevelId = 0;
    uint64_t eventUs = 0;
    double desktopX = 0, desktopY = 0;
};
inline std::atomic<uint32_t> g_lastInputTargetToplevel{0};
inline std::mutex g_lastInputTargetMutex;
inline LastInputTargetSnapshot g_lastInputTargetSnapshot;
inline void NoteInputTargetToplevel(uint32_t toplevelId, double x, double y)
{
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard<std::mutex> lock(g_lastInputTargetMutex);
    g_lastInputTargetSnapshot = {toplevelId, now, x, y};
    g_lastInputTargetToplevel.store(toplevelId, std::memory_order_relaxed);
}
inline LastInputTargetSnapshot LastInputTarget()
{
    std::lock_guard<std::mutex> lock(g_lastInputTargetMutex);
    return g_lastInputTargetSnapshot;
}
inline uint32_t LastInputTargetToplevel()
{
    return g_lastInputTargetToplevel.load(std::memory_order_relaxed);
}
} // namespace winehua

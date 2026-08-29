#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>

// ============================================================================
// presenter_common: 呈现侧 (virgl/venus presenter) 共享工具与帧周期常量
// (重构第 3 步, 行为平价 — 仅把两 presenter 逐字相同的工具函数与重复的
//  帧周期常量收编到这里, 逻辑与返回值不变)
//
// 为何收编: virgl_surface_presenter.cpp 与 venus_surface_presenter.cpp 此前
// 各自在匿名命名空间里重复定义 NowNs / PresentPerfSummaryEnabled 等逐字相同
// 的工具函数, 且两版 NormalizeFramePeriodNs / PacingPeriodNs 同名却策略不同。
// 收编要求: 逐字相同的提取为共享函数; 同名不同策略的拆为显式命名的两个
// 函数, 边界用命名常量表达 (不再靠同名魔法值区分)。
// ============================================================================

namespace winehua {

using SteadyClock = std::chrono::steady_clock;

// -- 帧周期常量 (两 presenter 共享; 边界命名化, 区分钳制策略) --
constexpr uint64_t kDefaultFramePeriodNs = 16666667;  // 60Hz 名义周期 (无配置时)
constexpr uint64_t kMinFramePeriodNs = 4000000;       // virgl pacer floor / clamp min; venus range min
constexpr uint64_t kVirglMaxFramePeriodNs = 33333333;  // virgl clamp max (30Hz)
constexpr uint64_t kVenusMaxFramePeriodNs = 100000000; // venus range max (10Hz)
constexpr uint64_t kDispatchLeadNs = 500000;           // 生产侧提前量 (两版同值)
constexpr uint64_t kReleaseFenceWatchdogNs = 1000000000;  // venus 释放栅栏监护上限

// -- 时钟 (本命名空间内唯一实现) --
inline uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

inline uint64_t NowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

// -- WINEHUA_VTEST_PRESENT_PERF_SUMMARY 三位开关 (仅只在三个调用点打过日志) --
inline bool PresentPerfSummaryEnabled()
{
    const char* summary = std::getenv("WINEHUA_VTEST_PRESENT_PERF_SUMMARY");
    return summary && summary[0] == '1' && !summary[1];
}

// -- 帧周期钳制: virgl 与 venus 策略不同, 各显式命名 --
// virgl (SurfaceQueueTarget): 0 → 默认; 否则 clamp 到 [min, virglMax]。
inline uint64_t NormalizeVirglFramePeriodNs(uint64_t framePeriodNs)
{
    if (!framePeriodNs) return kDefaultFramePeriodNs;
    if (framePeriodNs < kMinFramePeriodNs) return kMinFramePeriodNs;
    if (framePeriodNs > kVirglMaxFramePeriodNs) return kVirglMaxFramePeriodNs;
    return framePeriodNs;
}

// venus (VenusSurfaceQueueTarget): 在 [min, venusMax] 内则原样; 否则默认。
inline uint64_t NormalizeVenusFramePeriodNs(uint64_t value)
{
    return value >= kMinFramePeriodNs && value <= kVenusMaxFramePeriodNs
        ? value : kDefaultFramePeriodNs;
}

// -- 帧间隔提前量 (pacing): 两版策略不同, 显式命名 --
// virgl: 需保证剩余 >= min (floor=min); 显示周期减去提前量后不低于 min。
inline uint64_t VirglPacingPeriodNs(uint64_t displayPeriodNs)
{
    return displayPeriodNs > kMinFramePeriodNs + kDispatchLeadNs
        ? displayPeriodNs - kDispatchLeadNs : kMinFramePeriodNs;
}

// venus: 仅减去提前量, 不设 floor (显示周期本身是最小值)。
inline uint64_t VenusPacingPeriodNs(uint64_t displayPeriodNs)
{
    return displayPeriodNs > kDispatchLeadNs ? displayPeriodNs - kDispatchLeadNs
                                             : displayPeriodNs;
}

} // namespace winehua

#pragma once

#include <atomic>
#include <cstdint>

// P0-1 Task A (2026-09-17): 最近一次输入命中到的 toplevel。
// 只用于诊断日志（STEAM-WINDOW: 快照里报告 renderWindowKey 与 inputTarget 是否一致），
// 不参与任何合成/输入决策。定义在头里（inline 变量）以避免 compositor → input 的反向依赖。
namespace winehua {

inline std::atomic<uint32_t> g_lastInputTargetToplevel{0};

inline void NoteInputTargetToplevel(uint32_t toplevelId)
{
    g_lastInputTargetToplevel.store(toplevelId, std::memory_order_relaxed);
}

inline uint32_t LastInputTargetToplevel()
{
    return g_lastInputTargetToplevel.load(std::memory_order_relaxed);
}

} // namespace winehua

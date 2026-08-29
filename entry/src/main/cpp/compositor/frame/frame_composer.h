#pragma once
#include <cstdint>
#include <vector>

#include "presented_frame.h"

class DesktopCompositor;

// ============================================================================
// FrameComposer — 取帧路径策略实现 (重构第 2B 步 任务 2)
//
// 原 DesktopCompositor::TakeToplevelFrame 按 `id == desktopRootToplevelId_`
// 在单一函数内分流 PC 单窗口帧与 Desktop 整屏合成; 两条路径的取帧逻辑挤在
// 同一编排壳里, 调用点看不出"为什么走这条路"。本结构按 DisplayPolicy 拆为
// 两个策略实现 (FrameComposer/WindowFrameComposer), 编排者 (TakeToplevelFrame)
// 只问策略路由 (DisplayPolicy::FrameRouteFor) 要帧, 不再自己判 id。
//
// 两实现均无状态 (仅持 comp 引用; 全部合成状态在 comp_ / FramePlanner 的
// 快照池, 无跨帧 composer 状态), 故 TakeToplevelFrame 每次构图临时实例即可,
// 与旧实现每次新造 FramePlanner 的开销等价。行为平价: 每路 Compose 体与原
// 分支逐行等价, 锁边界/计时点/日志门控不变。
// ============================================================================
class FrameComposer {
public:
    virtual ~FrameComposer() = default;
    // 取指定 toplevel 的帧: DesktopRootFrameComposer 产整屏合成帧,
    // WindowFrameComposer 产 PC 单窗口帧。out 为像素载体 (调用方持有),
    // frame 为帧交付契约 (presented_frame.h)。
    virtual bool Compose(uint32_t id, std::vector<uint8_t>& out,
                         PresentedFrame& frame, bool frameTrace) = 0;
};

// Desktop root 帧合成 (原 TakeToplevelFrame desktop 分支):
// FramePlanner 锁内规划 + FrameBlitter 锁外纯像素合成, 整屏输出。
class DesktopRootFrameComposer : public FrameComposer {
public:
    explicit DesktopRootFrameComposer(DesktopCompositor& comp) : comp_(comp) {}
    bool Compose(uint32_t id, std::vector<uint8_t>& out,
                 PresentedFrame& frame, bool frameTrace) override;

private:
    DesktopCompositor& comp_;
};

// PC 单窗口帧 (原 TakeWindowFrameLocked): 窗口 SHM 帧作基底 + 窗口内
// subsurface blit, 单窗口输出; 窗口间层序由系统合成器保证, 不在此合成。
class WindowFrameComposer : public FrameComposer {
public:
    explicit WindowFrameComposer(DesktopCompositor& comp) : comp_(comp) {}
    bool Compose(uint32_t id, std::vector<uint8_t>& out,
                 PresentedFrame& frame, bool frameTrace) override;

private:
    DesktopCompositor& comp_;
};

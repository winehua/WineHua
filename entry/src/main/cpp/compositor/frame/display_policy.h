#pragma once
#include <cstdint>

// ============================================================================
// DisplayPolicy — PC 窗口模式 / Desktop 合成模式的差异策略查询点
//
// 背景: 两种模式的差异曾以 `IsDesktopMode()` 布尔散布 ~30 处, 调用点看不出
// "为什么分支"。本结构把差异归为四类 (见 docs/CPP_REFACTOR_PLAN.md 3.4),
// 每类一个命名查询, 调用点表达语义而非模式位:
//
//   ① 事件派发  OhosWindowPerToplevel()  — PC 每个 toplevel 对应独立 OHOS
//      窗口, created/argb/move/mask/move_start 等事件驱动 ArkTS 侧窗口
//      生命周期; Desktop 全部窗口合成在 root 内, ArkTS 无独立窗口可驱动
//   ② subsurface 处理  SubsurfaceAsLayer() — Desktop 合成进 root 帧的
//      layer (TakeToplevelFrame 时叠加); PC 登记 popup 伪 toplevel,
//      由 ArkTS 独立子窗口渲染
//   ③ 渲染取帧  RootCompositing() — Desktop 所有内容合成到 root 帧
//      (z-order/dirty/遮挡修复/root 帧输出都以此为据); PC 每窗独立渲染,
//      层序由系统合成器保证
//   ④ 输入命中  CompositorRoutesInput() — Desktop 由 compositor 按桌面
//      坐标自命中 (含 subsurface 层精确 enter) 并路由键盘焦点; PC 由
//      OHOS 窗口系统路由, 事件到达时已携带目标 toplevel
//
// 模式上报类调用 (给 wine 传环境变量/标记 desktop 进程/日志) 不属于策略
// 决策, 仍用 WaylandServer::IsDesktopMode()。
//
// phone 模式不走本 policy: 它只在 ncp_dispatch/phone_adapter 传输层,
// compositor 完全不感知 phone — 这是设计决定, 保持隔离。
// ============================================================================

struct DisplayPolicy {
    // 唯一存储位: true = Desktop 合成模式 (Pad/桌面), false = PC 窗口模式。
    // 新增分支请用下面的命名查询, 不要直接判这个位。
    bool desktop = false;

    static DisplayPolicy FromDesktopMode(bool d) { return DisplayPolicy{d}; }

    // ① 事件派发 (PC=true)
    bool OhosWindowPerToplevel() const { return !desktop; }
    // ② subsurface 作为 desktop layer 合成 (Desktop=true)
    bool SubsurfaceAsLayer() const { return desktop; }
    // ③ 渲染经 root 帧合成 (Desktop=true)
    bool RootCompositing() const { return desktop; }
    // ④ 输入由 compositor 自命中/路由 (Desktop=true)
    bool CompositorRoutesInput() const { return desktop; }

    // ⑤ 取帧路径路由 (任务 2, 重构第 2B 步): 该 toplevel 的帧应走 Desktop root
    //   整屏合成还是 PC 单窗口帧。原 DesktopCompositor::TakeToplevelFrame 按
    //   `RootCompositing() && id == desktopRootToplevelId_` 在单一函数内分流;
    //   现下沉为策略查询 — 编排者只问策略要帧, 不再自己判 id。逐点等价:
    //   desktop 且 id==root 才走 root 合成, 其余 (PC 模式 / non-root) 走窗口帧。
    enum class FrameRoute { DesktopRoot, Window };
    FrameRoute FrameRouteFor(uint32_t id, uint32_t desktopRootToplevelId) const {
        return (desktop && id == desktopRootToplevelId)
                   ? FrameRoute::DesktopRoot
                   : FrameRoute::Window;
    }
};

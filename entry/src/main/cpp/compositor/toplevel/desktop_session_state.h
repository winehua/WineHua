#pragma once
#include <atomic>
#include <cstdint>

#include "compositor/frame/compositor_constants.h"
#include "compositor/frame/display_policy.h"

// DesktopSessionState — 桌面会话共享状态 POD (重构第 6B 步)
//
// 出处: docs/COMPOSITOR_REFACTOR_PLAN.md §三 "DesktopSessionState —
// desktopRootToplevelId/output 尺寸等共享状态 POD, 消灭『引用成员指向宿主
// 子字段』的隐式同步" + §四阶段6 (共享状态收进 POD, 组件持同一对象;
// DesktopRootManager 真正拥有 root 状态)。
//
// # 本结构包含什么
// 会话级"一份状态多个所有者"的状态单点: desktop root 身份 (root/pending/
// taskbar/recognitionEnabled)、显示策略 (policy)、显示尺寸 (outputW/H)、
// 会话首帧标记 (firstFrame)。状态成员归属表 (成员 → 写读方 → 线程域)
// 见 docs/COMPOSITOR_REFACTOR_STATUS.md §二 6B 小节 — 逐成员核对:
// 不属于会话共享的 (如 toplevelEventSuppressed_ 已收口于 ToplevelEventBus,
// ZC 状态键已收口于 ZcBridge, per-toplevel 状态收口于 ToplevelState) 均
// 不在本结构。
//
// # 存储位置重排 (行为平价)
// 这些成员此前是 WaylandServer 的私有字段 (desktopRootToplevelId_ 等) /
// public 字段 (outputW_/outputH_) / 跨组件引用注入 (DesktopCompositor/
// InputResolver/PopupManager 持 const 引用, InputManager/PointerExtras 持
// 指针, DesktopRootManager 持可写引用)。6B 只做"存储位置重排 + 指向迁移":
// 写读时机/线程域/日志逐字不变, 注入形态不变 (引用/指针仍指向同一对象 —
// 只是对象从宿主字段换成 POD 字段)。
//
// # 字段可见性与注入引用的关系
// 字段保持可寻址 (public): 装配注入的引用 (const DisplayPolicy& /
// const uint32_t& / const int32_t& / 可写 uint32_t&) 需要绑定到 POD 内
// 真实存储, 私有化会迫使注入形态改为"持 POD 引用"而破坏 6A 已装配形态
// (红线: 注入形态兼容)。对外 (WaylandServer 公共接口) 的读经访问器
// (OutputWidth/OutputHeight/Policy/GetDesktopRootToplevelId), 写经
// WaylandServer 语义方法 (SetOutputSize/SetDesktopMode/ResetFirstFrame) —
// 外部不直接摸 POD。
//
// # 首次声明 + 原子字段
// firstFrame 是 std::atomic<bool> (会话首帧 CAS, 与旧 firstFrame_ 同属性:
// wl/其它线程复位 + wl 线程 CAS; 语义/复位点逐字平移, 仅移入 POD)。
// 本结构因 atomic 成员不可拷贝/移动 (无代码需要拷贝, 单例成员)。
class DesktopSessionState {
public:
    // -- root 身份 (desktop 模式; 写/读点与线程域见 STATUS §二 6B 归属表) --
    uint32_t desktopRootToplevelId = 0;          // 正式 root (DesktopRootManager 决策写入)
    uint32_t pendingDesktopRootToplevelId = 0;   // 待提 root (recognition disabled 暂存)
    uint32_t taskbarId = 0;                      // app_id=="explorer.exe.taskbar" (RaiseToplevel/GetWorkAreaHeight 用)
    bool desktopRootRecognitionEnabled = true;   // root 识别开关 (ArkTS 端完成后 写, DesktopRootManager 决策读)

    // -- 显示策略 (唯一模式存储位; SetDesktopMode 写, 策略查询散布各处只读) --
    DisplayPolicy policy{};

    // -- 显示尺寸 (唯一输出尺寸存储位; SetOutputSize 写 — ArkTS 权威源,
    //    桌面 root resize 不反写, 见 NotifyToplevelResize 注释) --
    int32_t outputW = compositor_consts::kDefaultOutputWidth;
    int32_t outputH = compositor_consts::kDefaultOutputHeight;

    // -- 会话首帧标记 (一次性 CAS: 首个 commit 触发 focus 预注入;
    //    Start/Stop/ResetSessionState/ResetFirstFrame 复位) --
    std::atomic<bool> firstFrame{false};
};

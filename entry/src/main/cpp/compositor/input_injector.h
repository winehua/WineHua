#pragma once

#include <wayland-server-core.h>  // wl_resource/wl_fixed_t — 协议注入层专属 (不在 host_tests 链路)
#include <cstdint>

#include "compositor/input_state_tracker.h"

// ============================================================================
// InputInjector — 唯一碰 wl_*_send_* 的注入层 (重构第 4C2 步从 InputManager 抽离)
//
// 职责: 把事件发给 Seat 登记的 wl_pointer/wl_keyboard 资源 —
//   - 注入前防御 (surface 存活校验/资源为空 DROP/client 过滤)
//   - 焦点/串行号状态写入 (经注入的 InputStateTracker 指针, 事件序
//     串行号必须与 enter 一致 — 协议层要求)
//   - 丢帧统计 (原 input_manager.cpp file-static gDrop* 同迁)
//
// 不碰的事 (编排层职责): 队列/去重/坐标变换/策略分流 — 编排层 Poll 出批后
// 逐事件调本层; 唯一例外是 PointerExtras::SendRelativeMotion
// (InjectRelativeMotion) — 它是"相对指针扩展"的注入面, 与 wl_*_send_* 同级。
//
// 线程模型: 全部只在 Wayland 线程调用 (FlushQueue dispatch / wl_core
// 首帧 focus 预设 — 原 InputManager::Inject* 调用点不变)。
// ============================================================================

class InputInjector {
public:
    // tracker 引用以指针注入 (InputManager 持成员对象, 构造时绑定;
    // 生命周期与 InputManager 单例一致)
    explicit InputInjector(InputStateTracker* tracker);

    void InjectPointerEnter(uint32_t tl, wl_resource* surface, wl_fixed_t sx, wl_fixed_t sy);
    void InjectPointerMotion(wl_fixed_t sx, wl_fixed_t sy);
    void InjectRelativeMotion(wl_fixed_t dx, wl_fixed_t dy);
    void InjectPointerButton(uint32_t button, uint32_t state);
    void InjectPointerAxis(int axis, wl_fixed_t value);
    void InjectPointerLeave();
    void InjectKeyboardEnter(uint32_t tl, wl_resource* surface);
    void InjectKeyboardKey(uint32_t key, uint32_t state);
    void InjectKeyboardLeave();
    void InjectKeyboardModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

private:
    InputStateTracker* tracker_;
};

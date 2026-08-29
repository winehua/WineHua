#pragma once

#include <wayland-server-core.h>  // wl_fixed_t (wl_util)
#include <atomic>
#include <cstdint>

#include "compositor/geometry.h"  // FitRect

class EglRenderer;  // 前向声明 (定义在 egl_renderer.h)

// ============================================================================
// InputSpaceMapper — 输入坐标空间映射单点
//
// 背景 (docs/COMPOSITOR_REFACTOR_PLAN.md §2.2 / §2.4):
//   - 渲染器登记结构泄漏到输入模块: 原 InputManager::CoordTransform 内联
//     "rootId → GetAnyRenderer" 三级 fallback 查找, InputManager 因此认识
//     PluginManager/renderer 内部实现。
//   - lastGlobalPtr 一名多义: 同两个原子字段在 desktop 模式存"桌面逻辑坐标"、
//     PC 模式存"窗口局部+窗口位置"还原值 (onPointerWarp 的 PC 分支存
//     surface 局部原值), 读方 (move grab) 仅凭 Policy 位隐式猜空间。
//
// 职责 (重构第 4C1 步从 InputManager 收口, 行为平价 — 纯搬移/重标, 无行为变化):
//   1. ResolveRendererFor: renderer 查找 fallback 链 (tl → root → any),
//      解释权归本模块 — InputManager 不再 include plugin_manager.h。
//   2. CoordTransform: 物理像素 → wine 逻辑坐标的 letterbox 逆映射
//      (锚点 = renderer GetInputLetterbox, 重构第 2B 步 PresentedFrame 契约)。
//   3. lastGlobalPtr 显式语义: GlobalPtrState 带 Space 标签 (写入点逐处
//      标注真实坐标空间), 读方不再隐式猜。
//
// 线程模型: 无内部锁 (不新增锁, 红线)。UpdateGlobalPtr 被 NAPI/请求源线程
// (SendPointerEvent/SendScrollEvent/Warp) 写, GetGlobalPtrX/Y 被 Wayland 线
// 程 (StartMoveGrab) 读 — 字段为独立 std::atomic, 读写序与旧实现逐点一致。
// CoordTransform 被 NAPI 线程调用 (保持原调用线程)。
//
// 4-C2 预留: 本模块不收 wl_*_send_* / PointerExtras 状态 / 队列与焦点追踪;
// 那是 InputStateTracker/InputInjector/InputQueue 的边界。InputManager 拆层
// 后经本类公开接口继续使用坐标映射。
// ============================================================================

// lastGlobalPtr 的显式语义 (PLAN §2.4 一名多义收口): 同字段两坐标系。
//   Desktop — 桌面逻辑坐标 (desktop 合成模式):
//             SendPointerEvent/SendScrollEvent 桌面分支 (root CoordTransform 输出);
//             OnPointerWarp desktop 分支 (SurfaceLocalToDesktop 换算后)。
//   Window  — 窗口系坐标 (PC 模式):
//             SendPointerEvent/SendScrollEvent PC 分支 = 窗口局部坐标+窗口位置
//             (tlGeo) 还原值 (grab 偏移基准用绝对坐标);
//             OnPointerWarp PC 分支 = surface 局部坐标原值 (未经 SurfaceLocalToDesktop,
//             且不加 tlGeo — 历史语义, 4C1 只重标不修正)。
struct GlobalPtrState {
    enum class Space { Desktop, Window };
    Space space = Space::Desktop;  // 标签: 纯语义标记, 无算法消费方
    wl_fixed_t x = 0;
    wl_fixed_t y = 0;
};

class InputSpaceMapper {
public:
    static InputSpaceMapper* GetInstance();

    // renderer 查找 fallback 链 (原 InputManager::CoordTransform 头部内联):
    // tl 直接查 → RootCompositing (desktop) 下经 root 查 → 当前登记的唯一
    // renderer。失败返回 nullptr (调用方按"无 renderer 处理"退出,
    // 见 CoordTransform)。
    EglRenderer* ResolveRendererFor(uint32_t tl);

    // 物理像素 → wine 逻辑坐标 (原 InputManager::CoordTransform, 函数体逐字
    // 搬移)。outLb 非空时回传本次映射使用的 letterbox 几何 (调用方做内容区
    // 钳制用)。
    void CoordTransform(double px, double py, uint32_t tl, wl_fixed_t* outX, wl_fixed_t* outY,
                        FitRect* outLb = nullptr);

    // "最近一次注入的全局指针位置" (move grab 偏移基准, 原 lastGlobalPtrX/Y_)。
    // space 标签按写入点实际坐标空间显式标注 (见 GlobalPtrState 注释)。
    void UpdateGlobalPtr(wl_fixed_t x, wl_fixed_t y, GlobalPtrState::Space space);
    void ResetGlobalPtr();  // Wine 会话终结复位 (与旧 "=0" 等价)
    wl_fixed_t GetGlobalPtrX() const;
    wl_fixed_t GetGlobalPtrY() const;
    GlobalPtrState GetGlobalPtr() const;  // 诊断快照 (含空间标签)

private:
    InputSpaceMapper() = default;

    // 与旧实现相同的独立原子字段 (X/Y 仍可被读方分两次 load, 写序先 X 后 Y —
    // 不合成单结构体 atomic, 避免把"X 新 Y 旧"的既有竞态窗口语义悄悄收紧)。
    std::atomic<wl_fixed_t> globalPtrX_{0};
    std::atomic<wl_fixed_t> globalPtrY_{0};
    std::atomic<GlobalPtrState::Space> globalPtrSpace_{GlobalPtrState::Space::Desktop};
};

#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <cstdint>

#include "compositor/frame/geometry.h"
#include "compositor/frame/display_policy.h"      // DisplayPolicy (6A 装配注入共享引用)
#include "compositor/input/input_space_mapper.h"  // CoordTransform 落点 + lastGlobalPtr 显式语义 (4C1)
#include "compositor/input/input_queue.h"         // 队列机制 (4C2 拆层)
#include "compositor/input/input_state_tracker.h" // 纯状态 (4C2 拆层)
#include "compositor/input/input_injector.h"      // 唯一 wl_*_send_* 注入 (4C2 拆层)

// InputManager: 统一输入事件管理器 — 编排门面 (重构第 4C2 步瘦身)
//
// 分层 (PLAN §四阶段4, 4C2):
//   InputManager  本类: 策略编排 — NAPI 入口 (坐标变换/目标解析/焦点判定/
//                   enter/leave 三语义变体) + FlushQueue dispatch (去重后的
//                   事件批 → 注入器) ; 公开接口与单例保持不变, 30+ 调用点
//                   (napi_init/wl_core/seat/wayland_server) 零改动
//   InputQueue    队列机制: pipe 唤醒 / 去重 Poll / flush_clients
//   InputStateTracker 纯状态: 按钮位掩码/修饰键/焦点/可见性/serial/相对基线
//   InputInjector 唯一碰 wl_*_send_* : Enter/Motion/Button/Axis/Leave/Key
//   InputSpaceMapper (4C1) 坐标变换单点
//
// 线程模型 (与旧实现逐字):
//   - NAPI/JS 线程: SendPointerEvent / SendKeyEvent / SendScrollEvent →
//     坐标变换 + 状态更新 + Enqueue (queue 锁内 push, 锁外 pipe)
//   - Wayland 线程: Pipe 回调 → FlushQueue → Poll+去重 → 注入 →
//     wl_display_flush_clients
// 所有 wl_*_send_* 调用必须在 Wayland 线程 (通过 pipe 唤醒)

class Seat;  // 前向声明
class ToplevelManager;   // 前向声明 (6A 装配注入, 见 BindCompositorDeps)
class InputResolver;     // 前向声明 (6A 装配注入)
class MoveGrabHandler;   // 前向声明 (6A 装配注入)

class InputManager {
public:
    static InputManager* GetInstance();

    // -- 生命周期 (WaylandServer::Start/Stop 调用) --
    void Initialize(wl_display* display);
    void Shutdown();

    // -- 子组件装配注入 (重构第 6A 步, WaylandServer::Start 调用) --
    // 替代本类经 WaylandServer 的一行转发 (FindInputTargetAt/
    // SurfaceLocalToDesktop/GetToplevelGeometrySnapshot/GetSurfaceForToplevel/
    // IsMoveGrabActive/GetMoveGrabToplevelId/Policy/GetDesktopRootToplevelId):
    // 输入编排直连 ToplevelManager/InputResolver/MoveGrabHandler/共享
    // policy+rootId 引用 (与 DesktopCompositor/InputResolver 注入同源)。
    // 生命周期: 引用属于 WaylandServer 单例成员 (Start 前已构造), 装配在
    // wl 事件循环启动前一次性 — 之后各线程 (NAPI/Wayland) 只读共享, 无新锁
    // (与 4C1 warpSink 装配同模式)。指针成员为装配出口 (单例构造不晚于
    // Start 时依赖存在), 装配前不可调用任何依赖方法。
    void BindCompositorDeps(ToplevelManager& tmgr, InputResolver& resolver,
                            MoveGrabHandler& moveGrab, const DisplayPolicy& policy,
                            const uint32_t& desktopRootToplevelId);

    // -- NAPI 入口 (JS 线程调用) --
    // action: ArkTS MouseAction (Press=1, Release=2, Move=3)
    // px/py: 已转为物理像素的坐标
    // button: 已由 ArkTS MouseMap 映射的 evdev button code (0x110/0x111/0x112)
    // rawDx/rawDy: ArkTS MouseEvent.rawDeltaX/Y (API15+, 鼠标硬件原始增量,
    //   物理移动距离单位, 非像素) — 仅 Move 有效, 其余事件/触屏路径传 0。
    //   是相对模式 (dinput 视角) 的真实位移源, 见 ACT_MOVE 处理
    // fromMouse: 来自物理鼠标 onMouse 通道 (触屏 onTouch 路径传 false) —
    //   相对模式下区分点击语义, 见 ACT_PRESS 处理
    void SendPointerEvent(uint32_t toplevelId, int action, double px, double py, int button,
                          double rawDx = 0, double rawDy = 0, bool fromMouse = false);

    // evdevCode: 已由 ArkTS KeyMap 映射的 evdev keycode
    // pressed: true=按下, false=释放
    void SendKeyEvent(uint32_t toplevelId, int evdevCode, bool pressed);

    // axis: 0=垂直(SCROLL_VERTICAL), 1=水平(SCROLL_HORIZONTAL)
    // value: 轴值 (正值=向下/向右, 负值=向上/向左)
    // scrollStep: ArkTS AxisEvent.scrollStep
    // px/py: 鼠标在组件上的物理像素坐标
    void SendScrollEvent(uint32_t toplevelId, int axis, double value, int scrollStep, double px, double py);

    // -- Wayland 线程注入 (编制层委托 InputInjector; 公开方法保留 —
    //    wl_core.cpp 首帧 focus 预设直接调用本方法) --
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

    // -- 状态重置 (Seat resource destroy 时调用) --
    void ResetPointerEnter();
    void ResetKeyboardEnter();

    // -- Wine 会话终结统一收口 (WaylandServer::ResetSessionState 调用) --
    // 全量复位会话级状态 (焦点/按键/修饰键/指针位置/可见性表), 使下一次
    // 引擎启动 (冷/热) 从与冷启动一致的基线开始 — 热重启复用同一进程,
    // 任何"只清不重置"的字段都会跨会话残留 (卡键/修饰键污染/焦点漂移)
    void ResetSessionState();

    // surface 销毁时重置焦点, 防止后续 Inject*Leave 引用已销毁的 surface
    // 如果不重置, 会导致 Wayland 协议错误 "invalid object" → Wine 断开连接
    void OnSurfaceDestroyed(wl_resource* surface);

    // -- 窗口可见性 (输入抑制) --
    void SetToplevelVisible(uint32_t tl, bool visible);

    // -- Focus 查询 (线程安全) --
    bool HasPointerFocus() const { return tracker_.HasPointerFocus(); }
    bool NeedsPointerEnter() const;
    uint32_t GetPointerFocusedToplevel() const { return tracker_.PointerFocusedToplevel(); }

    bool HasKeyboardFocus() const { return tracker_.KeyboardEntered(); }
    uint32_t GetKeyboardFocusedToplevel() const { return tracker_.KeyboardFocusedToplevel(); }

    // -- 辅助: 物理像素 → Wine 逻辑坐标映射 (供 FindToplevelAt 等使用) --
    // outLb 非空时回传本次映射使用的 letterbox 几何 (调用方做内容区钳制用)
    // 实现已迁 InputSpaceMapper (compositor/input_space_mapper.*, 重构第 4C1 步);
    // 本方法保留为公开委托 — renderer 查找 fallback 链与逆映射收口在 mapper。
    void CoordTransform(double px, double py, uint32_t tl, wl_fixed_t* outX, wl_fixed_t* outY,
                        FitRect* outLb = nullptr);

    // 最近一次注入的全局指针位置 (NAPI 线程写, Wayland 线程读)。
    // 语义 = move_grab 输入空间的绝对坐标, 空间标签显式化 (4C1):
    // desktop 为桌面逻辑坐标 (GlobalPtrState::Space::Desktop), PC 为
    // 窗口局部坐标 + 窗口位置 还原值 (Space::Window, OnPointerWarp 的 PC
    // 分支为 surface 局部原值 — 历史语义, 详见 input_space_mapper.h)。
    // xdg_toplevel.move 建立 grab 时据此立即算固定 grab 偏移 (绝对定位, 无累积)
    wl_fixed_t GetLastGlobalPointerX() const {
        return InputSpaceMapper::GetInstance()->GetGlobalPtrX();
    }
    wl_fixed_t GetLastGlobalPointerY() const {
        return InputSpaceMapper::GetInstance()->GetGlobalPtrY();
    }

    // SetCursorPos 位置同步 (wp_pointer_warp_v1 → PointerExtras 调入,
    // Wayland 线程)。sx/sy 是 wine 的 surface 局部坐标。wineserver 光标
    // 已在 wine 侧移动到位, host 只同步 move grab 的偏移基准。
    void OnPointerWarp(wl_resource* surface, double sx, double sy);

private:
    InputManager();

    // 修饰键快照入队 (读取 tracker 当前状态 → InputQueue, 消费侧读事件快照)
    void EnqueueModifiers();

    // Wayland 线程: Poll 去重批 → 逐事件 dispatch (注入 + move grab 语义) →
    // flush clients。由 InputQueue pipe 回调经构造注入的 flush 回调调用。
    void FlushQueue();

    // -- enter/leave 收敛 helper (重构第 4C2 步) --
    // 三变体 (ACT_PRESS / ACT_MOVE / SCROLL-ENTER) 中"确认要 enter 后"的共同
    // 动作: needLeave 双判据 (targetSurf 非空=surface 级, 否则 toplevel 级) →
    // PTR_LEAVE 入队 → PTR_ENTER 入队 (与旧三处逐字同一顺序/判定)。
    // needEnter 判定与 skipEnter 守卫**不**收敛 — PRESS 的强制重入+守卫、
    // MOVE/SCROLL 的 NeedsPointerEnter 先判, 三份语义不同, 由调用者各自
    // 保留 (红线: 不允许统一成一种语义)。targetSurf = 桌面裁决命中层
    // (nullptr = PC/回退的 toplevel 级语义)。
    void SubmitEnterLeave(uint32_t tl, wl_resource* targetSurf, wl_resource* surf,
                          wl_fixed_t x, wl_fixed_t y);

    // 四层 (拆层后 InputManager 只持成员对象; tracker 先于 injector 声明 —
    // 构造注入其引用, 见 InputManager 构造函数)
    InputStateTracker tracker_;
    InputQueue queue_;
    InputInjector injector_;

    // 6A 装配注入的子组件引用 (指针形式: InputManager 单例可能先于
    // WaylandServer 构造, BindCompositorDeps 在 Start 写、事件循环启动前 —
    // 见公开方法注释; controller 里注入方向是"子组件→编排层"单向)
    ToplevelManager* tmgr_ = nullptr;
    InputResolver* resolver_ = nullptr;
    MoveGrabHandler* moveGrab_ = nullptr;
    const DisplayPolicy* policy_ = nullptr;
    const uint32_t* desktopRootToplevelId_ = nullptr;
};

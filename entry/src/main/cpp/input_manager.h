#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "compositor/geometry.h"

// InputManager: 统一输入事件管理器
//
// 职责:
//   - 接收 ArkTS 通过 NAPI 发来的标准化事件 (唯一输入源)
//   - 坐标转换 (viewport → wine buffer letterbox)
//   - 状态追踪 (button bitmask, modifier state, pointer/keyboard focus)
//   - 事件入队 (pipe → Wayland 线程)
//   - 事件注入 (wl_pointer_*/wl_keyboard_* send)
//
// 线程模型:
//   - NAPI/JS 线程: SendPointerEvent / SendKeyEvent → 坐标变换 + 状态更新 + Enqueue
//   - Wayland 线程: FlushQueue → 去重 → Inject → wl_display_flush_clients
//
// 所有 wl_*_send_* 调用必须在 Wayland 线程 (通过 pipe 唤醒)

class Seat;  // 前向声明

class InputManager {
public:
    static InputManager* GetInstance();

    // -- 生命周期 (WaylandServer::Start/Stop 调用) --
    void Initialize(wl_display* display);
    void Shutdown();

    // -- NAPI 入口 (JS 线程调用) --
    // action: ArkTS MouseAction (Press=1, Release=2, Move=3)
    // px/py: 已转为物理像素的坐标
    // button: 已由 ArkTS MouseMap 映射的 evdev button code (0x110/0x111/0x112)
    void SendPointerEvent(uint32_t toplevelId, int action, double px, double py, int button);

    // evdevCode: 已由 ArkTS KeyMap 映射的 evdev keycode
    // pressed: true=按下, false=释放
    void SendKeyEvent(uint32_t toplevelId, int evdevCode, bool pressed);

    // axis: 0=垂直(SCROLL_VERTICAL), 1=水平(SCROLL_HORIZONTAL)
    // value: 轴值 (正值=向下/向右, 负值=向上/向左)
    // scrollStep: ArkTS AxisEvent.scrollStep
    // px/py: 鼠标在组件上的物理像素坐标
    void SendScrollEvent(uint32_t toplevelId, int axis, double value, int scrollStep, double px, double py);

    // -- Wayland 线程注入 (由 FlushQueue 调用) --
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
    bool HasPointerFocus() const { return pointerFocusedToplevel_.load() != 0; }
    bool NeedsPointerEnter() const;
    uint32_t GetPointerFocusedToplevel() const { return pointerFocusedToplevel_.load(); }

    bool HasKeyboardFocus() const { return keyboardEntered_.load(); }
    uint32_t GetKeyboardFocusedToplevel() const { return keyboardFocusedToplevel_.load(); }

    // -- 辅助: 物理像素 → Wine 逻辑坐标映射 (供 FindToplevelAt 等使用) --
    // outLb 非空时回传本次映射使用的 letterbox 几何 (调用方做内容区钳制用)
    wl_fixed_t CoordTransform(double px, double py, uint32_t tl, wl_fixed_t* outX, wl_fixed_t* outY,
                              FitRect* outLb = nullptr);

    // 最近一次注入的全局指针位置 (NAPI 线程写, Wayland 线程读)。
    // 语义 = move_grab 输入空间的绝对坐标: desktop 为桌面逻辑坐标,
    // PC 为 窗口局部坐标 + 窗口位置 还原值。xdg_toplevel.move 建立 grab
    // 时据此立即算固定 grab 偏移 (绝对定位, 无累积)
    wl_fixed_t GetLastGlobalPointerX() const { return lastGlobalPtrX_.load(); }
    wl_fixed_t GetLastGlobalPointerY() const { return lastGlobalPtrY_.load(); }

    // SetCursorPos 位置同步 (wp_pointer_warp_v1 → PointerExtras 调入,
    // Wayland 线程)。sx/sy 是 wine 的 surface 局部坐标。wineserver 光标
    // 已在 wine 侧移动到位, host 只同步 move grab 的偏移基准。
    void OnPointerWarp(wl_resource* surface, double sx, double sy);

private:
    InputManager() = default;

    // -- 事件队列 (NAPI → Wayland 线程) --
    struct InputEvent {
        enum Type { PTR_ENTER, PTR_LEAVE, PTR_MOTION, PTR_BUTTON, PTR_AXIS,
                    REL_MOTION,
                    KBD_ENTER, KBD_LEAVE, KBD_KEY, KBD_MODIFIERS } type;
        uint32_t tl = 0;
        wl_resource* surface = nullptr;
        wl_fixed_t x = 0, y = 0;
        uint32_t btn_or_key = 0;
        uint32_t state = 0;
        // axis fields
        int axis = 0;           // 0=vertical, 1=horizontal
        wl_fixed_t axis_value = 0;
        // modifiers fields
        uint32_t mod_depressed = 0, mod_latched = 0, mod_locked = 0, mod_group = 0;
    };

    std::mutex queueMutex_;
    std::vector<InputEvent> queue_;
    int pipeRead_ = -1, pipeWrite_ = -1;
    struct wl_event_source* pipeSource_ = nullptr;
    wl_display* display_ = nullptr;

    void Enqueue(InputEvent::Type type, uint32_t tl, wl_resource* surface,
                 wl_fixed_t x, wl_fixed_t y, uint32_t btn_or_key, uint32_t state);
    void EnqueueModifiers();  // 入队当前 modifiers_depressed_ 等状态
    void FlushQueue();
    static int OnPipeReadable(int fd, uint32_t mask, void* data);

    // -- 状态追踪 --
    // button bitmask: bit0=left(0x110), bit1=right(0x111), bit2=middle(0x112)
    static constexpr unsigned kBtnBitLeft   = 0;
    static constexpr unsigned kBtnBitRight  = 1;
    static constexpr unsigned kBtnBitMiddle = 2;
    uint32_t pressedButtons_ = 0;

    unsigned ButtonToBit(uint32_t btn);
    uint32_t BitToButton(unsigned bit);

    // modifier state
    uint32_t modifiers_depressed_ = 0;
    uint32_t modifiers_latched_ = 0;
    uint32_t modifiers_locked_ = 0;
    uint32_t modifiers_group_ = 0;
    void UpdateModifiers(int evdevCode, bool pressed);
    bool IsModifierKey(int evdevCode);

    // 最近一次注入的全局指针位置 (跨线程, 供 move grab 建立时算偏移)
    std::atomic<wl_fixed_t> lastGlobalPtrX_{0};
    std::atomic<wl_fixed_t> lastGlobalPtrY_{0};

    // pointer focus
    std::atomic<uint32_t> pointerFocusedToplevel_{0};
    // atomic: NAPI 线程 (SendPointerEvent) 用它做 surface 级 enter/leave 判定,
    // Wayland 线程 (Inject*) 写入 — desktop 模式菜单层与父窗口同 toplevelId,
    // 仅比较 toplevelId 无法察觉焦点需要在两个 surface 间切换
    std::atomic<wl_resource*> pointerFocusedSurface_{nullptr};
    std::atomic<uint32_t> pointerEnterSerial_{0};
    std::atomic<uint32_t> serial_{1};

    // keyboard focus (独立于 pointer)
    std::atomic<uint32_t> keyboardFocusedToplevel_{0};
    wl_resource* keyboardFocusedSurface_ = nullptr;
    std::atomic<bool> keyboardEntered_{false};

    // 窗口可见性 (鸿蒙侧最小化时抑制输入)
    std::mutex visibleMutex_;
    std::unordered_map<uint32_t, bool> toplevelVisible_;

    /*
     * 相对指针增量基准 (zwp_relative_pointer_v1): wine 相对模式 (隐藏光标 +
     * 约束, wayland_pointer.c needs_relative) 丢弃绝对 motion, 光标位置 =
     * 基线 + 增量累积。host 不做模式判断 — 始终发绝对 motion, 同时把注入
     * 坐标序列差分出增量, 有 relative 对象就入队 REL_MOTION。
     * 增量在 surface 局部坐标空间 (与绝对 motion 同空间, 见 SendPointerEvent)。
     * 仅 SendPointerEvent 所在线程 (ArkTS NAPI) 访问, 无需加锁。
     */
    double lastLocalX_ = 0, lastLocalY_ = 0;
    bool hasLastLocal_ = false;

    // 最近一次按下时刻 (ACT_RELEASE 的脉冲拉伸计时, 见 input_manager.cpp)
    std::atomic<uint32_t> lastPressMs_{0};
};

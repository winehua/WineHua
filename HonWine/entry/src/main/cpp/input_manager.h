#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

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

    // -- Wayland 线程注入 (由 FlushQueue 调用) --
    void InjectPointerEnter(uint32_t tl, wl_resource* surface, wl_fixed_t sx, wl_fixed_t sy);
    void InjectPointerMotion(wl_fixed_t sx, wl_fixed_t sy);
    void InjectPointerButton(uint32_t button, uint32_t state);
    void InjectPointerLeave();
    void InjectKeyboardEnter(uint32_t tl, wl_resource* surface);
    void InjectKeyboardKey(uint32_t key, uint32_t state);
    void InjectKeyboardLeave();
    void InjectKeyboardModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

    // -- 状态重置 (Seat resource destroy 时调用) --
    void ResetPointerEnter();
    void ResetKeyboardEnter();

    // surface 销毁时重置焦点, 防止后续 Inject*Leave 引用已销毁的 surface
    // 如果不重置, 会导致 Wayland 协议错误 "invalid object" → Wine 断开连接
    void OnSurfaceDestroyed(wl_resource* surface);

    // -- Focus 查询 (线程安全) --
    bool HasPointerFocus() const { return pointerFocusedToplevel_.load() != 0; }
    bool NeedsPointerEnter() const;
    uint32_t GetPointerFocusedToplevel() const { return pointerFocusedToplevel_.load(); }

    bool HasKeyboardFocus() const { return keyboardEntered_.load(); }
    uint32_t GetKeyboardFocusedToplevel() const { return keyboardFocusedToplevel_.load(); }

private:
    InputManager() = default;

    // -- 辅助 --
    wl_fixed_t CoordTransform(double px, double py, uint32_t tl, wl_fixed_t* outX, wl_fixed_t* outY);

    // -- 事件队列 (NAPI → Wayland 线程) --
    struct InputEvent {
        enum Type { PTR_ENTER, PTR_LEAVE, PTR_MOTION, PTR_BUTTON,
                    KBD_ENTER, KBD_LEAVE, KBD_KEY, KBD_MODIFIERS } type;
        uint32_t tl = 0;
        wl_resource* surface = nullptr;
        wl_fixed_t x = 0, y = 0;
        uint32_t btn_or_key = 0;
        uint32_t state = 0;
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

    // pointer focus
    std::atomic<uint32_t> pointerFocusedToplevel_{0};
    wl_resource* pointerFocusedSurface_ = nullptr;
    std::atomic<uint32_t> pointerEnterSerial_{0};
    std::atomic<uint32_t> serial_{1};

    // keyboard focus (独立于 pointer)
    std::atomic<uint32_t> keyboardFocusedToplevel_{0};
    wl_resource* keyboardFocusedSurface_ = nullptr;
    std::atomic<bool> keyboardEntered_{false};
};

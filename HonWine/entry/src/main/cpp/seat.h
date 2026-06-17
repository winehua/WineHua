#pragma once
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

// Seat: wl_seat 协议实现
//
// 职责:
//   - wl_seat global 注册/注销
//   - wl_pointer / wl_keyboard 资源生命周期管理
//   - XKB_V1 keymap 发送
//   - 对外提供 GetPointerResource/GetKeyboardResource (InputManager 注入时使用)
//
// 事件注入和状态追踪已移至 InputManager。
// 使用 vector 管理资源, 支持 Wine 多窗口反复 bind/destroy 场景。

class Seat {
public:
    static Seat* GetInstance();

    // -- 生命周期 --
    void Register(wl_display* display);
    void Unregister();

    // -- 资源查询 (线程安全, InputManager 调用) --
    wl_resource* GetPointerResource();
    wl_resource* GetKeyboardResource();
    std::vector<wl_resource*> GetAllPointerResources();
    std::vector<wl_resource*> GetAllKeyboardResources();
    bool HasPointerResource() const { return ptrCount_.load() > 0; }
    bool HasKeyboardResource() const { return kbdCount_.load() > 0; }

    // wl_display 访问 (InputManager 需要, 已移至 InputManager)
    // wl_display* GetDisplay() const { return display_; }

    // -- wl_seat bind 回调 --
    static void seat_bind(wl_client* client, void* data, uint32_t version, uint32_t id);

    // -- wl_seat_interface 方法 --
    static void seat_get_pointer(wl_client*, wl_resource*, uint32_t id);
    static void seat_get_keyboard(wl_client*, wl_resource*, uint32_t id);
    static void seat_get_touch(wl_client*, wl_resource*, uint32_t id);
    static void seat_release(wl_client*, wl_resource*);

    // -- Keycode 映射: OHOS KEY_* → Linux evdev (保留向后兼容, 新路径用 ArkTS KeyMap) --
    static uint32_t MapKeycode(int32_t ohosKeycode);

private:
    Seat() = default;

    // resource destructors
    static void pointer_destroy(wl_resource*);
    static void keyboard_destroy(wl_resource*);

    wl_global* global_ = nullptr;
    wl_display* display_ = nullptr;
    wl_resource* seatResource_ = nullptr;

    // pointer 资源池 (vector, mutex 保护)
    std::mutex ptrResMutex_;
    std::vector<wl_resource*> pointerResources_;
    std::atomic<int> ptrCount_{0};

    // keyboard 资源池 (vector, mutex 保护)
    std::mutex kbdResMutex_;
    std::vector<wl_resource*> keyboardResources_;
    std::atomic<int> kbdCount_{0};
};

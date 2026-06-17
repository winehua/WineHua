#include "input_manager.h"
#include "seat.h"
#include "plugin_manager.h"
#include "wayland_server.h"
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#undef LOG_TAG
#define LOG_TAG "WL_Input"
#include <hilog/log.h>

// -- Linux evdev button codes --
enum {
    BTN_LEFT   = 0x110,
    BTN_RIGHT  = 0x111,
    BTN_MIDDLE = 0x112,
};

// -- 单例 --
InputManager* InputManager::GetInstance() {
    static InputManager s;
    return &s;
}

// -- 辅助: 当前毫秒时间 --
static uint32_t NowMs() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return static_cast<uint32_t>(ms);
}

// ========================================================================
//  生命周期
// ========================================================================

void InputManager::Initialize(wl_display* display) {
    if (pipeRead_ >= 0) {
        OH_LOG_WARN(LOG_APP, "[Input] already initialized");
        return;
    }
    display_ = display;

    int fds[2];
    if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Input] pipe2 failed errno=%{public}d", errno);
        return;
    }
    pipeRead_  = fds[0];
    pipeWrite_ = fds[1];

    struct wl_event_loop* loop = wl_display_get_event_loop(display);
    pipeSource_ = wl_event_loop_add_fd(loop, pipeRead_, WL_EVENT_READABLE, OnPipeReadable, this);
    if (!pipeSource_) {
        OH_LOG_ERROR(LOG_APP, "[Input] wl_event_loop_add_fd failed");
        close(pipeRead_); close(pipeWrite_);
        pipeRead_ = pipeWrite_ = -1;
        return;
    }
    OH_LOG_INFO(LOG_APP, "[Input] initialized OK (pipe r=%{public}d w=%{public}d)", pipeRead_, pipeWrite_);
}

void InputManager::Shutdown() {
    if (pipeSource_) {
        wl_event_source_remove(pipeSource_);
        pipeSource_ = nullptr;
    }
    if (pipeRead_ >= 0)  { close(pipeRead_);  pipeRead_  = -1; }
    if (pipeWrite_ >= 0) { close(pipeWrite_); pipeWrite_ = -1; }

    // 清理状态
    pressedButtons_ = 0;
    modifiers_depressed_ = 0;
    modifiers_latched_ = 0;
    modifiers_locked_ = 0;
    modifiers_group_ = 0;
    pointerFocusedToplevel_ = 0;
    pointerFocusedSurface_ = nullptr;
    pointerEnterSerial_ = 0;
    keyboardFocusedToplevel_ = 0;
    keyboardFocusedSurface_ = nullptr;
    keyboardEntered_ = false;
    display_ = nullptr;

    OH_LOG_INFO(LOG_APP, "[Input] shutdown OK");
}

// ========================================================================
//  坐标转换
// ========================================================================

wl_fixed_t InputManager::CoordTransform(double px, double py, uint32_t tl,
                                         wl_fixed_t* outX, wl_fixed_t* outY) {
    auto* r = PluginManager::GetInstance()->GetRendererForToplevel(tl);
    if (!r) {
        OH_LOG_WARN(LOG_APP, "[Input] CoordTransform: no renderer for tl=%{public}u", tl);
        *outX = 0; *outY = 0;
        return wl_fixed_from_int(0);
    }
    int surfW = r->GetWidth();
    int surfH = r->GetHeight();
    int fw = r->GetFrameWidth();
    int fh = r->GetFrameHeight();
    int vpX = r->GetVpX();
    int vpY = r->GetVpY();
    int vpW = r->GetVpW();
    int vpH = r->GetVpH();

    if (surfW <= 0 || surfH <= 0 || fw <= 0 || fh <= 0 || vpW <= 0 || vpH <= 0) {
        *outX = 0; *outY = 0;
        return wl_fixed_from_int(0);
    }

    // Letterbox 坐标映射: (px - viewport_offset) / viewport_size * frame_size
    wl_fixed_t wx = wl_fixed_from_double((px - vpX) * fw / vpW);
    wl_fixed_t wy = wl_fixed_from_double((py - vpY) * fh / vpH);
    *outX = wx; *outY = wy;

    OH_LOG_INFO(LOG_APP, "[Input] CoordTransform px=(%{public}.0f,%{public}.0f) vp=(%{public}d,%{public}d %{public}dx%{public}d)"
                " surf=%{public}dx%{public}d frame=%{public}dx%{public}d → wine=(%{public}.0f,%{public}.0f)",
                px, py, vpX, vpY, vpW, vpH, surfW, surfH, fw, fh,
                wl_fixed_to_double(wx), wl_fixed_to_double(wy));
    return wx;
}

// ========================================================================
//  Focus 查询
// ========================================================================

bool InputManager::NeedsPointerEnter() const {
    auto* seat = Seat::GetInstance();
    // 需要 enter 当: 有 pointer resource 且没有已聚焦的 toplevel
    return seat->HasPointerResource() && pointerFocusedToplevel_.load() == 0;
}

void InputManager::ResetPointerEnter() {
    pointerFocusedToplevel_ = 0;
    pointerFocusedSurface_ = nullptr;
    pointerEnterSerial_ = 0;
    OH_LOG_INFO(LOG_APP, "[Input] ResetPointerEnter OK");
}

void InputManager::ResetKeyboardEnter() {
    keyboardEntered_ = false;
    keyboardFocusedToplevel_ = 0;
    keyboardFocusedSurface_ = nullptr;
    OH_LOG_INFO(LOG_APP, "[Input] ResetKeyboardEnter OK");
}

void InputManager::OnSurfaceDestroyed(wl_resource* surface) {
    // surface 已被 Wine 销毁, 如果仍持有引用并在后续 Inject*Leave 中使用,
    // 会导致 Wayland 协议错误 "invalid object" → Wine 断开连接
    if (pointerFocusedSurface_ == surface) {
        OH_LOG_INFO(LOG_APP, "[Input] OnSurfaceDestroyed: clearing pointer focus (surface=%{public}p was tl=%{public}u)",
                    surface, pointerFocusedToplevel_.load());
        pointerFocusedToplevel_ = 0;
        pointerFocusedSurface_ = nullptr;
        pointerEnterSerial_ = 0;
    }
    if (keyboardFocusedSurface_ == surface) {
        OH_LOG_INFO(LOG_APP, "[Input] OnSurfaceDestroyed: clearing keyboard focus (surface=%{public}p was tl=%{public}u)",
                    surface, keyboardFocusedToplevel_.load());
        keyboardEntered_ = false;
        keyboardFocusedToplevel_ = 0;
        keyboardFocusedSurface_ = nullptr;
    }
}

// ========================================================================
//  Button bitmask 辅助
// ========================================================================

unsigned InputManager::ButtonToBit(uint32_t btn) {
    switch (btn) {
        case BTN_LEFT:   return kBtnBitLeft;
        case BTN_RIGHT:  return kBtnBitRight;
        case BTN_MIDDLE: return kBtnBitMiddle;
        default:         return 99;  // unknown
    }
}

uint32_t InputManager::BitToButton(unsigned bit) {
    switch (bit) {
        case kBtnBitLeft:   return BTN_LEFT;
        case kBtnBitRight:  return BTN_RIGHT;
        case kBtnBitMiddle: return BTN_MIDDLE;
        default:            return 0;
    }
}

// ========================================================================
//  Modifier 追踪
// ========================================================================

bool InputManager::IsModifierKey(int evdevCode) {
    // evdev modifier keycodes
    switch (evdevCode) {
        case 42:  case 54:    // KEY_LEFTSHIFT, KEY_RIGHTSHIFT
        case 29:  case 97:    // KEY_LEFTCTRL, KEY_RIGHTCTRL
        case 56:  case 100:   // KEY_LEFTALT, KEY_RIGHTALT
        case 125: case 126:   // KEY_LEFTMETA, KEY_RIGHTMETA
        case 58:               // KEY_CAPSLOCK
        case 69:               // KEY_NUMLOCK
            return true;
        default:
            return false;
    }
}

void InputManager::UpdateModifiers(int evdevCode, bool pressed) {
    uint32_t bit = 0;
    switch (evdevCode) {
        case 42: case 54:   bit = (1u << 0); break;  // Shift
        case 58:            bit = (1u << 1); break;  // Caps Lock (toggle)
        case 29: case 97:   bit = (1u << 2); break;  // Ctrl
        case 56: case 100:  bit = (1u << 3); break;  // Alt
        case 69:            bit = (1u << 4); break;  // Num Lock (toggle)
        case 125: case 126: bit = (1u << 6); break;  // Super
        default: return;
    }

    if (evdevCode == 58 || evdevCode == 69) {
        // CapsLock / NumLock: toggle on each press
        if (pressed) {
            if (modifiers_locked_ & bit)
                modifiers_locked_ &= ~bit;
            else
                modifiers_locked_ |= bit;
        }
    } else {
        if (pressed)
            modifiers_depressed_ |= bit;
        else
            modifiers_depressed_ &= ~bit;
    }
}

// ========================================================================
//  NAPI 入口 (JS 线程)
// ========================================================================

void InputManager::SendPointerEvent(uint32_t tl, int action, double px, double py, int button) {
    auto* seat = Seat::GetInstance();

    // ArkTS MouseAction: Press=1, Release=2, Move=3
    // 注意: Move=3 不是 2! 旧代码曾误将此值交换导致 MOVE/RELEASE 错位
    const int ACT_PRESS   = 1;
    const int ACT_RELEASE = 2;
    const int ACT_MOVE    = 3;

    // 无 pointer resource 时所有事件跳过
    if (!seat->HasPointerResource()) return;

    // 坐标转换
    wl_fixed_t wx, wy;
    CoordTransform(px, py, tl, &wx, &wy);

    OH_LOG_INFO(LOG_APP, "[Input] PTR action=%{public}d tl=%{public}u btn=0x%{public}x px=(%{public}.0f,%{public}.0f)"
                " wine=(%{public}.0f,%{public}.0f) ptrRes=%{public}d needsEnter=%{public}d pressedBits=0x%{public}x",
                action, tl, button, px, py,
                wl_fixed_to_double(wx), wl_fixed_to_double(wy),
                seat->HasPointerResource(), NeedsPointerEnter(), pressedButtons_);

    switch (action) {
        case ACT_PRESS: {
            // 每次都发 enter: Wine 在两次点击间需要新的 pointer focus
            {
                wl_resource* surf = WaylandServer::GetInstance()->GetSurfaceForToplevel(tl);
                if (surf) {
                    if (pointerFocusedToplevel_.load() != 0 && pointerFocusedToplevel_.load() != tl)
                        Enqueue(InputEvent::PTR_LEAVE, 0, nullptr, 0, 0, 0, 0);
                    Enqueue(InputEvent::PTR_ENTER, tl, surf, wx, wy, 0, 0);
                }
            }
            Enqueue(InputEvent::PTR_MOTION, 0, nullptr, wx, wy, 0, 0);
            if (button) {
                unsigned bit = ButtonToBit(button);
                if (bit < 32) {
                    pressedButtons_ |= (1u << bit);
                    OH_LOG_INFO(LOG_APP, "[Input] BTN_PRESS btn=0x%{public}x bit=%{public}u pressedBits=0x%{public}x",
                                button, bit, pressedButtons_);
                }
                Enqueue(InputEvent::PTR_BUTTON, 0, nullptr, 0, 0, button, WL_POINTER_BUTTON_STATE_PRESSED);
            }
            break;
        }
        case ACT_RELEASE: {
            // ArkTS RELEASE 的 button 字段始终为 0x0
            // 从 pressedButtons_ bitmask 中查找被按下的按钮并释放
            unsigned bit = ButtonToBit(button);
            uint32_t releaseBtn = button;
            if (bit >= 32 && pressedButtons_) {
                // button=0 或未知按钮: 释放所有已按下的按钮
                for (unsigned b = 0; b < 3; b++) {
                    if (pressedButtons_ & (1u << b)) {
                        releaseBtn = BitToButton(b);
                        pressedButtons_ &= ~(1u << b);
                        break;
                    }
                }
            } else if (pressedButtons_ & (1u << bit)) {
                pressedButtons_ &= ~(1u << bit);
            }
            OH_LOG_INFO(LOG_APP, "[Input] BTN_RELEASE btn=0x%{public}x→0x%{public}x pressedBits=0x%{public}x",
                        button, releaseBtn, pressedButtons_);
            if (releaseBtn) {
                Enqueue(InputEvent::PTR_BUTTON, 0, nullptr, 0, 0, releaseBtn, WL_POINTER_BUTTON_STATE_RELEASED);
            }
            break;
        }
        case ACT_MOVE: {
            OH_LOG_INFO(LOG_APP, "[Input] MOVE tl=%{public}u wine=(%{public}.0f,%{public}.0f) needsEnter=%{public}d focusedTl=%{public}u ptrRes=%{public}d",
                        tl, wl_fixed_to_double(wx), wl_fixed_to_double(wy),
                        NeedsPointerEnter(), pointerFocusedToplevel_.load(), seat->HasPointerResource());
            if (NeedsPointerEnter() || pointerFocusedToplevel_.load() != tl) {
                wl_resource* surf = WaylandServer::GetInstance()->GetSurfaceForToplevel(tl);
                OH_LOG_INFO(LOG_APP, "[Input] MOVE-ENTER try surf=%{public}p for tl=%{public}u", surf, tl);
                if (surf) {
                    if (pointerFocusedToplevel_.load() != 0 && pointerFocusedToplevel_.load() != tl)
                        Enqueue(InputEvent::PTR_LEAVE, 0, nullptr, 0, 0, 0, 0);
                    Enqueue(InputEvent::PTR_ENTER, tl, surf, wx, wy, 0, 0);
                    OH_LOG_INFO(LOG_APP, "[Input] MOVE-ENTER enqueued OK");
                }
            }
            Enqueue(InputEvent::PTR_MOTION, 0, nullptr, wx, wy, 0, 0);
            break;
        }
        default:
            break;
    }
}

void InputManager::SendKeyEvent(uint32_t tl, int evdevCode, bool pressed) {
    auto* seat = Seat::GetInstance();

    OH_LOG_INFO(LOG_APP, "[Input] KEY tl=%{public}u evdev=%{public}d pressed=%{public}d"
                " kbdRes=%{public}d kbdEntered=%{public}d",
                tl, evdevCode, pressed,
                seat->GetKeyboardResource() ? 1 : 0,
                keyboardEntered_.load());

    // 键盘 enter 管理: 立即设置状态防止重复 enter (参考旧代码)
    if (pressed && (!keyboardEntered_.load() || keyboardFocusedToplevel_.load() != tl)) {
        wl_resource* surf = WaylandServer::GetInstance()->GetSurfaceForToplevel(tl);
        if (surf) {
            if (keyboardEntered_.load() && keyboardFocusedToplevel_.load() != tl) {
                Enqueue(InputEvent::KBD_LEAVE, 0, nullptr, 0, 0, 0, 0);
            }
            // 立即设置状态, 避免 NAPI 线程在 flush 前又发一次 enter
            keyboardFocusedToplevel_ = tl;
            keyboardFocusedSurface_ = surf;
            keyboardEntered_ = true;
            Enqueue(InputEvent::KBD_ENTER, tl, surf, 0, 0, 0, 0);
            // 发送初始 modifier 状态
            InputEvent mev;
            mev.type = InputEvent::KBD_MODIFIERS;
            mev.mod_depressed = modifiers_depressed_;
            mev.mod_latched = modifiers_latched_;
            mev.mod_locked = modifiers_locked_;
            mev.mod_group = modifiers_group_;
            {
                std::lock_guard<std::mutex> lk(queueMutex_);
                queue_.push_back(mev);
            }
        }
    }

    // 追踪 modifier 状态
    if (IsModifierKey(evdevCode)) {
        UpdateModifiers(evdevCode, pressed);
    }

    // 入队 key 事件
    uint32_t state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    Enqueue(InputEvent::KBD_KEY, 0, nullptr, 0, 0, evdevCode, state);
}

// ========================================================================
//  事件队列 (JS 线程 → Wayland 线程)
// ========================================================================

void InputManager::Enqueue(InputEvent::Type type, uint32_t tl, wl_resource* surface,
                            wl_fixed_t x, wl_fixed_t y, uint32_t btn_or_key, uint32_t state) {
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.push_back({type, tl, surface, x, y, btn_or_key, state});
    }
    // 唤醒 Wayland 线程
    if (pipeWrite_ >= 0) {
        char c = 1;
        ssize_t n = write(pipeWrite_, &c, 1);
        if (n < 0 && errno != EAGAIN) {
            OH_LOG_WARN(LOG_APP, "[Input] pipe write FAIL errno=%{public}d", errno);
        }
    }
}

int InputManager::OnPipeReadable(int fd, uint32_t mask, void* data) {
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {}
    static_cast<InputManager*>(data)->FlushQueue();
    return 0;
}

void InputManager::FlushQueue() {
    // Wayland 线程: 取出所有事件并发送
    std::vector<InputEvent> batch;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        batch.swap(queue_);
    }
    if (batch.empty()) return;

    // 去重
    std::vector<InputEvent> merged;
    for (auto& ev : batch) {
        if (!merged.empty()) {
            auto& last = merged.back();
            if (last.type == ev.type) {
                bool skip = false;
                switch (ev.type) {
                    case InputEvent::PTR_BUTTON:
                        skip = (last.btn_or_key == ev.btn_or_key && last.state == ev.state);
                        break;
                    case InputEvent::PTR_MOTION:
                        last = ev; continue;  // 只保留最后一个
                    case InputEvent::PTR_ENTER:
                        skip = (last.tl == ev.tl && last.surface == ev.surface);
                        break;
                    case InputEvent::KBD_KEY:
                        skip = (last.btn_or_key == ev.btn_or_key && last.state == ev.state);
                        break;
                    default: break;
                }
                if (skip) continue;
            }
        }
        merged.push_back(ev);
    }
    if (merged.size() != batch.size()) {
        OH_LOG_INFO(LOG_APP, "[Input] dedup %{public}zu→%{public}zu", batch.size(), merged.size());
    }

    for (auto& ev : merged) {
        switch (ev.type) {
            case InputEvent::PTR_ENTER:   InjectPointerEnter(ev.tl, ev.surface, ev.x, ev.y); break;
            case InputEvent::PTR_LEAVE:   InjectPointerLeave(); break;
            case InputEvent::PTR_MOTION:
                OH_LOG_INFO(LOG_APP, "[Input] Flush-MOTION sx=%{public}.1f sy=%{public}.1f",
                            wl_fixed_to_double(ev.x), wl_fixed_to_double(ev.y));
                InjectPointerMotion(ev.x, ev.y); break;
            case InputEvent::PTR_BUTTON:  InjectPointerButton(ev.btn_or_key, ev.state); break;
            case InputEvent::KBD_ENTER:   InjectKeyboardEnter(ev.tl, ev.surface); break;
            case InputEvent::KBD_LEAVE:   InjectKeyboardLeave(); break;
            case InputEvent::KBD_KEY:     InjectKeyboardKey(ev.btn_or_key, ev.state); break;
            case InputEvent::KBD_MODIFIERS: InjectKeyboardModifiers(ev.mod_depressed, ev.mod_latched, ev.mod_locked, ev.mod_group); break;
        }
    }
    if (display_) {
        wl_display_flush_clients(display_);
    }
}

// ========================================================================
//  事件注入 (Wayland 线程, 调用 wl_*_send_*)
// ========================================================================

void InputManager::InjectPointerEnter(uint32_t tl, wl_resource* surface, wl_fixed_t sx, wl_fixed_t sy) {
    auto* seat = Seat::GetInstance();
    auto ptrs = seat->GetAllPointerResources();
    if (ptrs.empty() || !surface) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectEnter DROP nPtrs=%{public}zu surf=%{public}p", ptrs.size(), surface);
        return;
    }

    // 防御: surface 可能在入队后到 flush 前被 Wine 销毁
    // 通过 toplevelSurfaceMap_ 验证 surface 仍然有效
    if (!WaylandServer::GetInstance()->GetSurfaceForToplevel(tl)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectEnter DROP tl=%{public}u: surface no longer in map (destroyed before flush?)", tl);
        return;
    }

    pointerFocusedToplevel_ = tl;
    pointerFocusedSurface_ = surface;
    uint32_t s = serial_++;
    pointerEnterSerial_ = s;
    int nSent = 0;
    struct wl_client* surfClient = wl_resource_get_client(surface);
    OH_LOG_INFO(LOG_APP, "[Input] InjectEnter tl=%{public}u serial=%{public}u sx=%{public}.1f sy=%{public}.1f nPtrs=%{public}zu",
                tl, s, wl_fixed_to_double(sx), wl_fixed_to_double(sy), ptrs.size());
    for (auto* ptr : ptrs) {
        // 安全检查: surface 必须与 pointer 属于同一 client (防止跨客户端错误)
        if (ptr && wl_resource_get_client(ptr) == surfClient) {
            wl_pointer_send_enter(ptr, s, surface, sx, sy);
            wl_pointer_send_frame(ptr);
            nSent++;
        }
    }
    OH_LOG_INFO(LOG_APP, "[Input] InjectEnter OK sent=%{public}d", nSent);
}

void InputManager::InjectPointerMotion(wl_fixed_t sx, wl_fixed_t sy) {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    if (ptrs.empty()) return;
    for (auto* ptr : ptrs) {
        if (ptr) {
            wl_pointer_send_motion(ptr, NowMs(), sx, sy);
            wl_pointer_send_frame(ptr);
        }
    }
    OH_LOG_INFO(LOG_APP, "[Input] InjectMotion sx=%{public}.1f sy=%{public}.1f OK n=%{public}zu",
                wl_fixed_to_double(sx), wl_fixed_to_double(sy), ptrs.size());
}

void InputManager::InjectPointerButton(uint32_t button, uint32_t state) {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    if (ptrs.empty()) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectButton DROP btn=0x%{public}x: no ptr", button);
        return;
    }
    // 使用最近一次 enter 的 serial (Wayland 协议要求 button 序列号与 enter 一致)
    uint32_t enterSerial = pointerEnterSerial_.load();
    uint32_t s = enterSerial ? enterSerial : serial_++;
    OH_LOG_INFO(LOG_APP, "[Input] InjectButton btn=0x%{public}x state=%{public}u serial=%{public}u (enterSerial=%{public}u) n=%{public}zu",
                button, state, s, enterSerial, ptrs.size());
    for (auto* ptr : ptrs) {
        if (ptr) {
            wl_pointer_send_button(ptr, s, NowMs(), button, state);
            wl_pointer_send_frame(ptr);
        }
    }
}

void InputManager::InjectPointerLeave() {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    wl_resource* surf = pointerFocusedSurface_;
    if (ptrs.empty() || !surf) return;
    uint32_t s = serial_++;
    struct wl_client* surfClient = wl_resource_get_client(surf);
    for (auto* ptr : ptrs) {
        if (ptr && wl_resource_get_client(ptr) == surfClient) {
            wl_pointer_send_leave(ptr, s, surf);
        }
    }
    pointerFocusedToplevel_ = 0;
    pointerFocusedSurface_ = nullptr;
    pointerEnterSerial_ = 0;
}

void InputManager::InjectKeyboardEnter(uint32_t tl, wl_resource* surface) {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    if (kbds.empty() || !surface) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKbdEnter DROP nKbds=%{public}zu surf=%{public}p", kbds.size(), surface);
        return;
    }

    // 防御: surface 可能在入队后到 flush 前被 Wine 销毁
    if (!WaylandServer::GetInstance()->GetSurfaceForToplevel(tl)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKbdEnter DROP tl=%{public}u: surface no longer in map (destroyed before flush?)", tl);
        return;
    }

    keyboardFocusedToplevel_ = tl;
    keyboardFocusedSurface_ = surface;
    keyboardEntered_ = true;
    uint32_t s = serial_++;
    int nSent = 0;
    struct wl_client* surfClient = wl_resource_get_client(surface);
    OH_LOG_INFO(LOG_APP, "[Input] InjectKbdEnter tl=%{public}u serial=%{public}u mods=0x%{public}x nKbds=%{public}zu",
                tl, s, modifiers_depressed_, kbds.size());

    for (auto* kbd : kbds) {
        if (!kbd) continue;
        // 安全检查: surface 必须与 keyboard 属于同一 client
        if (wl_resource_get_client(kbd) != surfClient) continue;
        wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(kbd, s, surface, &keys);
        wl_array_release(&keys);

        // 发送当前 modifier 状态
        wl_keyboard_send_modifiers(kbd, serial_++, modifiers_depressed_, modifiers_latched_,
                                   modifiers_locked_, modifiers_group_);
        nSent++;
    }
    OH_LOG_INFO(LOG_APP, "[Input] InjectKbdEnter OK sent=%{public}d", nSent);
}

void InputManager::InjectKeyboardKey(uint32_t key, uint32_t state) {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    if (kbds.empty()) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKey DROP evdev=%{public}u: no kbd", key);
        return;
    }
    uint32_t s = serial_++;
    int nSent = 0;
    struct wl_client* focusClient = keyboardFocusedSurface_ ? wl_resource_get_client(keyboardFocusedSurface_) : nullptr;
    for (auto* kbd : kbds) {
        if (kbd) {
            // 只发给已 enter 的 client (与 InjectKbdEnter 一致), 避免无 focused_hwnd 的 client 收到无效 key
            if (focusClient && wl_resource_get_client(kbd) == focusClient) {
                wl_keyboard_send_key(kbd, s, NowMs(), key, state);
                nSent++;
            }
        }
    }
    if (nSent == 0) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKey DROP evdev=%{public}u: no kbd with focus (nTotal=%{public}zu)", key, kbds.size());
    } else {
        OH_LOG_INFO(LOG_APP, "[Input] InjectKey evdev=%{public}u state=%{public}u serial=%{public}u sent=%{public}d",
                    key, state, s, nSent);
    }
}

void InputManager::InjectKeyboardLeave() {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    wl_resource* surf = keyboardFocusedSurface_;
    if (kbds.empty() || !keyboardEntered_.load() || !surf) return;
    uint32_t s = serial_++;
    struct wl_client* surfClient = wl_resource_get_client(surf);
    for (auto* kbd : kbds) {
        if (kbd && wl_resource_get_client(kbd) == surfClient) {
            wl_keyboard_send_leave(kbd, s, surf);
        }
    }
    keyboardEntered_ = false;
    keyboardFocusedToplevel_ = 0;
    keyboardFocusedSurface_ = nullptr;
    OH_LOG_INFO(LOG_APP, "[Input] InjectKbdLeave OK");
}

void InputManager::InjectKeyboardModifiers(uint32_t depressed, uint32_t latched,
                                            uint32_t locked, uint32_t group) {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    if (kbds.empty()) return;
    uint32_t s = serial_++;
    for (auto* kbd : kbds) {
        if (kbd) {
            wl_keyboard_send_modifiers(kbd, s, depressed, latched, locked, group);
        }
    }
}

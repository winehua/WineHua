#include "compositor/input_state_tracker.h"

// ============================================================================
// InputStateTracker 实现 — 原 InputManager 状态逻辑逐字平移 (重构第 4C2 步)。
// 本文件零外部依赖 (不 include wayland/hilog), 可由 host_tests 宿主 g++
// 直连编译 (make test), 见 input_state_test.cpp。
// ============================================================================

// -- Linux evdev button codes (与旧 input_manager.cpp 同值) --
namespace {
constexpr uint32_t kBtnLeft   = 0x110;
constexpr uint32_t kBtnRight  = 0x111;
constexpr uint32_t kBtnMiddle = 0x112;
} // namespace

// ============================================================================
//  Button bitmask 辅助 (原 InputManager::ButtonToBit/BitToButton 逐字)
// ============================================================================

unsigned InputStateTracker::ButtonToBit(uint32_t btn) const {
    switch (btn) {
        case kBtnLeft:   return kBtnBitLeft;
        case kBtnRight:  return kBtnBitRight;
        case kBtnMiddle: return kBtnBitMiddle;
        default:         return 99;  // unknown
    }
}

uint32_t InputStateTracker::BitToButton(unsigned bit) const {
    switch (bit) {
        case kBtnBitLeft:   return kBtnLeft;
        case kBtnBitRight:  return kBtnRight;
        case kBtnBitMiddle: return kBtnMiddle;
        default:            return 0;
    }
}

void InputStateTracker::OnButtonPress(uint32_t btn) {
    // 旧 ACT_PRESS: "if (button) { bit = ButtonToBit; if (bit < 32) pressedButtons_ |= (1u<<bit) }"
    unsigned bit = ButtonToBit(btn);
    if (bit < 32)
        pressedButtons_ |= (1u << bit);
}

uint32_t InputStateTracker::OnButtonRelease(uint32_t btn) {
    // 旧 ACT_RELEASE 逐字: button 字段来自 ArkTS (始终 0x0 表示"未知/全部"),
    // 从 bitmask 找第一个按下的位释放 (查找顺序 bit0→bit2); 指定按键则只释放
    // 该位。releaseBtn 初值 = 传参 button — 未按下的指定键也返回其键码,
    // 编排层 releaseBtn 非 0 即入队 RELEASED (既有行为, 不修正)。
    unsigned bit = ButtonToBit(btn);
    uint32_t releaseBtn = btn;
    if (bit >= 32 && pressedButtons_) {
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
    return releaseBtn;
}

// ============================================================================
//  Modifier 追踪 (原 InputManager::UpdateModifiers/IsModifierKey 逐字)
// ============================================================================

bool InputStateTracker::IsModifierKey(int evdevCode) const {
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

void InputStateTracker::UpdateModifiers(int evdevCode, bool pressed) {
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
            if (modifiersLocked_ & bit)
                modifiersLocked_ &= ~bit;
            else
                modifiersLocked_ |= bit;
        }
    } else {
        if (pressed)
            modifiersDepressed_ |= bit;
        else
            modifiersDepressed_ &= ~bit;
    }
}

// ============================================================================
//  pointer/keyboard 焦点
// ============================================================================

void InputStateTracker::SetPointerFocus(uint32_t tl, wl_resource* surface, uint32_t serial) {
    // 写序与旧 InjectPointerEnter 逐字: 先 tl, 后 surface, 最后 enter serial
    pointerFocusedToplevel_.store(tl);
    pointerFocusedSurface_.store(surface);
    pointerEnterSerial_.store(serial);
}

void InputStateTracker::ClearPointerFocus() {
    // 清序与旧 ResetPointerEnter/InjectPointerLeave 逐字
    pointerFocusedToplevel_.store(0);
    pointerFocusedSurface_.store(nullptr);
    pointerEnterSerial_.store(0);
}

void InputStateTracker::SetKeyboardFocus(uint32_t tl, wl_resource* surface) {
    // 旧代码两处写入此三元组 (PRESS 立即 / SendKeyEvent 立即) — 逐字搬移
    keyboardFocusedToplevel_.store(tl);
    keyboardFocusedSurface_ = surface;
    keyboardEntered_.store(true);
}

void InputStateTracker::ClearKeyboardFocus() {
    keyboardEntered_.store(false);
    keyboardFocusedToplevel_.store(0);
    keyboardFocusedSurface_ = nullptr;
}

// ============================================================================
//  窗口可见性 (原 visibleMutex_ + toplevelVisible_ 逐字)
// ============================================================================

void InputStateTracker::SetToplevelVisible(uint32_t tl, bool visible) {
    std::lock_guard<std::mutex> lk(visibleMutex_);
    toplevelVisible_[tl] = visible;
}

bool InputStateTracker::IsInputSuppressed(uint32_t tl) const {
    std::lock_guard<std::mutex> lk(visibleMutex_);
    auto it = toplevelVisible_.find(tl);
    return it != toplevelVisible_.end() && !it->second;
}

void InputStateTracker::ClearVisible() {
    std::lock_guard<std::mutex> lk(visibleMutex_);
    toplevelVisible_.clear();
}

// ============================================================================
//  相对增量基线 (原 lastLocalX_/lastLocalY_/hasLastLocal_), 仅 NAPI 线程访问
// ============================================================================

void InputStateTracker::UpdateLastLocal(double x, double y) {
    lastLocalX_ = x;
    lastLocalY_ = y;
    hasLastLocal_ = true;
}

void InputStateTracker::ResetLastLocal() {
    lastLocalX_ = 0;
    lastLocalY_ = 0;
    hasLastLocal_ = false;
}

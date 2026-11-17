#include "compositor/input/input_state_tracker.h"

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
    unsigned index;
    switch (evdevCode) {
        case 42: index = 0; break; case 54: index = 1; break;
        case 29: index = 2; break; case 97: index = 3; break;
        case 56: index = 4; break; case 100: index = 5; break;
        case 125: index = 6; break; case 126: index = 7; break;
        case 58: index = 8; break; case 69: index = 9; break;
        default: return;
    }
    const uint32_t key = 1u << index;
    const bool wasPressed = (modifierKeys_ & key) != 0;
    if (pressed) modifierKeys_ |= key;
    else modifierKeys_ &= ~key;
    if (pressed && !wasPressed && index >= 8)
        modifiersLocked_ ^= 1u << (index == 8 ? 1 : 4);
    modifiersDepressed_ = 0;
    if (modifierKeys_ & 0x03) modifiersDepressed_ |= 1u << 0;
    if (modifierKeys_ & 0x0c) modifiersDepressed_ |= 1u << 2;
    if (modifierKeys_ & 0x30) modifiersDepressed_ |= 1u << 3;
    if (modifierKeys_ & 0xc0) modifiersDepressed_ |= 1u << 6;
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
    // Record a successfully delivered enter, on the dispatch thread.
    keyboardFocusedToplevel_.store(tl);
    keyboardFocusedSurface_.store(surface);
    keyboardEntered_.store(true);
}

void InputStateTracker::ClearKeyboardFocus() {
    keyboardEntered_.store(false);
    keyboardFocusedToplevel_.store(0);
    keyboardFocusedSurface_.store(nullptr);
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

#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

struct wl_resource;  // 只存指针做身份比较, 不做任何操作 (宿主单测可用假指针)

// ============================================================================
// InputStateTracker — 输入状态追踪纯状态类 (重构第 4C2 步从 InputManager 抽离)
//
// 背景 (docs/COMPOSITOR_REFACTOR_PLAN.md 阶段 4): InputManager 拆四层的
// 状态层 — button bitmask / modifier 状态 / pointer+keyboard 焦点 / 窗口
// 可见性抑制 / serial 计数 / 相对增量基线 / 最近按下时刻, 全部收为标量
// 状态。设计约束 (与 env_spec.h 同模式): **不依赖 wayland 头与客户机资源** —
// wl_resource 等 opaque 句柄一律只存指针做身份比较, wl_fixed 类型改用统一
// 的 double/int32_t; 新文件不放 wayland include, host_tests
// (input_state_test.cpp) 用宿主 g++ -I entry/src/main/cpp 直连编译。
//
// 线程模型 (与旧 InputManager 字段声明逐字对应, 不收紧竞态窗口):
//   - NAPI/JS 线程写: buttons/modifiers/baseline/pressMs/visible
//     (Send*Event 路径); SetPointerFocus/SetKeyboardFocus 有 NAPI 线程写入
//     (PRESS/SendKeyEvent 的"立即设置状态"段), 与 Wayland 线程 (Inject*)
//     写入同一批 atomic 字段 — atomic 保留旧语义 (见各字段注释)。
//   - Wayland 线程写: SetPointerFocus/ClearPointerFocus (Inject*Enter/Leave)、
//     SetKeyboardFocus/ClearKeyboardFocus、NextSerial。
//   - 无内部锁 (visibleMutex_ 除外 — 可见性表旧实现自带互斥锁, 保持原锁边界)。
//
// 行为平价承诺: 本类方法体 = 旧 InputManager 内联状态逻辑逐字平移, 不引入
// 任何新语义 (toggle/查找首按下位/skip 判定/comparison 全部原样)。
// ============================================================================

class InputStateTracker {
public:
    // -- 按钮位掩码常量 (与旧 InputManager::kBtnBit* 同值) --
    // bit0=left(0x110), bit1=right(0x111), bit2=middle(0x112)
    static constexpr unsigned kBtnBitLeft   = 0;
    static constexpr unsigned kBtnBitRight  = 1;
    static constexpr unsigned kBtnBitMiddle = 2;

    // -- 按钮位掩码 (原 ButtonToBit/BitToButton + pressedButtons_) --
    unsigned ButtonToBit(uint32_t btn) const;
    uint32_t BitToButton(unsigned bit) const;
    uint32_t PressedButtons() const { return pressedButtons_; }
    // 置位: bit>=32 时忽略 (unknown/0 — 旧 ACT_PRESS "if (bit < 32)" 语义);
    // 编排层日志经 PressedButtons() 读更新后的掩码
    void OnButtonPress(uint32_t btn);
    // 释放: 语义与旧 ACT_RELEASE 完全一致 — button=0/未知 时从 bitmask 找
    // 第一个按下的位释放 (查找顺序 bit0→bit2), 指定按键则只释放该位; **未
    // 按下的指定键也返回其键码** (旧 releaseBtn 初值=传参 button, 编排层
    // releaseBtn 非 0 即入队 — 该"多发一次释放"是既有行为, 不修正)。
    // 返回值 = 编排层实际应注入释放的键码 (0 = 无键可释放)。
    uint32_t OnButtonRelease(uint32_t btn);
    void ResetButtons() { pressedButtons_ = 0; }

    // -- modifier 状态 (原 UpdateModifiers/IsModifierKey 逐字) --
    bool IsModifierKey(int evdevCode) const;
    void UpdateModifiers(int evdevCode, bool pressed);
    uint32_t ModifiersDepressed() const { return modifiersDepressed_; }
    uint32_t ModifiersLatched() const { return modifiersLatched_; }
    uint32_t ModifiersLocked() const { return modifiersLocked_; }
    uint32_t ModifiersGroup() const { return modifiersGroup_; }
    void ResetModifiers() { modifiersDepressed_ = 0; modifiersLatched_ = 0; modifiersLocked_ = 0; modifiersGroup_ = 0; }

    // -- pointer 焦点 (原子字段语义同旧: NAPI 读做 surface 级 enter/leave 判定,
    //    Wayland 线程 Write 注入) --
    bool HasPointerFocus() const { return pointerFocusedToplevel_.load() != 0; }
    // = NeedsPointerEnter 判定 (原 input_manager.cpp: seat 有资源且无焦点):
    // 组合了 Seat 状态为参数 (编排层查询后传入 — tracker 不认识 Seat)
    bool PointerNeedsEnter(bool hasPointerResource) const {
        return hasPointerResource && pointerFocusedToplevel_.load() == 0;
    }
    uint32_t PointerFocusedToplevel() const { return pointerFocusedToplevel_.load(); }
    wl_resource* PointerFocusedSurface() const { return pointerFocusedSurface_.load(); }
    // PRESS skipEnter 守卫: 指针已聚焦该 surface (相对模式重定位被跳过 ⇔ identity 相等)
    bool PointerFocusedSurfaceIs(wl_resource* s) const { return pointerFocusedSurface_.load() == s; }
    uint32_t PointerEnterSerial() const { return pointerEnterSerial_.load(); }
    void SetPointerFocus(uint32_t tl, wl_resource* surface, uint32_t serial);
    void ClearPointerFocus();

    // -- keyboard 焦点 (kbdFocusTL/surface/entered — 与旧字段同非 atomic 性:
    //    仅有 NAPI/Wayland 双线程写, 旧代码同样无锁) --
    bool KeyboardEntered() const { return keyboardEntered_.load(); }
    uint32_t KeyboardFocusedToplevel() const { return keyboardFocusedToplevel_.load(); }
    wl_resource* KeyboardFocusedSurface() const { return keyboardFocusedSurface_; }
    void SetKeyboardFocus(uint32_t tl, wl_resource* surface);
    void ClearKeyboardFocus();

    // -- 窗口可见性 (输入抑制; 原 visibleMutex_ + toplevelVisible_) --
    void SetToplevelVisible(uint32_t tl, bool visible);
    // 缺省语义: 未登记 = 不抑制 (Find 不到条目时放行 — 旧 if 逐字)
    bool IsInputSuppressed(uint32_t tl) const;
    void ClearVisible();

    // -- serial 计数 (原 serial_, 初值 1; Reset* 路径不清 — 跨会话单调) --
    // fetch_add 返回旧值 (与 serial_++ 一致); pointer 与 keyboard 共用
    uint32_t NextSerial() { return serial_.fetch_add(1); }

    // -- 相对增量基线 (surface 局部坐标; 仅 SendPointerEvent 所在 NAPI 线程
    //    访问, 无锁 — 旧实现同样无锁) --
    bool HasLastLocal() const { return hasLastLocal_; }
    double LastLocalX() const { return lastLocalX_; }
    double LastLocalY() const { return lastLocalY_; }
    void UpdateLastLocal(double x, double y);
    void ResetLastLocal();

    // -- 最近按下时刻 (ACT_RELEASE 脉冲拉伸计时, 旧 lastPressMs_ atomic) --
    uint32_t LastPressMs() const { return lastPressMs_.load(); }
    void SetLastPressMs(uint32_t ms) { lastPressMs_.store(ms); }
    void ResetLastPressMs() { lastPressMs_.store(0); }

private:
    // 与旧 InputManager 存储逐字对应 (含 atomic 类别与初值 — 竞态窗口语义不变)
    uint32_t pressedButtons_ = 0;
    // modifier state (原四个非 atomic 字段: NAPI 线程独占写; Wayland 线程
    // 仅在 InjectKbdEnter 日志读 dirty 值 — 与旧实现相同的无害竞态)
    uint32_t modifiersDepressed_ = 0;
    uint32_t modifiersLatched_ = 0;
    uint32_t modifiersLocked_ = 0;
    uint32_t modifiersGroup_ = 0;

    std::atomic<uint32_t> pointerFocusedToplevel_{0};
    std::atomic<wl_resource*> pointerFocusedSurface_{nullptr};
    std::atomic<uint32_t> pointerEnterSerial_{0};
    std::atomic<uint32_t> serial_{1};

    std::atomic<uint32_t> keyboardFocusedToplevel_{0};
    wl_resource* keyboardFocusedSurface_ = nullptr;
    std::atomic<bool> keyboardEntered_{false};

    // 窗口可见性表 (原 InputManager 自带互斥锁, 保持原锁边界; mutable —
    // const 查询 IsInputSuppressed 也要锁, 与 pointer_extras.h 同模式)
    mutable std::mutex visibleMutex_;
    std::unordered_map<uint32_t, bool> toplevelVisible_;

    double lastLocalX_ = 0, lastLocalY_ = 0;
    bool hasLastLocal_ = false;

    std::atomic<uint32_t> lastPressMs_{0};
};

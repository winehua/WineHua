#include "input_manager.h"
#include "seat.h"
#include "plugin_manager.h"
#include "pointer_extras.h"
#include "wayland_server.h"
#include <chrono>
#include <thread>
#include <atomic>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Input"
#include <hilog/log.h>

// -- Linux evdev button codes --
enum {
    BTN_LEFT   = 0x110,
    BTN_RIGHT  = 0x111,
    BTN_MIDDLE = 0x112,
};

// -- 丢帧统计 (全局计数器 + 周期性汇总, 60s 间隔) --
static std::atomic<int> gDropEnter{0}, gDropButton{0}, gDropKey{0}, gDropMotion{0};
static std::atomic<uint64_t> gLastDropReport{0};

static void MaybeReportDrops() {
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last = gLastDropReport.load();
    if (now - last > 60000) {  // 每 60 秒最多报一次
        if (gLastDropReport.compare_exchange_strong(last, now)) {
            OH_LOG_WARN(LOG_APP,
                "[Input-DROP] 60s summary: enter=%{public}d button=%{public}d key=%{public}d motion=%{public}d",
                gDropEnter.exchange(0), gDropButton.exchange(0),
                gDropKey.exchange(0), gDropMotion.exchange(0));
        }
    }
}

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

// 静止 tap 的最小按压时长: ≥2 个 PAL2 轮询帧 (~55ms/帧 @18fps),
// 保证 down 落在与 up 不同的 GetDeviceState 轮询窗口 (见 ACT_RELEASE)
static constexpr uint32_t kMinPressDurationMs = 100;

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
    // Desktop 模式 fallback: root 切换后可能用旧 ID 查 renderer
    if (!r && WaylandServer::GetInstance()->Policy().RootCompositing()) {
        uint32_t rootId = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
        if (rootId != tl) r = PluginManager::GetInstance()->GetRendererForToplevel(rootId);
    }
    if (!r) {
        OH_LOG_WARN(LOG_APP, "[Input] CoordTransform: no renderer for tl=%{public}u", tl);
        *outX = 0; *outY = 0;
        return wl_fixed_from_int(0);
    }
    int surfW = r->GetWidth();
    int surfH = r->GetHeight();
    const FitRect& lb = r->GetLetterbox();

    if (surfW <= 0 || surfH <= 0 || lb.dstW <= 0 || lb.dstH <= 0) {
        *outX = 0; *outY = 0;
        return wl_fixed_from_int(0);
    }

    // Letterbox 逆映射 (geometry.h 统一实现): 物理像素 → 去黑边 → 按帧尺寸缩放
    // 注意用取整后 dst 尺寸的变体 — 与 glViewport 实际显示的整数像素严格一致
    wl_fixed_t wx = wl_fixed_from_double(FitUnmapDisplayX(lb, px));
    wl_fixed_t wy = wl_fixed_from_double(FitUnmapDisplayY(lb, py));
    *outX = wx; *outY = wy;

    OH_LOG_DEBUG(LOG_APP, "[Input] CoordTransform px=(%{public}.0f,%{public}.0f) vp=(%{public}d,%{public}d %{public}dx%{public}d)"
                 " surf=%{public}dx%{public}d frame=%{public}dx%{public}d → wine=(%{public}.0f,%{public}.0f)",
                 px, py, lb.offX, lb.offY, lb.dstW, lb.dstH, surfW, surfH, lb.srcW, lb.srcH,
                 wl_fixed_to_double(wx), wl_fixed_to_double(wy));
    return wx;
}

// ========================================================================
//  指针 warp (wp_pointer_warp_v1) — Wayland 线程调用
// ========================================================================

void InputManager::OnPointerWarp(wl_resource* surface, double sx, double sy) {
    auto* ws = WaylandServer::GetInstance();
    // SetCursorPos 的位置同步。wineserver 光标已在 wine 侧移动到位, host
    // 不需要注入 motion: 绝对模式 (RTS 等) 下一次设备事件自然覆盖, 相对
    // 模式 wine 拒绝 SetCursorPos (wayland_pointer.c:1024) 不会发本请求。
    // 这里只把 move grab 的偏移基准同步到 warp 位置。
    double lx = sx, ly = sy;
    if (ws->IsDesktopMode()) {
        if (!ws->SurfaceLocalToDesktop(surface, sx, sy, lx, ly)) {
            OH_LOG_WARN(LOG_APP, "[Input] WARP sync failed: surf=%{public}p not mapped",
                        static_cast<void*>(surface));
            return;
        }
    }
    lastGlobalPtrX_.store(wl_fixed_from_double(lx));
    lastGlobalPtrY_.store(wl_fixed_from_double(ly));
    static uint32_t sWarpN = 0;
    if (++sWarpN % 120 == 1)
        OH_LOG_INFO(LOG_APP, "[Input] WARP pos=(%{public}.1f,%{public}.1f) desktop=%{public}d n=%{public}u",
                    lx, ly, ws->IsDesktopMode() ? 1 : 0, sWarpN);
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

void InputManager::ResetSessionState() {
    // Wine 会话结束后的全量状态复位。残留风险: 焦点指向已销毁的 toplevel;
    // 按下/修饰键残留会让新会话卡键 (会话结束时 Ctrl 按着, 新会话所有按键
    // 都带 Ctrl); 指针位置/相对增量基线污染新会话首次操作。只清状态不发
    // 事件 — client 已断开, send 到已销毁 surface 会触发协议错误。
    ResetPointerEnter();
    ResetKeyboardEnter();
    pressedButtons_ = 0;
    modifiers_depressed_ = 0;
    modifiers_latched_ = 0;
    modifiers_locked_ = 0;
    modifiers_group_ = 0;
    lastGlobalPtrX_ = 0;
    lastGlobalPtrY_ = 0;
    lastLocalX_ = 0;
    lastLocalY_ = 0;
    hasLastLocal_ = false;
    lastPressMs_ = 0;
    {
        std::lock_guard<std::mutex> lock(visibleMutex_);
        toplevelVisible_.clear();
    }
    OH_LOG_INFO(LOG_APP, "[Input] session state reset (focus/buttons/modifiers/position/visible)");
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

void InputManager::SetToplevelVisible(uint32_t tl, bool visible) {
    std::lock_guard<std::mutex> lk(visibleMutex_);
    toplevelVisible_[tl] = visible;
    OH_LOG_INFO(LOG_APP, "[Input] SetToplevelVisible tl=%{public}u visible=%{public}s", tl, visible ? "true" : "false");
}

void InputManager::SendPointerEvent(uint32_t tl, int action, double px, double py, int button) {
    // 窗口不可见时抑制输入
    {
        std::lock_guard<std::mutex> lk(visibleMutex_);
        auto it = toplevelVisible_.find(tl);
        if (it != toplevelVisible_.end() && !it->second) return;
    }

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
    auto* ws = WaylandServer::GetInstance();
    // Desktop 模式: 按桌面坐标解析精确输入目标 (菜单 subsurface 有自己的
    // wl_surface, 必须 enter 它并用层相对坐标 — 经父窗口 surface 的越界
    // 坐标会被 winewayland 的 motion clamp 夹回窗口内, 菜单伸出部分点不中)
    wl_resource* targetSurf = nullptr;
    if (ws->Policy().CompositorRoutesInput() && tl != ws->GetDesktopRootToplevelId()) {
        CoordTransform(px, py, ws->GetDesktopRootToplevelId(), &wx, &wy);
        // 记录最近一次注入的桌面全局指针位置 (grab 建立时算固定偏移用)
        lastGlobalPtrX_.store(wx);
        lastGlobalPtrY_.store(wy);
        // move grab 期间 (xdg_toplevel.move): compositor 用桌面全局坐标绝对定位
        // 被拖窗口, motion 必须注入全局坐标。局部坐标往返 (enqueue 时
        // local = logical - st->x, 消费时再 + st->x 还原) 在两个线程间基准
        // 漂移: 消费时刻的 st->x 已变, rx 多出"窗口自身刚移动的量"并叠进
        // 下一帧位移 → 快速拖动时窗口位移逐帧累积放大, 窗口瞬间飞出屏幕
        if (action == ACT_MOVE && ws->IsMoveGrabActive() &&
            ws->GetMoveGrabToplevelId() == tl) {
            Enqueue(InputEvent::PTR_MOTION, 0, nullptr, wx, wy, 0, 0);
            return;
        }
        double logicalX = wl_fixed_to_double(wx);
        double logicalY = wl_fixed_to_double(wy);
        WaylandServer::InputTarget target;
        if (ws->FindInputTargetAt(static_cast<int>(lround(logicalX)),
                                  static_cast<int>(lround(logicalY)), target)) {
            // 全屏黑边: 只吞 PRESS (防幻影点击/焦点切换)。MOVE/RELEASE 照常透传 —
            // 越界坐标由 winewayland 的 motion clamp 夹回窗口边缘;
            // 吞掉 RELEASE 会让 pressedButtons_ 永不清位 (按键卡死)
            if (target.swallow && action == ACT_PRESS) return;
            tl = target.toplevelId;
            targetSurf = target.surface;
            // 桌面坐标 → surface 局部坐标 (即 geometry.h 的 FitUnmapX/Y;
            // target.origin/scale 由 InputResolver 的 ComputeFitRect 给出)。
            // target.scale > 1 表示全屏窗口保比例放大显示, 局部坐标需按同一缩放除回来
            const double localX = (logicalX - target.originX) / target.scale;
            const double localY = (logicalY - target.originY) / target.scale;
            wx = wl_fixed_from_double(localX);
            wy = wl_fixed_from_double(localY);
        } else {
            // 目标 surface 不可用: 退回旧路径 (父窗口相对坐标)
            wx = wl_fixed_from_double(logicalX - ws->GetToplevelX(tl));
            wy = wl_fixed_from_double(logicalY - ws->GetToplevelY(tl));
        }
    } else {
        CoordTransform(px, py, tl, &wx, &wy);
        // PC 空间全局指针位置 = 窗口局部坐标 + 窗口位置 (grab 偏移基准)
        lastGlobalPtrX_.store(wl_fixed_from_double(wl_fixed_to_double(wx) + ws->GetToplevelX(tl)));
        lastGlobalPtrY_.store(wl_fixed_from_double(wl_fixed_to_double(wy) + ws->GetToplevelY(tl)));
        // move grab 降级路径 (PC 模式 startMoving 失败时): wx 是窗口局部坐标,
        // 补上窗口位置还原为绝对坐标, 供 compositor 绝对定位 (不在此做
        // 局部→全局往返, 消费侧不再二次读 st->x, 避免双线程基准漂移)
        if (action == ACT_MOVE && ws->IsMoveGrabActive() &&
            ws->GetMoveGrabToplevelId() == tl) {
            Enqueue(InputEvent::PTR_MOTION, 0, nullptr,
                    wl_fixed_from_double(wl_fixed_to_double(wx) + ws->GetToplevelX(tl)),
                    wl_fixed_from_double(wl_fixed_to_double(wy) + ws->GetToplevelY(tl)),
                    0, 0);
            return;
        }
        // PC 模式: wx/wy 即窗口局部坐标, 无需额外变换
    }

    // 相对指针增量 (zwp_relative_pointer_v1): wine 相对模式 (隐藏光标 + 约束,
    // wayland_pointer.c needs_relative) 丢弃绝对 motion, 光标位置 = 基线 +
    // 增量累积。host 不做模式判断 — 绝对 motion 照常注入 (相对模式下被 wine
    // 丢弃), 同时把注入坐标 (surface 局部空间) 差分出增量, 有 relative 对象
    // 就入队 REL_MOTION 转发。对象存在 ⇔ wine 判定当前为相对模式。
    // 基准只在 MOVE 时更新: PRESS/RELEASE 不发增量也不该移动基准, 否则按下
    // 瞬间的坐标跳变会污染后续增量 (wine 侧位置并未因 press 变化)。
    if (action == ACT_MOVE) {
        const double localX = wl_fixed_to_double(wx);
        const double localY = wl_fixed_to_double(wy);
        if (hasLastLocal_) {
            const double dx = localX - lastLocalX_;
            const double dy = localY - lastLocalY_;
            if ((dx != 0 || dy != 0) && PointerExtras::GetInstance()->HasRelativePointer())
                Enqueue(InputEvent::REL_MOTION, 0, nullptr,
                        wl_fixed_from_double(dx), wl_fixed_from_double(dy), 0, 0);
        }
        lastLocalX_ = localX;
        lastLocalY_ = localY;
        hasLastLocal_ = true;
    }


    // MOVE 是高频路径 (hover 移动 ~125Hz), 全量日志会刷爆 hilog → 抽样 120:1;
    // PRESS/RELEASE 低频且诊断价值高, 保持全量
    if (action != ACT_MOVE) {
        OH_LOG_INFO(LOG_APP, "[Input] PTR action=%{public}d tl=%{public}u btn=0x%{public}x px=(%{public}.0f,%{public}.0f)"
                    " wine=(%{public}.0f,%{public}.0f) ptrRes=%{public}d needsEnter=%{public}d pressedBits=0x%{public}x",
                    action, tl, button, px, py,
                    wl_fixed_to_double(wx), wl_fixed_to_double(wy),
                    seat->HasPointerResource(), NeedsPointerEnter(), pressedButtons_);
    } else {
        static uint32_t sMoveLogN = 0;
        if (++sMoveLogN % 120 == 0)
            OH_LOG_INFO(LOG_APP, "[Input] PTR MOVE tl=%{public}u px=(%{public}.0f,%{public}.0f)"
                        " wine=(%{public}.0f,%{public}.0f) focusedTl=%{public}u n=%{public}u",
                        tl, px, py, wl_fixed_to_double(wx), wl_fixed_to_double(wy),
                        pointerFocusedToplevel_.load(), sMoveLogN);
    }

    switch (action) {
        case ACT_PRESS: {
            // 每次都发 enter: Wine 在两次点击间需要新的 pointer focus。
            // 相对模式 (relative_pointer 对象存在) 且聚焦已建立时跳过:
            // wine 的 enter 处理会把光标跳到 enter 坐标 (pointer_handle_enter
            // → motion_internal, 相对模式只丢弃 motion 不丢弃 enter) — 点击
            // 瞬间游戏内光标瞬移到设备绝对位置 (实测偏移量 = 光标距屏幕
            // 中心的偏移)。相对模式聚焦持续有效, 无需重发 enter。
            const bool relSkipEnter = PointerExtras::GetInstance()->HasRelativePointer() &&
                                      pointerFocusedSurface_.load() != nullptr;
            if (!relSkipEnter) {
                wl_resource* surf = targetSurf ? targetSurf : ws->GetSurfaceForToplevel(tl);
                if (surf) {
                    // desktop: surface 级比较 (菜单层与父窗口同 toplevelId);
                    // 其余模式保持 toplevel 级比较 (一窗一 surface, 语义等价)
                    wl_resource* focused = pointerFocusedSurface_.load();
                    const bool needLeave = targetSurf
                        ? (focused != nullptr && focused != surf)
                        : (pointerFocusedToplevel_.load() != 0 && pointerFocusedToplevel_.load() != tl);
                    if (needLeave)
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
                lastPressMs_ = NowMs();  // 脉冲拉伸计时基准 (见 ACT_RELEASE)
                Enqueue(InputEvent::PTR_BUTTON, 0, nullptr, 0, 0, button, WL_POINTER_BUTTON_STATE_PRESSED);
            }

            //  键盘焦点跟随点击 (P0-1 + P0-3)
            // winewayland.drv: keyboard_enter → WM_WAYLAND_SET_FOREGROUND
            // → NtUserSetForegroundWindowInternal → Wine 前台窗口切换
            if (!keyboardEntered_.load() || keyboardFocusedToplevel_.load() != tl) {
                wl_resource* kbdSurf = ws->GetSurfaceForToplevel(tl);
                if (kbdSurf) {
                    if (keyboardEntered_.load() && keyboardFocusedToplevel_.load() != tl)
                        Enqueue(InputEvent::KBD_LEAVE, 0, nullptr, 0, 0, 0, 0);
                    keyboardFocusedToplevel_ = tl;
                    keyboardFocusedSurface_ = kbdSurf;
                    keyboardEntered_ = true;
                    Enqueue(InputEvent::KBD_ENTER, tl, kbdSurf, 0, 0, 0, 0);
                    EnqueueModifiers();
                    OH_LOG_INFO(LOG_APP, "[Input] PTR PRESS + KBD ENTER tl=%{public}u (focus follows click)", tl);
                }
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
            // 脉冲拉伸: 按下-抬起间隔 <kMinPressDurationMs 的点击, 抬手延迟
            // 补足再发。根因: ArkTS 触控手势状态机把静止 tap 合成为
            // Press+Release 同刻脉冲 (DesktopWindow.ets onTouch 的"等 Up 再
            // 补发"手势语义); down/up 挤在同一次 GetDeviceState 轮询 (~8ms)
            // 内相互净零, PAL2 类按帧轮询 dinput 的游戏永远看不见这次点击
            // (探针实证: smoke/dinput_click_probe.c; 真机 Windows / winlator
            // 的真实时序输入无此问题)。物理鼠标自 ArkTS .onMouse 直通修复后
            // 为真实时序, 本延迟主要对残余的触屏 tap 脉冲生效。
            // 延迟用短生命周期线程, 不阻塞 ArkTS 输入线程; 极限快速连点
            // (<100ms 间隔) 事件可能乱序, 对老游戏可接受。
            const uint32_t quick = NowMs() - lastPressMs_.load();
            if (releaseBtn && quick < kMinPressDurationMs) {
                const uint32_t delayMs = kMinPressDurationMs - quick;
                // 可观测性: 触屏 tap 之外的使用 (如物理鼠标) 不应触发本延迟;
                // 触发 = 某条输入路径在产塌缩脉冲, 是 bug 信号而非正常事件
                OH_LOG_INFO(LOG_APP, "[Input] STRETCH delay release btn=0x%{public}x quick=%{public}ums delay=%{public}ums",
                            releaseBtn, quick, delayMs);
                std::thread([this, delayMs, releaseBtn] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                    Enqueue(InputEvent::PTR_BUTTON, 0, nullptr, 0, 0, releaseBtn,
                            WL_POINTER_BUTTON_STATE_RELEASED);
                }).detach();
            } else if (releaseBtn) {
                Enqueue(InputEvent::PTR_BUTTON, 0, nullptr, 0, 0, releaseBtn,
                        WL_POINTER_BUTTON_STATE_RELEASED);
            }
            break;
        }
        case ACT_MOVE: {
            // 高频路径不打全量日志 (入口已有 120:1 抽样); MOVE-ENTER 是低频
            // 焦点切换事件, 保留全量日志
            // desktop: surface 级焦点判定 — 鼠标从窗口移入菜单层 (同 toplevelId,
            // 不同 surface) 时必须重新 enter, 否则 motion 继续发给窗口 surface
            const bool needEnter = targetSurf
                ? (NeedsPointerEnter() || pointerFocusedSurface_.load() != targetSurf)
                : (NeedsPointerEnter() || pointerFocusedToplevel_.load() != tl);
            if (needEnter) {
                wl_resource* surf = targetSurf ? targetSurf : ws->GetSurfaceForToplevel(tl);
                OH_LOG_INFO(LOG_APP, "[Input] MOVE-ENTER try surf=%{public}p for tl=%{public}u", surf, tl);
                if (surf) {
                    wl_resource* focused = pointerFocusedSurface_.load();
                    const bool needLeave = targetSurf
                        ? (focused != nullptr && focused != surf)
                        : (pointerFocusedToplevel_.load() != 0 && pointerFocusedToplevel_.load() != tl);
                    if (needLeave)
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
    // 窗口不可见时抑制输入
    {
        std::lock_guard<std::mutex> lk(visibleMutex_);
        auto it = toplevelVisible_.find(tl);
        if (it != toplevelVisible_.end() && !it->second) return;
    }

    auto* seat = Seat::GetInstance();

    OH_LOG_INFO(LOG_APP, "[Input] KEY tl=%{public}u evdev=%{public}d pressed=%{public}d"
                " kbdRes=%{public}d kbdEntered=%{public}d",
                tl, evdevCode, pressed,
                seat->GetKeyboardResource() ? 1 : 0,
                keyboardEntered_.load());

    // 键盘 enter 管理: 立即设置状态防止重复 enter (参考旧代码)
    // 桌面模式: 键盘事件永远发到 root, 不应覆盖点击建立的子窗口焦点
    if (pressed && !WaylandServer::GetInstance()->Policy().CompositorRoutesInput()
        && (!keyboardEntered_.load() || keyboardFocusedToplevel_.load() != tl)) {
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
            EnqueueModifiers();
        }
    }

    // 追踪 modifier 状态 → 同步到 Wine
    if (IsModifierKey(evdevCode)) {
        UpdateModifiers(evdevCode, pressed);
        EnqueueModifiers();  // 每次修饰键变化都同步, Wine 需要最新的 modifier state
    }

    // 入队 key 事件
    uint32_t state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    Enqueue(InputEvent::KBD_KEY, 0, nullptr, 0, 0, evdevCode, state);
}

void InputManager::EnqueueModifiers() {
    InputEvent ev;
    ev.type = InputEvent::KBD_MODIFIERS;
    ev.mod_depressed = modifiers_depressed_;
    ev.mod_latched = modifiers_latched_;
    ev.mod_locked = modifiers_locked_;
    ev.mod_group = modifiers_group_;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.push_back(ev);
    }
    if (pipeWrite_ >= 0) {
        char c = 1;
        ssize_t n = write(pipeWrite_, &c, 1);
        if (n < 0 && errno != EAGAIN) {
            OH_LOG_WARN(LOG_APP, "[Input] pipe write FAIL errno=%{public}d", errno);
        }
    }
}

void InputManager::SendScrollEvent(uint32_t tl, int axis, double value, int scrollStep,
                                    double px, double py) {
    auto* seat = Seat::GetInstance();
    if (!seat->HasPointerResource()) return;

    // 坐标转换
    wl_fixed_t wx, wy;
    CoordTransform(px, py, tl, &wx, &wy);

    // Wayland axis value 用 wl_fixed_t (256 精度)
    // HarmonyOS AxisEvent 的 value 是浮点数, 每个 notch 通常 ±1.0
    wl_fixed_t val = wl_fixed_from_double(value);

    OH_LOG_INFO(LOG_APP, "[Input] SCROLL tl=%{public}u axis=%{public}s val=%{public}.1f step=%{public}d"
                " px=(%{public}.0f,%{public}.0f) wine=(%{public}.1f,%{public}.1f) ptrRes=%{public}d",
                tl, axis == 0 ? "VERT" : "HORIZ", value, scrollStep, px, py,
                wl_fixed_to_double(wx), wl_fixed_to_double(wy),
                seat->HasPointerResource());

    // 确保指针已 enter (和 MOVE 同样的逻辑)
    if (NeedsPointerEnter() || pointerFocusedToplevel_.load() != tl) {
        wl_resource* surf = WaylandServer::GetInstance()->GetSurfaceForToplevel(tl);
        if (surf) {
            if (pointerFocusedToplevel_.load() != 0 && pointerFocusedToplevel_.load() != tl)
                Enqueue(InputEvent::PTR_LEAVE, 0, nullptr, 0, 0, 0, 0);
            Enqueue(InputEvent::PTR_ENTER, tl, surf, wx, wy, 0, 0);
        }
    }

    // 入队 axis 事件
    {
        InputEvent ev;
        ev.type = InputEvent::PTR_AXIS;
        ev.axis = axis;
        ev.axis_value = val;
        ev.tl = tl;
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.push_back(ev);
    }
    if (pipeWrite_ >= 0) {
        char c = 1;
        ssize_t n = write(pipeWrite_, &c, 1);
        if (n < 0 && errno != EAGAIN) {
            OH_LOG_WARN(LOG_APP, "[Input] pipe write FAIL errno=%{public}d", errno);
        }
    }
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
                        last = ev; continue;  // 只保留最后一个坐标 (绝对位置)
                    // PTR_AXIS 不去重: 每个值是累积滚动距离, 丢中间值 = 丢滚动量
                    // 快速滚轮/触控板会产生连续 axis 事件, 必须全部送达 Wine
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
            case InputEvent::PTR_MOTION: {
                //  Wayland 标准: xdg_toplevel.move 期间 compositor 接管 motion,
                // 不转发给 Wine (协议规定 surface loses device focus)
                if (WaylandServer::GetInstance()->ProcessMoveGrabMotion(ev.x, ev.y))
                    break;
                InjectPointerMotion(ev.x, ev.y); break;
            }
            case InputEvent::PTR_BUTTON:
                //  交互式移动结束: Release 时结束 grab 并转发给 Wine
                if (ev.state == WL_POINTER_BUTTON_STATE_RELEASED)
                    WaylandServer::GetInstance()->EndMoveGrab();
                InjectPointerButton(ev.btn_or_key, ev.state); break;
            case InputEvent::REL_MOTION:  InjectRelativeMotion(ev.x, ev.y); break;
            case InputEvent::PTR_AXIS:    InjectPointerAxis(ev.axis, ev.axis_value); break;
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
        gDropEnter.fetch_add(1); MaybeReportDrops();
        return;
    }

    // 防御: surface 可能在入队后到 flush 前被 Wine 销毁。
    // 用 surfaceResources_ 精确验证该 surface 本体仍存活 —
    // 菜单 subsurface 不在 toplevelSurfaceMap_ 里, 不能用 tl 的映射代替
    if (!WaylandServer::GetInstance()->IsSurfaceAlive(surface)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectEnter DROP tl=%{public}u surf=%{public}p: surface destroyed before flush", tl, surface);
        gDropEnter.fetch_add(1); MaybeReportDrops();
        return;
    }

    pointerFocusedToplevel_ = tl;
    pointerFocusedSurface_ = surface;
    uint32_t s = serial_++;
    pointerEnterSerial_ = s;
    int nSent = 0;
    struct wl_client* surfClient = wl_resource_get_client(surface);
    OH_LOG_INFO(LOG_APP, "[Input] InjectEnter tl=%{public}u serial=%{public}u sx=%{public}.1f sy=%{public}.1f nPtrs=%{public}zu t=%{public}u",
                tl, s, wl_fixed_to_double(sx), wl_fixed_to_double(sy), ptrs.size(), NowMs());
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

void InputManager::InjectRelativeMotion(wl_fixed_t dx, wl_fixed_t dy) {
    // 相对模式增量转发 (zwp_relative_pointer_v1)。wine 侧收到后累积进
    // wineserver 光标位置 (wayland_pointer.c relative_pointer_v1_relative_motion)。
    // 无 relative 对象 (绝对模式) 时 PointerExtras 内部空转。
    PointerExtras::GetInstance()->SendRelativeMotion(
        wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}

void InputManager::InjectPointerMotion(wl_fixed_t sx, wl_fixed_t sy) {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    if (ptrs.empty()) { gDropMotion.fetch_add(1); return; }
    for (auto* ptr : ptrs) {
        if (ptr) {
            wl_pointer_send_motion(ptr, NowMs(), sx, sy);
            wl_pointer_send_frame(ptr);
        }
    }
    // 高频路径 (hover ~125Hz) 抽样 120:1, 防止刷爆 hilog
    static uint32_t sInjMotionLogN = 0;
    if (++sInjMotionLogN % 120 == 0)
        OH_LOG_INFO(LOG_APP, "[Input] InjectMotion sx=%{public}.1f sy=%{public}.1f OK n=%{public}u ptrs=%{public}zu",
                    wl_fixed_to_double(sx), wl_fixed_to_double(sy), sInjMotionLogN, ptrs.size());
}

void InputManager::InjectPointerButton(uint32_t button, uint32_t state) {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    if (ptrs.empty()) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectButton DROP btn=0x%{public}x: no ptr", button);
        gDropButton.fetch_add(1); MaybeReportDrops();
        return;
    }
    // 使用最近一次 enter 的 serial (Wayland 协议要求 button 序列号与 enter 一致)
    uint32_t enterSerial = pointerEnterSerial_.load();
    uint32_t s = enterSerial ? enterSerial : serial_++;
    OH_LOG_INFO(LOG_APP, "[Input] InjectButton btn=0x%{public}x state=%{public}u serial=%{public}u (enterSerial=%{public}u) n=%{public}zu t=%{public}u",
                button, state, s, enterSerial, ptrs.size(), NowMs());
    for (auto* ptr : ptrs) {
        if (ptr) {
            wl_pointer_send_button(ptr, s, NowMs(), button, state);
            wl_pointer_send_frame(ptr);
        }
    }
}

void InputManager::InjectPointerAxis(int axis, wl_fixed_t value) {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    if (ptrs.empty()) { return; }
    uint32_t axisEnum = (axis == 0) ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                    : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    int nSent = 0;
    uint32_t t = NowMs();
    for (auto* ptr : ptrs) {
        if (ptr) {
            // winewayland.drv 只处理 axis_discrete/axis_value120, axis 是空函数。
            // 协议规定 discrete 在配对 axis 之前; steps 直接比较 wl_fixed 原值,
            // 避免 wl_fixed_to_int 截断把 |值|<1 的正向滚动 (触控板细步) 误判成反向
            if (wl_resource_get_version(ptr) >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) {
                int32_t steps = (value > 0) ? 1 : -1;
                wl_pointer_send_axis_discrete(ptr, axisEnum, steps);
            }
            wl_pointer_send_axis(ptr, t, axisEnum, value);
            wl_pointer_send_frame(ptr);
            nSent++;
        }
    }
    OH_LOG_INFO(LOG_APP, "[Input] InjectAxis %{public}s val=%{public}.1f t=%{public}u sent=%{public}d",
                axis == 0 ? "VERT" : "HORIZ", wl_fixed_to_double(value), t, nSent);
}

void InputManager::InjectPointerLeave() {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    wl_resource* surf = pointerFocusedSurface_.load();
    if (ptrs.empty() || !surf) return;
    // 防御: surface 可能在 leave 入队后到 flush 前被销毁 — 对已复用的
    // 对象 id 发 leave 会让 client 报 "invalid object ... leave(uo)" 并断开
    if (!WaylandServer::GetInstance()->IsSurfaceAlive(surf)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectLeave SKIP surf=%{public}p: destroyed before flush", surf);
        pointerFocusedToplevel_ = 0;
        pointerFocusedSurface_ = nullptr;
        pointerEnterSerial_ = 0;
        return;
    }
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
        gDropEnter.fetch_add(1); MaybeReportDrops();
        return;
    }

    // 防御: surface 可能在入队后到 flush 前被 Wine 销毁
    if (!WaylandServer::GetInstance()->GetSurfaceForToplevel(tl)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKbdEnter DROP tl=%{public}u: surface no longer in map (destroyed before flush?)", tl);
        gDropEnter.fetch_add(1); MaybeReportDrops();
        return;
    }

    keyboardFocusedToplevel_ = tl;
    keyboardFocusedSurface_ = surface;
    keyboardEntered_ = true;
    uint32_t s = serial_++;
    int nSent = 0;
    struct wl_client* surfClient = wl_resource_get_client(surface);
    OH_LOG_INFO(LOG_APP, "[Input] InjectKbdEnter tl=%{public}u serial=%{public}u mods=0x%{public}x nKbds=%{public}zu t=%{public}u",
                tl, s, modifiers_depressed_, kbds.size(), NowMs());

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
        gDropKey.fetch_add(1); MaybeReportDrops();
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
        gDropKey.fetch_add(1); MaybeReportDrops();
    } else {
        OH_LOG_INFO(LOG_APP, "[Input] InjectKey evdev=%{public}u state=%{public}u serial=%{public}u sent=%{public}d t=%{public}u",
                    key, state, s, nSent, NowMs());
    }
}

void InputManager::InjectKeyboardLeave() {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    wl_resource* surf = keyboardFocusedSurface_;
    if (kbds.empty() || !keyboardEntered_.load() || !surf) return;
    // 防御: 同 InjectPointerLeave — 对已销毁/复用的对象 id 发 leave 会断开 client
    if (!WaylandServer::GetInstance()->IsSurfaceAlive(surf)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKbdLeave SKIP surf=%{public}p: destroyed before flush", surf);
        keyboardEntered_ = false;
        keyboardFocusedToplevel_ = 0;
        keyboardFocusedSurface_ = nullptr;
        return;
    }
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

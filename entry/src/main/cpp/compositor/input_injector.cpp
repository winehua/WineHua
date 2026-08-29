#include "compositor/input_injector.h"

#include <wayland-server-protocol.h>  // wl_pointer_send_*/wl_keyboard_send_* 系列

#include "seat.h"
#include "pointer_extras.h"
#include "text_input.h"
// 6A 装配注入直呼 (BindResolvers 注入; wl 事件循环启动前装配, 见头注释):
#include "compositor/input_resolver.h"   // IsSurfaceAlive (注入前 surface 存活防御)
#include "compositor/toplevel_manager.h" // GetSurfaceForToplevel (surface 存在检查)

#include <chrono>
#include <atomic>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Input"
#include <hilog/log.h>

// ============================================================================
//  丢帧统计 (全局计数器 + 周期性汇总, 60s 间隔) —
//  原 input_manager.cpp file-static 同迁 (所有 fetch_add 点都在本层)
// ============================================================================
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

// -- 辅助: 当前毫秒时间 (各文件独立 file-static — 原 input_manager.cpp 同款) --
static uint32_t NowMs() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return static_cast<uint32_t>(ms);
}

InputInjector::InputInjector(InputStateTracker* tracker) : tracker_(tracker) {}

void InputInjector::BindResolvers(InputResolver* resolver, ToplevelManager* tmgr) {
    resolver_ = resolver;
    tmgr_ = tmgr;
}

// ========================================================================
//  事件注入 (Wayland 线程, 调用 wl_*_send_*)
// ========================================================================

void InputInjector::InjectPointerEnter(uint32_t tl, wl_resource* surface, wl_fixed_t sx, wl_fixed_t sy) {
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
    // 6A: 直呼注入的 InputResolver (原 WaylandServer::IsSurfaceAlive 转发)
    if (!resolver_->IsSurfaceAlive(surface)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectEnter DROP tl=%{public}u surf=%{public}p: surface destroyed before flush", tl, surface);
        gDropEnter.fetch_add(1); MaybeReportDrops();
        return;
    }

    // 焦点状态记录 + 串行号取号 (与旧 InjectPointerEnter 语句序一致:
    // tl/surface 先写、enter serial 后写; 取号与写之间本方法同线程独占
    // — 无联合读方, 旧 ts/tl/store 与 serial fetch_add 的相对顺序不可观察)
    uint32_t s = tracker_->NextSerial();
    tracker_->SetPointerFocus(tl, surface, s);

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

void InputInjector::InjectRelativeMotion(wl_fixed_t dx, wl_fixed_t dy) {
    // 相对模式增量转发 (zwp_relative_pointer_v1)。wine 侧收到后累积进
    // wineserver 光标位置 (wayland_pointer.c relative_pointer_v1_relative_motion)。
    // 无 relative 对象 (绝对模式) 时 PointerExtras 内部空转。
    PointerExtras::GetInstance()->SendRelativeMotion(
        wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}

void InputInjector::InjectPointerMotion(wl_fixed_t sx, wl_fixed_t sy) {
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

void InputInjector::InjectPointerButton(uint32_t button, uint32_t state) {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    if (ptrs.empty()) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectButton DROP btn=0x%{public}x: no ptr", button);
        gDropButton.fetch_add(1); MaybeReportDrops();
        return;
    }
    // 使用最近一次 enter 的 serial (Wayland 协议要求 button 序列号与 enter 一致)
    uint32_t enterSerial = tracker_->PointerEnterSerial();
    uint32_t s = enterSerial ? enterSerial : tracker_->NextSerial();
    OH_LOG_INFO(LOG_APP, "[Input] InjectButton btn=0x%{public}x state=%{public}u serial=%{public}u (enterSerial=%{public}u) n=%{public}zu t=%{public}u",
                button, state, s, enterSerial, ptrs.size(), NowMs());
    for (auto* ptr : ptrs) {
        if (ptr) {
            wl_pointer_send_button(ptr, s, NowMs(), button, state);
            wl_pointer_send_frame(ptr);
        }
    }
}

void InputInjector::InjectPointerAxis(int axis, wl_fixed_t value) {
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

void InputInjector::InjectPointerLeave() {
    auto ptrs = Seat::GetInstance()->GetAllPointerResources();
    wl_resource* surf = tracker_->PointerFocusedSurface();
    if (ptrs.empty() || !surf) return;
    // 防御: surface 可能在 leave 入队后到 flush 前被销毁 — 对已复用的
    // 对象 id 发 leave 会让 client 报 "invalid object ... leave(uo)" 并断开
    if (!resolver_->IsSurfaceAlive(surf)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectLeave SKIP surf=%{public}p: destroyed before flush", surf);
        tracker_->ClearPointerFocus();
        return;
    }
    uint32_t s = tracker_->NextSerial();
    struct wl_client* surfClient = wl_resource_get_client(surf);
    for (auto* ptr : ptrs) {
        if (ptr && wl_resource_get_client(ptr) == surfClient) {
            wl_pointer_send_leave(ptr, s, surf);
        }
    }
    tracker_->ClearPointerFocus();
}

void InputInjector::InjectKeyboardEnter(uint32_t tl, wl_resource* surface) {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    if (kbds.empty() || !surface) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKbdEnter DROP nKbds=%{public}zu surf=%{public}p", kbds.size(), surface);
        gDropEnter.fetch_add(1); MaybeReportDrops();
        return;
    }

    // 防御: surface 可能在入队后到 flush 前被 Wine 销毁
    // 6A: 直呼注入的 ToplevelManager (原 WaylandServer::GetSurfaceForToplevel 转发)
    if (!tmgr_->GetSurfaceForToplevel(tl)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKbdEnter DROP tl=%{public}u: surface no longer in map (destroyed before flush?)", tl);
        gDropEnter.fetch_add(1); MaybeReportDrops();
        return;
    }

    tracker_->SetKeyboardFocus(tl, surface);
    uint32_t s = tracker_->NextSerial();
    int nSent = 0;
    struct wl_client* surfClient = wl_resource_get_client(surface);
    OH_LOG_INFO(LOG_APP, "[Input] InjectKbdEnter tl=%{public}u serial=%{public}u mods=0x%{public}x nKbds=%{public}zu t=%{public}u",
                tl, s, tracker_->ModifiersDepressed(), kbds.size(), NowMs());

    for (auto* kbd : kbds) {
        if (!kbd) continue;
        // 安全检查: surface 必须与 keyboard 属于同一 client
        if (wl_resource_get_client(kbd) != surfClient) continue;
        wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(kbd, s, surface, &keys);
        wl_array_release(&keys);

        // 发送当前 modifier 状态
        wl_keyboard_send_modifiers(kbd, tracker_->NextSerial(), tracker_->ModifiersDepressed(),
                                   tracker_->ModifiersLatched(), tracker_->ModifiersLocked(),
                                   tracker_->ModifiersGroup());
        nSent++;
    }
    OH_LOG_INFO(LOG_APP, "[Input] InjectKbdEnter OK sent=%{public}d", nSent);
    // IME: keyboard enter → text-input enter (Wine 收到后 enable, 等待文本框
    // SetIMECompositionRect → 非零光标矩形 → TextInput 判定激活)
    TextInput::GetInstance()->OnKeyboardEnter(surface);
}

void InputInjector::InjectKeyboardKey(uint32_t key, uint32_t state) {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    if (kbds.empty()) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKey DROP evdev=%{public}u: no kbd", key);
        gDropKey.fetch_add(1); MaybeReportDrops();
        return;
    }
    uint32_t s = tracker_->NextSerial();
    int nSent = 0;
    struct wl_client* focusClient = tracker_->KeyboardFocusedSurface()
        ? wl_resource_get_client(tracker_->KeyboardFocusedSurface()) : nullptr;
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

void InputInjector::InjectKeyboardLeave() {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    wl_resource* surf = tracker_->KeyboardFocusedSurface();
    if (kbds.empty() || !tracker_->KeyboardEntered() || !surf) return;
    // 防御: 同 InjectPointerLeave — 对已销毁/复用的对象 id 发 leave 会断开 client
    if (!resolver_->IsSurfaceAlive(surf)) {
        OH_LOG_WARN(LOG_APP, "[Input] InjectKbdLeave SKIP surf=%{public}p: destroyed before flush", surf);
        tracker_->ClearKeyboardFocus();
        return;
    }
    uint32_t s = tracker_->NextSerial();
    struct wl_client* surfClient = wl_resource_get_client(surf);
    for (auto* kbd : kbds) {
        if (kbd && wl_resource_get_client(kbd) == surfClient) {
            wl_keyboard_send_leave(kbd, s, surf);
        }
    }
    tracker_->ClearKeyboardFocus();
    OH_LOG_INFO(LOG_APP, "[Input] InjectKbdLeave OK");
    // IME: keyboard leave → text-input leave (Wine disable, 失活)
    TextInput::GetInstance()->OnKeyboardLeave(surf);
}

void InputInjector::InjectKeyboardModifiers(uint32_t depressed, uint32_t latched,
                                            uint32_t locked, uint32_t group) {
    auto kbds = Seat::GetInstance()->GetAllKeyboardResources();
    if (kbds.empty()) return;
    uint32_t s = tracker_->NextSerial();
    for (auto* kbd : kbds) {
        if (kbd) {
            wl_keyboard_send_modifiers(kbd, s, depressed, latched, locked, group);
        }
    }
}

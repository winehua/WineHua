// compositor/input_state_tracker 纯状态机宿主机单元测试 (make test)。
// 背景 (docs/COMPOSITOR_REFACTOR_PLAN.md 阶段 4C2): InputManager 拆四层时把
// 修饰键/按钮位掩码/焦点/可见性/serial/相对基线全部抽到可单测的纯状态类 —
// 不依赖 wayland 头/客户机资源 (wl_resource 等 opaque 句柄只存指针做身份
// 比较), 用宿主 g++ 直连编译, 把输入状态机这种历史重灾区变成离线可验证。
// 期望语义 = 重构前 input_manager.cpp 内联状态逻辑逐字平移, 本测试即
// 行为平价的特征化 (特征化测试先于实现: 本文件是新 StateTracker 接口的第一个用例)。
#include "compositor/input_state_tracker.h"
#include <cstdint>
#include <cstdio>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        std::printf("       state: pressed=0x%X modsD=0x%X modsL=0x%X ptrTl=%u kbdEntered=%d\n", \
                    t.PressedButtons(), t.ModifiersDepressed(), t.ModifiersLocked(), \
                    t.PointerFocusedToplevel(), t.KeyboardEntered() ? 1 : 0); \
    } \
} while (0)

// opaque 句柄只做身份比较, 宿主测试用假指针 (绝不 deref)
static wl_resource* FakeRes(uintptr_t v) { return reinterpret_cast<wl_resource*>(v); }

int main()
{
    InputStateTracker t;

    // ============ 1. IsModifierKey 转换 (evdev → 修饰键识别) ============
    {
        for (int k : {42, 54, 29, 97, 56, 100, 125, 126, 58, 69})
            CHECK(t.IsModifierKey(k), "modifier keycode recognized");
        // 非修饰键 (如 KEY_A=30, KEY_SPACE=57, F11=87)
        CHECK(!t.IsModifierKey(30) && !t.IsModifierKey(57) && !t.IsModifierKey(87),
              "non-modifier keycode rejected");
    }

    // ============ 2. UpdateModifiers 位状态 (pressed/depressed) ============
    {
        CHECK(t.ModifiersDepressed() == 0 && t.ModifiersLatched() == 0
              && t.ModifiersLocked() == 0 && t.ModifiersGroup() == 0,
              "modifiers start zero");
        // Shift 按下: bit0
        t.UpdateModifiers(42, true);
        CHECK(t.ModifiersDepressed() == (1u << 0), "shift press sets bit0");
        // Ctrl (左手) 按下: bit2
        t.UpdateModifiers(29, true);
        CHECK(t.ModifiersDepressed() == ((1u << 0) | (1u << 2)), "ctrl press sets bit2");
        // 右手 Ctrl (97) 与左手同 bit — 独立上下键不互斥
        t.UpdateModifiers(29, false);
        t.UpdateModifiers(97, true);
        CHECK(t.ModifiersDepressed() == ((1u << 0) | (1u << 2)), "right ctrl shares bit2");
        t.UpdateModifiers(97, false);
        CHECK(t.ModifiersDepressed() == (1u << 0), "right ctrl release clears bit2");
        // Alt bit3 / Super bit6
        t.UpdateModifiers(56, true);  // LEFTALT
        CHECK(t.ModifiersDepressed() & (1u << 3), "alt sets bit3");
        t.UpdateModifiers(125, true); // LEFTMETA
        CHECK(t.ModifiersDepressed() & (1u << 6), "super sets bit6");
        t.UpdateModifiers(125, false);
        t.UpdateModifiers(56, false);
        // 未知 evdev: no-op
        t.UpdateModifiers(30, true);
        CHECK(t.ModifiersDepressed() == (1u << 0), "non-modifier evdev no-op on press");
        t.UpdateModifiers(30, false);
        t.UpdateModifiers(42, false);
        CHECK(t.ModifiersDepressed() == 0, "shift release clears bit0");
        // latched/group 无写路径, 恒 0 (KBD_MODIFIERS 快照完整性)
        CHECK(t.ModifiersLatched() == 0 && t.ModifiersGroup() == 0, "latched/group stay 0");
    }

    // ============ 3. CapsLock/NumLock: press 翻转 locked 位 (toggle) ============
    {
        // CapsLock = 58 → bit1; NumLock = 69 → bit4
        t.UpdateModifiers(58, true);
        CHECK(t.ModifiersLocked() == (1u << 1), "caps press toggles bit1 on");
        t.UpdateModifiers(58, false);   // release 不再 toggle (仅 press 翻转)
        CHECK(t.ModifiersLocked() == (1u << 1), "caps release keeps state");
        t.UpdateModifiers(58, true);
        CHECK(t.ModifiersLocked() == 0, "caps second press toggles bit1 off");
        t.UpdateModifiers(69, true);
        CHECK(t.ModifiersLocked() == (1u << 4), "numlock press toggles bit4 on");
        t.ResetModifiers();
        CHECK(t.ModifiersDepressed() == 0 && t.ModifiersLocked() == 0
              && t.ModifiersLatched() == 0 && t.ModifiersGroup() == 0, "ResetModifiers clears all");
    }

    // ============ 4. 按钮位掩码 ============
    {
        CHECK(t.ButtonToBit(0x110) == 0 && t.ButtonToBit(0x111) == 1
              && t.ButtonToBit(0x112) == 2, "BTN codes map to bits 0/1/2");
        CHECK(t.ButtonToBit(0) == 99 && t.ButtonToBit(0x999) == 99, "unknown/zero maps to 99");
        CHECK(t.BitToButton(0) == 0x110 && t.BitToButton(1) == 0x111
              && t.BitToButton(2) == 0x112, "bits map back to BTN codes");
        CHECK(t.BitToButton(3) == 0, "unknown bit maps to 0");
        CHECK(t.PressedButtons() == 0, "buttons start zero");

        t.OnButtonPress(0x110);
        CHECK(t.PressedButtons() == 0x1, "left press sets bit0");
        t.OnButtonPress(0x112);
        CHECK(t.PressedButtons() == (0x1 | 0x4), "middle press sets bit2");
        // 未知/0 按钮被忽略 (bit>=32)
        t.OnButtonPress(0x999);
        CHECK(t.PressedButtons() == (0x1 | 0x4), "unknown button ignored on press");
        t.OnButtonPress(0);
        CHECK(t.PressedButtons() == (0x1 | 0x4), "zero button ignored on press");

        // RELEASE 语义 (旧 ACT_RELEASE 逐字): button=0/未知 → 从 bitmask 找
        // 第一个按下位释放; 指定按键 → 只释放该位; 未按下的指定键也返回其
        // 键码 (编排层 releaseBtn 非 0 即入队, 与旧实现一致时行为等价)
        uint32_t r = t.OnButtonRelease(0);
        CHECK(r == 0x110 && t.PressedButtons() == 0x4, "release(0) pops first held (left)");
        CHECK(t.OnButtonRelease(0x111) == 0x111, "release(not-held btn) returns btn code");
        CHECK(t.PressedButtons() == 0x4, "not-held release leaves mask unchanged");
        // 掩码非空时 release(0) 弹下一个按下键 (仅剩 middle): 旧 ACT_RELEASE
        // 查找顺序 bit0→bit2, 与"点开左键后中键仍按住"的中间态一致
        CHECK(t.OnButtonRelease(0) == 0x112 && t.PressedButtons() == 0x0,
              "release(0) pops next held (middle)");
        CHECK(t.OnButtonRelease(0) == 0, "release(0) with empty mask returns 0");
        t.ResetButtons();
        CHECK(t.PressedButtons() == 0, "ResetButtons clears mask");
    }

    // ============ 5. pointer 焦点 enter/leave 状态转移 ============
    {
        wl_resource* surfA = FakeRes(0x1000);
        wl_resource* surfB = FakeRes(0x2000);
        CHECK(!t.HasPointerFocus(), "no focus initially");
        CHECK(t.PointerNeedsEnter(true), "has ptr resource + no focus → needs enter");
        CHECK(!t.PointerNeedsEnter(false), "no ptr resource → no enter");
        // 焦点转移: 无焦点 → enter (tl=42, surfA, serial=1)
        t.SetPointerFocus(42, surfA, 1);
        CHECK(t.HasPointerFocus(), "focus set");
        CHECK(t.PointerFocusedToplevel() == 42, "focused tl recorded");
        CHECK(t.PointerFocusedSurface() == surfA, "focused surface recorded");
        CHECK(t.PointerFocusedSurfaceIs(surfA), "focused surface identity matches");
        CHECK(!t.PointerFocusedSurfaceIs(surfB), "other surface not focused");
        CHECK(t.PointerEnterSerial() == 1, "enter serial recorded");
        CHECK(!t.PointerNeedsEnter(true), "focused+ptr resource → no re-enter (MOVE 语义)");
        // re-enter skip 判定 (PRESS skipEnter 守卫: 按同一 surface 重发 enter
        // 被跳过 ⇔ PointerFocusedSurfaceIs(pressTargetSurf))
        CHECK(t.PointerFocusedSurfaceIs(surfA), "press on same surface skips enter");
        // leave (surface 级 → toplevel 级均清)
        t.ClearPointerFocus();
        CHECK(!t.HasPointerFocus(), "leave clears focus");
        CHECK(t.PointerFocusedSurface() == nullptr, "leave clears surface");
        CHECK(t.PointerEnterSerial() == 0, "leave clears enter serial");
        CHECK(t.PointerNeedsEnter(true), "clear → needs enter again");
    }

    // ============ 6. keyboard 焦点转移 ============
    {
        wl_resource* surfB = FakeRes(0x2000);
        CHECK(!t.KeyboardEntered(), "kbd no focus initially");
        CHECK(t.KeyboardFocusedToplevel() == 0 && t.KeyboardFocusedSurface() == nullptr,
              "kbd focus fields zero");
        t.SetKeyboardFocus(11, surfB);
        CHECK(t.KeyboardEntered(), "kbd focus set");
        CHECK(t.KeyboardFocusedToplevel() == 11 && t.KeyboardFocusedSurface() == surfB,
              "kbd focus recorded");
        t.ClearKeyboardFocus();
        CHECK(!t.KeyboardEntered() && t.KeyboardFocusedToplevel() == 0
              && t.KeyboardFocusedSurface() == nullptr, "kbd leave clears all fields");
    }

    // ============ 7. 可见性抑制边界 ============
    {
        // 未登记 = 不抑制 (wine 窗口未上报过可见性, 缺省放行 — 旧实现语义)
        CHECK(!t.IsInputSuppressed(5), "unregistered tl not suppressed");
        t.SetToplevelVisible(5, true);
        CHECK(!t.IsInputSuppressed(5), "registered visible not suppressed");
        t.SetToplevelVisible(5, false);
        CHECK(t.IsInputSuppressed(5), "registered invisible suppressed");
        // 多个窗口互不影响; 重设 true 解除
        t.SetToplevelVisible(6, false);
        CHECK(t.IsInputSuppressed(5) && t.IsInputSuppressed(6), "suppression per-tl");
        t.SetToplevelVisible(5, true);
        CHECK(!t.IsInputSuppressed(5) && t.IsInputSuppressed(6), "per-tl restore");
        t.ClearVisible();
        CHECK(!t.IsInputSuppressed(5) && !t.IsInputSuppressed(6), "ClearVisible resets all");
    }

    // ============ 8. serial 递增规则 ============
    {
        // 初值 1 (与旧 InputManager::serial_{1} 一致), 每次 +1; 复位路径
        // (ResetPointerEnter/ResetSessionState) 不清 serial — 跨会话累积
        // 用户显式观察到的序列号必须唯一 (协议串号单调是调用方合同), 保留
        CHECK(t.NextSerial() == 1, "first serial is 1");
        CHECK(t.NextSerial() == 2 && t.NextSerial() == 3, "serial monotonic +1");
        CHECK(t.NextSerial() == 4, "serial continues");
    }

    // ============ 9. 相对增量基线 (surface 局部坐标) ============
    {
        CHECK(!t.HasLastLocal(), "baseline empty initially");
        CHECK(t.LastLocalX() == 0.0 && t.LastLocalY() == 0.0, "baseline zeros");
        t.UpdateLastLocal(1.5, -2.5);
        CHECK(t.HasLastLocal(), "baseline set");
        CHECK(t.LastLocalX() == 1.5 && t.LastLocalY() == -2.5, "baseline recorded");
        t.UpdateLastLocal(3.0, 4.0);
        CHECK(t.LastLocalX() == 3.0 && t.LastLocalY() == 4.0, "baseline overwritten");
        t.ResetLastLocal();
        CHECK(!t.HasLastLocal() && t.LastLocalX() == 0.0 && t.LastLocalY() == 0.0,
              "baseline reset");
    }

    // ============ 10. 最近按下时刻 (脉冲拉伸) ============
    {
        CHECK(t.LastPressMs() == 0, "press ts starts zero");
        t.SetLastPressMs(1234);
        CHECK(t.LastPressMs() == 1234, "press ts recorded");
        t.ResetLastPressMs();
        CHECK(t.LastPressMs() == 0, "press ts reset");
    }

    // ============ 11. ResetSessionState 组合 (编排层顺序的特征化) ============
    {
        // 模拟会话污染: 按键/修饰/焦点/可见性/基线/时间戳全量脏态
        wl_resource* surfA = FakeRes(0x1000);
        wl_resource* surfB = FakeRes(0x2000);
        t.UpdateModifiers(29, true);
        t.OnButtonPress(0x110);
        t.SetPointerFocus(1, surfA, 5);
        t.SetKeyboardFocus(2, surfB);
        t.SetToplevelVisible(3, false);
        t.UpdateLastLocal(9.0, 9.0);
        t.SetLastPressMs(999);
        t.NextSerial();  // serial 前移 (复位不清, 与旧实现一致)

        // = ResetSessionState 状态段 (次序: pointer → keyboard → buttons →
        // modifiers → baseline → press → visible; mapper ResetGlobalPtr 在
        // 编排层, 不在此类)
        t.ClearPointerFocus();
        t.ClearKeyboardFocus();
        t.ResetButtons();
        t.ResetModifiers();
        t.ResetLastLocal();
        t.ResetLastPressMs();
        t.ClearVisible();

        CHECK(t.PressedButtons() == 0 && t.ModifiersDepressed() == 0, "session reset: keys");
        CHECK(!t.HasPointerFocus() && !t.KeyboardEntered(), "session reset: focuses");
        CHECK(!t.IsInputSuppressed(3), "session reset: visible");
        CHECK(!t.HasLastLocal() && t.LastPressMs() == 0, "session reset: baselines");
        // serial 不清零: 复位后继续递增 (跨会话唯一)。本节开头 NextSerial()
        // 消耗了一次 (serial 5→6), 复位后再取 → 6
        CHECK(t.NextSerial() == 6, "session reset keeps serial monotonic");
    }

    std::printf("input_state_test: %d checks / %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

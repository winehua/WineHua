/*
 * display_input.h — M1-T1 displayroute 输入链的 C 接口
 *
 * wlr_seat.h / wlr_keyboard.h 在 C++ 下不安全 (同 ohos_output.c 的结论),
 * seat + 虚拟键盘 + 注入桥整体 C 编译, 对外只暴露 C 安全签名。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

struct wl_display;
struct wl_event_loop;
struct wlr_xwayland;

#ifdef __cplusplus
extern "C" {
#endif

// 建 wlr_seat + 虚拟键盘 (keymap 编译自 wine-data/xkb 的 evdev+us 规则,
// XKB_CONFIG_ROOT 在此指向 <files>/wine/xkb)。必须在 Xwayland server
// create 之前调 (seat global 要先于 Xwayland 客户端连接存在; ready 事件
// 异步于事件循环 dispatch, 本函数在 setup 线程同段执行)。
// loop 供真机门自动注入定时器使用。返回 0 = 成功。
int wl_ohos_input_seat_create(struct wl_display *wl, struct wl_event_loop *loop);

// xwm 建立后把 seat 接给 Xwayland (wlr_xwayland_set_seat)。调用时机 =
// wlr_xwayland_create_with_server 返回后 (M0 实证 xwm_create 同步于该调用,
// cppcrash 20260928204712 崩点即此); xwm 未建时 wlroots 只存指针, 建立时
// 自动接线, 故提前调用亦安全。
void wl_ohos_input_xwayland_set_seat(struct wlr_xwayland *xwayland);

// 真机门自动注入脚本重挂 (M1-T5): 重复触发 displayroute 时刷新每轮自动化
// (步序归零 + 8s 起重排)。必须在合成器事件循环线程调用 (内部操作事件
// 循环定时器); 链未建 (无 seat) 时为 no-op。
void wl_ohos_input_script_restart(void);

// 真机门自动注入脚本开关 (默认关)。编排 = 测试资产 (定时重放按键 +
// 硬编码窗口几何), 只应在 smoke/displayroute 验证流程开启; 不开 = 无此
// 行为 (原则 #23)。必须在 wl_ohos_input_seat_create 之前调。
void wl_ohos_input_set_script_enabled(bool enabled);

// OHOS 侧注入入口 —— 任意线程安全版 (smoke NAPI 用): 内部经投递队列
// 转合成器事件循环线程执行, 立即返回。key: evdev 键码 (KEY_A=30)。
void wl_ohos_input_post_key(uint32_t keycode, bool press);

// 指针注入的任意线程安全版 (语义同 display_input_inject_motion)。
void wl_ohos_input_post_motion(float nx, float ny, int phase);

// 按钮注入的任意线程安全版 (M3a-T4)。button: evdev 按钮码 (BTN_LEFT 0x110
// / BTN_RIGHT 0x111 / BTN_MIDDLE 0x112); wire 语义 = wl_pointer.button 原值,
// Xwayland 按 X 按钮映射换算 (libinput 后端同款直传)。投递前提 = pointer
// enter 已建立 (motion 先行), 无焦点时丢弃并记日志。
void wl_ohos_input_post_button(uint32_t button, bool press);

// OHOS 侧注入入口 (循环线程直呼版: 注入脚本/队列 drain 内部使用)。
// 必须在合成器事件循环线程调用 (wlr_seat 无锁)。
// key: evdev 键码 (KEY_A=30)。wire 语义 = wl_keyboard.key 原值, Xwayland
// 内部 +8 对 XKB keymap (libinput 后端同款直传, keyboard.c:52 实读)。
// 发 key 前内部保证对当前 client surface 已 keyboard enter。
void display_input_inject_key(uint32_t keycode, bool press);

// 指针注入。nx/ny = client surface 相对坐标 0..1 (左上原点);
// phase: 0=enter 1=motion 2=leave。T1 单窗口阶段不分辨命中窗口 (T2 列表化
// 后换真命中测试)。必须在合成器事件循环线程调用。
void display_input_inject_motion(float nx, float ny, int phase);

// 按钮注入 (循环线程直呼版)。button: evdev 按钮码; 必须在合成器事件循环
// 线程调用。发 button 前需 pointer enter 已建立 (display_input_inject_motion)。
void display_input_inject_button(uint32_t button, bool press);

#ifdef __cplusplus
}
#endif

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

// OHOS 侧注入入口 (smoke NAPI / 后续编排调用)。必须在合成器事件循环线程调。
// key: evdev 键码 (KEY_A=30)。wire 语义 = wl_keyboard.key 原值, Xwayland
// 内部 +8 对 XKB keymap (libinput 后端同款直传, keyboard.c:52 实读)。
// 发 key 前内部保证对当前 client surface 已 keyboard enter。
void display_input_inject_key(uint32_t keycode, bool press);

// 指针注入。nx/ny = client surface 相对坐标 0..1 (左上原点);
// phase: 0=enter 1=motion 2=leave。T1 单窗口阶段不分辨命中窗口 (T2 列表化
// 后换真命中测试)。
void display_input_inject_motion(float nx, float ny, int phase);

#ifdef __cplusplus
}
#endif

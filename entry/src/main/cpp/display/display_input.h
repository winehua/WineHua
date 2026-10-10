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

// D19 CJK 剪贴板桥: 把 UTF-8 串设为 seat selection (xwm 自动桥接为
// Xwayland CLIPBOARD, wine Ctrl+V 可粘贴)。任意线程可调 (内部经注入
// 队列移交 loop 线程); 链未建 (无 seat) 时串被丢弃。串复制进队, 调用
// 后调用方即可释放。
void wl_ohos_input_post_clipboard(const char *utf8);
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

// 轴注入的任意线程安全版 (D15 手势层: 双指滚动/物理滚轮)。which: 0=纵向
// 1=横向 (WL_POINTER_AXIS_* 枚举同序); steps = discrete 步数 (±N)。方向判据
// 实读源码钉死: Xwayland 滚轴 increment=+1.0 (xwayland-input.c:218),
// discrete>0 → DIX emulate_scroll_button_events 出 Button5 (滚轮向下);
// 与 wayland 分支的「向上=正=向下滚」(DesktopWindow.ets) 同号 —— 手指上扫
// accum 为正 → discrete +1 → 滚轮向下, 两条路线手感一致。投递前提 = pointer
// enter 已建立 (motion 先行), 无焦点时丢弃并记日志。
void wl_ohos_input_post_axis(int which, int steps);

// 文本上屏注入的任意线程安全版 (XIM spec Task 4: x11 路线 IME commit)。
// utf8 经投递队列到 loop 线程后转 xim_bridge_send_text (NCP xim server 以
// XIM_COMMIT 投给 wine)。串复制进队, 调用后调用方即可释放。x11 路线文本
// 注入唯一通道 —— wayland 路线的 commit_string 属 winewayland.drv text-input,
// 两者在 drain 分支互斥。
void wl_ohos_input_post_text(const char *utf8);

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

// 轴注入 (循环线程直呼版)。which/steps 语义同 wl_ohos_input_post_axis。
// 必须在合成器事件循环线程调用。
void display_input_inject_axis(int which, int steps);

// ── M4a-T5: x11 多窗模式按窗路由口 (x11_toplevel_input_* 的执行端) ──
// 目标窗口由调用方给 (toplevelId 反查的 xs), 不走 hit-test; lx/ly 是该窗
// 局部坐标。action: ArkTS MouseAction Press=1 Release=2 Move=3; button:
// evdev 按钮码。焦点/activate/frame/脉冲/settle 纪律内部全复用。
// 必须在合成器事件循环线程调用。
struct wlr_xwayland_surface;
void wl_ohos_input_multimode_pointer(struct wlr_xwayland_surface *xs,
                                     double lx, double ly,
                                     int action, uint32_t button);
void wl_ohos_input_multimode_key(struct wlr_xwayland_surface *xs,
                                 uint32_t keycode, bool press);

// M4a-T5 (C1 review 修复): 多窗注入的 napi 线程入口 —— 只入队 (lock +
// pipe 唤醒), loop 线程 drain 分派到 x11_toplevel_input_*_dispatch。
// 任意线程可调。
void wl_ohos_input_post_mm_pointer(uint32_t toplevelId, int lx, int ly,
                                   int action, uint32_t button);
void wl_ohos_input_post_mm_key(uint32_t toplevelId, uint32_t keycode,
                               bool press);

#ifdef __cplusplus
}
#endif

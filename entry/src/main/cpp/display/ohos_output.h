/*
 * ohos_output.h — T8 出图链的 C 接口
 *
 * wlr_output.h 在 C++ 下不安全 (color.h 的 C99 [static n] 语法, 实测编译
 * 报错), output/commit/present 逻辑整体放在 ohos_output.c (C 编译)。
 */
#pragma once

#include <stdint.h>

struct wlr_backend;
struct wlr_renderer;
struct wl_event_loop;
struct wlr_xwayland;
struct wlr_xwayland_surface;
#include <native_window/external_window.h> /* OHNativeWindow (tag=NativeWindow) */

#ifdef __cplusplus
extern "C" {
#endif

// 当前跟踪的 client xwayland surface (M0-T9 most-recent-wins; 无 = NULL)。
// display_input 注入取数口: xs->surface 为焦点面, activate 亦需 xs 本体
// (wlr_xwayland_surface_activate 是合成器侧 API, 不调则 X server 焦点
// 永不设置, 键事件无投递目标 —— 2026-09-29 gate5 实测)。T2 列表化后
// 语义随任务更新。
struct wlr_xwayland_surface *wl_ohos_output_client_xs(void);

// 建 headless output (800x600) + 自定义 OHOS allocator + 帧定时器:
// 每帧绘制渐变+边框测试图案 → commit → NativeWindow 直推 (Attach+Flush)。
// xwayland 非 NULL 时监听其 new_surface, 已映射的 X client 窗口优先于测试
// 图案合成上屏 (T9 端到端)。
// window 为 NULL 时调用方不应调用本函数。
// 返回 0 = 链路建立; 非 0 = 失败 (hilog 已打点, 具体步骤看 "ohos-output" 标签)。
int wl_ohos_output_chain_start(struct wlr_backend *backend,
                               struct wlr_renderer *renderer,
                               struct wl_event_loop *loop,
                               OHNativeWindow *window,
                               struct wlr_xwayland *xwayland);

#ifdef __cplusplus
}
#endif

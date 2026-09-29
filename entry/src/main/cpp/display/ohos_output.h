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
struct wl_display;
struct wl_event_loop;
struct wlr_xwayland;
struct wlr_xwayland_surface;
#include <native_window/external_window.h> /* OHNativeWindow (tag=NativeWindow) */

#ifdef __cplusplus
extern "C" {
#endif

// 当前跟踪的 client xwayland surface 取数口 (display_input 注入用)。
// client_xs = 最上层已映射窗口 (T1 自动注入目标); client_topmost_at =
// 帧坐标命中测试 (T2 多窗口); frame_size = 归一化注入坐标的基准。
// activate 需 xs 本体: wlr_xwayland_surface_activate 是合成器侧 API,
// 不调则 X server 焦点永不设置, 键事件无投递目标 (2026-09-29 gate5 实测)。
struct wlr_xwayland_surface *wl_ohos_output_client_xs(void);
struct wlr_xwayland_surface *wl_ohos_output_client_topmost_at(int fx, int fy);
void wl_ohos_output_frame_size(int *w, int *h);

// X 窗口可见性谓词: surface->buffer (最后有效像素) 存在即视为有内容。
// 不得用 surface->mapped —— Xwayland 例行空 commit 会翻转它 (0.20
// surface_commit_state: NULL buffer commit → unmap), T2 实测闪断。
struct wlr_surface;
int wl_ohos_surface_has_content(struct wlr_surface *surf);

// 建 headless output (800x600) + 自定义 OHOS allocator + 帧定时器:
// 每帧绘制渐变+边框测试图案 → commit → NativeWindow 直推 (Attach+Flush)。
// xwayland 非 NULL 时监听其 new_surface, 已映射的 X client 窗口优先于测试
// 图案合成上屏 (T9 端到端)。
// display 用于建 wlr_output_layout (wl_output global 的载体): Xwayland
// rootless 的 X 屏幕尺寸来自它镜像的 wl_output global —— 没有 layout 时
// X 屏幕 0x0, wine xinerama 枚举到 0x0 显示器, is_window_rect_mapped 恒
// FALSE, 所有 wine 窗口永不 XMapWindow (T5 t5l trace 实测)。须在 output
// 创建后调用。
// window 为 NULL 时调用方不应调用本函数。
// 返回 0 = 链路建立; 非 0 = 失败 (hilog 已打点, 具体步骤看 "ohos-output" 标签)。
int wl_ohos_output_chain_start(struct wlr_backend *backend,
                               struct wlr_renderer *renderer,
                               struct wl_event_loop *loop,
                               struct wl_display *display,
                               OHNativeWindow *window,
                               struct wlr_xwayland *xwayland);

#ifdef __cplusplus
}
#endif

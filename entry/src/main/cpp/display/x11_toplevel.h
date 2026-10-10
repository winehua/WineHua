#ifndef X11_TOPLEVEL_H
#define X11_TOPLEVEL_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wlr_xwayland_surface;
struct NativeWindow; /* 真身 SDK 侧 typedef OHNativeWindow; 文件作用域前置
                      * 声明 —— 原型作用域声明会在 .c 侧报 conflicting
                      * types (build 实测) */

/* M4a: xs 生命周期 → toplevel_event_bus 映射 (spec §3.1)。
 * 只做映射不做策略 —— 策略在 ArkTS 承载层 (与 wayland 路线同构)。id 复用
 * ToplevelManager::AllocateToplevelId; 事件名字符串逐字走
 * ToplevelEventName 红线 (toplevel_event_bus.h)。仅多窗模式激活
 * (x11_toplevel_set_active, chain_start multiwindow 位驱动)。 */
void x11_toplevel_set_active(bool on);
bool x11_toplevel_active(void);

/* ohos_output HandleNewSurface 的 multiwindow 分支调用: 登记并挂 per-xs
 * 监听 (destroy/associate/set_title/request_configure)。 */
void x11_toplevel_notify_new_surface(struct wlr_xwayland_surface *xs);

/* wine 停止链调用 (display_compositor fail 收尾): 清 id 表、摘全部监听、
 * 对仍登记的 toplevel 补发 destroyed (ArkTS 侧窗口清理依赖它)。 */
void x11_toplevel_session_reset(void);

/* 输入路由查表 (Task 5); id_of_xs 返回 0 = 未登记。 */
struct wlr_xwayland_surface *x11_toplevel_xs_of(uint32_t toplevelId);
uint32_t x11_toplevel_id_of_xs(struct wlr_xwayland_surface *xs);

/* ── Task 5: 按窗输入路由 ── */
/* lx/ly = ArkTS 承载窗局部坐标 (越界 [0,w/h) 即窗间缝隙, 不投递)。
 * action: ArkTS MouseAction Press=1 Release=2 Move=3。button: evdev 码。
 * 合成器线程调用 (napi 注入面已在该线程排队)。 */
void x11_toplevel_input_pointer(uint32_t toplevelId, int lx, int ly,
                                int action, uint32_t button);
void x11_toplevel_input_key(uint32_t toplevelId, uint32_t keycode,
                            bool press);

/* ── Task 3: per-xs 呈现 ── */
struct wlr_allocator;
struct wlr_renderer;
/* chain_start 后注入 (render_tick 画 buffer 用); 均为合成器线程对象。 */
void x11_toplevel_set_render_ctx(struct wlr_allocator *alloc,
                                 struct wlr_renderer *renderer);
/* ArkTS createRenderer 回绑 (plugin_manager x11 分支): 窗口 ←→ toplevel。 */
void x11_toplevel_attach_window(uint32_t toplevelId, struct NativeWindow *win,
                                int w, int h);
void x11_toplevel_detach_window(uint32_t toplevelId);
void x11_toplevel_resize_window(uint32_t toplevelId, int w, int h);
/* 帧时钟驱动 (ohos_output FrameTick 的 multiwindow 分支): 遍历已 attach 的
 * entry, 有新 commit 则画进该窗队列 buffer 并呈现; 未 attach 跳帧 (skip
 * 计数采样日志); xs 首帧到达时补发 created 事件。合成器线程调用。 */
void x11_toplevel_render_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* X11_TOPLEVEL_H */

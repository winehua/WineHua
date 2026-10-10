#ifndef X11_TOPLEVEL_H
#define X11_TOPLEVEL_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wlr_xwayland_surface;

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

#ifdef __cplusplus
}
#endif

#endif /* X11_TOPLEVEL_H */

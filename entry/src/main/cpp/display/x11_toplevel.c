/* M4a: x11 toplevel 映射层 (spec §3.1) —— xs 生命周期 → toplevel_event_bus
 * 的 22 事件语义翻译器。只做映射不做策略; id 复用 ToplevelManager;
 * 事件名字符串逐字走 ToplevelEventName 红线 (桥侧)。
 *
 * 本文件是 C (非 C++): 需要访问 wlr_xwayland_surface 全部字段, 而
 * xwayland/xwayland.h 的 `char *class` 撞 C++ 关键字 (display_compositor.cpp
 * :35 同款结论) —— C++ 侧无法 include。事件投递经 x11_toplevel_bridge.cpp
 * 的四个 post 桥 (C++ 侧构造 JSON + ToplevelEventName + Post)。 */
#define WLR_USE_UNSTABLE
#include "x11_toplevel.h"

#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/xwayland/xwayland.h>

/* C++ 桥 (x11_toplevel_bridge.cpp): 事件投递单点。evt 值 = ToplevelEventType
 * 的底层 uint32 (枚举定义 toplevel_event_bus.h, C 侧只透传不解释)。 */
void x11_toplevel_bridge_post_created(uint32_t id, int32_t w, int32_t h);
void x11_toplevel_bridge_post_title(uint32_t id, const char *title);
void x11_toplevel_bridge_post_resize(uint32_t id, int32_t w, int32_t h);
void x11_toplevel_bridge_post_destroyed(uint32_t id);
uint32_t x11_toplevel_bridge_allocate_id(void);

#define X11_TOPLEVEL_MAX 64

struct x11_xs_entry {
    struct wlr_xwayland_surface *xs;
    uint32_t toplevelId;
    bool createdPosted; /* created 只发一次 (associate + 首帧判定的入口去重) */
    bool dead;          /* destroy 已到: session_reset 时不再重复补发 */
    struct wl_listener destroy;
    struct wl_listener associate;
    struct wl_listener set_title;
    struct wl_listener request_configure;
};

static struct x11_xs_entry g_entries[X11_TOPLEVEL_MAX];
static size_t g_entry_count;
static bool g_active = false;

static struct x11_xs_entry *entry_of_xs(struct wlr_xwayland_surface *xs)
{
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].xs == xs && !g_entries[i].dead) return &g_entries[i];
    }
    return NULL;
}

static void detach_listeners(struct x11_xs_entry *e)
{
    wl_list_remove(&e->destroy.link);
    wl_list_remove(&e->associate.link);
    wl_list_remove(&e->set_title.link);
    wl_list_remove(&e->request_configure.link);
    wl_list_init(&e->destroy.link);   /* 防 double-remove (session_reset 后
                                       * destroy 再到的悬挂回调安全化) */
    wl_list_init(&e->associate.link);
    wl_list_init(&e->set_title.link);
    wl_list_init(&e->request_configure.link);
}

static void entry_remove(struct x11_xs_entry *e)
{
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (&g_entries[i] == e) {
            memmove(&g_entries[i], &g_entries[i + 1],
                    (g_entry_count - i - 1) * sizeof(*e));
            g_entry_count--;
            return;
        }
    }
}

/* created 时机 (spec §3.1): associate + 首帧 buffer 存在 —— 对齐 wayland
 * PC 模式的延后语义 (wl_core.cpp:639 同款)。associate 时 surface 已 valid
 * (wlroots xwayland.h:126), buffer 未到则等后续 associate/commit 补发。 */
static void try_post_created(struct x11_xs_entry *e)
{
    if (e->createdPosted || !e->xs->surface || !e->xs->surface->buffer) return;
    e->createdPosted = true;
    x11_toplevel_bridge_post_created(
        e->toplevelId, (int32_t)e->xs->width, (int32_t)e->xs->height);
}

static void handle_associate(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, associate);
    (void)data;
    try_post_created(e);
}

static void handle_set_title(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, set_title);
    (void)data;
    if (!e->createdPosted) return;
    x11_toplevel_bridge_post_title(e->toplevelId,
                                   e->xs->title ? e->xs->title : "");
}

static void handle_request_configure(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, request_configure);
    (void)data;
    /* M4a 只上送 resize 语义 (ArkTS 权威回写走 ResizeRenderer); 应用应答
     * (wlr_xwayland_surface_configure) 在多窗模式由 Task 3 渲染循环节按需
     * 处理 —— 无 scene 缓冲依赖, 不应答会造成 commit 滞留 (T9 教训), 落在
     * Task 3 的 render_entry 内。 */
    if (!e->createdPosted) return;
    x11_toplevel_bridge_post_resize(
        e->toplevelId, (int32_t)e->xs->width, (int32_t)e->xs->height);
}

static void handle_xs_destroy(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, destroy);
    (void)data;
    if (e->dead) return;
    e->dead = true;
    detach_listeners(e);
    x11_toplevel_bridge_post_destroyed(e->toplevelId);
    entry_remove(e);
}

void x11_toplevel_set_active(bool on) { g_active = on; }
bool x11_toplevel_active(void) { return g_active; }

void x11_toplevel_notify_new_surface(struct wlr_xwayland_surface *xs)
{
    if (!g_active || !xs) return;
    if (entry_of_xs(xs)) return; /* 重复 new_surface 防御 */
    if (g_entry_count >= X11_TOPLEVEL_MAX) return;
    struct x11_xs_entry *e = &g_entries[g_entry_count++];
    memset(e, 0, sizeof(*e));
    e->xs = xs;
    e->toplevelId = x11_toplevel_bridge_allocate_id();
    e->destroy.notify = handle_xs_destroy;
    wl_signal_add(&xs->events.destroy, &e->destroy);
    e->associate.notify = handle_associate;
    wl_signal_add(&xs->events.associate, &e->associate);
    e->set_title.notify = handle_set_title;
    wl_signal_add(&xs->events.set_title, &e->set_title);
    e->request_configure.notify = handle_request_configure;
    wl_signal_add(&xs->events.request_configure, &e->request_configure);
    /* xs 可能 associate 先于 new_surface 到达 (xwm 时序): 已有 surface 就
     * 立即判定, 否则等 associate 回调。 */
    try_post_created(e);
}

void x11_toplevel_session_reset(void)
{
    /* 会话重置 (spec §3.4): 对仍登记的 toplevel 补发 destroyed (ArkTS 清窗
     * 依赖), 再清表。active 位保持 (链重启时 StartWithSurface 按新模式位
     * 重置)。 */
    for (size_t i = 0; i < g_entry_count; ++i) {
        struct x11_xs_entry *e = &g_entries[i];
        if (e->dead) continue;
        e->dead = true;
        detach_listeners(e);
        x11_toplevel_bridge_post_destroyed(e->toplevelId);
    }
    g_entry_count = 0;
}

struct wlr_xwayland_surface *x11_toplevel_xs_of(uint32_t toplevelId)
{
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].toplevelId == toplevelId) return g_entries[i].xs;
    }
    return NULL;
}

uint32_t x11_toplevel_id_of_xs(struct wlr_xwayland_surface *xs)
{
    struct x11_xs_entry *e = entry_of_xs(xs);
    return e ? e->toplevelId : 0;
}

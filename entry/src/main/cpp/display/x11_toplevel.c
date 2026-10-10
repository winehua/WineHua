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
#include <time.h> /* clock_gettime (D50 frame done 泵) */
#include <pthread.h>

#include <wayland-server-core.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h> /* wlr_client_buffer 完整定义 (surface->buffer) */
#include <wlr/types/wlr_compositor.h>
#include <wlr/xwayland/xwayland.h>

#include "ohos_buffer.h"
#include "display_input.h" /* Task 5: multimode pointer/key 路由口 */

/* C++ 桥 (x11_toplevel_bridge.cpp): 事件投递单点。evt 值 = ToplevelEventType
 * 的底层 uint32 (枚举定义 toplevel_event_bus.h, C 侧只透传不解释)。 */
void x11_toplevel_bridge_post_created(uint32_t id, int32_t w, int32_t h,
                                      int32_t x, int32_t y);
void x11_toplevel_bridge_post_title(uint32_t id, const char *title);
void x11_toplevel_bridge_post_resize(uint32_t id, int32_t w, int32_t h);
void x11_toplevel_bridge_post_destroyed(uint32_t id);
/* M4b-T1: 状态面 (bus 既有枚举; activated 复用 Restored, 见
 * handle_request_activate 注释) */
void x11_toplevel_bridge_post_minimized(uint32_t id, bool minimized);
void x11_toplevel_bridge_post_fullscreen(uint32_t id, bool fullscreen);
void x11_toplevel_bridge_post_activated(uint32_t id);
void x11_toplevel_bridge_post_modal(uint32_t id, uint32_t owner_id,
                                    int32_t modal, int32_t dx, int32_t dy,
                                    int32_t w, int32_t h);
uint32_t x11_toplevel_bridge_allocate_id(void);

/* OHOS 侧日志 (hilog); ohos_output.c 的 OHLOG 是 static 宏, 本文件自取。
 * LOG_DOMAIN/LOG_TAG 必须在 include 前 define —— 缺省时 OH_LOG_INFO 展开
 * 出的 tag 为空, 日志整条丢弃 (T3 真机实测: skip 采样日志 0 条)。 */
#define LOG_DOMAIN 0x0000
#define LOG_TAG "x11-toplevel"
#include <hilog/log.h>
#define XTL_LOG(...) ((void)OH_LOG_INFO(LOG_APP, __VA_ARGS__))

#define X11_TOPLEVEL_MAX 64

struct x11_xs_entry {
    struct wlr_xwayland_surface *xs;
    uint32_t toplevelId;
    bool createdPosted; /* created 只发一次 (associate + 首帧判定的入口去重) */
    bool dead;          /* destroy 已到: session_reset 时不再重复补发 */
    bool dirty;         /* surface commit 待渲染 (render_tick 消费) */
    uint32_t skip_count; /* 未 attach 窗口的跳帧计数 (采样日志) */
    /* texture 不自管: surface->buffer 是 wlr_client_buffer, 其 ->texture
     * 由 wlroots 随 buffer 建好/销毁 (wlr_buffer.h:157), 渲染当帧直取 ——
     * 同帧同步 submit 完才释放引用, 无缓存失效问题。 */
    struct NativeWindow *win;   /* ArkTS 回绑的呈现窗 (NULL = 未 attach) */
    int win_w, win_h;
    struct wl_listener destroy;
    struct wl_listener associate;
    struct wl_listener dissociate; /* surface 解绑: 摘 surface_commit */
    struct wl_listener set_title;
    struct wl_listener request_configure;
    struct wl_listener surface_commit; /* xs->surface->events.commit → dirty */
    /* M4b-T1: 状态面 (minimize/fullscreen/activate/parent 直译 bus 既有
     * 枚举, 不做策略) */
    struct wl_listener request_minimize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_activate;
    struct wl_listener set_parent;
    struct wl_listener map_request; /* M4b-T4: wine 还原窗口的唯一宿主信号 */
};

static struct x11_xs_entry g_entries[X11_TOPLEVEL_MAX];
static size_t g_entry_count;
static bool g_active = false;
static struct wlr_allocator *g_alloc;   /* chain_start 注入 (buffer 目标) */
static struct wlr_renderer *g_renderer; /* render pass 用 */

/* entry 表锁 (M4a 收口 final review I1): 表在两个线程被访问 ——
 *   loop 线程: listener 回调 / render_tick / session_reset / 输入 dispatch
 *   napi 线程: attach/detach/resize_window (plugin_manager 承载窗回绑)
 * entry_remove 的 memmove 会让内嵌 entry 移位, 跨线程并发查表+写字段 =
 * 错窗回绑/形式数据竞态。一把非递归大锁; 纪律:
 *   - 公开入口全加锁; static 内部函数 (entry_of_xxx、entry_remove、
 *     detach_listeners、try_post_created、render_entry) 约定调用方持锁,
 *     不加锁
 *   - 同线程重入检查: listener 回调互相不嵌套、render_tick 不重入, 安全
 *   - render_entry 持锁执行 (GL draw + present 数 ms): napi attach 最多
 *     等一帧 (90Hz 约 11ms), 可接受 —— 换队列化的复杂度不值得 (首版
 *     InjectQueue 方案真机三轮 crash 已弃, 机制未明)
 *   - 持锁期调 bridge_post_xxx 走 napi_call_threadsafe_function (异步投
 *     递, 不阻塞) —— 持锁进 JS 投递安全 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct x11_xs_entry *entry_of_xs(struct wlr_xwayland_surface *xs)
{
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].xs == xs && !g_entries[i].dead) return &g_entries[i];
    }
    return NULL;
}

static void detach_listeners(struct x11_xs_entry *e)
{
    /* 前提: notify_new_surface 已对所有 listener wl_list_init —— 未 add 过
     * 的 (如 surface_commit/dissociate 在 associate 前) 是空链自摘, 安全。 */
    wl_list_remove(&e->destroy.link);
    wl_list_remove(&e->associate.link);
    wl_list_remove(&e->dissociate.link);
    wl_list_remove(&e->set_title.link);
    wl_list_remove(&e->request_configure.link);
    wl_list_remove(&e->surface_commit.link);
    wl_list_remove(&e->request_minimize.link);
    wl_list_remove(&e->request_fullscreen.link);
    wl_list_remove(&e->request_activate.link);
    wl_list_remove(&e->set_parent.link);
    wl_list_remove(&e->map_request.link);
    wl_list_init(&e->destroy.link);   /* 防 double-remove (session_reset 后
                                       * destroy 再到的悬挂回调安全化) */
    wl_list_init(&e->associate.link);
    wl_list_init(&e->dissociate.link); /* M11 (review): 与其余 listener 同款
                                        * 重置, 维持「全空链自引」不变量 */
    wl_list_init(&e->set_title.link);
    wl_list_init(&e->request_configure.link);
    wl_list_init(&e->surface_commit.link);
    wl_list_init(&e->request_minimize.link);
    wl_list_init(&e->request_fullscreen.link);
    wl_list_init(&e->request_activate.link);
    wl_list_init(&e->set_parent.link);
    wl_list_init(&e->map_request.link);
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
        e->toplevelId, (int32_t)e->xs->width, (int32_t)e->xs->height,
        e->xs->x, e->xs->y);
    /* title 补发 (T6 byTitle 实锤, 兜底 2/2): WM_NAME 经 xcb get_property
     * 异步到达, new_surface 兜底时可能还是空; 而 created 前到的 set_title
     * 信号被 handle_set_title 的 createdPosted 门丢弃, title 不再变化则信
     * 号永不再发 —— 在 created 落地时用当前值补一次, 覆盖全部时序:
     *   先于 new_surface → notify_new_surface 兜底;
     *   new_surface~created 之间 → 此处;
     *   created 之后变化 → handle_set_title 正常通道。 */
    if (e->xs->title && e->xs->title[0])
        x11_toplevel_bridge_post_title(e->toplevelId, e->xs->title);
}

static void handle_surface_commit(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, surface_commit);
    (void)data;
    pthread_mutex_lock(&g_lock);
    e->dirty = true;
    /* 首帧判定 (wayland PC 模式同款延后语义): commit 可能先于 associate ——
     * buffer 就位即补发 created, 不漏首帧。 */
    try_post_created(e);
    pthread_mutex_unlock(&g_lock);
}

static void handle_associate(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, associate);
    (void)data;
    /* commit 监听挂 surface (associate 才 valid); dissociate 时摘除, 防
     * surface 销毁后悬挂。 */
    pthread_mutex_lock(&g_lock);
    if (e->xs->surface) {
        e->surface_commit.notify = handle_surface_commit;
        wl_signal_add(&e->xs->surface->events.commit, &e->surface_commit);
    }
    try_post_created(e);
    pthread_mutex_unlock(&g_lock);
}

static void handle_dissociate(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, dissociate);
    (void)data;
    pthread_mutex_lock(&g_lock);
    wl_list_remove(&e->surface_commit.link);
    wl_list_init(&e->surface_commit.link);
    pthread_mutex_unlock(&g_lock);
}

static void handle_set_title(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, set_title);
    (void)data;
    pthread_mutex_lock(&g_lock);
    if (!e->createdPosted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    x11_toplevel_bridge_post_title(e->toplevelId,
                                   e->xs->title ? e->xs->title : "");
    pthread_mutex_unlock(&g_lock);
}

static void handle_request_configure(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, request_configure);
    const struct wlr_xwayland_surface_configure_event *ev = data;
    /* 应用应答必须发 (T9 教训: 不应答 commit 滞留 cached state 窗口不上
     * 屏)。多窗模式 X 侧几何跟随应用请求 (无 scene 布局器); ArkTS 权威
     * 尺寸回写走 ResizeRenderer, 这里只上送 resize 语义事件。 */
    wlr_xwayland_surface_configure(e->xs, ev->x, ev->y, ev->width, ev->height);
    pthread_mutex_lock(&g_lock);
    if (!e->createdPosted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    x11_toplevel_bridge_post_resize(
        e->toplevelId, (int32_t)ev->width, (int32_t)ev->height);
    pthread_mutex_unlock(&g_lock);
}

/* ── M4b-T1: 状态面直译 (bus 既有枚举, 不做策略) ── */

static void handle_request_minimize(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, request_minimize);
    const struct wlr_xwayland_minimize_event *ev = data;
    pthread_mutex_lock(&g_lock);
    if (!e->createdPosted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* minimize=false = 还原请求 → Restored (与 request_activate 的
     * Restored 复用同动作幂等, ArkTS 侧均为拉回前台) */
    x11_toplevel_bridge_post_minimized(e->toplevelId, ev && ev->minimize);
    /* ICCCM 应答半边 (M4b-T4 实锤): 合成器必须回写状态, 否则 wine 的
     * restore 是无操作 —— wine 侧 window_set_wm_state 用 wm_state_serial
     * 门等 WM_STATE PropertyNotify (Iconic→Normal 的 Mutter workaround 入口
     * `if (data->wm_state_serial) return`), 没人写 WM_STATE=Iconic 该 serial
     * 永不清零, XWithdrawWindow/XMapWindow 永不发出 (实测 r20261010-234547:
     * minimize 事件到达后 restore 全链静默)。set_minimized 只写属性不解 map,
     * 与「X 窗保持 mapped、ArkTS 承载窗负责可视性」的模型一致。 */
    wlr_xwayland_surface_set_minimized(e->xs, ev && ev->minimize);
    pthread_mutex_unlock(&g_lock);
}

/* M4b-T4: wine 还原 (deiconify) 的宿主信号 = map_request。
 *
 * 实测链 (job-r20261010-232352 + r20261010-234547 + 源码对读):
 *   - minimize: wine X11DRV window_set_wm_state Normal→Iconic 走
 *     XIconifyWindow = WM_CHANGE_STATE(IconicState) client message →
 *     xwm_handle_wm_change_state_message → request_minimize(true) ✓ (本
 *     函数上方的 request_minimize 监听已覆盖; X 窗不解 map, 呈现不断流)
 *   - restore: 同函数 Iconic→Normal 走 Mutter workaround (window.c
 *     「transition through WithdrawnState」) = XWithdrawWindow (synthetic
 *     UnmapNotify → xwm dissociate+withdrawn) + XMapWindow。X 对 withdrawn
 *     受管窗的 map 产生 MapRequest (非 MapNotify), xwm_handle_map_request
 *     发 events.map_request —— 全程无 WM_CHANGE_STATE(Normal)、无
 *     _NET_WM_STATE 翻转, request_minimize(false) 结构性不会发
 *     (wlroots 只在 WM_CHANGE_STATE / _NET_WM_STATE delta 两处发)。
 *   - 前提 (request_minimize 里的 ICCCM 应答): 不回写 WM_STATE=Iconic 时
 *     wine 的 wm_state_serial 不清零, workaround 入口
 *     `if (data->wm_state_serial) return` 让 restore 整体成无操作,
 *     map_request 根本不产生 (234547 轮实锤: 只挂 map_request 监听零触发)。
 *   ⇒ 两半齐备后: minimize → 承载窗最小化; restore → map_request →
 *     Restored → ArkTS FWM.show() (showWindow 是 d.ts 载明的 subWindow
 *     恢复 API) → 承载窗回显。缺任何一半 = 窗口从屏幕消失, fusion
 *     visual-a 红帧缺失。
 * createdPosted 门: 首次 map (创建) 时 created 尚未发 (associate/commit
 * 才补), map_request 穿过不处理; 其后的每次 map_request = wine 主动重显
 * 窗 (还原图标化 / SW_SHOW), 语义「wine 主动显示窗口」→ Restored
 * (bridge 后处理同 activated, ArkTS 动作幂等)。 */
static void handle_map_request(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, map_request);
    (void)data;
    pthread_mutex_lock(&g_lock);
    if (!e->createdPosted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    x11_toplevel_bridge_post_minimized(e->toplevelId, false);
    /* 配对清 minimized (M4b-T4): 不清则随后 map_notify → set_withdrawn(false)
     * 会按 stale minimized=true 写出 WM_STATE=Iconic, wine 收到「刚 map 完
     * 又被最小化」的假象。此刻 withdrawn 仍 true, 本调用先写 Withdrawn,
     * map_notify 的 set_withdrawn(false) 随后写 Normal 收敛 —— 与 wine 自身
     * withdraw→map 序列同形, window_wm_state_notify 状态机按序消化。 */
    wlr_xwayland_surface_set_minimized(e->xs, false);
    pthread_mutex_unlock(&g_lock);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, request_fullscreen);
    (void)data;
    pthread_mutex_lock(&g_lock);
    if (!e->createdPosted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* 信号无 payload, 读 xs 状态位 */
    x11_toplevel_bridge_post_fullscreen(e->toplevelId, e->xs->fullscreen);
    pthread_mutex_unlock(&g_lock);
}

static void handle_request_activate(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, request_activate);
    (void)data;
    pthread_mutex_lock(&g_lock);
    if (!e->createdPosted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* 定案 (plan T1): 不新增枚举 (22 事件红线), 复用 Restored —— 语义
     * 「wine 主动显示窗口」, x11 fusion 的 z 序在 ArkTS 承载窗层, ArkTS
     * restored 动作 = 拉回前台即置前; 与 minimize-restore 同动作幂等。 */
    x11_toplevel_bridge_post_activated(e->toplevelId);
    pthread_mutex_unlock(&g_lock);
}

static void handle_set_parent(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, set_parent);
    (void)data;
    pthread_mutex_lock(&g_lock);
    if (!e->createdPosted) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* Review Focus #3: owner 可能已 destroyed —— owner 在册才报 modal
     * 关系, 不在册降级 (不报, wine 侧行为不受影响)。dx/dy = modal 相对
     * owner 的 guest 坐标差 (对齐 wayland JsonModal 的 PC 定位语义)。 */
    struct x11_xs_entry *owner =
        e->xs->parent ? entry_of_xs(e->xs->parent) : NULL;
    if (owner) {
        x11_toplevel_bridge_post_modal(e->toplevelId, owner->toplevelId,
                                       e->xs->modal,
                                       e->xs->x - owner->xs->x,
                                       e->xs->y - owner->xs->y,
                                       (int32_t)e->xs->width,
                                       (int32_t)e->xs->height);
    } else {
        XTL_LOG("XTL set-parent owner-not-tracked id=%{public}u (degraded)",
                e->toplevelId);
    }
    pthread_mutex_unlock(&g_lock);
}

static void handle_xs_destroy(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, destroy);
    (void)data;
    pthread_mutex_lock(&g_lock);
    if (e->dead) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    e->dead = true;
    detach_listeners(e);
    /* win 引用随 destroyed 事件由 ArkTS 关窗路径回收 (不在此碰
     * NativeWindow); texture 归 client_buffer 所有, 无需我们放。 */
    e->win = NULL;
    x11_toplevel_bridge_post_destroyed(e->toplevelId);
    entry_remove(e);
    pthread_mutex_unlock(&g_lock);
}

void x11_toplevel_set_active(bool on) { g_active = on; }
bool x11_toplevel_active(void) { return g_active; }

void x11_toplevel_notify_new_surface(struct wlr_xwayland_surface *xs)
{
    if (!g_active || !xs) return;
    pthread_mutex_lock(&g_lock);
    if (entry_of_xs(xs) || g_entry_count >= X11_TOPLEVEL_MAX) {
        pthread_mutex_unlock(&g_lock);
        return; /* 重复 new_surface 防御 / 表满 (review minor: 满时静默) */
    }
    struct x11_xs_entry *e = &g_entries[g_entry_count++];
    memset(e, 0, sizeof(*e));
    e->xs = xs;
    e->toplevelId = x11_toplevel_bridge_allocate_id();
    /* 全部 listener 先空链自引 (detach_listeners 前提, 见其注释):
     * surface_commit/dissociate 是 associate 时才 add 的。 */
    wl_list_init(&e->destroy.link);
    wl_list_init(&e->associate.link);
    wl_list_init(&e->dissociate.link);
    wl_list_init(&e->set_title.link);
    wl_list_init(&e->request_configure.link);
    wl_list_init(&e->surface_commit.link);
    wl_list_init(&e->request_minimize.link);
    wl_list_init(&e->request_fullscreen.link);
    wl_list_init(&e->request_activate.link);
    wl_list_init(&e->set_parent.link);
    wl_list_init(&e->map_request.link);
    e->destroy.notify = handle_xs_destroy;
    wl_signal_add(&xs->events.destroy, &e->destroy);
    e->associate.notify = handle_associate;
    wl_signal_add(&xs->events.associate, &e->associate);
    e->dissociate.notify = handle_dissociate;
    wl_signal_add(&xs->events.dissociate, &e->dissociate);
    e->set_title.notify = handle_set_title;
    wl_signal_add(&xs->events.set_title, &e->set_title);
    e->request_configure.notify = handle_request_configure;
    wl_signal_add(&xs->events.request_configure, &e->request_configure);
    e->request_minimize.notify = handle_request_minimize;
    wl_signal_add(&xs->events.request_minimize, &e->request_minimize);
    e->request_fullscreen.notify = handle_request_fullscreen;
    wl_signal_add(&xs->events.request_fullscreen, &e->request_fullscreen);
    e->request_activate.notify = handle_request_activate;
    wl_signal_add(&xs->events.request_activate, &e->request_activate);
    e->set_parent.notify = handle_set_parent;
    wl_signal_add(&xs->events.set_parent, &e->set_parent);
    e->map_request.notify = handle_map_request;
    wl_signal_add(&xs->events.map_request, &e->map_request);
    /* 创建期 title 丢失兜底 (T6 byTitle 实锤): xwm 在 manage 时同步读
     * WM_NAME, set_title 信号可能先于 new_surface 发过 —— 挂 listener 后
     * 补发当前值, 否则 byTitle/按 title 定位在 ArkTS 侧永远找不到窗。 */
    if (xs->title && xs->title[0])
        x11_toplevel_bridge_post_title(e->toplevelId, xs->title);
    /* xs 可能 associate 先于 new_surface 到达 (xwm 时序): 已有 surface 就
     * 立即判定, 否则等 associate 回调。 */
    try_post_created(e);
    pthread_mutex_unlock(&g_lock);
}

void x11_toplevel_session_reset(void)
{
    /* 会话重置 (spec §3.4): 对仍登记的 toplevel 补发 destroyed (ArkTS 清窗
     * 依赖), 再清表。active 位保持 (链重启时 StartWithSurface 按新模式位
     * 重置)。 */
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_entry_count; ++i) {
        struct x11_xs_entry *e = &g_entries[i];
        if (e->dead) continue;
        e->dead = true;
        detach_listeners(e);
        e->win = NULL;
        x11_toplevel_bridge_post_destroyed(e->toplevelId);
    }
    g_entry_count = 0;
    pthread_mutex_unlock(&g_lock);
}

struct wlr_xwayland_surface *x11_toplevel_xs_of(uint32_t toplevelId)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].toplevelId == toplevelId) {
            struct wlr_xwayland_surface *xs = g_entries[i].xs;
            pthread_mutex_unlock(&g_lock);
            return xs;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

uint32_t x11_toplevel_id_of_xs(struct wlr_xwayland_surface *xs)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_xs(xs);
    uint32_t id = e ? e->toplevelId : 0;
    pthread_mutex_unlock(&g_lock);
    return id;
}

/* ── Task 3: per-xs 呈现 ── */

static struct x11_xs_entry *entry_of_id(uint32_t toplevelId)
{
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].toplevelId == toplevelId && !g_entries[i].dead)
            return &g_entries[i];
    }
    return NULL;
}

void x11_toplevel_set_render_ctx(struct wlr_allocator *alloc,
                                 struct wlr_renderer *renderer)
{
    g_alloc = alloc;
    g_renderer = renderer;
}

/* 返回 false = toplevel 已死 (created 与 createRenderer 的竞态, I5):
 * 调用方必须销毁 win, 所有权未转移。 */
bool x11_toplevel_attach_window(uint32_t toplevelId, struct NativeWindow *win,
                                int w, int h)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    e->win = win;
    e->win_w = w;
    e->win_h = h;
    e->skip_count = 0;
    e->dirty = true; /* attach 后强制画一帧 (commit 已过的话别等下一帧) */
    pthread_mutex_unlock(&g_lock);
    XTL_LOG("XTL attach id=%{public}u win=%{public}p %{public}dx%{public}d",
            toplevelId, (void *)win, w, h);
    return true;
}

void x11_toplevel_detach_window(uint32_t toplevelId)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    if (e) e->win = NULL;
    pthread_mutex_unlock(&g_lock);
    if (e) XTL_LOG("XTL detach id=%{public}u", toplevelId);
}

void x11_toplevel_resize_window(uint32_t toplevelId, int w, int h)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    e->win_w = w;
    e->win_h = h;
    /* D51 (真机 175709 实锤): resize 后必须强制重渲染一帧 —— 承载窗
     * attach 时是 1x1, 首帧 render 也在 1x1 上; onSurfaceChanged 更新
     * 尺寸后若 guest 内容静态 (无新 commit), surface 永远停留 1x1 帧
     * = 黑屏。notepad 类活窗被光标闪烁 commit 掩盖, 静态探针必现。 */
    e->dirty = true;
    pthread_mutex_unlock(&g_lock);
}

/* ── Task 5: 按窗输入路由 ── */

/* napi 线程入口 (C1 review 修复): 只入队, loop 线程 dispatch 执行。
 * px/py 是 OHOS 承载窗局部物理像素 (SendPointerEvent 契约), 坐标空间
 * 换算 (物理 → X 逻辑) 在 dispatch 里做 —— 比例取决于承载窗尺寸, 而
 * win_w/win_h 只在 loop 线程稳定。 */
void x11_toplevel_input_pointer(uint32_t toplevelId, int px, int py,
                                int action, uint32_t button)
{
    wl_ohos_input_post_mm_pointer(toplevelId, px, py, action, button);
}

void x11_toplevel_input_key(uint32_t toplevelId, uint32_t keycode,
                            bool press)
{
    wl_ohos_input_post_mm_key(toplevelId, keycode, press);
}

/* loop 线程执行 (InjectQueueDrain 分派): 查表 + 坐标空间换算 + 纪律投递 */
void x11_toplevel_input_pointer_dispatch(uint32_t toplevelId, int px, int py,
                                         int action, uint32_t button)
{
    struct x11_xs_entry *e;
    int lx, ly;
    /* I1: 查表与字段读取持锁 (win_w/win_h 会被 napi 线程 resize_window 写);
     * wl_ohos_input_multimode_pointer 的 seat 操作在锁外 (seat 不属 entry
     * 表, 且持锁调 seat 与 render_tick 的 present 无依赖冲突但没必要)。 */
    pthread_mutex_lock(&g_lock);
    e = entry_of_id(toplevelId);
    if (!e || !e->xs || !e->win) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* I3 (review): 物理像素 → X 逻辑坐标。承载窗尺寸由 onSurfaceChanged
     * 维护 (win_w/win_h); 未校准 (attach 初值 1x1) 时比例未知, 丢弃并
     * 采样留痕 —— 比乱投好 (scale≠1 设备上乱投 = 命中点偏 2 倍)。 */
    if (e->win_w <= 2 || e->win_h <= 2) {
        static unsigned skip_n;
        if (++skip_n % 60 == 1)
            XTL_LOG("XTL mm-ptr skip uncalibrated id=%{public}u win=%{public}dx%{public}d",
                    toplevelId, e->win_w, e->win_h);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    lx = (int)((int64_t)px * (int)e->xs->width / e->win_w);
    ly = (int)((int64_t)py * (int)e->xs->height / e->win_h);
    /* 缝隙不投递 (spec §4): 换算后越界 = 点在窗间空隙/标题条外 */
    if (lx < 0 || ly < 0 ||
        lx >= (int)e->xs->width || ly >= (int)e->xs->height) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_mutex_unlock(&g_lock);
    wl_ohos_input_multimode_pointer(e->xs, (double)lx, (double)ly,
                                    action, button);
}

void x11_toplevel_input_key_dispatch(uint32_t toplevelId, uint32_t keycode,
                                     bool press)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    struct wlr_xwayland_surface *xs = e ? e->xs : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!xs) return;
    wl_ohos_input_multimode_key(xs, keycode, press);
}

/* 画一帧: surface->buffer 的 texture (wlroots 自管) → 该窗队列 buffer →
 * present。dst 尺寸取队列 buffer 本体 (ArkTS 侧窗口多大, 队列 buffer 就
 * 多大), 源整幅拉伸 —— 首版不做 aspect/裁剪策略 (spec §3.2 呈现走最短
 * 路径)。 */
/* 返回 false = 本帧没画出去 (M8: 调用方保留 dirty 下一帧重试)。
 * nocb (client 提前释放 buffer) 不算失败 —— 窗口内容已亡, 无帧可画。 */
static bool render_entry(struct x11_xs_entry *e)
{
    if (!g_renderer) return true;
    /* unmap (close/minimize) 走 dissociate: surface 置 NULL 但 dirty 可能
     * 残留 (dissociate 前的末批 commit / M8 失败重试保留) —— 与本文件
     * try_post_created/render_tick 的 surface 判空同款, 缺此守卫 = compositor
     * loop 线程空指针崩溃 (final review C1, M4b 最小化路径必踩)。返回
     * false = 本帧没画出去, dirty 保留, remap 后首帧重绘。 */
    if (!e->xs->surface) return false;
    struct wlr_client_buffer *cb = e->xs->surface->buffer;
    if (!cb || !cb->texture) {
        XTL_LOG("XTL render-skip nocb id=%{public}u cb=%{public}p",
                e->toplevelId, (void *)cb);
        return true;
    }
    XTL_LOG("XTL render id=%{public}u tex=%{public}p %dx%d -> dst",
            e->toplevelId, (void *)cb->texture,
            cb->base.width, cb->base.height);
    /* 逐帧 render 日志: commit 驱动 (静止窗零日志), 频率安全。D50 排查期
     * 保留 —— 收口时降为失败路径日志。 */

    struct wlr_swapchain *swapchain = NULL;
    struct wlr_buffer *dst = wl_ohos_present_slot_acquire(e->win, &swapchain);
    if (!dst) {
        XTL_LOG("XTL slot-fail id=%{public}u", e->toplevelId);
        return false;
    }
    XTL_LOG("XTL render id=%{public}u dst=%{public}p %dx%d",
            e->toplevelId, (void *)dst, dst->width, dst->height);

    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(g_renderer, dst, NULL);
    if (!pass) {
        wl_ohos_present_buffer_abort(dst);
        XTL_LOG("XTL pass-fail id=%{public}u", e->toplevelId);
        return false;
    }

    struct wlr_render_texture_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.texture = cb->texture;
    opts.dst_box.x = 0;
    opts.dst_box.y = 0;
    opts.dst_box.width = dst->width;
    opts.dst_box.height = dst->height;
    /* 0 不是合法 transfer function (SRGB = 1<<0), 必须显式给 */
    opts.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_SRGB;
    wlr_render_pass_add_texture(pass, &opts);

    if (!wlr_render_pass_submit(pass)) {
        wl_ohos_present_buffer_abort(dst);
        XTL_LOG("XTL submit-fail id=%{public}u", e->toplevelId);
        return false;
    }

    int32_t rc = wl_ohos_present_buffer_present(dst, -1);
    if (rc != 0) {
        /* 未归还的 buffer 销毁时才兜底 abort (只留日志), 显式归还防槽位泄漏 */
        if (!wl_ohos_present_buffer_returned(dst))
            wl_ohos_present_buffer_abort(dst);
        XTL_LOG("XTL present-fail rc=%{public}d id=%{public}u", (int)rc, e->toplevelId);
        return false;
    }
    XTL_LOG("XTL present ok id=%{public}u", e->toplevelId);
    return true;
}

void x11_toplevel_render_tick(void)
{
    if (!g_active) return;
    /* D50: multiwindow 呈现绕过 wlr_output（手动 render pass + OH_NativeWindow
     * present），没人回发客户端 frame callback —— Xwayland 的 damage 提交被
     * pending frame 节流（xserver xwayland-screen.c block handler：
     * frame_callback 非空则 continue 跳过提交），首帧 commit 后永久冻结，
     * xs->buffer 停在初始全 0。此处角色 = 真合成器的 output frame：每帧对
     * 全部活动 xs 泵 frame done，机制与 normal 路径 ohos_output.c 的
     * g_clients 直发一致。 */
    /* I1: 全程持锁 (render_entry 持锁执行, napi attach 最多等一帧 ——
     * 见 g_lock 注释)。 */
    pthread_mutex_lock(&g_lock);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (size_t i = 0; i < g_entry_count; ++i) {
        struct x11_xs_entry *e = &g_entries[i];
        if (!e->dead && e->xs && e->xs->surface)
            wlr_surface_send_frame_done(e->xs->surface, &now);
    }
    for (size_t i = 0; i < g_entry_count; ++i) {
        struct x11_xs_entry *e = &g_entries[i];
        if (e->dead) continue;
        /* created 补发兜底: buffer 在 attach 之后才到 (associate/commit 都
         * 已试过) 的时序由帧钟收口。 */
        try_post_created(e);
        if (!e->win || !e->dirty) {
            if (!e->win && ++e->skip_count % 600 == 0)
                XTL_LOG("XTL skip-no-window id=%{public}u n=%{public}u",
                        e->toplevelId, (unsigned)e->skip_count);
            continue;
        }
        e->dirty = false;
        if (!render_entry(e)) {
            /* M8 (review): 本帧没画出去 (slot 借不到/pass 失败) —— 保留
             * dirty, 下一帧重试; 否则静止窗停在旧帧无自愈。 */
            e->dirty = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

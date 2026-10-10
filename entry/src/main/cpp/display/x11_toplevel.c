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
void x11_toplevel_bridge_post_created(uint32_t id, int32_t w, int32_t h);
void x11_toplevel_bridge_post_title(uint32_t id, const char *title);
void x11_toplevel_bridge_post_resize(uint32_t id, int32_t w, int32_t h);
void x11_toplevel_bridge_post_destroyed(uint32_t id);
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
};

static struct x11_xs_entry g_entries[X11_TOPLEVEL_MAX];
static size_t g_entry_count;
static bool g_active = false;
static struct wlr_allocator *g_alloc;   /* chain_start 注入 (buffer 目标) */
static struct wlr_renderer *g_renderer; /* render pass 用 */

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
    wl_list_init(&e->destroy.link);   /* 防 double-remove (session_reset 后
                                       * destroy 再到的悬挂回调安全化) */
    wl_list_init(&e->associate.link);
    wl_list_init(&e->dissociate.link); /* M11 (review): 与其余 listener 同款
                                        * 重置, 维持「全空链自引」不变量 */
    wl_list_init(&e->set_title.link);
    wl_list_init(&e->request_configure.link);
    wl_list_init(&e->surface_commit.link);
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

static void handle_surface_commit(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, surface_commit);
    (void)data;
    e->dirty = true;
    /* 首帧判定 (wayland PC 模式同款延后语义): commit 可能先于 associate ——
     * buffer 就位即补发 created, 不漏首帧。 */
    try_post_created(e);
}

static void handle_associate(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, associate);
    (void)data;
    /* commit 监听挂 surface (associate 才 valid); dissociate 时摘除, 防
     * surface 销毁后悬挂。 */
    if (e->xs->surface) {
        e->surface_commit.notify = handle_surface_commit;
        wl_signal_add(&e->xs->surface->events.commit, &e->surface_commit);
    }
    try_post_created(e);
}

static void handle_dissociate(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, dissociate);
    (void)data;
    wl_list_remove(&e->surface_commit.link);
    wl_list_init(&e->surface_commit.link);
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
    const struct wlr_xwayland_surface_configure_event *ev = data;
    /* 应用应答必须发 (T9 教训: 不应答 commit 滞留 cached state 窗口不上
     * 屏)。多窗模式 X 侧几何跟随应用请求 (无 scene 布局器); ArkTS 权威
     * 尺寸回写走 ResizeRenderer, 这里只上送 resize 语义事件。 */
    wlr_xwayland_surface_configure(e->xs, ev->x, ev->y, ev->width, ev->height);
    if (!e->createdPosted) return;
    x11_toplevel_bridge_post_resize(
        e->toplevelId, (int32_t)ev->width, (int32_t)ev->height);
}

static void handle_xs_destroy(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, destroy);
    (void)data;
    if (e->dead) return;
    e->dead = true;
    detach_listeners(e);
    /* win 引用随 destroyed 事件由 ArkTS 关窗路径回收 (不在此碰
     * NativeWindow); texture 归 client_buffer 所有, 无需我们放。 */
    e->win = NULL;
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
    /* 全部 listener 先空链自引 (detach_listeners 前提, 见其注释):
     * surface_commit/dissociate 是 associate 时才 add 的。 */
    wl_list_init(&e->destroy.link);
    wl_list_init(&e->associate.link);
    wl_list_init(&e->dissociate.link);
    wl_list_init(&e->set_title.link);
    wl_list_init(&e->request_configure.link);
    wl_list_init(&e->surface_commit.link);
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
    /* 创建期 title 丢失兜底 (T6 byTitle 实锤): xwm 在 manage 时同步读
     * WM_NAME, set_title 信号可能先于 new_surface 发过 —— 挂 listener 后
     * 补发当前值, 否则 byTitle/按 title 定位在 ArkTS 侧永远找不到窗。 */
    if (xs->title && xs->title[0])
        x11_toplevel_bridge_post_title(e->toplevelId, xs->title);
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
        e->win = NULL;
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
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    if (!e) return false;
    e->win = win;
    e->win_w = w;
    e->win_h = h;
    e->skip_count = 0;
    e->dirty = true; /* attach 后强制画一帧 (commit 已过的话别等下一帧) */
    XTL_LOG("XTL attach id=%{public}u win=%{public}p %{public}dx%{public}d",
            toplevelId, (void *)win, w, h);
    return true;
}

void x11_toplevel_detach_window(uint32_t toplevelId)
{
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    if (!e) return;
    e->win = NULL;
    XTL_LOG("XTL detach id=%{public}u", toplevelId);
}

void x11_toplevel_resize_window(uint32_t toplevelId, int w, int h)
{
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    if (!e) return;
    e->win_w = w;
    e->win_h = h;
    /* D51 (真机 175709 实锤): resize 后必须强制重渲染一帧 —— 承载窗
     * attach 时是 1x1, 首帧 render 也在 1x1 上; onSurfaceChanged 更新
     * 尺寸后若 guest 内容静态 (无新 commit), surface 永远停留 1x1 帧
     * = 黑屏。notepad 类活窗被光标闪烁 commit 掩盖, 静态探针必现。 */
    e->dirty = true;
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
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    int lx, ly;
    if (!e || !e->xs || !e->win) return;
    /* I3 (review): 物理像素 → X 逻辑坐标。承载窗尺寸由 onSurfaceChanged
     * 维护 (win_w/win_h); 未校准 (attach 初值 1x1) 时比例未知, 丢弃并
     * 采样留痕 —— 比乱投好 (scale≠1 设备上乱投 = 命中点偏 2 倍)。 */
    if (e->win_w <= 2 || e->win_h <= 2) {
        static unsigned skip_n;
        if (++skip_n % 60 == 1)
            XTL_LOG("XTL mm-ptr skip uncalibrated id=%{public}u win=%{public}dx%{public}d",
                    toplevelId, e->win_w, e->win_h);
        return;
    }
    lx = (int)((int64_t)px * (int)e->xs->width / e->win_w);
    ly = (int)((int64_t)py * (int)e->xs->height / e->win_h);
    /* 缝隙不投递 (spec §4): 换算后越界 = 点在窗间空隙/标题条外 */
    if (lx < 0 || ly < 0 ||
        lx >= (int)e->xs->width || ly >= (int)e->xs->height)
        return;
    wl_ohos_input_multimode_pointer(e->xs, (double)lx, (double)ly,
                                    action, button);
}

void x11_toplevel_input_key_dispatch(uint32_t toplevelId, uint32_t keycode,
                                     bool press)
{
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    if (!e || !e->xs) return;
    wl_ohos_input_multimode_key(e->xs, keycode, press);
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
}

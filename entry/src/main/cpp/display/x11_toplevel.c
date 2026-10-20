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
#include <wlr/render/wlr_texture.h> /* MW-GPU: wlr_texture_from_buffer (guest 帧) */
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
/* M4c-T1: x11 popup (override_redirect 窗) 四事件; 事件 id = owner
 * toplevelId, popup_id 走独立基址空间 (下方 X11_POPUP_ID_BASE) */
void x11_toplevel_bridge_post_popup_show(uint32_t parent_id, uint32_t popup_id,
                                         int32_t x, int32_t y,
                                         int32_t w, int32_t h, int32_t argb01);
void x11_toplevel_bridge_post_popup_move(uint32_t parent_id, uint32_t popup_id,
                                         int32_t x, int32_t y);
void x11_toplevel_bridge_post_popup_resize(uint32_t parent_id, uint32_t popup_id,
                                           int32_t w, int32_t h);
void x11_toplevel_bridge_post_popup_hide(uint32_t parent_id, uint32_t popup_id);
uint32_t x11_toplevel_bridge_allocate_id(void);

/* OHOS 侧日志 (hilog); ohos_output.c 的 OHLOG 是 static 宏, 本文件自取。
 * LOG_DOMAIN/LOG_TAG 必须在 include 前 define —— 缺省时 OH_LOG_INFO 展开
 * 出的 tag 为空, 日志整条丢弃 (T3 真机实测: skip 采样日志 0 条)。 */
#define LOG_DOMAIN 0x0000
#define LOG_TAG "x11-toplevel"
#include <hilog/log.h>
#define XTL_LOG(...) ((void)OH_LOG_INFO(LOG_APP, __VA_ARGS__))

#define X11_TOPLEVEL_MAX 64

/* M4c-T1: x11 popup id 独立基址 (plan RF#2)。ArkTS 侧 wayland popupId 与
 * x11 toplevelId 同出一个递增计数器 (AllocateToplevelId), 三个 id 空间在
 * setPendingToplevel/渲染器表汇合 —— popup 用 1<<20 基址避开两个既有空间
 * (计数器从 1 递增, 会话内不可达 2^20)。 */
#define X11_POPUP_ID_BASE (1u << 20)
static uint32_t g_popup_id_seq;

struct x11_xs_entry {
    struct wlr_xwayland_surface *xs;
    /* 语义按 is_popup 分叉: toplevel = AllocateToplevelId 分配的 id;
     * popup = X11_POPUP_ID_BASE+seq。ArkTS 侧 createRenderer/输入/渲染
     * 全按此 id 查表, popup 与 toplevel 共用同一查表路径。 */
    uint32_t toplevelId;
    bool is_popup;      /* OR 窗 (xs->override_redirect): popup 分支 ——
                         * 不发 created、不进 toplevel 事件流, 事件面 =
                         * popup_show/move/resize/hide 四事件 */
    bool createdPosted; /* created 只发一次 (associate + 首帧判定的入口去重) */
    bool dead;          /* destroy 已到: session_reset 时不再重复补发 */
    bool dirty;         /* surface commit 待渲染 (render_tick 消费) */
    uint32_t skip_count; /* 未 attach 窗口的跳帧计数 (采样日志) */
    /* MW-GPU: 记录身份序号 —— guest 帧路由的 generation 判据 (fusion 无
     * g_clients, 窗口身份由本表提供)。分配时自增, 终身不变; 墓碑槽复用
     * 换新记录即换新序号 ("同 id 是否新窗" 单点判据)。 */
    uint64_t generation;
    /* MW-GPU: fusion 下 guest gfx 零拷贝帧 (venus/virgl present)。desktop
     * 路线帧进 scene 图块; fusion 无 scene 提交, frame_set 路由到本字段,
     * render_entry 有它时优先画 (GDI 窗无此帧, 仍走 surface->buffer)。
     * 引用语义 = 本 entry lock 一份, 替换/entry 移除时 unlock。 */
    struct wlr_buffer *guestFrame;
    int guestFlip;      /* 1 = 纵向翻 (GL/virgl), 0 = 不翻 (Vulkan/venus) ——
                         * 与 scene 路径 frame_set 的 FLIPPED_180 口径一致 */
    /* MW-GPU: guestFrame 的纹理, set 时建、替换/移除时 destroy —— 与 scene
     * 节点 set_buffer 同款配对 (帧 wrapper 每 acquire 新建, 纹理无法跨帧
     * 缓存; 只建不毁 = GL/EGL 泄漏, 设备实测 60 帧/秒 50s 即触发内存清理)。 */
    struct wlr_texture *guestTexture;
    /* M4c-T1: popup 事件状态 (仅 is_popup 条目使用) */
    bool popup_visible;      /* popup_show 已发 (unmap/destroy 双触发 hide 的去重) */
    uint32_t popup_parent_id; /* show 时用的 owner toplevelId (hide 事件 id) */
    struct wlr_xwayland_surface *popup_owner_xs; /* show 时的 owner (焦点还原
                         * 用; hide 时经 entry_of_xs 复验存活) */
    bool popup_owner_logged; /* owner 缺失的降级日志只打一次 */
    int popup_px, popup_py, popup_pw, popup_ph; /* 已上报几何 (move/resize 去重) */
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
    struct wl_listener set_window_type; /* #167: WINDOW_TYPE 属性变化
                                          * (modal 判据的对话框声明半边) */
    struct wl_listener set_hints; /* #167: WM_HINTS 变化 = owner 禁用态
                                    * 翻转 (modal 判据的第二半边修正通道) */
    struct wl_listener map_request; /* M4b-T4: wine 还原窗口的唯一宿主信号 */
    /* M4c-T1: OR 窗几何变化 (wine XMoveResizeWindow → ConfigureNotify →
     * set_geometry)。request_configure 对 OR 窗结构性不发 (X server 直通
     * 应答, 无 MapRequest/ConfigureRequest 事件), move/resize 信号源只有
     * set_geometry (xwm.c:1512)。 */
    struct wl_listener set_geometry;
    /* 任务 #135: host→wine 回写的 echo 抑制基准 —— 最近一次回写目标
     * (x11_toplevel_geometry_*_dispatch 写, handle_request_configure 读)。
     * 仅抑制「wine 回声我们刚写的值」这类无操作上送, 不掩盖 app 真实
     * 改几何 (判据 = ε=2px, 依据见 handle_request_configure 注释)。 */
    bool wb_valid;
    int wb_x, wb_y, wb_w, wb_h;
};

static struct x11_xs_entry g_entries[X11_TOPLEVEL_MAX];
static size_t g_entry_count;
static uint64_t g_entryGeneration; /* 记录身份序号 (见 x11_xs_entry.generation) */
static bool g_active = false;
static struct wlr_allocator *g_alloc;   /* chain_start 注入 (buffer 目标) */
static struct wlr_renderer *g_renderer; /* render pass 用 */

/* entry 表锁 (M4a 收口 final review I1): 表在两个线程被访问 ——
 *   loop 线程: listener 回调 / render_tick / session_reset / 输入 dispatch
 *   napi 线程: attach/detach/resize_window (plugin_manager 承载窗回绑)
 * entry_remove 的 memmove 会让内嵌 entry 移位, 跨线程并发查表+写字段 =
 * 错窗回绑/形式数据竞态。一把非递归大锁; 纪律:
 *   - 公开入口全加锁; static 内部函数 (entry_of_xxx、entry_remove、
 *     detach_listeners、try_post_created、try_post_popup_show、
 *     post_popup_hide、render_entry、owned_dialog_is_modal、
 *     post_modal_relation) 约定调用方持锁, 不加锁
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

/* #167: 「owned 对话框且模态」判据, 单一实现 (modal_blocks 扫描与
 * post_modal_relation 上报共用; 评审 I1 修正后形态)。
 * 两半缺一不可:
 *  1. 对话框声明: xs->modal (_NET_WM_STATE_MODAL, EWMH §5.11 标准客户端)
 *     ∨ WINDOW_TYPE_DIALOG ∨ EWMH §5.6 缺省 (owned 无 WINDOW_TYPE 视作
 *     DIALOG; 仅 created 后应用 —— wlroots 属性批次在 associate 同步读,
 *     xwm.c:1341-1356, 之前 window_type_len 恒 0 不冒认)。wine 对 owned
 *     WS_POPUP ∨ WS_EX_DLGMODALFRAME 一律写 DIALOG (winex11.drv
 *     window.c:1170) —— DIALOG 半边单用会把 modeless 对话框也圈进来。
 *  2. owner 禁用: wine 的模态执行 = DialogBoxParam → EnableWindow(owner,
 *     FALSE), 且禁用态写进 owner 的 WM_HINTS.input=0 (winex11.drv
 *     window.c:1176, input = !(style & WS_DISABLED)) —— 这是 X 侧可达的
 *     win32 禁用镜像, 与 wayland 路线判据 (winewayland.drv/modal.c:
 *     WS_EX_DLGMODALFRAME + owner WS_DISABLED) 同源。modeless 对话框
 *     (CreateDialog, owner 未禁用) 由此不再被误判模态。
 *     hints 未读 (parent->hints == NULL) 按禁用对待 = 保守阻断;
 *     禁用态翻转经 set_hints 信号 (handle_set_hints) 重报修正。 */
static bool owned_dialog_is_modal(const struct x11_xs_entry *e)
{
    if (!e->createdPosted || e->is_popup || !e->xs->parent)
        return false;
    if (e->xs->parent->hints && e->xs->parent->hints->input)
        return false; /* owner 未禁用 = modeless, 不阻断 */
    return e->xs->modal ||
        wlr_xwayland_surface_has_window_type(
            e->xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
        e->xs->window_type_len == 0;
}

bool x11_toplevel_modal_blocks(const struct wlr_xwayland_surface *target)
{
    /* 任务 #167: 模态阻断查询 —— target 的祖先链 (经在册 entry 逐跳) 上
     * 有活的模态对话框 → 阻断。判据单点在 owned_dialog_is_modal 注释。
     * 查询时无状态扫描 (禁用态翻转走 set_hints 信号重报, 不在此维护
     * 缓存态); 重入安全: trylock fail-open (popup 持锁路径调 FocusClient
     * 时放行, popup/OR 窗非 owned 管理窗, 放行即正确)。行走只经
     * entry_of_xs (仅返回存活注册者)。 */
    if (!target)
        return false;
    if (pthread_mutex_trylock(&g_lock) != 0)
        return false; /* 重入或竞争: fail-open */
    bool blocked = false;
    for (size_t i = 0; i < g_entry_count && !blocked; ++i) {
        const struct x11_xs_entry *e = &g_entries[i];
        if (e->dead || !e->xs || !e->xs->parent || e->is_popup)
            continue;
        if (!owned_dialog_is_modal(e))
            continue;
        const struct wlr_xwayland_surface *p = e->xs->parent;
        for (int depth = 0; p != NULL && depth < 8; ++depth) {
            if (p == target) {
                blocked = true;
                break;
            }
            const struct x11_xs_entry *pe =
                entry_of_xs((struct wlr_xwayland_surface *)(uintptr_t)p);
            p = pe ? pe->xs->parent : NULL;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return blocked;
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
    wl_list_remove(&e->set_window_type.link);
    wl_list_remove(&e->set_hints.link);
    wl_list_remove(&e->map_request.link);
    wl_list_remove(&e->set_geometry.link);
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
    wl_list_init(&e->set_window_type.link);
    wl_list_init(&e->set_hints.link);
    wl_list_init(&e->map_request.link);
    wl_list_init(&e->set_geometry.link);
}

static void entry_remove(struct x11_xs_entry *e)
{
    /* 不 memmove (M4c-T4 现场实锤, cppcrash 20261011033916): entry 内嵌的
     * wl_listener link 节点地址被活跃信号链 (xs->events.destroy 等) 直接
     * 引用, memmove 挪动 entry = 其余 entry 的 link 全部指向错位地址 ——
     * 下一个销毁 surface 的 destroy 回调走不到, wlroots
     * xwayland_surface_destroy 的 assert(wl_list_empty(destroy
     * listener_list)) (xwm.c:701) 必炸, app 整进程 SIGABRT (组合器死 =
     * 全部 wine 子进程陪葬)。M4a 起每轮 fusion 收尾都在炸 (单用例套件
     * summary 先落地被掩盖, faultlogger cppcrash-app.hackeris.winehua 自
     * 20261010041412 起一路有账); M4c-T4 套件扩成多用例后 crash 移进跑批
     * 中段才现形。改为墓碑槽 + 复用, entry 地址终身稳定。前置: 两个死亡
     * 入口 (handle_xs_destroy / session_reset) 都先 detach_listeners 再
     * 进这里, 槽内 link 已全部空链自引。 */
    e->dead = true;
    e->xs = NULL;
    e->win = NULL;
    /* MW-GPU: 释放 guest 帧引用 (lock 在 x11_toplevel_set_guest_frame) 与
     * 配对纹理; 槽会被 entry_alloc 复用, 不清则新窗继承旧帧/泄漏纹理。
     * 纹理先拆 (EGLImage 引用缓冲内存) 再解锁缓冲。 */
    if (e->guestTexture) {
        wlr_texture_destroy(e->guestTexture);
        e->guestTexture = NULL;
    }
    if (e->guestFrame) {
        wlr_buffer_unlock(e->guestFrame);
        e->guestFrame = NULL;
    }
}

/* 空槽分配: 优先复用墓碑槽 (地址稳定, 见 entry_remove 注释), 无则追加。 */
static struct x11_xs_entry *entry_alloc(void)
{
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].dead && g_entries[i].xs == NULL)
            return &g_entries[i];
    }
    if (g_entry_count >= X11_TOPLEVEL_MAX) return NULL;
    return &g_entries[g_entry_count++];
}

/* ── #167: modal 关系上报（单一实现, 四处触发）──
 * 前提: 调用方持 g_lock。created 未发 / popup / 无 parent → 不发。
 * modal 值判据单点 = owned_dialog_is_modal (对话框声明 ∧ owner 禁用,
 * 评审 I1 后形态, 见其注释)。
 * 触发时机缺一不可 (x11 协议时序实测推演):
 *   1. try_post_created —— 常规对话框流的唯一有效时机: wine 在 map 前
 *      同一 round trip 内先写 TRANSIENT_FOR (window.c:1162) 再写
 *      WINDOW_TYPE (1167+), xwm 按请求序处理 → set_parent 信号到达时
 *      DIALOG 类型尚不可读, set_window_type 到达时 created 尚未落地,
 *      两个信号 handler 的 createdPosted 门都会拦下; created 落地时
 *      parent+类型均已终态, 在这里补报。
 *   2. set_parent —— owner 关系在 created 后才建立的路径。
 *   3. set_window_type —— 样式后变 (SetWindowLong 改 DLGMODALFRAME 等)
 *      或类型信号晚于 created 的路径。双向: 变回 NORMAL 时发 modal=0,
 *      ArkTS 侧拆模态承载。
 *   4. set_hints —— owner 禁用态翻转 (DialogBoxParam 的 EnableWindow /
 *      关框前 re-enable) 晚于 created 的路径, handle_set_hints 对自身
 *      与以自身为 owner 的对话框重报。
 * (wayland 路线无此问题: wine 经 winehua_toplevel 协议随 WindowPosChanged
 * 反复重发 modal, x11 的 X 属性只写一次, 时机必须自己补齐。) */
static void post_modal_relation(struct x11_xs_entry *e)
{
    if (!e->createdPosted || e->is_popup || !e->xs->parent)
        return;
    struct x11_xs_entry *owner = entry_of_xs(e->xs->parent);
    if (!owner) {
        XTL_LOG("XTL modal owner-not-tracked id=%{public}u (degraded)",
                e->toplevelId);
        return;
    }
    bool modal_flag = owned_dialog_is_modal(e);
    /* dx/dy = 相对 owner 的 guest 坐标差 (对齐 wayland JsonModal 的
     * PC 定位语义); Review Focus #3: owner 在册才报, 不在册降级。 */
    x11_toplevel_bridge_post_modal(e->toplevelId, owner->toplevelId,
                                   modal_flag,
                                   e->xs->x - owner->xs->x,
                                   e->xs->y - owner->xs->y,
                                   (int32_t)e->xs->width,
                                   (int32_t)e->xs->height);
}

/* created 时机 (spec §3.1): associate + 首帧 buffer 存在 —— 对齐 wayland
 * PC 模式的延后语义 (wl_core.cpp:639 同款)。associate 时 surface 已 valid
 * (wlroots xwayland.h:126), buffer 未到则等后续 associate/commit 补发。 */
static void try_post_created(struct x11_xs_entry *e)
{
    if (e->createdPosted || !e->xs->surface || !e->xs->surface->buffer) return;
    e->createdPosted = true;
    /* #167: modal 关系补报, 且必须先于 created 事件 —— ArkTS 按事件 FIFO
     * 消费, modal=1 先到才把 id 记入 modalToplevelIds, 随后的 created 被
     * 事件入口拦截, 不建常规承载; 反序则 created 抢建常规承载窗 (幽灵),
     * modal 到达时 ghost 守卫见 starting=true 拒删, 与 ModalWindowManager
     * 的模态子窗并存成双窗, 渲染器只挂其一, 另一窗黑屏 (2026-10-17 真机
     * 实测, 日志 #14: created.586 → modal.588 → FWM subWinId + MODAL shown
     * 并存)。wayland 路线天然 modal 先于 created (wine 首帧前发 modal,
     * created 等首帧), x11 的上报顺序自己摆平。created 此刻 parent+
     * WINDOW_TYPE 均已终态, 是常规对话框流的唯一有效上报时机
     * (post_modal_relation 注释的三时机推演)。 */
    post_modal_relation(e);
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

/* ── M4c-T1: popup (override_redirect 窗) 事件翻译 ──
 *
 * 生命周期信号 (wlroots xwayland 对 OR 窗无 map/unmap 信号, 实读 xwm.c):
 *   - show ≈ associate (wl_surface 绑定) + buffer 到位 —— 与 toplevel 的
 *     created 判定同构 (try_post_created 同款门)。X map 对 OR 窗不发
 *     MapRequest (X server 直通, xwm_handle_map_request 不会到达), 宿主
 *     可观测的首个信号即 associate/commit。
 *   - hide ≈ dissociate —— X unmap 与 destroy 都经 xwayland_surface_dissociate
 *     (xwm.c:668, unmap_notify 与 destroy 两路都调; OR 窗不走 withdrawn),
 *     surface 置 NULL 一站覆盖 plan RF#4 的双终态。
 *   - move/resize ≈ set_geometry (ConfigureNotify, xwm.c:1512)。
 * 坐标: xs->x/y 是 X 全局坐标; ArkTS PopupWindowManager.positionEntry 按
 * 「父窗口全局原点 + off×scale」定位 (offX/offY 语义 = 相对父窗口原点,
 * wayland 路线同款), 故这里换算成父相对偏移再上报 —— plan 架构句「X 全局
 * 坐标直传」按消费端语义落为「父相对直传」, 换算单点在 show/geometry 两处;
 * 全局定位由父窗定位链保证 (FWM 承载窗按 created 的 guest 坐标摆放,
 * WineWindowManager.startFusionSubWindow D52)。 */
/* owner 链上行: 沿 xs->parent 找最近的「已登记 toplevel」条目。子菜单的
 * TRANSIENT_FOR 直接指向父菜单窗 (OR popup), 而 ArkTS PopupWindowManager
 * 的定位/级联都以 toplevelId 为键 (parentWin = entries.get(id), popup id
 * 查不到 → 事件永久积压), 故 popup 事件一律挂到根 toplevel 上, 偏移按
 * 根 toplevel 原点计。链上窗未登记 (路线切换前创建等) → NULL 降级。 */
static struct x11_xs_entry *popup_root_owner(struct wlr_xwayland_surface *xs)
{
    struct wlr_xwayland_surface *p = xs->parent;
    while (p) {
        struct x11_xs_entry *pe = entry_of_xs(p);
        if (!pe) return NULL;
        if (!pe->is_popup) return pe;
        p = p->parent;
    }
    return NULL;
}

static void try_post_popup_show(struct x11_xs_entry *e)
{
    if (e->popup_visible || !e->xs->surface || !e->xs->surface->buffer) return;
    struct x11_xs_entry *owner = popup_root_owner(e->xs);
    if (!owner) {
        /* 退化: owner 不在册 (无 TRANSIENT_FOR hint / owner 未登记)。
         * 菜单无父链不可承载 (ArkTS 定位/级联销毁都以 parentToplevel 为键),
         * 丢弃并留一次痕; 后续 commit 到达会重试 (属性读取与 commit 的
         * 时序竞态自愈)。wine 菜单窗带 owner 创建 (win32u/menu.c:3357) 且
         * set_style_hints 无条件写 TRANSIENT_FOR (window.c:1162), 常规
         * 路径不进此分支。 */
        if (!e->popup_owner_logged) {
            e->popup_owner_logged = true;
            XTL_LOG("XTL popup owner-not-tracked id=%{public}u (drop show)",
                    e->toplevelId);
        }
        return;
    }
    e->popup_visible = true;
    e->popup_parent_id = owner->toplevelId;
    e->popup_owner_xs = owner->xs;
    e->popup_px = (int)e->xs->x - (int)owner->xs->x;
    e->popup_py = (int)e->xs->y - (int)owner->xs->y;
    e->popup_pw = (int)e->xs->width;
    e->popup_ph = (int)e->xs->height;
    x11_toplevel_bridge_post_popup_show(
        e->popup_parent_id, e->toplevelId,
        e->popup_px, e->popup_py, e->popup_pw, e->popup_ph,
        e->xs->has_alpha ? 1 : 0);
    /* seat 焦点让渡 (plan T2 / RF#3): 键盘焦点随菜单走; activate 对 OR 窗
     * 在 xwm 侧结构性 no-op (xwm_surface_activate 的 OR 门), keyboard
     * enter 照发。菜单打开期间的指针事件由 popup 承载子窗直送 (multimode
     * 口), 不依赖 seat 焦点。 */
    wl_ohos_input_focus_xs(e->xs);
    XTL_LOG("XTL popup-show id=%{public}u parent=%{public}u off=(%{public}d,%{public}d) %{public}ux%{public}u",
            e->toplevelId, e->popup_parent_id,
            e->popup_px, e->popup_py, e->xs->width, e->xs->height);
}

static void post_popup_hide(struct x11_xs_entry *e)
{
    if (!e->popup_visible) return;
    e->popup_visible = false;
    x11_toplevel_bridge_post_popup_hide(e->popup_parent_id, e->toplevelId);
    /* 焦点还 owner (plan T2): 只还在册且存活的 owner; owner 已死时 seat
     * 键盘焦点随 popup surface 销毁自清 (display_input TrackKbdFocus 的
     * destroy 监听), 无悬挂引用。 */
    struct x11_xs_entry *owner =
        e->popup_owner_xs ? entry_of_xs(e->popup_owner_xs) : NULL;
    if (owner) wl_ohos_input_focus_xs(owner->xs);
    e->popup_owner_xs = NULL;
    XTL_LOG("XTL popup-hide id=%{public}u parent=%{public}u",
            e->toplevelId, e->popup_parent_id);
}

/* OR 窗几何变化 (wine XMoveResizeWindow → ConfigureNotify)。去重按已上报
 * 值 (set_geometry 在鼠标 hover 高亮等纯重绘时不发, 只在几何真变时发)。 */
static void handle_popup_geometry(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, set_geometry);
    (void)data;
    pthread_mutex_lock(&g_lock);
    if (!e->popup_visible) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    int nx = e->popup_px, ny = e->popup_py;
    const int nw = (int)e->xs->width, nh = (int)e->xs->height;
    struct x11_xs_entry *owner = popup_root_owner(e->xs);
    if (owner) {
        nx = (int)e->xs->x - (int)owner->xs->x;
        ny = (int)e->xs->y - (int)owner->xs->y;
    } /* owner 已死: 保持上次父相对偏移 (菜单随父亡, 下一拍 hide 收尾) */
    if (nx != e->popup_px || ny != e->popup_py) {
        e->popup_px = nx;
        e->popup_py = ny;
        x11_toplevel_bridge_post_popup_move(e->popup_parent_id, e->toplevelId,
                                            nx, ny);
    }
    if (nw != e->popup_pw || nh != e->popup_ph) {
        e->popup_pw = nw;
        e->popup_ph = nh;
        x11_toplevel_bridge_post_popup_resize(e->popup_parent_id, e->toplevelId,
                                              nw, nh);
    }
    pthread_mutex_unlock(&g_lock);
}

static void handle_surface_commit(struct wl_listener *listener, void *data)
{
    struct x11_xs_entry *e =
        wl_container_of(listener, e, surface_commit);
    (void)data;
    pthread_mutex_lock(&g_lock);
    e->dirty = true;
    /* 首帧判定 (wayland PC 模式同款延后语义): commit 可能先于 associate ——
     * buffer 就位即补发 created, 不漏首帧。popup 分支 = show 判定 (同款门)。 */
    if (e->is_popup)
        try_post_popup_show(e);
    else
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
    if (e->is_popup)
        try_post_popup_show(e);
    else
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
    /* popup: X unmap 对 OR 窗 = dissociate (无 withdrawn 路径) → popup_hide
     * (幂等, popup_visible 去重); 焦点还 owner。 */
    if (e->is_popup)
        post_popup_hide(e);
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
    /* 任务 #135 echo 抑制: 请求值与最近一次 host→wine 回写 (±2px) 吻合 =
     * wine 回声我们刚写入的几何 —— 协议应答照发 (上行), 但不再上送 resize
     * 事件给承载窗 (承载窗本就在这个几何上, 上送 = 自回声空转, 拖拽中会
     * 以拍频放大成 resize 抖动)。ε=2px 实测依据: 本栈无 WM 帧, wine 的
     * win32⇄X 装饰折算是常数偏移 (D52: 632x446@(104,130) → 640x480@(100,100)
     * 创建实测), 请求值与回写值偏差超 2px 即 app 真实改几何 (如 SDL 锁
     * 宽高比回推), 必须放行 —— 抑制面只覆盖「值相同」的无操作类。 */
    const bool echo = e->wb_valid &&
        abs(ev->x - e->wb_x) <= 2 && abs(ev->y - e->wb_y) <= 2 &&
        abs(ev->width - e->wb_w) <= 2 && abs(ev->height - e->wb_h) <= 2;
    if (!e->createdPosted || echo) {
        pthread_mutex_unlock(&g_lock);
        if (echo && e->createdPosted)
            XTL_LOG("XTL geom-echo suppressed id=%{public}u %dx%d",
                    e->toplevelId, ev->width, ev->height);
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
    /* #167: owner 关系变化的 modal 上报走单点 (post_modal_relation,
     * created 门 / popup / 无 parent / owner 不在册都在其内收口)。
     * 注意 set_parent 信号到达时 DIALOG 类型通常尚未可读 (wine 同一
     * round trip 先写 TRANSIENT_FOR 后写 WINDOW_TYPE, xwm 按请求序发
     * 信号) —— 本时机只覆盖「created 后才建立 owner」的路径, 常规流
     * 由 try_post_created 处的补报兜住。且该路径下 modal 事件晚于
     * created, ArkTS 幽灵守卫见 win 已回填会拒删 entry (防 destroyed
     * 悬空的取舍), 常规承载与模态子窗会并存双窗 —— 已知残留 (评审
     * M1, wayland incumbent 同款), wine 不走「map 后才认 owner」的
     * 路径, 实际不触发。 */
    struct x11_xs_entry *e =
        wl_container_of(listener, e, set_parent);
    (void)data;
    pthread_mutex_lock(&g_lock);
    post_modal_relation(e);
    pthread_mutex_unlock(&g_lock);
}

static void handle_set_window_type(struct wl_listener *listener, void *data)
{
    /* #167: WINDOW_TYPE 属性变化 → modal 关系重报 (类型后变/晚到路径;
     * 变回 NORMAL 时 modal=0, ArkTS 拆模态承载 —— 双向)。data 恒 NULL
     * (xwm.c xs_property 回调约定)。常规对话框流中此信号到达早于
     * created, post_modal_relation 的 created 门拦下, 由 created 处的
     * 补报兜住。 */
    struct x11_xs_entry *e =
        wl_container_of(listener, e, set_window_type);
    (void)data;
    pthread_mutex_lock(&g_lock);
    post_modal_relation(e);
    pthread_mutex_unlock(&g_lock);
}

static void handle_set_hints(struct wl_listener *listener, void *data)
{
    /* #167: WM_HINTS 变化 → owner 禁用态翻转 (DialogBoxParam 的
     * EnableWindow(owner,FALSE) / 关框前 re-enable) → 重报自身与以自身
     * 为 owner 的全部对话框的 modal 关系 (评审 I1: created 时 hints 未读
     * 按禁用保守对待, 晚到的真实状态在此修正; data 恒 NULL)。 */
    struct x11_xs_entry *e =
        wl_container_of(listener, e, set_hints);
    (void)data;
    pthread_mutex_lock(&g_lock);
    post_modal_relation(e);
    for (size_t i = 0; i < g_entry_count; ++i) {
        struct x11_xs_entry *c = &g_entries[i];
        if (!c->dead && c->xs && c->xs->parent == e->xs)
            post_modal_relation(c);
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
    /* popup: destroyed 不发 (ArkTS 的 onToplevelDestroyed 按 toplevel 语义
     * 清 FWM/Ability, 不识别 popup id); hide 已由 destroy 路径前站的
     * dissociate (xwayland_surface_destroy 先 dissociate 后发 destroy 信号)
     * 发出 —— destroy 在此只是表回收。 */
    if (!e->is_popup)
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
    if (entry_of_xs(xs)) {
        pthread_mutex_unlock(&g_lock);
        return; /* 重复 new_surface 防御 */
    }
    struct x11_xs_entry *e = entry_alloc();
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        return; /* 表满 (review minor: 满时静默) */
    }
    memset(e, 0, sizeof(*e));
    e->xs = xs;
    e->generation = ++g_entryGeneration;
    /* M4c-T1: OR 分叉在最早入口 (plan RF#1) —— 判据只看 xs->override_redirect
     * (spec §3.5 红线, 不依赖 surface 存在; OR 窗 associate 是否发生不进判据)。 */
    e->is_popup = xs->override_redirect;
    if (e->is_popup) {
        /* popup id 独立基址 (RF#2), 不占 toplevel id 计数器 */
        e->toplevelId = X11_POPUP_ID_BASE + g_popup_id_seq++;
    } else {
        e->toplevelId = x11_toplevel_bridge_allocate_id();
    }
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
    wl_list_init(&e->set_window_type.link);
    wl_list_init(&e->set_hints.link);
    wl_list_init(&e->map_request.link);
    wl_list_init(&e->set_geometry.link);
    e->destroy.notify = handle_xs_destroy;
    wl_signal_add(&xs->events.destroy, &e->destroy);
    e->associate.notify = handle_associate;
    wl_signal_add(&xs->events.associate, &e->associate);
    e->dissociate.notify = handle_dissociate;
    wl_signal_add(&xs->events.dissociate, &e->dissociate);
    if (e->is_popup) {
        /* popup 事件面: map 状态走 associate/commit (见 try_post_popup_show
         * 注释), 几何走 set_geometry (OR 窗无 request_configure)。不挂
         * toplevel 状态面 (minimize/fullscreen/activate/parent/map_request
         * 对 OR 菜单结构性不会发 —— wine 菜单不解 map/不改状态), 挂了也无
         * 消费者 (createdPosted 恒 false 的门会全部拦下)。 */
        e->set_geometry.notify = handle_popup_geometry;
        wl_signal_add(&xs->events.set_geometry, &e->set_geometry);
        /* wine 的 dummy parent (winex11.drv get_dummy_parent, OR 1x1) 等
         * 无 surface 的 OR 窗也会进表 —— 永不 associate 即永不 show,
         * 静默等 destroy (与 toplevel 的未 attach 静默同款)。 */
        pthread_mutex_unlock(&g_lock);
        return;
    }
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
    /* #167: WINDOW_TYPE 变化信号 (modal 判据的对话框声明半边, 双向) */
    e->set_window_type.notify = handle_set_window_type;
    wl_signal_add(&xs->events.set_window_type, &e->set_window_type);
    /* #167: WM_HINTS 变化信号 (owner 禁用态半边; 对自身与以自身为 owner
     * 的对话框重报 modal 关系) */
    e->set_hints.notify = handle_set_hints;
    wl_signal_add(&xs->events.set_hints, &e->set_hints);
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
        e->xs = NULL; /* 墓碑态同 entry_remove (entry_alloc 复用判据) */
        e->win = NULL;
        /* MW-GPU: guest 帧/纹理随会话释放 (同 entry_remove —— 复位后
         * g_entry_count=0, 不释放即永久泄漏)。 */
        if (e->guestTexture) {
            wlr_texture_destroy(e->guestTexture);
            e->guestTexture = NULL;
        }
        if (e->guestFrame) {
            wlr_buffer_unlock(e->guestFrame);
            e->guestFrame = NULL;
        }
        /* popup: destroyed 不发 (同 handle_xs_destroy) —— ArkTS 的 popup
         * 子窗经父 toplevel destroyed 的 destroyPopupsForToplevel 级联销毁
         * (本循环对 owner 发的 destroyed 即触发), 无孤儿路径。 */
        if (!e->is_popup)
            x11_toplevel_bridge_post_destroyed(e->toplevelId);
    }
    g_entry_count = 0;
    pthread_mutex_unlock(&g_lock);
}


struct wlr_xwayland_surface *x11_toplevel_xs_of(uint32_t toplevelId)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].toplevelId == toplevelId && !g_entries[i].dead) {
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

void x11_toplevel_input_axis(uint32_t toplevelId, int px, int py,
                             int which, int steps)
{
    wl_ohos_input_post_mm_axis(toplevelId, px, py, which, steps);
}

/* 任务 #129 RF#3: 关闭请求。napi 线程只入队 (与 pointer/key/axis 同纪律);
 * xcb 连接为 loop 线程专用, wlr_xwayland_surface_close 必须在其上执行。 */
void x11_toplevel_close(uint32_t toplevelId)
{
    wl_ohos_input_post_mm_close(toplevelId);
}

void x11_toplevel_close_dispatch(uint32_t toplevelId)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    struct wlr_xwayland_surface *xs = e ? e->xs : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!xs) return;
    XTL_LOG("XTL close id=%{public}u -> WM_DELETE_WINDOW", toplevelId);
    wlr_xwayland_surface_close(xs);
}

/* pointer/axis dispatch 共用的落点解算 (M4d-review I1 抽出, 判据只留一处):
 * 查表与字段读取持锁 (win_w/win_h 会被 napi 线程 resize_window 写), seat
 * 操作由调用方在锁外做 (seat 不属 entry 表, 持锁调没必要)。物理像素 →
 * X 逻辑坐标; 未校准 (attach 初值 1x1) 时比例未知, 丢弃并采样留痕 ——
 * 比乱投好 (scale≠1 设备上乱投 = 命中点偏 2 倍); 换算后越界 = 点在窗间
 * 空隙/标题条外, 同样不投递 (spec §4)。返回 true = xs 有效且点在窗内。 */
static bool mm_resolve_local(uint32_t toplevelId, int px, int py,
                             struct wlr_xwayland_surface **xs_out,
                             double *lx_out, double *ly_out)
{
    struct x11_xs_entry *e;
    int lx, ly;
    pthread_mutex_lock(&g_lock);
    e = entry_of_id(toplevelId);
    if (!e || !e->xs || !e->win) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (e->win_w <= 2 || e->win_h <= 2) {
        static unsigned skip_n;
        if (++skip_n % 60 == 1)
            XTL_LOG("XTL mm-input skip uncalibrated id=%{public}u win=%{public}dx%{public}d",
                    toplevelId, e->win_w, e->win_h);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    lx = (int)((int64_t)px * (int)e->xs->width / e->win_w);
    ly = (int)((int64_t)py * (int)e->xs->height / e->win_h);
    if (lx < 0 || ly < 0 ||
        lx >= (int)e->xs->width || ly >= (int)e->xs->height) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    *xs_out = e->xs;
    *lx_out = (double)lx;
    *ly_out = (double)ly;
    pthread_mutex_unlock(&g_lock);
    return true;
}

/* loop 线程执行 (InjectQueueDrain 分派): 查表 + 坐标空间换算 + 纪律投递 */

/* GAP2 (#131): 注入前 X 堆叠回写 (click-to-raise)。
 *
 * 问题 (真机实测 2026-10-11, .temp/gap2/wine_trace_after_click1.log):
 * 按窗注入只决定局部坐标 + 目标 surface; X 投递由服务端 hit-test (X 窗树/
 * 堆叠) 决定 (xwayland-input.c dispatch_absolute_motion: root 点 =
 * focus_window->toplevel->drawable + local → QueuePointerEvents
 * POINTER_ABSOLUTE|POINTER_SCREEN)。x11 fusion 下各 app 的 X 窗都钉在创建
 * 位 (host→wine 几何回写缺失, §2.14), rect 恒重叠 ⇒ 堆叠决定命中。而
 * X 堆叠无人同步: wine 跨进程不 restack (X z 序归 WM 管), xwm(wlroots) 把
 * 堆叠留给合成器, 且 map_notify 对新窗 restack(BELOW) —— 实测: 点 B 承载
 * 窗 (tl=17) 的 ButtonPress 全落 A 的 X 窗 (0x10060/e00003), B 自激活
 * (wine foreground) 后 X 顶仍是 A。
 *
 * 机制: 指针/轴注入前把目标 restack 到 managed 栈顶 —— root 点按构造 ∈
 * 目标 X rect, 目标在顶 ⇒ hit-test 必中目标 (不变式)。语义 = 桌面
 * click-to-raise 惯例 (指针交互目标即 X 顶); OS 已按承载窗 z 序投递指针
 * 事件, 本调用令 X 命中与 OS 命中一致。键盘侧的 X 焦点对齐见
 * display_input.c SyncXInputFocus (同一缺口的两半)。
 *
 * 守卫: !override_redirect (wlroots restack 有 assert; popup/菜单走 X grab
 * 重定向, 不参与堆叠) + surface 在册 (withdrawn 窗 stack_link 已摘)。
 * 去重日志: 只记目标切换 (证据链)。
 * xcb 到达序: xwm_schedule_flush 在 socket 可写时同步 flush (xwm.c:3097),
 * restack 先于后续 wayland 输入写入。 */
static void mm_restack_target(struct wlr_xwayland_surface *xs,
                              uint32_t toplevelId)
{
    if (!xs || !xs->surface || xs->override_redirect)
        return;
    static struct wlr_xwayland_surface *last;
    if (last != xs)
    {
        last = xs;
        XTL_LOG("XTL mm-restack id=%{public}u win=0x%{public}x -> managed top",
                toplevelId, xs->window_id);
    }
    wlr_xwayland_surface_restack(xs, NULL, XCB_STACK_MODE_ABOVE);
}

void x11_toplevel_input_pointer_dispatch(uint32_t toplevelId, int px, int py,
                                         int action, uint32_t button)
{
    struct wlr_xwayland_surface *xs;
    double lx, ly;
    if (!mm_resolve_local(toplevelId, px, py, &xs, &lx, &ly))
        return;
    /* 模态阻断 (任务 #167): 被禁用 owner 的点击照发 (wine 自丢弃), 但不做
     * X restack —— click-to-raise 是 WM 动作, 对模态禁用窗不生效 (Windows
     * 语义: 点击禁用 owner 无任何效果)。 */
    if (!x11_toplevel_modal_blocks(xs))
        mm_restack_target(xs, toplevelId);
    wl_ohos_input_multimode_pointer(xs, lx, ly, action, button);
}

void x11_toplevel_input_axis_dispatch(uint32_t toplevelId, int px, int py,
                                      int which, int steps)
{
    struct wlr_xwayland_surface *xs;
    double lx, ly;
    if (!mm_resolve_local(toplevelId, px, py, &xs, &lx, &ly))
        return;
    /* 轴投递按 X 光标位置命中 (Xwayland emulate → Button4/5), 同样需要
     * 目标在 X 顶 (滚动是「指针下的窗」语义, 无点击前置)。模态阻断同上。 */
    if (!x11_toplevel_modal_blocks(xs))
        mm_restack_target(xs, toplevelId);
    wl_ohos_input_multimode_axis(xs, lx, ly, which, steps);
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

/* ── 任务 #135: host→wine 几何回写 (缺失半边, 头文件设计注释) ──
 * 回环分析 (wine 侧源码实读, 判据落点):
 *   - wine 对 WM 主导 configure 直接采信不回推: X11DRV_ConfigureNotify
 *     (winex11.drv/event.c:1096) → window_configure_notify (window.c:1993) →
 *     handle_state_change (window.c:1867) 的「unexpected/mismatch」分支把
 *     desired+pending+current 一并置为收到值, 随后 window_request_desired_state
 *     → window_set_config 因 EqualRect 短路 (window.c:1467) → 不发
 *     XReconfigureWMWindow。
 *   - 回写面因此无自发链: 回写只由承载窗几何事件驱动, wine 采纳不改承载窗。
 *     app 主动改几何 (SetWindowPos → XReconfigureWMWindow, window.c:1514) 照
 *     走 handle_request_configure: 与最近回写吻合的值由 echo 抑制 (上方), 不
 *     吻合 = app 真实意图放行 → 收敛于 app 尺寸后静止。
 * 无操作跳过 (值全等) 是防回声第一道门: 承载窗 resize() 回落 (ArkTS 尺寸精度
 * 往返)、wine 回声场景下不发无意义 configure (也不会触发 wlr 的合成
 * ConfigureNotify —— 尺寸不変时它会合成一条, 见 xwm.c:2384)。 */
static void geom_writeback(uint32_t toplevelId, bool size_half, int a, int b)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_id(toplevelId);
    struct wlr_xwayland_surface *xs = e ? e->xs : NULL;
    int x, y, w, h;
    if (!xs || !e->createdPosted || e->is_popup) {
        pthread_mutex_unlock(&g_lock);
        return; /* 未创建/已死/popup: 几何回写对 popup 结构性不适用 (无 WM 管) */
    }
    if (xs->minimized) {
        /* 镜像 xdg 侧最小化门禁 (NotifyToplevelResize 头注释): 图标化期间
         * 不动几何 —— 恢复后的跟随由后续承载窗事件自然补齐 (FWM.show →
         * surface 事件链)。 */
        pthread_mutex_unlock(&g_lock);
        XTL_LOG("XTL geom-%{public}s id=%{public}u SKIPPED (minimized)",
                size_half ? "size" : "move", toplevelId);
        return;
    }
    x = size_half ? (int)xs->x : a;
    y = size_half ? (int)xs->y : b;
    w = size_half ? a : (int)xs->width;
    h = size_half ? b : (int)xs->height;
    if (x == (int)xs->x && y == (int)xs->y &&
        w == (int)xs->width && h == (int)xs->height) {
        pthread_mutex_unlock(&g_lock);
        return; /* 无变化: 跳过 (防回声风暴第一道门, 见上) */
    }
    pthread_mutex_unlock(&g_lock);
    /* 值域防御: wlr 签名 x/y=int16_t, w/h=uint16_t —— 越界截断在 X 侧是
     * 未定义观感 (w 溢出 65535 回绕); 正常路径 (桌面逻辑尺寸) 远在域内。 */
    if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
    if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
    if (w > 32767) w = 32767;
    if (h > 32767) h = 32767;
    wlr_xwayland_surface_configure(xs, (int16_t)x, (int16_t)y,
                                   (uint16_t)w, (uint16_t)h);
    pthread_mutex_lock(&g_lock);
    if (e == entry_of_id(toplevelId)) { /* 同线程单跳, 复验仅防御性 */
        e->wb_valid = true;
        e->wb_x = x;
        e->wb_y = y;
        e->wb_w = w;
        e->wb_h = h;
    }
    pthread_mutex_unlock(&g_lock);
    XTL_LOG("XTL geom-%{public}s id=%{public}u (%{public}d,%{public}d %{public}dx%{public}d)",
            size_half ? "size" : "move", toplevelId, x, y, w, h);
}

void x11_toplevel_geometry_size_dispatch(uint32_t toplevelId, int w, int h)
{
    /* notifyToplevelResizeEnd (windowRectChange DRAG_END) 发 0 尺寸复位:
     * wayland 语义 = 清 RESIZING / wine 保持当前尺寸; x11 无 size-move 态,
     * 同语义落地 = 无操作 (绝不把 0 写进 X 窗)。 */
    if (w <= 0 || h <= 0) return;
    geom_writeback(toplevelId, true, w, h);
}

void x11_toplevel_geometry_move_dispatch(uint32_t toplevelId, int x, int y)
{
    geom_writeback(toplevelId, false, x, y);
}

bool x11_toplevel_owns(uint32_t toplevelId)
{
    pthread_mutex_lock(&g_lock);
    const bool owns = entry_of_id(toplevelId) != NULL;
    pthread_mutex_unlock(&g_lock);
    return owns;
}

/* napi/JS 线程入口 (与 x11_toplevel_input_* 同纪律): 只入队。 */
void x11_toplevel_geometry_size(uint32_t toplevelId, int w, int h)
{
    wl_ohos_input_post_mm_geom_size(toplevelId, w, h);
}

void x11_toplevel_geometry_move(uint32_t toplevelId, int x, int y)
{
    wl_ohos_input_post_mm_geom_move(toplevelId, x, y);
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
    /* MW-GPU: 有 guest 零拷贝帧纹理 (fusion GPU 窗) 优先画它 —— 此时
     * surface->buffer 停在 map 时的初始帧 (D50 链), 内容无价值; 无纹理
     * (未挂帧/建失败) 回退 surface 路径 (画旧帧不崩)。 */
    struct wlr_texture *src = e->guestTexture;
    if (!src) {
        if (!cb || !cb->texture) {
            XTL_LOG("XTL render-skip nocb id=%{public}u cb=%{public}p",
                    e->toplevelId, (void *)cb);
            return true;
        }
        src = cb->texture;
    }
    XTL_LOG("XTL render id=%{public}u tex=%{public}p %dx%d -> dst",
            e->toplevelId, (void *)src,
            src->width, src->height);
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
    opts.texture = src;
    opts.dst_box.x = 0;
    opts.dst_box.y = 0;
    opts.dst_box.width = dst->width;
    opts.dst_box.height = dst->height;
    /* 0 不是合法 transfer function (SRGB = 1<<0), 必须显式给 */
    opts.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_SRGB;
    /* MW-GPU: guest 帧行序修正 —— GL(virgl) 翻, Vulkan(venus) 不翻 (与
     * scene 路径 frame_set 的 FLIPPED_180 同口径); GDI 路径 memset 后
     * 即 NORMAL, 不变。 */
    if (e->guestFrame && e->guestFlip)
        opts.transform = WL_OUTPUT_TRANSFORM_FLIPPED_180;
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

/* MW-GPU: 按 X 窗号查在册 entry。持锁调用。 */
static struct x11_xs_entry *entry_of_window(uint32_t xwindow)
{
    for (size_t i = 0; i < g_entry_count; ++i) {
        struct x11_xs_entry *c = &g_entries[i];
        if (!c->dead && c->xs && c->xs->window_id == xwindow)
            return c;
    }
    return NULL;
}

/* MW-GPU: guest 帧路由的窗口查询面 —— fusion 无 g_clients 记录 (多窗分支
 * HandleNewSurface 不建 client, 窗口只在本映射层在册), ohos_output 的同名
 * client_* 查询在 multiwindow 下转接至此。语义与 client 路径对齐: 几何 =
 * X 全局坐标 (与 C 路径 xs->x/y 同源), 身份 = generation。返回 0 = 不在册。 */
int x11_toplevel_guest_anchor(uint32_t xwindow, int *x, int *y, int *w, int *h)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_window(xwindow);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (x) *x = e->xs->x;
    if (y) *y = e->xs->y;
    if (w) *w = e->xs->width;
    if (h) *h = e->xs->height;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int x11_toplevel_guest_generation(uint32_t xwindow, uint64_t *generation)
{
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_window(xwindow);
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (generation) *generation = e->generation;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

/* MW-GPU: fusion 承载窗挂 guest 零拷贝帧 (display_guest_frames 的 PullFrame
 * → ohos_output_client_frame_set multiwindow 分支路由到此)。引用语义与
 * scene 路径同款: 这里 lock 一份持有, 替换 / entry 移除时 unlock。返回 1 =
 * 已收下; 0 = 无此在册窗 (调用方自行释放该帧)。持锁执行, 纪律同
 * render_entry (napi 侧 attach 最多等一帧)。 */
int x11_toplevel_set_guest_frame(uint32_t xwindow, struct wlr_buffer *buffer,
                                 int flip)
{
    if (!buffer) return 0;
    /* MW-GPU: 纹理与帧缓冲建/毁配对 (scene 节点 set_buffer 同款节奏): 帧
     * wrapper 每 acquire 新建 (ohos_buffer.cpp calloc), 纹理无法跨帧缓存。
     * 锁外建纹理 (本函数只在合成器 loop 线程被调)。 */
    struct wlr_texture *tex = wlr_texture_from_buffer(g_renderer, buffer);
    if (!tex) {
        /* 建纹理失败 (EGL 压力等): 不收帧 (调用方解锁自持引用), 旧帧原样 */
        static uint32_t s_texfail;
        if (++s_texfail % 60 == 1)
            XTL_LOG("XTL guest-frame tex-fail xwin=%{public}u n=%{public}u",
                    xwindow, (unsigned)s_texfail);
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    struct x11_xs_entry *e = entry_of_window(xwindow);
    if (!e) {
        /* 拿帧窗口不在册 (销毁竞态 / popup 未挂 / id 未登记) —— 采样留证 */
        static uint32_t s_miss;
        if (++s_miss % 120 == 1)
            XTL_LOG("XTL guest-frame miss xwin=%{public}u n=%{public}u",
                    xwindow, (unsigned)s_miss);
        pthread_mutex_unlock(&g_lock);
        wlr_texture_destroy(tex);
        return 0;
    }
    wlr_buffer_lock(buffer);
    if (e->guestTexture) wlr_texture_destroy(e->guestTexture);
    if (e->guestFrame) wlr_buffer_unlock(e->guestFrame);
    e->guestFrame = buffer;
    e->guestTexture = tex;
    e->guestFlip = flip;
    e->dirty = true;
    pthread_mutex_unlock(&g_lock);
    return 1;
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
         * 已试过) 的时序由帧钟收口。popup 条目跳过 —— created 是 toplevel
         * 专属事件, 对 popup 发 = ArkTS 按 popupId 建 toplevel 承载 (M4c-T1
         * 自查修: associate/commit 兜底已覆盖 popup show, 帧钟只需渲染)。 */
        if (!e->is_popup)
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

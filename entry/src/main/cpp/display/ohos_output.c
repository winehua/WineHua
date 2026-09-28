/*
 * ohos_output.c — T8 出图链: headless output + 测试图案 + NativeWindow 直推
 *
 * 链路 (全部 C 编译, 见 ohos_output.h 的 C++ 兼容性说明):
 *   wl_ohos_allocator_create (ohos_buffer.cpp) → headless output 800x600
 *   → wlr_output_init_render → enable commit
 *   → allocator.create_buffer 分配帧缓冲 (OH_NativeBuffer 背书)
 *   → OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer + AttachBuffer
 *   → 30fps 定时器: 渐变+边框图案 → state_set_buffer → commit_state
 *   → events.commit → state->buffer → NativeWindowFlushBuffer 直推
 *
 * SDK 实测要点:
 * - NativeWindowBuffer 结构 opaque, 自建不可行; 正规路径是
 *   CreateNativeWindowBufferFromNativeBuffer 包装自有的 OH_NativeBuffer
 *   后 AttachBuffer, 每帧直接 FlushBuffer 该 buffer (T8 首选路径)。
 *   首验失败矩阵: Flush 报错 → 改 Request→GetBufferHandleFromNative→
 *   virAddr 写→UnlockAndFlush。
 */
#define WLR_USE_UNSTABLE
#include "ohos_output.h"
#include "ohos_buffer.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <libdrm/drm_fourcc.h>
#include <pixman-1/pixman.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/xwayland/xwayland.h>
#include <wlr/util/log.h>
#include <wlr/backend/headless.h>
#include <wlr/render/pixman.h>

#define LOG_TAG "ohos-output"
#define OHLOG(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)

#include <wlr/types/wlr_scene.h>

struct wl_ohos_output {
    struct wlr_output *output;
    struct wlr_scene *scene;            /* M1-T3: scene 图形栈 */
    struct wlr_scene_output *scene_output;
    OHNativeWindow *window;
    struct wl_listener commit_listener;
    struct wl_listener xnew_surface; /* xwayland->events.new_surface (T9) */
    struct wl_event_source *frame_timer;
    uint32_t frame_seq;
    uint32_t last_crc;
};

/* T9: X client surface 跟踪。T2 起为链表 (创建序, 链尾 = 最上层):
 * 多窗口 blit 按序画 (后创建压前), 注入命中测试按几何反查。映射状态
 * 不挂监听器——wlr_surface.mapped 轮询 (33ms 帧驱动内天然覆盖),
 * destroy 监听防悬垂。 */
struct ohos_client_surface {
    struct wlr_xwayland_surface *xs;
    struct wl_list link; /* g_clients */
    struct wlr_scene_surface *scene_surf; /* M1-T3: scene 节点 */
    struct wl_listener destroy;
    struct wl_listener request_configure;
    struct wl_listener associate; /* xs->surface 后到 (M0 spec §6.2) */
    struct wl_listener dissociate;
};

static struct wl_ohos_output g_out;
static struct wl_list g_clients;

// 帧数据的 32 位折叠校验 (高低 16 位异或), 用于 hilog 判帧稳定
static uint32_t FrameCrc(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 4)
        sum += p[i] + ((uint32_t)p[i + 1] << 8) + ((uint32_t)p[i + 2] << 16) +
               ((uint32_t)p[i + 3] << 24);
    return (sum & 0xffffu) ^ (sum >> 16);
}

/* M1-T3: 手搓渲染链 (RenderTestPattern/BlitClientSurface/RenderFrame,
 * 逐像素图案 + R/B 互换 blit) 已由 wlr_scene 取代 —— scene 经 pixman
 * 合成, X client surface 直接挂 scene graph, 不再逐像素手拷。帧率
 * 基线与分段耗时见 ledger。 */

/* T3: xs->surface 在 associate 事件才可用 (M0 spec §6.2 实测结论:
 * new_surface 时为 NULL, 手搓链靠逐帧轮询掩盖了这点, scene 挂载必须
 * 等 associate)。surface 销毁时 scene 节点由 wlroots 自动回收;
 * dissociate (M2+ 窗口管理复用语义) 时清指针。 */
static void ClientAssociate(struct wl_listener *listener, void *data)
{
    struct ohos_client_surface *c =
        wl_container_of(listener, c, associate);
    struct wlr_xwayland_surface *xs = c->xs;
    (void)data;
    if (!xs || !xs->surface || !g_out.scene)
        return;
    c->scene_surf = wlr_scene_surface_create(&g_out.scene->tree, xs->surface);
    if (!c->scene_surf)
        OH_LOG_ERROR(LOG_APP, "scene_surface create failed (associate)");
    OHLOG("client associated surf=%{public}p (scene attached)",
          (void *)xs->surface);
}

static void ClientDissociate(struct wl_listener *listener, void *data)
{
    struct ohos_client_surface *c =
        wl_container_of(listener, c, dissociate);
    (void)data;
    /* surface 已随 dissociate 失效; scene 节点由 wlroots 随 surface 销毁
     * 回收, 这里只清引用 */
    c->scene_surf = NULL;
}

// commit 帧 → 推 NativeWindow。
// 实测矩阵: 路径 A (Attach 后直接 FlushBuffer) = 41207000 BUFFER_STATE_INVALID;
// 路径 B (RequestBuffer→GetBufferHandleFromNative) = handle 在但 virAddr=NULL
// (BufferQueue 不自动 map 给生产者)。路径 C (本实现): LockBuffer+UnlockFlush
// (native_window.h:962/978, since API 23)——Lock 语义即 map, 锁后 virAddr 应
// 有效。LockBuffer 用 dlsym 运行时解析: 直接链接会让 entry.so 在缺符号的
// 老设备上加载失败。
typedef int32_t (*LockBufferFn)(OHNativeWindow *, Region, OHNativeWindowBuffer **);
typedef int32_t (*UnlockFlushFn)(OHNativeWindow *);
static LockBufferFn g_lock_buffer;
static UnlockFlushFn g_unlock_flush;
static int g_lock_symbols; /* -1 未试, 0 设备无, 1 可用 */

static int ResolveLockSymbols(void)
{
    if (g_lock_symbols >= 0)
        return g_lock_symbols;
    void *h = dlopen("libnative_window.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h)
        h = dlopen("libnative_window.so", RTLD_NOW);
    if (h) {
        g_lock_buffer = (LockBufferFn)dlsym(h, "OH_NativeWindow_LockBuffer");
        g_unlock_flush = (UnlockFlushFn)dlsym(h, "OH_NativeWindow_UnlockAndFlushBuffer");
    }
    g_lock_symbols = (g_lock_buffer && g_unlock_flush) ? 1 : 0;
    OHLOG("LockBuffer symbols available=%{public}d", g_lock_symbols);
    return g_lock_symbols;
}

static void HandleOutputCommit(struct wl_listener *listener, void *data)
{
    (void)listener;
    struct wlr_output_event_commit *event = data;
    struct timespec ts_enter;
    clock_gettime(CLOCK_MONOTONIC, &ts_enter);
    if (!event || !event->state || !event->state->buffer)
        return;
    ++g_out.frame_seq;

    /* Region 无内嵌数组: rects 是独立指针, 必须指向外部 RegionRect。
     * 之前写 `region.rects = &region.rects[0]` 是对未初始化指针取下标
     * (= 自赋垃圾值), 随栈残留值偶发可写不崩、常则 SEGV——T8 真机三次
     * cppcrash (20:23:33/20:23:57/20:34:37, Faultlogger 定位到本行) 的根因。
     * M1-T3: 尺寸取 commit 的 swapchain buffer (scene 渲染目标),
     * 不再是固定的 frame_buf。 */
    struct Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = (uint32_t)event->state->buffer->width;
    rect.h = (uint32_t)event->state->buffer->height;
    Region region;
    region.rects = &rect;
    region.rectNumber = 1;

    OHNativeWindowBuffer *win_buf = NULL;
    int mapped = 0;
    int dst_rows = 0;
    void *dst = NULL;
    size_t dst_stride = 0;
    int fence = -1;

    if (ResolveLockSymbols()) {
        /* 路径 C: Lock 即 map */
        int32_t rc = g_lock_buffer(g_out.window, region, &win_buf);
        if (rc != 0 || !win_buf) {
            OH_LOG_ERROR(LOG_APP, "LockBuffer rc=%{public}d (帧 %{public}u)",
                         rc, g_out.frame_seq);
            return;
        }
        BufferHandle *h = OH_NativeWindow_GetBufferHandleFromNative(win_buf);
        if (h && h->virAddr) {
            dst = h->virAddr;
            dst_stride = (size_t)h->stride;
            mapped = 0; /* UnlockAndFlush 负责解除 */
        } else {
            g_unlock_flush(g_out.window); /* 归还队列状态 */
            win_buf = NULL;
        }
    }

    if (!win_buf) {
        /* 路径 G: RequestBuffer + 自行 mmap handle->fd (virAddr 由系统
         * map 的场景只有 LockBuffer; 老设备无 LockBuffer, fd mmap 等价) */
        int32_t rc = OH_NativeWindow_NativeWindowRequestBuffer(
            g_out.window, &win_buf, &fence);
        if (rc != 0 || !win_buf) {
            if (g_out.frame_seq % 30 == 1)
                OH_LOG_ERROR(LOG_APP, "RequestBuffer rc=%{public}d (帧 %{public}u)",
                             rc, g_out.frame_seq);
            return;
        }
        BufferHandle *h = OH_NativeWindow_GetBufferHandleFromNative(win_buf);
        if (!h || h->fd < 0) {
            if (g_out.frame_seq == 1)
                OH_LOG_ERROR(LOG_APP, "no buffer fd h=%{public}p", (void *)h);
            if (fence >= 0) close(fence);
            return;
        }
        /* BufferHandle.stride 语义 = 字节/行 (size = stride*height);
         * 之前误 ×4 导致写越界 SEGV (T8 实测 cppcrash) */
        size_t bytes = (h->size > 0) ? (size_t)h->size
                                     : (size_t)h->stride * h->height;
        dst = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
        if (dst == MAP_FAILED) {
            OH_LOG_ERROR(LOG_APP, "mmap fd=%{public}d errno=%{public}d",
                         h->fd, errno);
            if (fence >= 0) close(fence);
            return;
        }
        dst_stride = (size_t)h->stride;
        dst_rows = h->height > 0 ? h->height : 0;
        mapped = 1;
    }

    /* 帧在 commit 的 swapchain buffer (M1-T3: scene 渲染目标, 经我们的
     * OHOS allocator 背书) 里; 拷贝进 window buffer。
     * 源读取绕开 wlr_buffer access 计数: commit 事件在 commit_state 内同步
     * 发射, 此时 pixman renderer 尚未 end 对该 buffer 的 access, 再 begin
     * 会命中 accessing_data 断言 → abort (T8 实测 cppcrash)。
     * OH_NativeBuffer_Map 直接映射同一物理内存, 与访问计数无关。 */
    OH_NativeBuffer *src_nb = wl_ohos_buffer_native(event->state->buffer);
    size_t src_stride = wl_ohos_buffer_stride(event->state->buffer);
    void *src = NULL;
    if (!src_nb || OH_NativeBuffer_Map(src_nb, &src) != 0 || !src) {
        if (fence >= 0) close(fence);
        return;
    }
    int w = region.rects[0].w, ht = region.rects[0].h;
    size_t copy_bytes = (size_t)w * 4;
    uint8_t *d = dst;
    uint8_t *s = src;
    /* 双侧行距一致且 >= 行宽才拷; 行数钳到两侧较小者 */
    if (dst_stride == src_stride && dst_stride >= copy_bytes) {
        int rows = (dst_rows > 0 && dst_rows < ht) ? dst_rows : ht;
        for (int y = 0; y < rows; ++y)
            memcpy(d + (size_t)y * dst_stride, s + (size_t)y * src_stride,
                   copy_bytes);
        if ((g_out.frame_seq % 30) == 1)
            g_out.last_crc = FrameCrc(s, (size_t)ht * src_stride);
    }
    OH_NativeBuffer_Unmap(src_nb);

    if (mapped) {
        size_t bytes = (size_t)ht * dst_stride;
        munmap(dst, bytes);
        /* fence 归还系统 (FlushBuffer 文档: fenceFd 由系统关闭) */
        int32_t rc = OH_NativeWindow_NativeWindowFlushBuffer(
            g_out.window, win_buf, fence, region);
        if (rc != 0 && g_out.frame_seq % 30 == 1)
            OH_LOG_ERROR(LOG_APP, "FlushBuffer rc=%{public}d", rc);
    } else {
        int32_t rc = g_unlock_flush(g_out.window);
        if (rc != 0)
            OH_LOG_ERROR(LOG_APP, "UnlockFlush rc=%{public}d (帧 %{public}u)",
                         rc, g_out.frame_seq);
    }
    if ((g_out.frame_seq % 30) == 1)
        OHLOG("commit seq=%{public}u crc=%{public}x", g_out.frame_seq, g_out.last_crc);
    /* T3 分段计时: commit→NativeWindow 拷贝推屏段的每帧耗时 (平均/最大,
     * 每 120 帧打一次)。零拷贝重构属 M2, 本任务只出数据。 */
    {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t us = (int64_t)(ts_now.tv_sec - ts_enter.tv_sec) * 1000000 +
                     (ts_now.tv_nsec - ts_enter.tv_nsec) / 1000;
        static int64_t sum_us;
        static int64_t max_us;
        static int n;
        sum_us += us;
        if (us > max_us) max_us = us;
        if (++n >= 120) {
            OHLOG("segment copy+flush: avg=%{public}lldus max=%{public}lldus n=%{public}d",
                  (long long)(sum_us / n), (long long)max_us, n);
            sum_us = 0;
            max_us = 0;
            n = 0;
        }
    }
}

// 30fps 帧时钟 (M1-T3 scene 版): frame_done 无条件按节拍泵出 (client 的
// frame 节流靠它解锁 —— 不泵则 client 等回调、无新 damage、scene 无帧
// 可提, 互等死锁, gate1 实测停在首帧), commit 由 scene damage 门控
// (画面静止时零渲染零拷贝)。headless output 无自身 frame 事件, 本定时器
// 即帧时钟。
static int FrameTick(void *data)
{
    (void)data;
    struct ohos_client_surface *c;
    wl_list_for_each(c, &g_clients, link) {
        if (c->scene_surf && c->xs)
            wlr_scene_node_set_position(&c->scene_surf->buffer->node,
                                        c->xs->x, c->xs->y);
    }
    if (g_out.scene_output) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        /* frame_done 直发 client surface: scene 级泵 (wlr_scene_output_
         * send_frame_done) 按 node.visible 过滤, 而 visible 只在渲染遍历
         * 时填充 —— headless 无 frame 事件 + commit 按 damage 门控时,
         * 无内容节点永远不可见, 首帧互等死锁 (gate2 探针: needs_frame
         * 恒 0, withbuf 恒 1)。surface 级直发不依赖渲染遍历, 与真合成器
         * 在 output frame 事件中的做法一致。 */
        struct ohos_client_surface *pc;
        wl_list_for_each(pc, &g_clients, link) {
            if (pc->xs && pc->xs->surface)
                wlr_surface_send_frame_done(pc->xs->surface, &now);
        }
        /* scene 提交段计时 (仅统计实际渲染帧: 无 damage 的 commit 内部
         * 直接跳过, 混入会稀释均值)。分段基线 ~18-20ms/帧 (T3 ledger),
         * 零拷贝重构属 M2 present 重构, 本遥测作其前后对照。 */
        bool render = wlr_scene_output_needs_frame(g_out.scene_output);
        struct timespec ts_r0;
        clock_gettime(CLOCK_MONOTONIC, &ts_r0);
        wlr_scene_output_commit(g_out.scene_output, NULL);
        struct timespec ts_r1;
        clock_gettime(CLOCK_MONOTONIC, &ts_r1);
        if (render) {
            int64_t us = (int64_t)(ts_r1.tv_sec - ts_r0.tv_sec) * 1000000 +
                         (ts_r1.tv_nsec - ts_r0.tv_nsec) / 1000;
            static int64_t rsum;
            static int64_t rmax;
            static int rn;
            rsum += us;
            if (us > rmax) rmax = us;
            if (++rn >= 120) {
                OHLOG("segment scene render+commit: avg=%{public}lldus max=%{public}lldus n=%{public}d",
                      (long long)(rsum / rn), (long long)rmax, rn);
                rsum = 0;
                rmax = 0;
                rn = 0;
            }
        }
    }
    wl_event_source_timer_update(g_out.frame_timer, 33);
    return 0;
}

// ── T9: xwayland surface 跟踪 (most-recent-wins) ──────────────────────────
static void ClientDestroy(struct wl_listener *listener, void *data)
{
    struct ohos_client_surface *c =
        wl_container_of(listener, c, destroy);
    (void)data;
    wl_list_remove(&c->destroy.link);
    wl_list_remove(&c->request_configure.link);
    wl_list_remove(&c->associate.link);
    wl_list_remove(&c->dissociate.link);
    wl_list_remove(&c->link);
    free(c);
}

/* XWM 的 geometry 请求必须应答: 不调 wlr_xwayland_surface_configure 则
 * surface 的 commit 全部滞留 cached state, current.buffer 恒 NULL
 * (T9 实测: surface mapped 但无 buffer, 窗口内容不上屏) */
static void ClientRequestConfigure(struct wl_listener *listener, void *data)
{
    struct ohos_client_surface *c =
        wl_container_of(listener, c, request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    if (!ev || ev->surface != c->xs)
        return;
    wlr_xwayland_surface_configure(c->xs, ev->x, ev->y,
                                   ev->width, ev->height);
}

static void HandleNewSurface(struct wl_listener *listener, void *data)
{
    struct wl_ohos_output *o = wl_container_of(listener, o, xnew_surface);
    (void)o;
    struct wlr_xwayland_surface *xs = data;
    if (!xs)
        return;
    struct ohos_client_surface *c = calloc(1, sizeof(*c));
    if (!c)
        return;
    c->xs = xs;
    c->destroy.notify = ClientDestroy;
    wl_signal_add(&xs->events.destroy, &c->destroy);
    c->request_configure.notify = ClientRequestConfigure;
    wl_signal_add(&xs->events.request_configure, &c->request_configure);
    c->associate.notify = ClientAssociate;
    wl_signal_add(&xs->events.associate, &c->associate);
    c->dissociate.notify = ClientDissociate;
    wl_signal_add(&xs->events.dissociate, &c->dissociate);
    wl_list_insert(g_clients.prev, &c->link); /* 链尾 = 最上层 */
    OHLOG("client surface created (%{public}dx%{public}d @%{public}d,%{public}d)",
          xs->width, xs->height, xs->x, xs->y);
}

/* 可见性谓词: Xwayland 表面的空 commit 会翻转 surface->mapped (0.20
 * surface_commit_state: NULL buffer commit → unmap, xwl 表面例行发空
 * commit), 而最后有效像素仍在 surface->buffer —— 以 buffer 存在性为准
 * (M0-T9 实证: 内容在 surf->buffer, current.buffer 可为 NULL)。mapped
 * 保留协议意义; withdrawn/最小化语义归 M2+ 窗口管理。 */
int wl_ohos_surface_has_content(struct wlr_surface *surf)
{
    return surf && surf->buffer != NULL;
}

/* M1-T1/T2: 注入取数口 (display_input.c 调用)。
 * client_xs = 最上层已映射窗口 (T1 自动注入目标);
 * client_topmost_at = 帧坐标命中 (T2 注入几何换算);
 * frame_size = 命中坐标归一化的基准 (输出尺寸当前固定 800x600)。 */
struct wlr_xwayland_surface *wl_ohos_output_client_xs(void)
{
    struct ohos_client_surface *c;
    wl_list_for_each_reverse(c, &g_clients, link) {
        if (c->xs && wl_ohos_surface_has_content(c->xs->surface))
            return c->xs;
    }
    return NULL;
}

struct wlr_xwayland_surface *wl_ohos_output_client_topmost_at(int fx, int fy)
{
    struct ohos_client_surface *c;
    struct wlr_xwayland_surface *hit = NULL;
    wl_list_for_each_reverse(c, &g_clients, link) {
        struct wlr_xwayland_surface *xs = c->xs;
        if (!wl_ohos_surface_has_content(xs ? xs->surface : NULL))
            continue;
        if (fx >= xs->x && fx < xs->x + xs->width &&
            fy >= xs->y && fy < xs->y + xs->height) {
            hit = xs;
            break;
        }
    }
    return hit;
}

void wl_ohos_output_frame_size(int *w, int *h)
{
    if (w) *w = 800;
    if (h) *h = 600;
}

int wl_ohos_output_chain_start(struct wlr_backend *backend,
                               struct wlr_renderer *renderer,
                               struct wl_event_loop *loop,
                               OHNativeWindow *window,
                               struct wlr_xwayland *xwayland)
{
    memset(&g_out, 0, sizeof(g_out));
    g_out.window = window;
    wl_list_init(&g_clients);

    struct wlr_allocator *alloc = wl_ohos_allocator_create();
    if (!alloc) {
        OH_LOG_ERROR(LOG_APP, "ohos allocator create failed");
        return -1;
    }
    g_out.output = wlr_headless_add_output(backend, 800, 600);
    if (!g_out.output) {
        OH_LOG_ERROR(LOG_APP, "headless add_output failed");
        return -1;
    }
    if (!wlr_output_init_render(g_out.output, alloc, renderer)) {
        OH_LOG_ERROR(LOG_APP, "output_init_render failed (caps 不匹配?)");
        return -1;
    }
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    // headless output 首次 commit 必须带 mode (只 set_enabled 实测 commit
    // 失败); refresh=0 交由后端补默认
    wlr_output_state_set_custom_mode(&st, 800, 600, 0);
    if (!wlr_output_commit_state(g_out.output, &st)) {
        OH_LOG_ERROR(LOG_APP, "output enable commit failed");
        wlr_output_state_finish(&st);
        return -1;
    }
    wlr_output_state_finish(&st);

    /* M1-T3: scene 图形栈取代手搓帧缓冲。scene 经 pixman 渲染到 output
     * 的 swapchain (buffer 由我们的 OHOS allocator 背书), commit 事件把
     * swapchain buffer 带给 HandleOutputCommit 拷推 NativeWindow ——
     * 下游推屏链路不变。背景 rect 兜底承担原测试图案的"无窗口可见"职责
     * (premultiplied 深灰)。 */
    g_out.scene = wlr_scene_create();
    if (!g_out.scene) {
        OH_LOG_ERROR(LOG_APP, "scene create failed");
        return -1;
    }
    const float bg[4] = {0.10f, 0.10f, 0.12f, 1.0f};
    if (!wlr_scene_rect_create(&g_out.scene->tree, 800, 600, bg)) {
        OH_LOG_ERROR(LOG_APP, "scene background rect failed");
        return -1;
    }
    g_out.scene_output = wlr_scene_output_create(g_out.scene, g_out.output);
    if (!g_out.scene_output) {
        OH_LOG_ERROR(LOG_APP, "scene_output create failed");
        return -1;
    }

    // window 队列 buffer 几何声明 (Request 按此分配, memcpy 尺寸才对)
    int32_t rc = OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY,
                                                       800, 600);
    OHLOG("SET_BUFFER_GEOMETRY rc=%{public}d", rc);

    g_out.commit_listener.notify = HandleOutputCommit;
    wl_signal_add(&g_out.output->events.commit, &g_out.commit_listener);

    if (xwayland) {
        g_out.xnew_surface.notify = HandleNewSurface;
        wl_signal_add(&xwayland->events.new_surface, &g_out.xnew_surface);
    }

    g_out.frame_timer = wl_event_loop_add_timer(loop, FrameTick, g_out.output);
    if (g_out.frame_timer)
        wl_event_source_timer_update(g_out.frame_timer, 100);
    OHLOG("output chain up: 800x600@30fps");
    return 0;
}

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

struct wl_ohos_output {
    struct wlr_output *output;
    struct wlr_buffer *frame_buf;
    OH_NativeBuffer *frame_nb; /* 帧背书; commit 回调直接 Map 读 (见下) */
    size_t frame_stride;       /* 字节/行, GetConfig 回读 */
    OHNativeWindowBuffer *frame_win_buf;
    OHNativeWindow *window;
    struct wl_listener commit_listener;
    struct wl_listener xnew_surface; /* xwayland->events.new_surface (T9) */
    struct wl_event_source *frame_timer;
    uint32_t frame_seq;
    uint32_t last_crc;
};

/* T9: 最近一个新建的 X client surface (most-recent-wins, M0 单窗口足够)。
 * 映射状态不挂监听器——wlr_surface.mapped 轮询 (33ms 帧驱动内天然覆盖),
 * 只需 destroy 监听防悬垂。 */
struct ohos_client_surface {
    struct wlr_xwayland_surface *xs;
    struct wl_listener destroy;
    struct wl_listener request_configure;
};

static struct wl_ohos_output g_out;
static struct ohos_client_surface *g_client;

// 帧数据的 32 位折叠校验 (高低 16 位异或), 用于 hilog 判帧稳定
static uint32_t FrameCrc(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 4)
        sum += p[i] + ((uint32_t)p[i + 1] << 8) + ((uint32_t)p[i + 2] << 16) +
               ((uint32_t)p[i + 3] << 24);
    return (sum & 0xffffu) ^ (sum >> 16);
}

// 渐变 + 黄边框测试图案写入已映射的帧内存; ABGR8888 = 内存字节 R,G,B,A
// (与 OHOS RGBA_8888 一致)
static void RenderTestPattern(uint8_t *px, int w, int h, uint32_t seq)
{
    for (int y = 0; y < h; ++y) {
        uint8_t *row = px + (size_t)y * g_out.frame_stride;
        for (int x = 0; x < w; ++x) {
            uint8_t r, g, b;
            if (x < 8 || x >= w - 8 || y < 8 || y >= h - 8) {
                r = 255; g = 255; b = 0; /* 黄边框 */
            } else {
                r = (uint8_t)(x * 255 / w);
                g = (uint8_t)(y * 255 / h);
                b = (uint8_t)((seq * 7) & 0xff); /* 随帧蓝道, 人眼确认刷新 */
            }
            row[(size_t)x * 4 + 0] = r;
            row[(size_t)x * 4 + 1] = g;
            row[(size_t)x * 4 + 2] = b;
            row[(size_t)x * 4 + 3] = 255;
        }
    }
}

/* T9 端到端: X client 窗口内容 blit 进帧 (居中)。X shm buffer 是
 * ARGB8888/XRGB8888 (内存字节 B,G,R,A), 帧是 ABGR8888 (R,G,B,A)——逐像素
 * R/B 互换。窗口大于帧时裁剪 (取左上)。读失败或无窗口返回 false。 */
static bool BlitClientSurface(uint8_t *px, int fw, int fh)
{
    if (!g_client || !g_client->xs)
        return false;
    struct wlr_surface *surf = g_client->xs->surface;
    if (!surf || !surf->mapped)
    {
        /* 一次性诊断: surface 在但未 map = X 窗口内容未走 wayland 提交 */
        static int said;
        if (++said == 60) /* ~7s 后打一次 */
            OHLOG("client not blitting: surf=%{public}p mapped=%{public}d "
                  "xs->surface=%{public}p",
                  (void *)surf, surf ? surf->mapped : -1,
                  (void *)g_client->xs->surface);
        return false;
    }
    /* 显示中内容在 surface->buffer (wlr_client_buffer)——current.buffer 是
     * 最近一次 commit 的裸 buffer, Xwayland child 的空 commit 会让它为 NULL
     * (T9 实测)。client buffer 的 base 即 wlr_buffer, shm 内容可直读。 */
    struct wlr_buffer *cbuf = surf->buffer ? &surf->buffer->base : NULL;
    if (!cbuf || cbuf->width <= 0 || cbuf->height <= 0)
    {
        static int said2;
        if (++said2 == 60)
            OHLOG("client buffer missing: cbuf=%{public}p w=%{public}d h=%{public}d "
                  "current.buffer=%{public}p",
                  (void *)cbuf, cbuf ? cbuf->width : -1, cbuf ? cbuf->height : -1,
                  (void *)surf->current.buffer);
        return false;
    }
    void *data = NULL;
    uint32_t format = 0;
    size_t cstride = 0;
    if (!wlr_buffer_begin_data_ptr_access(cbuf, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                          &data, &format, &cstride))
    {
        static int said3;
        if (++said3 == 60)
            OHLOG("client buffer access denied (renderer 持锁?)");
        return false;
    }
    int cw = cbuf->width, ch = cbuf->height;
    int ox = (fw - cw) / 2, oy = (fh - ch) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    int rows = (oy + ch <= fh) ? ch : (fh - oy);
    int cols = (ox + cw <= fw) ? cw : (fw - ox);
    for (int y = 0; y < rows; ++y) {
        const uint8_t *src = (const uint8_t *)data + (size_t)y * cstride;
        uint8_t *dst = px + (size_t)(oy + y) * g_out.frame_stride +
                       (size_t)ox * 4;
        for (int x = 0; x < cols; ++x) {
            /* BGRA(mem) → RGBA(mem): R/B 互换, A 取满 */
            dst[(size_t)x * 4 + 0] = src[(size_t)x * 4 + 2];
            dst[(size_t)x * 4 + 1] = src[(size_t)x * 4 + 1];
            dst[(size_t)x * 4 + 2] = src[(size_t)x * 4 + 0];
            dst[(size_t)x * 4 + 3] = 255;
        }
    }
    wlr_buffer_end_data_ptr_access(cbuf);
    return true;
}

static void RenderFrame(struct wlr_buffer *buf, uint32_t seq)
{
    void *data = NULL;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
                                          &data, &format, &stride))
        return;
    uint8_t *px = data;
    int w = buf->width, h = buf->height;
    if (!BlitClientSurface(px, w, h))
        RenderTestPattern(px, w, h, seq);
    /* CRC 必须在 end_data_ptr_access (Unmap) 之前算——Unmap 后 px 失效,
     * 解引用即 SEGV (T8 实测: row 599 日志后崩, use-after-unmap) */
    g_out.last_crc = FrameCrc(px, (size_t)h * g_out.frame_stride);
    wlr_buffer_end_data_ptr_access(buf);
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
    if (!event || !event->state || !event->state->buffer)
        return;
    ++g_out.frame_seq;

    /* T9: 应答 client surface 的 frame callback。Xwayland child 首帧 commit
     * 后会等 frame 事件再提交后续 damage; 无 scene 图形栈, 须手动应答,
     * 否则 X 窗口内容永远停在首帧/空帧 (T9 实测: surface mapped 但
     * current.buffer 恒 NULL)。 */
    if (g_client && g_client->xs && g_client->xs->surface
            && g_client->xs->surface->mapped)
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_surface_send_frame_done(g_client->xs->surface, &now);
    }

    /* Region 无内嵌数组: rects 是独立指针, 必须指向外部 RegionRect。
     * 之前写 `region.rects = &region.rects[0]` 是对未初始化指针取下标
     * (= 自赋垃圾值), 随栈残留值偶发可写不崩、常则 SEGV——T8 真机三次
     * cppcrash (20:23:33/20:23:57/20:34:37, Faultlogger 定位到本行) 的根因 */
    struct Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = (uint32_t)g_out.frame_buf->width;
    rect.h = (uint32_t)g_out.frame_buf->height;
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

    /* 帧已在 g_out.frame_buf (OH_NativeBuffer) 里; 拷贝进 window buffer。
     * 源读取绕开 wlr_buffer access 计数: commit 事件在 commit_state 内同步
     * 发射, 此时 pixman renderer 尚未 end 对 frame_buf 的 access, 再 begin
     * 会命中 accessing_data 断言 → abort (T8 实测 cppcrash)。
     * OH_NativeBuffer_Map 直接映射同一物理内存, 与访问计数无关。 */
    void *src = NULL;
    if (OH_NativeBuffer_Map(g_out.frame_nb, &src) != 0 || !src) {
        if (fence >= 0) close(fence);
        return;
    }
    int w = region.rects[0].w, ht = region.rects[0].h;
    size_t copy_bytes = (size_t)w * 4;
    uint8_t *d = dst;
    uint8_t *s = src;
    /* 双侧行距一致 (frame_stride) 且 >= 行宽才拷; 行数钳到两侧较小者 */
    if (dst_stride == g_out.frame_stride && dst_stride >= copy_bytes) {
        int rows = (dst_rows > 0 && dst_rows < ht) ? dst_rows : ht;
        for (int y = 0; y < rows; ++y)
            memcpy(d + (size_t)y * dst_stride, s + (size_t)y * g_out.frame_stride,
                   copy_bytes);
    }
    OH_NativeBuffer_Unmap(g_out.frame_nb);

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
}

// 30fps 帧驱动: 重绘同一 buffer → state_set_buffer → commit → commit 事件推屏
static int FrameTick(void *data)
{
    struct wlr_output *output = data;
    static uint32_t tick_seq = 0;
    RenderFrame(g_out.frame_buf, ++tick_seq);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_buffer(&state, g_out.frame_buf);
    pixman_region32_t dmg;
    pixman_region32_init_rect(&dmg, 0, 0,
                              (unsigned int)output->width, (unsigned int)output->height);
    wlr_output_state_set_damage(&state, &dmg);
    wlr_output_test_state(output, &state);
    wlr_output_commit_state(output, &state);
    pixman_region32_fini(&dmg);
    wlr_output_state_finish(&state);
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
    if (g_client == c)
        g_client = NULL;
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
    g_client = c;
    OHLOG("client surface created (%dx%d)", xs->width, xs->height);
}

/* M1-T1: 注入取数口 (display_input.c 调用) */
struct wlr_xwayland_surface *wl_ohos_output_client_xs(void)
{
    return g_client ? g_client->xs : NULL;
}

int wl_ohos_output_chain_start(struct wlr_backend *backend,
                               struct wlr_renderer *renderer,
                               struct wl_event_loop *loop,
                               OHNativeWindow *window,
                               struct wlr_xwayland *xwayland)
{
    memset(&g_out, 0, sizeof(g_out));
    g_out.window = window;

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

    struct wlr_drm_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.format = DRM_FORMAT_ABGR8888;
    fmt.len = 0;
    fmt.capacity = 0;
    g_out.frame_buf = alloc->impl->create_buffer(alloc, 800, 600, &fmt);
    if (!g_out.frame_buf) {
        OH_LOG_ERROR(LOG_APP, "frame buffer alloc failed");
        return -1;
    }
    g_out.frame_nb = wl_ohos_buffer_native(g_out.frame_buf);
    g_out.frame_stride = wl_ohos_buffer_stride(g_out.frame_buf);

    OH_NativeBuffer *nb = wl_ohos_buffer_native(g_out.frame_buf);
    g_out.frame_win_buf = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(nb);
    if (!g_out.frame_win_buf) {
        OH_LOG_ERROR(LOG_APP, "CreateNativeWindowBufferFromNativeBuffer failed");
        return -1;
    }
    int32_t rc = OH_NativeWindow_NativeWindowAttachBuffer(window, g_out.frame_win_buf);
    OHLOG("AttachBuffer rc=%{public}d", rc);
    // window 队列 buffer 几何跟随帧缓冲 (Request 按此分配, memcpy 尺寸才对)
    rc = OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY,
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

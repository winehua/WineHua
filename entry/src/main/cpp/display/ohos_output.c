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
#include "ohos_egl_import.h"

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
#include <wlr/render/swapchain.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/xwayland/xwayland.h>
#include <wlr/util/log.h>
#include <wlr/backend/headless.h>
#include <wlr/render/pixman.h>

#define LOG_TAG "ohos-output"
#define OHLOG(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)

#include <wlr/types/wlr_scene.h>

#include "display_guest_frames.h" /* M2-T5: guest Vulkan 帧接收侧 (帧时钟内驱动) */

struct wl_ohos_output {
    struct wlr_output *output;
    struct wlr_output_layout *layout; /* wl_output global 载体 (Xwayland 镜像) */
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
    struct wlr_scene_buffer *frame_node;  /* M2-T5: guest Vulkan 帧节点 (X 面之上) */
    uint32_t lastSeq; /* lastSeq: 速率仪表的提交序号基线 (见 FrameTick 的 rate 行) */
    struct wl_listener destroy;
    struct wl_listener request_configure;
    struct wl_listener associate; /* xs->surface 后到 (M0 spec §6.2) */
    struct wl_listener dissociate;
    struct wl_listener map_request; /* 生命周期仪器 (M1-T5, 见 ClientMapRequest) */
};

static struct wl_ohos_output g_out;
static struct wl_list g_clients;

static void DumpSceneRoot(const char *why); /* 诊断用, 见下方定义 */

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
    c->lastSeq = xs->surface->current.seq;
    OHLOG("client associated surf=%{public}p (scene attached)",
          (void *)xs->surface);
}

/* M2-T5 帧节点的所有权在本文件, 不在 wlroots: 节点由 frame_set 建在 scene
 * 根上 (X 面节点的同一父树), wlroots 只认识自己那个 X 面节点 —— surface 销毁
 * 时它回收自己的, 不会连带帧节点。因此窗口消失的每条路径都要显式销毁, 否则
 * 留下最后一帧的鬼影 + 节点/队列槽位泄漏 (M2 收尾评审对照实验: 停用本函数后
 * 窗销毁, scene_frames 停在 1)。 */
static void DestroyFrameNode(struct ohos_client_surface *c, const char *why)
{
    if (!c->frame_node)
        return;
    OHLOG("guest frame node destroyed xwin=%{public}u (%{public}s)",
          c->xs ? c->xs->window_id : 0u, why);
    wlr_scene_node_destroy(&c->frame_node->node); /* 连带释放其持有的队列 buffer */
    c->frame_node = NULL;
    DumpSceneRoot(why);
}

static void ClientDissociate(struct wl_listener *listener, void *data)
{
    struct ohos_client_surface *c =
        wl_container_of(listener, c, dissociate);
    (void)data;
    /* surface 已随 dissociate 失效; X 面节点由 wlroots 随 surface 销毁回收,
     * 这里只清引用 (dissociate 先于 surface destroy 送达: 本监听器注册早于
     * scene_surface 自建的销毁监听器)。帧节点不同父属 wlroots, 必须在此显式
     * 销毁: 窗口内容都已消失, 帧不能留下。 */
    DestroyFrameNode(c, "dissociate");
    c->scene_surf = NULL;
}

/* 生命周期仪器 (M1-T5 起, 低量永久保留): MapRequest = client 调了
 * XMapWindow, 与 associate (Xwayland 建 xwl_window + wl_surface 配对)
 * 是独立事件——中间任何一环卡住都表现为「窗口已建但永不上屏」(T5 排障
 * 实证: 屏幕尺寸 0x0 时 created 有而 map request 无)。 */
static void ClientMapRequest(struct wl_listener *listener, void *data)
{
    struct ohos_client_surface *c =
        wl_container_of(listener, c, map_request);
    struct wlr_xwayland_surface *xs = c->xs;
    (void)data;
    if (!xs)
        return;
    OHLOG("client map request %{public}dx%{public}d@%{public}d,%{public}d",
          xs->width, xs->height, xs->x, xs->y);
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

static void LogPresentSegment(struct timespec ts_enter);

static void HandleOutputCommit(struct wl_listener *listener, void *data)
{
    (void)listener;
    struct wlr_output_event_commit *event = data;
    struct timespec ts_enter;
    clock_gettime(CLOCK_MONOTONIC, &ts_enter);
    if (!event || !event->state || !event->state->buffer)
        return;
    ++g_out.frame_seq;

    /* M2-T4 零拷贝分支: 提交的 buffer 就是窗口队列 buffer —— GPU 已经画在
     * 它上面, 这里只剩 GPU 同步 + 归还队列, 没有 mmap/memcpy。判据是 buffer
     * 归属 (present buffer 由 ohos_buffer 包队列 buffer 而来)。 */
    if (wl_ohos_present_buffer_owns(event->state->buffer)) {
        if (wl_ohos_egl_active())
            wl_ohos_egl_finish(); /* 显示消费前必须 GPU 写完 (glFinish) */
        int32_t prc = wl_ohos_present_buffer_present(event->state->buffer, -1);
        if (prc != 0 && (g_out.frame_seq % 30) == 1)
            OH_LOG_ERROR(LOG_APP, "present FlushBuffer rc=%{public}d (帧 %{public}u)",
                         prc, g_out.frame_seq);
        if ((g_out.frame_seq % 30) == 1)
            OHLOG("commit seq=%{public}u mode=present", g_out.frame_seq);
        LogPresentSegment(ts_enter);
        return;
    }

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
    size_t mapped_bytes = 0; /* mmap 的实际长度 (M2-T1: munmap 必须同源配对) */
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
        mapped_bytes = bytes;
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
    /* M2-T4: gles2 渲染器下源 buffer 是 GL 渲染目标 —— 读像素前必须等 GPU
     * 写完 (pass submit 只 glFlush, 同步到 CPU 可见要 glFinish)。pixman
     * 路径是 CPU 直接写, 本调用空转。零拷贝输出落地后本段整体退位。 */
    if (wl_ohos_egl_active())
        wl_ohos_egl_finish();
    if (!src_nb || OH_NativeBuffer_Map(src_nb, &src) != 0 || !src) {
        /* 源映射失败: window buffer 必须归还, 否则路径 G 的 BufferQueue
         * 槽位连续泄漏几次即 RequestBuffer 饿死 (known-issues §1.1,
         * M2-T1)。AbortBuffer (since 8) = 无内容归还, 槽位立即可复用;
         * 路径 C 走 UnlockFlush。fence 归属: FlushBuffer 文档明确由系统
         * 关闭, AbortBuffer 文档未提 —— 自行关闭 (保守侧, 防 fd 泄漏)。 */
        if (mapped)
        {
            int32_t rc = OH_NativeWindow_NativeWindowAbortBuffer(g_out.window, win_buf);
            if (rc != 0)
                OH_LOG_ERROR(LOG_APP, "AbortBuffer rc=%{public}d (帧 %{public}u)",
                             rc, g_out.frame_seq);
        }
        else
        {
            g_unlock_flush(g_out.window);
        }
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
    } else {
        /* 行距分叉: 当前无逐行变 stride 拷贝能力, 本帧只能丢弃 —— 但必须
         * 可见 (known-issues §1.1): 无日志的丢帧 = "偶发掉帧且零证据"。
         * M2-T4 零拷贝落地后此分支整体退位。 */
        static unsigned stride_mismatch_count;
        ++stride_mismatch_count;
        if ((stride_mismatch_count % 30) == 1)
            OH_LOG_ERROR(LOG_APP, "stride mismatch dst=%{public}zu src=%{public}zu "
                         "copy=%{public}zu, frame dropped (count=%{public}u)",
                         dst_stride, src_stride, copy_bytes, stride_mismatch_count);
    }
    OH_NativeBuffer_Unmap(src_nb);

    if (mapped) {
        munmap(dst, mapped_bytes); /* 与 mmap 同源 (M2-T1) */
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
    LogPresentSegment(ts_enter);
}

/* 分段计时: commit→NativeWindow 段的每帧耗时 (平均/最大, 每 120 帧打一次)。
 * 拷贝路径的量是 mmap 拷贝 + Flush; 零拷贝 present 路径的量是 GPU 同步 +
 * Flush —— 段名不变, 前后可直接对照 (M2-T4 出口判据看的就是这一段归零)。 */
static void LogPresentSegment(struct timespec ts_enter)
{
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    int64_t us = (int64_t)(ts_now.tv_sec - ts_enter.tv_sec) * 1000000 +
                 (ts_now.tv_nsec - ts_enter.tv_nsec) / 1000;
    static int64_t sum_us;
    static int64_t max_us;
    static int n;
    static struct timespec win_start;
    sum_us += us;
    if (us > max_us) max_us = us;
    if (n == 0)
        win_start = ts_enter;
    if (++n >= 120) {
        OHLOG("segment copy+flush: avg=%{public}lldus max=%{public}lldus n=%{public}d",
              (long long)(sum_us / n), (long long)max_us, n);
        /* 提交节拍 (T4 出口判据): 分段量是**工作**时间, 本行是**节拍**时间
         * (含帧时钟空转) —— 与 M1-T3 基线 18.6fps (33ms 时钟 + ~20ms 工作,
         * 2 倍关系在同一模型下自洽) 对照的是本行的 period/fps。 */
        int64_t span_us = (int64_t)(ts_enter.tv_sec - win_start.tv_sec) * 1000000 +
                          (ts_enter.tv_nsec - win_start.tv_nsec) / 1000;
        int64_t period_us = (n > 1) ? span_us / (n - 1) : 0;
        OHLOG("commit rate: period=%{public}lldus fps_x10=%{public}lld n=%{public}d",
              (long long)period_us,
              (long long)(period_us > 0 ? 10000000LL / period_us : 0), n);
        sum_us = 0;
        max_us = 0;
        n = 0;
    }
}

/* ── M2-T4 零拷贝 present: 每帧借一格队列 buffer 当渲染目标 ─────────────
 *
 * 队列 buffer 不能跨帧复用 (见 ohos_buffer.h: 未重新 Request 就写 = 状态
 * 非法), 而 wlroots 的 output swapchain 是长期复用同一批 buffer 的模型 ——
 * 两者对不上, 所以 present 路径**每帧新建一个只服务本帧的 swapchain**, 并把
 * 刚借到的那一格经下面的单次 allocator 交给它。scene 渲染直接落在队列
 * buffer 上 (gles2 经导入器把 buffer 当 FBO), commit 事件里只做 GPU 同步 +
 * FlushBuffer (见 HandleOutputCommit 的 present 分支) —— 全程无 mmap/memcpy。
 *
 * 任一环节失败都归还队列并返回 false: 调用方回落既有拷贝路径 (pixman 与
 * gles2 都仍可用), 不会出现黑屏。 */
struct PresentOneShotAllocator {
    struct wlr_allocator base;
    struct wlr_buffer *buffer;
    int taken;
};

static struct wlr_buffer *OneShotCreateBuffer(struct wlr_allocator *alloc,
                                             int width, int height,
                                             const struct wlr_drm_format *format)
{
    struct PresentOneShotAllocator *one = (struct PresentOneShotAllocator *)alloc;
    (void)width; (void)height; (void)format; /* 存储已由队列 buffer 定 */
    if (one->taken || !one->buffer)
        return NULL;
    /* 所有权移交, 不是借用: wlr_allocator_create_buffer 返回的 buffer 由
     * 调用方 (swapchain 槽) 持有, 槽在 slot_reset 里 drop 恰好一次
     * (render/swapchain.c:40-48,107)。这里若再加一次 wlr_buffer_lock,
     * 槽的 drop 会把 dropped 置位而 n_locks>0 使其不销毁 → 包装泄漏, 且
     * 调用方事后的 drop 命中 assert(!dropped) —— 实测: assert 走 OHOS
     * AssertCallback → SendSyncEvent 等主线程, 事件循环线程从此永久停在
     * wlr_buffer_drop 里 (Faultlogger cppcrash 20260930042002 栈:
     * __assert_fail ← wlr_buffer_drop+84 ← … ← wl_event_loop_dispatch),
     * 合成停摆、Xwayland 握手不完成。 */
    one->taken = 1;
    return one->buffer;
}

static void OneShotAllocatorDestroy(struct wlr_allocator *alloc)
{
    free(alloc);
}

static const struct wlr_allocator_interface kOneShotAllocatorImpl = {
    .create_buffer = OneShotCreateBuffer,
    .destroy = OneShotAllocatorDestroy,
};

static struct wlr_allocator *OneShotAllocatorCreate(struct wlr_buffer *buffer)
{
    struct PresentOneShotAllocator *one =
        calloc(1, sizeof(struct PresentOneShotAllocator));
    if (!one)
        return NULL;
    one->buffer = buffer;
    wlr_allocator_init(&one->base, &kOneShotAllocatorImpl,
                       WLR_BUFFER_CAP_DATA_PTR);
    return &one->base;
}

/* 零拷贝一帧。返回 true = 已渲染并提交 (flush 在 commit 监听器里完成);
 * false = 本帧没出 (调用方回落拷贝路径或丢帧)。 */
static int PresentFrameZeroCopy(void)
{
    struct wlr_buffer *buf = wl_ohos_present_buffer_acquire(g_out.window);
    if (!buf)
        return 0; /* 队列无空槽 = 显示端背压, 本帧丢 (与拷贝路径同语义) */

    /* scene 的 build_state 对渲染目标有尺寸断言: 队列 buffer 与输出尺寸
     * 不一致时必须在这里挡下 (回落拷贝路径), 不能带进 wlroots */
    if (g_out.output && (buf->width != g_out.output->width ||
                         buf->height != g_out.output->height)) {
        static unsigned mismatch;
        if ((mismatch++ % 30) == 0)
            OH_LOG_ERROR(LOG_APP, "present buffer %{public}dx%{public}d != output "
                         "%{public}dx%{public}d, 本帧回落拷贝路径 (count=%{public}u)",
                         buf->width, buf->height, g_out.output->width,
                         g_out.output->height, mismatch);
        wl_ohos_present_buffer_abort(buf);
        wlr_buffer_drop(buf);
        return 0;
    }

    int ok = 0;
    struct wlr_allocator *alloc = OneShotAllocatorCreate(buf);
    struct PresentOneShotAllocator *one =
        (struct PresentOneShotAllocator *)alloc; /* taken 判据 (见 out:) */
    struct wlr_swapchain *swapchain = NULL;
    if (!alloc)
        goto out;
    {
        /* 格式只作 swapchain 记账 (存储由队列 buffer 定); 取队列 buffer 的
         * 实际 DRM 码, 认不出时用 XRGB8888 兜底 (与输出 primary format 同族) */
        uint32_t drm = wl_ohos_buffer_drm_format(buf);
        struct wlr_drm_format format = {0};
        format.format = drm ? drm : DRM_FORMAT_XRGB8888;
        swapchain = wlr_swapchain_create(alloc, buf->width, buf->height, &format);
        if (!swapchain)
            goto out;
        struct wlr_scene_output_state_options options = {0};
        options.swapchain = swapchain;
        ok = wlr_scene_output_commit(g_out.scene_output, &options);
    }
out:
    /* 队列槽位归还必须在 swapchain 销毁**之前**: 交给 swapchain 的 buffer 由
     * slot_reset 在这次销毁里 drop (唯一一次) → n_locks 归零随即 impl->destroy
     * 释放包装 —— 之后再访问 buf 就是 UAF。没被 present 分支归还的
     * (commit 失败 / 未走到) 在这里归还, 否则这一格队列槽位永久留在 dequeued,
     * 几次之后 RequestBuffer 饿死 (known-issues §1.1)。 */
    if (!wl_ohos_present_buffer_returned(buf))
        wl_ohos_present_buffer_abort(buf);
    bool taken = (one != NULL && one->taken);
    if (swapchain)
        wlr_swapchain_destroy(swapchain);
    if (alloc)
        wlr_allocator_destroy(alloc);
    if (!taken) {
        /* 没交出去 (alloc 没被取走 / swapchain 建不起来): 唯一一次 drop 归我们 */
        wlr_buffer_drop(buf);
    }
    return ok;
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
    /* M2-T5: guest Vulkan 帧先落点再摆位 (frame_set 建/置节点, 下面的循环
     * 统一按窗几何校正)。必须在 scene 提交之前 —— 本拍到的帧本拍就上屏。 */
    display_guest_frames_tick();
    wl_list_for_each(c, &g_clients, link) {
        if (c->scene_surf && c->xs)
            wlr_scene_node_set_position(&c->scene_surf->buffer->node,
                                        c->xs->x, c->xs->y);
        /* M2-T5: 帧节点跟窗走 (位置/尺寸以窗为准, 帧尺寸不合时按窗缩放) */
        if (c->frame_node && c->xs) {
            wlr_scene_node_set_position(&c->frame_node->node, c->xs->x, c->xs->y);
            wlr_scene_buffer_set_dest_size(c->frame_node, c->xs->width,
                                           c->xs->height);
        }
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
        /* M2-T4: gles2 + 可见窗 ⇒ 零拷贝 present (渲染直落队列 buffer);
         * 其余情形 (pixman 回退 / 无窗 / present 失败) 走既有 scene+拷贝路径。
         * 先判 needs_frame: 无 damage 时 scene 直接返回 true 不渲染, 而借用
         * 的队列 buffer 必须归还 (漏还 = 槽位永久丢失) —— 别进那条路。 */
        bool presented = false;
        if (render && wl_ohos_egl_active() && g_out.window && g_out.scene_output)
            presented = PresentFrameZeroCopy() != 0;
        if (!presented)
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
        /* 速率仪表 (M2-B 内容率排查, 每秒一行, 低量常驻): 链路每一跳的通过量。
         *   surfCommits = Σ client surface 的 current.seq 增量 —— Xwayland 把
         *                 X 窗口内容推给我们的频率 (跳 ③→④)
         *   outCommits  = 输出 commit 增量 —— 真正上屏的帧数 (跳 ⑥)
         *   needsFrame  = 本秒内"有 damage"的帧时钟拍数 (跳 ⑤ 的入口条件)
         *   ticks       = 帧时钟拍数 (应 ~30/s)
         * 判读: 客户端侧速率 (XCLIENT-STAT 行) 高而 surfCommits 低 ⇒ 丢在 ②~④;
         * surfCommits 高而 outCommits 低 ⇒ 丢在 ⑤/⑥。 */
        {
            struct ohos_client_surface *rc;
            static uint64_t s_surfCommits, s_needsFrame, s_ticks, s_lastOut, s_lastNs;
            wl_list_for_each(rc, &g_clients, link) {
                if (!rc->xs || !rc->xs->surface)
                    continue;
                uint32_t seq = rc->xs->surface->current.seq;
                s_surfCommits += (uint32_t)(seq - rc->lastSeq);
                rc->lastSeq = seq;
            }
            ++s_ticks;
            if (render)
                ++s_needsFrame;
            uint64_t ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
            if (!s_lastNs) {
                s_lastNs = ns;
                s_lastOut = g_out.frame_seq;
            } else if (ns - s_lastNs >= 1000000000ull) {
                OHLOG("rate surfCommits=%{public}llu outCommits=%{public}llu "
                      "needsFrame=%{public}llu ticks=%{public}llu over=%{public}llums",
                      (unsigned long long)s_surfCommits,
                      (unsigned long long)(g_out.frame_seq - s_lastOut),
                      (unsigned long long)s_needsFrame, (unsigned long long)s_ticks,
                      (unsigned long long)((ns - s_lastNs) / 1000000));
                s_lastOut = g_out.frame_seq;
                s_surfCommits = 0;
                s_needsFrame = 0;
                s_ticks = 0;
                s_lastNs = ns;
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
    /* dissociate 通常先到, 但 destroy 可能单独送达 (未 associate 就销毁, 或
     * 事件顺序变化): 帧节点的销毁不能只挂在 dissociate 上。 */
    DestroyFrameNode(c, "destroy");
    wl_list_remove(&c->destroy.link);
    wl_list_remove(&c->request_configure.link);
    wl_list_remove(&c->associate.link);
    wl_list_remove(&c->dissociate.link);
    wl_list_remove(&c->map_request.link);
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
    c->map_request.notify = ClientMapRequest;
    wl_signal_add(&xs->events.map_request, &c->map_request);
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

/* ── M2-T5: guest Vulkan 帧 (Venus 私有 present) 的落点 ─────────────────────
 * 查找一律按 window_id 每次遍历 (映射表不另存): 窗口销毁即从 g_clients 摘除,
 * 因此"查不到"就是「该 id 已失效」——X window id 会被 X server 复用, 复用的
 * 新窗是另一条 g_clients 记录, 老帧进不了新窗。 */

static struct ohos_client_surface *FindClientByWindow(uint32_t xwindow)
{
    struct ohos_client_surface *c;
    if (!xwindow)
        return NULL;
    wl_list_for_each(c, &g_clients, link) {
        if (c->xs && c->xs->window_id == xwindow)
            return c;
    }
    return NULL;
}

int wl_ohos_output_client_frame_anchor(uint32_t xwindow, int *x, int *y, int *w, int *h)
{
    struct ohos_client_surface *c = FindClientByWindow(xwindow);
    struct wlr_xwayland_surface *xs;
    if (!c)
        return 0;
    xs = c->xs;
    /* scene 锚 = 该窗 X 面的节点 (associate 才建; 未 associate 时帧无处可挂) */
    if (!c->scene_surf || !c->scene_surf->buffer)
        return 0;
    if (x) *x = xs->x;
    if (y) *y = xs->y;
    if (w) *w = xs->width;
    if (h) *h = xs->height;
    return 1;
}

int wl_ohos_output_client_xwindow_alive(uint32_t xwindow)
{
    return FindClientByWindow(xwindow) != NULL;
}

int wl_ohos_output_client_frame_set(uint32_t xwindow, struct wlr_buffer *buffer)
{
    struct ohos_client_surface *c = FindClientByWindow(xwindow);
    struct wlr_xwayland_surface *xs;
    if (!c || !buffer)
        return 0;
    xs = c->xs;
    if (!c->scene_surf || !c->scene_surf->buffer)
        return 0;
    if (!c->frame_node) {
        /* 挂到 X 面节点的同一父树 —— wlr_scene_surface_create 把 X 面节点直接
         * 建在 scene 根上 (wlroots 侧没有中间 tree, 见 types/scene/surface.c),
         * 所以这里取到的父就是 scene 根。place_above 把帧插到本窗 X 面之后:
         * scene 按父节点子表序绘制, 该位置既压住本窗内容, 又不会越过后注册
         * 的窗口 (后建窗口仍在子表更后)。节点归本文件所有, 窗口消失时必须
         * 显式销毁 (DestroyFrameNode) —— wlroots 不连带回收它。 */
        struct wlr_scene_tree *parent =
            wlr_scene_tree_from_node(c->scene_surf->buffer->node.parent);
        c->frame_node = wlr_scene_buffer_create(parent, NULL);
        if (!c->frame_node) {
            OH_LOG_ERROR(LOG_APP, "guest frame node create failed xwin=%{public}u",
                         xwindow);
            return 0;
        }
        wlr_scene_node_place_above(&c->frame_node->node,
                                   &c->scene_surf->buffer->node);
        DumpSceneRoot("frame node created");
    }
    /* set_buffer 解锁旧帧 (归还队列槽位由 buffer 析构兜底), dest_size 按窗口
     * 几何 —— 帧尺寸与窗一致时是恒等变换, 不一致时按窗口缩放 (不留黑边)。 */
    wlr_scene_buffer_set_buffer(c->frame_node, buffer);
    wlr_scene_buffer_set_dest_size(c->frame_node, xs->width, xs->height);
    wlr_scene_node_set_position(&c->frame_node->node, xs->x, xs->y);
    wlr_scene_node_set_enabled(&c->frame_node->node, true);
    return 1;
}

void wl_ohos_output_client_frame_clear(uint32_t xwindow)
{
    struct ohos_client_surface *c = FindClientByWindow(xwindow);
    if (!c || !c->frame_node)
        return;
    wlr_scene_buffer_set_buffer(c->frame_node, NULL);
    wlr_scene_node_set_enabled(&c->frame_node->node, false);
}

/* 递归数 scene 里还挂着本模块借来的队列 buffer 的节点 (即活着的 guest 帧).
 * 数 scene 真实状态而不是本文件的记录: 窗口记录在销毁时已 free, 孤儿节点却
 * 还挂在 scene 上 —— 那正是泄漏看不见的原因。判据: 窗口全部销毁后回 0。 */
static uint32_t CountOwnedFrames(struct wlr_scene_tree *tree)
{
    struct wlr_scene_node *n;
    uint32_t count = 0;
    wl_list_for_each(n, &tree->children, link) {
        if (n->type == WLR_SCENE_NODE_TREE) {
            count += CountOwnedFrames(wlr_scene_tree_from_node(n));
        } else if (n->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(n);
            if (sb->buffer && wl_ohos_consumer_buffer_owns(sb->buffer))
                ++count;
        }
    }
    return count;
}

uint32_t wl_ohos_output_frames_in_scene(void)
{
    return g_out.scene ? CountOwnedFrames(&g_out.scene->tree) : 0;
}

/* 诊断: 逐个子节点打类型/位置/启用态。计数对不上时靠它定位是谁的节点
 * (本文件建的只有背景 rect、X 面节点、帧节点三类)。 */
static void DumpSceneRoot(const char *why)
{
    struct wlr_scene_node *n;
    char line[512];
    int off = 0;
    if (!g_out.scene)
        return;
    wl_list_for_each(n, &g_out.scene->tree.children, link) {
        const char *t = n->type == WLR_SCENE_NODE_TREE ? "tree" :
                        (n->type == WLR_SCENE_NODE_BUFFER ? "buf" : "rect");
        off += snprintf(line + off, sizeof(line) - (size_t)off, " %s@%d,%d%s",
                        t, n->x, n->y, n->enabled ? "" : "*off");
        if (off >= (int)sizeof(line) - 32)
            break;
    }
    OHLOG("scene root (%{public}s):%{public}s", why, line);
}

int wl_ohos_output_chain_start(struct wlr_backend *backend,
                               struct wlr_renderer *renderer,
                               struct wl_event_loop *loop,
                               struct wl_display *display,
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

    /* wl_output global 载体: Xwayland rootless 镜像 compositor 的
     * wl_output 为 xwl_output (RANDR/Xinerama), 没有它 X 屏幕 0x0 →
     * wine is_window_rect_mapped 恒 FALSE → 窗口永不 map (T5 实测)。
     * late-arriving global 会被既有客户端 (xwm/已连 X client) 经
     * registry 广播收到, 无需时序。 */
    g_out.layout = wlr_output_layout_create(display);
    if (!g_out.layout || !wlr_output_layout_add_auto(g_out.layout, g_out.output)) {
        OH_LOG_ERROR(LOG_APP, "output layout create/add failed");
        return -1;
    }

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
    DumpSceneRoot("chain start");
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

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
#include <errno.h>
#include <sys/eventfd.h>
#include <native_vsync/native_vsync.h> /* 帧时钟 (任务 2): 系统 VSync 驱动 */
#include <sys/mman.h>
#include <sys/stat.h>
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
#include "../common/display_fps.h" /* 宿主显示序列发布 (guest displayed 门判据) */

struct wl_ohos_output {
    struct wlr_output *output;
    struct wlr_output_layout *layout; /* wl_output global 载体 (Xwayland 镜像) */
    struct wlr_scene *scene;            /* M1-T3: scene 图形栈 */
    struct wlr_scene_output *scene_output;
    struct wlr_scene_rect *bg_rect;     /* 背景兜底 rect (resize 随动, D10) */
    OHNativeWindow *window;
    struct wl_listener commit_listener;
    struct wl_listener xnew_surface; /* xwayland->events.new_surface (T9) */
    struct wl_event_source *frame_timer;  /* 兜底节拍 + VSync 看门狗 (任务 2) */
    struct wl_event_source *vsync_source; /* wl_event_loop_add_fd(vsync_fd) */
    OH_NativeVSync *vsync;                /* 帧时钟主驱动 (任务 2) */
    int vsync_fd;                         /* eventfd: VSync 线程 -> event loop */
    int64_t vsync_last_ns;                /* 最近 VSync 回调时刻 (跨线程, __atomic) */
    int64_t frame_period_ns;              /* 显示周期 (VSync 上报, 0 = 未接入) */
    uint64_t last_frame_key;              /* 最近交给 scene 的 guest 帧归属键 */
    uint64_t last_frame_ns;               /* 该帧的交出时刻 (归属发布用) */
    bool vsync_stalled;                   /* VSync 停摆/未启动 ⇒ 走兜底节拍 */
    int out_w;                            /* 输出尺寸 (chain_start 入参, M3a);
                                           * ≤0 视为未设, frame_size 回退 800x600 */
    int out_h;
    uint32_t frame_seq;
    uint32_t last_crc;
    struct wlr_xwayland *xwayland; /* D23 子窗几何查询 (chain_start 入参) */
};

/* D23 子窗 face 绑定: 虚拟桌面应用窗是 X 子窗口, 不在 g_clients (xwm 只
 * associate 顶层)。几何走 wlr_xwayland_query_child_geometry (wlroots xwm
 * 子窗表, 见 wlroots-ohos-xwm-child-geometry.patch), 帧节点挂 scene 根、
 * 按 guest 绝对坐标定位。注册表只管节点生命周期: 窗口消失 (查询失败) 由
 * wl_ohos_output_child_faces_sweep 回收 —— 与 g_clients 的 DestroyFrameNode
 * 同一泄漏防线 (M2-T5: wlroots 不连带回收本模块的帧节点)。 */
struct ohos_child_face {
    uint32_t xwindow;
    uint64_t generation;
    struct wlr_scene_buffer *frame_node;
    struct wl_list link;
};
static struct wl_list g_child_faces;

/* T9: X client surface 跟踪。T2 起为链表 (创建序, 链尾 = 最上层):
 * 多窗口 blit 按序画 (后创建压前), 注入命中测试按几何反查。映射状态
 * 不挂监听器——wlr_surface.mapped 轮询 (33ms 帧驱动内天然覆盖),
 * destroy 监听防悬垂。 */
struct ohos_client_surface {
    struct wlr_xwayland_surface *xs;
    /* 记录身份: X window id 会回收复用, 只比 id 的失效判据会被"同 id 新窗"
     * 骗过 (§2.7)。绑定方存挂接时的 generation, 每拍比对 —— 这就是 §2.7
     * 要求的"按记录身份而不是按 id"的仪器。 */
    uint64_t generation;
    struct wl_list link; /* g_clients */
    struct wlr_scene_surface *scene_surf; /* M1-T3: scene 节点 */
    struct wlr_scene_buffer *frame_node;  /* M2-T5: guest Vulkan 帧节点 (X 面之上) */
    uint32_t lastSeq; /* lastSeq: 速率仪表的提交序号基线 (见 FrameTick 的 rate 行) */
    int dbgLastX;     /* 诊断: 上次记录过的 xs->x (XPOS 时间线) */
    struct wl_listener destroy;
    struct wl_listener request_configure;
    struct wl_listener request_activate; /* D36: wine 激活请求 = 置前 */
    struct wl_listener request_restack;  /* D37: wine SetWindowPos Z 序变更 */
    struct wl_listener associate; /* xs->surface 后到 (M0 spec §6.2) */
    struct wl_listener dissociate;
    struct wl_listener map_request; /* 生命周期仪器 (M1-T5, 见 ClientMapRequest) */
};

static struct wl_ohos_output g_out;
static struct wl_list g_clients;
static uint64_t g_clientGeneration; /* 只增: 每条 client surface 记录一个身份 */

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
/* x11 桌面 shell 就绪标志 (M3a): 桌面根 toplevel 是 wayland 私有协议的
 * 概念, X 路线没有 —— LaunchPadMode 的 15s 根等待 (wine_launch.cpp) 只认
 * GetDesktopRootToplevelId, x11 下必然超时 → state:ready-degraded → 没有
 * evt:desktop-ready 补票 → engineState 永远停在 degraded, smoke 跑测门
 * (engineState==='ready') 永不过, runner 永不启动 (实测 2026-10-04, rate
 * job 三轮 15min 超时)。X 等价信号 = 首个 client 的 XMapWindow (桌面
 * shell 是链上第一个映射者); 置位供 launch 等待谓词同源判定, 并补发同一
 * 条 evt:desktop-ready (超时后迟到的映射走 ArkTS degraded 升级路径)。 */
static int g_desktop_shell_mapped;

int WineHua_DisplayRoute_DesktopShellMapped(void)
{
    return g_desktop_shell_mapped;
}

/* 定义在 display_compositor.cpp: 经会话状态通道补发 evt:desktop-ready
 * (WaylandServer 单例的 stateCb 与路线无关 —— napi_init 的引擎消息通道)。 */
extern void WineHua_DisplayRoute_FireDesktopReady(void);

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
    if (!g_desktop_shell_mapped) {
        g_desktop_shell_mapped = 1;
        WineHua_DisplayRoute_FireDesktopReady();
    }
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

/* ── 诊断: 背景像素异常检测 (常驻门禁) + 呈现时间线 (marker 开启) ──────────
 * 背景: 矩形移动场景曾出现「宿主送出的帧背景整块黑」(局部 damage 落到从没
 * 画过背景的队列槽位, 见 PresentFrameZeroCopy 的修复说明)。这里对每帧送屏
 * 前的背景采样点做校验, 偏离即 ERROR + 落 PPM 取证 —— 修复回归的第一现场。
 * 时间线 (XPOS/PRES) 与落盘预算默认关, 用 drive_c 下的 marker 文件打开
 * (diag-present-timeline / WINEHUA_PRESENT_DUMP), 避免常驻日志量。 */
static int64_t NowNs(void);
static int DiagFlagFile(const char *name);

/* 诊断开关 (默认关, 验证/排障时在 drive_c 下放 marker 文件打开):
 *   drive_c/diag-present-timeline  XPOS/PRES 呈现时间线日志
 *   WINEHUA_PRESENT_DUMP: 异常帧落盘预算 (张), 背景像素校验常驻 (门禁) */
static int DiagTimelineOn(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("WINEHUA_PRESENT_TIMELINE");
        if (v && *v)
            on = strcmp(v, "0") != 0;
        else
            on = DiagFlagFile("diag-present-timeline");
    }
    return on;
}

static int DiagDumpBudget(void)
{
    static int budget = -2;
    if (budget == -2) {
        const char *v = getenv("WINEHUA_PRESENT_DUMP");
        budget = (v && *v) ? atoi(v) : 40; /* 诊断期默认 40 张 */
        if (budget > 0) {
            mkdir("/data/storage/el2/base/files/.wine/drive_c/framedump", 0755);
            OHLOG("diag: 背景异常落盘已开, budget=%{public}d", budget);
        }
    }
    return budget;
}

/* ── 诊断开关: 文件存在即生效 (免重编; 1s 缓存) ────────────────────────────
 * drive_c/force-copy-present   跳过零拷贝, 走 M1 式 渲染→mmap→memcpy 路径
 * drive_c/force-timer-clock    停用 VSync 主驱动, 退 33ms 定时节拍
 * drive_c/diag-present-timeline 打开 XPOS/PRES 呈现时间线日志 */
static int DiagFlagFile(const char *name)
{
    static const char *names[3] = {"force-copy-present", "force-timer-clock",
                                   "diag-present-timeline"};
    static int val[3] = {-1, -1, -1};
    static int64_t lastNs[3];
    int idx = (strcmp(name, names[0]) == 0) ? 0
              : (strcmp(name, names[1]) == 0) ? 1 : 2;
    int64_t now = NowNs();
    if (val[idx] < 0 || now - lastNs[idx] > 1000000000ll) {
        char path[256];
        snprintf(path, sizeof(path),
                 "/data/storage/el2/base/files/.wine/drive_c/%s", names[idx]);
        struct stat st;
        val[idx] = (stat(path, &st) == 0);
        lastNs[idx] = now;
    }
    return val[idx];
}

/* 背景采样点 (避开两窗几何: win1 x60..404 y80..320, win2 x380..660 y300..500) */
static const int kBgPts[][2] = {{700, 560}, {30, 560}, {760, 40}, {350, 560}, {700, 120}};

/* 校验「刚渲染完、即将交给消费者的那一帧」的深色底 (期望 ~26,26,31)。
 * 偏离即 ERROR + 落 PPM (最多 budget 张; 另存一张基准帧)。 */
static void DiagPresentFrame(struct wlr_buffer *buffer, uint32_t seq)
{
    static int dumped, refd;
    if (DiagDumpBudget() <= 0)
        return;
    struct NativeWindowBuffer *nwb = wl_ohos_present_buffer_window_buffer(buffer);
    BufferHandle *h = nwb ? OH_NativeWindow_GetBufferHandleFromNative(nwb) : NULL;
    if (!h || h->fd < 0)
        return;
    size_t bytes = (h->size > 0) ? (size_t)h->size : (size_t)h->stride * h->height;
    if (bytes < (size_t)h->stride * (size_t)buffer->height)
        return;
    const uint8_t *base = mmap(NULL, bytes, PROT_READ, MAP_SHARED, h->fd, 0);
    if (base == MAP_FAILED)
        return;
    int bad = 0, bi = -1;
    uint8_t br = 0, bg = 0, bb = 0;
    for (size_t i = 0; i < sizeof(kBgPts) / sizeof(kBgPts[0]); ++i) {
        const uint8_t *px = base + (size_t)kBgPts[i][1] * h->stride +
                            (size_t)kBgPts[i][0] * 4;
        if (!(px[0] >= 8 && px[0] <= 56 && px[1] >= 8 && px[1] <= 56 &&
              px[2] >= 8 && px[2] <= 56)) {
            bad = 1;
            bi = (int)i;
            br = px[0];
            bg = px[1];
            bb = px[2];
            break;
        }
    }
    if (bad) {
        ++dumped;
        OH_LOG_ERROR(LOG_APP,
                     "diag: 背景采样偏离 frame=%{public}u pt=%{public}d rgb=%{public}u,%{public}u,%{public}u count=%{public}d",
                     seq, bi, br, bg, bb, dumped);
    }
    /* 落盘条件: 异常帧 (预算内) / 启动前 12 帧 / 一张基准帧 */
    if ((bad && dumped <= DiagDumpBudget()) || seq <= 12 || (!refd && seq > 3)) {
        refd = 1;
        char path[256];
        snprintf(path, sizeof(path),
                 "/data/storage/el2/base/files/.wine/drive_c/framedump/f%06u.ppm",
                 seq);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", buffer->width, buffer->height);
            for (int y = 0; y < buffer->height; ++y) {
                const uint8_t *row = base + (size_t)y * h->stride;
                for (int x = 0; x < buffer->width; ++x)
                    fwrite(row + (size_t)x * 4, 1, 3, f);
            }
            fclose(f);
        }
    }
    munmap((void *)base, bytes);
}

static void HandleOutputCommit(struct wl_listener *listener, void *data)
{
    (void)listener;
    struct wlr_output_event_commit *event = data;
    struct timespec ts_enter;
    clock_gettime(CLOCK_MONOTONIC, &ts_enter);
    if (!event || !event->state || !event->state->buffer)
        return;
    /* present 挂起 (D8): 画布未到, output 照常推进 (frame_done 驱动 X client
     * 绘制, scene 照常渲染进 swapchain —— 桌面内容在合成器里活着), 只是
     * 无消费者不出屏。frame_seq 不计 (它语义 = 已 present 帧)。 */
    if (!g_out.window)
        return;
    ++g_out.frame_seq;

    /* M3a-T7 画布 EGL swap 呈现: 提交的 buffer 是默认 swapchain 的
     * allocator buffer (OH_NativeBuffer 背书, gles2 渲染器本就经 EGLImage
     * 画入), 导入为纹理 → blit 到窗口 EGLSurface → swap。全程 GPU-GPU,
     * 不碰队列槽位、不 FlushBuffer、无 CPU 视图 —— 手工 FlushBuffer 对
     * 画布 surface 消费侧冻结 (vd12), CPU mmap 又与 GPU 写入不可靠一致
     * (vd21/22c: probe fail:pixel-compare + framedump 垃圾)。判据 = buffer
     * 由本 allocator 背书 (wl_ohos_buffer_native 仅认 allocator buffer)。 */
    if (wl_ohos_egl_window_surface_active() &&
        wl_ohos_buffer_native(event->state->buffer) &&
        wl_ohos_egl_window_present(event->state->buffer, g_out.window)) {
        if ((g_out.frame_seq % 30) == 1)
            OHLOG("commit seq=%{public}u mode=egl-swap-tex", g_out.frame_seq);
        LogPresentSegment(ts_enter);
        return;
    }

    /* M2-T4 零拷贝分支 (fusion 预览路径, 已证): 提交的 buffer 就是窗口队列
     * buffer —— GPU 已经画在它上面, 这里只剩 GPU 同步 + 归还队列, 没有
     * mmap/memcpy。判据是 buffer 归属 (present buffer 由 ohos_buffer 包
     * 队列 buffer 而来)。 */
    if (wl_ohos_present_buffer_owns(event->state->buffer)) {
        if (wl_ohos_egl_active())
            wl_ohos_egl_finish(); /* 显示消费前必须 GPU 写完 (glFinish) */
        DiagPresentFrame(event->state->buffer, g_out.frame_seq);
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
 * 队列 buffer 不能跨帧**免 Request**复用 (见 ohos_buffer.h: 未重新 Request
 * 就写 = 状态非法), 而每帧仍要 Request/Flush; 持久化的是包装层 —— wrapper +
 * allocator + swapchain 三元组按队列句柄缓存 (见 ohos_buffer.h 的 slot 一节,
 * T6.5; 旧一次性 swapchain 在桌面尺寸下是每帧 20MB 级分配风暴, 内核图形侧
 * ~230s 压爆)。scene 渲染直接落在队列 buffer 上 (gles2 经导入器把 buffer 当
 * FBO, 导入随 wrapper 持久化), commit 事件里只做 GPU 同步 + FlushBuffer
 * (见 HandleOutputCommit 的 present 分支) —— 全程无 mmap/memcpy。
 *
 * 任一环节失败都归还队列并返回 false: 调用方回落既有拷贝路径 (pixman 与
 * gles2 都仍可用), 不会出现黑屏。 */
/* 零拷贝一帧。返回 true = 已渲染并提交 (flush 在 commit 监听器里完成);
 * false = 本帧没出 (调用方回落拷贝路径或丢帧)。 */
static int PresentFrameZeroCopy(void)
{
    struct wlr_swapchain *swapchain = NULL;
    struct wlr_buffer *buf =
        wl_ohos_present_slot_acquire(g_out.window, &swapchain);
    if (!buf || !swapchain)
        return 0; /* 队列无空槽 = 显示端背压, 本帧丢 (与拷贝路径同语义) */

    /* M3a-T7 修复实验 (2026-10-05): 画布 surface (DesktopAbility 全屏窗)
     * 消费侧冻结在首帧 —— 同一 flush 代码对主窗预览面正常、对画布面冻结,
     * 而 wayland presenter (virgl_surface_presenter.cpp:192) 每次 present
     * 前 SET_UI_TIMESTAMP, 本链路此前不带。对齐之。 */
    {
        int32_t ts_rc = OH_NativeWindow_NativeWindowHandleOpt(
            g_out.window, SET_UI_TIMESTAMP, NowNs());
        static int ts_bad;
        if (ts_rc != 0 && (ts_bad++ % 120) == 0)
            OH_LOG_ERROR(LOG_APP, "SET_UI_TIMESTAMP rc=%{public}d (count=%{public}d)",
                         ts_rc, ts_bad);
    }

    /* scene 的 build_state 对渲染目标有尺寸断言: 队列 buffer 与输出尺寸
     * 不一致时必须在这里挡下 (回落拷贝路径), 不能带进 wlroots。只 abort
     * 不 drop —— wrapper 归 slot 表, 尺寸切换的旧句柄由表容量自然淘汰。 */
    if (g_out.output && (buf->width != g_out.output->width ||
                         buf->height != g_out.output->height)) {
        static unsigned mismatch;
        if ((mismatch++ % 30) == 0)
            OH_LOG_ERROR(LOG_APP, "present buffer %{public}dx%{public}d != output "
                         "%{public}dx%{public}d, 本帧回落拷贝路径 (count=%{public}u)",
                         buf->width, buf->height, g_out.output->width,
                         g_out.output->height, mismatch);
        wl_ohos_present_buffer_abort(buf);
        return 0;
    }

    /* 每帧按全幅 damage 重绘 (本变更只做持久化一件事, 控制变量): 旧一次性
     * wrapper 下 ring 记账与物理存储错配, 局部 damage 落到没画过背景的槽位
     * = 背景黑帧 (设备 .5 实测, known-issues §2.15)。T6.5 起三元组按句柄
     * 持久, ring 条目 ↔ wrapper ↔ 存储一一对应, 局部 damage 的前提已成立
     * —— 但它与持久化是两个变量, 收益也未测量 (原则 #19): 先持久化过验证,
     * 局部 damage 作为后续实测过的优化单独做。代价: 全幅重绘实测 segment
     * avg ~4.4ms, 120Hz 预算 8.3ms 内。不用 wlr_damage_ring_add_whole: 它
     * 从 ring 现存 buffer 取尺寸, ring 为空时是空操作。 */
    pixman_region32_t whole;
    pixman_region32_init_rect(&whole, 0, 0,
                              (int)buf->width, (int)buf->height);
    wlr_damage_ring_add(&g_out.scene_output->damage_ring, &whole);
    pixman_region32_fini(&whole);

    struct wlr_scene_output_state_options options = {0};
    options.swapchain = swapchain;
    int ok = wlr_scene_output_commit(g_out.scene_output, &options);

    /* 队列槽位归还: 没被 present 分支归还的 (commit 失败 / 未走到) 在这里
     * 归还, 否则这一格队列槽位永久留在 dequeued, 几次之后 RequestBuffer
     * 饿死 (known-issues §1.1)。wrapper 本体归 slot 表, 不 drop 不销毁。 */
    if (!wl_ohos_present_buffer_returned(buf))
        wl_ohos_present_buffer_abort(buf);
    return ok;
}

/* ── 帧时钟 (任务 2): 系统 VSync 主驱动, 33ms 定时器兜底 ──────────────────
 * 变更前: `wl_event_source_timer_update(…, 33)` 自建 30Hz 节拍 ⇒ 输出被钳在
 * ~30fps (spec §1.3 实测)。现在主驱动是系统 VSync (期望区间 {60,120,120},
 * 与 wayland 渲染器同口径), 回调只写 eventfd 唤醒 event loop —— 渲染仍在
 * loop 线程 (wlroots 非线程安全)。VSync 缺席/停摆 ⇒ 自动退回 33ms 定时器
 * (行为与本变更前一致), 回调恢复 ⇒ 自动切回 VSync。 */
#define FRAME_FALLBACK_MS 33  /* 兜底节拍 (与本变更前口径一致) */
#define VSYNC_STALL_MS 200    /* 超过此时长未见回调 ⇒ 判停摆 */
#define VSYNC_WATCHDOG_MS 250 /* VSync 存活时的看门狗周期 */

static void FrameStep(bool via_vsync);

static int64_t NowNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + (int64_t)ts.tv_nsec;
}

/* VSync 回调 (VSync 线程): 只记时刻 + 写 eventfd 唤醒, 不碰渲染状态。 */
static void OnVSync(long long timestamp, void *data)
{
    (void)timestamp;
    (void)data;
    __atomic_store_n(&g_out.vsync_last_ns, NowNs(), __ATOMIC_RELAXED);
    if (g_out.vsync_fd >= 0) {
        uint64_t one = 1;
        /* EAGAIN = 本拍已有待处理唤醒, 合并即可 (时钟不数拍, 只做节拍) */
        ssize_t n = write(g_out.vsync_fd, &one, sizeof(one));
        (void)n;
    }
}

static void VsyncRearm(void)
{
    if (g_out.vsync && OH_NativeVSync_RequestFrame(g_out.vsync, OnVSync, NULL) != 0)
        OHLOG("帧时钟: RequestFrame 失败, 等看门狗续订");
}

/* eventfd 唤醒 (event loop 线程): 渲染一帧后按拍续订 —— 渲染完成再续订,
 * 掉帧时丢拍而不是堆积多个待处理唤醒。 */
static int HandleVSyncWake(int fd, uint32_t mask, void *data)
{
    (void)mask;
    (void)data;
    uint64_t v;
    while (read(fd, &v, sizeof(v)) == (ssize_t)sizeof(v))
        ;
    if (g_out.vsync_stalled) {
        g_out.vsync_stalled = false;
        OHLOG("帧时钟: VSync 回调恢复, 从 %{public}dms 兜底节拍切回", FRAME_FALLBACK_MS);
    }
    {
        /* 显示周期只有回调之后才可用 (native_vsync.h:159 说明)。guest 侧
         * present 节奏要跟随它, 所以每次唤醒都取一次 —— 但上报值逐拍抖动
         * 几微秒 (实测 8330~8334us), 原样下发会让每次抖动都变成一次 IPC 与
         * 一行日志, 所以只认超过 0.5ms 的变化 (与 egl_renderer 同阈值)。 */
        long long period = 0;
        if (OH_NativeVSync_GetPeriod(g_out.vsync, &period) == 0 && period > 0) {
            int64_t prev = __atomic_load_n(&g_out.frame_period_ns, __ATOMIC_RELAXED);
            if (prev == 0 || period > prev + 500000 || period < prev - 500000) {
                __atomic_store_n(&g_out.frame_period_ns, (int64_t)period, __ATOMIC_RELAXED);
                OHLOG("帧时钟: 显示周期 %{public}lldns (%{public}.2fHz), 前值 %{public}lldns",
                      period, 1000000000.0 / (double)period, (long long)prev);
            }
        }
    }
    FrameStep(true);
    VsyncRearm();
    return 0;
}

/* 一拍 (M1-T3 scene 版): frame_done 无条件按节拍泵出 (client 的 frame 节流
 * 靠它解锁 —— 不泵则 client 等回调、无新 damage、scene 无帧可提, 互等死锁,
 * gate1 实测停在首帧), commit 由 scene damage 门控 (画面静止时零渲染零拷贝)。
 * headless output 无自身 frame 事件, 本函数即帧时钟的一拍。
 * via_vsync 只用于 rate 行的节拍来源归因。 */
static void FrameStep(bool via_vsync)
{
    struct ohos_client_surface *c;
    /* M2-T5: guest Vulkan 帧先落点再摆位 (frame_set 建/置节点, 下面的循环
     * 统一按窗几何校正)。必须在 scene 提交之前 —— 本拍到的帧本拍就上屏。 */
    display_guest_frames_tick();
    wl_list_for_each(c, &g_clients, link) {
        if (c->scene_surf && c->xs)
            wlr_scene_node_set_position(&c->scene_surf->buffer->node,
                                        c->xs->x, c->xs->y);
        /* 诊断 (XPOS): 位置实际生效的时刻 —— 与客户端 XMOVE 行并排即得随动延迟 */
        if (c->xs && DiagTimelineOn() && c->dbgLastX != c->xs->x) {
            OHLOG("XPOS t=%{public}lldms win=0x%{public}lx x=%{public}d y=%{public}d",
                  (long long)(NowNs() / 1000000), (unsigned long)c->xs->window_id,
                  c->xs->x, c->xs->y);
            c->dbgLastX = c->xs->x;
        }
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
         * 的队列 buffer 必须归还 (漏还 = 槽位永久丢失) —— 别进那条路。
         * 画布 EGL swap 路径 (window surface active) 不借队列槽位: EGL 面
         * 已占格, 再借 = frame 2 起饥饿 (vd22c 实测), 场景渲染走默认
         * swapchain (allocator buffer), commit 后由 EGLImage 导入分支上屏。 */
        bool presented = false;
        if (render && !DiagFlagFile("force-copy-present") && wl_ohos_egl_active() &&
            g_out.window && g_out.scene_output &&
            !wl_ohos_egl_window_surface_active())
            presented = PresentFrameZeroCopy() != 0;
        if (!presented)
            wlr_scene_output_commit(g_out.scene_output, NULL);
        /* 诊断 (PRES): 每帧上屏 + 各窗当前 x —— 「同一位置连续几帧」直接可数 */
        if (render && DiagTimelineOn()) {
            char pos[64] = {0};
            struct ohos_client_surface *tc;
            wl_list_for_each(tc, &g_clients, link) {
                if (!tc->xs)
                    continue;
                char one[24];
                snprintf(one, sizeof(one), "%s%d", pos[0] ? "," : "", tc->xs->x);
                strncat(pos, one, sizeof(pos) - strlen(pos) - 1);
            }
            OHLOG("PRES t=%{public}lldms seq=%{public}u zc=%{public}d x=[%{public}s]",
                  (long long)(NowNs() / 1000000), g_out.frame_seq,
                  presented ? 1 : 0, pos);
        }
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
            static uint64_t s_vsyncTicks, s_timerTicks;
            wl_list_for_each(rc, &g_clients, link) {
                if (!rc->xs || !rc->xs->surface)
                    continue;
                uint32_t seq = rc->xs->surface->current.seq;
                s_surfCommits += (uint32_t)(seq - rc->lastSeq);
                rc->lastSeq = seq;
            }
            ++s_ticks;
            if (via_vsync)
                ++s_vsyncTicks;
            else
                ++s_timerTicks;
            if (render)
                ++s_needsFrame;
            uint64_t ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
            if (!s_lastNs) {
                s_lastNs = ns;
                s_lastOut = g_out.frame_seq;
            } else if (ns - s_lastNs >= 1000000000ull) {
                /* 宿主显示序列发布 (guest 用例的 displayed 门读它, 与 wayland
                 * 渲染器共用 common/perf_utils.cpp 的写入实现): 只有当这一秒
                 * 真的提交过帧才推进序号 —— 序号不动 = 宿主没出图。 */
                {
                    uint64_t outCommits = g_out.frame_seq - s_lastOut;
                    if (outCommits)
                    {
                        /* 归属: 本秒内交给 scene 的 guest 面 (1 秒内没有则 0) */
                        uint64_t frameNs =
                            __atomic_load_n(&g_out.last_frame_ns, __ATOMIC_RELAXED);
                        uint64_t key = (frameNs && ns - frameNs <= 1000000000ull)
                                           ? g_out.last_frame_key : 0;
                        winehua_display_fps_publish(
                            0, g_out.frame_seq,
                            (double)outCommits * 1e9 / (double)(ns - s_lastNs),
                            "x11", key);
                    }
                }
                OHLOG("rate surfCommits=%{public}llu outCommits=%{public}llu "
                      "needsFrame=%{public}llu ticks=%{public}llu vsync=%{public}llu "
                      "timer=%{public}llu over=%{public}llums",
                      (unsigned long long)s_surfCommits,
                      (unsigned long long)(g_out.frame_seq - s_lastOut),
                      (unsigned long long)s_needsFrame, (unsigned long long)s_ticks,
                      (unsigned long long)s_vsyncTicks,
                      (unsigned long long)s_timerTicks,
                      (unsigned long long)((ns - s_lastNs) / 1000000));
                /* 客户端面状态dump (2026-10-04 全黑排查落地, M3 后续
                 * fusion+x11 接线期保留): buffer=NULL + mapped=1 = XWM
                 * 已映射但内容未到达; mapped=0 = XWM 未映射; buffer≠NULL
                 * + seq 推进 = 内容链路通。每秒一行, 与 rate 同生命周期。 */
                wl_list_for_each(rc, &g_clients, link) {
                    if (!rc->xs || !rc->xs->surface)
                        continue;
                    /* hilog 隐私规则: 裸 %p/%d/%u 打成 <private>, 指针按本文件
                     * rate 行的惯例转 unsigned long long 配 %{public}llu。 */
                    OHLOG("client-state surf=%{public}llx buffer=%{public}llx "
                          "mapped=%{public}d %{public}dx%{public}d@%{public}d,%{public}d "
                          "seq=%{public}u",
                          (unsigned long long)(uintptr_t)rc->xs->surface,
                          (unsigned long long)(uintptr_t)rc->xs->surface->buffer,
                          rc->xs->surface->mapped ? 1 : 0,
                          rc->xs->width, rc->xs->height, rc->xs->x, rc->xs->y,
                          rc->xs->surface->current.seq);
                }
                /* 一次性 buffer 像素 dump (2026-10-04 蓝底暗化排查):
                 * 实测结论 —— 桌面窗 mirror buffer 是 GL/dmabuf (access
                 * denied, 无 data ptr), Xwayland glamor 路径; 像素级判别
                 * 需走 EGL import + glReadPixels, 未做。保留接入点: 若
                 * 再需像素对质, 从这里扩 EGL import 路径。 */
                static bool s_buffer_dumped;
                static int s_buffer_dump_tries;
                if (!s_buffer_dumped && s_buffer_dump_tries < 15) {
                    s_buffer_dump_tries++;
                    struct ohos_client_surface *dc = NULL, *it;
                    wl_list_for_each_reverse(it, &g_clients, link) {
                        if (wl_ohos_surface_has_content(it->xs ? it->xs->surface
                                                               : NULL)) {
                            dc = it;
                            break;
                        }
                    }
                    if (dc) {
                        s_buffer_dumped = true;
                        struct wlr_buffer *wb = &dc->xs->surface->buffer->base;
                        void *data = NULL;
                        uint32_t fmt = 0;
                        size_t stride = 0;
                        if (wlr_buffer_begin_data_ptr_access(wb, 0, &data,
                                                             &fmt, &stride)) {
                            int bw = wb->width, bh = wb->height;
                            uint32_t *row0 = (uint32_t *)data;
                            uint32_t *rowN = (uint32_t *)((char *)data +
                                (size_t)(bh - 1) * stride);
                            uint32_t *center = (uint32_t *)((char *)data +
                                (size_t)(bh / 2) * stride +
                                (size_t)(bw / 2) * 4);
                            OHLOG("buf-dump fmt=%{public}u stride=%{public}zu "
                                  "%{public}dx%{public}d tl=%{public}08x "
                                  "tr=%{public}08x bl=%{public}08x "
                                  "br=%{public}08x c=%{public}08x",
                                  fmt, stride, bw, bh,
                                  row0[0], row0[bw - 1],
                                  rowN[0], rowN[bw - 1], *center);
                            FILE *f = fopen(
                                "/data/storage/el2/base/cache/desktop_dump.ppm",
                                "wb");
                            if (f) {
                                fprintf(f, "P6\n%d %d\n255\n", bw, bh);
                                for (int y = 0; y < bh; y++) {
                                    uint32_t *r = (uint32_t *)((char *)data +
                                        (size_t)y * stride);
                                    for (int x = 0; x < bw; x++) {
                                        uint32_t p = r[x]; /* ARGB8888 */
                                        uint8_t bgr[3] = { (uint8_t)p,
                                            (uint8_t)(p >> 8),
                                            (uint8_t)(p >> 16) };
                                        fwrite(bgr, 1, 3, f);
                                    }
                                }
                                fclose(f);
                                OHLOG("buf-dump written desktop_dump.ppm");
                            } else {
                                OHLOG("buf-dump fopen failed errno=%{public}d",
                                      errno);
                            }
                            wlr_buffer_end_data_ptr_access(wb);
                        } else {
                            /* GL/dmabuf 后端可能无 data ptr —— 也算结论:
                             * mirror 走的是 texture 路径而非 shm。一次性
                             * 不重试 (dc 命中即置 s_buffer_dumped)。 */
                            OHLOG("buf-dump access denied (GL/dmabuf buffer)");
                        }
                    }
                }
                s_lastOut = g_out.frame_seq;
                s_surfCommits = 0;
                s_needsFrame = 0;
                s_ticks = 0;
                s_vsyncTicks = 0;
                s_timerTicks = 0;
                s_lastNs = ns;
            }
        }
    }
}

/* 兜底节拍 + VSync 看门狗 (同一个定时器): VSync 存活时本回调只做停摆检测,
 * 不渲染; 停摆/缺席时按 33ms 泵帧 (对本变更前行为)。 */
static int FrameTick(void *data)
{
    (void)data;
    /* A/B: 强制 33ms 定时节拍 (M1 口径), 用于对照「滞留是否 VSync 时钟引入」 */
    if (DiagFlagFile("force-timer-clock")) {
        FrameStep(false);
        wl_event_source_timer_update(g_out.frame_timer, FRAME_FALLBACK_MS);
        return 0;
    }
    if (g_out.vsync && !g_out.vsync_stalled) {
        int64_t last = __atomic_load_n(&g_out.vsync_last_ns, __ATOMIC_RELAXED);
        int64_t now = NowNs();
        if (!last || now - last > (int64_t)VSYNC_STALL_MS * 1000000ll) {
            g_out.vsync_stalled = true;
            OHLOG("帧时钟: VSync 停摆 (最后回调 %{public}lldms 前) ⇒ 降级 %{public}dms 兜底节拍",
                  (long long)(last ? (now - last) / 1000000 : -1), FRAME_FALLBACK_MS);
        }
    }
    if (g_out.vsync && !g_out.vsync_stalled) {
        wl_event_source_timer_update(g_out.frame_timer, VSYNC_WATCHDOG_MS);
        return 0;
    }
    if (g_out.vsync) /* 停摆中: 每拍重试续订, 恢复由 HandleVSyncWake 收敛 */
        VsyncRearm();
    FrameStep(false);
    wl_event_source_timer_update(g_out.frame_timer, FRAME_FALLBACK_MS);
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
    wl_list_remove(&c->request_activate.link);
    wl_list_remove(&c->request_restack.link);
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

/* ── D36/D37: 窗口序 (raise/restack) ─────────────────────────────────
 * 此前 g_clients 与 scene 节点都只在 map 时插入 (创建序 = 视觉序, 永不
 * 重排)。现在两个通道驱动重排: ①request_activate (wine 激活/对话框弹出,
 * _NET_ACTIVE_WINDOW) + display_input 的 button press (点击置前) 走
 * wl_ohos_output_client_raise; ②wine SetWindowPos 的 Z 序变更
 * (XReconfigureWMWindow → stack-only ConfigureRequest, D37 wlroots 补丁
 * 应用后发 request_restack 事件) 走 client_restack —— to-top/to-bottom
 * 与兄弟相对插序都在此承接。
 * g_clients 约定: 链头 = 最底层, 链尾 = 最上层。scene 节点序与链表同步:
 * 窗口单元 = X 面节点 + 帧节点 (帧在自己窗口的 X 面之上, 与创建时的相对
 * 序一致)。0.20 的 wlr_scene_surface 无公开 tree 字段, 节点走
 * buffer->node。本函数在 wlroots 侧完成 X restack 之后被调用 (D37 事件
 * 时序保证), X 侧 _NET_CLIENT_LIST_STACKING 与合成器视觉序一致。 */
static void client_restack(struct wlr_xwayland_surface *xs,
                           struct wlr_xwayland_surface *sibling_xs,
                           enum xcb_stack_mode_t mode)
{
    struct ohos_client_surface *target = NULL;
    struct ohos_client_surface *sib = NULL;
    struct ohos_client_surface *c;
    wl_list_for_each(c, &g_clients, link) {
        if (c->xs == xs) {
            target = c;
            break;
        }
    }
    if (!target)
        return;
    if (sibling_xs && sibling_xs != xs) {
        wl_list_for_each(c, &g_clients, link) {
            if (c->xs == sibling_xs) {
                sib = c;
                break;
            }
        }
        if (!sib)
            return; /* 兄弟不在跟踪表内, 无法相对定位 (X 侧已 restack, 不破坏现状) */
    }

    if (mode == XCB_STACK_MODE_ABOVE) {
        if (sib) {
            wl_list_remove(&target->link);
            wl_list_insert(&sib->link, &target->link); /* sib 之后 = sib 之上 */
        } else {
            wl_list_remove(&target->link);
            wl_list_insert(g_clients.prev, &target->link); /* 链尾 = 最上层 */
        }
    } else if (mode == XCB_STACK_MODE_BELOW) {
        if (sib) {
            wl_list_remove(&target->link);
            wl_list_insert(sib->link.prev, &target->link); /* sib 之前 = sib 之下 */
        } else {
            wl_list_remove(&target->link);
            wl_list_insert(g_clients.next, &target->link); /* 链头 = 最底层 */
        }
    } else {
        return; /* TopIf/BottomIf/Opposite: D37 补丁在 xwm 侧同样跳过 */
    }

    /* scene 节点跟随。成对移动保帧/面相对序:
     * to-top: 面先提、帧后提; to-bottom: 帧先沉底、面再沉底 (帧留在面上方);
     * 兄弟相对: 以兄弟的节点为锚逐个 place (锚取兄弟最外层节点)。 */
    if (sib) {
        struct wlr_scene_node *sib_face =
            sib->scene_surf ? &sib->scene_surf->buffer->node : NULL;
        struct wlr_scene_node *sib_top =
            sib->frame_node ? &sib->frame_node->node : sib_face;
        struct wlr_scene_node *sib_bottom = sib_face ? sib_face : sib_top;
        if (!sib_bottom) {
            return; /* 兄弟尚无 scene 节点 (未 associate): 链表序已更新, 节点序随其挂载自愈 */
        }
        if (mode == XCB_STACK_MODE_BELOW) {
            /* 锚 = 兄弟最底层节点 (面): 帧先落到锚下、面再落到帧下,
             * 整对插进兄弟对之下 (锚取 sib_top 会插进兄弟的面/帧对中间)。 */
            if (target->frame_node)
                wlr_scene_node_place_below(&target->frame_node->node, sib_bottom);
            if (target->scene_surf)
                wlr_scene_node_place_below(&target->scene_surf->buffer->node,
                                           target->frame_node
                                               ? &target->frame_node->node
                                               : sib_bottom);
        } else if (target->scene_surf) {
            /* 锚 = 兄弟最上层节点 (帧): 面先提、帧后提, 整对落在兄弟对之上 */
            wlr_scene_node_place_above(&target->scene_surf->buffer->node, sib_top);
            if (target->frame_node)
                wlr_scene_node_place_above(&target->frame_node->node,
                                           &target->scene_surf->buffer->node);
        }
    } else if (mode == XCB_STACK_MODE_ABOVE) {
        if (target->scene_surf)
            wlr_scene_node_raise_to_top(&target->scene_surf->buffer->node);
        if (target->frame_node)
            wlr_scene_node_raise_to_top(&target->frame_node->node);
    } else {
        if (target->frame_node)
            wlr_scene_node_lower_to_bottom(&target->frame_node->node);
        if (target->scene_surf)
            wlr_scene_node_lower_to_bottom(&target->scene_surf->buffer->node);
    }
    OHLOG("client restack xs=%{public}p xwin=%{public}u mode=%{public}d sib=%{public}p",
          (void *)xs, xs ? xs->window_id : 0u, (int)mode, (void *)sibling_xs);
}

void wl_ohos_output_client_raise(struct wlr_xwayland_surface *xs)
{
    client_restack(xs, NULL, XCB_STACK_MODE_ABOVE);
}

static void ClientRequestRestack(struct wl_listener *listener, void *data)
{
    struct ohos_client_surface *c =
        wl_container_of(listener, c, request_restack);
    const struct wlr_xwayland_surface_restack_event *ev = data;
    if (!ev)
        return;
    client_restack(c->xs, ev->sibling, ev->mode);
}

static void ClientRequestActivate(struct wl_listener *listener, void *data)
{
    struct ohos_client_surface *c =
        wl_container_of(listener, c, request_activate);
    (void)data;
    wl_ohos_output_client_raise(c->xs);
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
    c->generation = ++g_clientGeneration;
    c->destroy.notify = ClientDestroy;
    wl_signal_add(&xs->events.destroy, &c->destroy);
    c->request_configure.notify = ClientRequestConfigure;
    wl_signal_add(&xs->events.request_configure, &c->request_configure);
    c->request_activate.notify = ClientRequestActivate;
    wl_signal_add(&xs->events.request_activate, &c->request_activate);
    c->request_restack.notify = ClientRequestRestack;
    wl_signal_add(&xs->events.request_restack, &c->request_restack);
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
 * frame_size = 命中坐标归一化的基准 (chain_start 入参, M3a 起可参数化)。 */
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
    /* 链未建时 g_out 为零值 ⇒ 回退 800x600 (与 chain_start 未指定同口径) */
    if (w) *w = g_out.out_w > 0 ? g_out.out_w : 800;
    if (h) *h = g_out.out_h > 0 ? g_out.out_h : 600;
}

/* ── M2-T5: guest Vulkan 帧 (Venus 私有 present) 的落点 ─────────────────────
 * 查找一律按 window_id 每次遍历 (映射表不另存): 窗口销毁即从 g_clients 摘除,
 * 因此"查不到"就是「该 id 已失效」——X window id 会被 X server 复用, 复用的
 * 新窗是另一条 g_clients 记录, 老帧进不了新窗。 */

/* D23: 子窗几何查询 (g_clients 未命中时的回退)。命中 = 活着的虚拟桌面子
 * 窗口; 失败 = 窗口不存在 (TryAttach 闸门/sweep 回收据此判定)。 */
static int ChildQuery(uint32_t xwindow, int *x, int *y, int *w, int *h,
                      uint64_t *generation)
{
    if (!g_out.xwayland)
        return 0;
    int32_t qx, qy;
    uint32_t qw, qh;
    uint64_t qg;
    if (!wlr_xwayland_query_child_geometry(g_out.xwayland, xwindow,
                                           &qx, &qy, &qw, &qh, &qg))
        return 0;
    if (x) *x = qx;
    if (y) *y = qy;
    if (w) *w = (int)qw;
    if (h) *h = (int)qh;
    if (generation) *generation = qg;
    return 1;
}

/* D31: 子窗客户区查询。GL/Vulkan 帧内容是客户区 (GL drawable), 面锚必须
 * 扣掉 wine 自绘装饰的内缩 (客户端经 _NET_WM_FRAME_EXTENTS 声明), 否则
 * face 拉伸铺满全窗矩形、盖住标题栏。客户端未声明 (无边框窗/属性未到)
 * 时回退全窗矩形 = 客户区, 退化安全。liveness/generation 语义同 ChildQuery。 */
static int ChildClientAreaQuery(uint32_t xwindow, int *x, int *y, int *w, int *h,
                                uint64_t *generation)
{
    if (!g_out.xwayland)
        return 0;
    int32_t qx, qy;
    uint32_t qw, qh;
    uint64_t qg;
    if (!wlr_xwayland_query_child_client_area(g_out.xwayland, xwindow,
                                              &qx, &qy, &qw, &qh, &qg))
        return 0;
    if (x) *x = qx;
    if (y) *y = qy;
    if (w) *w = (int)qw;
    if (h) *h = (int)qh;
    if (generation) *generation = qg;
    return 1;
}

static struct ohos_child_face *ChildFaceFind(uint32_t xwindow)
{
    struct ohos_child_face *f;
    wl_list_for_each(f, &g_child_faces, link) {
        if (f->xwindow == xwindow)
            return f;
    }
    return NULL;
}

static void ChildFaceDestroy(struct ohos_child_face *f, const char *why)
{
    OHLOG("child face node destroyed xwin=%{public}u (%{public}s)",
          f->xwindow, why);
    if (f->frame_node)
        wlr_scene_node_destroy(&f->frame_node->node);
    wl_list_remove(&f->link);
    free(f);
}

void wl_ohos_output_child_faces_sweep(void)
{
    struct ohos_child_face *f, *tmp;
    wl_list_for_each_safe(f, tmp, &g_child_faces, link) {
        uint64_t gen = 0;
        if (ChildQuery(f->xwindow, NULL, NULL, NULL, NULL, &gen) &&
            gen == f->generation)
            continue;
        /* 窗口没了, 或同 id 重建 (generation 变了) ⇒ 节点必须销毁:
         * 重建场景下一帧 frame_set 会以新 generation 重挂。 */
        ChildFaceDestroy(f, gen != f->generation ? "regenerated" : "window gone");
    }
}

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
    {
        /* D23: 虚拟桌面子窗口不在 g_clients —— 几何走 xwm 子窗表。 */
        return ChildQuery(xwindow, x, y, w, h, NULL);
    }
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

int wl_ohos_output_client_xwindow_generation(uint32_t xwindow, uint64_t *generation)
{
    struct ohos_client_surface *c = FindClientByWindow(xwindow);
    if (!c)
    {
        /* D23: 子窗口 generation = xwm 子窗表的单调计数 (重建即变)。 */
        return ChildQuery(xwindow, NULL, NULL, NULL, NULL, generation);
    }
    if (generation)
        *generation = c->generation;
    return 1;
}

int wl_ohos_output_client_frame_set(uint32_t xwindow, struct wlr_buffer *buffer,
                                    int flip_vertical, uint64_t surface_key)
{
    struct ohos_client_surface *c = FindClientByWindow(xwindow);
    struct wlr_xwayland_surface *xs;
    if (!c)
    {
        /* D23: 虚拟桌面子窗口 —— 帧节点挂 scene 根, 按 guest 绝对坐标定位。
         * D31: 锚矩形用客户区 (帧内容 = GL drawable = 客户区), 装饰区让给
         * 桌面缓冲里 wine 自绘的标题栏/边框。
         * 已知限制 (首版): 节点在 scene 根 = 对全部兄弟子窗置顶, 被其他
         * 窗口遮挡的 GL 窗会穿帮; 遮挡正确性待 X stacking 查询补。 */
        int x, y, w, h;
        uint64_t gen = 0;
        if (!buffer || !ChildClientAreaQuery(xwindow, &x, &y, &w, &h, &gen))
            return 0;
        struct ohos_child_face *f = ChildFaceFind(xwindow);
        if (f && f->generation != gen)
        {
            /* 同 id 重建: 旧节点作废, 下一帧以新 generation 重挂 */
            ChildFaceDestroy(f, "regenerated");
            f = NULL;
        }
        if (!f)
        {
            f = calloc(1, sizeof(*f));
            if (!f)
                return 0;
            f->xwindow = xwindow;
            f->generation = gen;
            f->frame_node = wlr_scene_buffer_create(&g_out.scene->tree, NULL);
            if (!f->frame_node)
            {
                OH_LOG_ERROR(LOG_APP,
                             "child face node create failed xwin=%{public}u",
                             xwindow);
                free(f);
                return 0;
            }
            wl_list_insert(&g_child_faces, &f->link);
            DumpSceneRoot("child face node created");
            /* D31 诊断: 首挂锚矩形 (客户区 = 全矩形 - FRAME_EXTENTS 内缩)
             * 与 buffer 实际尺寸。二者不等 = 局部/拉伸形态的来源。 */
            OHLOG("child face anchor xwin=%{public}u %{public}dx%{public}d@%{public}d,%{public}d buf=%{public}ux%{public}u gen=%{public}llu",
                  xwindow, w, h, x, y,
                  (unsigned int)buffer->width, (unsigned int)buffer->height,
                  (unsigned long long)gen);
        }
        /* 行序修正与顶层路径同口径: GL(virgl) 翻, Vulkan(venus) 不翻。 */
        wlr_scene_buffer_set_transform(f->frame_node,
                                       flip_vertical
                                           ? WL_OUTPUT_TRANSFORM_FLIPPED_180
                                           : WL_OUTPUT_TRANSFORM_NORMAL);
        wlr_scene_buffer_set_buffer(f->frame_node, buffer);
        wl_ohos_consumer_buffer_note_handoff(buffer, g_out.frame_seq,
                                             wl_ohos_egl_sync_count());
        wlr_scene_buffer_set_dest_size(f->frame_node, (uint32_t)w, (uint32_t)h);
        wlr_scene_node_set_position(&f->frame_node->node, x, y);
        wlr_scene_node_set_enabled(&f->frame_node->node, true);
        g_out.last_frame_key = surface_key;
        __atomic_store_n(&g_out.last_frame_ns, (uint64_t)NowNs(),
                         __ATOMIC_RELAXED);
        return 1;
    }
    if (!buffer)
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
    /* 行序修正必须在 set_buffer 之前定下 (见头文件 flip_vertical 的实测依据):
     * GL(virgl) 面纵向翻一次, Vulkan(venus) 面不翻。 */
    wlr_scene_buffer_set_transform(c->frame_node,
                                   flip_vertical ? WL_OUTPUT_TRANSFORM_FLIPPED_180
                                                 : WL_OUTPUT_TRANSFORM_NORMAL);
    /* set_buffer 解锁旧帧 (归还队列槽位由 buffer 析构兜底), dest_size 按窗口
     * 几何 —— 帧尺寸与窗一致时是恒等变换, 不一致时按窗口缩放 (不留黑边)。 */
    wlr_scene_buffer_set_buffer(c->frame_node, buffer);
    /* 打上「交出去的时刻」: 归还时拿它比对 (known-issues §2.6 不变量检查器)。
     * 必须在 set_buffer 之后 —— 旧帧的归还发生在 set_buffer 内部, 它比对的
     * 是上一拍打的标记。 */
    wl_ohos_consumer_buffer_note_handoff(buffer, g_out.frame_seq,
                                         wl_ohos_egl_sync_count());
    wlr_scene_buffer_set_dest_size(c->frame_node, xs->width, xs->height);
    wlr_scene_node_set_position(&c->frame_node->node, xs->x, xs->y);
    wlr_scene_node_set_enabled(&c->frame_node->node, true);
    /* 归属证据: 本秒内交给 scene 的是谁家的帧 (随显示序列发布给 guest,
     * 见 common/display_fps.h)。新帧即 damage ⇒ 本拍就会提交。 */
    g_out.last_frame_key = surface_key;
    __atomic_store_n(&g_out.last_frame_ns, (uint64_t)NowNs(), __ATOMIC_RELAXED);
    return 1;
}

uint32_t wl_ohos_output_present_seq(void)
{
    return g_out.frame_seq;
}

uint64_t wl_ohos_output_frame_period_ns(void)
{
    int64_t period = __atomic_load_n(&g_out.frame_period_ns, __ATOMIC_RELAXED);

    if (period > 0 && g_out.vsync && !g_out.vsync_stalled)
        return (uint64_t)period;
    return (uint64_t)FRAME_FALLBACK_MS * 1000000ull;
}

void wl_ohos_output_client_frame_clear(uint32_t xwindow)
{
    struct ohos_client_surface *c = FindClientByWindow(xwindow);
    if (!c)
    {
        /* D23: 子窗 face。与顶层同口径, 必须在调用方 (DropBinding) 销毁
         * OH_NativeImage **之前**摘掉帧节点: 节点上挂着借自该 image 的
         * 队列 buffer, image 先死后归还 = 悬垂间接调用 → CFI 陷阱
         * (2026-10-08 实测, GL 程序退出致 app 崩溃, D32 主进程死因;
         * 旧注释「清理走 sweep」的次序不成立 —— sweep 发生在 image 死后)。 */
        struct ohos_child_face *f = ChildFaceFind(xwindow);
        if (f)
            ChildFaceDestroy(f, "binding dropped");
        return;
    }
    if (!c->frame_node)
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

/* 窗口 present 配置段 (D8 自 chain_start 抽出): 窗口可在链启动后才到达
 * (产品路径 want 时起链、画布 engine-ready 才开), 首配与晚到挂载共用同一
 * 份声明, 漏一项的症状见各段注释。loop 线程调用。返回 0 = 就绪。 */
static int WindowPresentSetup(OHNativeWindow *window, bool canvas_egl_present)
{
    // window 队列 buffer 几何声明 (Request 按此分配, memcpy 尺寸才对)
    int32_t rc = OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY,
                                                       g_out.out_w, g_out.out_h);
    OHLOG("SET_BUFFER_GEOMETRY rc=%{public}d", rc);
    /* 超时必须显式设 0: SDK 默认 3000ms, 而本窗口的 RequestBuffer 跑在**帧
     * 时钟的 event loop 线程**上 —— 消费者(预览 XComponent)不还槽位时会把
     * 整条链冻结 3s (scene 不提交、Xwayland 包不 flush、注入队列不 drain),
     * 表观与 §2.3 的"停滞"同形，排查容易误判成 box64/冷启。两个 presenter
     * 的窗口都设了 0 (virgl_surface_presenter.cpp / venus_surface_presenter.cpp)，
     * 这里此前漏了。 */
    int32_t rcTimeout = OH_NativeWindow_NativeWindowHandleOpt(window, SET_TIMEOUT, 0);
    OHLOG("SET_TIMEOUT(0) rc=%{public}d", rcTimeout);

    /* M3a-T7 (2026-10-05): 窗口队列 buffer 的格式与用途声明 —— 对齐
     * virgl_surface_presenter.cpp:301-305 的已证配置。缺失后果 (实测):
     * 队列 buffer 按消费侧默认 (CPU 向) usage 分配, DesktopAbility 全屏
     * 窗的合成层只吃 GPU buffer → 画面冻结在首帧 (预览面合成路径不同,
     * 容忍 CPU buffer, 故只有画布冻)。SET_FORMAT(RGBA) 同时决定 slot
     * buffer 内存序 = R,G,B,A (呈现上传不再需要 R/B 对调)。 */
    OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT,
                                          NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
    OH_NativeWindow_NativeWindowHandleOpt(
        window, SET_USAGE,
        (uint64_t)(NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE));

    /* M3a-T7: 画布 EGL swap 呈现面。手工 slot+FlushBuffer 对 DesktopAbility
     * 全屏窗的 surface 消费侧冻结在首帧 (纯产品会话/红背景/A-B 三重实测,
     * 证据链 progress.md 2026-10-05), 呈现改走 eglSwapBuffers —— 同一
     * surface 对 wayland presenter 的 EGL swap 正常 (vd13: 桌面蓝+任务栏)。
     * 建面失败保持返回 true: 旧路径 (零拷贝/拷贝) 原样生效, 行为不变。
     * 仅画布绑定创建 (canvas_egl_present): EGL 面与零拷贝共用水式队列时,
     * EGL 的 dequeue/enqueue 占格, frame 2 起零拷贝 slot_acquire 静默失
     * 败回退拷贝路径 (vd22c 实测), 故画布同时改走默认 swapchain 渲染
     * (见 FrameStep 与 HandleOutputCommit 的 allocator 分支), fusion 预览
     * 保持已证零拷贝路径不变。 */
    if (canvas_egl_present)
        wl_ohos_egl_window_surface_create(window);
    return 0;
}

/* 画布晚到挂载 (D8, 2026-10-05): 链先建 (window=NULL, output/wl_output 已
 * 在场, present 挂起) 后画布到达 —— 在既有 output 上补跑窗口配置, 下一帧
 * 起正常出屏。canvas_egl_present 语义与 chain_start 同名参数一致 (产品画布
 * = true: 该全屏窗对手工 FlushBuffer 冻结, 必须 EGL swap 呈现)。
 * loop 线程调用 (与帧时钟同线程, g_out.window 无需加锁)。
 * 返回 0 = 已挂载; 非 0 = 拒绝 (output 已有窗口 —— surface 重建走 stop/start
 * 全链, 不存在运行中换窗的合法场景; 调用方负责销毁被拒窗口)。 */
int wl_ohos_output_attach_window(OHNativeWindow *window, bool canvas_egl_present)
{
    if (!window || !g_out.output)
        return -1;
    if (g_out.window) {
        OH_LOG_WARN(LOG_APP, "attach window refused: output already bound");
        return -1;
    }
    g_out.window = window;
    /* 挂载时按输出当前尺寸配置 (无窗早启的尺寸来源见 display_compositor
     * 的 deferred 段; 画布逻辑尺寸与之同源同值)。尺寸不一致只告警不重建
     * (output resize 是独立路径, M0 无此场景)。 */
    int rc = WindowPresentSetup(window, canvas_egl_present);
    OHLOG("attach window rc=%{public}d egl=%{public}d out=%{public}dx%{public}d",
          rc, canvas_egl_present, g_out.out_w, g_out.out_h);
    return rc;
}

/* 输出几何动态响应 (D10, 2026-10-05): 折叠/旋转使画布 surface 物理尺寸
 * 变化 (实测 2800x1840 ↔ 1840x2800), 此前链几何启动时钉死 → 旧帧多时期
 * 合成 (花屏) / 输入归一化分母错位 (touch y=2798 > 1840) / 转回后呈现
 * 倍率失效 (内容缩小)。resize = output custom_mode 重提交 + 背景 rect +
 * buffer geometry; 输入归一化 (frame_size) 与呈现 blit 随 g_out 自愈。
 * loop 线程调用。返回 0 = 已应用 (含 no-op)。 */
int wl_ohos_output_resize(int w, int h)
{
    if (!g_out.output || w <= 0 || h <= 0)
        return -1;
    if (w == g_out.out_w && h == g_out.out_h)
        return 0;
    int old_w = g_out.out_w, old_h = g_out.out_h;
    g_out.out_w = w;
    g_out.out_h = h;
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_custom_mode(&st, w, h, 0);
    if (!wlr_output_commit_state(g_out.output, &st))
    {
        OH_LOG_ERROR(LOG_APP, "resize commit failed (%{public}dx%{public}d)", w, h);
        wlr_output_state_finish(&st);
        /* 回滚内存态, 与 output 实际 mode 保持一致 */
        g_out.out_w = old_w;
        g_out.out_h = old_h;
        return -1;
    }
    wlr_output_state_finish(&st);
    if (g_out.bg_rect)
        wlr_scene_rect_set_size(g_out.bg_rect, w, h);
    if (g_out.window)
        OH_NativeWindow_NativeWindowHandleOpt(g_out.window, SET_BUFFER_GEOMETRY,
                                              w, h);
    OHLOG("output resized %{public}dx%{public}d -> %{public}dx%{public}d",
          old_w, old_h, w, h);
    return 0;
}

int wl_ohos_output_chain_start(struct wlr_backend *backend,
                               struct wlr_renderer *renderer,
                               struct wl_event_loop *loop,
                               struct wl_display *display,
                               OHNativeWindow *window,
                               struct wlr_xwayland *xwayland,
                               int out_w, int out_h,
                               bool canvas_egl_present)
{
    memset(&g_out, 0, sizeof(g_out));
    g_out.window = window;
    /* M3a 尺寸参数化: ≤0 = 未指定 (smoke 台架), 回退 800x600 —— 台架证据链
     * (探测器基线/presented-route 判定) 在该尺寸上校准, 不随调用方漂移 */
    g_out.out_w = out_w > 0 ? out_w : 800;
    g_out.out_h = out_h > 0 ? out_h : 600;
    wl_list_init(&g_clients);
    wl_list_init(&g_child_faces);
    g_out.xwayland = xwayland; /* D23: 子窗几何查询句柄 */
    g_desktop_shell_mapped = 0; /* 每轮链路独立判定 (x11 桌面就绪, 见定义处) */

    struct wlr_allocator *alloc = wl_ohos_allocator_create();
    if (!alloc) {
        OH_LOG_ERROR(LOG_APP, "ohos allocator create failed");
        return -1;
    }
    g_out.output = wlr_headless_add_output(backend, g_out.out_w, g_out.out_h);
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
    wlr_output_state_set_custom_mode(&st, g_out.out_w, g_out.out_h, 0);
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
    /* 关闭 direct scan-out (2026-10-05 实测): 场景仅剩一个全覆盖 surface 时
     * wlroots 会绕过 scene 渲染, 把该 surface 的 buffer (XWAYLAND 的 shm
     * buffer, 非我们 allocator 背书) 直接作为 output commit buffer —— 而本
     * 文 HandleOutputCommit 三个呈现分支 (egl-swap-tex / 零拷贝 / CPU 拷贝)
     * 全部只认自家 buffer, CPU 拷贝分支 wl_ohos_buffer_native() 返回 NULL
     * 后静默丢帧 → 画布冻结在 scan-out 生效前的最后一帧 (实测: 探针帧深蓝底
     * + 四角点, 桌面 surface 自身 mapped 且 2 commits/s 却整屏黑)。
     * 触发条件还随输入状态漂移: 软件光标挂在输出上时 scan-out 被抑制
     * (wlr_output_is_direct_scanout_allowed), 光标一离开即复发 —— 必须机制
     * 级关闭, 不做调用点特判。
     * scene->direct_scanout 在 WLR_PRIVATE 匿名成员里不可直赋, 走官方
     * env 开关 (wlr_scene_create 内 env_parse_bool, 仅认字面 "1"); 必须在
     * create 之前 setenv。 */
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);
    g_out.scene = wlr_scene_create();
    if (!g_out.scene) {
        OH_LOG_ERROR(LOG_APP, "scene create failed");
        return -1;
    }
    const float bg[4] = {0.10f, 0.10f, 0.12f, 1.0f};
    g_out.bg_rect = wlr_scene_rect_create(&g_out.scene->tree, g_out.out_w, g_out.out_h, bg);
    if (!g_out.bg_rect) {
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
    // D8: 窗口可晚到 —— 配置段抽成 WindowPresentSetup, 挂载时补跑。
    if (window && WindowPresentSetup(window, canvas_egl_present) != 0)
        return -1;

    g_out.commit_listener.notify = HandleOutputCommit;
    wl_signal_add(&g_out.output->events.commit, &g_out.commit_listener);

    if (xwayland) {
        g_out.xnew_surface.notify = HandleNewSurface;
        wl_signal_add(&xwayland->events.new_surface, &g_out.xnew_surface);
    }

    /* 帧时钟 (任务 2, 见 FrameTick 上方注释): 主驱动 = 系统 VSync, 33ms 定时器
     * 兜底。接入失败/停摆都不静默 —— 降级会把节拍来源打进日志与 rate 行。 */
    g_out.vsync_fd = -1;
    g_out.frame_timer = wl_event_loop_add_timer(loop, FrameTick, g_out.output);
    {
        static char vsyncName[] = "WineHuaDisplay";
        g_out.vsync = OH_NativeVSync_Create(vsyncName, sizeof(vsyncName) - 1);
        if (g_out.vsync) {
            OH_NativeVSync_ExpectedRateRange range = {60, 120, 120};
            int rr = OH_NativeVSync_SetExpectedFrameRateRange(g_out.vsync, &range);
            g_out.vsync_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (g_out.vsync_fd >= 0)
                g_out.vsync_source = wl_event_loop_add_fd(loop, g_out.vsync_fd,
                                                          WL_EVENT_READABLE,
                                                          HandleVSyncWake, NULL);
            if (!g_out.vsync_source) {
                if (g_out.vsync_fd >= 0) {
                    close(g_out.vsync_fd);
                    g_out.vsync_fd = -1;
                }
                OH_NativeVSync_Destroy(g_out.vsync);
                g_out.vsync = NULL;
                OH_LOG_ERROR(LOG_APP, "帧时钟: VSync 接入失败 (eventfd/add_fd) ⇒ 33ms 兜底");
            } else {
                g_out.vsync_stalled = true; /* 首次续订到回调之间按停摆算 */
                VsyncRearm();
                OHLOG("帧时钟: VSync 主驱动已接入 (min=%{public}d max=%{public}d "
                      "expected=%{public}d setRangeRc=%{public}d fd=%{public}d)",
                      range.min, range.max, range.expected, rr, g_out.vsync_fd);
            }
        } else {
            OH_LOG_ERROR(LOG_APP, "帧时钟: OH_NativeVSync_Create 失败 ⇒ 33ms 兜底");
        }
    }
    if (g_out.frame_timer)
        wl_event_source_timer_update(g_out.frame_timer,
                                     g_out.vsync ? VSYNC_WATCHDOG_MS : 100);
    OHLOG("output chain up: %{public}dx%{public}d, 帧时钟=%{public}s",
          g_out.out_w, g_out.out_h,
          g_out.vsync ? "VSync(期望 60-120Hz)" : "33ms 兜底");
    return 0;
}

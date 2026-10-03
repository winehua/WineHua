/*
 * ohos_buffer.cpp — OH_NativeBuffer 背书的 wlr_buffer / wlr_allocator (M0-T8)
 *
 * 链路: wlr_output_init_render(output, 本 allocator, pixman renderer) →
 * commit 时 allocator.create_buffer 分配 OH_NativeBuffer → pixman renderer
 * 经 begin_data_ptr_access 拿虚地址直接绘制 → commit 事件回调里取
 * state->buffer 推给 NativeWindow。
 *
 * SDK 实测要点 (API 签名以 sysroot native_buffer.h 为准):
 * - OH_NativeBuffer_Alloc(config): format 用 NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
 *   usage = CPU_READ | CPU_WRITE | CPU_READ_OFTEN (含 CPU 读写位, 首验通过)
 * - OH_NativeBuffer_Map(buffer, &virAddr) 拿 CPU 虚地址, stride 由系统定,
 *   经 OH_NativeBuffer_GetConfig 回读
 * - get_dmabuf 返回失败: 合法 (DATA_PTR caps), pixman 不走 dmabuf 门
 */
#include "ohos_buffer.h"
#include "ohos_egl_import.h" /* wl_ohos_egl_sync_count (不变量检查器) */
#include "ohos_output.h"     /* wl_ohos_output_present_seq (不变量检查器) */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <sys/mman.h>
#include <unistd.h>

#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
/* M2-T5 消费者: OH_NativeImage_Acquire/ReleaseNativeWindowBuffer */
#include <native_image/native_image.h>
#include <libdrm/drm_fourcc.h>

#define WLR_USE_UNSTABLE
extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/util/log.h>
}

namespace {

// native_buffer.h 的 usage 位与 SDK 头一致; 如后续真机返回码变化, 先查这里。
// HW_RENDER/HW_TEXTURE (M2-T4): gles2 渲染器把 buffer 当 FBO 渲染目标 + 纹理
// 采样 (T2 探针用同一组合真机通过: CPU 读写 + HW 位可共存, Map 仍可读)。
constexpr uint64_t kOhosBufferUsage = NATIVEBUFFER_USAGE_CPU_READ |
                                      NATIVEBUFFER_USAGE_CPU_WRITE |
                                      NATIVEBUFFER_USAGE_CPU_READ_OFTEN |
                                      NATIVEBUFFER_USAGE_HW_RENDER |
                                      NATIVEBUFFER_USAGE_HW_TEXTURE;

struct WlOhosBuffer {
    struct wlr_buffer base;
    OH_NativeBuffer *native = nullptr;
    /* EGL 导入载荷包装 (惰性创建, 随 buffer 销毁; 见 wl_ohos_buffer_window_buffer) */
    OHNativeWindowBuffer *window_buffer = nullptr;
    void *mapped = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    int map_depth = 0;
};

struct WlOhosBuffer *BufferFromBase(struct wlr_buffer *base)
{
    return reinterpret_cast<WlOhosBuffer *>(base);
}

void BufferDestroy(struct wlr_buffer *wlr_buf)
{
    WlOhosBuffer *buf = BufferFromBase(wlr_buf);
    /* 顺序要紧: 包装先于底层 buffer 释放 (包装持 OH_NativeBuffer 引用);
     * 调用方 (gles2 buffer addon) 已在本步之前销毁 EGLImage。 */
    if (buf->window_buffer)
    {
        OH_NativeWindow_DestroyNativeWindowBuffer(buf->window_buffer);
        buf->window_buffer = nullptr;
    }
    if (buf->native)
    {
        OH_NativeBuffer_Unreference(buf->native);
        buf->native = nullptr;
    }
    free(buf);
}

bool BufferGetDmabuf(struct wlr_buffer *, struct wlr_dmabuf_attributes *)
{
    // CPU 内存 buffer 无 dmabuf 导出; pixman renderer 走 data_ptr 门
    return false;
}

bool BufferBeginDataPtr(struct wlr_buffer *wlr_buf, uint32_t flags,
                        void **data, uint32_t *format, size_t *stride)
{
    auto *buf = BufferFromBase(wlr_buf);
    if (buf->map_depth == 0)
    {
        void *vir = nullptr;
        if (OH_NativeBuffer_Map(buf->native, &vir) != 0 || vir == nullptr)
            return false;
        buf->mapped = vir;
    }
    ++buf->map_depth;
    (void)flags;
    *data = buf->mapped;
    *format = buf->format;
    *stride = buf->stride;
    return true;
}

void BufferEndDataPtr(struct wlr_buffer *wlr_buf)
{
    auto *buf = BufferFromBase(wlr_buf);
    if (--buf->map_depth == 0)
    {
        OH_NativeBuffer_Unmap(buf->native);
        buf->mapped = nullptr;
    }
}

const struct wlr_buffer_impl kBufferImpl = {
    .destroy = BufferDestroy,
    .get_dmabuf = BufferGetDmabuf,
    .get_shm = nullptr,
    .begin_data_ptr_access = BufferBeginDataPtr,
    .end_data_ptr_access = BufferEndDataPtr,
};

struct wlr_buffer *AllocatorCreateBuffer(struct wlr_allocator *alloc,
                                         int width, int height,
                                         const struct wlr_drm_format *drm_format)
{
    (void)alloc;
    if (!drm_format)
        return nullptr;
    // DRM 四字码按小端字节序命名, OHOS 像素格式按字节序命名, 二者一一对应:
    //   ABGR8888 (内存 R,G,B,A) == NATIVEBUFFER_PIXEL_FMT_RGBA_8888
    //   ARGB8888 (内存 B,G,R,A) == NATIVEBUFFER_PIXEL_FMT_BGRA_8888
    // XRGB/BGRX 复用同布局 (X 字节不读)。headless 的 primary formats 是
    // ARGB/XRGB 族, 只收 ABGR 会导致 output enable commit 失败 (T8 实测)。
    int ohos_format;
    switch (drm_format->format)
    {
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
        ohos_format = NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
        break;
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        ohos_format = NATIVEBUFFER_PIXEL_FMT_BGRA_8888;
        break;
    default:
        wlr_log(WLR_ERROR, "ohos allocator: unsupported drm format 0x%08x",
                drm_format->format);
        return nullptr;
    }

    OH_NativeBuffer_Config cfg = {};
    cfg.width = width;
    cfg.height = height;
    cfg.format = ohos_format;
    cfg.usage = kOhosBufferUsage;

    OH_NativeBuffer *native = OH_NativeBuffer_Alloc(&cfg);
    if (!native)
    {
        wlr_log(WLR_ERROR, "OH_NativeBuffer_Alloc %dx%d failed", width, height);
        return nullptr;
    }

    OH_NativeBuffer_Config got = {};
    OH_NativeBuffer_GetConfig(native, &got);
    size_t stride = static_cast<size_t>(got.stride > 0 ? got.stride
                                                       : width * 4);

    auto *buf = static_cast<WlOhosBuffer *>(calloc(1, sizeof(WlOhosBuffer)));
    if (!buf)
    {
        OH_NativeBuffer_Unreference(native);
        return nullptr;
    }
    buf->native = native;
    buf->stride = stride;
    buf->format = drm_format->format;
    wlr_buffer_init(&buf->base, &kBufferImpl, width, height);
    wlr_log(WLR_INFO, "ohos buffer %dx%d stride=%zu seq=%u",
            width, height, stride, OH_NativeBuffer_GetSeqNum(native));
    return &buf->base;
}

void AllocatorDestroy(struct wlr_allocator *alloc)
{
    free(alloc);
}

const struct wlr_allocator_interface kAllocatorImpl = {
    .create_buffer = AllocatorCreateBuffer,
    .destroy = AllocatorDestroy,
};

// 目前无自有字段; 独立类型留名给后续 swapchain 记账 (帧直推需要知道
// 当前活跃 buffer 归属)
struct WlOhosAllocator {
    struct wlr_allocator base;
};

/* ── present buffer (window 队列借来的一格, 见 ohos_buffer.h) ── */
struct WlOhosPresentBuffer {
    struct wlr_buffer base;
    OHNativeWindow *window = nullptr;
    OHNativeWindowBuffer *window_buffer = nullptr;
    bool returned = false; /* 已 Flush/Abort 归还队列 */
    void *mapped = nullptr;
    size_t mapped_bytes = 0;
    uint32_t format = 0;
    size_t stride = 0;
};

struct WlOhosPresentBuffer *PresentFromBase(struct wlr_buffer *base)
{
    return reinterpret_cast<WlOhosPresentBuffer *>(base);
}

// 队列 buffer 的 OHOS 像素格式 → DRM 四字码 (与 allocator 侧同一套对应:
// 内存字节序命名 vs 四字码)。未知格式记 0: 导入不依赖它, 只有 alpha 判定用。
uint32_t OhosFormatToDrm(int32_t ohos_format)
{
    switch (ohos_format)
    {
    case NATIVEBUFFER_PIXEL_FMT_RGBA_8888:
        return DRM_FORMAT_ABGR8888;
    case NATIVEBUFFER_PIXEL_FMT_BGRA_8888:
        return DRM_FORMAT_ARGB8888;
    default:
        return 0;
    }
}

void PresentUnmap(WlOhosPresentBuffer *buf)
{
    if (buf->mapped)
    {
        munmap(buf->mapped, buf->mapped_bytes);
        buf->mapped = nullptr;
        buf->mapped_bytes = 0;
    }
}

void PresentDestroy(struct wlr_buffer *wlr_buf)
{
    WlOhosPresentBuffer *buf = PresentFromBase(wlr_buf);
    PresentUnmap(buf);
    /* 未归还就销毁 = 调用方漏了 present/abort, 槽位会永久留在 dequeued
     * (泄漏几次 RequestBuffer 即饿死): 兜底归还并留证。 */
    if (!buf->returned)
    {
        int32_t rc = OH_NativeWindow_NativeWindowAbortBuffer(
            buf->window, buf->window_buffer);
        wlr_log(WLR_ERROR, "ohos present buffer dropped without return, "
                "aborted (rc=%d)", rc);
    }
    free(buf);
}

bool PresentGetDmabuf(struct wlr_buffer *, struct wlr_dmabuf_attributes *)
{
    return false; /* 队列 buffer 无 dmabuf 导出, 走 EGL_NATIVE_BUFFER_OHOS 导入 */
}

bool PresentBeginDataPtr(struct wlr_buffer *wlr_buf, uint32_t flags,
                         void **data, uint32_t *format, size_t *stride)
{
    (void)flags;
    auto *buf = PresentFromBase(wlr_buf);
    if (!buf->mapped)
    {
        BufferHandle *h =
            OH_NativeWindow_GetBufferHandleFromNative(buf->window_buffer);
        if (!h || h->fd < 0)
            return false;
        size_t bytes = (h->size > 0) ? static_cast<size_t>(h->size)
                                     : static_cast<size_t>(h->stride) * h->height;
        void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                       h->fd, 0);
        if (p == MAP_FAILED)
            return false;
        buf->mapped = p;
        buf->mapped_bytes = bytes;
    }
    *data = buf->mapped;
    *format = buf->format;
    *stride = buf->stride;
    return true;
}

void PresentEndDataPtr(struct wlr_buffer *wlr_buf)
{
    /* 映射保留到归还/销毁: 每帧重复 mmap 不划算, 且 present 路径要按
     * BufferHandle 行距读回校验。 */
    (void)wlr_buf;
}

const struct wlr_buffer_impl kPresentBufferImpl = {
    .destroy = PresentDestroy,
    .get_dmabuf = PresentGetDmabuf,
    .get_shm = nullptr,
    .begin_data_ptr_access = PresentBeginDataPtr,
    .end_data_ptr_access = PresentEndDataPtr,
};

/* ── guest frame buffer (OH_NativeImage 消费者队列借来的一格, M2-T5) ──
 *
 * 生产者是宿主 virgl/vtest 侧按 X window id 路由进来的 guest Vulkan 帧
 * (win32u 送出的 SURFACE_ID = xwindow)。消费者侧与 present 侧同构: 一格一
 * 借、一帧一还, 未归还就销毁 = 队列槽位泄漏 (兜底归还并留证)。导入沿用
 * T4 的 EGL_NATIVE_BUFFER_OHOS 路径, 因此不提供 data_ptr 访问。
 */
struct WlOhosConsumerBuffer {
    struct wlr_buffer base;
    OH_NativeImage *image = nullptr;
    OHNativeWindowBuffer *window_buffer = nullptr;
    bool returned = false; /* 已 ReleaseNativeWindowBuffer 归还队列 */
    /* 不变量检查器 (见 ohos_buffer.h): 交给 scene 那一刻的 present 序号与
     * GPU 同步点计数; 归还时比对 —— handoff_valid 为假表示这格从没交给
     * scene (未上屏即丢弃), 不参与判定。 */
    bool handoff_valid = false;
    uint32_t handoff_present_seq = 0;
    uint64_t handoff_sync_count = 0;
    uint32_t format = 0;
    size_t stride = 0;
};

/* 归还侧统计 (每秒随 [GUEST-FRAMES] stats 行输出) */
uint64_t g_relTotal, g_relSynced, g_relViolations, g_relUnrendered;

struct WlOhosConsumerBuffer *ConsumerFromBase(struct wlr_buffer *base)
{
    return reinterpret_cast<WlOhosConsumerBuffer *>(base);
}

/* 队列归还 + 自持引用释放。Acquire 出来的 buffer 由本模块持一次引用
 * (native_image.h:281 契约: 用毕须 NativeObjectUnreference), 归还队列本身
 * 不退引用 —— 引用退在销毁时, 保证归还后到销毁前对象不会被队列回收。 */
void ConsumerReleaseQueueSlot(WlOhosConsumerBuffer *buf, int fence_fd, bool logDrop)
{
    if (buf->returned)
        return;
    /* 不变量检查 (known-issues §2.6): 这格交出去之后, 有没有发生过
     *   presented = 上过屏 (scene 渲染采样过它) —— 用 present 序号比对;
     *   synced    = 发生过 GPU 同步 (glFinish 自增的计数动了)。
     * 上屏过却没有同步点 = 安全性依赖的那条不变量破了 (有人拿掉了 present
     * 路径的 glFinish, 或改了归还时机), 必须叫。两者都没有 = 这格没被采样
     * 过就归还, 合法。 */
    if (buf->handoff_valid) {
        const uint32_t now_seq = wl_ohos_output_present_seq();
        const uint64_t now_sync = wl_ohos_egl_sync_count();
        const bool presented = now_seq != buf->handoff_present_seq;
        const bool synced = now_sync != buf->handoff_sync_count;
        ++g_relTotal;
        if (synced)
            ++g_relSynced;
        else if (presented) {
            ++g_relViolations;
            if (g_relViolations <= 5u)
                wlr_log(WLR_ERROR,
                        "guest frame 不变量破损: 已上屏但交付后无 GPU 同步 "
                        "(present %u→%u sync %llu→%llu) —— present 路径的 "
                        "glFinish 是否还在?",
                        buf->handoff_present_seq, now_seq,
                        (unsigned long long)buf->handoff_sync_count,
                        (unsigned long long)now_sync);
        } else {
            ++g_relUnrendered;
        }
    }
    if (logDrop)
        wlr_log(WLR_ERROR, "ohos guest frame dropped without release, releasing");
    if (buf->image && buf->window_buffer)
        OH_NativeImage_ReleaseNativeWindowBuffer(buf->image, buf->window_buffer,
                                                 fence_fd);
    buf->returned = true;
}

void ConsumerDestroy(struct wlr_buffer *wlr_buf)
{
    WlOhosConsumerBuffer *buf = ConsumerFromBase(wlr_buf);
    ConsumerReleaseQueueSlot(buf, -1, true);
    if (buf->window_buffer)
        OH_NativeWindow_NativeObjectUnreference(buf->window_buffer);
    free(buf);
}

bool ConsumerGetDmabuf(struct wlr_buffer *, struct wlr_dmabuf_attributes *)
{
    return false; /* 队列 buffer 无 dmabuf 导出, 走 EGL_NATIVE_BUFFER_OHOS 导入 */
}

const struct wlr_buffer_impl kConsumerBufferImpl = {
    .destroy = ConsumerDestroy,
    .get_dmabuf = ConsumerGetDmabuf,
    .get_shm = nullptr,
    .begin_data_ptr_access = nullptr,
    .end_data_ptr_access = nullptr,
};

} // namespace

extern "C" struct wlr_allocator *wl_ohos_allocator_create(void)
{
    auto *alloc = static_cast<WlOhosAllocator *>(calloc(1, sizeof(WlOhosAllocator)));
    if (!alloc)
        return nullptr;
    // 注: WlOhosAllocator 目前无自有字段, 结构体留名给后续 swapchain 记账;
    // base 首成员布局由 wlr_allocator_init 初始化
    wlr_allocator_init(&alloc->base, &kAllocatorImpl, WLR_BUFFER_CAP_DATA_PTR);
    return &alloc->base;
}

extern "C" struct OH_NativeBuffer *wl_ohos_buffer_native(struct wlr_buffer *buffer)
{
    if (!buffer || buffer->impl != &kBufferImpl)
        return nullptr;
    return BufferFromBase(buffer)->native;
}

extern "C" size_t wl_ohos_buffer_stride(struct wlr_buffer *buffer)
{
    if (!buffer)
        return 0;
    if (buffer->impl == &kPresentBufferImpl)
        return PresentFromBase(buffer)->stride;
    if (buffer->impl == &kConsumerBufferImpl)
        return ConsumerFromBase(buffer)->stride;
    if (buffer->impl != &kBufferImpl)
        return 0;
    return BufferFromBase(buffer)->stride;
}

extern "C" uint32_t wl_ohos_buffer_drm_format(struct wlr_buffer *buffer)
{
    if (!buffer)
        return 0;
    if (buffer->impl == &kPresentBufferImpl)
        return PresentFromBase(buffer)->format;
    if (buffer->impl == &kConsumerBufferImpl)
        return ConsumerFromBase(buffer)->format;
    if (buffer->impl != &kBufferImpl)
        return 0;
    return BufferFromBase(buffer)->format;
}

/* ── present slot (T6.5): 队列句柄 → 持久 (wrapper + allocator + swapchain) ──
 *
 * 一次性 swapchain 设计的每帧分配风暴在桌面尺寸下压爆内核图形侧 (证据与
 * 对照组见 ohos_buffer.h 头注释)。三元组按 RequestBuffer 返回的句柄缓存,
 * 每帧只做队列契约要求的 Request/Flush; "Allocating new swapchain buffer"
 * 从每帧一次降到每槽位生命周期一次。
 *
 * wrapper 生命周期 (两持有者, 释放顺序无关 —— buffer_consider_destroy 要求
 * dropped 且 n_locks==0 才真正销毁):
 *   swapchain slot: 持唯一 drop (OneShotCreateBuffer 所有权移交, slot_reset
 *     在 swapchain 销毁时 drop 恰好一次 —— 与 T6.5 前同一条语义);
 *   slot 表: 持 1 引用 (wlr_buffer_lock), shutdown 时退。
 * 队列深度上界 = 表容量: 队列循环复用固定几格, 同句柄不会并发两个 wrapper
 * (那会让两个 swapchain 指向同一块存储, 是数据竞争)。 */
#define PRESENT_SLOT_CAP 4

struct PresentSlot
{
    OHNativeWindowBuffer *wb;
    WlOhosPresentBuffer *buf;
    struct wlr_allocator *alloc;
    struct wlr_swapchain *swapchain;
};

static PresentSlot g_present_slots[PRESENT_SLOT_CAP];
static size_t g_present_slot_count;

/* 一次性 allocator: wlr_swapchain_create 不立即向 allocator 取 buffer
 * (render/swapchain.c:104 惰性分配在首个 slot_acquire), 首次 acquire 时把
 * wrapper 移交给 swapchain slot。容量 1 的 "allocator" 是把队列 buffer 塞进
 * swapchain 记账模型的接口适配, 不是真的分配器。 */
struct PresentOneShotAllocator
{
    struct wlr_allocator base;
    struct wlr_buffer *buffer;
    int taken;
};

static struct wlr_buffer *OneShotCreateBuffer(struct wlr_allocator *alloc,
                                              int width, int height,
                                              const struct wlr_drm_format *format)
{
    struct PresentOneShotAllocator *one = (struct PresentOneShotAllocator *)alloc;
    (void)width;
    (void)height;
    (void)format; /* 存储已由队列 buffer 定 */
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

extern "C" struct wlr_buffer *wl_ohos_present_slot_acquire(struct NativeWindow *window,
                                                           struct wlr_swapchain **out_swapchain)
{
    if (out_swapchain)
        *out_swapchain = nullptr;
    if (!window)
        return nullptr;
    OHNativeWindowBuffer *wb = nullptr;
    int32_t fence = -1;
    int32_t rc = OH_NativeWindow_NativeWindowRequestBuffer(window, &wb, &fence);
    if (rc != 0 || !wb)
    {
        /* 队列空 (消费者未释放) 是正常背压, 调用方按丢帧处理 */
        if (fence >= 0)
            close(fence);
        wlr_log(WLR_DEBUG, "ohos present slot: RequestBuffer rc=%d", rc);
        return nullptr;
    }

    /* 命中: 同一队列槽位回来了 —— 复用三元组。returned 按新一次 Request
     * 周期重置; swapchain 的槽位上一帧已随 commit 结束释放 (scene 的
     * output state finish 会 unlock), 本帧可再次 acquire。 */
    for (size_t i = 0; i < g_present_slot_count; ++i)
    {
        if (g_present_slots[i].wb != wb)
            continue;
        /* LRU 触底: 命中项挪表尾 (逐出固定打表头) */
        PresentSlot hit = g_present_slots[i];
        for (size_t j = i; j + 1 < g_present_slot_count; ++j)
            g_present_slots[j] = g_present_slots[j + 1];
        g_present_slots[g_present_slot_count - 1] = hit;
        hit.buf->returned = false;
        if (fence >= 0)
            close(fence); /* Request 的 fence 只对「写前等消费者」有意义, 本路径
                           * 不等待 (与既有拷贝路径同语义, 真机验证无撕裂) */
        if (out_swapchain)
            *out_swapchain = hit.swapchain;
        return &hit.buf->base;
    }

    /* miss: 新建三元组 */
    BufferHandle *h = OH_NativeWindow_GetBufferHandleFromNative(wb);
    if (!h || h->width <= 0 || h->height <= 0 || h->stride <= 0)
    {
        wlr_log(WLR_ERROR, "ohos present slot: bad handle %p", (void *)h);
        OH_NativeWindow_NativeWindowAbortBuffer(window, wb);
        if (fence >= 0)
            close(fence);
        return nullptr;
    }
    auto *buf = static_cast<WlOhosPresentBuffer *>(calloc(1, sizeof(WlOhosPresentBuffer)));
    if (!buf)
    {
        OH_NativeWindow_NativeWindowAbortBuffer(window, wb);
        if (fence >= 0)
            close(fence);
        return nullptr;
    }
    buf->window = window;
    buf->window_buffer = wb;
    buf->stride = static_cast<size_t>(h->stride);
    buf->format = OhosFormatToDrm(h->format);
    wlr_buffer_init(&buf->base, &kPresentBufferImpl, h->width, h->height);
    wlr_buffer_lock(&buf->base); /* slot 表的生命周期引用 (见上方生命周期注释) */
    if (fence >= 0)
        close(fence);
    wlr_log(WLR_DEBUG, "ohos present slot new %dx%d stride=%zu fmt=0x%x drm=0x%x",
            h->width, h->height, buf->stride, h->format, buf->format);

    auto *one = (PresentOneShotAllocator *)calloc(1, sizeof(PresentOneShotAllocator));
    if (!one)
    {
        wlr_buffer_drop(&buf->base);   /* 无人接手: 唯一 drop 归我们 */
        wlr_buffer_unlock(&buf->base); /* 退表引用 → 真正销毁 */
        return nullptr;
    }
    one->buffer = &buf->base;
    wlr_allocator_init(&one->base, &kOneShotAllocatorImpl, WLR_BUFFER_CAP_DATA_PTR);

    /* 格式只作 swapchain 记账 (存储由队列 buffer 定); 认不出时用 XRGB8888
     * 兜底 (与输出 primary format 同族) */
    struct wlr_drm_format format = {0};
    format.format = buf->format ? buf->format : DRM_FORMAT_XRGB8888;
    struct wlr_swapchain *swapchain =
        wlr_swapchain_create(&one->base, h->width, h->height, &format);
    if (!swapchain)
    {
        /* swapchain 建失败发生在向 allocator 取 buffer 之前 (惰性), wrapper
         * 未被接手 —— 唯一 drop 与表引用都在我们。 */
        wlr_buffer_drop(&buf->base);
        wlr_buffer_unlock(&buf->base);
        wlr_allocator_destroy(&one->base);
        return nullptr;
    }

    if (g_present_slot_count == PRESENT_SLOT_CAP)
    {
        /* 表满 = 队列深度超出预期 (正常 ≤3)。被逐三元组的 wrapper 若还在
         * swapchain 槽位上, drop 已随之发生; 表引用是最后一计数, 这里按
         * swapchain → allocator → 引用的顺序拆, 与 shutdown 同构。 */
        PresentSlot *evict = &g_present_slots[0];
        wlr_log(WLR_INFO, "ohos present slot table full (%d), evicting oldest",
                PRESENT_SLOT_CAP);
        wlr_swapchain_destroy(evict->swapchain);
        wlr_allocator_destroy(evict->alloc);
        wlr_buffer_unlock(&evict->buf->base);
        for (size_t j = 0; j + 1 < PRESENT_SLOT_CAP; ++j)
            g_present_slots[j] = g_present_slots[j + 1];
        g_present_slot_count--;
    }
    g_present_slots[g_present_slot_count++] = {wb, buf, &one->base, swapchain};
    if (out_swapchain)
        *out_swapchain = swapchain;
    return &buf->base;
}

extern "C" void wl_ohos_present_slots_shutdown(void)
{
    for (size_t i = 0; i < g_present_slot_count; ++i)
    {
        PresentSlot *slot = &g_present_slots[i];
        /* 顺序: swapchain 先 (slot_reset 对 wrapper 唯一一次 drop), allocator
         * 随后 (swapchain 挂着它的 destroy 监听), 表引用最后退 —— 三步之后
         * dropped && n_locks==0, wrapper 真正销毁 (munmap + 归还兜底)。 */
        wlr_swapchain_destroy(slot->swapchain);
        wlr_allocator_destroy(slot->alloc);
        wlr_buffer_unlock(&slot->buf->base);
    }
    g_present_slot_count = 0;
}

extern "C" int wl_ohos_present_buffer_owns(struct wlr_buffer *buffer)
{
    return buffer && buffer->impl == &kPresentBufferImpl;
}

extern "C" int32_t wl_ohos_present_buffer_present(struct wlr_buffer *buffer, int fence_fd)
{
    if (!buffer || buffer->impl != &kPresentBufferImpl)
        return -1;
    WlOhosPresentBuffer *buf = PresentFromBase(buffer);
    if (buf->returned)
        return -1;
    /* Region::Rect 是嵌套类型 (C++ 作用域, C 里才可裸写 struct Rect) */
    Region::Rect rect = {};
    rect.w = static_cast<uint32_t>(buffer->width);
    rect.h = static_cast<uint32_t>(buffer->height);
    Region region = {};
    region.rects = &rect;
    region.rectNumber = 1;
    buf->returned = true;
    return OH_NativeWindow_NativeWindowFlushBuffer(buf->window, buf->window_buffer,
                                                   fence_fd, region);
}

extern "C" int32_t wl_ohos_present_buffer_abort(struct wlr_buffer *buffer)
{
    if (!buffer || buffer->impl != &kPresentBufferImpl)
        return -1;
    WlOhosPresentBuffer *buf = PresentFromBase(buffer);
    if (buf->returned)
        return -1;
    buf->returned = true;
    return OH_NativeWindow_NativeWindowAbortBuffer(buf->window, buf->window_buffer);
}

extern "C" int wl_ohos_present_buffer_returned(struct wlr_buffer *buffer)
{
    if (!buffer || buffer->impl != &kPresentBufferImpl)
        return 0;
    return PresentFromBase(buffer)->returned ? 1 : 0;
}

extern "C" struct NativeWindowBuffer *wl_ohos_present_buffer_window_buffer(struct wlr_buffer *buffer)
{
    if (!buffer || buffer->impl != &kPresentBufferImpl)
        return nullptr;
    return PresentFromBase(buffer)->window_buffer;
}

extern "C" struct wlr_buffer *wl_ohos_consumer_buffer_acquire(struct OH_NativeImage *image)
{
    if (!image)
        return nullptr;
    OHNativeWindowBuffer *wb = nullptr;
    int32_t fence = -1;
    int32_t rc = OH_NativeImage_AcquireNativeWindowBuffer(image, &wb, &fence);
    if (rc != 0 || !wb)
    {
        /* 没有新帧是正常状态 (生产者没提交), 调用方按"本轮无帧"处理 */
        if (fence >= 0)
            close(fence);
        return nullptr;
    }
    /* 同步 fence 不往下传 (EGL_NATIVE_BUFFER_OHOS 导入由系统图形栈在首次使用
     * 时消费 buffer 自带 fence, T4 输出侧同口径), 但必须关掉 —— SDK 契约。 */
    if (fence >= 0)
        close(fence);
    /* SDK 契约 (native_image.h:281): Acquire 出的 OHNativeWindowBuffer 要自持
     * 一次引用, 用毕 NativeObjectUnreference (退还点在 ConsumerDestroy)。 */
    if (OH_NativeWindow_NativeObjectReference(wb) != 0)
    {
        OH_NativeImage_ReleaseNativeWindowBuffer(image, wb, -1);
        return nullptr;
    }
    BufferHandle *h = OH_NativeWindow_GetBufferHandleFromNative(wb);
    if (!h || h->width <= 0 || h->height <= 0 || h->stride <= 0)
    {
        OH_NativeImage_ReleaseNativeWindowBuffer(image, wb, -1);
        OH_NativeWindow_NativeObjectUnreference(wb);
        return nullptr;
    }
    auto *buf = static_cast<WlOhosConsumerBuffer *>(calloc(1, sizeof(WlOhosConsumerBuffer)));
    if (!buf)
    {
        OH_NativeImage_ReleaseNativeWindowBuffer(image, wb, -1);
        OH_NativeWindow_NativeObjectUnreference(wb);
        return nullptr;
    }
    buf->image = image;
    buf->window_buffer = wb;
    buf->stride = static_cast<size_t>(h->stride);
    buf->format = OhosFormatToDrm(h->format);
    wlr_buffer_init(&buf->base, &kConsumerBufferImpl,
                    static_cast<int>(h->width), static_cast<int>(h->height));
    return &buf->base;
}

extern "C" int wl_ohos_consumer_buffer_owns(struct wlr_buffer *buffer)
{
    return buffer && buffer->impl == &kConsumerBufferImpl;
}

extern "C" int32_t wl_ohos_consumer_buffer_release(struct wlr_buffer *buffer, int fence_fd)
{
    if (!buffer || buffer->impl != &kConsumerBufferImpl)
        return -1;
    WlOhosConsumerBuffer *buf = ConsumerFromBase(buffer);
    if (buf->returned)
        return -1;
    const int32_t rc = OH_NativeImage_ReleaseNativeWindowBuffer(
        buf->image, buf->window_buffer, fence_fd);
    buf->returned = true;
    return rc;
}

extern "C" void wl_ohos_consumer_buffer_drop(struct wlr_buffer *buffer)
{
    /* 未挂上 scene 的帧必须显式丢弃: 走 wlr_buffer 析构 → ConsumerDestroy
     * 归还槽位 (不能用 release —— 那只归还队列, buffer 对象仍占用一次引用,
     * 调用方手上就没有再释放它的机会了) */
    if (buffer)
        wlr_buffer_drop(buffer);
}

extern "C" void wl_ohos_consumer_buffer_note_handoff(struct wlr_buffer *buffer,
                                                     uint32_t present_seq,
                                                     uint64_t sync_count)
{
    if (!buffer || !wl_ohos_consumer_buffer_owns(buffer))
        return;
    WlOhosConsumerBuffer *buf = ConsumerFromBase(buffer);
    buf->handoff_valid = true;
    buf->handoff_present_seq = present_seq;
    buf->handoff_sync_count = sync_count;
}

extern "C" void wl_ohos_consumer_handoff_stats(uint64_t *total, uint64_t *synced,
                                               uint64_t *violations,
                                               uint64_t *unrendered)
{
    if (total) *total = g_relTotal;
    if (synced) *synced = g_relSynced;
    if (violations) *violations = g_relViolations;
    if (unrendered) *unrendered = g_relUnrendered;
}

extern "C" struct NativeWindowBuffer *wl_ohos_consumer_buffer_window_buffer(struct wlr_buffer *buffer)
{
    if (!buffer || buffer->impl != &kConsumerBufferImpl)
        return nullptr;
    return ConsumerFromBase(buffer)->window_buffer;
}

extern "C" struct NativeWindowBuffer *wl_ohos_buffer_window_buffer(struct wlr_buffer *buffer)
{
    if (!buffer)
        return nullptr;
    /* 一种判据一处实现: 导入器只认这一个入口, 三类 buffer (自有 / 输出队列
     * 借来 / guest 帧队列借来) 的载荷在内部区分 */
    if (buffer->impl == &kPresentBufferImpl)
        return PresentFromBase(buffer)->window_buffer;
    if (buffer->impl == &kConsumerBufferImpl)
        return ConsumerFromBase(buffer)->window_buffer;
    if (buffer->impl != &kBufferImpl)
        return nullptr;
    WlOhosBuffer *buf = BufferFromBase(buffer);
    if (buf->window_buffer)
        return buf->window_buffer;
    /* CreateNativeWindowBufferFromNativeBuffer 只包一层句柄 (不搬数据),
     * 创建一次缓存到 buffer 生命周期 —— 每帧重建会让 EGLImage 的底层对象
     * 提前换血 (探针里包装活到图像销毁之后才释放)。 */
    buf->window_buffer =
        OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(buf->native);
    if (!buf->window_buffer)
        wlr_log(WLR_ERROR, "ohos buffer: CreateNativeWindowBufferFromNativeBuffer failed");
    return buf->window_buffer;
}

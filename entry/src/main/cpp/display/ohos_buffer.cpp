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

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <sys/mman.h>
#include <unistd.h>

#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <libdrm/drm_fourcc.h>

#define WLR_USE_UNSTABLE
extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
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
    if (buffer->impl != &kBufferImpl)
        return 0;
    return BufferFromBase(buffer)->format;
}

extern "C" struct wlr_buffer *wl_ohos_present_buffer_acquire(struct NativeWindow *window)
{
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
        wlr_log(WLR_DEBUG, "ohos present buffer: RequestBuffer rc=%d", rc);
        return nullptr;
    }
    BufferHandle *h = OH_NativeWindow_GetBufferHandleFromNative(wb);
    if (!h || h->width <= 0 || h->height <= 0 || h->stride <= 0)
    {
        wlr_log(WLR_ERROR, "ohos present buffer: bad handle %p", (void *)h);
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
    if (fence >= 0)
        close(fence); /* Request 的 fence 只对「写前等消费者」有意义, 本路径
                       * 不等待 (与既有拷贝路径同语义, 真机验证无撕裂) */
    wlr_log(WLR_DEBUG, "ohos present buffer %dx%d stride=%zu fmt=0x%x drm=0x%x",
            h->width, h->height, buf->stride, h->format, buf->format);
    return &buf->base;
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

extern "C" struct NativeWindowBuffer *wl_ohos_buffer_window_buffer(struct wlr_buffer *buffer)
{
    if (!buffer)
        return nullptr;
    /* 一种判据一处实现: 导入器只认这一个入口, 两类 buffer (自有 / 队列借来)
     * 的载荷在内部区分 */
    if (buffer->impl == &kPresentBufferImpl)
        return PresentFromBase(buffer)->window_buffer;
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

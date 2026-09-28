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

#include <native_buffer/native_buffer.h>
#include <libdrm/drm_fourcc.h>

#define WLR_USE_UNSTABLE
extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/log.h>
}

namespace {

// native_buffer.h 的 usage 位与 SDK 头一致; 如后续真机返回码变化, 先查这里
constexpr uint64_t kOhosBufferUsage = NATIVEBUFFER_USAGE_CPU_READ |
                                      NATIVEBUFFER_USAGE_CPU_WRITE |
                                      NATIVEBUFFER_USAGE_CPU_READ_OFTEN;

struct WlOhosBuffer {
    struct wlr_buffer base;
    OH_NativeBuffer *native = nullptr;
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
    if (!buffer || buffer->impl != &kBufferImpl)
        return 0;
    return BufferFromBase(buffer)->stride;
}

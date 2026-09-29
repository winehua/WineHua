/*
 * ohos_buffer.h — OH_NativeBuffer 背书的 wlr_buffer / wlr_allocator (M0-T8)
 *
 * pixman renderer 经 begin_data_ptr_access 直接写 buffer 内存
 * (render/pixman/renderer.c:33 wlr_buffer_begin_data_ptr_access 门);
 * OHOS 分配的 RGBA8888 布局 == DRM_FORMAT_ABGR8888 (DRM 四字码按小端
 * 字节序命名), pixman 格式表支持 (render/pixman/pixel_format.c:32)。
 */
#pragma once

/*
 * WLR_USE_UNSTABLE + extern "C": wlroots 头无 C++ guard, C++ TU 直接
 * include 会把符号按 C++ mangle, 链接报 undefined (T8 实测)。
 */
#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif

struct OH_NativeBuffer;
#ifdef __cplusplus
extern "C" {
#endif

#include <wlr/render/allocator.h>

// 创建 CPU 内存 (OH_NativeBuffer) 背书的 allocator, caps = DATA_PTR。
// 返回值由 wlr_allocator 通用销毁 (wlr_allocator_destroy) 释放。
struct wlr_allocator *wl_ohos_allocator_create(void);

// 取 wlr_buffer 背书的 OH_NativeBuffer (仅接受本 allocator 创建的 buffer)
struct OH_NativeBuffer *wl_ohos_buffer_native(struct wlr_buffer *buffer);

// 帧缓冲字节步长 (GetConfig 回读; 源/写两侧统一用它防行错位)
size_t wl_ohos_buffer_stride(struct wlr_buffer *buffer);

#ifdef __cplusplus
}
#endif

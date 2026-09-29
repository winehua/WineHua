/*
 * ohos_buffer.h — OH_NativeBuffer 背书的 wlr_buffer / wlr_allocator (M0-T8)
 *
 * pixman renderer 经 begin_data_ptr_access 直接写 buffer 内存
 * (render/pixman/renderer.c:33 wlr_buffer_begin_data_ptr_access 门);
 * OHOS 分配的 RGBA8888 布局 == DRM_FORMAT_ABGR8888 (DRM 四字码按小端
 * 字节序命名), pixman 格式表支持 (render/pixman/pixel_format.c:32)。
 */
#pragma once

#include <stdint.h>

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

// 帧缓冲 DRM 四字码 (allocator 记录的那一个, 与 OHOS 像素格式一一对应)
uint32_t wl_ohos_buffer_drm_format(struct wlr_buffer *buffer);

/*
 * 取 OH_NativeBuffer 的 OHNativeWindowBuffer 包装 (惰性创建, 随 buffer 销毁
 * 释放)。EGL 的 EGL_NATIVE_BUFFER_OHOS 导入只接受这个包装载荷 —— 裸
 * OH_NativeBuffer* 被按 native_window 对象校验拒掉 (M2-T2 实测:
 * EGL_BAD_PARAMETER 0x300c + hilog "NativeObject Invalid magic illegal")。
 * 声明用 struct NativeWindowBuffer*: SDK 侧是
 * `typedef struct NativeWindowBuffer OHNativeWindowBuffer`, 同一类型。
 */
struct NativeWindowBuffer *wl_ohos_buffer_window_buffer(struct wlr_buffer *buffer);

/*
 * ── present buffer: window BufferQueue 借来的 buffer (M2-T4 零拷贝输出) ──
 *
 * 来源是 NativeWindow 队列 (RequestBuffer 借一格 → 写 → FlushBuffer 归还),
 * 不是 allocator 分配 —— 因此**一帧一借**: 队列状态机不允许跨帧复用同一个
 * buffer (未重新 Request 就写 = 状态非法, T8 路径 A BUFFER_STATE_INVALID
 * 的实测教训)。wlroots swapchain 的复用模型与它对不上, present 路径每帧
 * 新建 swapchain (见 ohos_output.c)。
 *
 * 行距/尺寸/格式以 BufferHandle 为准 (系统定的, 可能带 padding) —— 与
 * allocator buffer 不同, 这里没有 stride==width*4 的假设。
 */
/* SDK 侧 typedef struct NativeWindow OHNativeWindow; 前置声明要用真 tag */
struct NativeWindow;

// 借一格队列 buffer 并包成 wlr_buffer (失败返回 NULL)。
// 失败时内部已归还可能的 fence, 调用方无需处理。
struct wlr_buffer *wl_ohos_present_buffer_acquire(struct NativeWindow *window);

// 是否本模块 present buffer (供提交路径分流: present = 直推, 其他 = 拷贝)
int wl_ohos_present_buffer_owns(struct wlr_buffer *buffer);

// 归还队列: FlushBuffer (dequeued→queued, 消费者可显示)。返回 OHOS rc。
int32_t wl_ohos_present_buffer_present(struct wlr_buffer *buffer, int fence_fd);

// 归还队列但并不呈现 (dequeued→free, 丢帧/失败路径必须调用, 否则槽位泄漏)
int32_t wl_ohos_present_buffer_abort(struct wlr_buffer *buffer);

// 是否已归还 (present/abort 之后为 1)。未归还的 buffer 销毁时会兜底 abort,
// 但那条路上只能记日志 —— 调用方按此显式归还, 避免走到兜底。
int wl_ohos_present_buffer_returned(struct wlr_buffer *buffer);

// EGL 导入载荷 (EGL_NATIVE_BUFFER_OHOS 要的 OHNativeWindowBuffer*; 队列
// buffer 本身就是, 直接返回)
struct NativeWindowBuffer *wl_ohos_present_buffer_window_buffer(struct wlr_buffer *buffer);

/*
 * ── guest frame buffer: OH_NativeImage 消费者队列借来的 buffer (M2-T5) ──
 *
 * 生产者是宿主 virgl/vtest 侧按 SURFACE_ID (= X window id) 路由进来的 guest
 * Vulkan 帧 (win32u 私有 present 送出)。消费者侧与 present 侧同构: 一格一
 * 借、一帧一还, 未归还就销毁等于队列槽位泄漏 (销毁时兜底归还并留证)。导入
 * 沿用 T4 的 EGL_NATIVE_BUFFER_OHOS 路径, 因此没有 data_ptr 访问。
 */
struct OH_NativeImage;

// 借一格 guest 帧 buffer 并包成 wlr_buffer。无新帧 (生产者没提交) 或失败
// 返回 NULL —— 都是正常状态, 调用方按"本轮无帧"处理。
struct wlr_buffer *wl_ohos_consumer_buffer_acquire(struct OH_NativeImage *image);

// 是否本模块 guest 帧 buffer
int wl_ohos_consumer_buffer_owns(struct wlr_buffer *buffer);

// 归还队列: ReleaseNativeWindowBuffer (dequeued→free, 生产者可再写)。
// 与 present 侧同样传 -1 (不做 GPU 侧同步), 依据: T4 输出路径实测口径。
int32_t wl_ohos_consumer_buffer_release(struct wlr_buffer *buffer, int fence_fd);

// 丢弃未上屏的一帧 (释放 buffer 对象并归还队列槽位)。未挂上 scene 的帧必须
// 走这里 —— 只调 release 的话 buffer 对象仍持一次引用, 没人再释放它。
void wl_ohos_consumer_buffer_drop(struct wlr_buffer *buffer);

// EGL 导入载荷 (同上, 队列 buffer 本身就是)
struct NativeWindowBuffer *wl_ohos_consumer_buffer_window_buffer(struct wlr_buffer *buffer);

#ifdef __cplusplus
}
#endif

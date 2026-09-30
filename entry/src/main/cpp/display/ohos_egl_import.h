/*
 * ohos_egl_import.h — host 侧 gles2 渲染器 + OHOS NativeBuffer EGL 导入 (M2-T4)
 *
 * 合成器渲染器从 pixman 切到 wlroots gles2; 本项目的 OH_NativeBuffer 背书
 * buffer 没有 DMA-BUF 导出, 由本模块注册的导入器出 EGLImage
 * (EGL_NATIVE_BUFFER_OHOS), 渲染目标与采样纹理共用同一个 GL 对象 —— 即
 * 输出路径的零拷贝底座 (实现与实测依据见 ohos_egl_import.c)。
 *
 * wlroots 头不 C++ 安全, 本头只暴露不透明指针 (display_compositor.cpp 走它)。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct wlr_renderer;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 建 gles2 渲染器 (自建 host EGL 上下文 + 注册 OHOS 导入器)。
 * 失败返回 NULL, 调用方回退 pixman。返回值由 wlr_renderer_destroy 释放
 * ——该调用同时销毁本模块自建的 EGL 上下文 (wlroots gles2 销毁链),
 * 不要再单独碰 EGL。
 */
struct wlr_renderer *wl_ohos_egl_renderer_create(void);

/* gles2 是否已激活 (是否需要在读源 buffer 像素前做 GPU 同步的判据) */
bool wl_ohos_egl_active(void);

/*
 * 把已提交的 GL 命令同步到 CPU 可见 (glFinish)。
 * M2-T4 Step A 的 memcpy 呈现路径在映射源 buffer 之前调用; 输出路径全量
 * 退位为零拷贝后, 本调用连同 memcpy 一并消失。
 */
void wl_ohos_egl_finish(void);

/*
 * GPU 同步点计数: 每次 glFinish 成功自增。guest 帧归还侧用它做不变量检查
 * (known-issues §2.6): 「buffer 交给合成器之后有没有发生过一次 GPU 同步」
 * —— 计数只由 wl_ohos_egl_finish 自增, 把 glFinish 换成 fence 的人必须同时
 * 改这里, 否则归还侧会立刻报不变量破损 (防静默退化)。
 */
uint64_t wl_ohos_egl_sync_count(void);

/*
 * present 前置探针 (M2-T4 Step 3 门): window 队列借一格 buffer → 生产导入器
 * 出 EGLImage → GPU 渲染 → CPU 按 handle 行距逐像素校验 → FlushBuffer 归还。
 * 结论写 marker_path (设备端只跑不判, 判定在主机)。marker 已存在则跳过并
 * 返回 true。must-run-but-failed 返回 false (调用方只记日志, 不改行为)。
 */
struct NativeWindow;
bool wl_ohos_egl_present_probe(struct NativeWindow *window,
                               const char *marker_path);

#ifdef __cplusplus
}
#endif

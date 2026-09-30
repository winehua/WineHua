/*
 * display_fps.h — 宿主"显示序列"发布接口 (C 可调)
 *
 * guest 用例 (winehua_graphics_smoke) 读 WINEHUA_DISPLAY_FPS_FILE 判断"宿主
 * 是否真的在出图": 文件内容 <sequence> <fps> <id>, sequence 变化才算观察到
 * 显示序列 (见 thirdparty/wine/programs/winehua_graphics_smoke/main.c
 * update_display_fps)。两条路线的宿主渲染器都要写它:
 *   wayland 路线: graphics/egl_renderer.cpp (per-toplevel, RendererPerfWindow)
 *   X 路线:       display/ohos_output.c (整个 output 的提交序列)
 * 写法只有一份 (common/perf_utils.cpp: 原子 tmp+rename), 本头只给 C 侧入口。
 */
#ifndef WINEHUA_COMMON_DISPLAY_FPS_H
#define WINEHUA_COMMON_DISPLAY_FPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sequence: 宿主显示序列号 (单调递增, 不前进 = 没出图); surface_id: 该序列
 * 归属的窗口 id, X 路线是 output 级序列, 传 0。返回 0 = 已落盘。 */
int winehua_display_fps_publish(uint32_t surface_id, uint64_t sequence, double fps);

#ifdef __cplusplus
}
#endif

#endif /* WINEHUA_COMMON_DISPLAY_FPS_H */

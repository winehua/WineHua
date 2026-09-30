/*
 * display_fps.h — 宿主"显示序列"发布接口 (C 可调)
 *
 * guest 用例 (winehua_graphics_smoke) 读 WINEHUA_DISPLAY_FPS_FILE 判断"宿主
 * 是否真的在出图": 文件内容 <sequence> <fps> <id> <route> <surface_key>,
 * sequence 变化才算观察到显示序列 (见 thirdparty/wine/programs/
 * winehua_graphics_smoke/main.c update_display_fps)。两条路线的宿主渲染器
 * 都要写它:
 *   wayland 路线: graphics/egl_renderer.cpp (per-toplevel, RendererPerfWindow)
 *   X 路线:       display/ohos_output.c (整个 output 的提交序列)
 * 写法只有一份 (common/perf_utils.cpp: 原子 tmp+rename), 本头只给 C 侧入口。
 *
 * route / surface_key 是**归属证据**, 不是装饰: 2026-10-01 实测过一次
 * "GL 帧实际由 wayland 渲染器取走、X 路线一帧没收到, 用例照样 PASS" ——
 * 只看 sequence/fps 分不出是哪条路线在出图。route 写本渲染器所属路线,
 * surface_key 写本秒内实际呈现的那个 guest 面 (没有则 0)。guest 只上报原始
 * 读数; 归属断言在判定层做 (automation/checks presented-route): key 高 32 位
 * 是 Unix getpid, 与运行器记录的 spawn pid 对账 —— guest 侧拿不到 Unix pid
 * (只有 Wine ptid), 自己比恒假。
 */
#ifndef WINEHUA_COMMON_DISPLAY_FPS_H
#define WINEHUA_COMMON_DISPLAY_FPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sequence: 宿主显示序列号 (单调递增, 不前进 = 没出图); surface_id: 该序列
 * 归属的窗口 id, X 路线是 output 级序列, 传 0; route: "wayland" / "x11";
 * surface_key: 本秒内实际呈现的 guest 面 ((pid<<32)|surface_id), 无则 0。
 * 返回 0 = 已落盘。 */
int winehua_display_fps_publish(uint32_t surface_id, uint64_t sequence, double fps,
                                const char *route, uint64_t surface_key);

#ifdef __cplusplus
}
#endif

#endif /* WINEHUA_COMMON_DISPLAY_FPS_H */

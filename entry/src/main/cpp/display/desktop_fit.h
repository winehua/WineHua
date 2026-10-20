#pragma once
/*
 * desktop_fit.h — 桌面内容 → 画布 保比例适配矩形 (letterbox) 的 C 入口。
 *
 * 用途 (x11 虚拟桌面 "游戏全屏适配", 2026-10-15): 游戏 ChangeDisplaySettings
 * 后 wine 把虚拟桌面窗缩到游戏分辨率 (上游语义, defwnd.c WM_DISPLAYCHANGE),
 * 宿主 x11 臂原来按 X 坐标 1:1 呈现 —— 桌面缩成 800x600 后画面只占画布左上
 * 角。产品口径 (用户拍板): Pad 画布恒全屏, 桌面内容等比放大居中显示, 剩余
 * 用纯色 (黑) letterbox 填充; 输入坐标按同一映射逆变换。
 *
 * 实现单一性: 几何数学 = compositor/frame/geometry.h 的 ComputeFitRect /
 * FitUnmapDisplayX (wayland 臂全屏 fit 的既有唯一实现), 本头只是 C 可用的
 * extern "C" 壳 (ohos_output.c / display_input.c / ohos_egl_import.c 是 C
 * 编译), 不另写一份公式 —— 四套变换纪律 (COMPOSITOR_UNIFICATION S4) 跨臂
 * 同样适用。
 *
 * 坐标系约定: 全部参数/结果都在 "画布逻辑系" (= out_w/out_h, 也 = 画布
 * EGL buffer 的像素系, 见 ohos_output.c wl_ohos_output_resize 的
 * SET_BUFFER_GEOMETRY; 系统管线再把 buffer 铺满物理面板)。桌面内容矩形
 * (dx,dy,dw,dh) 是桌面窗在 X 坐标系的几何 (wl_ohos_output_desktop_rect)。
 */
#ifdef __cplusplus
extern "C" {
#endif

struct DesktopFitRect {
    int srcW, srcH;   /* 内容尺寸 (桌面窗) */
    int dstW, dstH;   /* 内容在画布内的缩放后尺寸 (黑边之内) */
    int offX, offY;   /* 内容区在画布中的原点 (letterbox 偏移) */
    double scale;     /* dst/src 的未取整缩放系数 */
};

/* 计算保比例缩放 + 居中适配 (canvas 画布 ← content 桌面窗)。
 * 任一尺寸 <= 0 返回 0 (out 不动); 相等时输出恒等 (scale=1, off=0)。 */
int WineHua_ComputeDesktopFit(int canvasW, int canvasH,
                              int contentW, int contentH,
                              struct DesktopFitRect *out);

/* 画布逻辑坐标 → 内容系坐标 (相对内容原点; 调用方自行加桌面原点 dx/dy)。
 * 与呈现严格互逆: 按取整后 dst 尺寸 (FitUnmapDisplayX 族) —— 屏幕上实际
 * 可见像素 = 整数 dst, 逆映射必须同源, 否则复现 1px 偏差事故。 */
double WineHua_FitUnmapX(const struct DesktopFitRect *t, double px);
double WineHua_FitUnmapY(const struct DesktopFitRect *t, double py);

#ifdef __cplusplus
}
#endif

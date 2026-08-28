#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// 保比例适配 (letterbox) 几何: 正/逆映射的唯一实现。
//
// 背景: "把 Wine 帧保比例缩放、居中显示到窗口/桌面" 这一映射曾有多份独立实现
// (egl_renderer letterbox、ComputeFullscreenTransform、输入侧手写逆映射),
// 系数和取整方式不一致是全屏/黑边鼠标问题的温床 (docs/CPP_REFACTOR_PLAN.md Phase 1)。
// 本模块为纯函数, 不依赖 wayland/OHOS, 宿主机可直接单测 (host_tests/geometry_test.cpp,
// make test)。

struct FitRect {
    int srcW = 0, srcH = 0;  // 内容尺寸 (Wine 帧)
    int dstW = 0, dstH = 0;  // 内容在显示区内的缩放后尺寸 (黑边之内)
    int offX = 0, offY = 0;  // 内容区在显示区中的原点 (黑边偏移)
    double scale = 1.0;      // dst/src 的未取整缩放系数
};

// 计算保比例缩放 + 居中的适配矩形。任一尺寸 <= 0 返回 false (out 不动)。
bool ComputeFitRect(int rootW, int rootH, int winW, int winH, FitRect& out);

// -- 正/逆映射 (数学定义, 用未取整 scale) --
// desktop 合成/命中路径用: 合成用未取整 scale blit, 输入按同一 scale 除回, 严格互逆。
inline double FitMapX(const FitRect& t, double x) { return t.offX + x * t.scale; }
inline double FitMapY(const FitRect& t, double y) { return t.offY + y * t.scale; }

// subsurface 相对窗口原点的 fit 映射矩形 — 渲染 blit 与输入命中共用的唯一
// 实现 (原两侧手写同一数学: 渲染 desktop_compositor.cpp blitSubsurface 全屏
// 分支, 输入 input_resolver.cpp FindInputTargetAt 全屏分支)。relX/relY 为
// subsurface 相对所属窗口原点的偏移; dispW/dispH 为实际绘制尺寸 (viewport
// 裁剪后); scale=0 时宽度取 0 → 调用方以 max(1, ...) 兜底不为 0。
inline void FitMapLayerRect(const FitRect& t, int relX, int relY,
                            int dispW, int dispH,
                            int& scrX, int& scrY, int& scrW, int& scrH)
{
    scrX = static_cast<int>(lround(FitMapX(t, relX)));
    scrY = static_cast<int>(lround(FitMapY(t, relY)));
    scrW = std::max(1, static_cast<int>(lround(dispW * t.scale)));
    scrH = std::max(1, static_cast<int>(lround(dispH * t.scale)));
}

// -- 正/逆映射 (按取整后的 dst 尺寸) --
// glViewport 路径用: 实际显示占据 dstW x dstH 整数像素, 与屏幕上可见像素的换算
// 严格一致。egl_renderer zero-copy 层视口 (正) 和 CoordTransform 输入换算 (逆) 用。
// 全部带零尺寸防御: ComputeFitRect 失败时 FitRect 保持全零, 除零会直接 SIGFPE。
inline int FitMapDisplayX(const FitRect& t, int64_t x) { return t.srcW > 0 ? t.offX + static_cast<int>(x * t.dstW / t.srcW) : 0; }
inline int FitMapDisplayY(const FitRect& t, int64_t y) { return t.srcH > 0 ? t.offY + static_cast<int>(y * t.dstH / t.srcH) : 0; }
// 尺寸正映射 (同上, 不带原点偏移)
inline int FitSizeDisplayW(const FitRect& t, int64_t w) { return t.srcW > 0 ? static_cast<int>(w * t.dstW / t.srcW) : 0; }
inline int FitSizeDisplayH(const FitRect& t, int64_t h) { return t.srcH > 0 ? static_cast<int>(h * t.dstH / t.srcH) : 0; }
inline double FitUnmapDisplayX(const FitRect& t, double px) { return t.dstW > 0 ? (px - t.offX) * t.srcW / t.dstW : 0.0; }
inline double FitUnmapDisplayY(const FitRect& t, double py) { return t.dstH > 0 ? (py - t.offY) * t.srcH / t.dstH : 0.0; }

// -- wp_viewport destination 生效后的显示尺寸 --
// vpDst > 0 时 surface 按 viewport 目标尺寸显示, 否则 (<=0, 含未设置的 -1)
// 回退 buffer 尺寸 (w/h)。两个变体语义不同, 原为 11 处手写三元式
// (docs/COMPOSITOR_REFACTOR_PLAN.md §2.3), clamp 与否的差异可能是有意的,
// 不得互换/统一 (统一语义是单独的行为变更):
//
// - DisplaySizeAfterViewport (不 clamp): viewport dst 直通, 显示尺寸可超过
//   buffer (buffer 对齐填充大于内容 / 需要放大显示时, 源像素按此尺寸缩放
//   上屏)。使用场景: ZC 层内容几何 (GetZeroCopyLayerInfo /
//   GetZeroCopyContentSize / GetZeroCopyOccluders / BuildWindowLayerList)、
//   wl_core CopyToplevelContent 逻辑尺寸。
// - DisplaySizeAfterViewportClamped (min clamp): 显示尺寸封顶不超过 buffer
//   — blit 源只有 buffer 尺寸, dst 更大时无源像素可放大, 按 buffer 截断。
//   使用场景: 全屏 fit 路径 (fullscreenContentCovered 判定 / 全屏 fit 矩形 /
//   输入 FindInputTargetAt 全屏命中)。
inline int DisplaySizeAfterViewport(int32_t vpDst, int fallback)
{
    return vpDst > 0 ? vpDst : fallback;
}
inline int DisplaySizeAfterViewportClamped(int32_t vpDst, int fallback)
{
    return vpDst > 0 ? std::min(vpDst, fallback) : fallback;
}

// 全屏内容尺寸选择 (渲染/输入共用单一权威源, 渲染侧全屏几何的镜像):
// - ZC 游戏 (toplevel 带 zero-copy GL 层): 画面是 GL 层按实际内容几何渲染的,
//   全屏后 buffer 被 Wine 扩到输出尺寸但画面不变 → 用 layer 几何
//   (zero-copy subsurface 层实际内容尺寸, vpDst 裁剪后 = egl_renderer 渲染
//   视口同源值)。用 layer 而非「全屏前尺寸快照」: 后者只是窗口尺寸的近似,
//   与画面真实内容可失配 (viewport 裁剪 / 游戏先扩 buffer 再 set_fullscreen),
//   曾导致输入逆映射与渲染显示基准分裂 → 全屏游戏光标常数平移偏移。
// - SHM 游戏: buffer 即画面, 扩到输出尺寸后内容填满整个 buffer → 用 buffer 尺寸
// hasZeroCopyLayer 与 layerW/H 由调用方遍历合成层得到 (非纯函数部分, 故作
// 参数传入); layer 无效 (0) 时退化用 buffer (与渲染视口层几何不可用一致)。
inline void SelectFullscreenContentSize(int32_t layerW, int32_t layerH,
                                        int32_t bufW, int32_t bufH,
                                        bool hasZeroCopyLayer,
                                        int& outW, int& outH)
{
    const bool useLayer = hasZeroCopyLayer && layerW > 0 && layerH > 0;
    outW = useLayer ? layerW : bufW;
    outH = useLayer ? layerH : bufH;
}

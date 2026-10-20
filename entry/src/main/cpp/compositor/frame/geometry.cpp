#include "geometry.h"
#include "display/desktop_fit.h" /* C 壳 (x11 臂呈现/输入消费本文件的唯一实现) */
#include <algorithm>
#include <cmath>

bool ComputeFitRect(int rootW, int rootH, int winW, int winH, FitRect& out)
{
    if (rootW <= 0 || rootH <= 0 || winW <= 0 || winH <= 0) return false;
    out.srcW = winW;
    out.srcH = winH;
    out.scale = std::min(rootW / static_cast<double>(winW), rootH / static_cast<double>(winH));
    // dst 取整用 lround (而非截断): 居中偏移 (root - dst) / 2 两侧的取整方向对称,
    // 避免历史实现中截断导致的 1px 系统性偏移
    out.dstW = std::max(1, static_cast<int>(lround(winW * out.scale)));
    out.dstH = std::max(1, static_cast<int>(lround(winH * out.scale)));
    out.offX = (rootW - out.dstW) / 2;
    out.offY = (rootH - out.dstH) / 2;
    return true;
}

/* ── C 壳 (display/desktop_fit.h) ────────────────────────────────────────
 * x11 臂 (C 编译的 display/*.c) 复用本文件唯一实现的入口。薄转发, 无公式
 * 复制; 字段一一对应。 */
extern "C" int WineHua_ComputeDesktopFit(int canvasW, int canvasH,
                                         int contentW, int contentH,
                                         struct DesktopFitRect *out)
{
    if (!out) return 0;
    FitRect f;
    if (!ComputeFitRect(canvasW, canvasH, contentW, contentH, f)) return 0;
    out->srcW = f.srcW;
    out->srcH = f.srcH;
    out->dstW = f.dstW;
    out->dstH = f.dstH;
    out->offX = f.offX;
    out->offY = f.offY;
    out->scale = f.scale;
    return 1;
}

extern "C" double WineHua_FitUnmapX(const struct DesktopFitRect *t, double px)
{
    if (!t) return px;
    FitRect f{t->srcW, t->srcH, t->dstW, t->dstH, t->offX, t->offY, t->scale};
    return FitUnmapDisplayX(f, px);
}

extern "C" double WineHua_FitUnmapY(const struct DesktopFitRect *t, double py)
{
    if (!t) return py;
    FitRect f{t->srcW, t->srcH, t->dstW, t->dstH, t->offX, t->offY, t->scale};
    return FitUnmapDisplayY(f, py);
}

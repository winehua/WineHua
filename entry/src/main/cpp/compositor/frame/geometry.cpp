#include "geometry.h"
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

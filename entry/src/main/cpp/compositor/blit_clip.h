#pragma once
#include <algorithm>

// blit 裁剪数学 (纯函数, 无 wayland/OHOS 依赖 — 宿主机可直接单测,
// host_tests/blit_clip_test.cpp, make test)。
// 原为 TakeToplevelFrame 各 blit 段内联的手写裁剪式 (重构第 2A 步逐字提取,
// 数学不变; 对应 docs/COMPOSITOR_REFACTOR_PLAN.md §2.3 "矩形相交/裁剪数学
// 5 份手写" 中 blit 用的 4 份 — 第 5 份 (GetZeroCopyOccluders pushRect)
// 语义不同不合并)。

// 源图 (srcW×srcH) 绘制到目标 (targetW×targetH) 的 (posX,posY): 负原点裁剪
// (源起点正偏移) + 目标边界裁剪。返回 false = 完全不可见 (拷贝尺寸 ≤0)。
inline bool ClipBlitToTarget(int posX, int posY, int srcW, int srcH,
                             int targetW, int targetH,
                             int& srcX, int& srcY, int& dstX, int& dstY,
                             int& copyW, int& copyH)
{
    dstX = (posX > 0) ? posX : 0;
    dstY = (posY > 0) ? posY : 0;
    srcX = (posX < 0) ? -posX : 0;
    srcY = (posY < 0) ? -posY : 0;
    copyW = srcW - srcX;
    copyH = srcH - srcY;
    if (dstX + copyW > targetW) copyW = targetW - dstX;
    if (dstY + copyH > targetH) copyH = targetH - dstY;
    return copyW > 0 && copyH > 0;
}

// dst 矩形与重绘矩形 R 求交, src 起点同步偏移 (局部合成: R 外复用上帧内容,
// 不重画)。返回 false = 与 R 不相交。
inline bool IntersectBlitWithDamage(int& srcX, int& srcY, int& dstX, int& dstY,
                                    int& copyW, int& copyH,
                                    int dmgX, int dmgY, int dmgW, int dmgH)
{
    const int l = std::max(dstX, dmgX), t = std::max(dstY, dmgY);
    const int r = std::min(dstX + copyW, dmgX + dmgW);
    const int b = std::min(dstY + copyH, dmgY + dmgH);
    if (r <= l || b <= t) return false;
    srcX += l - dstX; srcY += t - dstY;
    dstX = l; dstY = t; copyW = r - l; copyH = b - t;
    return true;
}

// 矩形与重绘矩形 R 求交 (无源, 填充用)。返回 false = 与 R 不相交。
inline bool IntersectRectWithDamage(int& x, int& y, int& w, int& h,
                                    int dmgX, int dmgY, int dmgW, int dmgH)
{
    const int l = std::max(x, dmgX), t = std::max(y, dmgY);
    const int r = std::min(x + w, dmgX + dmgW);
    const int b = std::min(y + h, dmgY + dmgH);
    if (r <= l || b <= t) return false;
    x = l; y = t; w = r - l; h = b - t;
    return true;
}

// src 矩形与 damage 包围盒求交, dst 起点同步偏移。返回 false = 不相交。
inline bool ClipBlitSourceToRect(int& srcX, int& srcY, int& dstX, int& dstY,
                                 int& copyW, int& copyH,
                                 int rectX, int rectY, int rectW, int rectH)
{
    const int l = std::max(srcX, rectX), t = std::max(srcY, rectY);
    const int r = std::min(srcX + copyW, rectX + rectW);
    const int b = std::min(srcY + copyH, rectY + rectH);
    if (r <= l || b <= t) return false;
    dstX += l - srcX; dstY += t - srcY;
    srcX = l; srcY = t; copyW = r - l; copyH = b - t;
    return true;
}

// blit_clip.h (FrameBlitter 纯像素裁剪数学) 的宿主机单元测试 (make test)。
// 覆盖: 负原点裁剪 (ClipBlitToTarget)、dst∩R 裁剪 (IntersectBlitWithDamage /
// IntersectRectWithDamage)、src∩damage 包围盒 (ClipBlitSourceToRect)。
// 黄金值用例 + 固定种子随机用例与逐像素暴力参考实现对比。
#include "compositor/frame/blit_clip.h"
#include "compositor/frame/compositor_blit.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

// 矩形 x,y,w,h 与 rx,ry,rw,rh 的逐像素暴力交集 (无交集返回 false)。
// 与生产实现使用不同的计算路径 (逐像素成员判定), 作为随机用例参考。
static bool BruteIntersect(int x, int y, int w, int h,
                           int rx, int ry, int rw, int rh,
                           int& ox, int& oy, int& ow, int& oh)
{
    int l = 0, t = 0, r = 0, b = 0;
    bool first = true;
    for (int cy = std::min(y, ry); cy < std::max(y + h, ry + rh); ++cy)
        for (int cx = std::min(x, rx); cx < std::max(x + w, rx + rw); ++cx) {
            const bool in = cx >= x && cx < x + w && cy >= y && cy < y + h &&
                            cx >= rx && cx < rx + rw && cy >= ry && cy < ry + rh;
            if (!in) continue;
            if (first) { l = cx; t = cy; r = cx + 1; b = cy + 1; first = false; }
            else {
                l = std::min(l, cx); t = std::min(t, cy);
                r = std::max(r, cx + 1); b = std::max(b, cy + 1);
            }
        }
    if (first) return false;
    ox = l; oy = t; ow = r - l; oh = b - t;
    return true;
}

int main()
{
    // -- ClipBlitToTarget 黄金值 --
    {
        int sx = -1, sy = -1, dx = -1, dy = -1, cw = -1, ch = -1;
        // 完全在内
        CHECK(ClipBlitToTarget(10, 20, 30, 40, 100, 100, sx, sy, dx, dy, cw, ch),
              "inside visible");
        CHECK(sx == 0 && sy == 0 && dx == 10 && dy == 20 && cw == 30 && ch == 40,
              "inside values");
        // 负原点: 源起点正偏移, dst 钳 0
        CHECK(ClipBlitToTarget(-5, -10, 30, 40, 100, 100, sx, sy, dx, dy, cw, ch),
              "neg origin visible");
        CHECK(sx == 5 && sy == 10 && dx == 0 && dy == 0 && cw == 25 && ch == 30,
              "neg origin values");
        // 负原点超出源: 不可见
        CHECK(!ClipBlitToTarget(-30, 0, 30, 40, 100, 100, sx, sy, dx, dy, cw, ch),
              "neg beyond src");
        CHECK(!ClipBlitToTarget(0, -41, 30, 40, 100, 100, sx, sy, dx, dy, cw, ch),
              "neg beyond src y");
        // 右/下越界部分裁剪
        CHECK(ClipBlitToTarget(80, 90, 30, 40, 100, 100, sx, sy, dx, dy, cw, ch),
              "off right/bottom visible");
        CHECK(sx == 0 && sy == 0 && dx == 80 && dy == 90 && cw == 20 && ch == 10,
              "off right/bottom values");
        // 完全在目标外
        CHECK(!ClipBlitToTarget(100, 0, 30, 40, 100, 100, sx, sy, dx, dy, cw, ch),
              "fully off right");
        CHECK(!ClipBlitToTarget(0, 100, 30, 40, 100, 100, sx, sy, dx, dy, cw, ch),
              "fully off bottom");
        // 源大于目标
        CHECK(ClipBlitToTarget(0, 0, 200, 150, 100, 100, sx, sy, dx, dy, cw, ch),
              "src larger visible");
        CHECK(dx == 0 && dy == 0 && cw == 100 && ch == 100, "src larger clamp");
        // 负原点 + 目标边界同时裁剪
        CHECK(ClipBlitToTarget(-10, -10, 50, 50, 35, 35, sx, sy, dx, dy, cw, ch),
              "neg+clamp visible");
        CHECK(sx == 10 && sy == 10 && dx == 0 && dy == 0 && cw == 35 && ch == 35,
              "neg+clamp values");
        // 贴边 1 像素
        CHECK(ClipBlitToTarget(99, 99, 10, 10, 100, 100, sx, sy, dx, dy, cw, ch),
              "edge 1px visible");
        CHECK(dx == 99 && dy == 99 && cw == 1 && ch == 1, "edge 1px values");
    }

    // -- IntersectBlitWithDamage 黄金值 (dst∩R, src 同步偏移) --
    {
        int sx, sy, dx, dy, cw, ch;
        // 部分相交 (左上): R 切掉 dst 左/上边
        sx = 2; sy = 3; dx = 10; dy = 20; cw = 30; ch = 40;
        CHECK(IntersectBlitWithDamage(sx, sy, dx, dy, cw, ch, 25, 35, 10, 10),
              "dst∩R partial visible");
        CHECK(dx == 25 && dy == 35 && cw == 10 && ch == 10, "dst∩R partial dst");
        CHECK(sx == 17 && sy == 18, "dst∩R partial src sync");
        // 不相交
        sx = 0; sy = 0; dx = 0; dy = 0; cw = 10; ch = 10;
        CHECK(!IntersectBlitWithDamage(sx, sy, dx, dy, cw, ch, 20, 20, 5, 5),
              "dst∩R disjoint");
        // 边缘相贴不算相交
        CHECK(!IntersectBlitWithDamage(sx, sy, dx, dy, cw, ch, 10, 0, 5, 5),
              "dst∩R touching edge");
        // R 完全包含 blit: 不变
        sx = 2; sy = 3; dx = 10; dy = 20; cw = 30; ch = 40;
        CHECK(IntersectBlitWithDamage(sx, sy, dx, dy, cw, ch, 0, 0, 100, 100),
              "dst∩R contains visible");
        CHECK(sx == 2 && sy == 3 && dx == 10 && dy == 20 && cw == 30 && ch == 40,
              "dst∩R contains unchanged");
        // blit 完全包含 R: 结果恰为 R, src 同步
        sx = 5; sy = 6; dx = 10; dy = 20; cw = 30; ch = 40;
        CHECK(IntersectBlitWithDamage(sx, sy, dx, dy, cw, ch, 15, 25, 8, 9),
              "dst∩R inside visible");
        CHECK(dx == 15 && dy == 25 && cw == 8 && ch == 9, "dst∩R inside dst");
        CHECK(sx == 10 && sy == 11, "dst∩R inside src sync");
    }

    // -- IntersectRectWithDamage 黄金值 (无源矩形∩R) --
    {
        int x, y, w, h;
        x = 10; y = 20; w = 30; h = 40;
        CHECK(IntersectRectWithDamage(x, y, w, h, 25, 35, 10, 10), "rect∩R visible");
        CHECK(x == 25 && y == 35 && w == 10 && h == 10, "rect∩R values");
        x = 0; y = 0; w = 10; h = 10;
        CHECK(!IntersectRectWithDamage(x, y, w, h, 20, 20, 5, 5), "rect∩R disjoint");
        x = 10; y = 20; w = 30; h = 40;
        CHECK(IntersectRectWithDamage(x, y, w, h, 0, 0, 100, 100), "rect∩R contains");
        CHECK(x == 10 && y == 20 && w == 30 && h == 40, "rect∩R contains unchanged");
    }

    // -- ClipBlitSourceToRect 黄金值 (src∩damage, dst 同步偏移) --
    {
        int sx, sy, dx, dy, cw, ch;
        // 部分相交: 只留 damage 内源像素, dst 同步
        sx = 5; sy = 5; dx = 105; dy = 105; cw = 20; ch = 20;
        CHECK(ClipBlitSourceToRect(sx, sy, dx, dy, cw, ch, 10, 10, 5, 5),
              "src∩dmg visible");
        CHECK(sx == 10 && sy == 10 && cw == 5 && ch == 5, "src∩dmg src");
        CHECK(dx == 110 && dy == 110, "src∩dmg dst sync");
        // 不相交
        sx = 0; sy = 0; dx = 100; dy = 100; cw = 10; ch = 10;
        CHECK(!ClipBlitSourceToRect(sx, sy, dx, dy, cw, ch, 20, 20, 5, 5),
              "src∩dmg disjoint");
        // damage 完全包含源: 不变
        sx = 5; sy = 6; dx = 100; dy = 100; cw = 10; ch = 10;
        CHECK(ClipBlitSourceToRect(sx, sy, dx, dy, cw, ch, 0, 0, 50, 50),
              "src∩dmg contains visible");
        CHECK(sx == 5 && sy == 6 && dx == 100 && dy == 100 && cw == 10 && ch == 10,
              "src∩dmg contains unchanged");
    }

    // -- 固定种子随机用例 vs 逐像素暴力参考 --
    {
        std::mt19937 rng(20260828);
        std::uniform_int_distribution<int> coord(-20, 60);
        std::uniform_int_distribution<int> size(1, 30);
        int fuzzFailures = 0;
        for (int iter = 0; iter < 2000; ++iter) {
            const int targetW = 50, targetH = 50;
            // ClipBlitToTarget: dst 矩形 = (posX,posY,srcW,srcH) ∩ 目标
            {
                const int posX = coord(rng), posY = coord(rng);
                const int srcW = size(rng), srcH = size(rng);
                int sx = -1, sy = -1, dx = -1, dy = -1, cw = -1, ch = -1;
                const bool vis = ClipBlitToTarget(posX, posY, srcW, srcH, targetW, targetH,
                                                  sx, sy, dx, dy, cw, ch);
                int bx, by, bw, bh;
                const bool bvis = BruteIntersect(posX, posY, srcW, srcH,
                                                 0, 0, targetW, targetH, bx, by, bw, bh);
                if (vis != bvis ||
                    (vis && (dx != bx || dy != by || cw != bw || ch != bh ||
                             sx != bx - posX || sy != by - posY))) {
                    ++fuzzFailures;
                    std::printf("FAIL: fuzz ClipBlitToTarget pos=(%d,%d) src=%dx%d\n",
                                posX, posY, srcW, srcH);
                }
            }
            // IntersectBlitWithDamage / IntersectRectWithDamage: dst 矩形 ∩ R,
            // 不变式: src-dst 偏移守恒
            {
                const int osx = coord(rng), osy = coord(rng);
                const int odx = coord(rng), ody = coord(rng);
                const int ocw = size(rng), och = size(rng);
                const int rx = coord(rng), ry = coord(rng);
                const int rw = size(rng), rh = size(rng);
                int sx = osx, sy = osy, dx = odx, dy = ody, cw = ocw, ch = och;
                const bool vis = IntersectBlitWithDamage(sx, sy, dx, dy, cw, ch,
                                                         rx, ry, rw, rh);
                int bx, by, bw, bh;
                const bool bvis = BruteIntersect(odx, ody, ocw, och, rx, ry, rw, rh,
                                                 bx, by, bw, bh);
                if (vis != bvis ||
                    (vis && (dx != bx || dy != by || cw != bw || ch != bh ||
                             sx - dx != osx - odx || sy - dy != osy - ody))) {
                    ++fuzzFailures;
                    std::printf("FAIL: fuzz IntersectBlitWithDamage iter=%d\n", iter);
                }
                int x = odx, y = ody, w = ocw, h = och;
                const bool rvis = IntersectRectWithDamage(x, y, w, h, rx, ry, rw, rh);
                if (rvis != bvis ||
                    (rvis && (x != bx || y != by || w != bw || h != bh))) {
                    ++fuzzFailures;
                    std::printf("FAIL: fuzz IntersectRectWithDamage iter=%d\n", iter);
                }
            }
            // ClipBlitSourceToRect: src 矩形 ∩ damage, 不变式: dst 同步偏移
            {
                const int osx = coord(rng), osy = coord(rng);
                const int odx = coord(rng), ody = coord(rng);
                const int ocw = size(rng), och = size(rng);
                const int rx = coord(rng), ry = coord(rng);
                const int rw = size(rng), rh = size(rng);
                int sx = osx, sy = osy, dx = odx, dy = ody, cw = ocw, ch = och;
                const bool vis = ClipBlitSourceToRect(sx, sy, dx, dy, cw, ch,
                                                      rx, ry, rw, rh);
                int bx, by, bw, bh;
                const bool bvis = BruteIntersect(osx, osy, ocw, och, rx, ry, rw, rh,
                                                 bx, by, bw, bh);
                if (vis != bvis ||
                    (vis && (sx != bx || sy != by || cw != bw || ch != bh ||
                             dx - odx != sx - osx || dy - ody != sy - osy))) {
                    ++fuzzFailures;
                    std::printf("FAIL: fuzz ClipBlitSourceToRect iter=%d\n", iter);
                }
            }
        }
        CHECK(fuzzFailures == 0, "fuzz vs brute force");
    }

    // -- IsFullyOpaqueArgb: ARGB 层透明性精确判定 (合成两条路径共用) --
    {
        // 全 255 alpha → 不透明
        std::vector<uint8_t> opaque(4 * 4 * 4, 0);
        for (size_t i = 3; i < opaque.size(); i += 4) opaque[i] = 255;
        CHECK(IsFullyOpaqueArgb(opaque.data(), 4, 4, 4), "all alpha=255 is opaque");

        // 任一像素 alpha != 255 → 非不透明
        std::vector<uint8_t> semi = opaque;
        semi[1 * 4 + 3] = 254;
        CHECK(!IsFullyOpaqueArgb(semi.data(), 4, 4, 4), "alpha=254 pixel breaks opaque");

        // 全透明 → 非不透明 (GL readback 未绘制区域的典型值)
        std::vector<uint8_t> clear(4 * 4 * 4, 0);
        CHECK(!IsFullyOpaqueArgb(clear.data(), 4, 4, 4), "all alpha=0 is not opaque");

        // stride > w: 只检内容区, 跨距填充不参与 (buffer 对齐 padding 场景)
        std::vector<uint8_t> strided(6 * 4 * 4, 0);  // stride=6, w=4, h=4
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) strided[(y * 6 + x) * 4 + 3] = 255;
        CHECK(IsFullyOpaqueArgb(strided.data(), 6, 4, 4),
              "stride padding outside content ignored");
        // 内容区外 (padding 区) 的 alpha=0 不应影响判定
        strided[(0 * 6 + 5) * 4 + 3] = 0;
        CHECK(IsFullyOpaqueArgb(strided.data(), 6, 4, 4),
              "padding alpha does not affect content verdict");

        // 边界/退化入参: 空指针 / stride<w / 非正尺寸 → false (防御)
        CHECK(!IsFullyOpaqueArgb(nullptr, 4, 4, 4), "null pixels is not opaque");
        CHECK(!IsFullyOpaqueArgb(opaque.data(), 3, 4, 4), "stride<w rejected");
        CHECK(!IsFullyOpaqueArgb(opaque.data(), 4, 0, 4), "zero width rejected");
        CHECK(!IsFullyOpaqueArgb(opaque.data(), 4, 4, -1), "negative height rejected");
    }

    std::printf("blit_clip_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

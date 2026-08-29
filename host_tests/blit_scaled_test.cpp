// BlitScaled (compositor_blit.cpp) 的宿主机单元测试 (make test)。
// 参考实现 blitScaledRef 是 2026-08-22 的"逐像素 4 采样"历史实现 (98b87ce
// 回退目标), 固化于此作为对比基准: 未来对 BlitScaled 的任何改写 (分离
// 插值/NEON/裁剪相位改动) 都必须与之逐位一致; 黄金值用例验证混合/clip 语义。
#include "compositor/frame/compositor_blit.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <random>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

// -- 参考实现: 旧版 (2026-08-22 BlitScaled 分离化前的逐像素 4 采样) --
static void blitScaledRef(uint8_t* dst, int rootW, int rootH,
                          const uint8_t* src, int srcStride, int srcW, int srcH,
                          int dstX, int dstY, int dstW, int dstH, bool alphaBlend,
                          int clipX, int clipY, int clipW, int clipH)
{
    if (!dst || !src || srcStride <= 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    if (srcW > srcStride) srcW = srcStride;
    int x0 = std::max(0, dstX), y0 = std::max(0, dstY);
    int x1 = std::min(rootW, dstX + dstW), y1 = std::min(rootH, dstY + dstH);
    if (clipW > 0 && clipH > 0) {
        x0 = std::max(x0, clipX); y0 = std::max(y0, clipY);
        x1 = std::min(x1, clipX + clipW); y1 = std::min(y1, clipY + clipH);
    }
    if (x1 <= x0 || y1 <= y0) return;
    const int64_t stepX = (static_cast<int64_t>(srcW) << 16) / dstW;
    const int64_t stepY = (static_cast<int64_t>(srcH) << 16) / dstH;
    const int64_t maxFx = static_cast<int64_t>(srcW - 1) << 16;
    const int64_t maxFy = static_cast<int64_t>(srcH - 1) << 16;
    std::vector<int> sx0(x1 - x0), sx1(x1 - x0), wx0(x1 - x0), wx1(x1 - x0);
    for (int i = 0; i < x1 - x0; ++i) {
        int64_t fx = static_cast<int64_t>(x0 + i - dstX) * stepX + (stepX >> 1) - (1 << 15);
        fx = std::max<int64_t>(0, std::min(maxFx, fx));
        sx0[i] = static_cast<int>(fx >> 16);
        sx1[i] = std::min(sx0[i] + 1, srcW - 1);
        wx1[i] = static_cast<int>((fx >> 8) & 0xFF);
        wx0[i] = 256 - wx1[i];
    }
    for (int y = y0; y < y1; ++y) {
        int64_t fy = static_cast<int64_t>(y - dstY) * stepY + (stepY >> 1) - (1 << 15);
        fy = std::max<int64_t>(0, std::min(maxFy, fy));
        const int sy = static_cast<int>(fy >> 16);
        const int sy1 = std::min(sy + 1, srcH - 1);
        const unsigned wy1 = static_cast<unsigned>((fy >> 8) & 0xFF);
        const unsigned wy0 = 256 - wy1;
        const uint8_t* row0 = src + static_cast<size_t>(sy) * srcStride * 4;
        const uint8_t* row1 = src + static_cast<size_t>(sy1) * srcStride * 4;
        uint8_t* drow = dst + (static_cast<size_t>(y) * rootW + x0) * 4;
        for (int i = 0; i < x1 - x0; ++i) {
            const uint8_t* p00 = row0 + sx0[i] * 4;
            const uint8_t* p01 = row0 + sx1[i] * 4;
            const uint8_t* p10 = row1 + sx0[i] * 4;
            const uint8_t* p11 = row1 + sx1[i] * 4;
            const unsigned w00 = static_cast<unsigned>(wx0[i]) * wy0;
            const unsigned w01 = static_cast<unsigned>(wx1[i]) * wy0;
            const unsigned w10 = static_cast<unsigned>(wx0[i]) * wy1;
            const unsigned w11 = static_cast<unsigned>(wx1[i]) * wy1;
            uint8_t* dpx = drow + i * 4;
            const unsigned b = (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) >> 16;
            const unsigned g = (p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) >> 16;
            const unsigned r = (p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) >> 16;
            if (!alphaBlend) {
                dpx[0] = static_cast<uint8_t>(b);
                dpx[1] = static_cast<uint8_t>(g);
                dpx[2] = static_cast<uint8_t>(r);
                dpx[3] = 255;
                continue;
            }
            const unsigned a = (p00[3] * w00 + p01[3] * w01 + p10[3] * w10 + p11[3] * w11) >> 16;
            if (a == 0) continue;
            if (a >= 255) {
                dpx[0] = static_cast<uint8_t>(b);
                dpx[1] = static_cast<uint8_t>(g);
                dpx[2] = static_cast<uint8_t>(r);
                dpx[3] = 255;
            } else {
                const unsigned inv = 255 - a;
                const unsigned nb = b + (dpx[0] * inv) / 255;
                const unsigned ng = g + (dpx[1] * inv) / 255;
                const unsigned nr = r + (dpx[2] * inv) / 255;
                dpx[0] = static_cast<uint8_t>(std::min(nb, 255u));
                dpx[1] = static_cast<uint8_t>(std::min(ng, 255u));
                dpx[2] = static_cast<uint8_t>(std::min(nr, 255u));
                dpx[3] = 255;
            }
        }
    }
}

int main()
{
    // 1. 随机对比: 新/旧实现逐位一致 (含放大/缩小/错位/裁剪/混合与不混合)
    std::mt19937 rng(20260822);
    for (int iter = 0; iter < 400; ++iter) {
        const int srcW = 3 + (int)(rng() % 12);
        const int srcH = 3 + (int)(rng() % 12);
        const int rootW = 8 + (int)(rng() % 24);
        const int rootH = 8 + (int)(rng() % 24);
        const int dstW = 2 + (int)(rng() % 20);
        const int dstH = 2 + (int)(rng() % 20);
        const int dstX = -3 + (int)(rng() % 10);
        const int dstY = -3 + (int)(rng() % 10);
        const bool alphaBlend = (rng() % 2) != 0;
        const int stride = srcW + (int)(rng() % 3);  // 含 padding, 练习 stride!=w
        std::vector<uint8_t> src((size_t)stride * srcH * 4);
        for (size_t i = 0; i < src.size(); ++i) src[i] = (uint8_t)(rng() & 0xFF);
        std::vector<uint8_t> outA((size_t)rootW * rootH * 4), outB((size_t)rootW * rootH * 4);
        for (int i = 0; i < (int)outA.size(); ++i) outA[i] = outB[i] = (uint8_t)(rng() & 0xFF);
        std::vector<uint8_t> baseA = outA, baseB = outB;
        BlitScaled(outA.data(), rootW, rootH, src.data(), stride, srcW, srcH,
                   dstX, dstY, dstW, dstH, alphaBlend);
        blitScaledRef(outB.data(), rootW, rootH, src.data(), stride, srcW, srcH,
                      dstX, dstY, dstW, dstH, alphaBlend, 0, 0, 0, 0);
        if (outA != outB) {
            ++g_failures; ++g_checks;
            // 找第一处差异明细 (只报一次)
            size_t diff = 0;
            while (diff < outA.size() && outA[diff] == outB[diff]) ++diff;
            std::printf("FAIL: iter=%d src=%dx%d root=%dx%d dst=(%d,%d %dx%d) alpha=%d "
                        "first diff @%zu: new=%d ref=%d\n",
                        iter, srcW, srcH, rootW, rootH, dstX, dstY, dstW, dstH,
                        alphaBlend ? 1 : 0, diff, outA[diff], outB[diff]);
            continue;
        }
        ++g_checks;
    }

    // 2. 黄金值: 2x2 纯色放大 → 无色差
    {
        const uint8_t src[2 * 2 * 4] = {
            0x55, 0xEE, 0x99, 0xFF,  0x55, 0xEE, 0x99, 0xFF,
            0x55, 0xEE, 0x99, 0xFF,  0x55, 0xEE, 0x99, 0xFF };
        uint8_t out[6 * 6 * 4];
        std::memset(out, 0xAA, sizeof(out));
        BlitScaled(out, 6, 6, src, 2, 2, 2, 1, 1, 4, 4, false /* no alpha blend */);
        for (int y = 1; y < 5; ++y)
            for (int x = 1; x < 5; ++x) {
                const uint8_t* p = out + (size_t)(y * 6 + x) * 4;
                if (p[0] != 0x55 || p[1] != 0xEE || p[2] != 0x99 || p[3] != 0xFF) {
                    ++g_failures; ++g_checks;
                    std::printf("FAIL: solid fill pixel (%d,%d) mismatch\n", x, y);
                    break;
                }
            }
        ++g_checks;
    }

    // 3. clip 黄金值: 只写入 clip 区域, 其它保持哨兵
    {
        uint8_t src[4 * 4] = { 0x10,0x20,0x30,0xFF, 0x10,0x20,0x30,0xFF,
                               0x10,0x20,0x30,0xFF, 0x10,0x20,0x30,0xFF };
        uint8_t out[8 * 8 * 4];
        std::memset(out, 0xAA, sizeof(out));
        BlitScaled(out, 8, 8, src, 2, 2, 2, 2, 2, 2, 2, false, 2, 2, 2, 2);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const bool inClip = (x >= 2 && x < 4 && y >= 2 && y < 4);
                const uint8_t expect = inClip ? 0xFF : 0xAA;  // 哨兵 0xAA
                if (out[(size_t)(y * 8 + x) * 4 + 3] != expect) {
                    ++g_failures; ++g_checks;
                    std::printf("FAIL: clip sentinel (%d,%d)\n", x, y);
                }
            }
        ++g_checks;
    }

    std::printf("blit_scaled_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

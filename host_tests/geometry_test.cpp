// compositor/geometry 纯函数的宿主机单元测试 (make test)。
// 不依赖 wayland/OHOS SDK, 用宿主 g++ 编译, 目的是把坐标换算这种
// 历史重灾区逻辑 (全屏/黑边鼠标映射) 变成可离线验证的纯函数。
#include "compositor/geometry.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

static bool near(double a, double b, double eps) { return std::fabs(a - b) < eps; }

int main()
{
    // 1. 同宽高比: 无缩放无黑边
    {
        FitRect t;
        CHECK(ComputeFitRect(1920, 1080, 1920, 1080, t), "same aspect ok");
        CHECK(near(t.scale, 1.0, 1e-12), "same aspect scale=1");
        CHECK(t.dstW == 1920 && t.dstH == 1080 && t.offX == 0 && t.offY == 0, "same aspect fill");
        CHECK(t.srcW == 1920 && t.srcH == 1080, "src recorded");
    }

    // 2. 宽内容 → 高显示区: 上下黑边 (letterbox)
    {
        FitRect t;
        CHECK(ComputeFitRect(1080, 1920, 1280, 720, t), "letterbox ok");
        CHECK(t.dstW == 1080, "letterbox width bound");
        CHECK(t.offX == 0, "letterbox no x offset");
        CHECK(t.dstH > 0 && t.dstH < 1920, "letterbox dstH shrunk");
        CHECK(t.offY == (1920 - t.dstH) / 2, "letterbox centered");
    }

    // 3. 高内容 → 宽显示区: 左右黑边 (pillarbox)
    {
        FitRect t;
        CHECK(ComputeFitRect(1920, 1080, 720, 1280, t), "pillarbox ok");
        CHECK(t.dstH == 1080 && t.offY == 0, "pillarbox height bound");
        CHECK(t.offX == (1920 - t.dstW) / 2, "pillarbox centered");
    }

    // 4. 零/负尺寸防御: 计算拒绝 + 映射函数不除零
    {
        FitRect t;
        CHECK(!ComputeFitRect(0, 100, 100, 100, t), "zero rootW rejected");
        CHECK(!ComputeFitRect(100, 0, 100, 100, t), "zero rootH rejected");
        CHECK(!ComputeFitRect(100, 100, 0, 100, t), "zero winW rejected");
        CHECK(!ComputeFitRect(100, 100, 100, -1, t), "negative winH rejected");
        FitRect z{};
        CHECK(FitMapDisplayX(z, 10) == 0 && FitMapDisplayY(z, 10) == 0, "zero rect map safe");
        CHECK(FitUnmapDisplayX(z, 10) == 0.0 && FitUnmapDisplayY(z, 10) == 0.0, "zero rect unmap safe");
        CHECK(FitSizeDisplayW(z, 10) == 0 && FitSizeDisplayH(z, 10) == 0, "zero rect size safe");
    }

    // 5. 极端宽高比: dst 至少 1px
    {
        FitRect t;
        CHECK(ComputeFitRect(100, 100, 10000, 1, t), "extreme aspect ok");
        CHECK(t.dstW == 100 && t.dstH == 1, "extreme aspect clamped to 1px");
    }

    // 6. 正/逆映射互逆 (取整 dst 变体, 整数截断允许 <1px 误差)
    {
        FitRect t;
        ComputeFitRect(2800, 1840, 1400, 920, t);
        for (int64_t x : {0, 1, 700, 1399}) {
            CHECK(near(FitUnmapDisplayX(t, FitMapDisplayX(t, x)), x, 1.0), "display map/unmap X ~1px");
        }
        for (int64_t y : {0, 1, 460, 919}) {
            CHECK(near(FitUnmapDisplayY(t, FitMapDisplayY(t, y)), y, 1.0), "display map/unmap Y ~1px");
        }
    }

    // 7. dst 尺寸用 lround 而非截断: 720 * (1000/1280) = 562.5 → 563
    {
        FitRect t;
        ComputeFitRect(1000, 1000, 1280, 720, t);
        CHECK(t.dstH == 563, "dst rounding is lround, not trunc");
    }

    // 8. 全屏内容尺寸选择 (ZC 游戏 zero-copy 层几何 / SHM 游戏 buffer)
    {
        int w = 0, h = 0;
        // ZC 游戏: 画面在 zero-copy 层, 内容 = 层实际几何 (与渲染视口同源,
        // 修复 preFs 快照与层几何失配导致的光标常数平移偏移)
        SelectFullscreenContentSize(640, 480, 1400, 920, true, w, h);
        CHECK(w == 640 && h == 480, "ZC game uses layer geometry");
        // ZC 层几何与 buffer 同尺寸 (窗口化全屏): 等价, 用 layer (== buffer)
        SelectFullscreenContentSize(1400, 920, 1400, 920, true, w, h);
        CHECK(w == 1400 && h == 920, "layer==buffer uses layer");
        // SHM 游戏: buffer 即画面, 填满整个 buffer → buffer 尺寸
        SelectFullscreenContentSize(640, 480, 1400, 920, false, w, h);
        CHECK(w == 1400 && h == 920, "SHM game uses buffer");
        // layer 几何未就绪 (0): 退化用 buffer
        SelectFullscreenContentSize(0, 0, 1400, 920, true, w, h);
        CHECK(w == 1400 && h == 920, "no layer geometry uses buffer");
        // layer 部分无效: 恒用 buffer
        SelectFullscreenContentSize(640, 0, 1400, 920, true, w, h);
        CHECK(w == 1400 && h == 920, "half-valid layer uses buffer");
    }

    // 9. A 1280x800 virtual frame fits a 1280x720 panel without distortion.
    {
        FitRect t;
        CHECK(ComputeFitRect(1280, 720, 1280, 800, t), "1280x800 virtual mode fits 720p");
        CHECK(near(t.scale, 0.9, 1e-12), "1280x800 to 720p scale=0.9");
        CHECK(t.dstW == 1152 && t.dstH == 720, "1280x800 keeps 16:10 aspect");
        CHECK(t.offX == 64 && t.offY == 0, "1280x800 is centered with side bars");
        CHECK(FitMapDisplayX(t, 640) == 640 && FitMapDisplayY(t, 400) == 360,
              "1280x800 center maps to panel center");
        CHECK(near(FitUnmapDisplayX(t, 640), 640.0, 1e-12) &&
              near(FitUnmapDisplayY(t, 360), 400.0, 1e-12),
              "720p input maps back to virtual coordinates");
    }

    // 10. vpDst 显示尺寸 (重构阶段 1 特征化: 锁定 11 处手写三元式的两种
    // 现有语义变体, 先写测试再做替换 — 行为平价, 不统一变体)
    {
        // 不 clamp 变体: vpDst<=0 (0 或负数, -1=未设置) 回退 buffer 尺寸
        CHECK(DisplaySizeAfterViewport(0, 640) == 640, "vp size: 0 falls back to w");
        CHECK(DisplaySizeAfterViewport(-1, 640) == 640, "vp size: negative falls back to w");
        // vpDst < w: 按 vpDst 显示
        CHECK(DisplaySizeAfterViewport(600, 640) == 600, "vp size: vpDst<w uses vpDst");
        // vpDst > w: 直通不截断 (buffer 对齐填充/放大显示场景)
        CHECK(DisplaySizeAfterViewport(800, 640) == 800, "vp size: vpDst>w passes through");

        // min clamp 变体: 回退行为相同
        CHECK(DisplaySizeAfterViewportClamped(0, 640) == 640, "vp size clamped: 0 falls back to w");
        CHECK(DisplaySizeAfterViewportClamped(-1, 640) == 640, "vp size clamped: negative falls back to w");
        // vpDst < w: 按 vpDst 显示 (min 不生效)
        CHECK(DisplaySizeAfterViewportClamped(600, 640) == 600, "vp size clamped: vpDst<w uses vpDst");
        // vpDst > w: 截断到 buffer 尺寸 (blit 源只有 buffer, 无像素可放大)
        CHECK(DisplaySizeAfterViewportClamped(800, 640) == 640, "vp size clamped: vpDst>w truncated");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

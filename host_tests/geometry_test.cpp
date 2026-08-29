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

    // 11. ClampToContent (重构第 4A 步自 input_manager.cpp file-static 收进
    // geometry.h 的内容区钳制 — 全屏 letterbox 黑边/拖出窗口边缘的注入前钳位,
    // 防相对增量差分累积黑边里的幽灵位移)
    {
        // content<=0: 无内容尺寸信息 (非全屏目标), 不钳制 (交给 winewayland clamp)
        CHECK(near(ClampToContent(356.7, 0), 356.7, 1e-12), "clamp: content=0 no-op");
        CHECK(near(ClampToContent(-12.0, -1), -12.0, 1e-12), "clamp: negative content no-op");
        // 正常钳 [0, content-1]
        CHECK(near(ClampToContent(-3.0, 800), 0.0, 1e-12), "clamp: negative to 0");
        CHECK(near(ClampToContent(799.0, 800), 799.0, 1e-12), "clamp: inside unchanged");
        CHECK(near(ClampToContent(801.0, 800), 799.0, 1e-12), "clamp: overflow to content-1");
        CHECK(near(ClampToContent(0.0, 800), 0.0, 1e-12), "clamp: zero ok");
        // content=1: 值域退化 [0,0] (黑边全沿被钳死, 增量恒 0)
        CHECK(near(ClampToContent(5.0, 1), 0.0, 1e-12), "clamp: content=1 collapses to 0");
        CHECK(near(ClampToContent(-5.0, 1), 0.0, 1e-12), "clamp: content=1 negative to 0");
    }

    // 12. ComputeLocalPoint (重构第 4A 步新增: InputResolver 命中终态的
    // 桌面逻辑坐标 → surface 局部坐标逆映射, 与合成侧 FitMapX 正变换严格互逆)
    {
        double lx, ly;
        // 恒等: 普通窗口原点 (100,200), scale=1, content 无效 → 平移不钳
        ComputeLocalPoint(120.0, 230.0, 100.0, 200.0, 1.0, 0, 0, lx, ly);
        CHECK(near(lx, 20.0, 1e-9) && near(ly, 30.0, 1e-9), "local: identity shift");
        // 全屏保比例放大 (int 缩放 2): origin=(64,0), 内容 800x600
        ComputeLocalPoint(64.0 + 400.0 * 2.0, 0.0 + 300.0 * 2.0,
                          64.0, 0.0, 2.0, 800, 600, lx, ly);
        CHECK(near(lx, 400.0, 1e-9) && near(ly, 300.0, 1e-9), "local: fit inverse");
        // 黑边 (swallow MOVE/RELEASE 透传语义): 内容区外坐标钳到 [0, content-1]
        ComputeLocalPoint(0.0, 0.0, 64.0, 0.0, 2.0, 800, 600, lx, ly);
        CHECK(near(lx, 0.0, 1e-9) && near(ly, 0.0, 1e-9), "local: letterbox clamped to edge");
        ComputeLocalPoint(64.0 - 40.0, -20.0, 64.0, 0.0, 2.0, 800, 600, lx, ly);
        CHECK(near(lx, 0.0, 1e-9) && near(ly, 0.0, 1e-9), "local: letterbox negative clamped");
        // 非整数缩放: 与 FitMapX 正变换 (未取整 double scale) 逐点互逆
        {
            FitRect t;
            CHECK(ComputeFitRect(1400, 920, 800, 600, t), "fit rect for inverse check");
            CHECK(near(t.scale, 920.0 / 600.0, 1e-12), "fit scale = min ratio");
            for (double e : {0.0, 100.0, 399.5, 799.0}) {
                double dx = FitMapX(t, e);
                ComputeLocalPoint(dx, 0.0, t.offX, t.offY, t.scale, 800, 600, lx, ly);
                CHECK(near(lx, e, 1e-6), "local: fit inverse roundtrip");
            }
        }
    }

    // 13. 4A 精度对账: InputTarget scale 由旧 float 截断改 double 未取整
    // (与 FitRect scale 同源同精度)。对账: Δlocal ≤ |local|·2^-23 (float
    // 表示误差上界的宽松界), 远小于 wl_fixed 半格 (1/512); 即注入坐标最多
    // 差 1 个 fixed 单位且仅当 local 落入 1/256 格边界附近 — 方向为消除
    // 旧 float 截断误差 (FitMapX 未取整 scale 严格互逆), 无语义变化
    {
        FitRect t;
        CHECK(ComputeFitRect(1400, 920, 800, 600, t), "fit rect for 4A prec check");
        const double scaleNew = t.scale;
        const double scaleOld = static_cast<double>(static_cast<float>(t.scale));
        CHECK(scaleNew != scaleOld, "pre-4A float truncation is lossy");
        double maxDev = 0.0;
        for (double e = 0.0; e <= 799.0; e += 37.5) {
            const double dx = t.offX + e * scaleOld;  // 旧字段值换算出的桌面坐标
            const double localNew = (dx - t.offX) / scaleNew;  // 4A 后路径
            maxDev = std::max(maxDev, std::fabs(localNew - e));
        }
        CHECK(maxDev < 800.0 * (1.0 / (1.0 * (1 << 23))), "4A dev bounded by float eps");
        CHECK(maxDev < 0.5 / 256.0, "4A dev below wl_fixed half-step");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

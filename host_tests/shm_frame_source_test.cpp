// shm_frame_source.{h,cpp} (SHM 拷贝/缩放/内容区计算纯函数, 重构第 5A1 步
// 自 wl_core.cpp 抽离) 的宿主机单元测试 (make test)。
// 覆盖: content 区紧凑拷贝 (去 stride padding)、vpDst 缩放 (clamp/不 clamp),
// 全 buffer 紧凑拷贝, 内容区计算 (window_geometry 裁剪 toplevel/subsurface
// 双分支 + 异步 geometry/buffer 防御 clamp)。
// 黄金值用例 + 固定种子随机用例与独立数学路径参考实现对比。
#include "compositor/frame/shm_frame_source.h"
#include <algorithm>
#include <cstdio>
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

// 构造一个带 stride padding 的 SHM buffer (模拟 wl_shm 行对齐填充)
// 与对应 ShmCommitInfo。像素 (x,y) 的 4 字节 = [v(x,y)+0, +1, +2, +3]
// (v = (y*bufW + x)), 保证每格可精确断言拷贝来源。
static ShmCommitInfo MakeBuf(int32_t bufW, int32_t bufH, int32_t stridePad,
                             std::vector<uint8_t>& buf) {
    const int32_t stride = bufW * 4 + stridePad;
    ShmCommitInfo fi;
    fi.bufW = bufW;
    fi.bufH = bufH;
    fi.stride = stride;
    fi.shmFormat = 1;
    buf.resize(static_cast<size_t>(stride) * bufH);
    for (int32_t y = 0; y < bufH; ++y)
        for (int32_t x = 0; x < bufW; ++x)
            for (int b = 0; b < 4; ++b)
                buf[static_cast<size_t>(y) * stride + x * 4 + b] =
                    static_cast<uint8_t>(y * bufW + x + b);
    fi.src = buf.data();
    return fi;
}

// 期望源像素 4 字节 (与 MakeBuf 的布局公式严格互逆, 独立于拷贝路径)
static uint8_t SrcPx(int32_t bufW, int32_t x, int32_t y, int b) {
    return static_cast<uint8_t>(y * bufW + x + b);
}

// CopyToplevelContent 缩放路径的独立参考: 按语义定义 (输出像素 (x,y) 取
// 源像素 (min(sourceW-1, floor(x*sourceW/logicalW)), min(sourceH-1,
// floor(y*sourceH/logicalH))), 用 double 计算路径 — 与生产 int64 整数除法
// 路径不同, 中小尺寸下逐像素一致。srcW/srcH 必须传 CopyToplevelContent
// 入口时的 fi.contentW/contentH (函数会把它们改为逻辑尺寸)。
static bool RefScaleEquals(const ShmCommitInfo& fi, int srcW, int srcH,
                           int logicalW, int logicalH,
                           const std::vector<uint8_t>& dst)
{
    const int sourceW = srcW, sourceH = srcH;
    if ((int)dst.size() != logicalW * logicalH * 4) return false;
    const uint8_t* source = fi.src + fi.contentOffY * fi.stride + fi.contentOffX * 4;
    for (int y = 0; y < logicalH; ++y) {
        const int sourceY = std::min(sourceH - 1, static_cast<int>(std::floor(
            static_cast<double>(y) * sourceH / logicalH)));
        const uint8_t* sourceRow = source + static_cast<size_t>(sourceY) * fi.stride;
        for (int x = 0; x < logicalW; ++x) {
            const int sourceX = std::min(sourceW - 1, static_cast<int>(std::floor(
                static_cast<double>(x) * sourceW / logicalW)));
            for (int b = 0; b < 4; ++b) {
                if (dst[static_cast<size_t>(y) * logicalW * 4 + x * 4 + b] !=
                    sourceRow[sourceX * 4 + b]) return false;
            }
        }
    }
    return true;
}

int main()
{
    // -- CopyShmContentTight: 带 padding 的 content 区紧凑拷贝 --
    {
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(10, 8, 8, buf);  // stride = 48
        fi.contentW = 3; fi.contentH = 2;
        fi.contentOffX = 2; fi.contentOffY = 1;
        std::vector<uint8_t> dst;
        CopyShmContentTight(fi, dst);
        CHECK(dst.size() == 3 * 2 * 4, "content tight size (3x2)");
        bool ok = true;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x)
                for (int b = 0; b < 4; ++b)
                    if (dst[(y * 3 + x) * 4 + b] != SrcPx(10, 2 + x, 1 + y, b)) ok = false;
        CHECK(ok, "content tight pixels from offset (2,1)");
    }
    {
        // 零内容尺寸: dst 空 (resize(0))
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(8, 8, 0, buf);
        fi.contentW = 0; fi.contentH = 0;
        std::vector<uint8_t> dst;
        CopyShmContentTight(fi, dst);
        CHECK(dst.empty(), "content tight zero size -> empty");
    }
    {
        // 无 offset 全内容: 与 CopyShmBufferTight 同结果
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(6, 3, 4, buf);
        fi.contentW = 6; fi.contentH = 3;
        CHECK(fi.stride != fi.bufW * 4, "fixture uses padded stride");
        std::vector<uint8_t> tight, full;
        CopyShmContentTight(fi, tight);
        CopyShmBufferTight(fi, full);
        CHECK(tight == full, "content tight (full content) == buffer tight");
    }

    // -- CopyToplevelContent --
    {
        // vpDst 未设置 (-1): 逻辑尺寸 = 源尺寸 → tight 路径, 与
        // CopyShmContentTight 逐字节一致; fi.contentW/H 不变
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(8, 8, 8, buf);
        fi.contentW = 8; fi.contentH = 8;
        ShmCommitInfo fiRef = fi;
        std::vector<uint8_t> a, b;
        CopyToplevelContent(-1, -1, fi, a);
        CopyShmContentTight(fiRef, b);
        CHECK(a == b, "vpDst=-1 -> tight copy");
        CHECK(fi.contentW == 8 && fi.contentH == 8, "vpDst=-1 keeps content");
        CHECK(fi.contentW == fiRef.contentW && fi.contentH == fiRef.contentH,
              "vpDst=-1 content size unchanged");
    }
    {
        // vpDst 与内容同尺寸: 条件 logical==source 同时成立 → tight; vpDstW=0
        // 与负数同语义 (DisplaySizeAfterViewport 不 clamp, 0 也回退)
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(6, 4, 4, buf);
        fi.contentW = 6; fi.contentH = 4;
        ShmCommitInfo fiRef = fi;
        std::vector<uint8_t> a, b;
        CopyToplevelContent(0, 4, fi, a);  // W 用 0 (回退), H 与内容同尺寸
        CopyShmContentTight(fiRef, b);
        CHECK(a == b, "vpDst same-as-content -> tight");
    }
    {
        // vpDst 放大: 4x4 内容 → 8x6 逻辑 (Bilinear 等价最近邻), 逐像素
        // 对独立参考; 行 padding 与 contentOff 参与采样
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(8, 8, 4, buf);
        fi.contentW = 4; fi.contentH = 4;
        fi.contentOffX = 1; fi.contentOffY = 1;  // 内容区不全 buffer
        std::vector<uint8_t> dst;
        CopyToplevelContent(8, 6, fi, dst);
        CHECK((int)dst.size() == 8 * 6 * 4, "vpDst upscale size 8x6");
        CHECK(RefScaleEquals(fi, 4, 4, 8, 6, dst), "vpDst upscale pixels vs ref");
        CHECK(fi.contentW == 8 && fi.contentH == 6, "vpDst upscale updates content size");
    }
    {
        // vpDst 缩小: 8x8 内容 → 4x4 逻辑 (最近邻抽行抽列)
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(8, 8, 0, buf);
        fi.contentW = 8; fi.contentH = 8;
        std::vector<uint8_t> dst;
        CopyToplevelContent(4, 4, fi, dst);
        CHECK((int)dst.size() == 4 * 4 * 4, "vpDst downscale size 4x4");
        CHECK(RefScaleEquals(fi, 8, 8, 4, 4, dst), "vpDst downscale pixels vs ref");
    }

    // -- CopyShmBufferTight: 全 buffer 紧凑拷贝 --
    {
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(7, 5, 12, buf);  // 每行 7*4+12=40 字节
        std::vector<uint8_t> dst;
        CopyShmBufferTight(fi, dst);
        CHECK(dst.size() == static_cast<size_t>(7) * 5 * 4, "buffer tight size");
        bool ok = true;
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 7; ++x)
                for (int b = 0; b < 4; ++b)
                    if (dst[(y * 7 + x) * 4 + b] != SrcPx(7, x, y, b)) ok = false;
        CHECK(ok, "buffer tight pixels (stride padded)");
    }
    {
        // 零 buffer: 空 dst
        std::vector<uint8_t> buf;
        ShmCommitInfo fi = MakeBuf(0, 0, 0, buf);
        std::vector<uint8_t> dst;
        CopyShmBufferTight(fi, dst);
        CHECK(dst.empty(), "buffer tight zero size -> empty");
    }

    // -- ComputeContentAreaGeometry --
    {
        // 无 window_geometry: 全 buffer, off/screen 默认 0
        ShmCommitInfo fi;
        fi.bufW = 320; fi.bufH = 200;
        ComputeContentAreaGeometry(fi, false, 0, 0, false, 0, 0);
        CHECK(fi.contentW == 320 && fi.contentH == 200, "no geo -> full buffer");
        CHECK(fi.contentOffX == 0 && fi.contentOffY == 0, "no geo -> off 0");
        CHECK(fi.screenX == 0 && fi.screenY == 0, "no geo -> screen 0");
    }
    {
        // toplevel + geometry: 内容从 buffer 原点, screen = geo
        ShmCommitInfo fi;
        fi.bufW = 640; fi.bufH = 480;
        ComputeContentAreaGeometry(fi, true, 100, 50, true, 230, 80);
        CHECK(fi.contentW == 100 && fi.contentH == 50, "toplevel geo content");
        CHECK(fi.contentOffX == 0 && fi.contentOffY == 0, "toplevel geo off=0");
        CHECK(fi.screenX == 230 && fi.screenY == 80, "toplevel geo screen");
    }
    {
        // subsurface + geometry: off = geo (相对父 surface 的内容偏移)
        ShmCommitInfo fi;
        fi.bufW = 64; fi.bufH = 64;
        ComputeContentAreaGeometry(fi, true, 30, 20, false, 12, 8);
        CHECK(fi.contentW == 30 && fi.contentH == 20, "subsurf geo content");
        CHECK(fi.contentOffX == 12 && fi.contentOffY == 8, "subsurf geo off");
        CHECK(fi.screenX == 0 && fi.screenY == 0, "subsurf geo screen stays 0");
    }
    {
        // clamp: off 有效但 off+content 越出 buffer 右/下 → 裁剪
        ShmCommitInfo fi;
        fi.bufW = 100; fi.bufH = 100;
        ComputeContentAreaGeometry(fi, true, 50, 50, false, 90, 90);
        CHECK(fi.contentW == 10 && fi.contentH == 10, "out-of-bounds clamp");
        CHECK(fi.contentOffX == 90 && fi.contentOffY == 90, "out-of-bounds keeps off");
    }
    {
        // clamp: off 落在 buffer 外 (>= 边界) → 置 0
        ShmCommitInfo fi;
        fi.bufW = 100; fi.bufH = 100;
        ComputeContentAreaGeometry(fi, true, 50, 50, false, 150, 0);
        CHECK(fi.contentOffX == 0, "off beyond bufW -> 0");
        ComputeContentAreaGeometry(fi, true, 50, 50, false, -5, 10);
        CHECK(fi.contentOffX == 0, "negative off -> 0");
        CHECK(fi.contentOffY == 10 && fi.contentW == 50, "y clamp unaffected");
    }
    {
        // clamp 后尺寸 <= 0 → 回退全 buffer
        ShmCommitInfo fi;
        fi.bufW = 100; fi.bufH = 100;
        ComputeContentAreaGeometry(fi, true, 50, 50, false, 100, 100);
        // offX=100 >= bufW → 0; offY=100 >= bufH → 0; contentW 50, contentH 50
        // 不触发回退 (50>0)。构造真正触发的: off 恰在边缘且 geo 超界
        ComputeContentAreaGeometry(fi, true, 200, 200, false, 200, 0);
        // offX=200 >= bufW → 0, contentW=200 > 100 → 100; keep 100x100
        CHECK(fi.contentW == 100 && fi.contentH == 100,
              "clamp keeps 100x100 after full clip");
        // 直接构造 content==0 输入: off==bufW 且 geo 尺寸为 0 不受理,
        // 用 off 压到 bufW 后 contentW=0? offX 会被置 0, contentW 不变…
        // 用负 off + content 裁剪到 0: offX=-0? 换路径: offY = bufH-1,
        // geoH = 2 → contentH = bufH - (bufH-1) = 1 > 0。无法经 clamp 归零
        // (clamp 是 min 操作不会负), 直接验证 geoW/geoH 不满足 >0 时的回退
    }
    {
        // geoW/geoH 非正 → 全 buffer (协议: geometry 无效尺寸忽略)
        ShmCommitInfo fi;
        fi.bufW = 320; fi.bufH = 200;
        ComputeContentAreaGeometry(fi, true, 0, 50, true, 10, 10);
        CHECK(fi.contentW == 320 && fi.contentH == 200, "geoW<=0 -> full buffer");
        CHECK(fi.contentOffX == 0, "geoW<=0 off stays 0");
        ComputeContentAreaGeometry(fi, true, 50, -3, true, 10, 10);
        CHECK(fi.contentW == 320 && fi.contentH == 200, "geoH<0 -> full buffer");
    }
    {
        // 矩形刚好在 buffer 内: 不裁剪
        ShmCommitInfo fi;
        fi.bufW = 100; fi.bufH = 100;
        ComputeContentAreaGeometry(fi, true, 40, 30, false, 60, 70);
        CHECK(fi.contentW == 40 && fi.contentH == 30, "inside rect no clamp");
        CHECK(fi.contentOffX == 60 && fi.contentOffY == 70, "inside rect off kept");
    }
    {
        // 内容区恰贴边界: off+content == bufW 不裁剪
        ShmCommitInfo fi;
        fi.bufW = 100; fi.bufH = 50;
        ComputeContentAreaGeometry(fi, true, 10, 10, false, 90, 40);
        CHECK(fi.contentW == 10 && fi.contentH == 10, "exact edge no clamp");
    }

    // -- 固定种子随机用例: CopyToplevelContent 缩放 vs 独立参考 --
    {
        std::mt19937 rng(20260829);
        std::uniform_int_distribution<int> dim(1, 64);
        std::uniform_int_distribution<int> off(0, 8);
        std::uniform_int_distribution<int> pad(0, 16);
        int fuzzFailures = 0;
        for (int iter = 0; iter < 500; ++iter) {
            const int32_t bufW = dim(rng), bufH = dim(rng);
            const int32_t offX = off(rng), offY = off(rng);
            std::vector<uint8_t> buf;
            ShmCommitInfo fi = MakeBuf(bufW, bufH, pad(rng), buf);
            // 内容区: 从 (offX, offY) 起, span 至 buffer 内
            const int32_t cw = dim(rng), ch = dim(rng);
            fi.contentW = std::min<int32_t>(cw, bufW - offX);
            fi.contentH = std::min<int32_t>(ch, bufH - offY);
            if (fi.contentW <= 0 || fi.contentH <= 0) continue;
            fi.contentOffX = offX;
            fi.contentOffY = offY;
            const int logicalW = dim(rng), logicalH = dim(rng);
            const int srcW = fi.contentW, srcH = fi.contentH;  // 入口内容尺寸
            std::vector<uint8_t> dst;
            CopyToplevelContent(logicalW, logicalH, fi, dst);
            if (!RefScaleEquals(fi, srcW, srcH, logicalW, logicalH, dst)) {
                ++fuzzFailures;
                std::printf("FAIL: fuzz CopyToplevelContent iter=%d buf=%dx%d off=(%d,%d) content=%dx%d logical=%dx%d\n",
                            iter, bufW, bufH, offX, offY, fi.contentW, fi.contentH,
                            logicalW, logicalH);
            }
        }
        CHECK(fuzzFailures == 0, "fuzz vpDst scale vs ref");
    }

    std::printf("shm_frame_source_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

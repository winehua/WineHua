#include "compositor/shm_frame_source.h"
#include "compositor/geometry.h"  // DisplaySizeAfterViewport (不 clamp 变体)
#include <algorithm>
#include <cstring>

// ============================================================================
// ShmFrameSource 实现 — wl_core.cpp 原 file-static 纯函数逐字搬移
// (重构第 5A1 步, 行为平价)。零 wayland/hilog 依赖, host_tests 直连编译。
// ============================================================================

// 紧凑拷贝 content 区 (去 stride padding, 裁剪 window_geometry 后的区域)
void CopyShmContentTight(const ShmCommitInfo& fi, std::vector<uint8_t>& dst) {
    const int contentRowBytes = fi.contentW * 4;
    dst.resize(static_cast<size_t>(contentRowBytes) * fi.contentH);
    uint8_t* d = dst.data();
    const uint8_t* rowStart = fi.src + fi.contentOffY * fi.stride + fi.contentOffX * 4;
    for (int32_t y = 0; y < fi.contentH; y++) {
        std::memcpy(d, rowStart + y * fi.stride, contentRowBytes);
        d += contentRowBytes;
    }
}

// wp_viewport destination is the logical surface size. Wine may attach an
// aligned SHM buffer whose source rectangle is smaller than that destination
// (for example 640 pixels scaled to a 652-pixel decorated window). Preserve
// all BGRA channels while scaling so ARGB window masks keep their alpha.
void CopyToplevelContent(int32_t vpDstW, int32_t vpDstH,
                         ShmCommitInfo& fi, std::vector<uint8_t>& dst) {
    const int sourceW = fi.contentW;
    const int sourceH = fi.contentH;
    const int logicalW = DisplaySizeAfterViewport(vpDstW, sourceW);
    const int logicalH = DisplaySizeAfterViewport(vpDstH, sourceH);

    if (logicalW == sourceW && logicalH == sourceH) {
        CopyShmContentTight(fi, dst);
    } else {
        dst.resize(static_cast<size_t>(logicalW) * logicalH * 4);
        const uint8_t* source = fi.src + fi.contentOffY * fi.stride + fi.contentOffX * 4;
        for (int y = 0; y < logicalH; ++y) {
            const int sourceY = std::min(sourceH - 1,
                static_cast<int>((static_cast<int64_t>(y) * sourceH) / logicalH));
            const uint8_t* sourceRow = source + static_cast<size_t>(sourceY) * fi.stride;
            uint8_t* destinationRow = dst.data() + static_cast<size_t>(y) * logicalW * 4;
            for (int x = 0; x < logicalW; ++x) {
                const int sourceX = std::min(sourceW - 1,
                    static_cast<int>((static_cast<int64_t>(x) * sourceW) / logicalW));
                std::memcpy(destinationRow + x * 4, sourceRow + sourceX * 4, 4);
            }
        }
    }

    fi.contentW = logicalW;
    fi.contentH = logicalH;
}

// 紧凑拷贝全 buffer (subsurface 帧 staging / deprecated 全局帧缓冲用)
void CopyShmBufferTight(const ShmCommitInfo& fi, std::vector<uint8_t>& dst) {
    const int rowBytes = fi.bufW * 4;
    dst.resize(static_cast<size_t>(rowBytes) * fi.bufH);
    uint8_t* d = dst.data();
    for (int32_t y = 0; y < fi.bufH; y++) {
        std::memcpy(d, fi.src + y * fi.stride, rowBytes);
        d += rowBytes;
    }
}

// 计算实际内容区: 优先 xdg_surface window_geometry (协议: geometry 是
// buffer 内的"可见内容"矩形), 否则全 buffer。toplevel 的 geoX/geoY 在桌面
// 模式另有含义 (虚拟桌面屏幕位置), subsurface 则是相对父 surface 的偏移。
void ComputeContentAreaGeometry(ShmCommitInfo& fi,
                                bool hasWindowGeometry,
                                int32_t geoW, int32_t geoH,
                                bool hasToplevel,
                                int32_t geoX, int32_t geoY) {
    fi.contentW = fi.bufW;
    fi.contentH = fi.bufH;
    if (hasWindowGeometry && geoW > 0 && geoH > 0) {
        fi.contentW = geoW;
        fi.contentH = geoH;
        if (hasToplevel) {
            // 桌面模式: toplevel content 永远从 buffer 原点开始,
            // geoX/geoY 是虚拟桌面屏幕位置
            fi.contentOffX = 0;
            fi.contentOffY = 0;
            fi.screenX = geoX;
            fi.screenY = geoY;
        } else {
            // subsurface: geoX/geoY 是相对父 surface 的内容偏移
            fi.contentOffX = geoX;
            fi.contentOffY = geoY;
        }
        /*
         * 防御: geometry 与 buffer 是异步更新的 — 显示模式切换瞬间
         * Wine 会先发新 geometry (如 1400x920) 而 buffer 仍是旧尺寸
         * (如 896x640, GL readback 管线尚未跟上)。content 必须 clamp
         * 进 buffer 实际范围, 否则拷贝越界读 shm → SIGSEGV
         * (实测: 游戏退出恢复桌面分辨率的瞬间崩溃于 memcpy)。
         * 该帧显示为部分内容, 下一帧 buffer 跟上后自然恢复。
         */
        if (fi.contentOffX < 0 || fi.contentOffX >= fi.bufW) fi.contentOffX = 0;
        if (fi.contentOffY < 0 || fi.contentOffY >= fi.bufH) fi.contentOffY = 0;
        if (fi.contentOffX + fi.contentW > fi.bufW) fi.contentW = fi.bufW - fi.contentOffX;
        if (fi.contentOffY + fi.contentH > fi.bufH) fi.contentH = fi.bufH - fi.contentOffY;
        if (fi.contentW <= 0 || fi.contentH <= 0) {
            fi.contentOffX = fi.contentOffY = 0;
            fi.contentW = fi.bufW;
            fi.contentH = fi.bufH;
        }
    }
}

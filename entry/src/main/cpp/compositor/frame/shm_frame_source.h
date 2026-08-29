#pragma once
#include <cstdint>
#include <vector>

// ShmFrameSource — wl_core.cpp 的 SHM 帧读取/拷贝/缩放纯函数模块。
// 抽出背景: docs/COMPOSITOR_REFACTOR_PLAN.md §四阶段5 "SHM 拷贝/缩放抽为
// ShmFrameSource 纯函数 (可进 host_tests)"; 本文件原有知识 (原 wl_core.cpp
// 的 file-static CopyShmContentTight/CopyToplevelContent/CopyShmBufferTight
// 与成员函数 ComputeContentArea 的几何计算段) 于重构第 5A1 步逐字搬移,
// 行为平价 — 函数体/坐标/裁剪/缩放语义不变, 仅值语义参数化 (无 SurfaceData/
// wl_resource 现场依赖)。
//
// 零依赖约定: 本模块不 include wayland/hilog — host_tests 用宿主 g++
// 直连编译 (make test, 见 host_tests/shm_frame_source_test.cpp)。
// wl_shm_buffer 只是 ShmCommitInfo 中的不透明标识; 取值与 begin_access /
// end_access 的生命周期 (wl 资源协议知识) 留在 wl_core.cpp 的
// WaylandServer::BeginShmAccess, 不在本模块。

struct wl_shm_buffer;

// surface_commit 单次提交内共享的 shm 帧上下文 (原定义在 compositor/surface_data.h,
// 随 SHM 纯函数迁至本模块; BeginShmAccess 填充, 各分段函数只读;
// 见 wl_core.cpp 的分段实现)
struct ShmCommitInfo {
    wl_shm_buffer* shm = nullptr;
    const uint8_t* src = nullptr;      // buffer 像素基址 (begin_access 期间有效)
    int32_t bufW = 0, bufH = 0;        // buffer 全尺寸
    int32_t stride = 0;                // 每行字节数 (可含 padding)
    uint32_t shmFormat = 1;            // wl_shm format (0=ARGB8888, 1=XRGB8888)
    int contentW = 0, contentH = 0;    // 内容区尺寸 (window_geometry 裁剪后)
    int contentOffX = 0, contentOffY = 0;  // 内容区在 buffer 内的偏移
    int screenX = 0, screenY = 0;      // 桌面模式: 虚拟桌面屏幕位置
    uint64_t shmCommitSerial = 0;      // 本次 commit 的序号 (zero-copy 回退判定)
};

// 紧凑拷贝 content 区 (去 stride padding, 裁剪 window_geometry 后的区域)
void CopyShmContentTight(const ShmCommitInfo& fi, std::vector<uint8_t>& dst);

// wp_viewport destination is the logical surface size. Wine may attach an
// aligned SHM buffer whose source rectangle is smaller than that destination
// (for example 640 pixels scaled to a 652-pixel decorated window). Preserve
// all BGRA channels while scaling so ARGB window masks keep their alpha.
// vpDstW/vpDstH 为 SurfaceData 的 wp_viewport destination (-1=未设置/全 buffer)
// 的值语义参数化; 结束时 fi.contentW/contentH 更新为逻辑显示尺寸。
void CopyToplevelContent(int32_t vpDstW, int32_t vpDstH,
                         ShmCommitInfo& fi, std::vector<uint8_t>& dst);

// 紧凑拷贝全 buffer (subsurface 帧 staging / deprecated 全局帧缓冲用)
void CopyShmBufferTight(const ShmCommitInfo& fi, std::vector<uint8_t>& dst);

// 计算实际内容区: 优先 xdg_surface window_geometry (协议: geometry 是
// buffer 内的"可见内容"矩形), 否则全 buffer。toplevel 的 geoX/geoY 在桌面
// 模式另有含义 (虚拟桌面屏幕位置), subsurface 则是相对父 surface 的偏移。
// 几何参数为 SurfaceData 现场字段的值语义参数化 (原 WaylandServer::
// ComputeContentArea 的计算段逐字搬移, 行为平价 — 含异步 geometry/buffer
// 防御性 clamp)。日志 (MW-GEO/MW-STRIDE) 留在 wl_core.cpp 包装函数。
void ComputeContentAreaGeometry(ShmCommitInfo& fi,
                                bool hasWindowGeometry,
                                int32_t geoW, int32_t geoH,
                                bool hasToplevel,
                                int32_t geoX, int32_t geoY);

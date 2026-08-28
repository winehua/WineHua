#pragma once
#include <wayland-server-core.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct wl_shm_buffer;

// wl_surface 的每个实例携带的数据。
// 提取为独立头文件: compositor 子模块和 WaylandServer 各自 include, 无需互相依赖。

// surface_commit 单次提交内共享的 shm 帧上下文 (BeginShmAccess 填充,
// 各分段函数只读; 见 wl_core.cpp 的分段实现)
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

struct SurfaceData {
    wl_resource* surface = nullptr;
    uint64_t surfaceKey = 0;
    uint32_t clientPid = 0;
    uint32_t protocolId = 0;
    wl_resource* pendingBuffer = nullptr;
    std::vector<wl_resource*> frameCallbacks;

    // per-surface pixel buffer
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    std::atomic<uint64_t> shmCommitSerial{0};

    // toplevel identity
    uint32_t toplevelId = 0;
    bool hasToplevel = false;
    std::string title;

    // xdg_surface window geometry (content area within buffer), 默认全 buffer
    bool hasWindowGeometry = false;
    int geoX = 0, geoY = 0, geoW = 0, geoH = 0;

    // subsurface 父子追踪 (用于 popup 菜单合成到父 toplevel)
    wl_resource* parentSurface = nullptr;     // 父 wl_surface (仅 subsurface)
    int32_t subsurfaceX = 0, subsurfaceY = 0; // wl_subsurface.set_position
    bool isSubsurface = false;

    // surface_damage 累积包围盒 (buffer 坐标), 用于裁剪 subsurface 渲染
    int32_t damageX = 0, damageY = 0, damageW = 0, damageH = 0;

    // wp_viewport destination (实际显示尺寸, -1=未设置/使用 buffer 尺寸)
    int32_t vpDstW = -1, vpDstH = -1;
    // wp_viewport source rectangle (buffer 内的真实内容区域, -1=未设置/全 buffer)
    // Wine popup 的 shm buffer 常按 2 的幂次对齐填充, 真实尺寸经 set_source 给出
    int32_t vpSrcX = 0, vpSrcY = 0, vpSrcW = -1, vpSrcH = -1;

    // window states
    // 状态边界 (docs/CPP_REFACTOR_PLAN.md Phase 2):
    //   minimized/fullscreen 的"生效状态"唯一权威在 ToplevelState (ToplevelManager),
    //   本结构不存副本; 查询经 WaylandServer::IsToplevelMinimized/Fullscreen。
    //   maximized + preMax/preFs 是协议侧状态: 只有 xdg_shell 状态机和
    //   NotifyToplevelResize 消费, compositor 合成/命中不读。
    // app_id (xdg_toplevel.set_app_id), 用于识别 explorer 桌面
    std::string appId;
    bool maximized = false;
    int32_t preMaxW = 0, preMaxH = 0;  // 最大化前尺寸, restore 用
    int32_t preFsW = 0, preFsH = 0;    // 全屏前尺寸, unset_fullscreen restore 用

    // xdg_toplevel resize 约束 (0 = 无限制)
    bool hasSizeLimits = false;
    int32_t minWidth = 0, minHeight = 0;
    int32_t maxWidth = 0, maxHeight = 0;

    // wl_surface.set_input_region: true = 空区域 (不接受输入, 穿透点击)
    bool inputRegionEmpty = false;
};

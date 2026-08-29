#pragma once
#include <wayland-server-core.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "shm_frame_source.h"     // ShmCommitInfo (SHM 帧上下文随 ShmFrameSource 纯函数迁出)
#include "committed_surface.h"    // CommittedSurface 快照 (重构第 5A2 步: commit 产物命名快照)

// wl_surface 的每个实例携带的数据。
// 提取为独立头文件: compositor 子模块和 WaylandServer 各自 include, 无需互相依赖。
// ShmCommitInfo 已随 SHM 拷贝/缩放纯函数 (重构第 5A1 步) 迁至 shm_frame_source.h
// (纯值字段无 wayland 依赖, host_tests 可编译); CommittedSurface (commit 产物
// 命名快照, 见 committed_surface.h) 于重构第 5A2 步引入。

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

    // xdg_surface window geometry (content area within buffer) 已整体迁入
    // CommittedSurface: 旧 geoX/geoY/geoW/geoH 三义字段 (toplevel 桌面模式=
    // 虚拟桌面坐标、toplevel PC 模式=内容偏移、subsurface=buffer 内内容偏移,
    // PLAN §2.4 一名多义) 与 hasWindowGeometry 于重构第 5A2 步消亡 — 写点
    // (xdg_shell xs_set_window_geometry, 直写 hasWindowGeometry+contentRect),
    // 消费端按命名字段取义 (映射表见对应提交说明, 语义定义见 committed_surface.h)。

    // CommittedSurface 快照 (重构第 5A2 步): commit 产物 (role/contentRect/
    // screenPos/parentOffset/frame 命名字段 — PLAN 出处与 geoX/geoY 三义
    // 消亡说明见 committed_surface.h)。提交 1 (5A2·1/2) 只产出: 与旧字段
    // 同一次计算的两种表达, 旧读取路径零改动; 提交 2 (5A2·2/2) 消费端切换到
    // 本快照, geoX/geoY/geoW/geoH/hasWindowGeometry 从本结构删除。
    CommittedSurface committed;

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

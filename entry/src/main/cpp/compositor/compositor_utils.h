#pragma once
#include <cstdint>
#include <vector>
#include <wayland-server-core.h>
#include "compositor_constants.h"
#include "toplevel_manager.h"
// 像素合成原语 (BlitScaled / BlitClipAlpha / PixelBlend) 在
// compositor_blit.h — 纯函数独立文件, 宿主机单元测试直接编译真实实现。
#include "compositor_blit.h"

// 保比例适配 (letterbox) 几何已迁至 geometry.h (FitRect / ComputeFitRect)。

// 最小化自动恢复: Wine 没有 unset_minimized 协议, 还原时直接 commit
// 正常尺寸内容, 而最小化标题栏只 commit ~200x30 的小表面 (定期刷新)。
// 大于阈值的 commit 判定为真实窗口恢复。
inline bool IsRestoreSizeCommit(bool minimized, int32_t contentW, int32_t contentH) {
    return minimized && contentW > compositor_consts::kRestoreMinContentWidth &&
           contentH > compositor_consts::kRestoreMinContentHeight;
}

// ---- 最小化 subsurface offset 补偿 ----
//
// Windows 窗口管理器将最小化窗口移到 (-32000, -32000)，这是一个自 Win95 以来
// 的既定行为——WS_MINIMIZE 是语义标记，(-32000,-32000) 是其副作用。
// Wine 忠实地复现了这一行为；winewayland.drv 在计算 subsurface offset 时
// 直接使用 window->rect，数学上并无错误：
//
//   wayland_surface.c: wayland_surface_reconfigure_client()
//     client_x = client_rect->left + window->client_rect.left - window->rect.left
//              = 正常屏幕坐标 - (-32000) = 正常坐标 + 32000
//
// 这不是 Wine 的 bug——Wine 的角色是忠实地表达 Windows 窗口系统的状态。
// 若在 Wine 侧特判 window->minimized 排除 rect 偏移，等于在协议通道上掩盖
// 正确的 Windows 行为，不利于其他 compositor 理解真实状态。
//
// 因此补偿放在 compositor 侧：检测 offset 超过阈值时减去 32000 还原。
// 超过 16000 才算偏移是因为正常窗口坐标不会这么大（虚拟桌面极端值约 ±8000）。
// sx/sy 两个坐标分别判定补偿 (阈值判定各自独立)。
inline void CompensateMinimizedSubsurfaceOffset(const ToplevelManager::ToplevelState* pst,
                                                int32_t& sx, int32_t& sy) {
    if (pst && pst->IsMinimized()) {
        if (sx > compositor_consts::kMinimizedCoordThreshold) sx -= compositor_consts::kMinimizedCoordOffset;
        if (sy > compositor_consts::kMinimizedCoordThreshold) sy -= compositor_consts::kMinimizedCoordOffset;
    }
}

// -- Surface 工具 --

uint64_t MakeSurfaceKey(uint32_t clientPid, uint32_t surfaceId);
uint32_t GetWaylandClientPid(wl_client* client);

#pragma once
#include <cstdint>

// compositor 全局常量。
// 原为散落于 wayland_server.cpp / desktop_compositor.cpp 的 magic number,
// 集中命名并注明来源/原因 (重构原则: 特例有名有姓有原因, 见 docs/CPP_REFACTOR_PLAN.md)。
namespace compositor_consts {

// -- 虚拟 wl_output 上报参数 --
// Wine winewayland 读 mode/geometry 推算 DPI; 上报值不影响渲染内容。
constexpr int32_t kDefaultOutputWidth = 1280;
constexpr int32_t kDefaultOutputHeight = 720;
constexpr int32_t kOutputRefreshMillihertz = 60000;  // 60Hz, 协议单位为 mHz
// 默认分辨率下的物理尺寸 (mm): 1280x720 → 340x190 ≈ 96 DPI; 实际分辨率按比例折算
constexpr int32_t kOutputPhysWidthMm = 340;
constexpr int32_t kOutputPhysHeightMm = 190;

// -- FNV-1a 64 位哈希 (ARGB 形状掩码哈希 / 桌面合成签名共用) --
constexpr uint64_t kFnv1aOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t kFnv1aPrime = 1099511628211ULL;

// -- ARGB 窗口剪影掩码 (setWindowMask 用) --
// 阈值 128: 半透明抗锯齿边缘向内收半像素, 避免灰边外扩
constexpr uint8_t kArgbMaskAlphaThreshold = 128;

// -- 最小化自动恢复阈值 --
// Wine 没有 unset_minimized 协议, 还原时直接 commit 正常尺寸内容;
// 最小化标题栏约 200x30, 大于该尺寸的 commit 判定为真实窗口恢复
constexpr int32_t kRestoreMinContentWidth = 200;
constexpr int32_t kRestoreMinContentHeight = 50;

// -- Wine 最小化坐标补偿 --
// Windows 窗口管理器把最小化窗口移到 (-32000, -32000) — 自 Win95 以来的既定行为，
// Wine 忠实地复现。winewayland.drv 计算 subsurface offset 时使用 window->rect,
// 导致最小化时的 subsurface offset 被附加了 +32000 (数学上配方正确，不是 bug)。
//
// Compositor 在 UpdateSubsurfaceLayerOnCommit 中检测并减去偏移, 而非在 Wine 侧
// 特判屏蔽——Wine 的角色是正确表达 Windows 状态, 不应掩盖窗口的真实坐标。
//
// 阈值 16000: 正常窗口坐标远达不到此值 (虚拟桌面极端值约 ±8000)。
constexpr int32_t kMinimizedCoordThreshold = 16000;
constexpr int32_t kMinimizedCoordOffset = 32000;

// -- Wine 语义化 app_id (winewayland wayland_surface.c 按窗口 class 附加后缀) --
// desktop-shell: explorer 的 #32769 桌面窗口 (desktop root 识别)
// taskbar: Shell_TrayWnd 任务栏 (置顶 pin / 工作区高度计算)
constexpr const char* kAppIdExplorerDesktopShell = "explorer.exe.desktop-shell";
constexpr const char* kAppIdExplorerTaskbar = "explorer.exe.taskbar";

} // namespace compositor_consts

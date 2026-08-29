#include "compositor/input/input_space_mapper.h"

#include "plugin_manager.h"
#include "wayland_server.h"
#include "egl_renderer.h"  // GetWidth/GetHeight/GetInputLetterbox

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Input"
#include <hilog/log.h>

// ============================================================================
//  单例
// ============================================================================

InputSpaceMapper* InputSpaceMapper::GetInstance() {
    static InputSpaceMapper s;
    return &s;
}

// ============================================================================
//  renderer 查找 fallback 链 (原 InputManager::CoordTransform 内联段)
// ============================================================================

EglRenderer* InputSpaceMapper::ResolveRendererFor(uint32_t tl) {
    auto* r = PluginManager::GetInstance()->GetRendererForToplevel(tl);
    // Desktop 模式 fallback: root 切换后可能用旧 ID 查 renderer
    if (!r && WaylandServer::GetInstance()->Policy().RootCompositing()) {
        uint32_t rootId = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
        if (rootId != tl) r = PluginManager::GetInstance()->GetRendererForToplevel(rootId);
        // 兜底: RootCompositing 下 renderer 永远渲染桌面根，letterbox 映射与
        // 登记 id 无关；桌面根重建瞬间或前台窗口"提升"后按 id 查不到
        // renderer 时，取当前登记的唯一 renderer 仍能得到正确的 viewport
        // 映射（否则坐标全部坍缩为 (0,0)，触摸/鼠标不可用）。
        if (!r) r = PluginManager::GetInstance()->GetAnyRenderer();
    }
    return r;
}

// ============================================================================
//  坐标变换 (原 InputManager::CoordTransform 逐字搬移)
// ============================================================================

void InputSpaceMapper::CoordTransform(double px, double py, uint32_t tl,
                                      wl_fixed_t* outX, wl_fixed_t* outY,
                                      FitRect* outLb) {
    auto* r = ResolveRendererFor(tl);
    if (!r) {
        OH_LOG_WARN(LOG_APP, "[Input] CoordTransform: no renderer for tl=%{public}u", tl);
        *outX = 0; *outY = 0;
        return;
    }
    int surfW = r->GetWidth();
    int surfH = r->GetHeight();
    // 桌面系基准 (20260822 红警2 主菜单点击无效根因修复): 输入坐标换算的
    // 锚点是"桌面逻辑坐标" (root toplevel 尺寸), 与"渲染当前帧格式"解耦。
    // 此前用 r->GetLetterbox() (渲染视口) 做逆映射 — 直传 (7930495) 时
    // renderer 的帧是游戏直传源 800x600, letterbox 逆映射先把物理坐标缩到
    // 800x600 系, 再进 InputResolver 的 fit 被二次缩放 → 注入坐标与视觉
    // 光标错位 → 游戏永远点不到按钮 (红警2 主菜单点击无效)。CPU 帧
    // (1400x920) 时渲染视口恰为恒等映射, 两个基准重合, 故此前单测有效。
    // 输入逆映射锚 (PresentedFrame 契约, 重构第 2B 步): 由 renderer 按最近一帧
    // 契约的 contentW/H 给出 surface 保比例 fit — 桌面合成/快进/直传帧锚桌面
    // 逻辑尺寸, PC 窗口帧锚窗口内容尺寸。旧实现在此绕路重算: 桌面模式用 root
    // 尺寸做 ComputeFitRect, root 未就绪或 PC 模式退回渲染器显示 letterbox。
    // 契约化后 GetInputLetterbox 内部承接同一 fallback (无帧 contentW/H=0 或
    // fit 失败时返回显示 letterbox_)。基准 (20260822 红警2 直传点击修复):
    // 输入锚是"桌面逻辑坐标", 与"渲染当前帧格式"解耦 — 直传游戏帧 buffer
    // 是内容尺寸 (800x600), 锚仍是桌面尺寸 (1400x920), 否则逆映射二次缩放。
    FitRect lb = r->GetInputLetterbox();
    if (outLb) *outLb = lb;

    if (surfW <= 0 || surfH <= 0 || lb.dstW <= 0 || lb.dstH <= 0) {
        *outX = 0; *outY = 0;
        return;
    }

    // Letterbox 逆映射 (geometry.h 统一实现): 物理像素 → 去黑边 → 按帧尺寸缩放
    // 注意用取整后 dst 尺寸的变体 — 与 glViewport 实际显示的整数像素严格一致
    wl_fixed_t wx = wl_fixed_from_double(FitUnmapDisplayX(lb, px));
    wl_fixed_t wy = wl_fixed_from_double(FitUnmapDisplayY(lb, py));
    *outX = wx; *outY = wy;

    // 系统性链路日志 (断点 1): letterbox 逆映射是"物理像素→桌面坐标"的关键,
    // DEBUG 级别 release 下被滤掉 → 升 INFO + MOVE 高频抽样。分析鼠标问题时
    // 与 TARGET 日志 (断点 2) 配对看: 此处输出桌面逻辑坐标, 后者换算到窗口局部
    static uint32_t sCoordLogN = 0;
    if (++sCoordLogN % 120 == 0)
        OH_LOG_INFO(LOG_APP, "[Input] CoordTransform px=(%{public}.0f,%{public}.0f) vp=(%{public}d,%{public}d %{public}dx%{public}d)"
                     " surf=%{public}dx%{public}d frame=%{public}dx%{public}d → wine=(%{public}.0f,%{public}.0f) n=%{public}u",
                     px, py, lb.offX, lb.offY, lb.dstW, lb.dstH, surfW, surfH, lb.srcW, lb.srcH,
                     wl_fixed_to_double(wx), wl_fixed_to_double(wy), sCoordLogN);
}

// ============================================================================
//  全局指针位置 (lastGlobalPtrX_/Y_ 从 InputManager 收口, 显式语义化)
// ============================================================================

void InputSpaceMapper::UpdateGlobalPtr(wl_fixed_t x, wl_fixed_t y, GlobalPtrState::Space space) {
    globalPtrX_.store(x);
    globalPtrY_.store(y);
    globalPtrSpace_.store(space);
}

void InputSpaceMapper::ResetGlobalPtr() {
    globalPtrX_.store(0);
    globalPtrY_.store(0);
    globalPtrSpace_.store(GlobalPtrState::Space::Desktop);
}

wl_fixed_t InputSpaceMapper::GetGlobalPtrX() const { return globalPtrX_.load(); }
wl_fixed_t InputSpaceMapper::GetGlobalPtrY() const { return globalPtrY_.load(); }

GlobalPtrState InputSpaceMapper::GetGlobalPtr() const {
    return GlobalPtrState{globalPtrSpace_.load(), globalPtrX_.load(), globalPtrY_.load()};
}

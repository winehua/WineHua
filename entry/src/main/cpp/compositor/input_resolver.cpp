#include "input_resolver.h"
#include "toplevel_manager.h"
#include "desktop_compositor.h"
#include "geometry.h"
#include "compositor/surface_data.h"
#include <algorithm>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

InputResolver::InputResolver(ToplevelManager& tmgr, DesktopCompositor& compositor,
                             const uint32_t& desktopRootToplevelId,
                             const int32_t& outputW, const int32_t& outputH)
    : tmgr_(tmgr)
    , compositor_(compositor)
    , desktopRootToplevelId_(desktopRootToplevelId)
    , outputW_(outputW)
    , outputH_(outputH)
{
}

void InputResolver::ResolveRootSize(int& rootW, int& rootH) const
{
    const auto* rootSt = tmgr_.FindToplevelLocked(desktopRootToplevelId_);
    rootW = (rootSt && rootSt->Width() > 0) ? rootSt->Width() : outputW_;
    rootH = (rootSt && rootSt->Height() > 0) ? rootSt->Height() : outputH_;
}

uint32_t InputResolver::FindToplevelAt(int x, int y)
{
    InputTarget target;
    FindInputTargetAt(x, y, target);
    return target.toplevelId;
}

bool InputResolver::FindInputTargetAt(int x, int y, InputTarget& out)
{
    auto lk = tmgr_.Lock();
    uint32_t rootId = desktopRootToplevelId_;

    // 层序单一数据源 (阶段 1): 与渲染侧 (TakeToplevelFrame) 遍历同一个按
    // zIndex 升序的 Layer 列表。方案 B: 命中也是单一 zIndex 逆序遍历 —
    // 全屏窗口作为普通层参与 Z 序, 命中几何用 fit (与渲染 blitToplevel/
    // blitSubsurface 同一变换); "盖在游戏之上的层优先命中"由逆序天然
    // 保证, 不再有独立的前置命中循环。
    int rootW, rootH;
    ResolveRootSize(rootW, rootH);
    const auto layers = compositor_.BuildLayerListLocked(rootW, rootH);

    // 全屏目标选取 + fit 几何 — 与渲染侧 (TakeToplevelFrame) 共用单一实现:
    // PickFullscreenLayerLocked: 可见全屏窗口中取 fsPriority 最大者 (多窗口
    // 可同时 fullscreen, 显示模式切换时 Wine 会连带标记旧窗口 — 2026-07 实测
    // notepad 被连带标记并压在游戏上), 规则原因/局限见 ToplevelState::fsPriority
    // 注释; ComputeFullscreenFitLocked: ZC 游戏用全屏前尺寸 (游戏分辨率),
    // SHM 游戏用 buffer 尺寸 (见该函数注释)
    const uint32_t fullscreenId = compositor_.PickFullscreenLayerLocked(layers);
    const ToplevelManager::ToplevelState* zst =
        fullscreenId ? tmgr_.FindToplevelLocked(fullscreenId) : nullptr;
    FitRect transform;
    const bool fsOk = zst &&
        compositor_.ComputeFullscreenFitLocked(fullscreenId, rootW, rootH, transform);
    if (fsOk) {
        // 诊断: 全屏输入目标选取 (仅目标变化时输出 — 多窗口同时全屏时
        // 选错窗口的点击路由问题靠它定位, 例如旧窗口被连带标记压在游戏上)
        static uint32_t sLastPicked = 0;
        if (fullscreenId != sLastPicked) {
            sLastPicked = fullscreenId;
            int layerW = 0, layerH = 0;
            const bool hasZC = compositor_.GetZeroCopyContentSizeLocked(fullscreenId, layerW, layerH);
            OH_LOG_INFO(LOG_APP,
                "[Input] fs-pick tl=#%{public}u pri=%{public}llu zc=%{public}d"
                " layer=%{public}dx%{public}d buf=%{public}dx%{public}d → content=%{public}dx%{public}d",
                fullscreenId, static_cast<unsigned long long>(zst->FsPriority()),
                hasZC ? 1 : 0,
                layerW, layerH, zst->Width(), zst->Height(),
                transform.srcW, transform.srcH);
        }
    }
    const int fsWinX = zst ? zst->X() : 0;
    const int fsWinY = zst ? zst->Y() : 0;

    /*
     * 单一 Z 序命中循环 (方案 B): 从最高 zIndex 向下遍历 Layer 列表, 与
     * 渲染侧 TakeToplevelFrame 单一合成循环同源 (同列表, 每层同几何)。
     * - 全屏窗口 (fs-pick 选中) 作为普通层参与: 命中用 fit 变换后的屏幕
     *   几何 (内容区命中 / 黑边命中标 swallow), 与其 subsurface 一起在
     *   循环里各自的 zIndex 位置处理。更高 zIndex 的窗口 (对话框等) 渲染
     *   在游戏上方, 逆序先到 → 命中优先, 修复"全屏时弹出新窗口显示在上方、
     *   点击却回到游戏"。
     * - 连带 fullscreen 旧窗口 (显示模式切换时被 winewayland 批量标记)
     *   渲染时被跳过 (blitToplevel 705 / blitSubsurface 793), 命中也跳过
     *   (防点到看不见的层)。
     * - zero-copy GL 层不参与置顶命中 (ShouldSkipCpu: zcActive) — 渲染时
     *   被遮挡重绘压回, 命中同样下放给下方 z-order; GPU→CPU fallback 时
     *   key 移出 zeroCopySurfaceKeys_, 该层自动恢复为普通 subsurface
     *   (CPU 合成置顶, 命中也置顶), 无需特判。
     * - 黑边命中: 归属仍是全屏窗口但标 swallow — 调用方只吞 PRESS (防幻影
     *   点击/焦点切换), MOVE/RELEASE 照常透传: 坐标越界由 winewayland 的
     *   motion clamp 夹回窗口边缘; 若连 RELEASE 一起吞, 内容区按下拖到黑边
     *   松手会丢失 release, pressedButtons_ 按键状态永久卡死。
     */
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const auto& layer = *it;
        if (layer.ShouldSkipCpu()) continue;

        // 连带 fullscreen 跳过 (与渲染 blitToplevel/blitSubsurface 同一规则,
        // 单一实现 ShouldSkipFullscreenCascade): fsOk 时, 非主全屏窗口 (被
        // 连带标记的旧窗口) 渲染被跳过, 命中同样下放 (防点到看不见的层);
        // 非全屏弹窗/对话框保留
        if (DesktopCompositor::ShouldSkipFullscreenCascade(layer, fullscreenId, fsOk, tmgr_)) continue;

        if (layer.type == DesktopCompositor::CompositorLayer::Type::Subsurface) {
            // 内部菜单: enter 层自己的 wl_surface, 坐标以层原点为基。层可伸出
            // 父窗口边界 — 若改走父窗口 surface, 伸出部分产生越界的窗口相对
            // 坐标, 会被 winewayland 的 motion clamp (wayland_pointer.c
            // "bring them within bounds") 夹回窗口内, 菜单项永远收不到该区域
            // 的点击。外部菜单 (isExternal): 任务栏弹出等, subsurface offset
            // 是 Wine 虚拟屏幕坐标 → 走 root, Wine explorer 内部处理点击分发
            if (layer.w <= 0 || layer.h <= 0) continue;
            const auto& sl = *layer.sub;
            if (fsOk && layer.toplevelId == fullscreenId) {
                // 主全屏窗口的 subsurface 绘制在窗口内容之上, 先命中 (同一
                // fit 变换, 与渲染 blitSubsurface 全屏分支同几何)
                const int layerDispW = sl.vpDstW > 0 ? std::min(sl.vpDstW, sl.w) : sl.w;
                const int layerDispH = sl.vpDstH > 0 ? std::min(sl.vpDstH, sl.h) : sl.h;
                int layerScrX, layerScrY, layerScrW, layerScrH;
                FitMapLayerRect(transform, layer.x - fsWinX, layer.y - fsWinY,
                                layerDispW, layerDispH,
                                layerScrX, layerScrY, layerScrW, layerScrH);
                if (x >= layerScrX && x < layerScrX + layerScrW &&
                    y >= layerScrY && y < layerScrY + layerScrH) {
                    out.toplevelId = fullscreenId;
                    out.surface = sl.surface;
                    out.originX = layerScrX;
                    out.originY = layerScrY;
                    out.scale = static_cast<float>(transform.scale);
                    return out.surface != nullptr;
                }
            } else if (x >= layer.x && x < layer.x + layer.w &&
                       y >= layer.y && y < layer.y + layer.h) {
                if (sl.isExternal) {
                    out.toplevelId = rootId;
                    out.surface = tmgr_.GetSurfaceForToplevel(rootId);
                    out.originX = 0;
                    out.originY = 0;
                } else {
                    out.toplevelId = layer.toplevelId;
                    out.surface = sl.surface;
                    out.originX = layer.x;
                    out.originY = layer.y;
                }
                return out.surface != nullptr;
            }
        } else if (layer.type == DesktopCompositor::CompositorLayer::Type::Toplevel) {
            if (fsOk && layer.toplevelId == fullscreenId) {
                // 主全屏窗口: 内容区 (fit 矩形) 命中
                if (x >= transform.offX && x < transform.offX + transform.dstW &&
                    y >= transform.offY && y < transform.offY + transform.dstH) {
                    out.toplevelId = fullscreenId;
                    out.surface = tmgr_.GetSurfaceForToplevel(fullscreenId);
                    out.originX = transform.offX;
                    out.originY = transform.offY;
                    out.scale = static_cast<float>(transform.scale);
                    return out.surface != nullptr;
                }
                // 黑边: 更高层已检查未命中, 黑边归属全屏窗口 (渲染时填黑),
                // 标 swallow — 调用方只吞 PRESS, MOVE/RELEASE 照常透传
                out.toplevelId = fullscreenId;
                out.surface = tmgr_.GetSurfaceForToplevel(fullscreenId);
                out.originX = transform.offX;
                out.originY = transform.offY;
                out.scale = static_cast<float>(transform.scale);
                out.swallow = true;
                return out.surface != nullptr;
            }
            if (x >= layer.x && x < layer.x + layer.w && y >= layer.y && y < layer.y + layer.h) {
                wl_resource* surf = tmgr_.GetSurfaceForToplevel(layer.toplevelId);
                if (surf) {
                    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(surf));
                    if (sd && sd->inputRegionEmpty) continue;
                }
                out.toplevelId = layer.toplevelId;
                out.surface = surf;
                out.originX = layer.x;
                out.originY = layer.y;
                out.scale = 1.0f;
                return out.surface != nullptr;
            }
        }
    }

    out.toplevelId = rootId;
    out.surface = tmgr_.GetSurfaceForToplevel(rootId);
    out.originX = 0;
    out.originY = 0;
    return out.surface != nullptr;
}

bool InputResolver::IsSurfaceAlive(wl_resource* surface)
{
    if (!surface) return false;
    auto lk = tmgr_.Lock();
    return tmgr_.ContainsSurfaceResource(surface);
}

bool InputResolver::IsZcGameSurface(wl_resource* surface)
{
    const uint32_t tl = tmgr_.FindToplevelBySurface(surface);
    if (!tl) return false;
    auto lk = tmgr_.Lock();
    return compositor_.HasZeroCopyLayerForToplevelLocked(tl);
}

bool InputResolver::SurfaceLocalToDesktop(wl_resource* surface, double lx, double ly,
                                          double& dx, double& dy)
{
    const uint32_t tl = tmgr_.FindToplevelBySurface(surface);
    if (!tl) return false;
    auto lk = tmgr_.Lock();
    const auto* st = tmgr_.FindToplevelLocked(tl);
    if (!st) return false;
    // NOTE: 此处按 IsFullscreen() 判定, 与 FindInputTargetAt 的 fs-pick
    // (fsPriority 最大者) 是两套全屏定义 — 显示模式切换时被连带标记的旧
    // 窗口 IsFullscreen()=true 但非 fs-pick 选中。warp 请求 (SetCursorPos
    // 回中等) 一般只针对当前前台窗口, 实际不冲突; 若未来观察到连带窗口
    // warp 错位, 统一为 fs-pick 语义 (PickFullscreenLayerLocked 选中者)。
    if (st->IsFullscreen()) {
        // 与 FindInputTargetAt 全屏分支同一几何 (ComputeFullscreenFitLocked),
        // 保证 warp 锚点与输入逆映射互为正反变换
        int rootW, rootH;
        ResolveRootSize(rootW, rootH);
        FitRect transform;
        if (!compositor_.ComputeFullscreenFitLocked(tl, rootW, rootH, transform)) return false;
        dx = transform.offX + lx * transform.scale;
        dy = transform.offY + ly * transform.scale;
        return true;
    }
    dx = st->X() + lx;
    dy = st->Y() + ly;
    return true;
}

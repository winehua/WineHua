#include "input_resolver.h"
#include "toplevel_manager.h"
#include "desktop_compositor.h"
#include "geometry.h"
#include "compositor/frame/surface_data.h"
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

bool InputResolver::IsToplevelIsolated(uint32_t id) const
{
    return compositor_.IsToplevelIsolated(id);
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

bool InputResolver::FindInputTargetAt(double logicalX, double logicalY, InputTarget& out)
{
    auto lk = tmgr_.Lock();
    int rootW, rootH;
    ResolveRootSize(rootW, rootH);
    const auto layers = compositor_.BuildLayerListLocked(rootW, rootH);
    const uint32_t fullscreenId = compositor_.PickFullscreenLayerLocked(layers);
    return FindInputTargetInLayersLocked(logicalX, logicalY, out, layers,
                                         rootW, rootH, fullscreenId, true);
}

bool InputResolver::FindInputTargetInLayersLocked(double logicalX, double logicalY, InputTarget& out,
                                                 const std::vector<CompositorLayer>& layers,
                                                 int rootW, int rootH, uint32_t fullscreenId,
                                                 bool logSelection)
{
    out = {};
    uint32_t rootId = desktopRootToplevelId_;

    // 命中判定用取整桌面坐标 (与旧调用方 lround 后传 int 逐点一致);
    // 逆映射用未取整 double (旧调用方同样用未取整 logicalX/Y 换算 local) —
    // 两处精度角色不同, 4A 收内时 1-to-1 平移, 不合并。
    const int x = static_cast<int>(lround(logicalX));
    const int y = static_cast<int>(lround(logicalY));

    // 终态产出 (逆映射 + 内容区钳制 — 4A 自调用方 input_manager.cpp :437-441
    // 收内): 各命中分支在 origin/scale/content 就位后调用, 一次性把
    // localX/localY 算完, 调用方只做注入。
    // contentW/H = 内容区钳制上界 (fit src 尺寸, 0=不钳), 是 ComputeLocalPoint
    // 的管道参数 — 无外部消费者, 不进 InputTarget (4C2 顺手项, 见
    // input_resolver.h 头部注释)
    int contentW = 0, contentH = 0;
    auto finalize = [&]() {
        out.contentW = contentW;
        out.contentH = contentH;
        ComputeLocalPoint(logicalX, logicalY, out.originX, out.originY, out.scale,
                          contentW, contentH, out.localX, out.localY);
    };

    // 层序单一数据源 (阶段 1): 与渲染侧 (TakeToplevelFrame) 遍历同一个按
    // zIndex 升序的 Layer 列表。方案 B: 命中也是单一 zIndex 逆序遍历 —
    // 全屏窗口作为普通层参与 Z 序, 命中几何用 fit (与渲染 blitToplevel/
    // blitSubsurface 同一变换); "盖在游戏之上的层优先命中"由逆序天然
    // 保证, 不再有独立的前置命中循环。
    const ToplevelManager::ToplevelState* zst =
        fullscreenId ? tmgr_.FindToplevelLocked(fullscreenId) : nullptr;
    FitRect transform;
    const bool fsOk = zst &&
        compositor_.ComputeFullscreenFitLocked(fullscreenId, rootW, rootH, transform);
    if (fsOk) {
        // 诊断: 全屏输入目标选取 (仅目标变化时输出 — 多窗口同时全屏时
        // 选错窗口的点击路由问题靠它定位, 例如旧窗口被连带标记压在游戏上)
        static uint32_t sLastPicked = 0;
        if (logSelection && fullscreenId != sLastPicked) {
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
     *   点击/焦点切换), MOVE/RELEASE 照常透传: 越界坐标在 finalize
     *   (ComputeLocalPoint) 内按 contentW/contentH 钳到内容区边缘 (与相对
     *   增量差分同源, 防黑边位移被累积成游戏内幽灵位移); 若连 RELEASE
     *   一起吞, 内容区按下拖到黑边松手会丢失 release, 按键状态位 (旧
     *   pressedButtons_, 现 InputStateTracker) 永久卡死。
     */
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const auto& layer = *it;
        if (layer.ShouldSkipCpu()) continue;

        // 连带 fullscreen 跳过 (与渲染 blitToplevel/blitSubsurface 同一规则,
        // 单一实现 ShouldSkipFullscreenCascade): fsOk 时, 非主全屏窗口 (被
        // 连带标记的旧窗口) 渲染被跳过, 命中同样下放 (防点到看不见的层);
        // 非全屏弹窗/对话框保留
        if (DesktopCompositor::ShouldSkipFullscreenCascade(layer, fullscreenId, fsOk, tmgr_)) continue;

        if (layer.type == CompositorLayer::Type::Subsurface) {
            // Wine's GPU client surfaces explicitly delegate pointer events to
            // their parent. Direct queue registration must not turn these
            // input-transparent drawables into independent input targets.
            auto* data = layer.sub->surface
                ? static_cast<SurfaceData*>(wl_resource_get_user_data(layer.sub->surface)) : nullptr;
            if (data && data->inputRegionEmpty) continue;
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
                const int layerDispW = DisplaySizeAfterViewportClamped(sl.vpDstW, sl.w);
                const int layerDispH = DisplaySizeAfterViewportClamped(sl.vpDstH, sl.h);
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
                    out.scale = transform.scale;
                    out.blockedModalId = compositor_.FirstVisibleContentModalLocked(layer.toplevelId);
                    finalize();
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
                    // WineHua: 命中 owner 暴露区 (菜单层归属 owner) → 拦
                    out.blockedModalId = compositor_.FirstVisibleContentModalLocked(layer.toplevelId);
                }
                // scale 保持默认 1 (恒等变换), content 保持默认 0 (不钳制)
                finalize();
                return out.surface != nullptr;
            }
        } else if (layer.type == CompositorLayer::Type::Toplevel) {
            if (layer.externalHostId) {
                const auto* source = tmgr_.FindToplevelLocked(layer.toplevelId);
                if (source && layer.w > 0 && layer.h > 0 &&
                    x >= layer.x && y >= layer.y && x < layer.x + layer.w && y < layer.y + layer.h) {
                    out.toplevelId = layer.externalHostId;
                    out.surface = tmgr_.GetSurfaceForToplevel(layer.toplevelId);
                    out.originX = layer.x; out.originY = layer.y;
                    out.scale = static_cast<double>(layer.w) / source->Width();
                    contentW = source->Width(); contentH = source->Height();
                    out.blockedModalId = compositor_.FirstVisibleContentModalLocked(layer.externalHostId);
                    finalize();
                    return out.surface != nullptr;
                }
                continue;
            }
            if (fsOk && layer.toplevelId == fullscreenId) {
                // 主全屏窗口: 内容区 (fit 矩形) 命中
                if (x >= transform.offX && x < transform.offX + transform.dstW &&
                    y >= transform.offY && y < transform.offY + transform.dstH) {
                    out.toplevelId = fullscreenId;
                    out.surface = tmgr_.GetSurfaceForToplevel(fullscreenId);
                    out.originX = transform.offX;
                    out.originY = transform.offY;
                    out.scale = transform.scale;
                    contentW = transform.srcW;
                    contentH = transform.srcH;
                    out.blockedModalId = compositor_.FirstVisibleContentModalLocked(fullscreenId);
                    finalize();
                    return out.surface != nullptr;
                }
                // 黑边: 更高层已检查未命中, 黑边归属全屏窗口 (渲染时填黑),
                // 标 swallow — 调用方只吞 PRESS, MOVE/RELEASE 钳到内容边缘透传
                out.toplevelId = fullscreenId;
                out.surface = tmgr_.GetSurfaceForToplevel(fullscreenId);
                out.originX = transform.offX;
                out.originY = transform.offY;
                out.scale = transform.scale;
                contentW = transform.srcW;
                contentH = transform.srcH;
                out.swallow = true;
                finalize();
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
                out.scale = 1.0;
                // WineHua: 命中被模态禁用的 owner → 拦截 (吞点击+焦点切 modal)
                out.blockedModalId = layer.toplevelId != rootId
                    ? compositor_.FirstVisibleContentModalLocked(layer.toplevelId)
                    : 0;
                finalize();
                return out.surface != nullptr;
            }
        }
    }

    out.toplevelId = rootId;
    out.surface = tmgr_.GetSurfaceForToplevel(rootId);
    out.originX = 0;
    out.originY = 0;
    // scale 保持默认 1 (恒等), content 保持默认 0 (不钳制) — local = 桌面坐标
    finalize();
    return out.surface != nullptr;
}

wl_resource* InputResolver::ResolveKeyboardFocusSurface(uint32_t toplevelId,
                                                       wl_resource* hitSurface)
{
    auto* owner = tmgr_.GetSurfaceForToplevel(toplevelId);
    auto lock = tmgr_.Lock();
    if (!tmgr_.FindRegisteredSurfaceDataLocked(owner)) return nullptr;
    // The host HWND owns activation, while its explicitly bound foreign
    // Wayland client receives keyboard/IME. Never infer this from a title.
    const auto foreign = compositor_.ExternalSourceLocked(toplevelId);
    if (foreign && (hitSurface == owner || hitSurface == tmgr_.GetSurfaceForToplevel(foreign)))
        return tmgr_.GetSurfaceForToplevel(foreign);
    // Hold one lock over the entire ancestry walk, including the data reads.
    auto* current = hitSurface;
    for (unsigned depth = 0; current && depth < 32; ++depth) {
        auto* data = tmgr_.FindRegisteredSurfaceDataLocked(current);
        if (!data || tmgr_.FindSurfaceResource(data->surfaceKey) != current) return nullptr;
        if (current == owner) return owner;
        if (!data->isSubsurface || !data->parentSurface) return nullptr;
        current = data->parentSurface;
    }
    return nullptr;
}

bool InputResolver::IsSurfaceAlive(wl_resource* surface)
{
    if (!surface) return false;
    auto lk = tmgr_.Lock();
    return tmgr_.ContainsSurfaceResource(surface);
}

bool InputResolver::IsExternallyHostedSurface(wl_resource* surface)
{
    const auto id = tmgr_.FindToplevelBySurface(surface);
    auto lock = tmgr_.Lock();
    return id && compositor_.ExternalHostLocked(id) != 0;
}

bool InputResolver::CanMoveExternalHost(uint32_t source, uint32_t host)
{
    auto lock = tmgr_.Lock();
    const auto* window = tmgr_.FindToplevelLocked(host);
    return source && host && compositor_.ExternalHostLocked(source) == host && window &&
        !window->IsMinimized() && !window->IsFullscreen();
}

bool InputResolver::SurfaceLocalToDesktop(wl_resource* surface, double lx, double ly,
                                          double& dx, double& dy)
{
    const uint32_t tl = tmgr_.FindToplevelBySurface(surface);
    if (!tl) return false;
    auto lk = tmgr_.Lock();
    const auto* st = tmgr_.FindToplevelLocked(tl);
    if (!st) return false;
    if (compositor_.ExternalHostLocked(tl)) {
        int rw, rh; ResolveRootSize(rw, rh);
        for (const auto& layer : compositor_.BuildLayerListLocked(rw, rh))
            if (layer.externalHostId && layer.toplevelId == tl && layer.visible) {
                dx = layer.x + lx * layer.w / st->Width();
                dy = layer.y + ly * layer.h / st->Height();
                return true;
            }
        return false;
    }
    // Do not warp through a cascaded fullscreen window that is not displayed.
    if (st->IsFullscreen()) {
        // 与 FindInputTargetAt 全屏分支同一几何 (ComputeFullscreenFitLocked),
        // 保证 warp 锚点与输入逆映射互为正反变换
        int rootW, rootH;
        ResolveRootSize(rootW, rootH);
        const auto layers = compositor_.BuildLayerListLocked(rootW, rootH);
        if (compositor_.PickFullscreenLayerLocked(layers) != tl) return false;
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

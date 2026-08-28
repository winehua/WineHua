#include "zc_bridge.h"

#include "compositor/surface_data.h"  // SurfaceData (wl_resource_get_user_data)
#include "compositor_utils.h"         // CompensateMinimizedSubsurfaceOffset
#include "compositor/zorder_policy.h" // ZOrderNeedsParentPosCheck (ZC 遮挡层序)
#include "desktop_compositor.h"       // DesktopCompositor (friend), SubsurfaceLayer
#include "geometry.h"                 // DisplaySizeAfterViewport
#include "toplevel_manager.h"

#include <algorithm>  // std::find / std::max / std::min
#include <atomic>     // std::memory_order_acquire
#include <cstdint>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

// ============================================================================
// ZcBridge: ZC 层几何供给与 key 簿记
// (原 DesktopCompositor 方法, 逐字搬移, 行为平价 — 仅成员引用改写:
//  tmgr_/policy_/desktopRootToplevelId_/subsurfaceLayers_ → comp_.xxx,
//  zeroCopySurfaceKeys_ → activeKeys_, protocolOnly → source)
// ============================================================================

void ZcBridge::SetEnabled(uint64_t surfaceKey, bool enabled)
{
    if (!surfaceKey) return;
    auto lk = comp_.tmgr_.Lock();
    if (enabled)
        activeKeys_.insert(surfaceKey);
    else
        activeKeys_.erase(surfaceKey);
    comp_.MarkDesktopRootDirtyLocked();
    comp_.desktopCompositionSignature_ = 0;
}

void ZcBridge::RemoveKey(uint64_t surfaceKey)
{
    activeKeys_.erase(surfaceKey);
}

bool ZcBridge::GetLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                            int fallbackWidth, int fallbackHeight,
                            ZeroCopyLayerInfo& info)
{
    auto lk = comp_.tmgr_.Lock();
    auto* wlRes = comp_.tmgr_.FindSurfaceResource(surfaceKey);
    if (!wlRes) return false;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(wlRes));
    if (!sd) return false;

    info = {};
    info.surfaceKey = surfaceKey;
    info.clientPid = sd->clientPid;
    info.surfaceId = sd->protocolId;
    if (sd->isSubsurface && sd->parentSurface)
    {
        auto* parent = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
        if (!parent || !parent->hasToplevel) return false;
        info.parentToplevel = parent->toplevelId;
        info.width = DisplaySizeAfterViewport(sd->vpDstW, sd->w);
        info.height = DisplaySizeAfterViewport(sd->vpDstH, sd->h);
        if (comp_.policy_.RootCompositing())
        {
            if (rendererToplevelId != comp_.desktopRootToplevelId_ ||
                (info.parentToplevel != comp_.desktopRootToplevelId_ &&
                 !comp_.tmgr_.IsToplevelVisibleLocked(info.parentToplevel, comp_.desktopRootToplevelId_)))
                return false;
            for (const auto& layer : comp_.subsurfaceLayers_)
            {
                if (layer.surface != wlRes) continue;
                comp_.ResolveSubsurfaceLayerPositionLocked(layer, info.x, info.y);
                info.width = DisplaySizeAfterViewport(layer.vpDstW, layer.w);
                info.height = DisplaySizeAfterViewport(layer.vpDstH, layer.h);
                info.shmCommitSerial = layer.shmCommitSerial;
                info.desktopCoordinates = true;
                if (const auto* pst = comp_.tmgr_.FindToplevelLocked(layer.parentToplevel))
                    info.fullscreen = pst->IsFullscreen();
                info.source = ZeroCopySource::ShmLayer;
                return info.width > 0 && info.height > 0;
            }

            // Vulkan private-present surfaces may have no wl_shm commit. Wayland
            // still supplies the parent/offset while the present protocol supplies
            // the image dimensions.
            int sx = sd->subsurfaceX;
            int sy = sd->subsurfaceY;
            const auto* parentState = comp_.tmgr_.FindToplevelLocked(info.parentToplevel);
            CompensateMinimizedSubsurfaceOffset(parentState, sx, sy);
            const int compX = parentState ? parentState->X() : 0;
            const int compY = parentState ? parentState->Y() : 0;
            const int wineX = parentState ? parentState->WineX() : 0;
            const int wineY = parentState ? parentState->WineY() : 0;
            const int compW = parentState ? parentState->Width() : 0;
            const int compH = parentState ? parentState->Height() : 0;
            const bool insideWin = sx >= 0 && sx < compW && sy >= 0 && sy < compH;
            info.x = (insideWin ? compX : wineX) + sx;
            info.y = (insideWin ? compY : wineY) + sy;
            info.width = DisplaySizeAfterViewport(sd->vpDstW, sd->w);
            info.height = DisplaySizeAfterViewport(sd->vpDstH, sd->h);
            if (info.width <= 0) info.width = fallbackWidth;
            if (info.height <= 0) info.height = fallbackHeight;
            info.shmCommitSerial = sd->shmCommitSerial.load(std::memory_order_acquire);
            info.desktopCoordinates = true;
            info.source = ZeroCopySource::ProtocolOnly;
            if (parentState) info.fullscreen = parentState->IsFullscreen();
            // 一次性日志去重 (每 key 只打一条): static 局部集合, 调用串行化
            // 由函数入口的 tmgr_.Lock() 保证
            static std::unordered_set<uint64_t> protocolGeometryLogged;
            if (protocolGeometryLogged.insert(surfaceKey).second) {
                OH_LOG_INFO(LOG_APP,
                            "[MW-ZC] protocol-only geometry key=%{public}llu "
                            "pid=%{public}u surface=%{public}u parent=%{public}u "
                            "offset=%{public}d,%{public}d layer=%{public}dx%{public}d "
                            "fallback=%{public}dx%{public}d",
                            static_cast<unsigned long long>(surfaceKey), info.clientPid,
                            info.surfaceId, info.parentToplevel, sx, sy, info.width,
                            info.height, fallbackWidth, fallbackHeight);
            }
            return info.width > 0 && info.height > 0;
        }

        if (rendererToplevelId != info.parentToplevel) return false;
        info.x = sd->subsurfaceX - parent->geoX;
        info.y = sd->subsurfaceY - parent->geoY;
        info.shmCommitSerial = sd->shmCommitSerial.load(std::memory_order_acquire);
        return info.width > 0 && info.height > 0;
    }

    if (!sd->hasToplevel) return false;
    info.parentToplevel = sd->toplevelId;
    info.width = sd->w;
    info.height = sd->h;
    if (info.width <= 0) info.width = fallbackWidth;
    if (info.height <= 0) info.height = fallbackHeight;
    info.shmCommitSerial = sd->shmCommitSerial.load(std::memory_order_acquire);
    if (comp_.policy_.RootCompositing())
    {
        if (rendererToplevelId != comp_.desktopRootToplevelId_ ||
            (sd->toplevelId != comp_.desktopRootToplevelId_ && !comp_.tmgr_.IsToplevelVisibleLocked(sd->toplevelId, comp_.desktopRootToplevelId_)))
            return false;
        if (const auto* st = comp_.tmgr_.FindToplevelLocked(sd->toplevelId)) {
            info.x = st->X();
            info.y = st->Y();
            info.fullscreen = st->IsFullscreen();
        }
        info.desktopCoordinates = true;
        return info.width > 0 && info.height > 0;
    }
    return rendererToplevelId == sd->toplevelId && info.width > 0 && info.height > 0;
}

int ZcBridge::GetOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                           ZeroCopyOccluderRect* out, int maxOut)
{
    if (!out || maxOut <= 0) return 0;
    ZeroCopyLayerInfo info;
    if (!GetLayerInfo(surfaceKey, rendererToplevelId, 0, 0, info) ||
        !info.desktopCoordinates)
        return 0;

    auto lk = comp_.tmgr_.Lock();
    const int layerL = info.x;
    const int layerT = info.y;
    const int layerR = info.x + info.width;
    const int layerB = info.y + info.height;
    const auto* rootSt = comp_.tmgr_.FindToplevelLocked(comp_.desktopRootToplevelId_);
    if (!rootSt) return 0;
    const int rootW = rootSt->Width();
    const int rootH = rootSt->Height();
    int count = 0;
    auto pushRect = [&](int x, int y, int w, int h) {
        if (count >= maxOut || w <= 0 || h <= 0) return;
        const int l = std::max({x, layerL, 0});
        const int t = std::max({y, layerT, 0});
        const int r = std::min({x + w, layerR, rootW});
        const int b = std::min({y + h, layerB, rootH});
        if (r <= l || b <= t) return;
        out[count++] = {l, t, r - l, b - t};
    };

    auto zbegin = comp_.tmgr_.toplevelZOrder().begin();
    auto zcIt = comp_.tmgr_.toplevelZOrder().end();
    if (info.parentToplevel != comp_.desktopRootToplevelId_) {
        zcIt = std::find(comp_.tmgr_.toplevelZOrder().begin(), comp_.tmgr_.toplevelZOrder().end(),
                         info.parentToplevel);
        if (zcIt != comp_.tmgr_.toplevelZOrder().end()) zbegin = std::next(zcIt);
    }
    for (auto zit = zbegin; zit != comp_.tmgr_.toplevelZOrder().end() && count < maxOut; ++zit) {
        const uint32_t cid = *zit;
        if (!comp_.tmgr_.IsToplevelVisibleLocked(cid, comp_.desktopRootToplevelId_)) continue;
        const auto* cst = comp_.tmgr_.FindToplevelLocked(cid);
        if (!cst) continue;
        if (cst->IsFullscreen()) pushRect(0, 0, rootW, rootH);
        else pushRect(cst->X(), cst->Y(), cst->Width(), cst->Height());
    }

    // 新层序 (subsurface 挂父窗口层内, 见 BuildLayerListLocked): 仅父窗口
    // z-order 不低于 ZC 窗口的层遮挡 ZC (同窗口的菜单等仍在 ZC 层之上);
    // parent==root / 不在 z-order 的层保持置顶语义, 仍遮挡。
    for (const auto& layer : comp_.subsurfaceLayers_) {
        if (count >= maxOut) break;
        if (activeKeys_.count(layer.surfaceKey)) continue;
        if (layer.parentToplevel != comp_.desktopRootToplevelId_ &&
            !comp_.tmgr_.IsToplevelVisibleLocked(layer.parentToplevel, comp_.desktopRootToplevelId_)) continue;
        // 遮挡防护条件收口于 zorder_policy.h (ZOrderNeedsParentPosCheck, 行为平价):
        // 父==ZC 窗口或==root 恒遮挡; 否则父 z-order 位置须 >= ZC 位置 (zcIt)。
        if (winehua::ZOrderNeedsParentPosCheck(
                layer.parentToplevel == info.parentToplevel,
                layer.parentToplevel == comp_.desktopRootToplevelId_)) {
            const auto pit = std::find(comp_.tmgr_.toplevelZOrder().begin(),
                                       comp_.tmgr_.toplevelZOrder().end(),
                                       layer.parentToplevel);
            if (pit == comp_.tmgr_.toplevelZOrder().end() || pit < zcIt) continue;
        }
        int x = 0, y = 0;
        comp_.ResolveSubsurfaceLayerPositionLocked(layer, x, y);
        pushRect(x, y,
                 DisplaySizeAfterViewport(layer.vpDstW, layer.w),
                 DisplaySizeAfterViewport(layer.vpDstH, layer.h));
    }
    return count;
}

bool ZcBridge::HasLayerForToplevel(uint32_t id) const
{
    return comp_.FindZeroCopyLayerForToplevelLocked(id) != nullptr;
}

bool ZcBridge::GetContentSize(uint32_t toplevelId, int& outW, int& outH) const
{
    // 与 HasZeroCopyLayerForToplevelLocked 同一层集合判定 (共用
    // FindZeroCopyLayerForToplevelLocked 单一查找); 内容尺寸取
    // vpDst 裁剪后几何, 与 GetZeroCopyLayerInfo (egl_renderer 渲染视口
    // 缓存 zeroCopyLayerW_/H_ 的来源) 完全同规则 — 保证输入 fit 与渲染
    // 显示严格互逆。
    const auto* layer = comp_.FindZeroCopyLayerForToplevelLocked(toplevelId);
    if (!layer) return false;
    outW = DisplaySizeAfterViewport(layer->vpDstW, layer->w);
    outH = DisplaySizeAfterViewport(layer->vpDstH, layer->h);
    return true;
}

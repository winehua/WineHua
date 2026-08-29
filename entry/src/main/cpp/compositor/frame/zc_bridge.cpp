#include "zc_bridge.h"

#include "graphics_broker.h"          // SetZeroCopySurfaceReady (ready marker, 进程单例)
#include "compositor/frame/surface_data.h"  // SurfaceData (wl_resource_get_user_data)
#include "compositor_utils.h"         // CompensateMinimizedSubsurfaceOffset
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

// ============================================================================
// ZC 状态机: 何时发布/回退/确认 (重构第 3C 步)
// 自 EglRenderer 三方法/三状态位按 key 化迁入 (原 egl_renderer.cpp:419-449 +
// 各调用点的状态位维护), protocol owner 即此处。时序注释源自原调用点收敛块,
// 禁止合并: 发布先 compositor key 后 ready marker (先让合成跳过, 再通知
// guest 走 ZC); fallback 分两步 — 先撤 ready (guest 立即切 SHM), 等
// shmCommitSerial 越过基线 (新 SHM 帧已到) 再撤 compositor key (恢复合成),
// 避免合成到 ZC 前的旧 SHM 帧。broker 的 attached 集合 (IPC 簿记) 由
// Attach/DetachZeroCopyTarget 独立维护, 不参与合成判定 (CompositorLayer::
// zcActive 是唯一消费字段)。所有方法从渲染线程调用 (原调用点上下文不变);
// SetEnabled 内含 tmgr 锁, begin/confirm/cancel/release/bind/查询均无额外锁 —
// 与原实现一致。
// ============================================================================

void ZcBridge::Activate(uint64_t surfaceKey, uint32_t rendererToplevelId)
{
    auto& st = publishStates_[surfaceKey];
    if (st.readyPublished) return;
    SetEnabled(surfaceKey, true);  // 原 SetSurfaceZeroCopy(key,true): 插 activeKeys_ + dirty + signature=0
    winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(surfaceKey, true);
    st.readyPublished = true;
    OH_LOG_INFO(LOG_APP,
                "[VIRGL-ZC][MAIN] GPU_ACTIVE tl=%{public}u key=%{public}llu",
                rendererToplevelId,
                static_cast<unsigned long long>(surfaceKey));
}

void ZcBridge::BeginFallback(uint64_t surfaceKey, uint64_t shmBaseline,
                             bool baselineValid, uint32_t rendererToplevelId)
{
    auto& st = publishStates_[surfaceKey];
    if (!st.readyPublished) return;
    // 原失败调用点 :313-315 — 仅 GetZeroCopyLayerInfo 成功才抓 baseline
    // (shmBaseline=layer.shmCommitSerial); 失败时保留上次记录值 (0 或
    // attach 时初值)
    if (baselineValid) st.fallbackShmSerial = shmBaseline;
    winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(surfaceKey, false);
    st.readyPublished = false;
    st.fallbackPending = true;  // 原调用点 :317
    OH_LOG_WARN(LOG_APP,
                "[VIRGL-ZC][MAIN] ready revoked tl=%{public}u key=%{public}llu",
                rendererToplevelId,
                static_cast<unsigned long long>(surfaceKey));
}

bool ZcBridge::ConfirmFallback(uint64_t surfaceKey, uint64_t shmSerial)
{
    const auto it = publishStates_.find(surfaceKey);
    if (it == publishStates_.end() || !it->second.fallbackPending ||
        shmSerial <= it->second.fallbackShmSerial)
        return false;
    // SetEnabled 内部有 surfaceKey 检查, erase 不存在的 key 是 no-op — 天然幂等
    SetEnabled(surfaceKey, false);  // 原 ClearZeroCopyCompositorKey
    it->second.fallbackPending = false;  // 原调用点 :151
    return true;
}

void ZcBridge::CancelFallback(uint64_t surfaceKey)
{
    auto it = publishStates_.find(surfaceKey);
    if (it == publishStates_.end() || !it->second.fallbackPending) return;
    it->second.fallbackPending = false;  // 原调用点 :377
}

void ZcBridge::Release(uint64_t surfaceKey, uint32_t rendererToplevelId)
{
    // 原 EglRenderer::ReleaseZeroCopyBinding 状态复位序列 (:464-465/:467/:506):
    // 撤 ready (未发布过则整体是 no-op, 无日志) → 清 compositor key
    // (key=0 时 SetEnabled 内部 no-op) → 状态位全清。
    auto& st = publishStates_[surfaceKey];
    if (st.readyPublished)
    {
        winehua::GraphicsBroker::GetInstance().SetZeroCopySurfaceReady(surfaceKey, false);
        st.readyPublished = false;
        OH_LOG_WARN(LOG_APP,
                    "[VIRGL-ZC][MAIN] ready revoked tl=%{public}u key=%{public}llu",
                    rendererToplevelId,
                    static_cast<unsigned long long>(surfaceKey));
    }
    SetEnabled(surfaceKey, false);
    st = {};
}

void ZcBridge::BindSurface(uint64_t surfaceKey, uint64_t initialShmBaseline)
{
    // 原 TryAttachZeroCopySurface 成功路径 (:252-253): pending 复位 + 记录
    // 初始 shm commit serial 作为下次 fallback 的基线起点; readyPublished
    // 不清零 (原代码 attach 路径从不写该位, 该位只在 Release/Activate/
    // BeginFallback 维护, attach 成功时恒为 false — 保持等价)。
    auto& st = publishStates_[surfaceKey];
    st.fallbackPending = false;
    st.fallbackShmSerial = initialShmBaseline;
}

bool ZcBridge::IsReadyPublished(uint64_t surfaceKey) const
{
    const auto it = publishStates_.find(surfaceKey);
    return it != publishStates_.end() && it->second.readyPublished;
}

bool ZcBridge::IsFallbackPending(uint64_t surfaceKey) const
{
    const auto it = publishStates_.find(surfaceKey);
    return it != publishStates_.end() && it->second.fallbackPending;
}

uint64_t ZcBridge::GetFallbackShmSerial(uint64_t surfaceKey) const
{
    const auto it = publishStates_.find(surfaceKey);
    return it == publishStates_.end() ? 0 : it->second.fallbackShmSerial;
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
        // 父几何读点 (重构第 5A2 步): 旧读 parent->geoX/geoY (即时窗口几何值),
        // 新读 parent->committed.contentRect.x/y — 同一写入点
        // (xs_set_window_geometry 直写快照) 的同步表达式, 逐点等价。
        // 偏移公式收口 (重构第 5B2 步): geometry.h ComputePopupOffset 单点
        // (PLAN §2.3 4 份公式), 算法逐字 (offX = subX - parentContentX)
        const auto [subOffX, subOffY] =
            ComputePopupOffset(sd->subsurfaceX, sd->subsurfaceY,
                               parent->committed.contentRect.x,
                               parent->committed.contentRect.y);
        info.x = subOffX;
        info.y = subOffY;
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

    // 遮挡源改遍历 BuildLayerListLocked 层列表 (层序单一数据源), 遮挡锚 =
    // ZC 父窗口的 Toplevel 层 zIndex (层列表按 zIndex 升序即绘制序 — 父层
    // 之后的所有可见非 ZC 层均在本层之上)。锚取父层而非 ZC 层自身:
    //   - 同父 subsurface (菜单) 恒 > 父层锚 → 纳入, 且不再依赖 ZC 层自身的
    //     zIndex 位置 (菜单 attach 早于 ZC 层时 zIndex 更低但仍在 ZC 内容上);
    //   - ProtocolOnly ZC 层 (无 SubsurfaceLayer, 不在层列表): 锚仍可找到,
    //     保留旧扫描语义, 不再因 "找不到 ZC 层" 整组退化为空;
    //   - 父==root 时锚 = Root 层 zIndex (0): Root 之后全部纳入 — 即旧
    //     zbegin=begin() 全扫 toplevel 语义。
    // 置顶层 (菜单/任务栏) zIndex 最大 → 恒纳入, 与 BuildLayerListLocked
    // "菜单恒置顶" 层序一致 — 对低 z-order 父窗口的置顶菜单属语义修正
    // (旧 ZOrderNeedsParentPosCheck 需父 z-order 位置 >= ZC 父位置才判遮挡)。
    // 性能: 每次调用构建一次层列表 (O(n) 拷贝), 调用频率 = GL overlay 遮挡
    // 重绘路径; 不引缓存 (超出本次范围, 需独立评审)。
    const auto layers = comp_.BuildLayerListLocked(rootW, rootH);
    size_t anchorZ = 0;  // 父==root: 锚 = Root 层 zIndex (0)
    bool anchorFound = (info.parentToplevel == comp_.desktopRootToplevelId_);
    if (!anchorFound) {
        for (const auto& layer : layers) {
            if (layer.type == DesktopCompositor::CompositorLayer::Type::Toplevel &&
                layer.toplevelId == info.parentToplevel) {
                anchorZ = layer.zIndex;
                anchorFound = true;
                break;
            }
        }
    }
    if (!anchorFound) return 0;  // 父窗口 Toplevel 层不在列表, 保守不出遮挡者

    for (const auto& layer : layers) {
        if (!layer.visible) continue;
        if (layer.type == DesktopCompositor::CompositorLayer::Type::Root) continue;
        if (layer.zcActive) continue;  // 跳过所有 ZC 层 (旧 activeKeys_ 检查同义)
        if (layer.zIndex <= anchorZ) continue;
        if (layer.type == DesktopCompositor::CompositorLayer::Type::Toplevel) {
            if (layer.fullscreen) pushRect(0, 0, rootW, rootH);
            else pushRect(layer.x, layer.y, layer.w, layer.h);
        } else {
            // Subsurface: x/y 已 Resolve 为桌面坐标; 尺寸取 vpDst 裁剪后几何
            // (层字段 w/h 是原 buffer 尺寸, 与旧实现取法一致 — 不用 layer.w/h)。
            pushRect(layer.x, layer.y,
                     DisplaySizeAfterViewport(layer.sub->vpDstW, layer.sub->w),
                     DisplaySizeAfterViewport(layer.sub->vpDstH, layer.sub->h));
        }
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

#include "desktop_compositor.h"
#include "toplevel_manager.h"
#include "compositor_utils.h"
#include "geometry.h"
#include "compositor/surface_data.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

DesktopCompositor::DesktopCompositor(ToplevelManager& tmgr,
                                     const DisplayPolicy& policy,
                                     const uint32_t& desktopRootToplevelId,
                                     const int32_t& outputW,
                                     const int32_t& outputH)
    : tmgr_(tmgr)
    , policy_(policy)
    , desktopRootToplevelId_(desktopRootToplevelId)
    , outputW_(outputW)
    , outputH_(outputH)
{
}

void DesktopCompositor::MarkDesktopRootDirtyLocked()
{
    tmgr_.MarkToplevelDirtyLocked(desktopRootToplevelId_);
}

void DesktopCompositor::UpdateSubsurfaceLayerLocalPosition(wl_resource* surface, int32_t x, int32_t y)
{
    for (auto& layer : subsurfaceLayers_) {
        if (layer.surface == surface) {
            layer.localX = x;
            layer.localY = y;
            return;
        }
    }
}

bool DesktopCompositor::RemoveSubsurfaceLayer(wl_resource* surface)
{
    auto it = std::find_if(subsurfaceLayers_.begin(), subsurfaceLayers_.end(),
                           [surface](const SubsurfaceLayer& l) { return l.surface == surface; });
    if (it == subsurfaceLayers_.end()) return false;
    subsurfaceLayers_.erase(it);
    return true;
}

std::vector<uint8_t> DesktopCompositor::UpsertSubsurfaceLayer(
    SubsurfaceLayer&& layer, std::vector<uint8_t>&& newPixels)
{
    for (auto& l : subsurfaceLayers_) {
        if (l.surface == layer.surface) {
            auto oldPixels = std::move(l.pixels);
            l = std::move(layer);
            l.pixels = std::move(newPixels);
            return oldPixels;
        }
    }
    layer.pixels = std::move(newPixels);
    subsurfaceLayers_.push_back(std::move(layer));
    return {};
}

bool DesktopCompositor::ReorderSubsurfaceLayerAbove(wl_resource* child, wl_resource* sibling)
{
    int myIdx = -1, siblingIdx = -1;
    for (size_t i = 0; i < subsurfaceLayers_.size(); i++) {
        if (subsurfaceLayers_[i].surface == child) myIdx = static_cast<int>(i);
        if (subsurfaceLayers_[i].surface == sibling) siblingIdx = static_cast<int>(i);
    }
    if (myIdx < 0 || siblingIdx < 0 || myIdx == siblingIdx + 1) return false;
    int target = siblingIdx;
    if (myIdx < target) target--;
    auto layer = std::move(subsurfaceLayers_[myIdx]);
    subsurfaceLayers_.erase(subsurfaceLayers_.begin() + myIdx);
    subsurfaceLayers_.insert(subsurfaceLayers_.begin() + target + 1, std::move(layer));
    return true;
}

bool DesktopCompositor::ReorderSubsurfaceLayerBelow(wl_resource* child, wl_resource* sibling)
{
    int myIdx = -1, siblingIdx = -1;
    for (size_t i = 0; i < subsurfaceLayers_.size(); i++) {
        if (subsurfaceLayers_[i].surface == child) myIdx = static_cast<int>(i);
        if (subsurfaceLayers_[i].surface == sibling) siblingIdx = static_cast<int>(i);
    }
    if (myIdx < 0 || siblingIdx < 0 || myIdx == siblingIdx - 1) return false;
    int target = siblingIdx;
    if (myIdx > target) target++;
    auto layer = std::move(subsurfaceLayers_[myIdx]);
    subsurfaceLayers_.erase(subsurfaceLayers_.begin() + myIdx);
    subsurfaceLayers_.insert(subsurfaceLayers_.begin() + target, std::move(layer));
    return true;
}

void DesktopCompositor::RemoveZeroCopyKeyLocked(uint64_t surfaceKey)
{
    zeroCopySurfaceKeys_.erase(surfaceKey);
    zeroCopyProtocolGeometryLogged_.erase(surfaceKey);
}

bool DesktopCompositor::HasZeroCopyLayerForToplevelLocked(uint32_t id) const
{
    for (const auto& layer : subsurfaceLayers_)
        if (layer.parentToplevel == id && zeroCopySurfaceKeys_.count(layer.surfaceKey))
            return true;
    return false;
}

bool DesktopCompositor::GetZeroCopyContentSizeLocked(uint32_t toplevelId,
                                                     int& outW, int& outH) const
{
    // 与 HasZeroCopyLayerForToplevelLocked 同一层集合判定; 内容尺寸取
    // vpDst 裁剪后几何, 与 GetZeroCopyLayerInfo (egl_renderer 渲染视口
    // 缓存 zeroCopyLayerW_/H_ 的来源) 完全同规则 — 保证输入 fit 与渲染
    // 显示严格互逆。
    for (const auto& layer : subsurfaceLayers_) {
        if (layer.parentToplevel != toplevelId ||
            !zeroCopySurfaceKeys_.count(layer.surfaceKey))
            continue;
        outW = layer.vpDstW > 0 ? layer.vpDstW : layer.w;
        outH = layer.vpDstH > 0 ? layer.vpDstH : layer.h;
        return true;
    }
    return false;
}

std::vector<DesktopCompositor::CompositorLayer> DesktopCompositor::BuildLayerListLocked(int rootW, int rootH)
{
    std::vector<CompositorLayer> layers;
    const uint32_t rootId = desktopRootToplevelId_;
    size_t zIndex = 0;

    {
        CompositorLayer rootLayer;
        rootLayer.type = CompositorLayer::Type::Root;
        rootLayer.zIndex = zIndex++;
        rootLayer.visible = true;
        rootLayer.w = rootW;
        rootLayer.h = rootH;
        layers.push_back(std::move(rootLayer));
    }

    // toplevel 层 (z-order 升序) + 各窗口的 subsurface 层挂在其父窗口层内
    // (文档 §4.2): z-order 更高的 toplevel 自然盖住低窗口的 subsurface —
    // 修复"GL 画面 (subsurface) 永远置顶、无法被其它窗口遮挡"。
    // root 由 Root 层表示, 不在 z-order 里重复。
    // 可见性判定与原合成/输入循环同源 (IsToplevelVisibleLocked)。
    for (uint32_t childId : tmgr_.toplevelZOrder()) {
        if (childId == rootId) continue;
        const auto* cst = tmgr_.FindToplevelLocked(childId);
        if (!cst) continue;
        CompositorLayer layer;
        layer.type = CompositorLayer::Type::Toplevel;
        layer.zIndex = zIndex++;
        layer.visible = tmgr_.IsToplevelVisibleLocked(childId, rootId);
        layer.toplevelId = childId;
        layer.x = cst->X();
        layer.y = cst->Y();
        layer.w = cst->Width();
        layer.h = cst->Height();
        layer.fullscreen = cst->IsFullscreen();
        layers.push_back(std::move(layer));

        // 该窗口的 subsurface 层 (按 subsurfaceLayers_ 原顺序, zIndex 紧随
        // 父窗口)。位置已 Resolve 为桌面坐标; zcActive 由 zeroCopySurfaceKeys_
        // 派生 (合成/输入跳过, GPU 内容由 egl_renderer 绘制)。
        for (const auto& sl : subsurfaceLayers_) {
            if (sl.parentToplevel != childId) continue;
            CompositorLayer subLayer;
            subLayer.type = CompositorLayer::Type::Subsurface;
            subLayer.zIndex = zIndex++;
            subLayer.visible = (sl.parentToplevel == rootId) ||
                               tmgr_.IsToplevelVisibleLocked(sl.parentToplevel, rootId);
            subLayer.zcActive = zeroCopySurfaceKeys_.count(sl.surfaceKey) > 0;
            subLayer.toplevelId = sl.parentToplevel;
            int lx = 0, ly = 0;
            ResolveSubsurfaceLayerPositionLocked(sl, lx, ly);
            subLayer.x = lx;
            subLayer.y = ly;
            subLayer.w = sl.w;
            subLayer.h = sl.h;
            subLayer.sub = &sl;
            layers.push_back(std::move(subLayer));
        }
    }

    // 未跟随 toplevel 的 subsurface (parent==root / 不在 z-order): 追加尾部,
    // 保持旧置顶语义 (任务栏等外部层, 避免沉底回归)。
    for (const auto& sl : subsurfaceLayers_) {
        if (sl.parentToplevel == rootId || !tmgr_.IsInZOrder(sl.parentToplevel)) {
            CompositorLayer subLayer;
            subLayer.type = CompositorLayer::Type::Subsurface;
            subLayer.zIndex = zIndex++;
            subLayer.visible = (sl.parentToplevel == rootId) ||
                               tmgr_.IsToplevelVisibleLocked(sl.parentToplevel, rootId);
            subLayer.zcActive = zeroCopySurfaceKeys_.count(sl.surfaceKey) > 0;
            subLayer.toplevelId = sl.parentToplevel;
            int lx = 0, ly = 0;
            ResolveSubsurfaceLayerPositionLocked(sl, lx, ly);
            subLayer.x = lx;
            subLayer.y = ly;
            subLayer.w = sl.w;
            subLayer.h = sl.h;
            subLayer.sub = &sl;
            layers.push_back(std::move(subLayer));
        }
    }
    return layers;
}

std::vector<DesktopCompositor::CompositorLayer>
DesktopCompositor::BuildWindowLayerListLocked(uint32_t toplevelId, int winW, int winH)
{
    std::vector<CompositorLayer> layers;
    size_t zIndex = 0;

    {
        CompositorLayer rootLayer;
        rootLayer.type = CompositorLayer::Type::Root;
        rootLayer.zIndex = zIndex++;
        rootLayer.visible = true;
        rootLayer.w = winW;
        rootLayer.h = winH;
        layers.push_back(std::move(rootLayer));
    }

    // 窗口内 subsurface 层 (窗口局部坐标)。PC 模式 subsurface 全部转 popup
    // 伪 toplevel (UpdatePopupOnCommit), 这里当前恒空 — 层序结构为窗口内
    // 内容扩展预留; 若未来窗口内 layer 化, 按协议顺序 zIndex 递增。
    for (const auto& sl : subsurfaceLayers_) {
        if (sl.parentToplevel != toplevelId) continue;
        if (sl.isExternal) continue;  // 外部层 (Wine 虚拟屏幕坐标), 不属于窗口内容
        CompositorLayer subLayer;
        subLayer.type = CompositorLayer::Type::Subsurface;
        subLayer.zIndex = zIndex++;
        subLayer.visible = tmgr_.IsToplevelVisibleLocked(toplevelId, desktopRootToplevelId_);
        subLayer.zcActive = zeroCopySurfaceKeys_.count(sl.surfaceKey) > 0;
        subLayer.toplevelId = toplevelId;
        subLayer.x = sl.localX;
        subLayer.y = sl.localY;
        subLayer.w = sl.w;
        subLayer.h = sl.h;
        subLayer.sub = &sl;
        layers.push_back(std::move(subLayer));
    }

    // ZC 层 (窗口内最顶): 该窗口的 GPU 内容。形态二选一 — toplevel surface
    // (整窗口, GL 窗口 attach 自身) / subsurface (内嵌子表面, 局部几何,
    // 与 GetZeroCopyLayerInfo PC 分支同规则)。合成跳过 (GPU 自绘覆盖,
    // 与 desktop 模式同语义: CPU 帧保留 SHM 内容, 不抠除 — GPU 帧不透明
    // 时覆盖等价, fallback 窗口期显示旧 SHM 内容比黑屏稳)。
    for (uint64_t key : zeroCopySurfaceKeys_) {
        auto* wlRes = tmgr_.FindSurfaceResource(key);
        if (!wlRes) continue;
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(wlRes));
        if (!sd) continue;
        CompositorLayer zcLayer;
        zcLayer.zcActive = true;
        zcLayer.visible = true;
        if (sd->hasToplevel) {
            if (sd->toplevelId != toplevelId) continue;
            zcLayer.type = CompositorLayer::Type::Toplevel;
            zcLayer.x = 0;
            zcLayer.y = 0;
            zcLayer.w = winW;
            zcLayer.h = winH;
        } else if (sd->isSubsurface && sd->parentSurface) {
            auto* parent = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
            if (!parent || parent->toplevelId != toplevelId) continue;
            zcLayer.type = CompositorLayer::Type::Subsurface;
            zcLayer.x = sd->subsurfaceX - parent->geoX;
            zcLayer.y = sd->subsurfaceY - parent->geoY;
            zcLayer.w = sd->vpDstW > 0 ? sd->vpDstW : sd->w;
            zcLayer.h = sd->vpDstH > 0 ? sd->vpDstH : sd->h;
        } else {
            continue;
        }
        zcLayer.toplevelId = toplevelId;
        zcLayer.zIndex = zIndex++;
        layers.push_back(std::move(zcLayer));
    }
    return layers;
}

uint32_t DesktopCompositor::PickFullscreenLayerLocked(
    const std::vector<CompositorLayer>& layers) const
{
    // 全屏目标选取 (阶段 4, S3 收敛): 渲染与输入共用的唯一实现, 遍历
    // 同一 Layer 列表 — 可见全屏窗口中取 fsPriority 最大者 (多窗口可
    // 同时 fullscreen, 规则原因/局限见 ToplevelState::fsPriority 注释)
    uint32_t picked = 0;
    const ToplevelManager::ToplevelState* best = nullptr;
    for (const auto& layer : layers) {
        if (layer.type != CompositorLayer::Type::Toplevel ||
            !layer.visible || !layer.fullscreen) continue;
        const auto* cand = tmgr_.FindToplevelLocked(layer.toplevelId);
        if (!cand) continue;
        if (!best || cand->FsPriority() > best->FsPriority()) {
            best = cand;
            picked = layer.toplevelId;
        }
    }
    return picked;
}

bool DesktopCompositor::ComputeFullscreenFitLocked(uint32_t toplevelId, int rootW, int rootH,
                                                   FitRect& out) const
{
    // 全屏内容尺寸选择 (ZC 游戏用 zero-copy 层实际内容几何, SHM 用 buffer
    // 尺寸) + 保比例 fit 的唯一实现 — 替换渲染 (TakeToplevelFrame) 与输入
    // (FindInputTargetAt / SurfaceLocalToDesktop) 各自的组合。
    // 注: 合成侧此前直接用 buffer 尺寸 (ZC 分支填黑不消费 transform,
    // SHM 分支与 SelectFullscreenContentSize 结果等价); 统一后 ZC 游戏
    // 的 transform 按 layer 几何计算 — 与输入命中/GPU 层几何同源 (修正
    // 而非回归: 旧输入侧用 preFs 快照, 与渲染 layer 几何可失配, 曾导致
    // 全屏游戏光标常数平移偏移, 见 geometry.h SelectFullscreenContentSize
    // 注释; preFs 已从输入路径移除)。
    const auto* st = tmgr_.FindToplevelLocked(toplevelId);
    if (!st) return false;
    int layerW = 0, layerH = 0;
    const bool hasZC = GetZeroCopyContentSizeLocked(toplevelId, layerW, layerH);
    int contentW = 0, contentH = 0;
    SelectFullscreenContentSize(layerW, layerH, st->Width(), st->Height(), hasZC,
                                contentW, contentH);
    return ComputeFitRect(rootW, rootH, contentW, contentH, out);
}

bool DesktopCompositor::ShouldSkipFullscreenCascade(const CompositorLayer& layer,
                                                    uint32_t fullscreenId, bool fsOk,
                                                    ToplevelManager& tmgr)
{
    if (!fsOk || layer.toplevelId == fullscreenId) return false;
    if (layer.type == CompositorLayer::Type::Toplevel)
        return layer.fullscreen;
    if (layer.type == CompositorLayer::Type::Subsurface) {
        const auto* parent = tmgr.FindToplevelLocked(layer.toplevelId);
        return parent && parent->IsFullscreen();
    }
    return false;
}

void DesktopCompositor::ResolveSubsurfaceLayerPositionLocked(
    const SubsurfaceLayer& layer, int& x, int& y) const
{
    x = layer.x;
    y = layer.y;
    if (layer.isExternal) return;

    const auto it = tmgr_.toplevels().find(layer.parentToplevel);
    if (it != tmgr_.toplevels().end() && it->second.HasPosition()) {
        x = it->second.X() + layer.localX;
        y = it->second.Y() + layer.localY;
    }
}

bool DesktopCompositor::GetZeroCopyLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                                             int fallbackWidth, int fallbackHeight,
                                             ZeroCopyLayerInfo& info)
{
    auto lk = tmgr_.Lock();
    auto* wlRes = tmgr_.FindSurfaceResource(surfaceKey);
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
        info.width = sd->vpDstW > 0 ? sd->vpDstW : sd->w;
        info.height = sd->vpDstH > 0 ? sd->vpDstH : sd->h;
        if (policy_.RootCompositing())
        {
            if (rendererToplevelId != desktopRootToplevelId_ ||
                (info.parentToplevel != desktopRootToplevelId_ &&
                 !tmgr_.IsToplevelVisibleLocked(info.parentToplevel, desktopRootToplevelId_)))
                return false;
            for (const auto& layer : subsurfaceLayers_)
            {
                if (layer.surface != wlRes) continue;
                ResolveSubsurfaceLayerPositionLocked(layer, info.x, info.y);
                info.width = layer.vpDstW > 0 ? layer.vpDstW : layer.w;
                info.height = layer.vpDstH > 0 ? layer.vpDstH : layer.h;
                info.shmCommitSerial = layer.shmCommitSerial;
                info.desktopCoordinates = true;
                if (const auto* pst = tmgr_.FindToplevelLocked(layer.parentToplevel))
                    info.fullscreen = pst->IsFullscreen();
                info.protocolOnly = false;
                return info.width > 0 && info.height > 0;
            }

            // Vulkan private-present surfaces may have no wl_shm commit. Wayland
            // still supplies the parent/offset while the present protocol supplies
            // the image dimensions.
            int sx = sd->subsurfaceX;
            int sy = sd->subsurfaceY;
            const auto* parentState = tmgr_.FindToplevelLocked(info.parentToplevel);
            if (parentState && parentState->IsMinimized()) {
                if (sx > 16000) sx -= 32000;
                if (sy > 16000) sy -= 32000;
            }
            const int compX = parentState ? parentState->X() : 0;
            const int compY = parentState ? parentState->Y() : 0;
            const int wineX = parentState ? parentState->WineX() : 0;
            const int wineY = parentState ? parentState->WineY() : 0;
            const int compW = parentState ? parentState->Width() : 0;
            const int compH = parentState ? parentState->Height() : 0;
            const bool insideWin = sx >= 0 && sx < compW && sy >= 0 && sy < compH;
            info.x = (insideWin ? compX : wineX) + sx;
            info.y = (insideWin ? compY : wineY) + sy;
            info.width = sd->vpDstW > 0 ? sd->vpDstW : sd->w;
            info.height = sd->vpDstH > 0 ? sd->vpDstH : sd->h;
            if (info.width <= 0) info.width = fallbackWidth;
            if (info.height <= 0) info.height = fallbackHeight;
            info.shmCommitSerial = sd->shmCommitSerial.load(std::memory_order_acquire);
            info.desktopCoordinates = true;
            info.protocolOnly = true;
            if (parentState) info.fullscreen = parentState->IsFullscreen();
            if (zeroCopyProtocolGeometryLogged_.insert(surfaceKey).second) {
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
    if (policy_.RootCompositing())
    {
        if (rendererToplevelId != desktopRootToplevelId_ ||
            (sd->toplevelId != desktopRootToplevelId_ && !tmgr_.IsToplevelVisibleLocked(sd->toplevelId, desktopRootToplevelId_)))
            return false;
        if (const auto* st = tmgr_.FindToplevelLocked(sd->toplevelId)) {
            info.x = st->X();
            info.y = st->Y();
            info.fullscreen = st->IsFullscreen();
        }
        info.desktopCoordinates = true;
        return info.width > 0 && info.height > 0;
    }
    return rendererToplevelId == sd->toplevelId && info.width > 0 && info.height > 0;
}

void DesktopCompositor::SetSurfaceZeroCopy(uint64_t surfaceKey, bool enabled)
{
    if (!surfaceKey) return;
    auto lk = tmgr_.Lock();
    if (enabled)
        zeroCopySurfaceKeys_.insert(surfaceKey);
    else
        zeroCopySurfaceKeys_.erase(surfaceKey);
    MarkDesktopRootDirtyLocked();
    desktopCompositionSignature_ = 0;
}

int DesktopCompositor::GetZeroCopyOccluders(uint64_t surfaceKey, uint32_t rendererToplevelId,
                                            ZeroCopyOccluderRect* out, int maxOut)
{
    if (!out || maxOut <= 0) return 0;
    ZeroCopyLayerInfo info;
    if (!GetZeroCopyLayerInfo(surfaceKey, rendererToplevelId, 0, 0, info) ||
        !info.desktopCoordinates)
        return 0;

    auto lk = tmgr_.Lock();
    const int layerL = info.x;
    const int layerT = info.y;
    const int layerR = info.x + info.width;
    const int layerB = info.y + info.height;
    const auto* rootSt = tmgr_.FindToplevelLocked(desktopRootToplevelId_);
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

    auto zbegin = tmgr_.toplevelZOrder().begin();
    auto zcIt = tmgr_.toplevelZOrder().end();
    if (info.parentToplevel != desktopRootToplevelId_) {
        zcIt = std::find(tmgr_.toplevelZOrder().begin(), tmgr_.toplevelZOrder().end(),
                         info.parentToplevel);
        if (zcIt != tmgr_.toplevelZOrder().end()) zbegin = std::next(zcIt);
    }
    for (auto zit = zbegin; zit != tmgr_.toplevelZOrder().end() && count < maxOut; ++zit) {
        const uint32_t cid = *zit;
        if (!tmgr_.IsToplevelVisibleLocked(cid, desktopRootToplevelId_)) continue;
        const auto* cst = tmgr_.FindToplevelLocked(cid);
        if (!cst) continue;
        if (cst->IsFullscreen()) pushRect(0, 0, rootW, rootH);
        else pushRect(cst->X(), cst->Y(), cst->Width(), cst->Height());
    }

    // 新层序 (subsurface 挂父窗口层内, 见 BuildLayerListLocked): 仅父窗口
    // z-order 不低于 ZC 窗口的层遮挡 ZC (同窗口的菜单等仍在 ZC 层之上);
    // parent==root / 不在 z-order 的层保持置顶语义, 仍遮挡。
    for (const auto& layer : subsurfaceLayers_) {
        if (count >= maxOut) break;
        if (zeroCopySurfaceKeys_.count(layer.surfaceKey)) continue;
        if (layer.parentToplevel != desktopRootToplevelId_ &&
            !tmgr_.IsToplevelVisibleLocked(layer.parentToplevel, desktopRootToplevelId_)) continue;
        if (layer.parentToplevel != info.parentToplevel &&
            layer.parentToplevel != desktopRootToplevelId_) {
            const auto pit = std::find(tmgr_.toplevelZOrder().begin(),
                                       tmgr_.toplevelZOrder().end(),
                                       layer.parentToplevel);
            if (pit == tmgr_.toplevelZOrder().end() || pit < zcIt) continue;
        }
        int x = 0, y = 0;
        ResolveSubsurfaceLayerPositionLocked(layer, x, y);
        pushRect(x, y,
                 layer.vpDstW > 0 ? layer.vpDstW : layer.w,
                 layer.vpDstH > 0 ? layer.vpDstH : layer.h);
    }
    return count;
}

// ============================================================================
// TakeToplevelFrame: 桌面合成核心 (~390 行)
// ============================================================================

bool DesktopCompositor::TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, int& w, int& h) {
    struct TakeBreakdownWindow {
        uint64_t count = 0;
        uint64_t sums[6] = {};
        uint64_t maxima[6] = {};

        void Add(uint64_t lockWait, uint64_t rootCopy, uint64_t children,
                 uint64_t subsurfaces, uint64_t output, uint64_t total) {
            const uint64_t values[6] = {lockWait, rootCopy, children, subsurfaces, output, total};
            for (size_t i = 0; i < 6; ++i) {
                sums[i] += values[i];
                maxima[i] = std::max(maxima[i], values[i]);
            }
            if (++count != 120) return;
            OH_LOG_INFO(LOG_APP,
                        "[GL-TAKE] samples=120 avg_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu "
                        "max_us=%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu/%{public}llu",
                        static_cast<unsigned long long>(sums[0] / count),
                        static_cast<unsigned long long>(sums[1] / count),
                        static_cast<unsigned long long>(sums[2] / count),
                        static_cast<unsigned long long>(sums[3] / count),
                        static_cast<unsigned long long>(sums[4] / count),
                        static_cast<unsigned long long>(sums[5] / count),
                        static_cast<unsigned long long>(maxima[0]),
                        static_cast<unsigned long long>(maxima[1]),
                        static_cast<unsigned long long>(maxima[2]),
                        static_cast<unsigned long long>(maxima[3]),
                        static_cast<unsigned long long>(maxima[4]),
                        static_cast<unsigned long long>(maxima[5]));
            count = 0;
            for (size_t i = 0; i < 6; ++i) {
                sums[i] = 0;
                maxima[i] = 0;
            }
        }
    };
    static TakeBreakdownWindow breakdown;

    using TakeClock = std::chrono::steady_clock;
    const auto takeStarted = TakeClock::now();
    auto lk = tmgr_.Lock();
    const auto lockAcquired = TakeClock::now();

    if (policy_.RootCompositing() && id == desktopRootToplevelId_) {
        auto* rst = tmgr_.FindToplevelLocked(id);
        if (!rst || !rst->HasFrame()) return false;
        if (!rst->IsDirty()) return false;

        int rootW = rst->Width();
        int rootH = rst->Height();

        bool hasChildren = false;
        for (uint32_t cid : tmgr_.toplevelZOrder()) {
            if (cid == id) continue;
            const auto* cst = tmgr_.FindToplevelLocked(cid);
            if (cst && cst->HasFrame()) {
                hasChildren = true;
                break;
            }
        }
        if (!hasChildren && subsurfaceLayers_.empty()) {
            out = rst->Pixels();
            w = rootW;
            h = rootH;
            rst->ClearDirty();
            return true;
        }

        // 层序单一数据源 (阶段 1): 一帧全部内容来源的层列表, 构建一次,
        // fs-pick / 覆盖判定 / 签名 / 合成共用。zIndex: root < toplevel <
        // subsurface — 顺序与旧双循环等价 (见 CompositorLayer 注释)。
        const auto layers = BuildLayerListLocked(rootW, rootH);

        uint32_t fullscreenId = 0;
        FitRect transform;
        bool hasFullscreen = false;
        // ZC 游戏 (画面在 zero-copy GL 层): 全屏独占输出, 见下方填黑分支
        bool isZcGame = false;
        int fullscreenX = 0, fullscreenY = 0;
        // 全屏目标选取 (阶段 4, S3 收敛): 与输入侧 (FindInputTargetAt) 共用
        // PickFullscreenLayerLocked 单一实现 — 可见全屏窗口中取 fsPriority
        // 最大者 (多窗口可同时 fullscreen, 规则原因/局限见
        // ToplevelState::fsPriority 注释); fit 几何同样共用
        // ComputeFullscreenFitLocked (含内容尺寸选择, 见该函数注释)
        fullscreenId = PickFullscreenLayerLocked(layers);
        const ToplevelManager::ToplevelState* fsWin =
            fullscreenId ? tmgr_.FindToplevelLocked(fullscreenId) : nullptr;
        if (fsWin) {
            fullscreenX = fsWin->X();
            fullscreenY = fsWin->Y();
            hasFullscreen = ComputeFullscreenFitLocked(fullscreenId, rootW, rootH, transform);
            isZcGame = HasZeroCopyLayerForToplevelLocked(fullscreenId);
        }

        bool fullscreenContentCovered = false;
        if (hasFullscreen) {
            const auto* fst = tmgr_.FindToplevelLocked(fullscreenId);
            const int winW = fst ? fst->Width() : 0;
            const int winH = fst ? fst->Height() : 0;
            for (const auto& layer : layers) {
                if (layer.type != CompositorLayer::Type::Subsurface) continue;
                if (layer.toplevelId != fullscreenId) continue;
                if (layer.ShouldSkipCpu()) continue;
                const auto& sl = *layer.sub;
                if (sl.w <= 0 || sl.h <= 0) continue;
                if (sl.shmFormat == 0 && !sl.opaque) continue;
                const int dispW = sl.vpDstW > 0 ? std::min(sl.vpDstW, sl.w) : sl.w;
                const int dispH = sl.vpDstH > 0 ? std::min(sl.vpDstH, sl.h) : sl.h;
                const int relX = layer.x - fullscreenX;
                const int relY = layer.y - fullscreenY;
                if (relX <= 0 && relY <= 0 &&
                    relX + dispW >= winW && relY + dispH >= winH) {
                    fullscreenContentCovered = true;
                    break;
                }
            }
        }

        uint64_t compositionSignature = compositor_consts::kFnv1aOffsetBasis;
        auto mixSignature = [&](uint64_t value) {
            compositionSignature ^= value;
            compositionSignature *= compositor_consts::kFnv1aPrime;
        };
        mixSignature(id);
        mixSignature(static_cast<uint32_t>(rootW));
        mixSignature(static_cast<uint32_t>(rootH));
        // 签名遍历 Layer 列表: 每个可见 toplevel/subsurface 的几何与标记
        // (与旧两个循环 mix 序列等价; 不可见 toplevel 的 (id,0) 不再混入,
        // 仅影响 rebuildBase 触发时机, 不影响输出像素 — 不可见窗口不参与
        // 合成, root 像素变化仍由 desktopRootFrameSerial_ 兜底)。
        for (const auto& layer : layers) {
            if (layer.type == CompositorLayer::Type::Toplevel) {
                mixSignature(layer.toplevelId);
                mixSignature(layer.visible ? 1 : 0);
                if (!layer.visible) continue;
                mixSignature(static_cast<uint32_t>(layer.x));
                mixSignature(static_cast<uint32_t>(layer.y));
                mixSignature(static_cast<uint32_t>(layer.w));
                mixSignature(static_cast<uint32_t>(layer.h));
                mixSignature(layer.fullscreen ? 1 : 0);
            } else if (layer.type == CompositorLayer::Type::Subsurface) {
                mixSignature(reinterpret_cast<uintptr_t>(layer.sub->surface));
                mixSignature(layer.zcActive ? 1 : 0);
                mixSignature(layer.toplevelId);
                mixSignature(layer.visible ? 1 : 0);
                mixSignature(static_cast<uint32_t>(layer.x));
                mixSignature(static_cast<uint32_t>(layer.y));
                mixSignature(static_cast<uint32_t>(layer.w));
                mixSignature(static_cast<uint32_t>(layer.h));
                mixSignature(static_cast<uint32_t>(layer.sub->vpDstW));
                mixSignature(static_cast<uint32_t>(layer.sub->vpDstH));
            }
        }

        const size_t rootBytes = static_cast<size_t>(rootW) * rootH * 4;
        const bool rebuildBase = !desktopOutputInitialized_ ||
            out.size() != rootBytes ||
            desktopOutputRootFrameSerial_ != desktopRootFrameSerial_ ||
            desktopCompositionSignature_ != compositionSignature;
        if (rebuildBase) {
            out = rst->Pixels();
            desktopOutputInitialized_ = true;
            desktopOutputRootFrameSerial_ = desktopRootFrameSerial_;
            desktopCompositionSignature_ = compositionSignature;
        }
        auto& composited = out;
        const auto rootCopied = TakeClock::now();

        // 合成单循环 (阶段 1): 按 zIndex 升序遍历 Layer 列表 — 等价旧
        // toplevel 循环 + subsurface 循环的两段顺序 (Layer zIndex 分配保证)。
        // 全屏独占/跳过特判原样保留 (等价形式), 行为不变。
        auto blitToplevel = [&](const CompositorLayer& layer) {
            if (layer.ShouldSkipCpu()) return;
            // 跳过非主全屏的 toplevel: SHM 游戏只跳过被连带标 fullscreen 的
            // 旧窗口 (notepad/explorer 等, 显示模式切换时 winewayland 批量
            // 标记, fsPriority 选了游戏但它仍在 z-order 高位, 普通 blit 会
            // 盖在游戏上面), 非全屏弹窗/对话框保留。规则单一实现:
            // ShouldSkipFullscreenCascade (与输入命中同源)
            if (ShouldSkipFullscreenCascade(layer, fullscreenId, hasFullscreen, tmgr_)) return;
            auto* cst = tmgr_.FindToplevelLocked(layer.toplevelId);
            if (!cst) return;
            const auto& childPx = cst->Pixels();
            int childW = cst->Width();
            int childH = cst->Height();
            int posX = cst->X();
            int posY = cst->Y();
            if (layer.toplevelId == fullscreenId && hasFullscreen) {
                if (isZcGame) {
                    // ZC 游戏: 整幅填黑, 跳过 SHM BlitScaled — 其 SHM 内容是
                    // explorer 桌面而非游戏画面, 实际画面由 GL ZC 层渲染
                    // (egl_renderer zeroCopyFullscreen_ 路径)。
                    // 必须填不透明黑 0xFF000000, 不能图省事 memset 0:
                    // 渲染 context 不开 GL_BLEND 时 alpha=0 恰好无害, 但那是
                    // 隐式依赖 — 一旦以后给桌面纹理开混合, 黑边就会变透明
                    std::fill_n(reinterpret_cast<uint32_t*>(composited.data()),
                                composited.size() / 4, 0xFF000000u);
                    return;
                }
                auto fillBlackRect = [&](int fx, int fy, int fw, int fh) {
                    if (fw <= 0 || fh <= 0) return;
                    for (int row = fy; row < fy + fh; ++row)
                        std::fill_n(reinterpret_cast<uint32_t*>(composited.data()) +
                                    static_cast<size_t>(row) * rootW + fx, fw, 0xFF000000u);
                };
                const bool contentOpaque = (cst->ShmFormat() != 0) || fullscreenContentCovered;
                if (contentOpaque) {
                    fillBlackRect(0, 0, rootW, transform.offY);
                    fillBlackRect(0, transform.offY + transform.dstH, rootW,
                                  rootH - transform.offY - transform.dstH);
                    fillBlackRect(0, transform.offY, transform.offX, transform.dstH);
                    fillBlackRect(transform.offX + transform.dstW, transform.offY,
                                  rootW - transform.offX - transform.dstW, transform.dstH);
                } else {
                    std::fill_n(reinterpret_cast<uint32_t*>(composited.data()),
                                composited.size() / 4, 0xFF000000u);
                }
                if (!fullscreenContentCovered) {
                    BlitScaled(composited.data(), rootW, rootH,
                               childPx.data(), childW, childW, childH,
                               transform.offX, transform.offY, transform.dstW, transform.dstH,
                               cst->ShmFormat() == 0);
                }
                return;
            }
            int dstX = (posX > 0) ? posX : 0;
            int dstY = (posY > 0) ? posY : 0;
            int srcX = (posX < 0) ? -posX : 0;
            int srcY = (posY < 0) ? -posY : 0;
            int copyW = childW - srcX;
            int copyH = childH - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) return;
            const bool childArgb = (cst->ShmFormat() == 0);
            for (int y = 0; y < copyH; y++) {
                auto* srcRow = &childPx[(srcY + y) * childW * 4];
                auto* dstRow = &composited[(dstY + y) * rootW * 4];
                // SrcOnly 混合语义 (源不乘 alpha, clamp, 目标 alpha 强制 255)
                BlitClipAlpha(&dstRow[dstX * 4], &srcRow[srcX * 4], copyW,
                              childArgb, PixelBlend::SrcOnly);
            }
        };
        auto blitSubsurface = [&](const CompositorLayer& layer) {
            if (layer.ShouldSkipCpu()) return;
            if (layer.w <= 0 || layer.h <= 0) return;
            // 只跳过"父 toplevel 也连带 fullscreen"的 subsurface (显示模式
            // 切换时被 winewayland 批量标记的旧窗口残留菜单, 防盖在游戏上);
            // 非全屏普通窗口 (如游戏上方新弹出的对话框) 的 subsurface 正常
            // 渲染。规则单一实现: ShouldSkipFullscreenCascade (与输入命中同源)
            if (ShouldSkipFullscreenCascade(layer, fullscreenId, hasFullscreen, tmgr_)) return;
            const auto& sl = *layer.sub;
            int layerX = layer.x;
            int layerY = layer.y;
            size_t expectSz = (size_t)sl.w * sl.h * 4;
            if (sl.pixels.size() < expectSz) {
                OH_LOG_WARN(LOG_APP, "[MW-SUBSURF] layer size mismatch: w=%{public}d h=%{public}d px=%{public}zu expected=%{public}zu",
                            sl.w, sl.h, sl.pixels.size(), expectSz);
                return;
            }
            if (hasFullscreen && layer.toplevelId == fullscreenId) {
                const int layerDispW = sl.vpDstW > 0 ? std::min(sl.vpDstW, sl.w) : sl.w;
                const int layerDispH = sl.vpDstH > 0 ? std::min(sl.vpDstH, sl.h) : sl.h;
                // 与输入 FindInputTargetAt 全屏分支同几何 (FitMapLayerRect 唯一实现)
                int layerDstX, layerDstY, layerDstW, layerDstH;
                FitMapLayerRect(transform, layerX - fullscreenX, layerY - fullscreenY,
                                layerDispW, layerDispH,
                                layerDstX, layerDstY, layerDstW, layerDstH);
                BlitScaled(composited.data(), rootW, rootH,
                           sl.pixels.data(), sl.w, layerDispW, layerDispH,
                           layerDstX, layerDstY, layerDstW, layerDstH,
                           sl.shmFormat == 0 && !sl.opaque);
                return;
            }
            int srcX = (layerX < 0) ? -layerX : 0;
            int srcY = (layerY < 0) ? -layerY : 0;
            int dstX = (layerX > 0) ? layerX : 0;
            int dstY = (layerY > 0) ? layerY : 0;
            int copyW = sl.w - srcX;
            int copyH = sl.h - srcY;
            if (dstX + copyW > rootW) copyW = rootW - dstX;
            if (dstY + copyH > rootH) copyH = rootH - dstY;
            if (copyW <= 0 || copyH <= 0) return;
            int renderW = copyW, renderH = copyH;
            int renderSrcX = srcX, renderSrcY = srcY;
            int renderDstX = dstX, renderDstY = dstY;
            if (sl.vpDstW > 0 && sl.vpDstW < copyW) renderW = sl.vpDstW;
            if (sl.vpDstH > 0 && sl.vpDstH < copyH) renderH = sl.vpDstH;
            if (sl.dmgW > 0 && sl.dmgH > 0) {
                const int damageLeft = std::max(renderSrcX, sl.dmgX);
                const int damageTop = std::max(renderSrcY, sl.dmgY);
                const int damageRight = std::min(renderSrcX + renderW, sl.dmgX + sl.dmgW);
                const int damageBottom = std::min(renderSrcY + renderH, sl.dmgY + sl.dmgH);
                if (damageRight <= damageLeft || damageBottom <= damageTop) return;
                renderDstX += damageLeft - renderSrcX;
                renderDstY += damageTop - renderSrcY;
                renderSrcX = damageLeft;
                renderSrcY = damageTop;
                renderW = damageRight - damageLeft;
                renderH = damageBottom - damageTop;
            }
            const bool needsAlphaBlend = sl.shmFormat == 0 && !sl.opaque;
            for (int y = 0; y < renderH; y++) {
                const uint8_t* srcRow = sl.pixels.data() +
                    ((renderSrcY + y) * sl.w + renderSrcX) * 4;
                uint8_t* dstRow = composited.data() +
                    ((renderDstY + y) * rootW + renderDstX) * 4;
                BlitClipAlpha(dstRow, srcRow, renderW, needsAlphaBlend, PixelBlend::Normal);
            }
        };
        for (const auto& layer : layers) {
            switch (layer.type) {
                case CompositorLayer::Type::Root:
                    break;  // 基底已在 rebuildBase 时拷贝
                case CompositorLayer::Type::Toplevel:
                    blitToplevel(layer);
                    break;
                case CompositorLayer::Type::Subsurface:
                    blitSubsurface(layer);
                    break;
            }
        }
        const auto childrenComposited = TakeClock::now();
        // 旧双循环有两个分段时间点; 单循环后合并为一个
        const auto subsurfacesComposited = childrenComposited;

        const auto outputMoved = TakeClock::now();
        auto elapsedUs = [](TakeClock::time_point begin, TakeClock::time_point end) {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin).count());
        };
        breakdown.Add(elapsedUs(takeStarted, lockAcquired),
                      elapsedUs(lockAcquired, rootCopied),
                      elapsedUs(rootCopied, childrenComposited),
                      elapsedUs(childrenComposited, subsurfacesComposited),
                      elapsedUs(subsurfacesComposited, outputMoved),
                      elapsedUs(takeStarted, outputMoved));
        w = rootW;
        h = rootH;
        rst->ClearDirty();
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] root #%{public}u %{public}dx%{public}d children=%{public}zu subsurfaces=%{public}zu",
                    id, w, h, tmgr_.toplevelZOrder().size(), subsurfaceLayers_.size());
        return true;
    }

    auto* st = tmgr_.FindToplevelLocked(id);
    if (!st || !st->IsDirty()) return false;
    const int winW = st->Width();
    const int winH = st->Height();
    if (winW <= 0 || winH <= 0) return false;

    // 窗口内层序 (阶段 3, PC 模式): Root(窗口帧) < Subsurface(窗口局部
    // 坐标) < ZC 层(最顶)。窗口间层序由系统合成器保证, 不在此合成。
    // PC 模式 subsurface 当前恒空 (全部转 popup 伪 toplevel), 合成输出 =
    // 窗口 SHM 帧; ZC 层 (zcActive) 合成跳过 — GPU 内容由 renderer 自绘
    // 覆盖, CPU 帧保留 SHM 内容不抠除 (与 desktop 模式同语义: GPU 帧
    // 不透明时覆盖等价, fallback 窗口期显示旧内容比黑屏稳)。
    const auto layers = BuildWindowLayerListLocked(id, winW, winH);
    out = st->Pixels();
    auto blitWindowSubsurface = [&](const CompositorLayer& layer) {
        const auto& sl = *layer.sub;
        size_t expectSz = (size_t)sl.w * sl.h * 4;
        if (sl.pixels.size() < expectSz) return;
        int srcX = (layer.x < 0) ? -layer.x : 0;
        int srcY = (layer.y < 0) ? -layer.y : 0;
        int dstX = (layer.x > 0) ? layer.x : 0;
        int dstY = (layer.y > 0) ? layer.y : 0;
        int copyW = sl.w - srcX;
        int copyH = sl.h - srcY;
        if (dstX + copyW > winW) copyW = winW - dstX;
        if (dstY + copyH > winH) copyH = winH - dstY;
        if (copyW <= 0 || copyH <= 0) return;
        const bool needsAlphaBlend = sl.shmFormat == 0 && !sl.opaque;
        for (int y = 0; y < copyH; y++) {
            const uint8_t* srcRow = sl.pixels.data() + ((srcY + y) * sl.w + srcX) * 4;
            uint8_t* dstRow = out.data() + ((dstY + y) * winW + dstX) * 4;
            BlitClipAlpha(dstRow, srcRow, copyW, needsAlphaBlend, PixelBlend::Normal);
        }
    };
    for (const auto& layer : layers) {
        switch (layer.type) {
            case CompositorLayer::Type::Root:
                break;  // 基底已在 out = st->pixels 拷贝
            case CompositorLayer::Type::Toplevel:
                break;  // 窗口内 ZC 整窗口层: GPU 自绘, CPU 帧跳过
            case CompositorLayer::Type::Subsurface:
                if (layer.ShouldSkipCpu()) break;  // ZC 子表面 (GPU 自绘) / 不可见: 同上
                blitWindowSubsurface(layer);
                break;
        }
    }
    w = winW;
    h = winH;
    st->ClearDirty();
    OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu",
                id, w, h, out.size());
    return true;
}

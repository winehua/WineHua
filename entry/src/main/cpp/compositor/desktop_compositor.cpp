#include "desktop_compositor.h"
#include "toplevel_manager.h"
#include "compositor_utils.h"
#include "geometry.h"
#include "compositor/surface_data.h"
#include "perf_utils.h"
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
}

const DesktopCompositor::SubsurfaceLayer*
DesktopCompositor::FindZeroCopyLayerForToplevelLocked(uint32_t id) const
{
    for (const auto& layer : subsurfaceLayers_)
        if (layer.parentToplevel == id && zeroCopySurfaceKeys_.count(layer.surfaceKey))
            return &layer;
    return nullptr;
}

bool DesktopCompositor::HasZeroCopyLayerForToplevelLocked(uint32_t id) const
{
    return FindZeroCopyLayerForToplevelLocked(id) != nullptr;
}

bool DesktopCompositor::GetZeroCopyContentSizeLocked(uint32_t toplevelId,
                                                     int& outW, int& outH) const
{
    // 与 HasZeroCopyLayerForToplevelLocked 同一层集合判定 (共用
    // FindZeroCopyLayerForToplevelLocked 单一查找); 内容尺寸取
    // vpDst 裁剪后几何, 与 GetZeroCopyLayerInfo (egl_renderer 渲染视口
    // 缓存 zeroCopyLayerW_/H_ 的来源) 完全同规则 — 保证输入 fit 与渲染
    // 显示严格互逆。
    const auto* layer = FindZeroCopyLayerForToplevelLocked(toplevelId);
    if (!layer) return false;
    outW = DisplaySizeAfterViewport(layer->vpDstW, layer->w);
    outH = DisplaySizeAfterViewport(layer->vpDstH, layer->h);
    return true;
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

    // subsurface 层填充块 (原两份逐字相同的填充体合并, 行为不变 — 仅两个
    // 循环的过滤条件不同): 位置已 Resolve 为桌面坐标; zcActive 由
    // zeroCopySurfaceKeys_ 派生 (合成/输入跳过, GPU 内容由 egl_renderer 绘制);
    // zIndex 与调用点共享同一计数器, 分配顺序与合并前一致。
    auto appendSubsurfaceLayer = [&](const SubsurfaceLayer& sl) {
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
    };

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
        // 父窗口)。弹出式菜单 (isExternal, 跨窗口 offset) 不跟随父窗口 —
        // 统一置顶, 见尾部追加循环。
        for (const auto& sl : subsurfaceLayers_) {
            if (sl.parentToplevel != childId || sl.isExternal) continue;
            appendSubsurfaceLayer(sl);
        }
    }

    // 尾部置顶层: parent==root / 不在 z-order 的旧外部层 (任务栏等,
    // 避免沉底回归) + 所有弹出式菜单 (isExternal)。
    // 菜单恒置顶语义: 菜单挂的父窗口可能是普通应用窗口 (任务栏按钮右键
    // 菜单 owner 是应用窗口), 若跟随父窗口 z-order, 会被置顶 pin 的任务栏
    // 挡住 — 所有菜单都应叠在任务栏上方 (Windows popup 语义, 2026-08 实测)。
    // 渲染与输入共用本列表 (单一数据源), 置顶后点击菜单的命中同步优先。
    for (const auto& sl : subsurfaceLayers_) {
        if (sl.parentToplevel == rootId || sl.isExternal ||
            !tmgr_.IsInZOrder(sl.parentToplevel)) {
            appendSubsurfaceLayer(sl);
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
            zcLayer.w = DisplaySizeAfterViewport(sd->vpDstW, sd->w);
            zcLayer.h = DisplaySizeAfterViewport(sd->vpDstH, sd->h);
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
        info.width = DisplaySizeAfterViewport(sd->vpDstW, sd->w);
        info.height = DisplaySizeAfterViewport(sd->vpDstH, sd->h);
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
                info.width = DisplaySizeAfterViewport(layer.vpDstW, layer.w);
                info.height = DisplaySizeAfterViewport(layer.vpDstH, layer.h);
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
            info.protocolOnly = true;
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
                 DisplaySizeAfterViewport(layer.vpDstW, layer.w),
                 DisplaySizeAfterViewport(layer.vpDstH, layer.h));
    }
    return count;
}

// ============================================================================
// TakeToplevelFrame: 桌面合成核心
// ============================================================================
//
// 阶段拆分 (重构第 2A 步, 纯结构拆分行为平价): 桌面分支拆为"锁内规划 /
// 锁外绘制"两阶段 — Plan*Locked 方法在 tmgr 锁内按原内联段顺序执行
// (dirty 门控 → 全屏仲裁 → 覆盖检测 → 直传判定 → 签名/基底 → damage R →
// 基底落盘 → 快照), 产出 DesktopTakePlan; Blit*/Composite* 锁外纯像素消费
// plan。锁边界/计时点/日志门控与原单函数实现逐段对应。

namespace {

// 帧合成耗时分段统计 ([GL-TAKE], 120 样本窗口): 原 TakeToplevelFrame 函数内
// static 上提到文件域, 供直传/合成两个日志点共享; 分段语义不变。
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

TakeBreakdownWindow& TakeBreakdown()
{
    static TakeBreakdownWindow breakdown;
    return breakdown;
}

using TakeClock = std::chrono::steady_clock;

uint64_t TakeElapsedUs(TakeClock::time_point begin, TakeClock::time_point end)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        end - begin).count());
}

// -- blit 裁剪数学 (纯函数): 原各 blit 段内联手写式的逐字提取, 数学不变 --

// 源图 (srcW×srcH) 绘制到目标 (targetW×targetH) 的 (posX,posY): 负原点裁剪
// (源起点正偏移) + 目标边界裁剪。返回 false = 完全不可见 (拷贝尺寸 ≤0)。
bool ClipBlitToTarget(int posX, int posY, int srcW, int srcH,
                      int targetW, int targetH,
                      int& srcX, int& srcY, int& dstX, int& dstY,
                      int& copyW, int& copyH)
{
    dstX = (posX > 0) ? posX : 0;
    dstY = (posY > 0) ? posY : 0;
    srcX = (posX < 0) ? -posX : 0;
    srcY = (posY < 0) ? -posY : 0;
    copyW = srcW - srcX;
    copyH = srcH - srcY;
    if (dstX + copyW > targetW) copyW = targetW - dstX;
    if (dstY + copyH > targetH) copyH = targetH - dstY;
    return copyW > 0 && copyH > 0;
}

// dst 矩形与重绘矩形 R 求交, src 起点同步偏移 (局部合成: R 外复用上帧内容,
// 不重画)。返回 false = 与 R 不相交。
bool IntersectBlitWithDamage(int& srcX, int& srcY, int& dstX, int& dstY,
                             int& copyW, int& copyH,
                             int dmgX, int dmgY, int dmgW, int dmgH)
{
    const int l = std::max(dstX, dmgX), t = std::max(dstY, dmgY);
    const int r = std::min(dstX + copyW, dmgX + dmgW);
    const int b = std::min(dstY + copyH, dmgY + dmgH);
    if (r <= l || b <= t) return false;
    srcX += l - dstX; srcY += t - dstY;
    dstX = l; dstY = t; copyW = r - l; copyH = b - t;
    return true;
}

// 矩形与重绘矩形 R 求交 (无源, 填充用)。返回 false = 与 R 不相交。
bool IntersectRectWithDamage(int& x, int& y, int& w, int& h,
                             int dmgX, int dmgY, int dmgW, int dmgH)
{
    const int l = std::max(x, dmgX), t = std::max(y, dmgY);
    const int r = std::min(x + w, dmgX + dmgW);
    const int b = std::min(y + h, dmgY + dmgH);
    if (r <= l || b <= t) return false;
    x = l; y = t; w = r - l; h = b - t;
    return true;
}

// src 矩形与 damage 包围盒求交, dst 起点同步偏移。返回 false = 不相交。
bool ClipBlitSourceToRect(int& srcX, int& srcY, int& dstX, int& dstY,
                          int& copyW, int& copyH,
                          int rectX, int rectY, int rectW, int rectH)
{
    const int l = std::max(srcX, rectX), t = std::max(srcY, rectY);
    const int r = std::min(srcX + copyW, rectX + rectW);
    const int b = std::min(srcY + copyH, rectY + rectH);
    if (r <= l || b <= t) return false;
    dstX += l - srcX; dstY += t - srcY;
    srcX = l; srcY = t; copyW = r - l; copyH = b - t;
    return true;
}

// 黑边矩形填充 (全屏 fit 黑边 / 局部合成时裁剪到 R)
void FillBlackRect(std::vector<uint8_t>& composited, int rootW,
                   const DesktopCompositor::DamageRect& dmg,
                   int fx, int fy, int fw, int fh)
{
    if (fw <= 0 || fh <= 0) return;
    // 局部合成: 黑边矩形裁剪到 R (黑边区域不在内容 fit 矩形内,
    // 与 R 不相交时无操作 — R 内重建完整黑边窗口)
    if (!dmg.full &&
        !IntersectRectWithDamage(fx, fy, fw, fh, dmg.x, dmg.y, dmg.w, dmg.h))
        return;
    for (int row = fy; row < fy + fh; ++row)
        std::fill_n(reinterpret_cast<uint32_t*>(composited.data()) +
                    static_cast<size_t>(row) * rootW + fx, fw, 0xFF000000u);
}

// 重绘矩形并集 (局部合成 R): 新矩形先裁剪到 root 再并入
void UnionDamageRect(DesktopCompositor::DamageRect& dmg, int rootW, int rootH,
                     int ux, int uy, int uw, int uh)
{
    if (uw <= 0 || uh <= 0) return;
    const int l = std::max(ux, 0), t = std::max(uy, 0);
    const int r = std::min(ux + uw, rootW), b = std::min(uy + uh, rootH);
    if (r <= l || b <= t) return;
    if (dmg.w <= 0 || dmg.h <= 0) {
        dmg.x = l; dmg.y = t; dmg.w = r - l; dmg.h = b - t;
    } else {
        const int nl = std::min(dmg.x, l), nt = std::min(dmg.y, t);
        const int nr = std::max(dmg.x + dmg.w, r), nb = std::max(dmg.y + dmg.h, b);
        dmg.x = nl; dmg.y = nt; dmg.w = nr - nl; dmg.h = nb - nt;
    }
}

} // namespace

bool DesktopCompositor::TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out, int& w, int& h) {
    // 帧级诊断统一门控 (perf_utils.h): 关闭时跳过 breakdown 累加与 [MW-TAKE] 输出
    const bool frameTrace = winehua::FrameTraceEnabled();

    using TakeClock = std::chrono::steady_clock;
    const auto takeStarted = TakeClock::now();
    auto lk = tmgr_.Lock();
    const auto lockAcquired = TakeClock::now();

    if (policy_.RootCompositing() && id == desktopRootToplevelId_) {
        DesktopTakePlan plan;
        const TakePlanOutcome outcome =
            PlanDesktopFrameLocked(id, frameTrace, takeStarted, lockAcquired, out, w, h, plan);
        if (outcome != TakePlanOutcome::kCompose)
            return outcome != TakePlanOutcome::kNoFrame;
        lk.unlock();  // ── 锁到此为止, 以下 blit 不持锁 ──
        CompositeDesktopFrame(id, frameTrace, takeStarted, lockAcquired, plan, out, w, h);
        return true;
    }

    return TakeWindowFrameLocked(id, out, w, h, frameTrace);
}

DesktopCompositor::TakePlanOutcome
DesktopCompositor::PlanDesktopFrameLocked(uint32_t id, bool frameTrace,
                                          TakeClock::time_point takeStarted,
                                          TakeClock::time_point lockAcquired,
                                          std::vector<uint8_t>& out, int& w, int& h,
                                          DesktopTakePlan& plan) {
    ToplevelManager::ToplevelState* rst = nullptr;
    // 阶段 1: dirty 门控 + 无子窗口快进
    const TakePlanOutcome gate = GateDesktopDirtyLocked(id, plan, out, w, h, rst);
    if (gate != TakePlanOutcome::kCompose) return gate;

    // 层序单一数据源 (阶段 1): 一帧全部内容来源的层列表, 构建一次,
    // fs-pick / 覆盖判定 / 签名 / 合成共用。zIndex: root < toplevel <
    // subsurface — 顺序与旧双循环等价 (见 CompositorLayer 注释)。
    plan.layers = BuildLayerListLocked(plan.rootW, plan.rootH);

    // 阶段 2: 全屏 pick/fit
    PlanFullscreenLocked(plan);
    // 阶段 3: fullscreenContentCovered 覆盖检测
    plan.fullscreenContentCovered = DetectFullscreenContentCoveredLocked(plan);
    // 阶段 4: SHM 全屏直传判定 (通过即完成本帧: out/w/h 已填 + 直传日志)
    if (TryShmFullscreenDirectLocked(id, rst, frameTrace, takeStarted, lockAcquired,
                                     plan, out, w, h))
        return TakePlanOutcome::kDirectPass;
    // 阶段 5: 合成签名 FNV 哈希 + rebuildBase 判定
    plan.compositionSignature = ComputeCompositionSignatureLocked(id, plan);
    const size_t rootBytes = static_cast<size_t>(plan.rootW) * plan.rootH * 4;
    plan.rebuildBase = !desktopOutputInitialized_ ||
        out.size() != rootBytes ||
        desktopOutputRootFrameSerial_ != desktopRootFrameSerial_ ||
        desktopCompositionSignature_ != plan.compositionSignature;
    // 阶段 6: 局部合成重绘矩形 R 计算 (空 R = 无内容变化, 本帧不产出)
    if (!ComputeDamageRectLocked(rst, plan)) return TakePlanOutcome::kNoFrame;
    // 阶段 7: 基底重建/局部基底拷贝
    CopyBaseToOutputLocked(rst, plan, out);
    plan.rootCopied = TakeClock::now();
    // 阶段 8: 锁内快照 (BlitSource/snapPool/ARGB opaque 融合扫描)
    SnapshotBlitSourcesLocked(rst, plan);
    plan.nZOrder = tmgr_.toplevelZOrder().size();
    plan.nSubLayers = subsurfaceLayers_.size();
    plan.snapshotDone = TakeClock::now();
    return TakePlanOutcome::kCompose;
}

DesktopCompositor::TakePlanOutcome
DesktopCompositor::GateDesktopDirtyLocked(uint32_t id, DesktopTakePlan& plan,
                                          std::vector<uint8_t>& out, int& w, int& h,
                                          ToplevelManager::ToplevelState*& rst) {
    rst = tmgr_.FindToplevelLocked(id);
    if (!rst || !rst->HasFrame()) return TakePlanOutcome::kNoFrame;
    if (!rst->IsDirty()) return TakePlanOutcome::kNoFrame;

    plan.rootW = rst->Width();
    plan.rootH = rst->Height();

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
        w = plan.rootW;
        h = plan.rootH;
        rst->ClearDirty();
        return TakePlanOutcome::kFastPath;
    }
    return TakePlanOutcome::kCompose;
}

void DesktopCompositor::PlanFullscreenLocked(DesktopTakePlan& plan) {
    // 全屏目标选取 (阶段 4, S3 收敛): 与输入侧 (FindInputTargetAt) 共用
    // PickFullscreenLayerLocked 单一实现 — 可见全屏窗口中取 fsPriority
    // 最大者 (多窗口可同时 fullscreen, 规则原因/局限见
    // ToplevelState::fsPriority 注释); fit 几何同样共用
    // ComputeFullscreenFitLocked (含内容尺寸选择, 见该函数注释)
    plan.fullscreenId = PickFullscreenLayerLocked(plan.layers);
    const ToplevelManager::ToplevelState* fsWin =
        plan.fullscreenId ? tmgr_.FindToplevelLocked(plan.fullscreenId) : nullptr;
    if (fsWin) {
        plan.fullscreenX = fsWin->X();
        plan.fullscreenY = fsWin->Y();
        plan.hasFullscreen = ComputeFullscreenFitLocked(plan.fullscreenId, plan.rootW,
                                                        plan.rootH, plan.transform);
        plan.isZcGame = HasZeroCopyLayerForToplevelLocked(plan.fullscreenId);
    }
}

bool DesktopCompositor::DetectFullscreenContentCoveredLocked(const DesktopTakePlan& plan) const {
    bool fullscreenContentCovered = false;
    if (plan.hasFullscreen) {
        const auto* fst = tmgr_.FindToplevelLocked(plan.fullscreenId);
        const int winW = fst ? fst->Width() : 0;
        const int winH = fst ? fst->Height() : 0;
        for (const auto& layer : plan.layers) {
            if (layer.type != CompositorLayer::Type::Subsurface) continue;
            if (layer.toplevelId != plan.fullscreenId) continue;
            if (layer.ShouldSkipCpu()) continue;
            const auto& sl = *layer.sub;
            if (sl.w <= 0 || sl.h <= 0) continue;
            if (sl.shmFormat == 0 && !sl.opaque) continue;
            const int dispW = DisplaySizeAfterViewportClamped(sl.vpDstW, sl.w);
            const int dispH = DisplaySizeAfterViewportClamped(sl.vpDstH, sl.h);
            const int relX = layer.x - plan.fullscreenX;
            const int relY = layer.y - plan.fullscreenY;
            if (relX <= 0 && relY <= 0 &&
                relX + dispW >= winW && relY + dispH >= winH) {
                fullscreenContentCovered = true;
                break;
            }
        }
    }
    return fullscreenContentCovered;
}

bool DesktopCompositor::TryShmFullscreenDirectLocked(uint32_t id,
                                                     ToplevelManager::ToplevelState* rst,
                                                     bool frameTrace,
                                                     TakeClock::time_point takeStarted,
                                                     TakeClock::time_point lockAcquired,
                                                     DesktopTakePlan& plan,
                                                     std::vector<uint8_t>& out, int& w, int& h) {
    /*
     * SHM 全屏游戏直传 (锁内判定, 通过即提前返回): 全屏 SHM 游戏独占
     * 画面时, 把游戏层的原始像素 (如 war3 的 800x600 sub 帧) 直接作为
     * 输出帧, 跳过整帧 1400x920 CPU 合成 — 实测 blit 段 40-78ms/帧
     * (随 box64 负载摆动) 是快照改造后剩余的瓶颈。渲染器 ComputeFitRect
     * 本就按帧尺寸保比例缩放 + 填黑边 (GPU 完成), 且两个保比例 fit 的
     * 复合等于一次直接 fit, 故画面几何与 CPU 合成逐像素一致, 输入映射
     * (走 root 桌面坐标, 与渲染帧尺寸无关) 不受影响; 纹理上传同时从
     * ~5MB 降到 ~1.9MB。
     * 放宽条件 (相比初版"恰好 1-2 层"): 改为按层序判定, 支撑真实游戏
     * 形态 (war3: 全屏窗口 + 其内 800x600 游戏 sub, 合成里还有
     * explorer/任务栏等层, 初版的 contentLayers==1/2 永远不满足):
     * - 有非 ZC 全屏窗口 (ZC 游戏画面在 GL 层, 无 SHM 像素可传)
     * - root 为 XRGB: 渲染器据 root 格式置 frameArgb_=false, GPU 填的
     *   黑边才不透明
     * - 全屏窗口覆盖整个 root (x/y<=0 且 x+w/h >= root): 其下所有层被
     *   盖住, 无需合成
     * - 全屏窗口之上 (层序更高) 无可见内容层: ZC/不可见/连带全屏的旧
     *   窗口跳过; 其它可见层 (弹窗/对话框)= 真遮挡, 回退真合成
     * - 直传源尺寸 == 全屏 fit 的内容尺寸 (transform.srcW/H): 渲染器
     *   GL fit 与 CPU BlitScaled 到 transform 的几何严格一致。直传源为
     *   全屏窗口自身帧 或 其内不透明的 sub (无 viewport 裁剪)
     * - 源 buffer 严格 == w*h*4 (带 padding 的 buffer 拒绝直传, 防
     *   rowLen 错位; 回退 CPU 合成更稳)
     * 退出全屏/开窗弹窗时条件自然失效, 自动回退 CPU 合成; 回退帧
     * out.size() != rootBytes 触发 rebuildBase 重建基底。
     */
    if (plan.hasFullscreen && !plan.isZcGame && rst->ShmFormat() != 0 &&
        plan.transform.srcW > 0 && plan.transform.srcH > 0) {
        const ToplevelManager::ToplevelState* fsTop =
            tmgr_.FindToplevelLocked(plan.fullscreenId);
        // 放宽直传门槛 (20060822 实测): 全屏游戏窗口逻辑几何
        // (800x600/640x480 等) 由 fit 放大铺满屏幕, 旧条件"逻辑几何
        // 覆盖 root"连真实游戏全屏都不满足 → 直传永不触发, 每帧 CPU
        // BlitScaled 1227x920 70-85ms 钉死 ~10fps (鼠标不够跟手根因,
        // GL-TAKE 合成段 avg 70-85ms 实测)。"其下层被盖住"由 fit 输出
        // 自身保证: CPU 路径 fillBlackRect 整屏黑边 + 内容 fit, 最终
        // 像素覆盖整屏, 下层贡献恒 0; 直传路径 GPU 侧 glClear 黑底 +
        // GL fit (ComputeFitRect 与 CPU 同源, 直传源尺寸==fit 内容尺寸
        // 时两者像素级一致, 见下方 directPixels 校验)。上方遮挡仍由
        // topOccluded 检查; 真正必要条件只有下面逐条校验的:
        //   1. 窗口自身几何正直 (Width/Height > 0)
        //   2. 直传源尺寸 == transform.srcW/H (几何等价)
        //   3. 无上层可见内容层 (topOccluded)
        if (fsTop && fsTop->Width() > 0 && fsTop->Height() > 0) {
            size_t fsZ = 0;
            bool fsLayerFound = false;
            for (const auto& layer : plan.layers) {
                if (layer.type == CompositorLayer::Type::Toplevel &&
                    layer.toplevelId == plan.fullscreenId) {
                    fsZ = layer.zIndex;
                    fsLayerFound = true;
                    break;
                }
            }
            if (fsLayerFound) {
                const SubsurfaceLayer* contentSub = nullptr;
                bool topOccluded = false;
                uint32_t occlTl = 0;
                int occlType = -1;
                bool occlZc = false;
                for (const auto& layer : plan.layers) {
                    if (layer.zIndex <= fsZ) continue;  // 全屏窗口及其下: 被盖住
                    if (layer.ShouldSkipCpu() ||
                        ShouldSkipFullscreenCascade(layer, plan.fullscreenId,
                                                    plan.hasFullscreen, tmgr_))
                        continue;
                    if (layer.type == CompositorLayer::Type::Subsurface &&
                        layer.toplevelId == plan.fullscreenId) {
                        // 本窗口内容 sub (游戏画面): 候选直传源
                        if (!contentSub) {
                            const auto& sl = *layer.sub;
                            // 注: 不再要求 (shmFormat!=0 || opaque) — GL
                            // readback 类画面 (opengl_readback, ARGB 800x600)
                            // 的 alpha 通道是 GL 帧缓冲残留 (未清区 0/绘制区
                            // 255), opaque 全帧扫描被非 255 像素拦下, 害直传
                            // 落到窗口黑帧 (20260822 黑屏实锤: 窗口帧 95%
                            // black/alpha0)。ARGB sub 由渲染器 uForceOpaque
                            // 强制不透明: alpha=255 区域与 CPU 混合分支逐
                            // 像素一致, alpha=0 区域 CPU 保留黑底 (RGB 残留
                            // 值通常亦黑) — 视觉等价。
                            if (sl.w > 0 && sl.h > 0 &&
                                (sl.vpDstW <= 0 || sl.vpDstW >= sl.w) &&
                                (sl.vpDstH <= 0 || sl.vpDstH >= sl.h))
                                contentSub = &sl;
                        }
                        continue;
                    }
                    topOccluded = true;  // 上方可见层: 需要真合成
                    occlTl = layer.toplevelId;
                    occlType = static_cast<int>(layer.type);
                    occlZc = layer.zcActive;
                    break;
                }
                const std::vector<uint8_t>* directPixels = nullptr;
                int directW = 0, directH = 0;
                if (!topOccluded) {
                    // 直传源几何必须与全屏 fit 一致 (src = fsTop 内容尺寸)
                    if (contentSub &&
                        contentSub->w == plan.transform.srcW &&
                        contentSub->h == plan.transform.srcH &&
                        contentSub->pixels.size() ==
                            static_cast<size_t>(contentSub->w) * contentSub->h * 4) {
                        directPixels = &contentSub->pixels;
                        directW = contentSub->w;
                        directH = contentSub->h;
                    // 不再要求 shmFormat!=0 (XRGB): 20260822 实测红警2 类
                    // 全屏游戏窗口帧是 ARGB (shmFormat=0) 但内容 alpha 全
                    // 255 (游戏自绘不透明画面), 原 XRGB 条件把直传全部
                    // 摁死 → 每帧 CPU BlitScaled 70-85ms。渲染器 context
                    // 无 GL_BLEND + uForceOpaque 强制不透明: alpha=255 时
                    // 与 CPU 合成输出逐像素一致 (黑边 glClear 同效
                    // fillBlackRect); 半透明全屏内容 (预期无, 游戏用
                    // XRGB/全不透明) 会丢失混合 — 容忍并看验收。
                    } else if (fsTop->Width() == plan.transform.srcW &&
                               fsTop->Height() == plan.transform.srcH &&
                               fsTop->Pixels().size() ==
                                   static_cast<size_t>(fsTop->Width()) * fsTop->Height() * 4) {
                        directPixels = &fsTop->Pixels();
                        directW = fsTop->Width();
                        directH = fsTop->Height();
                    }
                }
                if (directPixels) {
                    // assign 而非 swap: 源缓冲属 wl 线程的层状态, 必须拷出
                    out.assign(directPixels->begin(), directPixels->end());
                    w = directW;
                    h = directH;
                    rst->ClearDirty();
                    const auto directDone = TakeClock::now();
                    if (frameTrace) {
                        // 分段语义同下: 直传无基底拷贝/快照/blit, 全部计入输出段
                        TakeBreakdown().Add(TakeElapsedUs(takeStarted, lockAcquired),
                                      0, 0, 0,
                                      TakeElapsedUs(lockAcquired, directDone),
                                      TakeElapsedUs(takeStarted, directDone));
                        // 帧总结 (与 CPU 合成分支同格式, 数据量一致 — 直传
                        // 期间也能从日志确认"当前在直传模式")
                        OH_LOG_INFO(LOG_APP,
                                    "[MW-TAKE] root #%{public}u %{public}dx%{public}d "
                                    "subsurfaces=%{public}zu mode=direct fs=%{public}d",
                                    id, w, h, subsurfaceLayers_.size(),
                                    plan.hasFullscreen ? 1 : 0);
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

uint64_t DesktopCompositor::ComputeCompositionSignatureLocked(uint32_t id,
                                                              const DesktopTakePlan& plan) const {
    uint64_t compositionSignature = compositor_consts::kFnv1aOffsetBasis;
    auto mixSignature = [&](uint64_t value) {
        compositionSignature ^= value;
        compositionSignature *= compositor_consts::kFnv1aPrime;
    };
    mixSignature(id);
    mixSignature(static_cast<uint32_t>(plan.rootW));
    mixSignature(static_cast<uint32_t>(plan.rootH));
    // 签名遍历 Layer 列表: 每个可见 toplevel/subsurface 的几何与标记
    // (与旧两个循环 mix 序列等价; 不可见 toplevel 的 (id,0) 不再混入,
    // 仅影响 rebuildBase 触发时机, 不影响输出像素 — 不可见窗口不参与
    // 合成, root 像素变化仍由 desktopRootFrameSerial_ 兜底)。
    for (const auto& layer : plan.layers) {
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
    return compositionSignature;
}

bool DesktopCompositor::ComputeDamageRectLocked(ToplevelManager::ToplevelState* rst,
                                                DesktopTakePlan& plan) {
    plan.srcs.resize(plan.layers.size());
    // 跳过条件与原 blit 入口完全一致 (单一实现, 不复制规则): 预计算供
    // R 计算与快照循环共享。
    // - ShouldSkipCpu: ZC 层 (GPU 自绘) / 不可见层
    // - ShouldSkipFullscreenCascade: 只跳过被连带标 fullscreen 的旧窗口
    //   (notepad/explorer 等, 显示模式切换时 winewayland 批量标记,
    //   fsPriority 选了游戏但它仍在 z-order 高位, 普通 blit 会盖在游戏
    //   上面), 非全屏弹窗/对话框保留; 与输入命中同源
    for (size_t li = 0; li < plan.layers.size(); ++li) {
        const auto& layer = plan.layers[li];
        if (layer.type == CompositorLayer::Type::Root) continue;
        plan.srcs[li].skip = layer.ShouldSkipCpu() ||
                        ShouldSkipFullscreenCascade(layer, plan.fullscreenId,
                                                    plan.hasFullscreen, tmgr_);
    }

    /*
     * 本帧重绘矩形 (局部合成): war3 实测每帧 1400x920 全桌面 CPU 合成
     * ~74ms (13fps) 是鼠标"滞后"的直接原因 — 画面更新慢于输入派发。
     * 局部合成只重建"内容更新的层"可见矩形并集 R, R 外复用上帧输出
     * (渲染线程帧缓冲跨帧保留), 静止层 (explorer/任务栏/基底) 不重画。
     * 判定依据: 层内容序列号 (sub=shmCommitSerial 每次 commit 递增 /
     * toplevel=ToplevelState::FrameSerial), 几何/层序/显隐变化已并入
     * compositionSignature → rebuildBase; 全屏路径 (fit 缩放/填黑/ZC 独占)
     * 保守整帧 (行为与旧实现一致)。
     * 正确性: R 内按 z 升序重画与 R 相交的全部非跳过层 + root 基底
     * (锁内落盘), 输出与整帧合成在 R 内逐像素一致 — 不透明层盖住低层,
     * 半透明层以本次重建的底混合, 静态层与 R 不相交时内容不变不必重画。
     */
    // 全屏 SHM 游戏也走局部 (R=游戏内容 fit 后屏幕区域, 黑边区固定不必
    // 重画 — 实测 war3 全屏 800x600→1400x920 fit 每帧 74ms 皆全屏全帧,
    // 是本分段最大浪费); 只有 ZC 游戏 (画面在 GPU 层) 与几何/层序/显隐
    // 变化 (rebuildBase) 保持整帧。
    if (plan.rebuildBase || plan.isZcGame) {
        plan.dmg.full = true;
    } else {
        plan.dmg.full = false;
        plan.dmg.x = plan.dmg.y = 0; plan.dmg.w = plan.dmg.h = 0;
        for (size_t li = 0; li < plan.layers.size(); ++li) {
            const auto& layer = plan.layers[li];
            if (plan.srcs[li].skip) continue;
            int ux = 0, uy = 0, uw = 0, uh = 0;
            if (layer.type == CompositorLayer::Type::Subsurface) {
                const auto& sl = *layer.sub;
                const auto it = lastSubSerial_.find(sl.surfaceKey);
                if (it != lastSubSerial_.end() && it->second == sl.shmCommitSerial)
                    continue;  // 像素未更新 (上帧合成已含当前内容)
                if (plan.hasFullscreen && layer.toplevelId == plan.fullscreenId) {
                    // 全屏 SHM 游戏: R 贡献 = fit 后屏幕区域 — 与
                    // blitSubsurface 全屏分支同源映射 (FitMapLayerRect),
                    // 保证 R 恰好=变化内容的显示区域 (黑边区无需重画)
                    const int dispW = DisplaySizeAfterViewportClamped(sl.vpDstW, layer.w);
                    const int dispH = DisplaySizeAfterViewportClamped(sl.vpDstH, layer.h);
                    FitMapLayerRect(plan.transform, layer.x - plan.fullscreenX,
                                    layer.y - plan.fullscreenY,
                                    dispW, dispH, ux, uy, uw, uh);
                } else {
                    ux = layer.x; uy = layer.y; uw = layer.w; uh = layer.h;
                    // 与 blitSubsurface 同规则的 dmg 包围盒裁剪 (显示内容
                    // 更窄时重绘范围随之收窄; 无效/为空用全层矩形兜底)
                    if (sl.dmgW > 0 && sl.dmgH > 0) {
                        const int dl = std::max(ux, layer.x + sl.dmgX);
                        const int dt = std::max(uy, layer.y + sl.dmgY);
                        const int dr = std::min(ux + uw, layer.x + sl.dmgX + sl.dmgW);
                        const int db = std::min(uy + uh, layer.y + sl.dmgY + sl.dmgH);
                        if (dr > dl && db > dt) { ux = dl; uy = dt; uw = dr - dl; uh = db - dt; }
                    }
                }
            } else if (layer.type == CompositorLayer::Type::Toplevel) {
                auto* cst = tmgr_.FindToplevelLocked(layer.toplevelId);
                if (!cst) continue;
                const auto it = lastTopSerial_.find(layer.toplevelId);
                if (it != lastTopSerial_.end() && it->second == cst->FrameSerial())
                    continue;
                if (plan.hasFullscreen && layer.toplevelId == plan.fullscreenId) {
                    // 全屏游戏窗口: R 贡献 = fit 后屏幕内容区 — 与
                    // blitSubsurface 全屏分支同几何 (内容 blit 到
                    // transform.offX/Y + dstW/H)。窗口逻辑几何 (红警2
                    // 全屏窗口 800x600) 不是屏幕矩形: 直用会把 R 算成
                    // "左上角 800x600" (MW-TAKE dmg=(0,0 800x600) 实锤),
                    // R 外区域永久复用上帧 → 画面只剩左上角。黑边
                    // (fillBlackRect) 不在 R 无需重画: 首帧/rebuildBase
                    // 整帧时已落盘, 之后黑边不变。
                    FitMapLayerRect(plan.transform, layer.x - plan.fullscreenX,
                                    layer.y - plan.fullscreenY,
                                    cst->Width(), cst->Height(),
                                    ux, uy, uw, uh);
                } else {
                    ux = cst->X(); uy = cst->Y(); uw = cst->Width(); uh = cst->Height();
                }
            }
            if (uw <= 0 || uh <= 0) continue;
            UnionDamageRect(plan.dmg, plan.rootW, plan.rootH, ux, uy, uw, uh);
        }
        if (plan.dmg.empty()) {
            // 无内容变化 (commit 未重写像素, 如空帧): 本帧不产出新帧,
            // 渲染器保留上一帧纹理 — 等价"没取到帧"。上帧输出保持: R 外
            // (全部) 内容仍有效。
            rst->ClearDirty();
            return false;
        }
    }
    return true;
}

void DesktopCompositor::CopyBaseToOutputLocked(const ToplevelManager::ToplevelState* rst,
                                               DesktopTakePlan& plan,
                                               std::vector<uint8_t>& out) {
    if (plan.rebuildBase) {
        out = rst->Pixels();
        desktopOutputInitialized_ = true;
        desktopOutputRootFrameSerial_ = desktopRootFrameSerial_;
        desktopCompositionSignature_ = plan.compositionSignature;
    } else if (!plan.dmg.full) {
        // 局部合成: R∩root 基底落盘到输出 (R 外复用上帧内容)。持锁读
        // root 帧像素; out.size()==rootBytes 已由 rebuildBase 判定保证。
        const auto& base = rst->Pixels();
        const size_t rowBytes = static_cast<size_t>(plan.dmg.w) * 4;
        for (int row = 0; row < plan.dmg.h; ++row)
            std::memcpy(out.data() +
                            (static_cast<size_t>(plan.dmg.y + row) * plan.rootW + plan.dmg.x) * 4,
                        base.data() +
                            (static_cast<size_t>(plan.dmg.y + row) * plan.rootW + plan.dmg.x) * 4,
                        rowBytes);
    }
}

void DesktopCompositor::SnapshotBlitSourcesLocked(ToplevelManager::ToplevelState* rst,
                                                  DesktopTakePlan& plan) {
    /*
     * 快照阶段 (持锁): 把 blit 要读的全部源像素拷成私有副本 (元数据已在
     * BlitSource / srcs 预计算), 随后立即解锁, blit 在锁外进行。
     */
    // 快照缓冲池: 跨帧复用容量, 避免每帧新建多 MB vector 的分配+缺页
    // 开销 (实测每帧全新分配让快照段从预期 ~2ms 涨到 35ms)。本函数仅
    // 渲染线程调用, 池无需加锁; 层数变化时 resize, 既有条目容量保留。
    snapPool_.resize(plan.layers.size());
    for (size_t li = 0; li < plan.layers.size(); ++li) {
        const auto& layer = plan.layers[li];
        if (layer.type == CompositorLayer::Type::Root) continue;
        auto& bs = plan.srcs[li];
        if (bs.skip) continue;
        // 局部合成: 与 R 不相交的层不会画到 R 内 (blit 有 R 裁剪早退),
        // 其像素本帧保持上帧内容 — 跳过快照拷贝。相交但未变化的层仍
        // 快照+重画 (半透明层以本次重建的底混合, 见 R 注释)。
        if (!plan.dmg.full && (plan.dmg.x >= layer.x + layer.w ||
                          plan.dmg.y >= layer.y + layer.h ||
                          plan.dmg.x + plan.dmg.w <= layer.x ||
                          plan.dmg.y + plan.dmg.h <= layer.y)) {
            bs.skip = true;
            continue;
        }
        if (layer.type == CompositorLayer::Type::Toplevel) {
            auto* cst = tmgr_.FindToplevelLocked(layer.toplevelId);
            if (!cst) { bs.skip = true; continue; }
            bs.w = cst->Width(); bs.h = cst->Height();
            bs.x = cst->X(); bs.y = cst->Y();
            bs.shmFormat = cst->ShmFormat();
            lastTopSerial_[layer.toplevelId] = cst->FrameSerial();  // 局部合成基准
            // ZC 游戏整幅填黑不读像素, 省下全屏拷贝 (pixels 留空)
            if (!(layer.toplevelId == plan.fullscreenId && plan.hasFullscreen && plan.isZcGame)) {
                auto& buf = snapPool_[li];
                const auto& src = cst->Pixels();
                buf.assign(src.begin(), src.end());
                bs.pixels = &buf;
            }
        } else {  // Subsurface
            const auto& sl = *layer.sub;
            bs.w = sl.w; bs.h = sl.h;
            bs.shmFormat = sl.shmFormat; bs.opaque = sl.opaque;
            bs.vpDstW = sl.vpDstW; bs.vpDstH = sl.vpDstH;
            bs.dmgX = sl.dmgX; bs.dmgY = sl.dmgY; bs.dmgW = sl.dmgW; bs.dmgH = sl.dmgH;
            lastSubSerial_[sl.surfaceKey] = sl.shmCommitSerial;  // 局部合成基准
            auto& buf = snapPool_[li];
            if (sl.shmFormat == 0) {
                // ARGB: opaque 精确判定融合进拷贝 (单次内存遍历; wl 线程
                // 不再扫描 — 见 UpdateSubsurfaceLayerOnCommit 注释)。
                // 结果写回 layer (fullscreenContentCovered 等下一帧用新值)
                uint32_t nw = static_cast<uint32_t>(sl.pixels.size() / 4);
                buf.resize(sl.pixels.size());
                const uint32_t* s = reinterpret_cast<const uint32_t*>(sl.pixels.data());
                uint32_t* d = reinterpret_cast<uint32_t*>(buf.data());
                bool allOpaque = true;
                for (uint32_t i = 0; i < nw; ++i) {
                    const uint32_t px = s[i];
                    d[i] = px;
                    if ((px & 0xFF000000u) != 0xFF000000u) allOpaque = false;
                }
                bs.opaque = allOpaque;
                const_cast<SubsurfaceLayer*>(layer.sub)->opaque = allOpaque;
            } else {
                buf.assign(sl.pixels.begin(), sl.pixels.end());
            }
            bs.pixels = &buf;
        }
    }
    rst->ClearDirty();  // 快照已取走本帧全部内容; 解锁后的新 commit 会重新置位
}

void DesktopCompositor::CompositeDesktopFrame(uint32_t id, bool frameTrace,
                                              TakeClock::time_point takeStarted,
                                              TakeClock::time_point lockAcquired,
                                              const DesktopTakePlan& plan,
                                              std::vector<uint8_t>& out, int& w, int& h) {
    auto& composited = out;
    // 合成单循环 (阶段 1): 按 zIndex 升序遍历 Layer 列表 — 等价旧
    // toplevel 循环 + subsurface 循环的两段顺序 (Layer zIndex 分配保证)。
    // 全屏独占/跳过特判原样保留 (等价形式), 行为不变。
    // 注: 本循环在锁外执行, 只读 BlitSource 快照, 不碰 tmgr_/layer.sub。
    for (size_t li = 0; li < plan.layers.size(); ++li) {
        const auto& layer = plan.layers[li];
        switch (layer.type) {
            case CompositorLayer::Type::Root:
                break;  // 基底已在持锁阶段拷贝 (rebuildBase 整帧 / 局部 R∩root)
            case CompositorLayer::Type::Toplevel:
                BlitToplevel(plan, layer, plan.srcs[li], composited);
                break;
            case CompositorLayer::Type::Subsurface:
                BlitSubsurface(plan, layer, plan.srcs[li], composited);
                break;
        }
    }
    const auto childrenComposited = TakeClock::now();

    const auto outputMoved = TakeClock::now();
    w = plan.rootW;
    h = plan.rootH;
    if (frameTrace) {
        // 分段语义 (快照改造后): lockWait / 基底拷贝 / 快照(持锁) / blit(锁外) / 输出 / 总计
        TakeBreakdown().Add(TakeElapsedUs(takeStarted, lockAcquired),
                      TakeElapsedUs(lockAcquired, plan.rootCopied),
                      TakeElapsedUs(plan.rootCopied, plan.snapshotDone),
                      TakeElapsedUs(plan.snapshotDone, childrenComposited),
                      TakeElapsedUs(childrenComposited, outputMoved),
                      TakeElapsedUs(takeStarted, outputMoved));
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] root #%{public}u %{public}dx%{public}d children=%{public}zu subsurfaces=%{public}zu mode=%{public}s fs=%{public}d dmg=(%{public}d,%{public}d %{public}dx%{public}d)",
                    id, w, h, plan.nZOrder, plan.nSubLayers, plan.dmg.full ? "full" : "partial",
                    plan.hasFullscreen ? 1 : 0, plan.dmg.x, plan.dmg.y, plan.dmg.w, plan.dmg.h);
    }
}

void DesktopCompositor::BlitToplevel(const DesktopTakePlan& plan, const CompositorLayer& layer,
                                     const BlitSource& bs, std::vector<uint8_t>& composited) {
    if (bs.skip) return;
    if (layer.toplevelId == plan.fullscreenId && plan.hasFullscreen && plan.isZcGame) {
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
    const auto& childPx = *bs.pixels;
    int childW = bs.w;
    int childH = bs.h;
    int posX = bs.x;
    int posY = bs.y;
    if (layer.toplevelId == plan.fullscreenId && plan.hasFullscreen) {
        const bool contentOpaque = (bs.shmFormat != 0) || plan.fullscreenContentCovered;
        if (contentOpaque) {
            FillBlackRect(composited, plan.rootW, plan.dmg,
                          0, 0, plan.rootW, plan.transform.offY);
            FillBlackRect(composited, plan.rootW, plan.dmg,
                          0, plan.transform.offY + plan.transform.dstH, plan.rootW,
                          plan.rootH - plan.transform.offY - plan.transform.dstH);
            FillBlackRect(composited, plan.rootW, plan.dmg,
                          0, plan.transform.offY, plan.transform.offX, plan.transform.dstH);
            FillBlackRect(composited, plan.rootW, plan.dmg,
                          plan.transform.offX + plan.transform.dstW, plan.transform.offY,
                          plan.rootW - plan.transform.offX - plan.transform.dstW,
                          plan.transform.dstH);
        } else {
            // 垫黑底只垫本帧重绘范围 (R): 整帧垫黑在局部合成时会
            // 抹掉 R 外上帧已合成的内容 (20260822 review 发现:
            // ARGB 全屏窗口 contentOpaque=false 时命中此分支, 上方
            // 弹窗更新触发 partial 帧即黑屏一次)。full 帧垫整帧。
            if (!plan.dmg.full) {
                for (int row = plan.dmg.y; row < plan.dmg.y + plan.dmg.h; ++row)
                    std::fill_n(reinterpret_cast<uint32_t*>(composited.data()) +
                                static_cast<size_t>(row) * plan.rootW + plan.dmg.x,
                                plan.dmg.w, 0xFF000000u);
            } else {
                std::fill_n(reinterpret_cast<uint32_t*>(composited.data()),
                            composited.size() / 4, 0xFF000000u);
            }
        }
        if (!plan.fullscreenContentCovered) {
            BlitScaled(composited.data(), plan.rootW, plan.rootH,
                       childPx.data(), childW, childW, childH,
                       plan.transform.offX, plan.transform.offY,
                       plan.transform.dstW, plan.transform.dstH,
                       bs.shmFormat == 0,
                       plan.dmg.full ? 0 : plan.dmg.x, plan.dmg.full ? 0 : plan.dmg.y,
                       plan.dmg.full ? 0 : plan.dmg.w, plan.dmg.full ? 0 : plan.dmg.h);
        }
        return;
    }
    int srcX = 0, srcY = 0, dstX = 0, dstY = 0, copyW = 0, copyH = 0;
    if (!ClipBlitToTarget(posX, posY, childW, childH, plan.rootW, plan.rootH,
                          srcX, srcY, dstX, dstY, copyW, copyH))
        return;
    // 局部合成: 裁剪到本帧重绘矩形 (R 外复用上帧内容, 不重画)
    if (!plan.dmg.full &&
        !IntersectBlitWithDamage(srcX, srcY, dstX, dstY, copyW, copyH,
                                 plan.dmg.x, plan.dmg.y, plan.dmg.w, plan.dmg.h))
        return;
    const bool childArgb = (bs.shmFormat == 0);
    for (int y = 0; y < copyH; y++) {
        auto* srcRow = &childPx[(srcY + y) * childW * 4];
        auto* dstRow = &composited[(dstY + y) * plan.rootW * 4];
        // SrcOnly 混合语义 (源不乘 alpha, clamp, 目标 alpha 强制 255)
        BlitClipAlpha(&dstRow[dstX * 4], &srcRow[srcX * 4], copyW,
                      childArgb, PixelBlend::SrcOnly);
    }
}

void DesktopCompositor::BlitSubsurface(const DesktopTakePlan& plan, const CompositorLayer& layer,
                                       const BlitSource& bs, std::vector<uint8_t>& composited) {
    if (bs.skip) return;
    if (layer.w <= 0 || layer.h <= 0) return;
    int layerX = layer.x;
    int layerY = layer.y;
    size_t expectSz = (size_t)bs.w * bs.h * 4;
    if (bs.pixels->size() < expectSz) {
        OH_LOG_WARN(LOG_APP, "[MW-SUBSURF] layer size mismatch: w=%{public}d h=%{public}d px=%{public}zu expected=%{public}zu",
                    bs.w, bs.h, bs.pixels->size(), expectSz);
        return;
    }
    if (plan.hasFullscreen && layer.toplevelId == plan.fullscreenId) {
        const int layerDispW = DisplaySizeAfterViewportClamped(bs.vpDstW, bs.w);
        const int layerDispH = DisplaySizeAfterViewportClamped(bs.vpDstH, bs.h);
        // 与输入 FindInputTargetAt 全屏分支同几何 (FitMapLayerRect 唯一实现)
        int layerDstX, layerDstY, layerDstW, layerDstH;
        FitMapLayerRect(plan.transform, layerX - plan.fullscreenX, layerY - plan.fullscreenY,
                        layerDispW, layerDispH,
                        layerDstX, layerDstY, layerDstW, layerDstH);
        BlitScaled(composited.data(), plan.rootW, plan.rootH,
                   bs.pixels->data(), bs.w, layerDispW, layerDispH,
                   layerDstX, layerDstY, layerDstW, layerDstH,
                   bs.shmFormat == 0 && !bs.opaque,
                   plan.dmg.full ? 0 : plan.dmg.x, plan.dmg.full ? 0 : plan.dmg.y,
                   plan.dmg.full ? 0 : plan.dmg.w, plan.dmg.full ? 0 : plan.dmg.h);
        return;
    }
    int srcX = 0, srcY = 0, dstX = 0, dstY = 0, copyW = 0, copyH = 0;
    if (!ClipBlitToTarget(layerX, layerY, bs.w, bs.h, plan.rootW, plan.rootH,
                          srcX, srcY, dstX, dstY, copyW, copyH))
        return;
    int renderW = copyW, renderH = copyH;
    int renderSrcX = srcX, renderSrcY = srcY;
    int renderDstX = dstX, renderDstY = dstY;
    if (bs.vpDstW > 0 && bs.vpDstW < copyW) renderW = bs.vpDstW;
    if (bs.vpDstH > 0 && bs.vpDstH < copyH) renderH = bs.vpDstH;
    if (bs.dmgW > 0 && bs.dmgH > 0 &&
        !ClipBlitSourceToRect(renderSrcX, renderSrcY, renderDstX, renderDstY,
                              renderW, renderH, bs.dmgX, bs.dmgY, bs.dmgW, bs.dmgH))
        return;
    // 局部合成: 再裁剪到本帧重绘矩形 (R 外复用上帧内容, 不重画)
    if (!plan.dmg.full &&
        !IntersectBlitWithDamage(renderSrcX, renderSrcY, renderDstX, renderDstY,
                                 renderW, renderH,
                                 plan.dmg.x, plan.dmg.y, plan.dmg.w, plan.dmg.h))
        return;
    const bool needsAlphaBlend = bs.shmFormat == 0 && !bs.opaque;
    for (int y = 0; y < renderH; y++) {
        const uint8_t* srcRow = bs.pixels->data() +
            ((renderSrcY + y) * bs.w + renderSrcX) * 4;
        uint8_t* dstRow = composited.data() +
            ((renderDstY + y) * plan.rootW + renderDstX) * 4;
        BlitClipAlpha(dstRow, srcRow, renderW, needsAlphaBlend, PixelBlend::Normal);
    }
}

void DesktopCompositor::BlitWindowSubsurface(const CompositorLayer& layer, int winW, int winH,
                                             std::vector<uint8_t>& out) {
    const auto& sl = *layer.sub;
    size_t expectSz = (size_t)sl.w * sl.h * 4;
    if (sl.pixels.size() < expectSz) return;
    int srcX = 0, srcY = 0, dstX = 0, dstY = 0, copyW = 0, copyH = 0;
    if (!ClipBlitToTarget(layer.x, layer.y, sl.w, sl.h, winW, winH,
                          srcX, srcY, dstX, dstY, copyW, copyH))
        return;
    const bool needsAlphaBlend = sl.shmFormat == 0 && !sl.opaque;
    for (int y = 0; y < copyH; y++) {
        const uint8_t* srcRow = sl.pixels.data() + ((srcY + y) * sl.w + srcX) * 4;
        uint8_t* dstRow = out.data() + ((dstY + y) * winW + dstX) * 4;
        BlitClipAlpha(dstRow, srcRow, copyW, needsAlphaBlend, PixelBlend::Normal);
    }
}

bool DesktopCompositor::TakeWindowFrameLocked(uint32_t id, std::vector<uint8_t>& out,
                                              int& w, int& h, bool frameTrace) {
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
    for (const auto& layer : layers) {
        switch (layer.type) {
            case CompositorLayer::Type::Root:
                break;  // 基底已在 out = st->pixels 拷贝
            case CompositorLayer::Type::Toplevel:
                break;  // 窗口内 ZC 整窗口层: GPU 自绘, CPU 帧跳过
            case CompositorLayer::Type::Subsurface:
                if (layer.ShouldSkipCpu()) break;  // ZC 子表面 (GPU 自绘) / 不可见: 同上
                BlitWindowSubsurface(layer, winW, winH, out);
                break;
        }
    }
    w = winW;
    h = winH;
    st->ClearDirty();
    if (frameTrace) {
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu",
                    id, w, h, out.size());
    }
    return true;
}

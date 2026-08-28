#include "desktop_compositor.h"
#include "frame_pipeline.h"
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
// 阶段拆分 (重构第 2A 步, 纯结构拆分行为平价): 桌面分支的"锁内规划 /
// 锁外绘制"两阶段在 frame_pipeline.{h,cpp} — FramePlanner 锁内按原内联段
// 顺序执行 (dirty 门控 → 全屏仲裁 → 覆盖检测 → 直传判定 → 签名/基底 →
// damage R → 基底落盘 → 快照) 产出 FramePlan, FrameBlitter 锁外纯像素
// 消费; 本函数只剩编排。锁边界/计时点/日志门控与原单函数实现逐段对应。

bool DesktopCompositor::TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out,
                                          PresentedFrame& frame) {
    // 帧级诊断统一门控 (perf_utils.h): 关闭时跳过 breakdown 累加与 [MW-TAKE] 输出
    const bool frameTrace = winehua::FrameTraceEnabled();

    const auto takeStarted = TakeClock::now();
    auto lk = tmgr_.Lock();
    const auto lockAcquired = TakeClock::now();

    if (policy_.RootCompositing() && id == desktopRootToplevelId_) {
        FramePlan plan;
        FramePlanner planner(*this, frameTrace);
        const FramePlanOutcome outcome =
            planner.PlanDesktopLocked(id, takeStarted, lockAcquired, out, frame, plan);
        if (outcome != FramePlanOutcome::kCompose) {
            if (outcome != FramePlanOutcome::kNoFrame) frame.pixels = out.data();
            return outcome != FramePlanOutcome::kNoFrame;
        }
        lk.unlock();  // ── 锁到此为止, 以下 blit 不持锁 ──
        FrameBlitter blitter(frameTrace);
        blitter.Composite(id, takeStarted, lockAcquired, plan, out);
        frame.pixels = out.data();
        return true;
    }

    return TakeWindowFrameLocked(id, out, frame, frameTrace);
}

bool DesktopCompositor::TakeWindowFrameLocked(uint32_t id, std::vector<uint8_t>& out,
                                              PresentedFrame& frame, bool frameTrace) {
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
                FrameBlitter::BlitWindowSubsurface(layer, winW, winH, out);
                break;
        }
    }
    // 帧交付契约: PC 窗口帧 — 窗口局部空间, buffer = 内容 = 窗口尺寸
    frame.kind = PresentedFrame::Kind::Composed;
    frame.baseSpace = PresentedFrame::BaseSpace::Window;
    frame.w = winW;
    frame.h = winH;
    frame.contentW = winW;
    frame.contentH = winH;
    frame.opaque = (st->ShmFormat() != 0);
    frame.pixels = out.data();
    st->ClearDirty();
    if (frameTrace) {
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu",
                    id, frame.w, frame.h, out.size());
    }
    return true;
}

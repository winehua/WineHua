#include "desktop_compositor.h"
#include "frame_composer.h"
#include "frame_pipeline.h"
#include "toplevel_manager.h"
#include "compositor_utils.h"
#include "compositor/zorder_policy.h"
#include "geometry.h"
#include "compositor/surface_data.h"
#include "perf_utils.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>
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
    , zc_(*this)  // ZcBridge 绑定本类 (friend 访问层容器/tmgr/policy/root/dirty)
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

const DesktopCompositor::SubsurfaceLayer*
DesktopCompositor::FindZeroCopyLayerForToplevelLocked(uint32_t id) const
{
    // toplevel 的 zero-copy subsurface 层查找 — 层集合判定单一实现 (ZcBridge::
    // HasLayerForToplevel / GetContentSize 共用, 同一遍历同一谓词); ZC key 权威
    // 集合已迁至 zc_ (ZcBridge::activeKeys_)。
    for (const auto& layer : subsurfaceLayers_)
        if (layer.parentToplevel == id && zc_.IsActive(layer.surfaceKey))
            return &layer;
    return nullptr;
}

bool DesktopCompositor::HasZeroCopyLayerForToplevelLocked(uint32_t id) const
{
    return zc_.HasLayerForToplevel(id);
}

bool DesktopCompositor::GetZeroCopyContentSizeLocked(uint32_t toplevelId,
                                                     int& outW, int& outH) const
{
    return zc_.GetContentSize(toplevelId, outW, outH);
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

    // 层序显式化 (重构第 3B 步): 旧"嵌套循环隐式排布" (main 循环按 z-order
    // 逐窗口块 + 尾部置顶循环) 替换为"排序键显式排布" — 每项先带 ZOrderSeq
    // 排序键入 pending, std::sort 后按输出顺序分配 zIndex。输出序列与旧
    // 实现逐元素一致 (行为平价): Root 恒首 → InZOrder 段 (z-order 升序,
    // 父层 + 其非 external 子层成块, 块内父层恒首位) → TopAnchored 段
    // (subsurfaceLayers_ 原顺序)。对账点见提交说明。

    // subsurface 层字段填充块 (原两份逐字相同的填充体合并, 行为不变 — 仅
    // 组归属/排序键不同): 位置已 Resolve 为桌面坐标; zcActive 由
    // ZcBridge::IsActive (zc_) 派生 (合成/输入跳过, GPU 内容由 egl_renderer
    // 绘制); zIndex 不再在此分配 — 由排序后的输出顺序统一分配 (输出循环),
    // 编号与旧行为一致 (分配顺序=最终输出顺序)。
    auto fillSubsurfaceLayer = [&](const SubsurfaceLayer& sl) -> CompositorLayer {
        CompositorLayer subLayer;
        subLayer.type = CompositorLayer::Type::Subsurface;
        subLayer.visible = (sl.parentToplevel == rootId) ||
                           tmgr_.IsToplevelVisibleLocked(sl.parentToplevel, rootId);
        subLayer.zcActive = zc_.IsActive(sl.surfaceKey);
        subLayer.toplevelId = sl.parentToplevel;
        int lx = 0, ly = 0;
        ResolveSubsurfaceLayerPositionLocked(sl, lx, ly);
        subLayer.x = lx;
        subLayer.y = ly;
        subLayer.w = sl.w;
        subLayer.h = sl.h;
        subLayer.sub = &sl;
        return subLayer;
    };

    // 待排项: 排序键 + 排布后的层 (字段构建时即填全, 只差 zIndex)。
    struct PendingLayer {
        winehua::ZOrderSeq seq;
        CompositorLayer layer;
    };
    std::vector<PendingLayer> pending;
    pending.reserve(tmgr_.toplevelZOrder().size() + subsurfaceLayers_.size());

    // --- Toplevel 待排项: 遍历 z-order (与旧 main 循环同序遍历) ---
    // toplevel 层 (z-order 升序) + 各窗口的 subsurface 层挂在其父窗口层内
    // (文档 §4.2): z-order 更高的 toplevel 自然盖住低窗口的 subsurface —
    // 修复"GL 画面 (subsurface) 永远置顶、无法被其它窗口遮挡"。
    // root 由 Root 层表示, 不在 z-order 里重复。
    // 可见性判定与原合成/输入循环同源 (IsToplevelVisibleLocked)。
    // laneSeq = 该窗口在 z-order 的组内序号 (即已 emit 的窗口序号, 与旧 main
    // 循环"循环到的顺序"一一对应 — 注意不是 toplevelZOrder_ 原始下标: root
    // 可能占位在列, 原始下标会把整个 lane 序偏移, 破块序)。
    std::unordered_map<uint32_t, int64_t> laneOf;  // 父窗口 id → laneSeq
    laneOf.reserve(tmgr_.toplevelZOrder().size());
    int64_t zi = 0;
    for (uint32_t childId : tmgr_.toplevelZOrder()) {
        if (childId == rootId) continue;
        const auto* cst = tmgr_.FindToplevelLocked(childId);
        if (!cst) continue;
        PendingLayer p;
        p.seq.group = winehua::ZOrderGroup::InZOrder;
        p.seq.laneSeq = zi;
        p.seq.itemSeq = 0;  // 父层恒在同 lane 首位, 子层 (itemSeq=li+1) 其后
        CompositorLayer& layer = p.layer;
        layer.type = CompositorLayer::Type::Toplevel;
        layer.visible = tmgr_.IsToplevelVisibleLocked(childId, rootId);
        layer.toplevelId = childId;
        layer.x = cst->X();
        layer.y = cst->Y();
        layer.w = cst->Width();
        layer.h = cst->Height();
        layer.fullscreen = cst->IsFullscreen();
        laneOf.emplace(childId, zi);
        pending.push_back(std::move(p));
        ++zi;
    }

    // --- Subsurface 待排项: 遍历 subsurfaceLayers_ 一次 (旧 main/尾部循环
    // 各自遍历一次, 过滤条件由密钥生成收口为 ZOrderGroupFor, 行为平价) ---
    // 弹出式菜单 (isExternal, 跨窗口 offset) 不跟随父窗口 — 统一置顶。
    // 菜单恒置顶语义: 菜单挂的父窗口可能是普通应用窗口 (任务栏按钮右键
    // 菜单 owner 是应用窗口), 若跟随父窗口 z-order, 会被置顶 pin 的任务栏
    // 挡住 — 所有菜单都应叠在任务栏上方 (Windows popup 语义, 2026-08 实测)。
    // 渲染与输入共用本列表 (单一数据源), 置顶后点击菜单的命中同步优先。
    int64_t li = 0;
    for (const auto& sl : subsurfaceLayers_) {
        const winehua::ZOrderGroup grp = winehua::ZOrderGroupFor(
            sl.parentToplevel == rootId, sl.isExternal,
            tmgr_.IsInZOrder(sl.parentToplevel));
        PendingLayer p;
        p.seq.group = grp;
        p.layer = fillSubsurfaceLayer(sl);
        if (grp == winehua::ZOrderGroup::InZOrder) {
            // 组归属保证父在 z-order 且非 root (ZOrderGroupFor 已把其余归
            // TopAnchored), laneOf 必命中; 未命中 (仅 root/父缺失的假设
            // 态, 与旧 main 循环"跳过缺失父整块"同丢弃)。
            auto it = laneOf.find(sl.parentToplevel);
            if (it == laneOf.end()) { ++li; continue; }
            p.seq.laneSeq = it->second;
            p.seq.itemSeq = li + 1;  // 子层恒在父层 (itemSeq=0) 之后, 同父
                                     // 子层按 subsurfaceLayers_ 顺序 (li 递增)
        } else {
            // TopAnchored (parent==root / isExternal / 父不在 z-order:
            // 任务栏等外部层, 避免沉底回归): 恒置顶段, 顺序 = subsurfaceLayers_
            // 原顺序 (与旧尾部循环一致)。
            p.seq.laneSeq = 0;
            p.seq.itemSeq = li;
        }
        pending.push_back(std::move(p));
        ++li;
    }

    // --- 排序 (全序, 键无相等冲突: Toplevel (1,zi,0) 与子层 (1,zi,li+1)
    // li+1>=1 不同; 同父子层 li 互异; Toplevel 项 zi 互异; TopAnchored
    // (2,0,li) li 互异; 两段 group 不同) ---
    std::sort(pending.begin(), pending.end(),
              [](const PendingLayer& a, const PendingLayer& b) { return a.seq < b.seq; });

    // --- 输出: zIndex 按输出顺序递增 (Root=0 → 后续 1..N) — 与旧行为一致
    // (旧 zIndex 分配顺序 = 最终输出顺序) ---
    for (auto& p : pending) {
        p.layer.zIndex = zIndex++;
        layers.push_back(std::move(p.layer));
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
        subLayer.zcActive = zc_.IsActive(sl.surfaceKey);
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
    for (uint64_t key : zc_.activeKeys()) {
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
    // 同时 fullscreen, 规则原因/局限见 ToplevelState::fsPriority 注释)。
    // 选取法则收口于 zorder_policy (ZOrderFullscreenCandidateBeats, 行为平价)。
    uint32_t picked = 0;
    const ToplevelManager::ToplevelState* best = nullptr;
    for (const auto& layer : layers) {
        if (layer.type != CompositorLayer::Type::Toplevel ||
            !layer.visible || !layer.fullscreen) continue;
        const auto* cand = tmgr_.FindToplevelLocked(layer.toplevelId);
        if (!cand) continue;
        if (winehua::ZOrderFullscreenCandidateBeats(cand->FsPriority(),
                                                    best ? best->FsPriority() : 0,
                                                    best != nullptr)) {
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




// ============================================================================
// TakeToplevelFrame: 帧输出编排 (纯编排, 不持合成逻辑)
// ============================================================================
//
// 编排 (任务 2, 重构第 2B 步): 取帧路径按 DisplayPolicy::FrameRouteFor 路由 —
// Desktop root 帧整屏合成与 PC 单窗口帧两条路径拆为独立策略实现
// (frame_composer.{h,cpp}: DesktopRootFrameComposer / WindowFrameComposer),
// 本函数不再按 id==root 在自身内分 PC/Desktop 合成逻辑, 编排者只问策略要帧。
//
// 各路径内部: 桌面分支为"锁内规划 / 锁外绘制"两阶段 (frame_pipeline.{h,cpp} —
// FramePlanner 锁内按原内联段顺序执行产出 FramePlan, FrameBlitter 锁外纯像素
// 消费); PC 分支为窗口 SHM 帧基底 + 窗口内 subsurface blit。
// 锁边界/计时点/日志门控与原单函数实现逐段对应, 行为平价。

bool DesktopCompositor::TakeToplevelFrame(uint32_t id, std::vector<uint8_t>& out,
                                          PresentedFrame& frame) {
    // 帧级诊断统一门控 (perf_utils.h): 关闭时跳过 breakdown 累加与 [MW-TAKE] 输出
    const bool frameTrace = winehua::FrameTraceEnabled();

    // 任务 2 (重构第 2B 步): 取帧路径按 DisplayPolicy 路由 — Desktop root 帧
    // 整屏合成 (FramePlanner/FrameBlitter, 见 frame_composer.cpp) 走
    // DesktopRootFrameComposer, PC 单窗口帧走 WindowFrameComposer。两实现均
    // 无状态, 此处构图临时实例, 与原实现 "每次新造 FramePlanner" 的开销等价;
    // 锁边界/计时点/行为平价。
    switch (policy_.FrameRouteFor(id, desktopRootToplevelId_)) {
        case DisplayPolicy::FrameRoute::DesktopRoot: {
            DesktopRootFrameComposer composer(*this);
            return composer.Compose(id, out, frame, frameTrace);
        }
        case DisplayPolicy::FrameRoute::Window: {
            WindowFrameComposer composer(*this);
            return composer.Compose(id, out, frame, frameTrace);
        }
    }
    return false;  // 不可达 (FrameRoute 枚举穷尽)
}



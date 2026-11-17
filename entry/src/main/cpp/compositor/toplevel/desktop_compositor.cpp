#include "desktop_compositor.h"
#include "frame_composer.h"
#include "frame_pipeline.h"
#include "toplevel_manager.h"
#include "compositor_utils.h"
#include "compositor/toplevel/zorder_policy.h"
#include "geometry.h"
#include "compositor/frame/surface_data.h"
#include "common/perf_utils.h"
#include "common/frame_loop_diagnostics.h"
#include "compositor/input/input_resolver.h"
#include "compositor/frame/input_target_probe.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
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

bool DesktopCompositor::IsToplevelIsolated(uint32_t id) const
{
    auto lock = tmgr_.Lock();
    return isolatedToplevels_.count(id) != 0;
}

void DesktopCompositor::SetToplevelIsolated(uint32_t id, bool isolated)
{
    if (!id) return;
    auto lock = tmgr_.Lock();
    if (isolated) isolatedToplevels_.insert(id);
    else isolatedToplevels_.erase(id);
    MarkDesktopRootDirtyLocked();
    desktopCompositionSignature_ = 0;
}

bool DesktopCompositor::UpdateSubsurfaceLayerLocalPosition(wl_resource* surface, int32_t x, int32_t y,
                                                           uint32_t& outParentToplevel,
                                                           DisplayPolicy::SubsurfaceRoute& outRoute)
{
    for (auto& layer : subsurfaceLayers_) {
        if (layer.surface == surface) {
            layer.localX = x;
            layer.localY = y;
            // InlineClient 的 x/y 与 localX/localY 同义 (窗口局部坐标,
            // 见 UpdateInlineSubsurfaceOnCommit), 同步更新保持自洽;
            // DesktopLayer 的 x/y 是桌面坐标, 由 ResolveSubsurfaceLayerPositionLocked
            // 每帧重算, 此处不写。
            if (layer.route == DisplayPolicy::SubsurfaceRoute::InlineClient) {
                layer.x = x;
                layer.y = y;
            }
            outParentToplevel = layer.parentToplevel;
            outRoute = layer.route;
            return true;
        }
    }
    return false;
}

bool DesktopCompositor::RemoveSubsurfaceLayer(wl_resource* surface, uint32_t& outParentToplevel,
                                              DisplayPolicy::SubsurfaceRoute& outRoute)
{
    auto it = std::find_if(subsurfaceLayers_.begin(), subsurfaceLayers_.end(),
                           [surface](const SubsurfaceLayer& l) { return l.surface == surface; });
    if (it == subsurfaceLayers_.end()) return false;
    outParentToplevel = it->parentToplevel;
    outRoute = it->route;
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

const SubsurfaceLayer*
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
    return directDesktopVisibleToplevels_.count(id) || zc_.HasLayerForToplevel(id);
}

bool DesktopCompositor::GetZeroCopyContentSizeLocked(uint32_t toplevelId,
                                                     int& outW, int& outH) const
{
    auto direct = directDesktopContentSizes_.find(toplevelId);
    if (direct != directDesktopContentSizes_.end()) {
        outW = direct->second.first; outH = direct->second.second;
        return true;
    }
    return zc_.GetContentSize(toplevelId, outW, outH);
}

bool DesktopCompositor::ExternalWindowLiveLocked(const ExternalWindow& b) const
{
    auto* source = tmgr_.FindSurfaceResource(b.sourceKey);
    auto* host = tmgr_.FindSurfaceResource(b.hostKey);
    auto* sd = source ? tmgr_.FindRegisteredSurfaceDataLocked(source) : nullptr;
    auto* hd = host ? tmgr_.FindRegisteredSurfaceDataLocked(host) : nullptr;
    return sd && hd && sd->hasToplevel && hd->hasToplevel &&
        sd->toplevelId == b.source && hd->toplevelId == b.host;
}

bool DesktopCompositor::BindExternalToplevel(uint32_t source, uint32_t host,
                                             uint32_t sourcePid, uint32_t hostPid,
                                             int x, int y, int w, int h)
{
    auto lock = tmgr_.Lock();
    if (!policy_.RootCompositing() || !source || !host || source == host ||
        source == desktopRootToplevelId_ || host == desktopRootToplevelId_ ||
        !sourcePid || !hostPid || x < 0 || y < 0 || w < 1 || h < 1 ||
        x > 4096 || y > 4096 || w > 4096 || h > 4096) return false;
    ExternalWindow binding; binding.source = source; binding.host = host;
    binding.x = x; binding.y = y; binding.w = w; binding.h = h;
    for (const auto& [key, resource] : tmgr_.SurfaceResources()) {
        auto* sd = tmgr_.FindRegisteredSurfaceDataLocked(resource);
        if (!sd || !sd->hasToplevel) continue;
        if (sd->toplevelId == source && sd->clientPid == sourcePid) binding.sourceKey = key;
        if (sd->toplevelId == host && sd->clientPid == hostPid) binding.hostKey = key;
    }
    if (!binding.sourceKey || !binding.hostKey || !ExternalWindowLiveLocked(binding)) return false;
    for (const auto& [id, other] : externalWindows_)
        if (id != source && (other.host == host || other.source == host || other.host == source)) return false;
    const auto previous = externalWindows_.find(source);
    if (previous != externalWindows_.end() && previous->second.sourceKey == binding.sourceKey &&
        previous->second.hostKey == binding.hostKey) {
        const auto& saved = previous->second;
        binding.hasWindowedPosition = saved.hasWindowedPosition;
        binding.windowedX = saved.windowedX; binding.windowedY = saved.windowedY;
        binding.windowedWineX = saved.windowedWineX; binding.windowedWineY = saved.windowedWineY;
    }
    externalWindows_[source] = binding;
    MarkDesktopRootDirtyLocked();
    return true;
}

void DesktopCompositor::UnbindExternalToplevel(uint32_t host)
{
    auto lock = tmgr_.Lock();
    for (auto it = externalWindows_.begin(); it != externalWindows_.end();)
        if (it->second.host == host) it = externalWindows_.erase(it); else ++it;
    MarkDesktopRootDirtyLocked();
}

uint32_t DesktopCompositor::ExternalHostLocked(uint32_t source) const
{
    const auto it = externalWindows_.find(source);
    return it != externalWindows_.end() && ExternalWindowLiveLocked(it->second) ? it->second.host : 0;
}

uint32_t DesktopCompositor::ExternalSourceLocked(uint32_t host) const
{
    for (const auto& [id, binding] : externalWindows_)
        if (binding.host == host && ExternalWindowLiveLocked(binding)) return id;
    return 0;
}

void DesktopCompositor::TrackExternalHostFullscreenLocked(uint32_t host, bool on)
{
    const auto source = ExternalSourceLocked(host);
    const auto* current = tmgr_.FindToplevelLocked(host);
    if (!source || !current || current->IsFullscreen() == on) return;
    auto& binding = externalWindows_.at(source);
    auto* state = tmgr_.FindToplevelLocked(host);
    if (on && state->HasPosition()) {
        binding.hasWindowedPosition = true;
        binding.windowedX = state->X(); binding.windowedY = state->Y();
        binding.windowedWineX = state->WineX(); binding.windowedWineY = state->WineY();
    } else if (!on && binding.hasWindowedPosition) {
        state->SetPosition(binding.windowedX, binding.windowedY);
        // A compositor drag is intentionally unknown to Wine. Its old window
        // coordinates on restore must not overwrite the saved dragged position.
        state->SetWinePosition(binding.windowedWineX, binding.windowedWineY);
        binding.hasWindowedPosition = false;
    }
}

bool DesktopCompositor::IsContentVisibleLocked(uint32_t id) const
{
    if (id == desktopRootToplevelId_) return false;
    const auto* state = tmgr_.FindToplevelLocked(id);
    return state && !state->IsBackground() && !state->IsMinimized() &&
        (state->HasFrame() || zc_.HasActiveContent(id) || directDesktopVisibleToplevels_.count(id));
}

uint32_t DesktopCompositor::FirstVisibleContentModalLocked(uint32_t id) const
{
    if (!tmgr_.FindToplevelLocked(id)) return 0;
    const auto& modals = tmgr_.ModalListLocked(id);
    for (auto it = modals.rbegin(); it != modals.rend(); ++it) {
        if (IsContentVisibleLocked(*it)) return *it;
        if (const auto nested = FirstVisibleContentModalLocked(*it)) return nested;
    }
    return 0;
}

std::vector<CompositorLayer> DesktopCompositor::BuildLayerListLocked(int rootW, int rootH)
{
    for (auto it = externalWindows_.begin(); it != externalWindows_.end();)
        if (!ExternalWindowLiveLocked(it->second)) it = externalWindows_.erase(it); else ++it;
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
                           IsContentVisibleLocked(sl.parentToplevel);
        subLayer.zcActive = zc_.IsActive(sl.surfaceKey);
        subLayer.toplevelId = sl.parentToplevel;
        int lx = 0, ly = 0;
        ResolveSubsurfaceLayerPositionLocked(sl, lx, ly);
        subLayer.x = lx;
        subLayer.y = ly;
        DirectImageSampling sampling;
        if (!ComputeSubsurfaceViewport(sl, subLayer.w, subLayer.h, sampling))
            subLayer.visible = false;
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
    // WineHua: 在 owner lane 内递归展开 modal 组 (嵌套对话框: modal 又是
    // 另一 modal 的 owner)。itemSeq 用 [100000, ...) 区间 — 与 owner 的
    // subsurface 子层 (itemSeq=li+1) 不相撞, 且恒在同 lane 的 subsurface 之后
    // (模态框显示在 owner 全部内容之上, Win32 owned 窗口语义)。
    const auto kModalItemBase = 100000;
    std::function<void(uint32_t, int64_t)> appendModals = [&](uint32_t ownerId, int64_t lane) {
        int64_t item = kModalItemBase;
        for (uint32_t modalId : tmgr_.ModalListLocked(ownerId)) {
            const auto* mst = tmgr_.FindToplevelLocked(modalId);
            if (!mst) continue;
            PendingLayer mp;
            mp.seq.group = winehua::ZOrderGroup::InZOrder;
            mp.seq.laneSeq = lane;
            mp.seq.itemSeq = item++;
            CompositorLayer& mlayer = mp.layer;
            mlayer.type = CompositorLayer::Type::Toplevel;
            mlayer.visible = IsContentVisibleLocked(modalId);
            mlayer.toplevelId = modalId;
            mlayer.x = mst->X();
            mlayer.y = mst->Y();
            mlayer.w = mst->Width();
            mlayer.h = mst->Height();
            // modal 层恒以普通层参与 (fullscreen 位强制清零): 尺寸等于
            // 输出尺寸的对话框 (全屏游戏的分辨率切换确认框等) 会被 wine
            // 标 WINE_SWP_FULLSCREEN, 若带入 fullscreen 位会被
            // ShouldSkipFullscreenCascade 连渲染带命中一起跳过, 而输入
            // 拦截 (FirstVisibleModalLocked) 仍生效 → 对话框不可见且点不到
            mlayer.fullscreen = false;
            pending.push_back(std::move(mp));
            appendModals(modalId, lane);  // 嵌套: 同一 lane, 继续展开
        }
    };
    int64_t zi = 0;
    for (uint32_t childId : tmgr_.toplevelZOrder()) {
        if (childId == rootId) continue;
        if (ExternalHostLocked(childId)) continue; // Draw only inside its Wine host.
        const auto* cst = tmgr_.FindToplevelLocked(childId);
        if (!cst) continue;
        PendingLayer p;
        p.seq.group = winehua::ZOrderGroup::InZOrder;
        p.seq.laneSeq = zi;
        p.seq.itemSeq = 0;  // 父层恒在同 lane 首位, 子层 (itemSeq=li+1) 其后
        CompositorLayer& layer = p.layer;
        layer.type = CompositorLayer::Type::Toplevel;
        layer.visible = IsContentVisibleLocked(childId);
        layer.toplevelId = childId;
        layer.x = cst->X();
        layer.y = cst->Y();
        layer.w = cst->Width();
        layer.h = cst->Height();
        layer.fullscreen = cst->IsFullscreen();
        laneOf.emplace(childId, zi);
        pending.push_back(std::move(p));
        const auto source = ExternalSourceLocked(childId);
        if (source) {
            const auto& binding = externalWindows_.at(source);
            const auto* content = tmgr_.FindToplevelLocked(source);
            if (content && content->HasFrame() && content->Width() > 0 && content->Height() > 0) {
                FitRect fit;
                ComputeFitRect(binding.w, binding.h, content->Width(), content->Height(), fit);
                PendingLayer embedded;
                embedded.seq.group = winehua::ZOrderGroup::InZOrder;
                embedded.seq.laneSeq = zi; embedded.seq.itemSeq = 50000;
                auto& image = embedded.layer;
                image.type = CompositorLayer::Type::Toplevel;
                image.toplevelId = source; image.externalHostId = childId;
                image.visible = IsContentVisibleLocked(childId);
                image.x = cst->X() + binding.x + fit.offX;
                image.y = cst->Y() + binding.y + fit.offY;
                image.w = fit.dstW; image.h = fit.dstH;
                pending.push_back(std::move(embedded));
            }
        }
        appendModals(childId, zi);  // WineHua: modal 组展开 (成员不入 z-order)
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
        // 承载路由过滤 (单一权威): 本列表只服务 root 帧合成, InlineClient
        // 层由父窗口帧的 BuildWindowLayerListLocked 消费 — 若在此纳入会被
        // root 与窗口双重绘制 (坐标语义也不同: 前者桌面坐标, 后者窗口局部)。
        if (sl.route == DisplayPolicy::SubsurfaceRoute::InlineClient) continue;
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
        // A separate Ability owns these pixels and input; the Wine desktop
        // must not composite or hit-test a second copy of the same window.
        if (isolatedToplevels_.count(p.layer.toplevelId)) continue;
        p.layer.zIndex = zIndex++;
        layers.push_back(std::move(p.layer));
    }
    return layers;
}

void DesktopCompositor::ClearDirectDesktopContentSizes()
{
    auto lock = tmgr_.Lock();
    directDesktopContentSizes_.clear();
    directDesktopVisibleToplevels_.clear();
}

bool DesktopCompositor::SnapshotGpuDesktopScene(const std::vector<GpuDesktopDirectSource>& direct,
                                               GpuDesktopSnapshotCache& cache, GpuDesktopScene& out,
                                               const std::vector<GpuDesktopLayer>& zeroCopy)
{
    auto lock = tmgr_.Lock();
    out = {};
    const auto* root = tmgr_.FindToplevelLocked(desktopRootToplevelId_);
    if (!policy_.RootCompositing() || !root) return false;
    out.rootId = desktopRootToplevelId_;
    out.width = root->Width() > 0 ? root->Width() : outputW_;
    out.height = root->Height() > 0 ? root->Height() : outputH_;
    if (out.width <= 0 || out.height <= 0) return false;
    // A GPU child may arrive before its parent's first SHM frame. Validate the
    // actual protocol edge; a snapshot key alone never establishes ownership.
    const auto protocolChild = [&](const GpuDesktopLayer& source) {
        if (!source.subsurface || !source.ownerSurfaceKey) return false;
        auto* resource = tmgr_.FindSurfaceResource(source.ownerSurfaceKey);
        auto* child = resource ? static_cast<SurfaceData*>(wl_resource_get_user_data(resource)) : nullptr;
        auto* parent = child && child->isSubsurface && child->parentSurface
            ? tmgr_.FindRegisteredSurfaceDataLocked(child->parentSurface) : nullptr;
        if (!parent || !parent->hasToplevel || parent->toplevelId != source.parentToplevel ||
            tmgr_.FindSurfaceResource(parent->surfaceKey) != child->parentSurface) return false;
        const auto* state = tmgr_.FindToplevelLocked(source.parentToplevel);
        return !state || (!state->IsBackground() && !state->IsMinimized());
    };
    std::unordered_map<uint32_t, std::vector<std::pair<SurfaceData*, GpuDesktopLayer>>> clients;
    directDesktopContentSizes_.clear();
    directDesktopVisibleToplevels_.clear();
    for (const auto& source : direct) {
        const uint64_t key = (uint64_t(source.pid) << 32) | source.wlSurface;
        auto* resource = tmgr_.FindSurfaceResource(key);
        auto* client = resource ? static_cast<SurfaceData*>(wl_resource_get_user_data(resource)) : nullptr;
        auto* parent = client && client->isSubsurface && client->parentSurface
            ? tmgr_.FindRegisteredSurfaceDataLocked(client->parentSurface) : nullptr;
        if (!client || !parent || !parent->hasToplevel || parent->toplevelId != source.toplevel ||
            client->directOwnerKey != parent->surfaceKey || !client->directVisible ||
            client->directGeneration != source.generation || !source.generation ||
            tmgr_.FindSurfaceResource(parent->surfaceKey) != client->parentSurface) continue;
        GpuDesktopLayer image;
        image.directSurfaceKey = key;
        image.ownerSurfaceKey = key; image.parentToplevel = source.toplevel;
        image.subsurface = true;
        image.x = client->subsurfaceX; image.y = client->subsurfaceY;
        if (!ComputeDirectViewport(client->directViewport, source.width, source.height,
                                   image.w, image.h, image.sampling)) continue;
        image.sourceW = source.width; image.sourceH = source.height;
        // The full client, rather than an auxiliary tiny CEF child, determines
        // fullscreen/input scaling. All actual drawables remain composited.
        directDesktopVisibleToplevels_.insert(source.toplevel);
        const auto* state = tmgr_.FindToplevelLocked(source.toplevel);
        const bool mainCanvas = image.x == 0 && image.y == 0 &&
            (!state || !state->HasFrame() ||
             (image.w >= state->Width() && image.h >= state->Height()));
        if (mainCanvas) {
            auto& size = directDesktopContentSizes_[source.toplevel];
            if (int64_t(image.w) * image.h > int64_t(size.first) * size.second)
                size = {image.w, image.h};
        }
        clients[source.toplevel].emplace_back(client, image);
    }
    for (auto& [top, children] : clients)
        std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
            return a.first->directGeneration < b.first->directGeneration;
        });
    auto layers = BuildLayerListLocked(out.width, out.height);
    const auto fullscreenId = PickFullscreenLayerLocked(layers);
    std::unordered_set<uint64_t> orderedDrawables;
    for (const auto& layer : layers) if (layer.type == CompositorLayer::Type::Subsurface)
        orderedDrawables.insert(layer.sub->surfaceKey);
    // Bounded, opt-in observation of this exact decision. No pointer escapes
    // the lock and no input event, raise, focus or resolver log cache is changed.
    if (winehua::FrameTraceEnabled() || (winehua::FrameLoopDiagnosticState() & 1)) {
        const uint64_t now = winehua::PerfNowUs();
        if (now >= nextSceneDiagnosticUs_) {
            nextSceneDiagnosticUs_ = now + 2000000;
            out.diagnosticSerial = ++sceneDiagnosticSerial_;
            out.diagnosticUs = now;
            InputResolver probe(tmgr_, *this, desktopRootToplevelId_, outputW_, outputH_);
            InputTarget hit;
            const double cx = out.width / 2.0, cy = out.height / 2.0;
            const bool hitOk = probe.FindInputTargetInLayersLocked(cx, cy, hit, layers,
                out.width, out.height, fullscreenId, false);
            const auto last = winehua::LastInputTarget();
            OH_LOG_INFO(LOG_APP, "[SCENE-DECISION] sample=%{public}llu at_us=%{public}llu "
                "fullscreen=%{public}u center=(%{public}.1f,%{public}.1f) currentHit=%{public}u "
                "hit_ok=%{public}d modal=%{public}u swallow=%{public}d layers=%{public}zu "
                "lastInputTarget=%{public}u last_event_us=%{public}llu last_xy=(%{public}.1f,%{public}.1f)",
                static_cast<unsigned long long>(out.diagnosticSerial), static_cast<unsigned long long>(now),
                fullscreenId, cx, cy, hit.toplevelId, hitOk, hit.blockedModalId, hit.swallow, layers.size(),
                last.toplevelId, static_cast<unsigned long long>(last.eventUs), last.desktopX, last.desktopY);
            size_t count = 0;
            for (const auto& layer : layers) {
                if (layer.type != CompositorLayer::Type::Toplevel) continue;
                if (count++ >= 16) continue;
                const auto* state = tmgr_.FindToplevelLocked(layer.toplevelId);
                if (!state) continue;
                const char* reason = !layer.visible ? "not-visible" :
                    ShouldSkipFullscreenCascade(layer, fullscreenId, fullscreenId != 0, tmgr_)
                        ? "fullscreen-cascade" : "included";
                OH_LOG_INFO(LOG_APP, "[SCENE-CANDIDATE] sample=%{public}llu top=%{public}u z=%{public}zu "
                    "priority=%{public}llu shm=%{public}d gpu=%{public}d fs=%{public}d "
                    "background=%{public}d minimized=%{public}d modalOwner=%{public}u reason=%{public}s",
                    static_cast<unsigned long long>(out.diagnosticSerial), layer.toplevelId, layer.zIndex,
                    static_cast<unsigned long long>(state->FsPriority()), state->HasFrame(),
                    zc_.HasActiveContent(layer.toplevelId), layer.fullscreen, state->IsBackground(),
                    state->IsMinimized(), state->ModalOwnerId(), reason);
            }
            OH_LOG_INFO(LOG_APP, "[SCENE-CANDIDATES-END] sample=%{public}llu total=%{public}zu truncated=%{public}zu",
                static_cast<unsigned long long>(out.diagnosticSerial), count, count > 16 ? count - 16 : 0);
        }
    }
    FitRect fullscreenFit;
    bool fullscreenUsesGpuContent = false;
    if (fullscreenId) ComputeFullscreenFitLocked(fullscreenId, out.width, out.height,
                                                fullscreenFit, &fullscreenUsesGpuContent);
    std::unordered_set<uint64_t> used;
    auto snapshot = [&](GpuDesktopLayer& layer, const std::vector<uint8_t>& pixels) {
        if (layer.sourceW <= 0 || layer.sourceH <= 0 ||
            pixels.size() < static_cast<size_t>(layer.sourceW) * layer.sourceH * 4) return false;
        auto& saved = cache[layer.key];
        if (!saved.pixels || saved.serial != layer.serial || saved.sourceW != layer.sourceW ||
            saved.sourceH != layer.sourceH) {
            saved = layer;
            saved.pixels = std::make_shared<const std::vector<uint8_t>>(pixels);
        }
        layer.pixels = saved.pixels;
        used.insert(layer.key);
        return true;
    };
    for (const auto& layer : layers) {
        if (!layer.visible || ShouldSkipFullscreenCascade(layer, fullscreenId, fullscreenId != 0, tmgr_)) continue;
        GpuDesktopLayer item;
        item.parentToplevel = layer.type == CompositorLayer::Type::Root ? out.rootId : layer.toplevelId;
        item.x = layer.x; item.y = layer.y; item.w = layer.w; item.h = layer.h;
        const std::vector<uint8_t>* pixels = nullptr;
        if (layer.type == CompositorLayer::Type::Subsurface) {
            auto client = clients.find(layer.toplevelId);
            if (client != clients.end()) {
                auto drawable = std::find_if(client->second.begin(), client->second.end(),
                    [&](const auto& entry) { return layer.sub->surface == entry.first->surface; });
                if (drawable != client->second.end()) {
                    auto image = drawable->second;
                    image.x = layer.x; image.y = layer.y;
                    const auto* state = fullscreenId && layer.toplevelId == fullscreenId
                        ? tmgr_.FindToplevelLocked(fullscreenId) : nullptr;
                    if (state) FitMapLayerRect(fullscreenFit, image.x - state->X(), image.y - state->Y(),
                        image.w, image.h, image.x, image.y, image.w, image.h);
                    out.layers.push_back(std::move(image));
                    continue;
                }
            }
            if (layer.zcActive && zeroCopy.empty()) continue;
            const auto& sub = *layer.sub;
            item.subsurface = true;
            item.external = sub.isExternal;
            item.ownerSurfaceKey = sub.surfaceKey;
            item.key = sub.surfaceKey;
            item.serial = sub.shmCommitSerial;
            item.sourceW = sub.w; item.sourceH = sub.h;
            if (!ComputeSubsurfaceViewport(sub, item.w, item.h, item.sampling)) continue;
            item.opaque = sub.shmFormat != 0;
            pixels = &sub.pixels;
        } else {
            const auto* state = layer.type == CompositorLayer::Type::Root ? root : tmgr_.FindToplevelLocked(layer.toplevelId);
            if (!state) continue;
            item.key = (1ULL << 63) | (layer.type == CompositorLayer::Type::Root ? out.rootId : layer.toplevelId);
            item.serial = state->FrameSerial();
            item.sourceW = state->Width(); item.sourceH = state->Height();
            item.opaque = state->ShmFormat() != 0;
            pixels = &state->Pixels();
        }
        const auto* fsState = fullscreenId ? tmgr_.FindToplevelLocked(fullscreenId) : nullptr;
        if (fsState && layer.toplevelId == fullscreenId)
            FitMapLayerRect(fullscreenFit, item.x - fsState->X(), item.y - fsState->Y(),
                            item.w, item.h, item.x, item.y, item.w, item.h);
        const bool fullscreenWindow = layer.type == CompositorLayer::Type::Toplevel &&
                                      layer.toplevelId == fullscreenId &&
                                      fullscreenFit.dstW > 0 && fullscreenFit.dstH > 0;
        if (fullscreenWindow) {
            // The fitted content covers only its aspect-preserving rectangle.
            // Put an opaque backing in this window's lane for SHM, Direct and
            // zero-copy alike; later menus/popups retain their normal order.
            GpuDesktopLayer black;
            black.key = (1ULL << 62) | fullscreenId;
            black.solidBlack = true; black.w = out.width; black.h = out.height;
            out.layers.push_back(std::move(black));
        }
        const bool fullscreenDirect = fullscreenWindow && fullscreenUsesGpuContent && clients.count(fullscreenId);
        if (!fullscreenDirect && !(layer.zcActive && !zeroCopy.empty()) && snapshot(item, *pixels))
            out.layers.push_back(std::move(item));
        else if (!fullscreenDirect && !zeroCopy.empty()) out.layers.push_back(std::move(item));
        if (layer.type == CompositorLayer::Type::Toplevel) {
            auto client = clients.find(layer.toplevelId);
            if (client == clients.end()) continue;
            for (const auto& drawable : client->second) {
                if (orderedDrawables.count(drawable.first->surfaceKey)) continue;
                auto image = drawable.second;
                if (layer.toplevelId == fullscreenId) {
                    FitMapLayerRect(fullscreenFit, image.x, image.y, image.w, image.h,
                                    image.x, image.y, image.w, image.h);
                } else { image.x += layer.x; image.y += layer.y; }
                out.layers.push_back(std::move(image));
            }
        }
    }
    if (!zeroCopy.empty()) {
        std::vector<GpuDesktopLayer> native;
        for (auto source : zeroCopy) {
            if (isolatedToplevels_.count(source.parentToplevel)) continue;
            uint64_t liveOwner = 0;
            uint32_t liveTop = 0;
            int liveW = 0, liveH = 0;
            if (!zc_.ActiveOwner(source.zeroCopyKey, liveOwner, liveTop, liveW, liveH) ||
                liveOwner != source.ownerSurfaceKey || liveTop != source.parentToplevel) continue;
            const auto owner = std::find_if(layers.begin(), layers.end(), [&](const auto& layer) {
                return (layer.type == CompositorLayer::Type::Toplevel &&
                        layer.toplevelId == source.parentToplevel) ||
                       (layer.type == CompositorLayer::Type::Root && source.parentToplevel == out.rootId);
            });
            if (owner == layers.end()) {
                // Match BuildLayerListLocked's existing TopAnchored child rule.
                // Without a parent lane only a real protocol child can enter;
                // never synthesize an owner or choose an arbitrary z-order.
                if (!protocolChild(source) || fullscreenId ||
                    winehua::ZOrderGroupFor(source.parentToplevel == out.rootId, source.external,
                                           tmgr_.IsInZOrder(source.parentToplevel)) !=
                        winehua::ZOrderGroup::TopAnchored) continue;
                source.external = true;
            } else if (!owner->visible ||
                       ShouldSkipFullscreenCascade(*owner, fullscreenId, fullscreenId != 0, tmgr_)) continue;
            const auto* state = fullscreenId && source.parentToplevel == fullscreenId
                ? tmgr_.FindToplevelLocked(fullscreenId) : nullptr;
            if (state) FitMapLayerRect(fullscreenFit, source.x - state->X(), source.y - state->Y(),
                                      source.w, source.h, source.x, source.y, source.w, source.h);
            native.push_back(std::move(source));
        }
        MergeZeroCopySceneLayers(out, native);
    }
    for (auto it = cache.begin(); it != cache.end();)
        if (!used.count(it->first)) it = cache.erase(it); else ++it;
    return true;
}

bool DesktopCompositor::GetDirectDesktopLayout(uint32_t pid, uint32_t id,
                                               uint32_t wlSurfaceId, int imageW, int imageH, uint64_t generation,
                                               DirectDesktopLayout& out, uint32_t scope, const char** rejectionReason)
{
    auto lk = tmgr_.Lock();
    if (rejectionReason) *rejectionReason = nullptr;
    auto reject = [&](const char* reason) {
        if (rejectionReason) *rejectionReason = reason;
        return false;
    };
    const auto* state = tmgr_.FindToplevelLocked(id);
    if (!tmgr_.IsToplevelVisibleLocked(id, desktopRootToplevelId_))
        return reject(id == desktopRootToplevelId_ ? "desktop_root" :
            !state ? "missing_toplevel" : state->IsMinimized() ? "minimized" :
            state->IsBackground() ? "background" : "no_shm_frame");
    if (scope ? (scope != id || !policy_.OhosWindowPerToplevel()) : !policy_.RootCompositing())
        return reject("scope_mismatch");
    const auto* root = tmgr_.FindToplevelLocked(desktopRootToplevelId_);
    auto* resource = tmgr_.FindSurfaceResource((static_cast<uint64_t>(pid) << 32) | wlSurfaceId);
    auto* client = resource ? static_cast<SurfaceData*>(wl_resource_get_user_data(resource)) : nullptr;
    auto* parent = client && client->isSubsurface && client->parentSurface
        ? tmgr_.FindRegisteredSurfaceDataLocked(client->parentSurface) : nullptr;
    if (scope && state && client && imageW > 0 && imageH > 0 && generation) {
        // Legacy managed Wine WSI binds the queue to its exact toplevel;
        // desktop WSI uses the independently declared client subsurface.
        const bool ownToplevel = client->hasToplevel && client->toplevelId == id;
        const bool ownChild = parent && parent->hasToplevel && parent->toplevelId == id &&
            client->directOwnerKey == parent->surfaceKey && client->directVisible &&
            client->directGeneration == generation &&
            tmgr_.FindSurfaceResource(parent->surfaceKey) == client->parentSurface;
        if (!ownToplevel && !ownChild) return reject("surface_owner_mismatch");
        out = {};
        DirectImageSampling sampling;
        if (ownChild) {
            if (!ComputeDirectViewport(client->directViewport, imageW, imageH, out.w, out.h, sampling))
                return reject("invalid_viewport");
            const auto offset = ComputePopupOffset(client->subsurfaceX, client->subsurfaceY,
                parent->committed.contentRect.x, parent->committed.contentRect.y);
            out.x = offset.first; out.y = offset.second;
        } else {
            // Legacy managed WSI identifies the queue by its parent toplevel,
            // but that parent's viewport belongs to the decorated SHM buffer.
            // Resolve the unique inline client by Wine's protocol role instead
            // of applying that crop to a smaller native swapchain image.
            const SurfaceData* drawable = nullptr;
            for (const auto& [key, resource] : tmgr_.SurfaceResources()) {
                const auto* child = static_cast<SurfaceData*>(wl_resource_get_user_data(resource));
                if (!child || !child->isSubsurface || child->parentSurface != client->surface ||
                    !DisplayPolicy::IsInlineClientSurface(child->inputRegionEmpty, child->vpSrcW)) continue;
                if (drawable && drawable != child) return reject("ambiguous_managed_client");
                drawable = child;
            }
            if (drawable) {
                if (!ComputeDirectViewport(drawable->directViewport, imageW, imageH, out.w, out.h, sampling))
                    return reject("invalid_client_viewport");
                const auto offset = ComputePopupOffset(drawable->subsurfaceX, drawable->subsurfaceY,
                    client->committed.contentRect.x, client->committed.contentRect.y);
                out.x = offset.first; out.y = offset.second;
            } else {
                // A whole-window producer without a separate client is valid
                // only when its image covers that exact window. Wait for the
                // real client role if its decorated parent is larger.
                if (imageW != state->Width() || imageH != state->Height())
                    return reject("managed_client_pending");
                out.w = imageW; out.h = imageH;
            }
            // Managed Wine WSI binds its entire-window GPU producer to the
            // toplevel. Inline SHM clients are below that producer, exactly as
            // for the whole-window ZC layer in BuildWindowLayerListLocked.
            // Treating their retained white/GDI frames as later occluders
            // clips away all Direct pixels. Menus/popups have separate OHOS
            // outputs; only explicitly declared child drawables below need
            // sibling occlusion within this output.
            return true;
        }
        // Other OHOS windows have their own output. Only this window's later
        // inline children can occlude its Direct layer.
        const auto layers = BuildWindowLayerListLocked(id, state->Width(), state->Height());
        if (ownChild) for (const auto& layer : layers)
            if (layer.type == CompositorLayer::Type::Subsurface && layer.sub->surface == client->surface)
                out.z = layer.zIndex;
        for (const auto& layer : layers) {
            if (layer.type != CompositorLayer::Type::Subsurface || !layer.visible || layer.zcActive ||
                layer.sub->surface == client->surface || layer.zIndex <= out.z) continue;
            out.occluders.push_back({layer.x, layer.y,
                DisplaySizeAfterViewport(layer.sub->vpDstW, layer.w),
                DisplaySizeAfterViewport(layer.sub->vpDstH, layer.h)});
        }
        return true;
    }
    if (scope || !root || !state || !client || !parent || !parent->hasToplevel || parent->toplevelId != id ||
        client->directOwnerKey != parent->surfaceKey || !client->directVisible ||
        client->directGeneration != generation || !generation ||
        tmgr_.FindSurfaceResource(parent->surfaceKey) != client->parentSurface)
        return reject(!client ? "missing_surface" : !parent ? "missing_parent" : "drawable_identity");
    out = {};
    out.x = state->X() + client->subsurfaceX;
    out.y = state->Y() + client->subsurfaceY;
    DirectImageSampling sampling;
    if (!ComputeDirectViewport(client->directViewport, imageW, imageH, out.w, out.h, sampling)) return reject("invalid_viewport");
    out.fullscreen = state->IsFullscreen();
    if (imageW <= 0 || imageH <= 0) return false;
    const auto layers = BuildLayerListLocked(root->Width(), root->Height());
    const uint32_t fullscreenId = PickFullscreenLayerLocked(layers);
    bool found = false;
    for (const auto& layer : layers) {
        if (layer.type == CompositorLayer::Type::Toplevel && layer.toplevelId == id) {
            if (ShouldSkipFullscreenCascade(layer, fullscreenId, fullscreenId != 0, tmgr_))
                return false;
            out.z = layer.zIndex;
            found = true;
            break;
        }
    }
    if (!found) return false;
    for (const auto& layer : layers) {
        if (!layer.visible || layer.zcActive || layer.zIndex <= out.z ||
            ShouldSkipFullscreenCascade(layer, fullscreenId, fullscreenId != 0, tmgr_)) continue;
        if (layer.type == CompositorLayer::Type::Subsurface && layer.sub->surface == client->surface)
            continue;
        if (layer.type == CompositorLayer::Type::Toplevel && layer.fullscreen)
            out.occluders.push_back({0, 0, root->Width(), root->Height()});
        else if (layer.type == CompositorLayer::Type::Subsurface)
            out.occluders.push_back({layer.x, layer.y,
                DisplaySizeAfterViewport(layer.sub->vpDstW, layer.w),
                DisplaySizeAfterViewport(layer.sub->vpDstH, layer.h)});
        else if (layer.type == CompositorLayer::Type::Toplevel)
            out.occluders.push_back({layer.x, layer.y, layer.w, layer.h});
    }
    return out.w > 0 && out.h > 0;
}

std::vector<CompositorLayer>
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

    // 窗口内 subsurface 层 (窗口局部坐标): 只收 InlineClient (多窗口模式客户区,
    // 见 DisplayPolicy::SubsurfaceRoute)。判定用显式 route 字段, 不用
    // isExternal 反推 (后者只表达"坐标是否 Wine 基底", 与承载方式正交) —
    // DesktopLayer 层属 root 帧合成, Popup 层不在本容器 (走伪 toplevel)。
    // 层序 = 容器顺序 (protocol 顺序, place_above/below 经同一容器重排)。
    for (const auto& sl : subsurfaceLayers_) {
        if (sl.parentToplevel != toplevelId) continue;
        if (sl.route != DisplayPolicy::SubsurfaceRoute::InlineClient) continue;
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
            auto* parent = tmgr_.FindRegisteredSurfaceDataLocked(sd->parentSurface);
            if (!parent || parent->toplevelId != toplevelId) continue;
            zcLayer.type = CompositorLayer::Type::Subsurface;
            // 父几何读点 (重构第 5A2 步): 旧读 parent->geoX/geoY (即时窗口
            // 几何值), 新读 parent->committed.contentRect.x/y — 同一写入点
            // (xs_set_window_geometry 直写快照) 的同步表达式, 逐点等价。
            // 偏移公式收口 (重构第 5B2 步): geometry.h ComputePopupOffset 单点
            // (PLAN §2.3 4 份公式), 算法逐字 (offX = subX - parentContentX)
            const auto [subOffX, subOffY] =
                ComputePopupOffset(sd->subsurfaceX, sd->subsurfaceY,
                                   parent->committed.contentRect.x,
                                   parent->committed.contentRect.y);
            zcLayer.x = subOffX;
            zcLayer.y = subOffY;
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
                                                   FitRect& out, bool* gpuContent) const
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
    if (gpuContent) *gpuContent = hasZC;
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

void DesktopCompositor::RequestFullRedraw(uint32_t id) {
    auto lock = tmgr_.Lock();
    desktopOutputInitialized_ = false;
    if (auto* state = tmgr_.FindToplevelLocked(id)) state->MarkDirty();
    MarkDesktopRootDirtyLocked();
}

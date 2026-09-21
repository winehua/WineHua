#include "zc_bridge.h"

#include <string>
#include <vector>
#include <cstring>

#include "graphics/graphics_broker.h"          // SetZeroCopySurfaceReady (ready marker, 进程单例)
#include "compositor/frame/surface_data.h"  // SurfaceData (wl_resource_get_user_data)
#include "compositor_utils.h"         // CompensateMinimizedSubsurfaceOffset
#include "desktop_compositor.h"       // DesktopCompositor (friend), SubsurfaceLayer
#include "geometry.h"                 // DisplaySizeAfterViewport
#include "toplevel_manager.h"
#include "input_target_probe.h"       // P0-1 Task A: 最近输入目标 (仅诊断)

#include <algorithm>  // std::find / std::max / std::min
#include <atomic>     // std::memory_order_acquire
#include <chrono>
#include <cstdint>
#include <cstdio>     // TEMP-DIAG(FRAME-DUMP)
#include <cstdlib>
#include <fcntl.h>
#include <map>        // TEMP-DIAG(FRAME-DUMP): 每窗口的上次抓帧时间/次数
#include <set>        // TEMP-DIAG(FRAME-DUMP): 每个窗口只抓一帧
#include <string>
#include <vector>     // TEMP-DIAG(BIND): 有界去重表
#include <unistd.h>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

namespace {
// TEMP-DIAG(BIND): 有界去重 (OHOS libc++ 没有 std::hash<std::string>, 不能用 unordered_set)
bool BindDiagDedupInsert(std::vector<std::string>& v, const std::string& s)
{
    for (const auto& e : v) if (e == s) return false;
    if (v.size() < 256) v.push_back(s);
    return true;
}

// TEMP-DIAG(BIND): 同时写 hilog 和 app temp 文件 —— hilog 环形缓冲会滚掉历史,
// 绑定探针的证据必须落盘 (temp/bind-diag-<pid>.log, 每个进程上限 4MB)。
void BindDiagEmit(const char* line)
{
    OH_LOG_INFO(LOG_APP, "%{public}s", line);
    static int fd = -1;
    static size_t written = 0;
    if (fd < 0)
    {
        char path[160];
        snprintf(path, sizeof(path), "/data/storage/el2/base/temp/bind-diag-%d.log", (int)getpid());
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (fd >= 0 && written < (4u << 20))
    {
        ssize_t w = write(fd, line, strlen(line));
        if (w > 0) written += (size_t)w;
    }
}
}  // namespace


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
                            ZeroCopyLayerInfo& info, const char** outReason)
{
    // 诊断输出 (2026-09-16): 每次拒绝都带原因码, 行为与旧实现逐点一致。
    const auto reject = [outReason](const char* reason) {
        if (outReason) *outReason = reason;
        return false;
    };
    if (outReason) *outReason = "ok";
    auto lk = comp_.tmgr_.Lock();

    auto* wlRes = comp_.tmgr_.FindSurfaceResource(surfaceKey);
    auto* sd = wlRes
        ? static_cast<SurfaceData*>(wl_resource_get_user_data(wlRes)) : nullptr;

    // P0-1 L0：已有 PresentBinding → 直接以绑定窗口作为几何来源（不再做几何搜索）。
    // 绑定建立后 resize/move 只更新窗口几何，identity 不再重猜（方案 §10）。
    {
        WineHuaPresentBinding binding;
        bool newlyBound = false;
        if (ResolvePresentBinding(surfaceKey, static_cast<uint32_t>(fallbackWidth),
                                  static_cast<uint32_t>(fallbackHeight), &binding, &newlyBound))
        {
            const uint64_t boundWindowKey =
                (static_cast<uint64_t>(binding.window.ownerHostPid) << 32) | binding.window.wlSurfaceId;
            auto* boundRes = comp_.tmgr_.FindSurfaceResource(boundWindowKey);
            auto* boundSd = boundRes
                ? static_cast<SurfaceData*>(wl_resource_get_user_data(boundRes)) : nullptr;
            if (boundSd)
            {
                wlRes = boundRes;
                sd = boundSd;
                if (outReason) *outReason = "present-binding";
                // 2026-09-20: 这里**不再**写消费时刻。本函数同时服务"每帧查询"与
                // "真正消费"两条路径, 把查询当消费会让 lastProducerUs/lastDrawUs
                // 变成"渲染器上次查询时刻"(= 恒为 8ms 的假活跃)。真实消费时刻改由
                // 渲染线程在 UpdateSurfaceImage 成功后经 NoteLayerConsumed 写入。
            }
        }
    }

    if (!wlRes) return reject("no_surface_resource");
    if (!sd) return reject("no_surface_data");

    // -- present surface → owner 窗口解析 (2026-09-16, Steam 跨进程黑窗) --
    //
    // Wine 为 Vulkan present 单独创建一个 role-less wl_surface (guest 侧
    // winehua_surface_id = 该 host surface 的低 32 位 = 这个 wl_surface 的 id)。
    // 它既不是 toplevel 也不是 subsurface, 于是旧实现按 "surface_without_toplevel"
    // 拒绝, app 侧永远不 attach, presenter 侧每次 present 都要等满 2.5s 然后返回
    // -EAGAIN (=DXVK 眼里的 VK_SUBOPTIMAL_KHR) —— 黑窗 + CEF 卡死即由此而来。
    //
    // 解析规则 (保守, 只在唯一可判定时才接管):
    //   1. 只允许同 client pid 的 surface 作为 owner 候选 (约束: ownerPid + surfaceId);
    //   2. 候选必须是 toplevel 本身, 或 parent 有 toplevel 的 subsurface;
    //   3. 若 present 报的尺寸与候选 committed 尺寸一致者唯一 → 取之;
    //      否则若候选唯一 → 取之; 其余情况一律拒绝 (ambiguous_owner)。
    // 解析结果只替换"几何来源" (sd/wlRes), 目标 key 仍是 present 侧的 key,
    // 因此 presenter 侧 attach/呈现契约不变。
    bool ownerResolved = false;
    if (!sd->hasToplevel && !sd->isSubsurface)
    {
        const uint32_t presentPidForLog = sd->clientPid;
        struct OwnerCandidate {
            wl_resource* res = nullptr;
            SurfaceData* sd = nullptr;
            bool sizeMatches = false;
        };
        OwnerCandidate only{};
        OwnerCandidate sized{};
        OwnerCandidate sizedAnyPid{};
        uint32_t ownerCandidates = 0;
        uint32_t sizeMatches = 0;
        uint32_t sizeMatchesAnyPid = 0;
        for (const auto& [otherKey, otherRes] : comp_.tmgr_.SurfaceResources())
        {
            static_cast<void>(otherKey);
            if (!otherRes || otherRes == wlRes) continue;
            auto* otherSd = static_cast<SurfaceData*>(wl_resource_get_user_data(otherRes));
            if (!otherSd) continue;
            SurfaceData* windowSd = otherSd;
            if (otherSd->isSubsurface && otherSd->parentSurface)
                windowSd = static_cast<SurfaceData*>(
                    wl_resource_get_user_data(otherSd->parentSurface));
            if (!windowSd || !windowSd->hasToplevel) continue;
            // 桌面根 toplevel 是合成画布而不是"某个窗口": 尺寸恒等于桌面尺寸,
            // 会被误当成候选并让真正的窗口变成"歧义", 因此直接排除。
            if (windowSd->toplevelId == comp_.desktopRootToplevelId_) continue;
            // 根合成下只接受当前可见窗口 (隐藏/最小化窗口不可能是呈现目标)
            if (comp_.policy_.RootCompositing() &&
                !comp_.tmgr_.IsToplevelVisibleLocked(windowSd->toplevelId,
                                                     comp_.desktopRootToplevelId_))
                continue;
            // 匹配尺寸优先用 surface 的 committed 尺寸; 最大化/无 shm 提交的窗口
            // committed 尺寸为 0, 此时退回该 toplevel 的合成几何 (Width/Height)。
            int windowW = otherSd->w;
            int windowH = otherSd->h;
            if ((windowW <= 0 || windowH <= 0) && windowSd->hasToplevel)
            {
                if (const auto* windowState =
                        comp_.tmgr_.FindToplevelLocked(windowSd->toplevelId))
                {
                    windowW = windowState->Width();
                    windowH = windowState->Height();
                }
            }
            const bool matches = fallbackWidth > 0 && fallbackHeight > 0 &&
                windowW == fallbackWidth && windowH == fallbackHeight;
            if (otherSd->clientPid != sd->clientPid)
            {
                // 跨进程 owner (CEF 的 GPU 进程为浏览器进程的窗口 present):
                // 只收尺寸完全一致的候选, 以免误绑到同进程外的无关窗口。
                if (matches)
                {
                    sizedAnyPid = {otherRes, otherSd, true};
                    ++sizeMatchesAnyPid;
                }
                continue;
            }
            ++ownerCandidates;
            only = {otherRes, otherSd, false};
            if (matches)
            {
                sized = {otherRes, otherSd, true};
                ++sizeMatches;
            }
        }
        if (sizeMatches == 1)
        {
            wlRes = sized.res;
            sd = sized.sd;
            ownerResolved = true;
            static std::unordered_set<uint64_t> ownerResolvedLogged;
            if (ownerResolvedLogged.insert(surfaceKey).second)
                OH_LOG_WARN(LOG_APP,
                            "[MW-ZC] present owner by size key=%{public}llu "
                            "owner_pid=%{public}u owner_surface=%{public}u "
                            "size=%{public}dx%{public}d top=%{public}u",
                            static_cast<unsigned long long>(surfaceKey), sd->clientPid,
                            sd->protocolId, fallbackWidth, fallbackHeight, sd->toplevelId);
        }
        else if (ownerCandidates == 1)
        {
            wlRes = only.res;
            sd = only.sd;
            ownerResolved = true;
            static std::unordered_set<uint64_t> ownerResolvedLogged;
            if (ownerResolvedLogged.insert(surfaceKey).second)
                OH_LOG_WARN(LOG_APP,
                            "[MW-ZC] present owner by unique window key=%{public}llu "
                            "owner_pid=%{public}u owner_surface=%{public}u top=%{public}u",
                            static_cast<unsigned long long>(surfaceKey), sd->clientPid,
                            sd->protocolId, sd->toplevelId);
        }
        else if (sizeMatchesAnyPid == 1)
        {
            wlRes = sizedAnyPid.res;
            sd = sizedAnyPid.sd;
            ownerResolved = true;
            static std::unordered_set<uint64_t> ownerResolvedLogged;
            if (ownerResolvedLogged.insert(surfaceKey).second)
                OH_LOG_WARN(LOG_APP,
                            "[MW-ZC] present owner by size (cross-pid) key=%{public}llu "
                            "present_pid=%{public}u owner_pid=%{public}u "
                            "owner_surface=%{public}u size=%{public}dx%{public}d top=%{public}u",
                            static_cast<unsigned long long>(surfaceKey), presentPidForLog,
                            sd->clientPid, sd->protocolId, fallbackWidth, fallbackHeight,
                            sd->toplevelId);
        }
        else
        {
            static std::unordered_set<uint64_t> ownerAmbiguousLogged;
            if (ownerAmbiguousLogged.insert(surfaceKey).second)
                OH_LOG_WARN(LOG_APP,
                            "[MW-ZC] present owner ambiguous key=%{public}llu "
                            "pid=%{public}u surface=%{public}u candidates=%{public}u "
                            "size_matches=%{public}u size_any_pid=%{public}u "
                            "size=%{public}dx%{public}d",
                            static_cast<unsigned long long>(surfaceKey), sd->clientPid,
                            sd->protocolId, ownerCandidates, sizeMatches, sizeMatchesAnyPid,
                            fallbackWidth, fallbackHeight);
        }
    }
    static_cast<void>(ownerResolved);

    info = {};
    info.surfaceKey = surfaceKey;
    info.clientPid = sd->clientPid;
    info.surfaceId = sd->protocolId;
    if (sd->isSubsurface && sd->parentSurface)
    {
        auto* parent = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
        if (!parent || !parent->hasToplevel) return reject("parent_without_toplevel");
        info.parentToplevel = parent->toplevelId;
        const auto* parentState = comp_.tmgr_.FindToplevelLocked(info.parentToplevel);
        info.width = DisplaySizeAfterViewport(sd->vpDstW, sd->w);
        info.height = DisplaySizeAfterViewport(sd->vpDstH, sd->h);
        if (comp_.policy_.RootCompositing())
        {
            if (rendererToplevelId != comp_.desktopRootToplevelId_)
                return reject("renderer_not_desktop_root");
            // A protocol-only GL/Vulkan child can be the first buffer-bearing
            // surface of a window. In that case the xdg-toplevel is valid but no
            // ToplevelState/SHM frame exists yet, so the general compositor
            // visibility predicate would create a circular attach dependency.
            // An existing state still supplies the lifecycle exclusions that
            // matter here.
            if (info.parentToplevel != comp_.desktopRootToplevelId_ &&
                parentState &&
                (parentState->IsBackground() || parentState->IsMinimized()))
                return reject("parent_hidden");
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
                return info.width > 0 && info.height > 0
                    ? true : reject("shm_layer_zero_size");
            }

            // Vulkan private-present surfaces may have no wl_shm commit. Wayland
            // still supplies the parent/offset while the present protocol supplies
            // the image dimensions.
            int sx = sd->subsurfaceX;
            int sy = sd->subsurfaceY;
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
            return info.width > 0 && info.height > 0
                ? true : reject("protocol_only_zero_size");
        }

        if (rendererToplevelId != info.parentToplevel)
            return reject("subsurface_toplevel_mismatch");
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
        return info.width > 0 && info.height > 0
            ? true : reject("subsurface_zero_size");
    }

    if (!sd->hasToplevel) return reject("surface_without_toplevel");
    info.parentToplevel = sd->toplevelId;
    info.width = sd->w;
    info.height = sd->h;
    if (info.width <= 0) info.width = fallbackWidth;
    if (info.height <= 0) info.height = fallbackHeight;
    info.shmCommitSerial = sd->shmCommitSerial.load(std::memory_order_acquire);
    if (comp_.policy_.RootCompositing())
    {
        if (rendererToplevelId != comp_.desktopRootToplevelId_ ||
            (sd->toplevelId != comp_.desktopRootToplevelId_ &&
             !comp_.tmgr_.IsToplevelVisibleLocked(sd->toplevelId, comp_.desktopRootToplevelId_)))
            return reject(rendererToplevelId != comp_.desktopRootToplevelId_
                              ? "renderer_not_desktop_root" : "toplevel_not_visible");
        if (const auto* st = comp_.tmgr_.FindToplevelLocked(sd->toplevelId)) {
            info.x = st->X();
            info.y = st->Y();
            info.fullscreen = st->IsFullscreen();
        }
        info.desktopCoordinates = true;
        return info.width > 0 && info.height > 0
            ? true : reject("toplevel_zero_size");
    }
    if (rendererToplevelId != sd->toplevelId) return reject("toplevel_renderer_mismatch");
    return info.width > 0 && info.height > 0 ? true : reject("toplevel_zero_size");
}

// 诊断 (只看不改): 该 key 在当前合成状态下的原始事实, 用于解释 GetLayerInfo 的拒绝原因。
// ============================================================================
// 2026-09-20 关键修复: 真实 present 活性 (producer liveness)
// ----------------------------------------------------------------------------
// 现场 (ROUND3 §7): producer 16:38:14 之后不再交帧, 但 STEAM-WINDOW 一直显示
// producerAgeMs≈8ms, 因为旧实现把"渲染循环每帧查询"当成"producer 活跃":
// 同一字段既驱动黑窗归因, 又驱动"能否被新 producer 接管"的守卫 → 被弃用的
// producer 永远霸占窗口, 新 present surface 拿不到窗口, 帧流再也回不来。
// 现在活性只来自 native present 回调 (NoteProducerPresent)。
void ZcBridge::NoteProducerPresent(uint64_t surfaceKey, uint64_t nowUs)
{
    if (!surfaceKey) return;
    std::lock_guard<std::mutex> lock(presentLivenessMutex_);
    lastPresentUsByKey_[surfaceKey] = nowUs;
}

uint64_t ZcBridge::LastPresentUs(uint64_t surfaceKey) const
{
    if (!surfaceKey) return 0;
    std::lock_guard<std::mutex> lock(presentLivenessMutex_);
    const auto it = lastPresentUsByKey_.find(surfaceKey);
    return it == lastPresentUsByKey_.end() ? 0 : it->second;
}

void ZcBridge::NoteLayerConsumed(uint64_t surfaceKey, uint64_t nowUs)
{
    if (!surfaceKey) return;
    auto lk = comp_.tmgr_.Lock();
    const auto it = presentBindings_.find(surfaceKey);
    if (it != presentBindings_.end()) it->second.lastDrawUs = nowUs;
}

void ZcBridge::PruneStaleWindowBindings()
{
    for (auto it = windowBindings_.begin(); it != windowBindings_.end();)
    {
        const bool bindingAlive = presentBindings_.count(it->second) > 0;
        const bool resourceAlive = comp_.tmgr_.FindSurfaceResource(it->first) != nullptr;
        if (bindingAlive && resourceAlive)
        {
            ++it;
            continue;
        }
        it = windowBindings_.erase(it);
    }
}

// P0-1 PresentBinding (2026-09-17) — 调用方须已持有 comp_.tmgr_ 锁
// ============================================================================
bool ZcBridge::ResolvePresentBinding(uint64_t surfaceKey, uint32_t frameWidth,
                                     uint32_t frameHeight, WineHuaPresentBinding* outBinding,
                                     bool* outNewlyBound)
{
    if (!surfaceKey || !outBinding) return false;
    if (outNewlyBound) *outNewlyBound = false;
    // 2026-09-20: 先清掉"窗口 → 已消失 producer"的残留映射。否则该窗口永远无法
    // 被新 producer 接管 (ROUND3 §7.2: 表面重建后新 present surface 拿不到窗口)。
    PruneStaleWindowBindings();
    const uint64_t nowUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    const uint32_t presenterHostPid = static_cast<uint32_t>(surfaceKey >> 32);
    const uint32_t presentSurfaceId = static_cast<uint32_t>(surfaceKey);
    const uint32_t rootId = comp_.desktopRootToplevelId_;
    const auto windowKeyOf = [](const SurfaceData* sd) {
        return (static_cast<uint64_t>(sd->clientPid) << 32) | sd->protocolId;
    };

    // ---- TEMP-DIAG(BIND-PRODUCER, 2026-09-18): producer arrival + frame-chain activity ----
    if (frameWidth && frameHeight)
    {
        auto& rec = bindDiagProducers_[surfaceKey];
        if (!rec.firstUs) rec.firstUs = nowUs;
        rec.lastUs = nowUs;
        rec.frames++;
        if (rec.width != frameWidth || rec.height != frameHeight)
        {
            rec.width = frameWidth;
            rec.height = frameHeight;
            char line[256];
            snprintf(line, sizeof(line),
                     "BIND-PRODUCER: key=0x%llx hostPid=%u surfaceId=%u extent=%ux%u frames=%u bound=%d\n",
                     static_cast<unsigned long long>(surfaceKey), presenterHostPid,
                     presentSurfaceId, frameWidth, frameHeight, rec.frames, rec.bound ? 1 : 0);
            BindDiagEmit(line);
        }
    }
    // 只观察不绑定: 候选窗口四套尺寸 + 每条拒绝原因 (去重)
    if (frameWidth && frameHeight)
        BindDiagPass(surfaceKey, frameWidth, frameHeight, presenterHostPid, presentSurfaceId, rootId);

    // L0：已有绑定，且窗口仍在 → 直接复用（绝不重新搜索）
    auto existing = presentBindings_.find(surfaceKey);
    if (existing != presentBindings_.end())
    {
        // Task E: 已被接替的旧 producer → 解除其绑定 (其 layer 由渲染器释放),
        // 避免旧 producer 与接管者争抢同一扇窗口。
        if (existing->second.retired)
        {
            // 2026-09-20: 只有该窗口**仍然指向这个被退役的 producer** 时才清映射。
            // 接管 (BIND-TAKEOVER/BIND-RETIRE) 之后窗口已经指向新 producer, 旧
            // producer 的收尾若按窗口无条件删除, 会把新绑定一起抹掉 → 窗口回到
            // binding=none / 黑 SHM (现场: 17:14:02.745 BIND-RETIRE 之后
            // 17:14:03.332 STEAM-WINDOW 立刻变成 binding=none producer=0x0)。
            const uint64_t retiredWindowKey =
                (static_cast<uint64_t>(existing->second.window.ownerHostPid) << 32) |
                existing->second.window.wlSurfaceId;
            const auto retiredWindow = windowBindings_.find(retiredWindowKey);
            if (retiredWindow != windowBindings_.end() && retiredWindow->second == surfaceKey)
                windowBindings_.erase(retiredWindow);
            presentBindings_.erase(existing);
            return false;
        }
        const uint64_t boundKey = (static_cast<uint64_t>(existing->second.window.ownerHostPid) << 32) |
                                  existing->second.window.wlSurfaceId;
        if (comp_.tmgr_.FindSurfaceResource(boundKey))
        {
            // L0 复用：刷新 producer 侧 extent；活性时刻只取**真实 present** 表
            // (2026-09-20: 旧实现写 nowUs, 使失效 producer 永远显示活跃)
            existing->second.frameWidth = frameWidth;
            existing->second.frameHeight = frameHeight;
            if (const uint64_t lastPresentUs = LastPresentUs(surfaceKey))
                existing->second.lastProducerUs = lastPresentUs;
            // Task E/F: 接管者出第一帧后, 才真正退役旧 producer (旧 layer 在此期间继续显示最后帧)
            if (existing->second.pending)
            {
                existing->second.pending = false;
                const uint64_t windowKey = boundKey;
                for (auto& [producerKey, other] : presentBindings_)
                {
                    if (producerKey == surfaceKey) continue;
                    const uint64_t otherWindowKey =
                        (static_cast<uint64_t>(other.window.ownerHostPid) << 32) | other.window.wlSurfaceId;
                    if (otherWindowKey != windowKey || other.retired) continue;
                    other.retired = true;
                    OH_LOG_INFO(LOG_APP,
                                "BIND-RETIRE: window=(%{public}u,%{public}u) old=0x%{public}llx "
                                "replaced_by=0x%{public}llx (first frame of new producer)",
                                other.window.ownerHostPid, other.window.wlSurfaceId,
                                static_cast<unsigned long long>(producerKey),
                                static_cast<unsigned long long>(surfaceKey));
                }
            }
            *outBinding = existing->second;
            return true;
        }
        windowBindings_.erase(boundKey);
        presentBindings_.erase(existing);   // 窗口已销毁 → 失效，下一 generation 重绑
    }

    SurfaceData* candidate = nullptr;
    const char* reason = nullptr;
    bool takeoverCandidate = false;

    // Task D (2026-09-17): 候选窗口判定 —— 除 toplevel 外, **subsurface 也必须是候选**:
    // Wine 的菜单/tooltip/dialog 在这套 compositor 里表现为「父 toplevel 的 subsurface」,
    // 旧实现只收 hasToplevel → 这些 popup 的 producer 永远拿不到 WindowKey（菜单黑）。
    // 规则: 自身或父有 toplevel、非桌面根、未最小化/未被切走、尺寸 >0。
    const auto considerWindow = [&](SurfaceData* sd, uint32_t* outTopId, int* outW, int* outH,
                                    bool* outSizeKnown) -> bool {
        if (!sd) return false;
        uint32_t topId = 0;
        if (sd->hasToplevel)
        {
            topId = sd->toplevelId;
        }
        else if (sd->isSubsurface && sd->parentSurface)
        {
            auto* parent = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
            if (!parent || !parent->hasToplevel) return false;
            topId = parent->toplevelId;
        }
        else return false;
        if (!topId || topId == rootId) return false;
        const auto* st = comp_.tmgr_.FindToplevelLocked(topId);
        if (!st || st->IsMinimized() || st->IsBackground()) return false;
        *outTopId = topId;
        if (sd->hasToplevel)
        {
            *outW = st->Width();
            *outH = st->Height();
            *outSizeKnown = true;
        }
        else
        {
            // subsurface: 用自身 committed 尺寸 (菜单/tooltip 的实际大小)
            *outW = sd->w;
            *outH = sd->h;
            *outSizeKnown = sd->w > 0 && sd->h > 0;
        }
        return true;
    };

    // L1：同进程显式路径（GTA V / Heaven / 普通 DXVK：presenterHostPid == ownerHostPid）
    {
        uint32_t count = 0;
        SurfaceData* only = nullptr;
        for (const auto& [key, res] : comp_.tmgr_.SurfaceResources())
        {
            static_cast<void>(key);
            if (!res) continue;
            auto* other = static_cast<SurfaceData*>(wl_resource_get_user_data(res));
            uint32_t otherTopId = 0;
            int otherW = 0, otherH = 0;
            bool otherSizeKnown = false;
            if (!considerWindow(other, &otherTopId, &otherW, &otherH, &otherSizeKnown)) continue;
            if (other->clientPid != presenterHostPid) continue;
            if (windowBindings_.count(windowKeyOf(other))) continue;
            only = other;
            ++count;
        }
        if (count == 1) { candidate = only; reason = "same-process"; }
    }

    // L3：首次几何 bootstrap —— 必须唯一候选，且 toplevel/可见/非桌面根/尺寸精确相等
    if (!candidate && frameWidth && frameHeight)
    {
        uint32_t count = 0;
        SurfaceData* only = nullptr;
        for (const auto& [key, res] : comp_.tmgr_.SurfaceResources())
        {
            static_cast<void>(key);
            if (!res) continue;
            auto* other = static_cast<SurfaceData*>(wl_resource_get_user_data(res));
            uint32_t otherTopId = 0;
            int otherW = 0, otherH = 0;
            bool otherSizeKnown = false;
            if (!considerWindow(other, &otherTopId, &otherW, &otherH, &otherSizeKnown)) continue;
            // Task E (2026-09-17): 占用同一窗口的旧 producer 只有在"已停帧 (>1s)"时才
            // 允许被接管；仍活跃的占用者保持独占（避免抢窗口导致旧内容立即消失）。
            bool takeover = false;
            if (const auto claim = windowBindings_.find(windowKeyOf(other));
                claim != windowBindings_.end())
            {
                const auto claimBinding = presentBindings_.find(claim->second);
                if (claimBinding == presentBindings_.end())
                {
                    // 2026-09-20: claim 指向的 producer 已无绑定 (被 release/销毁) →
                    // 该占用是残留, 直接清掉并让本 producer 正常竞争该窗口。
                    windowBindings_.erase(claim);
                }
                else
                {
                // 活性取自**真实 present** 时间戳 (旧实现取查询刷新值 → 永不超时,
                // 新 producer 永远无法接管被弃用的窗口)。
                const uint64_t claimLastUs = LastPresentUs(claim->second);
                const uint64_t claimAgeMs =
                    claimLastUs && nowUs > claimLastUs ? (nowUs - claimLastUs) / 1000 : 0;
                if (claimAgeMs <= 1000) continue;
                takeover = true;
                }
            }
            const bool sizeMatch = otherSizeKnown &&
                                   static_cast<uint32_t>(otherW) == frameWidth &&
                                   static_cast<uint32_t>(otherH) == frameHeight;
            const bool bufferMatch = static_cast<uint32_t>(other->w) == frameWidth &&
                                     static_cast<uint32_t>(other->h) == frameHeight;
            if (!sizeMatch && !bufferMatch) continue;
            only = other;
            takeoverCandidate = takeover;
            ++count;
        }
        if (count == 1) {
            candidate = only;
            reason = takeoverCandidate ? "unique-geometry-takeover" : "unique-geometry";
        }
    }

    if (!candidate)
    {
        if (bindingRejectedLogged_.insert(surfaceKey).second)
        {
            // 2026-09-17: 拒绝时把候选集打出来（计划 §9 要求"安全失败"，但必须可诊断）。
            // 统计口径：所有非桌面根、未最小化/未被切走的 toplevel 窗口。
            uint32_t totalWindows = 0, sizeMatched = 0, shown = 0;
            for (const auto& [key, res] : comp_.tmgr_.SurfaceResources())
            {
                static_cast<void>(key);
                if (!res) continue;
                auto* other = static_cast<SurfaceData*>(wl_resource_get_user_data(res));
                if (!other || !other->hasToplevel || other->toplevelId == rootId) continue;
                const auto* st = comp_.tmgr_.FindToplevelLocked(other->toplevelId);
                if (!st || st->IsMinimized() || st->IsBackground()) continue;
                ++totalWindows;
                const bool stateMatch = static_cast<uint32_t>(st->Width()) == frameWidth &&
                                        static_cast<uint32_t>(st->Height()) == frameHeight;
                const bool bufferMatch = static_cast<uint32_t>(other->w) == frameWidth &&
                                         static_cast<uint32_t>(other->h) == frameHeight;
                if (!stateMatch && !bufferMatch) continue;
                ++sizeMatched;
                if (shown < 4)
                {
                    ++shown;
                    OH_LOG_WARN(LOG_APP,
                                "BIND-CAND: producer=0x%{public}llx window=(%{public}u,%{public}u) "
                                "toplevel=%{public}u state=%{public}dx%{public}d buffer=%{public}dx%{public}d "
                                "claimed=%{public}d",
                                static_cast<unsigned long long>(surfaceKey), other->clientPid,
                                other->protocolId, other->toplevelId, st->Width(), st->Height(),
                                other->w, other->h,
                                windowBindings_.count(windowKeyOf(other)) ? 1 : 0);
                }
            }
            OH_LOG_WARN(LOG_APP,
                        "BIND-REJECT: producer=0x%{public}llx presenterHostPid=%{public}u "
                        "presentSurfaceId=%{public}u size=%{public}ux%{public}u "
                        "bindings=%{public}zu windows=%{public}u size_matches=%{public}u "
                        "(no unique candidate)",
                        static_cast<unsigned long long>(surfaceKey), presenterHostPid,
                        presentSurfaceId, frameWidth, frameHeight, presentBindings_.size(),
                        totalWindows, sizeMatched);
        }
        return false;
    }

    WineHuaPresentBinding binding;
    binding.producer.presenterHostPid = presenterHostPid;
    binding.producer.presentSurfaceId = presentSurfaceId;
    binding.producer.producerGeneration = 0;
    binding.window.ownerHostPid = candidate->clientPid;
    binding.window.wlSurfaceId = candidate->protocolId;
    binding.window.windowGeneration = 0;
    binding.bindGeneration = 0;
    binding.reason = reason;
    binding.frameWidth = frameWidth;
    binding.frameHeight = frameHeight;
    // 2026-09-20: 活性取真实 present 时间 (可能为 0 = 尚未见过该 producer 交帧);
    // 消费时刻交给 NoteLayerConsumed, 不再用"绑定发生时刻"冒充两者。
    binding.lastProducerUs = LastPresentUs(surfaceKey);
    binding.lastDrawUs = 0;

    const uint64_t windowKey = windowKeyOf(candidate);
    presentBindings_[surfaceKey] = binding;
    // Task E: 若是从已停帧的旧 producer 手里接管, 标记 pending ——
    // 旧 producer 的 layer 继续显示最后帧, 直到本 producer 出第一帧再退役旧绑定。
    if (takeoverCandidate)
    {
        presentBindings_[surfaceKey].pending = true;
        OH_LOG_INFO(LOG_APP,
                    "BIND-TAKEOVER: window=(%{public}u,%{public}u) new=0x%{public}llx "
                    "extent=%{public}ux%{public}u (old producer stalled)",
                    candidate->clientPid, candidate->protocolId,
                    static_cast<unsigned long long>(surfaceKey), frameWidth, frameHeight);
    }
    windowBindings_[windowKey] = surfaceKey;
    bindDiagProducers_[surfaceKey].bound = true;
    snprintf(bindDiagProducers_[surfaceKey].lastReject,
             sizeof(bindDiagProducers_[surfaceKey].lastReject), "bound:%s", reason);
    if (outNewlyBound) *outNewlyBound = true;
    *outBinding = binding;

    OH_LOG_INFO(LOG_APP,
                "BIND: producer=0x%{public}llx presenterHostPid=%{public}u presentSurfaceId=%{public}u "
                "→ window ownerHostPid=%{public}u wlSurfaceId=%{public}u toplevelId=%{public}u "
                "reason=%{public}s size=%{public}ux%{public}u",
                static_cast<unsigned long long>(surfaceKey), presenterHostPid, presentSurfaceId,
                candidate->clientPid, candidate->protocolId, candidate->toplevelId, reason,
                frameWidth, frameHeight);
    return true;
}

void ZcBridge::InvalidateBindingsForWindow(uint32_t ownerHostPid, uint32_t wlSurfaceId)
{
    if (!ownerHostPid || !wlSurfaceId) return;
    const uint64_t windowKey = (static_cast<uint64_t>(ownerHostPid) << 32) | wlSurfaceId;
    auto it = windowBindings_.find(windowKey);
    if (it == windowBindings_.end()) return;
    const uint64_t producerKey = it->second;
    windowBindings_.erase(it);
    presentBindings_.erase(producerKey);
    bindingRejectedLogged_.erase(producerKey);
    OH_LOG_INFO(LOG_APP,
                "BIND-RELEASE: worker=0x%{public}llx window=(%{public}u,%{public}u) "
                "reason=window-destroyed",
                static_cast<unsigned long long>(producerKey), ownerHostPid, wlSurfaceId);
}

// P0-1 Task A (2026-09-17): per-window 黑窗归因快照。
// 一次日志即可判定黑窗属于哪一类（方案 §2/§3）：
//   NoBinding       窗口没有 producer 绑定
//   ProducerStall   绑定存在但 producer 侧很久没有帧被消费
//   CompositeStall  绑定存在且 producer 活跃，但合成侧没有消费它
//   CEFBlackContent 两侧都活跃（内容黑 → 属于 CEF/runtime 问题）
void ZcBridge::DumpWindowBindingDiag()
{
    const uint64_t nowUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint32_t rootId = comp_.desktopRootToplevelId_;
    const uint32_t inputTarget = winehua::LastInputTargetToplevel();

    /* TEMP-DIAG(BIND-PRODUCER-STATE, 2026-09-18): 每个 producer 的帧链/绑定状态与最后拒绝原因。
     * 与 BIND-PRODUCER / BIND-REJECT 一起回答"win64 native 下 CEF 有多少 producer、
     * 哪个没绑上、为什么"。 */
    for (const auto& [producerKey, rec] : bindDiagProducers_)
    {
        const bool boundNow = presentBindings_.count(producerKey) > 0;
        const uint64_t producerAgeMs =
            rec.lastUs && nowUs > rec.lastUs ? (nowUs - rec.lastUs) / 1000 : 0;
        // 2026-09-20: 另打印**真实 present** 时延 (上面的 ageMs 是查询时延)。
        const uint64_t lastPresentUs = LastPresentUs(producerKey);
        const uint64_t presentAgeMs = lastPresentUs && nowUs > lastPresentUs
            ? (nowUs - lastPresentUs) / 1000 : 0;
        char line[320];
        snprintf(line, sizeof(line),
                 "BIND-PRODUCER-STATE: producer=0x%llx extent=%ux%u frames=%u ageMs=%llu "
                 "presentAgeMs=%llu binding=%s lastReject=%s contentRectWouldMatch=%d\n",
                 static_cast<unsigned long long>(producerKey), rec.width, rec.height, rec.frames,
                 static_cast<unsigned long long>(producerAgeMs),
                 static_cast<unsigned long long>(presentAgeMs), boundNow ? "bound" : "none",
                 rec.lastReject[0] ? rec.lastReject : "none", rec.contentRectWouldMatch ? 1 : 0);
        BindDiagEmit(line);
    }

    for (const auto& [key, res] : comp_.tmgr_.SurfaceResources())
    {
        static_cast<void>(key);
        if (!res) continue;
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(res));
        if (!sd || !sd->hasToplevel || sd->toplevelId == rootId) continue;
        const auto* st = comp_.tmgr_.FindToplevelLocked(sd->toplevelId);
        if (!st || st->IsMinimized() || st->IsBackground()) continue;

        const uint64_t windowKey = (static_cast<uint64_t>(sd->clientPid) << 32) | sd->protocolId;
        const auto bound = windowBindings_.find(windowKey);
        const WineHuaPresentBinding* binding = nullptr;
        if (bound != windowBindings_.end())
        {
            const auto it = presentBindings_.find(bound->second);
            if (it != presentBindings_.end()) binding = &it->second;
        }

        const auto ageMs = [nowUs](uint64_t thenUs) -> uint64_t {
            return thenUs && nowUs > thenUs ? (nowUs - thenUs) / 1000 : 0;
        };
        uint64_t producerAgeMs = 0, drawAgeMs = 0;
        const char* klass = "CEFBlackContent";
        if (!binding)
        {
            // 没有 producer 绑定并不等于异常：走 SHM 的普通窗口本来就没有 Native Producer。
            klass = st->HasFrame() ? "SHM" : "NoContent";
        }
        else
        {
            producerAgeMs = ageMs(binding->lastProducerUs);
            drawAgeMs = ageMs(binding->lastDrawUs);
            if (producerAgeMs > 5000) klass = "ProducerDead";
            else if (producerAgeMs > 1000) klass = "ProducerStall";
            else if (drawAgeMs > 1000) klass = "CompositeStall";
            else klass = "OK";
        }

        /* TEMP-DIAG(FRAME-DUMP): 把合成器实际收到的 SHM 帧周期性落盘 (同一窗口覆盖同一个文件)。
         * 为什么周期抓: 只抓"第一帧"可能拿到启动阶段的占位黑帧, 必须看稳态最新帧
         * (后续计划 §2.1: 确认抓到的是当前最新内容 buffer)。
         * 结果文件: temp/frame-w<W>-h<H>-own<pid>-<surf>.raw (头行含 W/H/FMT/TLV/OWNER/PIX)。
         * 用途: 区分"帧内容本身黑(client/CEF 侧没画)" vs "帧有内容但没上屏"; 不依赖手机屏幕。 */
        {
            static std::map<std::string, uint64_t> lastDumpMs;
            static std::map<std::string, int> dumpCount;
            const bool bigWindow = st->Width() >= 1000;
            const uint64_t nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (st->HasFrame() && st->Width() >= 200 && st->Height() >= 20 &&
                (bigWindow || dumpCount.size() < 4))
            {
                char path[192];
                snprintf(path, sizeof(path), "/data/storage/el2/base/temp/frame-w%u-h%u-own%u-%u.raw",
                         st->Width(), st->Height(), sd->clientPid, sd->protocolId);
                const std::string key(path);
                auto it = lastDumpMs.find(key);
                const bool due = (it == lastDumpMs.end()) || (nowMs - it->second >= 30000);
                if (due && dumpCount[key] < 8)
                {
                    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0)
                    {
                        const auto& px = st->Pixels();
                        char hdr[192];
                        int hn = snprintf(hdr, sizeof(hdr),
                                          "W %d H %d FMT %u TLV %u OWNER %u,%u PIX %zu SERIAL %llu\n",
                                          st->Width(), st->Height(), st->ShmFormat(), sd->toplevelId,
                                          sd->clientPid, sd->protocolId, px.size(),
                                          static_cast<unsigned long long>(st->FrameSerial()));
                        if (hn > 0) write(fd, hdr, static_cast<size_t>(hn));
                        size_t off = 0;
                        while (off < px.size())
                        {
                            ssize_t w = write(fd, px.data() + off, px.size() - off);
                            if (w <= 0) break;
                            off += static_cast<size_t>(w);
                        }
                        close(fd);
                        lastDumpMs[key] = nowMs;
                        dumpCount[key]++;
                        OH_LOG_INFO(LOG_APP,
                                    "FRAME-DUMP: %{public}s w=%{public}d h=%{public}d fmt=%{public}u n=%{public}d",
                                    path, st->Width(), st->Height(), st->ShmFormat(), dumpCount[key]);
                    }
                }
            }
        }
        OH_LOG_INFO(LOG_APP,
                    "STEAM-WINDOW: window=(%{public}u,%{public}u) toplevel=%{public}u "
                    "geometry=%{public}dx%{public}d+%{public}d,%{public}d visible=%{public}d "
                    "binding=%{public}s producer=0x%{public}llx extent=%{public}ux%{public}u "
                    "producerAgeMs=%{public}llu drawAgeMs=%{public}llu contentSource=%{public}s "
                    "shmFrame=%{public}d inputTarget=%{public}u class=%{public}s",
                    sd->clientPid, sd->protocolId, sd->toplevelId,
                    st->Width(), st->Height(), st->X(), st->Y(),
                    comp_.tmgr_.IsToplevelVisibleLocked(sd->toplevelId, rootId) ? 1 : 0,
                    binding ? "bound" : "none",
                    static_cast<unsigned long long>(bound != windowBindings_.end() ? bound->second : 0),
                    binding ? binding->frameWidth : 0u, binding ? binding->frameHeight : 0u,
                    static_cast<unsigned long long>(producerAgeMs),
                    static_cast<unsigned long long>(drawAgeMs),
                    binding ? "VenusNative" : (st->HasFrame() ? "SHM" : "none"),
                    st->HasFrame() ? 1 : 0, inputTarget, klass);
    }
}

bool ZcBridge::DiagnoseLayerInfo(uint64_t surfaceKey, uint32_t rendererToplevelId,
                                 ZcLayerDiag& out)
{
    auto lk = comp_.tmgr_.Lock();
    out = {};
    out.rootCompositing = comp_.policy_.RootCompositing();
    out.desktopRootToplevelId = comp_.desktopRootToplevelId_;
    out.registeredSurfaces = static_cast<uint32_t>(comp_.tmgr_.SurfaceResourceCount());
    auto* wlRes = comp_.tmgr_.FindSurfaceResource(surfaceKey);
    if (!wlRes) return false;
    out.hasResource = true;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(wlRes));
    if (!sd) return false;
    out.hasData = true;
    out.hasToplevel = sd->hasToplevel;
    out.isSubsurface = sd->isSubsurface;
    out.clientPid = sd->clientPid;
    out.protocolId = sd->protocolId;
    out.toplevelId = sd->toplevelId;
    out.surfaceW = sd->w;
    out.surfaceH = sd->h;
    out.vpDstW = sd->vpDstW;
    out.vpDstH = sd->vpDstH;
    out.subX = sd->subsurfaceX;
    out.subY = sd->subsurfaceY;
    if (sd->isSubsurface && sd->parentSurface)
    {
        auto* parent = static_cast<SurfaceData*>(wl_resource_get_user_data(sd->parentSurface));
        if (parent)
        {
            out.parentToplevel = parent->toplevelId;
            out.hasToplevel = parent->hasToplevel;
        }
    }
    else if (sd->hasToplevel)
    {
        out.parentToplevel = sd->toplevelId;
        out.rootVisible = comp_.tmgr_.IsToplevelVisibleLocked(
            sd->toplevelId, comp_.desktopRootToplevelId_);
    }
    // 同一 client pid 的其余 surface: 用于判定 present surface 的 owner 窗口
    for (const auto& [otherKey, otherRes] : comp_.tmgr_.SurfaceResources())
    {
        static_cast<void>(otherKey);
        if (out.peerCount >= 8) break;
        if (!otherRes || otherRes == wlRes) continue;
        auto* otherSd = static_cast<SurfaceData*>(wl_resource_get_user_data(otherRes));
        if (!otherSd || otherSd->clientPid != sd->clientPid) continue;
        auto& peer = out.peers[out.peerCount++];
        peer.surfaceId = otherSd->protocolId;
        peer.toplevelId = static_cast<uint32_t>(otherSd->toplevelId);
        peer.hasToplevel = otherSd->hasToplevel ? 1 : 0;
        peer.isSubsurface = otherSd->isSubsurface ? 1 : 0;
        peer.w = static_cast<int16_t>(otherSd->w);
        peer.h = static_cast<int16_t>(otherSd->h);
        if (otherSd->isSubsurface && otherSd->parentSurface)
        {
            auto* parent = static_cast<SurfaceData*>(
                wl_resource_get_user_data(otherSd->parentSurface));
            if (parent) peer.parentToplevel = parent->toplevelId;
        }
    }
    // 全量注册表 (跨 client): owner 解析失败时用于定位"哪个 toplevel 才是目标窗口"
    for (const auto& [otherKey, otherRes] : comp_.tmgr_.SurfaceResources())
    {
        static_cast<void>(otherKey);
        if (out.regCount >= 16) break;
        if (!otherRes) continue;
        auto* otherSd = static_cast<SurfaceData*>(wl_resource_get_user_data(otherRes));
        if (!otherSd) continue;
        auto& entry = out.registry[out.regCount++];
        entry.clientPid = otherSd->clientPid;
        entry.surfaceId = otherSd->protocolId;
        entry.toplevelId = static_cast<uint32_t>(otherSd->toplevelId);
        entry.hasToplevel = otherSd->hasToplevel ? 1 : 0;
        entry.isSubsurface = otherSd->isSubsurface ? 1 : 0;
        entry.w = static_cast<int16_t>(otherSd->w);
        entry.h = static_cast<int16_t>(otherSd->h);
        if (otherSd->isSubsurface && otherSd->parentSurface)
        {
            auto* parent = static_cast<SurfaceData*>(
                wl_resource_get_user_data(otherSd->parentSurface));
            if (parent) entry.parentToplevel = parent->toplevelId;
        }
        if (entry.hasToplevel)
        {
            if (const auto* st = comp_.tmgr_.FindToplevelLocked(entry.toplevelId))
            {
                entry.stateW = static_cast<int16_t>(st->Width());
                entry.stateH = static_cast<int16_t>(st->Height());
            }
        }
    }
    static_cast<void>(rendererToplevelId);
    return true;
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
            // A native-only window may commit an all-zero SHM placeholder while
            // its pixels live in a separate, role-less present surface. Repainting
            // that placeholder over another native window hides its actual frame.
            bool nativeBound = false;
            for (const auto& [windowKey, producerKey] : windowBindings_) {
                static_cast<void>(producerKey);
                auto* resource = comp_.tmgr_.FindSurfaceResource(windowKey);
                auto* window = resource
                    ? static_cast<SurfaceData*>(wl_resource_get_user_data(resource)) : nullptr;
                if (window && window->hasToplevel &&
                    window->toplevelId == layer.toplevelId) {
                    nativeBound = true;
                    break;
                }
            }
            const auto* state = nativeBound
                ? comp_.tmgr_.FindToplevelLocked(layer.toplevelId) : nullptr;
            if (state && state->HasFrame()) {
                if (blankShmFrames_.size() >= 64 &&
                    blankShmFrames_.count(layer.toplevelId) == 0)
                    blankShmFrames_.clear();
                auto& cached = blankShmFrames_[layer.toplevelId];
                if (!cached.valid || cached.serial != state->FrameSerial()) {
                    cached.serial = state->FrameSerial();
                    cached.blank = std::all_of(state->Pixels().begin(), state->Pixels().end(),
                                               [](uint8_t pixel) { return pixel == 0; });
                    cached.valid = true;
                }
                if (cached.blank) continue;
            }
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

// TEMP-DIAG(BIND-PRODUCER, 2026-09-18): observation-only candidate/rejection report.
// Answers: how many CEF producers exist, their extents/frame counts, which Wayland
// toplevel/subsurface candidates exist (with the four size views), which rule rejected
// each pair, and whether a contentRect-exact match would have succeeded.
// Never changes binding behaviour; dedup sets are bounded.
void ZcBridge::BindDiagPass(uint64_t surfaceKey, uint32_t frameWidth, uint32_t frameHeight,
                            uint32_t presenterHostPid, uint32_t presentSurfaceId, uint32_t rootId)
{
    auto& rec = bindDiagProducers_[surfaceKey];
    rec.bound = presentBindings_.count(surfaceKey) > 0;
    int eligible = 0;

    for (const auto& [key, res] : comp_.tmgr_.SurfaceResources())
    {
        static_cast<void>(key);
        if (!res) continue;
        auto* other = static_cast<SurfaceData*>(wl_resource_get_user_data(res));
        if (!other) continue;

        uint32_t topId = 0;
        int candW = 0, candH = 0;
        bool sizeKnown = false;
        const bool windowLike = [&]() -> bool {
            if (other->hasToplevel) topId = other->toplevelId;
            else if (other->isSubsurface && other->parentSurface) {
                auto* parent = static_cast<SurfaceData*>(wl_resource_get_user_data(other->parentSurface));
                if (!parent || !parent->hasToplevel) return false;
                topId = parent->toplevelId;
            } else return false;
            if (!topId) return false;
            const auto* st = comp_.tmgr_.FindToplevelLocked(topId);
            if (!st) return false;
            if (other->hasToplevel) { candW = st->Width(); candH = st->Height(); sizeKnown = true; }
            else { candW = other->w; candH = other->h; sizeKnown = other->w > 0 && other->h > 0; }
            return true;
        }();

        const auto& c = other->committed;
        const auto* st = topId ? comp_.tmgr_.FindToplevelLocked(topId) : nullptr;
        const int rawW = c.w, rawH = c.h;
        const int vpW = other->vpDstW, vpH = other->vpDstH;
        const int outerW = candW, outerH = candH;
        const int crX = c.hasWindowGeometry ? c.contentRect.x : -1;
        const int crY = c.hasWindowGeometry ? c.contentRect.y : -1;
        const int crW = c.hasWindowGeometry ? c.contentRect.w : -1;
        const int crH = c.hasWindowGeometry ? c.contentRect.h : -1;
        const bool crMatch = c.hasWindowGeometry && frameWidth && frameHeight &&
                             static_cast<uint32_t>(crW) == frameWidth &&
                             static_cast<uint32_t>(crH) == frameHeight;
        if (crMatch) rec.contentRectWouldMatch = true;

        const uint64_t windowKey = (static_cast<uint64_t>(other->clientPid) << 32) | other->protocolId;
        const bool claimed = windowBindings_.count(windowKey) > 0 &&
                             windowBindings_[windowKey] != surfaceKey;
        const bool visible = topId && comp_.tmgr_.IsToplevelVisibleLocked(topId, rootId);

        const char* reason = nullptr;
        if (!frameWidth || !frameHeight) reason = "DEGENERATE_SIZE";
        else if (!windowLike) reason = "NO_WINDOW_ROLE";
        else if (topId == rootId) reason = "DESKTOP_ROOT";
        else if (!visible) reason = "NOT_VISIBLE";
        else if (st && (st->IsMinimized() || st->IsBackground())) reason = "MINIMIZED";
        else if (claimed) reason = "ALREADY_CLAIMED";
        else if ((sizeKnown && static_cast<uint32_t>(outerW) == frameWidth &&
                  static_cast<uint32_t>(outerH) == frameHeight) ||
                 (static_cast<uint32_t>(rawW) == frameWidth && static_cast<uint32_t>(rawH) == frameHeight))
            reason = nullptr;
        else reason = "OUTER_SIZE_MISMATCH";

        if (windowLike && !reason) eligible++;

        if (windowLike && bindDiagWindowLogged_.size() < 256)
        {
            char line[512];
            int n = snprintf(line, sizeof(line),
                     "BIND-WINDOW: windowKey=0x%llx clientPid=%u protocolId=%u toplevel=%u role=%s "
                     "parentTop=%u visible=%d minimized=%d claimedBy=0x%llx "
                     "rawSurface=%dx%d viewport=%dx%d outerWindow=%dx%d contentRect=(%d,%d %dx%d) offset=%d,%d",
                     (unsigned long long)windowKey, other->clientPid, other->protocolId, topId,
                     other->hasToplevel ? "toplevel" : (other->isSubsurface ? "subsurface" : "plain"),
                     topId, visible ? 1 : 0,
                     (st && (st->IsMinimized() || st->IsBackground())) ? 1 : 0,
                     (unsigned long long)(windowBindings_.count(windowKey) ? windowBindings_[windowKey] : 0),
                     rawW, rawH, vpW, vpH, outerW, outerH, crX, crY, crW, crH, crX, crY);
            if (n > 0 && BindDiagDedupInsert(bindDiagWindowLogged_, std::string(line)))
            {
                strncat(line, "\n", sizeof(line) - strlen(line) - 1);
                BindDiagEmit(line);
            }
        }

        if (reason && bindDiagRejectLogged_.size() < 256)
        {
            char line[512];
            int n = snprintf(line, sizeof(line),
                     "BIND-REJECT: producer=0x%llx hostPid=%u surfaceId=%u window=0x%llx reason=%s "
                     "producerExtent=%ux%u outer=%dx%d content=%dx%d viewport=%dx%d "
                     "contentRectWouldMatch=%d",
                     (unsigned long long)surfaceKey, presenterHostPid, presentSurfaceId,
                     (unsigned long long)windowKey, reason, frameWidth, frameHeight,
                     outerW, outerH, crW, crH, vpW, vpH, crMatch ? 1 : 0);
            if (n > 0 && BindDiagDedupInsert(bindDiagRejectLogged_, std::string(line)))
            {
                strncat(line, "\n", sizeof(line) - strlen(line) - 1);
                BindDiagEmit(line);
                snprintf(rec.lastReject, sizeof(rec.lastReject), "%s%s", reason,
                         crMatch ? "(contentRectWouldMatch)" : "");
            }
        }
    }

    if (!rec.bound)
    {
        if (eligible > 1) snprintf(rec.lastReject, sizeof(rec.lastReject), "MULTIPLE_CANDIDATES");
        else if (!eligible && !rec.lastReject[0]) snprintf(rec.lastReject, sizeof(rec.lastReject), "NO_CANDIDATE");
    }
}

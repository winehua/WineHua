#include "frame_pipeline.h"
#include "compositor_blit.h"
#include "compositor_constants.h"
#include "compositor/frame/direct_pass_policy.h"  // DirectPassPolicy 能力位 (任务 3)
#include "geometry.h"
#include "plugin_manager.h"  // GetRendererForToplevel (直传能力查询)
#include <algorithm>
#include <chrono>
#include <cstring>
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

using CompositorLayer = DesktopCompositor::CompositorLayer;
using DamageRect = DesktopCompositor::DamageRect;
using SubsurfaceLayer = DesktopCompositor::SubsurfaceLayer;

namespace {

// 帧合成耗时分段统计 ([GL-TAKE], 120 样本窗口): 直传/合成两个日志点共享;
// 分段语义不变。
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

uint64_t TakeElapsedUs(TakeClock::time_point begin, TakeClock::time_point end)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        end - begin).count());
}

// 黑边矩形填充 (全屏 fit 黑边 / 局部合成时裁剪到 R)
void FillBlackRect(std::vector<uint8_t>& composited, int rootW,
                   const DamageRect& dmg,
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
void UnionDamageRect(DamageRect& dmg, int rootW, int rootH,
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

FramePlanner::FramePlanner(DesktopCompositor& comp, bool frameTrace)
    : comp_(comp)
    , tmgr_(comp.tmgr_)
    , frameTrace_(frameTrace)
{
}

FramePlanOutcome FramePlanner::PlanDesktopLocked(uint32_t id,
                                                 TakeClock::time_point takeStarted,
                                                 TakeClock::time_point lockAcquired,
                                                 std::vector<uint8_t>& out,
                                                 PresentedFrame& frame,
                                                 FramePlan& plan) {
    ToplevelManager::ToplevelState* rst = nullptr;
    // 阶段 1: dirty 门控 + 无子窗口快进
    const FramePlanOutcome gate = GateDesktopDirtyLocked(id, plan, out, frame, rst);
    if (gate != FramePlanOutcome::kCompose) return gate;

    // 层序单一数据源 (阶段 1): 一帧全部内容来源的层列表, 构建一次,
    // fs-pick / 覆盖判定 / 签名 / 合成共用。zIndex: root < toplevel <
    // subsurface — 顺序与旧双循环等价 (见 CompositorLayer 注释)。
    plan.layers = comp_.BuildLayerListLocked(plan.rootW, plan.rootH);

    // 阶段 2: 全屏 pick/fit
    PlanFullscreenLocked(plan);
    // 阶段 3: fullscreenContentCovered 覆盖检测
    plan.fullscreenContentCovered = DetectFullscreenContentCoveredLocked(plan);
    // 阶段 4: SHM 全屏直传判定 (通过即完成本帧: out+frame 已填 + 直传日志)
    if (TryShmFullscreenDirectLocked(id, rst, takeStarted, lockAcquired,
                                     plan, out, frame))
        return FramePlanOutcome::kDirectPass;
    // 阶段 5: 合成签名 FNV 哈希 + rebuildBase 判定
    plan.compositionSignature = ComputeCompositionSignatureLocked(id, plan);
    const size_t rootBytes = static_cast<size_t>(plan.rootW) * plan.rootH * 4;
    plan.rebuildBase = !comp_.desktopOutputInitialized_ ||
        out.size() != rootBytes ||
        comp_.desktopOutputRootFrameSerial_ != comp_.desktopRootFrameSerial_ ||
        comp_.desktopCompositionSignature_ != plan.compositionSignature;
    // 阶段 6: 局部合成重绘矩形 R 计算 (空 R = 无内容变化, 本帧不产出)
    if (!ComputeDamageRectLocked(rst, plan)) return FramePlanOutcome::kNoFrame;
    // 阶段 7: 基底重建/局部基底拷贝
    CopyBaseToOutputLocked(rst, plan, out);
    plan.rootCopied = TakeClock::now();
    // 阶段 8: 锁内快照 (BlitSource/snapPool/ARGB opaque 融合扫描)
    SnapshotBlitSourcesLocked(rst, plan);
    plan.nZOrder = tmgr_.toplevelZOrder().size();
    plan.nSubLayers = comp_.subsurfaceLayers_.size();
    plan.snapshotDone = TakeClock::now();
    // 帧交付契约 (kCompose): 合成帧 — 桌面空间, buffer = 内容 = root 尺寸
    frame.kind = PresentedFrame::Kind::Composed;
    frame.baseSpace = PresentedFrame::BaseSpace::Desktop;
    frame.w = plan.rootW;
    frame.h = plan.rootH;
    frame.contentW = plan.rootW;
    frame.contentH = plan.rootH;
    frame.opaque = (rst->ShmFormat() != 0);
    return FramePlanOutcome::kCompose;
}

FramePlanOutcome FramePlanner::GateDesktopDirtyLocked(uint32_t id, FramePlan& plan,
                                                      std::vector<uint8_t>& out,
                                                      PresentedFrame& frame,
                                                      ToplevelManager::ToplevelState*& rst) {
    rst = tmgr_.FindToplevelLocked(id);
    if (!rst || !rst->HasFrame()) return FramePlanOutcome::kNoFrame;
    if (!rst->IsDirty()) return FramePlanOutcome::kNoFrame;

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
    if (!hasChildren && comp_.subsurfaceLayers_.empty()) {
        out = rst->Pixels();
        // 帧交付契约 (kFastPath): 无子窗口快进 = root 帧直通, 语义同合成帧
        frame.kind = PresentedFrame::Kind::Composed;
        frame.baseSpace = PresentedFrame::BaseSpace::Desktop;
        frame.w = plan.rootW;
        frame.h = plan.rootH;
        frame.contentW = plan.rootW;
        frame.contentH = plan.rootH;
        frame.opaque = (rst->ShmFormat() != 0);
        rst->ClearDirty();
        return FramePlanOutcome::kFastPath;
    }
    return FramePlanOutcome::kCompose;
}

void FramePlanner::PlanFullscreenLocked(FramePlan& plan) {
    // 全屏目标选取 (阶段 4, S3 收敛): 与输入侧 (FindInputTargetAt) 共用
    // PickFullscreenLayerLocked 单一实现 — 可见全屏窗口中取 fsPriority
    // 最大者 (多窗口可同时 fullscreen, 规则原因/局限见
    // ToplevelState::fsPriority 注释); fit 几何同样共用
    // ComputeFullscreenFitLocked (含内容尺寸选择, 见该函数注释)
    plan.fullscreenId = comp_.PickFullscreenLayerLocked(plan.layers);
    const ToplevelManager::ToplevelState* fsWin =
        plan.fullscreenId ? tmgr_.FindToplevelLocked(plan.fullscreenId) : nullptr;
    if (fsWin) {
        plan.fullscreenX = fsWin->X();
        plan.fullscreenY = fsWin->Y();
        plan.hasFullscreen = comp_.ComputeFullscreenFitLocked(plan.fullscreenId, plan.rootW,
                                                              plan.rootH, plan.transform);
        plan.isZcGame = comp_.HasZeroCopyLayerForToplevelLocked(plan.fullscreenId);
    }
}

bool FramePlanner::DetectFullscreenContentCoveredLocked(const FramePlan& plan) const {
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

bool FramePlanner::TryShmFullscreenDirectLocked(uint32_t id,
                                                ToplevelManager::ToplevelState* rst,
                                                TakeClock::time_point takeStarted,
                                                TakeClock::time_point lockAcquired,
                                                FramePlan& plan,
                                                std::vector<uint8_t>& out,
                                                PresentedFrame& frame) {
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
    // 直传前提能力检查 (任务 3, 行为平价): 直传帧与 CPU 合成逐像素一致的
    // 正确性前提是渲染器 GL 行为 (uForceOpaque/无 GL_BLEND/fit 同源/XRGB
    // 不透明) — 能力位收口于 direct_pass_policy.h, 由渲染器声明, 此处只
    // 查询。能力缺失 = 前提不成立 → 回退 CPU 合成。行为平价: 当前渲染器
    // 恒备全部能力 (EglRenderer::DirectPassCapabilities), 判定结果不变;
    // 渲染器不可查 (异常态) 时不拦截, 与旧行为一致。
    if (const auto* dpolicy = PluginManager::GetInstance()->GetRendererForToplevel(
            comp_.desktopRootToplevelId_)) {
        if ((dpolicy->DirectPassCapabilities() & winehua::kDirectPassCapabilitiesAll)
            != winehua::kDirectPassCapabilitiesAll) {
            return false;
        }
    }
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
                for (const auto& layer : plan.layers) {
                    if (layer.zIndex <= fsZ) continue;  // 全屏窗口及其下: 被盖住
                    if (layer.ShouldSkipCpu() ||
                        comp_.ShouldSkipFullscreenCascade(layer, plan.fullscreenId,
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
                    // 帧交付契约 (kDirectPass): 直传帧 buffer 是游戏内容尺寸
                    // (如 800x600), 但帧作为整屏桌面输出交付 — 输入逆映射锚
                    // 仍是桌面逻辑尺寸 (contentW/H = root), 与 buffer 尺寸
                    // 解耦 (红警2 直传点击修复的契约化); 直传门控已要求
                    // root XRGB → 帧按不透明呈现
                    frame.kind = PresentedFrame::Kind::DirectPass;
                    frame.baseSpace = PresentedFrame::BaseSpace::Desktop;
                    frame.w = directW;
                    frame.h = directH;
                    frame.contentW = plan.rootW;
                    frame.contentH = plan.rootH;
                    frame.opaque = (rst->ShmFormat() != 0);
                    rst->ClearDirty();
                    const auto directDone = TakeClock::now();
                    if (frameTrace_) {
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
                                    id, frame.w, frame.h, comp_.subsurfaceLayers_.size(),
                                    plan.hasFullscreen ? 1 : 0);
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

uint64_t FramePlanner::ComputeCompositionSignatureLocked(uint32_t id,
                                                         const FramePlan& plan) const {
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

bool FramePlanner::ComputeDamageRectLocked(ToplevelManager::ToplevelState* rst,
                                           FramePlan& plan) {
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
                        comp_.ShouldSkipFullscreenCascade(layer, plan.fullscreenId,
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
                const auto it = comp_.lastSubSerial_.find(sl.surfaceKey);
                if (it != comp_.lastSubSerial_.end() && it->second == sl.shmCommitSerial)
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
                const auto it = comp_.lastTopSerial_.find(layer.toplevelId);
                if (it != comp_.lastTopSerial_.end() && it->second == cst->FrameSerial())
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

void FramePlanner::CopyBaseToOutputLocked(const ToplevelManager::ToplevelState* rst,
                                          FramePlan& plan,
                                          std::vector<uint8_t>& out) {
    if (plan.rebuildBase) {
        out = rst->Pixels();
        comp_.desktopOutputInitialized_ = true;
        comp_.desktopOutputRootFrameSerial_ = comp_.desktopRootFrameSerial_;
        comp_.desktopCompositionSignature_ = plan.compositionSignature;
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

void FramePlanner::SnapshotBlitSourcesLocked(ToplevelManager::ToplevelState* rst,
                                             FramePlan& plan) {
    /*
     * 快照阶段 (持锁): 把 blit 要读的全部源像素拷成私有副本 (元数据已在
     * BlitSource / srcs 预计算), 随后立即解锁, blit 在锁外进行。
     */
    // 快照缓冲池: 跨帧复用容量, 避免每帧新建多 MB vector 的分配+缺页
    // 开销 (实测每帧全新分配让快照段从预期 ~2ms 涨到 35ms)。本函数仅
    // 渲染线程调用, 池无需加锁; 层数变化时 resize, 既有条目容量保留。
    comp_.snapPool_.resize(plan.layers.size());
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
            comp_.lastTopSerial_[layer.toplevelId] = cst->FrameSerial();  // 局部合成基准
            // ZC 游戏整幅填黑不读像素, 省下全屏拷贝 (pixels 留空)
            if (!(layer.toplevelId == plan.fullscreenId && plan.hasFullscreen && plan.isZcGame)) {
                auto& buf = comp_.snapPool_[li];
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
            comp_.lastSubSerial_[sl.surfaceKey] = sl.shmCommitSerial;  // 局部合成基准
            auto& buf = comp_.snapPool_[li];
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

FrameBlitter::FrameBlitter(bool frameTrace)
    : frameTrace_(frameTrace)
{
}

void FrameBlitter::Composite(uint32_t id,
                             TakeClock::time_point takeStarted,
                             TakeClock::time_point lockAcquired,
                             const FramePlan& plan,
                             std::vector<uint8_t>& out) {
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
    if (frameTrace_) {
        // 分段语义 (快照改造后): lockWait / 基底拷贝 / 快照(持锁) / blit(锁外) / 输出 / 总计
        TakeBreakdown().Add(TakeElapsedUs(takeStarted, lockAcquired),
                      TakeElapsedUs(lockAcquired, plan.rootCopied),
                      TakeElapsedUs(plan.rootCopied, plan.snapshotDone),
                      TakeElapsedUs(plan.snapshotDone, childrenComposited),
                      TakeElapsedUs(childrenComposited, outputMoved),
                      TakeElapsedUs(takeStarted, outputMoved));
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] root #%{public}u %{public}dx%{public}d children=%{public}zu subsurfaces=%{public}zu mode=%{public}s fs=%{public}d dmg=(%{public}d,%{public}d %{public}dx%{public}d)",
                    id, plan.rootW, plan.rootH, plan.nZOrder, plan.nSubLayers, plan.dmg.full ? "full" : "partial",
                    plan.hasFullscreen ? 1 : 0, plan.dmg.x, plan.dmg.y, plan.dmg.w, plan.dmg.h);
    }
}

void FrameBlitter::BlitToplevel(const FramePlan& plan, const CompositorLayer& layer,
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

void FrameBlitter::BlitSubsurface(const FramePlan& plan, const CompositorLayer& layer,
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

void FrameBlitter::BlitWindowSubsurface(const CompositorLayer& layer, int winW, int winH,
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

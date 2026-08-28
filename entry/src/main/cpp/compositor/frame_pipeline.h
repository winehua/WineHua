#pragma once
#include <chrono>
#include <cstdint>
#include <vector>

#include "blit_clip.h"
#include "desktop_compositor.h"
#include "geometry.h"
#include "toplevel_manager.h"

// 帧合成管线 (重构第 2A 步: TakeToplevelFrame 纯结构拆分, 行为平价)。
// 桌面合成按"锁内规划 / 锁外绘制"分两阶段:
// - FramePlanner: tmgr 锁内运行, 按原 TakeToplevelFrame 内联段顺序执行
//   (dirty 门控 → 全屏仲裁 → 覆盖检测 → 直传判定 → 签名/基底 → damage R →
//   基底落盘 → 快照), 产出 FramePlan。经 friend 访问 DesktopCompositor 的
//   层容器/快照池/合成状态 — 状态仍由 DesktopCompositor 持有, 读写线程域
//   不变 (渲染线程 + tmgr 锁内)。
// - FrameBlitter: 锁外纯像素 (不碰 tmgr 锁), 消费 FramePlan 在调用方提供
//   的 out 上增量合成 (rebuildBase/局部基底拷贝已在规划段落盘)。
// 锁边界与原单函数逐段对应 (原 lk.unlock() 前的代码全在 Planner, 之后的
// 全在 Blitter)。

using TakeClock = std::chrono::steady_clock;

/*
 * 层间快照结构 (BlitSource): 把 blit 要读的全部源像素/元数据拷成
 * 私有副本, 随后立即解锁, blit 在锁外进行。动机 (实测): 旧实现持锁
 * 完成整帧 CPU blit (1400x920 全屏合成 ~25ms), wl 事件循环线程的
 * commit 与输入派发同抢 tmgr_ 锁 — commit 实测平均被堵 27ms
 * (p95 ~90ms), 输入注入 NAPI→INJ 中位 8ms; 快照仅 ~1-3ms memcpy,
 * 锁占用 ↓10 倍。正确性: 快照后 wl 线程的新 commit 只影响下一帧
 * (dirty 重新置位), 与本帧 blit 无共享指针; layer.sub /
 * ToplevelState 指针解锁后失效, 故所需字段全部拷入 BlitSource。
 */
struct BlitSource {
    const std::vector<uint8_t>* pixels = nullptr;  // 指向 snapPool_ 条目 (ZC 层为空)
    int w = 0, h = 0;        // 源像素尺寸 (toplevel: Width/Height; sub: sl.w/h)
    int x = 0, y = 0;        // toplevel 屏幕位置 (cst->X/Y)
    uint32_t shmFormat = 1;  // 0=ARGB8888 1=XRGB8888
    bool opaque = false;     // sub: 不透明标记
    int vpDstW = 0, vpDstH = 0;           // sub: viewport 目标尺寸
    int dmgX = 0, dmgY = 0, dmgW = 0, dmgH = 0;  // sub: damage 矩形
    bool skip = false;       // 预计算: 本帧不参与合成/快照 (见 Planner 赋值注释)
};

// 一帧桌面合成的规划产物: FramePlanner (锁内) 填, FrameBlitter (锁外) 消费。
// layers 的 sub 指针解锁后失效 — Blitter 只读各层值字段 (type/toplevelId/
// x/y/w/h), 不得解引用 sub (所需字段已全部拷入 srcs)。
struct FramePlan {
    std::vector<DesktopCompositor::CompositorLayer> layers;  // 层列表 (锁内构建)
    std::vector<BlitSource> srcs;         // 与 layers 等长 (锁内快照)
    int rootW = 0, rootH = 0;
    uint32_t fullscreenId = 0;
    bool hasFullscreen = false;
    // ZC 游戏 (画面在 zero-copy GL 层): 全屏独占输出, 见 Blitter 填黑分支
    bool isZcGame = false;
    int fullscreenX = 0, fullscreenY = 0;
    FitRect transform;
    bool fullscreenContentCovered = false;
    // -- 规划段内部中间态 (Blitter 不读) --
    uint64_t compositionSignature = 0;
    bool rebuildBase = false;
    // -- 本帧重绘矩形 (局部合成范围) --
    DesktopCompositor::DamageRect dmg;
    // -- 日志计数 + 分段计时点 (锁内捕获, 锁外 [GL-TAKE]/[MW-TAKE] 用) --
    size_t nZOrder = 0;
    size_t nSubLayers = 0;
    TakeClock::time_point rootCopied;
    TakeClock::time_point snapshotDone;
};

// 规划结果: kNoFrame=无新帧 (TakeToplevelFrame 返回 false); kFastPath=无子
// 窗口快进 (out/w/h 已填, 返回 true); kDirectPass=SHM 全屏直传 (out/w/h 已
// 填+日志, 返回 true); kCompose=需锁外合成 (plan 已备)。
enum class FramePlanOutcome { kNoFrame, kFastPath, kDirectPass, kCompose };

// 锁内规划器: 全部方法在 tmgr 锁内运行 (调用方持锁)。状态经 comp_ friend
// 访问; tmgr_ 为 comp_.tmgr_ 别名。
class FramePlanner {
public:
    // frameTrace 为编排者捕获的帧级诊断门控 (perf_utils.h FrameTraceEnabled)
    FramePlanner(DesktopCompositor& comp, bool frameTrace);

    // 锁内规划主流程 (阶段顺序与原 TakeToplevelFrame 内联段一致)
    FramePlanOutcome PlanDesktopLocked(uint32_t id,
                                       TakeClock::time_point takeStarted,
                                       TakeClock::time_point lockAcquired,
                                       std::vector<uint8_t>& out, int& w, int& h,
                                       FramePlan& plan);

private:
    // 阶段 1: dirty 门控 + 无子窗口快进
    FramePlanOutcome GateDesktopDirtyLocked(uint32_t id, FramePlan& plan,
                                            std::vector<uint8_t>& out, int& w, int& h,
                                            ToplevelManager::ToplevelState*& rst);
    // 阶段 2: 全屏 pick/fit
    void PlanFullscreenLocked(FramePlan& plan);
    // 阶段 3: fullscreenContentCovered 覆盖检测
    bool DetectFullscreenContentCoveredLocked(const FramePlan& plan) const;
    // 阶段 4: SHM 全屏直传判定 (通过即填好 out/w/h 并打直传日志, 返回 true)
    bool TryShmFullscreenDirectLocked(uint32_t id, ToplevelManager::ToplevelState* rst,
                                      TakeClock::time_point takeStarted,
                                      TakeClock::time_point lockAcquired,
                                      FramePlan& plan,
                                      std::vector<uint8_t>& out, int& w, int& h);
    // 阶段 5: 合成签名 FNV 哈希 (rebuildBase 判定在 PlanDesktopLocked)
    uint64_t ComputeCompositionSignatureLocked(uint32_t id, const FramePlan& plan) const;
    // 阶段 6: 局部合成重绘矩形 R 计算 (含 BlitSource skip 预计算);
    // 返回 false = R 为空 (无内容变化, 本帧不产出, 已 ClearDirty)
    bool ComputeDamageRectLocked(ToplevelManager::ToplevelState* rst, FramePlan& plan);
    // 阶段 7: 基底重建/局部基底拷贝
    void CopyBaseToOutputLocked(const ToplevelManager::ToplevelState* rst,
                                FramePlan& plan, std::vector<uint8_t>& out);
    // 阶段 8: 锁内快照 (BlitSource/snapPool/ARGB opaque 融合扫描) + ClearDirty
    void SnapshotBlitSourcesLocked(ToplevelManager::ToplevelState* rst, FramePlan& plan);

    DesktopCompositor& comp_;
    ToplevelManager& tmgr_;
    bool frameTrace_;
};

// 锁外绘制器: 纯像素, 不碰 tmgr 锁。
class FrameBlitter {
public:
    explicit FrameBlitter(bool frameTrace);

    // 合成主循环 + 每帧 [GL-TAKE]/[MW-TAKE] 日志 (frameTrace 门控);
    // 只读 plan 快照, 在 out 上增量叠加
    void Composite(uint32_t id,
                   TakeClock::time_point takeStarted,
                   TakeClock::time_point lockAcquired,
                   const FramePlan& plan,
                   std::vector<uint8_t>& out, int& w, int& h);

    // PC 模式窗口内 subsurface blit (纯像素): 直接读层像素 (无快照 — PC
    // 路径调用方全程持 tmgr 锁, 与原实现一致), 本函数自身不碰锁。
    static void BlitWindowSubsurface(const DesktopCompositor::CompositorLayer& layer,
                                     int winW, int winH, std::vector<uint8_t>& out);

private:
    static void BlitToplevel(const FramePlan& plan,
                             const DesktopCompositor::CompositorLayer& layer,
                             const BlitSource& bs, std::vector<uint8_t>& composited);
    static void BlitSubsurface(const FramePlan& plan,
                               const DesktopCompositor::CompositorLayer& layer,
                               const BlitSource& bs, std::vector<uint8_t>& composited);

    bool frameTrace_;
};

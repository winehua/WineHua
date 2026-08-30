#include "frame_composer.h"

#include "desktop_compositor.h"
#include "frame_pipeline.h"
#include "common/perf_utils.h"
#include "toplevel_manager.h"

#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Server"

// ============================================================================
// DesktopRootFrameComposer: Desktop root 帧整屏合成
// (原 DesktopCompositor::TakeToplevelFrame 的 desktop 分支, 逐行等价)
//
// 阶段拆分 (重构第 2A 步, 纯结构拆分行为平价): 桌面分支的"锁内规划 / 锁外
// 绘制"两阶段在 frame_pipeline.{h,cpp} — FramePlanner 锁内按原内联段顺序
// 执行 (dirty 门控 → 全屏仲裁 → 覆盖检测 → 直传判定 → 签名/基底 → damage R
// → 基底落盘 → 快照) 产出 FramePlan, FrameBlitter 锁外纯像素消费。锁边界/
// 计时点/日志门控与原单函数实现逐段对应。
// ============================================================================
bool DesktopRootFrameComposer::Compose(uint32_t id, std::vector<uint8_t>& out,
                                       PresentedFrame& frame, bool frameTrace) {
    const auto takeStarted = TakeClock::now();
    auto lk = comp_.tmgr_.Lock();
    const auto lockAcquired = TakeClock::now();

    FramePlan plan;
    FramePlanner planner(comp_, frameTrace);
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

// ============================================================================
// WindowFrameComposer: PC 单窗口帧
// (原 DesktopCompositor::TakeWindowFrameLocked, 逐行等价)
//
// 窗口内层序 (阶段 3, PC 模式): Root(窗口帧) < Subsurface(窗口局部坐标) <
// ZC 层(最顶)。窗口间层序由系统合成器保证, 不在此合成。PC 模式 subsurface
// 当前恒空 (全部转 popup 伪 toplevel), 合成输出 = 窗口 SHM 帧; ZC 层
// (zcActive) 合成跳过 — GPU 内容由 renderer 自绘覆盖, CPU 帧保留 SHM 内容
// 不抠除 (与 desktop 模式同语义: GPU 帧不透明时覆盖等价, fallback 窗口期
// 显示旧内容比黑屏稳)。
// ============================================================================
bool WindowFrameComposer::Compose(uint32_t id, std::vector<uint8_t>& out,
                                  PresentedFrame& frame, bool frameTrace) {
    auto* st = comp_.tmgr_.FindToplevelLocked(id);
    if (!st || !st->IsDirty()) return false;
    const int winW = st->Width();
    const int winH = st->Height();
    if (winW <= 0 || winH <= 0) return false;

    const auto layers = comp_.BuildWindowLayerListLocked(id, winW, winH);
    out = st->Pixels();
    for (const auto& layer : layers) {
        switch (layer.type) {
            case DesktopCompositor::CompositorLayer::Type::Root:
                break;  // 基底已在 out = st->pixels 拷贝
            case DesktopCompositor::CompositorLayer::Type::Toplevel:
                break;  // 窗口内 ZC 整窗口层: GPU 自绘, CPU 帧跳过
            case DesktopCompositor::CompositorLayer::Type::Subsurface:
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

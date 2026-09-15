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
// 窗口内层序 (阶段 3, 多窗口模式 — PC 窗口模式与 Pad 多窗口模式共用, 两者
// DisplayPolicy::desktop 均为 false): Root(窗口帧) < 内嵌客户区 Subsurface
// (窗口局部坐标, route=InlineClient) < ZC 层(最顶)。窗口间层序由系统合成器
// 保证, 不在此合成。Popup 类 subsurface (菜单/越界浮层) 不在本容器 — 走
// 伪 toplevel + 独立 OHOS 子窗口, 见 DisplayPolicy::RouteForSubsurface。
// ZC 层 (zcActive) 合成跳过 — GPU 内容由 renderer 自绘覆盖, CPU 帧保留 SHM
// 内容不抠除 (与 desktop 模式同语义: GPU 帧不透明时覆盖等价, fallback
// 窗口期显示旧内容比黑屏稳)。
// ============================================================================
bool WindowFrameComposer::Compose(uint32_t id, std::vector<uint8_t>& out,
                                  PresentedFrame& frame, bool frameTrace) {
    // 全程持 tmgr 锁 (RAII, 出函数解锁): 本路径直接消费共享层 — layer.sub
    // 指向 subsurfaceLayers_ 元素 (BuildWindowLayerListLocked), blit 会就地
    // 写其 opaque/opaqueCheckedSerial 缓存, st/st->Pixels()/ClearDirty 亦为
    // 锁内契约 (见 CompositorLayer 注释)。无锁时与 WL_Server 线程的
    // UpsertSubsurfaceLayer (持锁; 首次 push_back 扩容 / 更新 move 换 pixels
    // 缓冲) 并发 → layer.sub->pixels 悬垂 → 渲染线程 SIGSEGV (Pad 实测:
    // 内嵌客户区 smoke 跑 ~25s 必崩, cppcrash tid=渲染线程)。按类分流前
    // 窗口内层列表恒空 (PC 模式 subsurface 全转 popup), 该缺陷未暴露。
    // 注: desktop 路径的"锁内规划/锁外绘制"之所以安全, 是因为 FramePlan 已
    // 快照像素; 本路径无快照阶段, 故 blit 必须留在锁内 (窗口内层数据量小)。
    auto lk = comp_.tmgr_.Lock();
    auto* st = comp_.tmgr_.FindToplevelLocked(id);
    if (!st || !st->IsDirty()) return false;
    const int winW = st->Width();
    const int winH = st->Height();
    if (winW <= 0 || winH <= 0) return false;

    const auto layers = comp_.BuildWindowLayerListLocked(id, winW, winH);
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
    lk.unlock();  // ── 锁到此为止, 以下日志/返回不持锁 ──
    if (frameTrace) {
        OH_LOG_INFO(LOG_APP, "[MW-TAKE] toplevel #%{public}u frame %{public}dx%{public}d px=%{public}zu",
                    id, frame.w, frame.h, out.size());
    }
    return true;
}

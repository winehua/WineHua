#pragma once

#include <cstdint>

// ============================================================================
// direct_pass_policy: SHM 全屏直传能力位 (重构第 2B 步任务 3, 行为平价)
//
// TryShmFullscreenDirectLocked (frame_pipeline.cpp) 判定"直传帧与 CPU 合成
// 逐像素一致"的正确性前提是渲染器 GL 行为 — 此前这些前提只存在于注释假设
// (uForceOpaque 强制不透明 / 无 GL_BLEND / fit 与 CPU 同源), 渲染器行为
// 漂移时无任何检测。能力位把这些前提收口为渲染器声明的接口: 合成侧只
// 查询能力, 不再靠注释假设。
//
// 行为平价: 渲染器当前实现恒备全部能力 (EglRenderer::DirectPassCapabilities
// 返回 kDirectPassCapabilitiesAll, 能力来源见其实现注释), 查询恒通过 →
// 直传判定结果与旧实现逐点一致。
// ============================================================================

namespace winehua {

// 直传正确性前提能力 (渲染器 GL 行为):
enum class DirectPassCapabilities : uint32_t {
    kForceOpaqueNoBlend = 1u << 0,  // 渲染器无 GL_BLEND + uForceOpaque 强制不透明
                                    // (egl_renderer.cpp:874/:924): ARGB 源直传时
                                    // alpha=255 区域与 CPU 混合分支逐像素一致,
                                    // alpha=0 区域 CPU 保留黑底 (黑屏容忍见
                                    // TryShmFullscreenDirectLocked 注释)
    kFitSameAsCpu = 1u << 1,        // 渲染器保比例 fit 与 CPU ComputeFitRect 同源
                                    // (egl_renderer.cpp:819-821, 几何统一): 直传帧
                                    // 与 CPU 合成输出几何逐像素一致
    kXrgbFrameOpaque = 1u << 2,     // root XRGB 帧不透明呈现 (frameArgb_=false →
                                    // GPU 黑边不透明): 直传帧整屏覆盖有效
};

inline constexpr uint32_t kDirectPassCapabilitiesAll =
    static_cast<uint32_t>(DirectPassCapabilities::kForceOpaqueNoBlend) |
    static_cast<uint32_t>(DirectPassCapabilities::kFitSameAsCpu) |
    static_cast<uint32_t>(DirectPassCapabilities::kXrgbFrameOpaque);

// 直传能力声明接口: 由渲染器 (EglRenderer) 实现, 合成侧经查询决定直传
// 是否触发; 能力缺失 = 直传前提不成立, 回退 CPU 合成。
class DirectPassPolicy {
public:
    virtual ~DirectPassPolicy() = default;
    virtual uint32_t DirectPassCapabilities() const = 0;
};

} // namespace winehua

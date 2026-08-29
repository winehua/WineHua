#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace winehua {

using PerfClock = std::chrono::steady_clock;

uint64_t PerfNowUs();

// 帧级诊断日志的统一运行时门控 (默认关闭): [GL-TAKE] / [WL-T] / [MW-TAKE] /
// [DBG-CPU] / [MW-SWAP] / [VENUS-ORDER][MAIN] 等每帧/常驻诊断的输出与统计
// 累加全部挂此开关, 关闭时零开销。
// 开启方式:
// - WINEHUA_FRAME_TRACE=1 — 进程启动环境, 缓存一次读取结果 (避免每帧 getenv)
// - VKR_WINEHUA_SHADOW_TRACE=1 / =present-image-trace — 兼容已有取值语义;
//   该 env 由 SetHostShadowProfile 运行期 setenv (napi_init.cpp), 不缓存,
//   保持原有动态生效行为
inline bool FrameTraceEnabled() {
    static const bool kWinehuaFrameTrace = [] {
        const char* v = std::getenv("WINEHUA_FRAME_TRACE");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    if (kWinehuaFrameTrace) return true;
    const char* trace = std::getenv("VKR_WINEHUA_SHADOW_TRACE");
    return trace && (!std::strcmp(trace, "1") || !std::strcmp(trace, "present-image-trace"));
}

struct RendererPerfWindow {
    static constexpr size_t kSamples = 120;

    std::array<uint64_t, kSamples> takeUs{};
    std::array<uint64_t, kSamples> uploadUs{};
    std::array<uint64_t, kSamples> swapUs{};
    std::array<uint64_t, kSamples> totalUs{};
    size_t count = 0;
    uint64_t displayed = 0;
    uint64_t windowDisplayed = 0;
    uint64_t failedSwaps = 0;
    uint64_t uploadBytes = 0;
    uint64_t startedUs = PerfNowUs();
    uint64_t publishStartedUs = startedUs;
    uint64_t publishFrames = 0;
    uint64_t publishSequence = 0;

    void PublishDisplayedFps(uint32_t toplevelId, uint64_t nowUs);

    static uint64_t Percentile(std::array<uint64_t, kSamples> values, size_t count,
                               unsigned int percentile);

    void Add(uint32_t toplevelId, uint64_t take, uint64_t upload, uint64_t swap,
             uint64_t total, size_t bytes, bool swapOk);
};

} // namespace winehua

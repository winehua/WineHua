#ifndef WINEHUA_AUDIO_PCM_METRICS_H
#define WINEHUA_AUDIO_PCM_METRICS_H

#include <atomic>
#include <cstdint>

namespace winehua {

constexpr int32_t kNearFullScaleThreshold = 32000;
constexpr uint32_t kClickDelta16 = 16384;
constexpr uint32_t kClickDelta32 = 32768;
constexpr uint32_t kUnityGainQ15 = 32768;
constexpr uint32_t kHalfGainQ15 = 16384;

enum class AudioDiagGainProfile : uint32_t
{
    G00 = 0,
    G10 = 1,
    G01 = 2,
    G11 = 3,
};

struct PcmContinuityState
{
    bool havePrevious = false;
    int32_t previousL = 0;
    int32_t previousR = 0;
    uint32_t currentFullScaleRunL = 0;
    uint32_t currentFullScaleRunR = 0;
    uint64_t seenGeneration = 0;
};

struct PcmBlockMetrics
{
    uint32_t frames = 0;

    uint32_t peakL = 0;
    uint32_t peakR = 0;

    uint64_t sumSquaresL = 0;
    uint64_t sumSquaresR = 0;
    int64_t sumL = 0;
    int64_t sumR = 0;

    uint32_t fullScaleSamplesL = 0;
    uint32_t fullScaleSamplesR = 0;
    uint32_t nearFullScaleSamplesL = 0;
    uint32_t nearFullScaleSamplesR = 0;

    uint32_t maxFullScaleRunL = 0;
    uint32_t maxFullScaleRunR = 0;

    uint32_t maxDeltaL = 0;
    uint32_t maxDeltaR = 0;
    uint32_t boundaryDeltaL = 0;
    uint32_t boundaryDeltaR = 0;
    uint32_t click16L = 0;
    uint32_t click16R = 0;
    uint32_t click32L = 0;
    uint32_t click32R = 0;

    int32_t firstL = 0;
    int32_t firstR = 0;
    int32_t lastL = 0;
    int32_t lastR = 0;
    bool hasFrames = false;
};

struct PcmMetricSnapshot
{
    uint64_t frames = 0;
    uint32_t peakL = 0;
    uint32_t peakR = 0;
    uint64_t sumSquaresL = 0;
    uint64_t sumSquaresR = 0;
    int64_t sumL = 0;
    int64_t sumR = 0;
    uint32_t fullScaleSamplesL = 0;
    uint32_t fullScaleSamplesR = 0;
    uint32_t nearFullScaleSamplesL = 0;
    uint32_t nearFullScaleSamplesR = 0;
    uint32_t maxFullScaleRunL = 0;
    uint32_t maxFullScaleRunR = 0;
    uint32_t maxDeltaL = 0;
    uint32_t maxDeltaR = 0;
    uint32_t maxBoundaryDeltaL = 0;
    uint32_t maxBoundaryDeltaR = 0;
    uint32_t click16L = 0;
    uint32_t click16R = 0;
    uint32_t click32L = 0;
    uint32_t click32R = 0;
    uint32_t validCallbacks = 0;
    uint32_t invalidCallbacks = 0;
};

struct PcmMetricAccumulators
{
    std::atomic<uint64_t> frames{0};
    std::atomic<uint32_t> peakL{0};
    std::atomic<uint32_t> peakR{0};
    std::atomic<uint64_t> sumSquaresL{0};
    std::atomic<uint64_t> sumSquaresR{0};
    std::atomic<int64_t> sumL{0};
    std::atomic<int64_t> sumR{0};
    std::atomic<uint32_t> fullScaleSamplesL{0};
    std::atomic<uint32_t> fullScaleSamplesR{0};
    std::atomic<uint32_t> nearFullScaleSamplesL{0};
    std::atomic<uint32_t> nearFullScaleSamplesR{0};
    std::atomic<uint32_t> maxFullScaleRunL{0};
    std::atomic<uint32_t> maxFullScaleRunR{0};
    std::atomic<uint32_t> maxDeltaL{0};
    std::atomic<uint32_t> maxDeltaR{0};
    std::atomic<uint32_t> maxBoundaryDeltaL{0};
    std::atomic<uint32_t> maxBoundaryDeltaR{0};
    std::atomic<uint32_t> click16L{0};
    std::atomic<uint32_t> click16R{0};
    std::atomic<uint32_t> click32L{0};
    std::atomic<uint32_t> click32R{0};
    std::atomic<uint32_t> validCallbacks{0};
    std::atomic<uint32_t> invalidCallbacks{0};

    void Add(const PcmBlockMetrics& metrics);
    PcmMetricSnapshot ExchangeReset();
};

uint32_t AbsI32(int32_t value);
bool IsFullScaleS16(int32_t value);
uint32_t RmsFromSumSquares(uint64_t sumSquares, uint64_t frames);
int32_t DcFromSum(int64_t sum, uint64_t frames);

PcmBlockMetrics AnalyzeStereoS16(const int16_t* pcm,
                                  uint32_t frames,
                                  PcmContinuityState& continuity,
                                  uint64_t generation);

int16_t ApplyGainQ15(int16_t sample, uint32_t gainQ15);
void ApplyStereoGainQ15(int16_t* pcm, uint32_t frames, uint32_t gainQ15);

uint32_t GainQ15ForOrdinal(AudioDiagGainProfile profile, uint32_t ordinal);
const char* AudioDiagGainProfileName(AudioDiagGainProfile profile);

template <typename T>
void AtomicMax(std::atomic<T>& target, T value)
{
    T current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed))
    {
    }
}

} // namespace winehua

#endif

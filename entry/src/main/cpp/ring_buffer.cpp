#include "ring_buffer.h"

#include <algorithm>
#include <limits>

namespace winehua {

uint32_t AlignUpPowerOfTwo(uint32_t value, uint32_t minimum)
{
    uint32_t aligned = std::max(minimum, 1u);

    while (aligned < value && aligned <= (std::numeric_limits<uint32_t>::max() >> 1))
        aligned <<= 1;
    return aligned < value ? value : aligned;
}

uint32_t ComputeRingCapacityFrames(uint32_t callbackFrames)
{
    uint32_t normalized = callbackFrames ? callbackFrames : kAudioTargetCallbackFrames;
    uint32_t requested = normalized * kAudioRingDepthMultiplier;
    return AlignUpPowerOfTwo(requested, kAudioMinimumRingFrames);
}

uint32_t MixFramesToClientFramesFloor(uint32_t mixFrames, uint32_t clientRate)
{
    if (!mixFrames || !clientRate) return 0;
    if (clientRate == kAudioMixSampleRate) return mixFrames;
    return static_cast<uint32_t>((static_cast<uint64_t>(mixFrames) * clientRate) / kAudioMixSampleRate);
}

uint32_t MixFramesToClientFramesCeil(uint32_t mixFrames, uint32_t clientRate)
{
    if (!mixFrames || !clientRate) return 0;
    if (clientRate == kAudioMixSampleRate) return mixFrames;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(mixFrames) * clientRate + kAudioMixSampleRate - 1) / kAudioMixSampleRate);
}

uint32_t ClientFramesToMixFramesCeil(uint32_t clientFrames, uint32_t clientRate)
{
    if (!clientFrames || !clientRate) return 0;
    if (clientRate == kAudioMixSampleRate) return clientFrames;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(clientFrames) * kAudioMixSampleRate + clientRate - 1) / clientRate);
}

} // namespace winehua

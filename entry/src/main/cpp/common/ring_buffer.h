#ifndef WINEHUA_RING_BUFFER_H
#define WINEHUA_RING_BUFFER_H

#include <stdint.h>

namespace winehua {

constexpr uint32_t kAudioMixSampleRate = 48000;
constexpr uint32_t kAudioMixChannels = 2;
constexpr uint32_t kAudioMixFrameSize = 4;
constexpr uint32_t kAudioMinimumRingFrames = 4096;
constexpr uint32_t kAudioTargetCallbackFrames = 480;
constexpr uint32_t kAudioRingDepthMultiplier = 8;
constexpr uint32_t kAudioMaxCallbackFrames = 4096;

uint32_t AlignUpPowerOfTwo(uint32_t value, uint32_t minimum);
uint32_t ComputeRingCapacityFrames(uint32_t callbackFrames);
uint32_t MixFramesToClientFramesFloor(uint32_t mixFrames, uint32_t clientRate);
uint32_t MixFramesToClientFramesCeil(uint32_t mixFrames, uint32_t clientRate);
uint32_t ClientFramesToMixFramesCeil(uint32_t clientFrames, uint32_t clientRate);

} // namespace winehua

#endif

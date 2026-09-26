#pragma once

#include <cstdint>

namespace winehua::direct {

constexpr int32_t kSurfaceProbeVersion = 1;
constexpr uint32_t kSurfaceProbeProduce = 1;
constexpr uint32_t kSurfaceProbeFinish = 2;
constexpr int32_t kSurfaceProbeFrameCount = 6;

struct SurfaceProbeFrame {
    int32_t status = -1;
    int32_t stage = 0;
    int32_t pid = -1;
    int32_t frame = -1;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t bufferSeq = 0;
};

enum SurfaceProbeStage : int32_t {
    kSurfaceComplete = 0,
    kSurfaceReadWindow = 1,
    kSurfaceConfigure = 2,
    kSurfaceRequestBuffer = 3,
    kSurfaceRequestFence = 4,
    kSurfaceNativeBuffer = 5,
    kSurfaceBufferConfig = 6,
    kSurfaceMap = 7,
    kSurfaceUnmap = 8,
    kSurfaceFlush = 9,
};

} // namespace winehua::direct

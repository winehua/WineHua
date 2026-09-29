#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include "direct_viewport.h"

// Immutable SHM snapshots only. A Direct layer contains identity/geometry;
// its GPU image stays in BufferQueue and is never copied into this structure.
struct GpuDesktopLayer {
    uint64_t key = 0, serial = 0;
    uint32_t directToplevel = 0;
    int x = 0, y = 0, w = 0, h = 0;
    int sourceW = 0, sourceH = 0;
    DirectImageSampling sampling;
    bool opaque = true;
    bool solidBlack = false;
    std::shared_ptr<const std::vector<uint8_t>> pixels;
};

struct GpuDesktopScene {
    uint32_t rootId = 0;
    int width = 0, height = 0;
    std::vector<GpuDesktopLayer> layers;
};

struct GpuDesktopDirectSource {
    uint32_t pid = 0, toplevel = 0, wlSurface = 0;
    int width = 0, height = 0;
};

using GpuDesktopSnapshotCache = std::unordered_map<uint64_t, GpuDesktopLayer>;

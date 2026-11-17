#pragma once

#include <cstdint>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>
#include "direct_viewport.h"
#include "zc_bridge.h"

// Immutable SHM snapshots only. A Direct layer contains identity/geometry;
// its GPU image stays in BufferQueue and is never copied into this structure.
struct GpuDesktopLayer {
    uint64_t key = 0, serial = 0;
    uint64_t zeroCopyKey = 0, ownerSurfaceKey = 0;
    uint32_t parentToplevel = 0;
    bool subsurface = false, external = false;
    uint64_t directSurfaceKey = 0;
    int x = 0, y = 0, w = 0, h = 0;
    int sourceW = 0, sourceH = 0;
    DirectImageSampling sampling;
    bool opaque = true;
    bool solidBlack = false;
    std::shared_ptr<const std::vector<uint8_t>> pixels;
};

struct GpuDesktopScene {
    // Sample identity only; deliberately excluded from SameGpuDesktopScene.
    uint64_t diagnosticSerial = 0, diagnosticUs = 0;
    uint32_t rootId = 0;
    int width = 0, height = 0;
    std::vector<GpuDesktopLayer> layers;
};

// Native producers present completed Wine window drawables, not Wayland
// premultiplied UI pixels. Wine Vulkan advertises OPAQUE and upstream WGL
// requests EGL_PRESENT_OPAQUE_EXT. The OHOS pbuffer bridge preserves the same
// contract even when game draws leave fractional/zero framebuffer alpha.
// Layered/shaped SHM windows have their own opacity flag and bypass this path.
inline GpuDesktopLayer MakeNativeWindowLayer(uint64_t key, const ZeroCopyLayerInfo& info,
                                            int sourceW, int sourceH)
{
    GpuDesktopLayer source;
    source.zeroCopyKey = key;
    source.ownerSurfaceKey = (static_cast<uint64_t>(info.clientPid) << 32) | info.surfaceId;
    source.parentToplevel = info.parentToplevel;
    source.subsurface = info.subsurface;
    source.external = info.external;
    source.x = info.x; source.y = info.y;
    source.w = info.width; source.h = info.height;
    source.sourceW = sourceW; source.sourceH = sourceH;
    source.opaque = true;
    return source;
}

inline void SetNativeProducerSampling(GpuDesktopLayer& layer, bool vulkan)
{
    layer.sampling.v = vulkan ? std::array<float, 4>{0, 0, 1, 0} :
                               std::array<float, 4>{1, 0, -1, 0};
}

struct GpuDesktopDirectSource {
    uint32_t pid = 0, toplevel = 0, wlSurface = 0;
    int width = 0, height = 0;
    uint64_t generation = 0;
};

using GpuDesktopSnapshotCache = std::unordered_map<uint64_t, GpuDesktopLayer>;

// Preserve the compositor's SHM layer order. Native child surfaces replace
// their exact SHM slot; native windows draw after their window frame. A child
// with no SHM commit draws inside its owner's lane, below software menus.
inline void MergeZeroCopySceneLayers(GpuDesktopScene& scene,
                                    const std::vector<GpuDesktopLayer>& sources)
{
    auto ordered = sources;
    // A full native window must be beneath its native child, even when the
    // child was discovered before the parent in the present-query results.
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.subsurface < b.subsurface;
    });
    for (const auto& source : ordered) {
        auto exact = scene.layers.end();
        auto owner = scene.layers.end();
        for (auto it = scene.layers.begin(); it != scene.layers.end(); ++it) {
            if (!it->subsurface && it->parentToplevel == source.parentToplevel)
                owner = it;
            if (source.subsurface && it->subsurface &&
                it->ownerSurfaceKey == source.ownerSurfaceKey) exact = it;
        }
        if (exact != scene.layers.end()) *exact = source;
        else if (source.external) scene.layers.push_back(source);
        else if (owner != scene.layers.end()) {
            auto position = owner + 1;
            while (position != scene.layers.end() && position->zeroCopyKey &&
                   position->subsurface && !position->external &&
                   position->parentToplevel == source.parentToplevel) ++position;
            scene.layers.insert(position, source);
        }
    }
    for (auto it = scene.layers.begin(); it != scene.layers.end();)
        if (!it->zeroCopyKey && !it->pixels && !it->solidBlack && !it->directSurfaceKey)
            it = scene.layers.erase(it);
        else ++it;
}

inline bool SameGpuDesktopScene(const GpuDesktopScene& a, const GpuDesktopScene& b)
{
    if (a.rootId != b.rootId || a.width != b.width || a.height != b.height ||
        a.layers.size() != b.layers.size()) return false;
    for (size_t i = 0; i < a.layers.size(); ++i) {
        const auto& x = a.layers[i]; const auto& y = b.layers[i];
        if (x.key != y.key || x.zeroCopyKey != y.zeroCopyKey || x.serial != y.serial ||
            x.ownerSurfaceKey != y.ownerSurfaceKey || x.parentToplevel != y.parentToplevel ||
            x.subsurface != y.subsurface || x.external != y.external ||
            x.directSurfaceKey != y.directSurfaceKey || x.sampling != y.sampling ||
            x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h ||
            x.sourceW != y.sourceW || x.sourceH != y.sourceH ||
            x.opaque != y.opaque || x.solidBlack != y.solidBlack || x.pixels != y.pixels)
            return false;
    }
    return true;
}

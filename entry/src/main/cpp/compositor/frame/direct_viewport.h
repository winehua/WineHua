#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Direct image sampling state only. Legacy SHM cropping keeps its own pixel
// snapshots; applying this transform to those snapshots would crop twice.
struct DirectViewportState {
    double x = 0, y = 0, width = -1, height = -1;
    int destinationW = -1, destinationH = -1;
    int scale = 1;
    int transform = 0; // wl_output_transform (0..7)
};

// Affine surface-UV -> raw buffer-UV rows, shared with the fragment shader.
struct DirectImageSampling {
    std::array<float, 4> u{0, 1, 0, 0};
    std::array<float, 4> v{0, 0, 1, 0};
    bool operator==(const DirectImageSampling& other) const { return u == other.u && v == other.v; }
    bool operator!=(const DirectImageSampling& other) const { return !(*this == other); }
};
static_assert(sizeof(DirectImageSampling) == 32);

inline bool ComputeDirectViewport(const DirectViewportState& state, int bufferW, int bufferH,
                                  int& destinationW, int& destinationH, DirectImageSampling& out)
{
    if (bufferW <= 0 || bufferH <= 0 || state.scale <= 0 || state.transform < 0 || state.transform > 7)
        return false;
    const bool rotated = (state.transform & 1) != 0;
    const double width = static_cast<double>(rotated ? bufferH : bufferW) / state.scale;
    const double height = static_cast<double>(rotated ? bufferW : bufferH) / state.scale;
    const bool sourceSet = state.width != -1 || state.height != -1;
    const double x = sourceSet ? state.x : 0, y = sourceSet ? state.y : 0;
    const double w = sourceSet ? state.width : width, h = sourceSet ? state.height : height;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) ||
        x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > width || y + h > height) return false;
    if (state.destinationW == -1 && state.destinationH == -1) {
        // Without a destination, viewporter requires an integral source size.
        if (std::floor(w) != w || std::floor(h) != h) return false;
        destinationW = static_cast<int>(w); destinationH = static_cast<int>(h);
    } else {
        if (state.destinationW <= 0 || state.destinationH <= 0) return false;
        destinationW = state.destinationW; destinationH = state.destinationH;
    }
    // set_source is in surface-local coordinates AFTER buffer transform/scale.
    // Match Weston's surface-to-buffer convention: 90 maps (u,v) -> (v,1-u).
    auto bufferUv = [&](double u, double v) -> std::array<double, 2> {
        if (state.transform >= 4) u = 1 - u;
        switch (state.transform & 3) {
        case 1: return {v, 1 - u};
        case 2: return {1 - u, 1 - v};
        case 3: return {1 - v, u};
        default: return {u, v};
        }
    };
    const auto origin = bufferUv(x / width, y / height);
    const auto right = bufferUv((x + w) / width, y / height);
    const auto bottom = bufferUv(x / width, (y + h) / height);
    out.u = {static_cast<float>(origin[0]), static_cast<float>(right[0] - origin[0]),
             static_cast<float>(bottom[0] - origin[0]), 0};
    out.v = {static_cast<float>(origin[1]), static_cast<float>(right[1] - origin[1]),
             static_cast<float>(bottom[1] - origin[1]), 0};
    return true;
}

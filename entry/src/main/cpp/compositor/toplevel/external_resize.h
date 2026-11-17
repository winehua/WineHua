#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

// xdg_toplevel resize edges. Absolute pointer deltas avoid accumulating the
// window's own movement when resizing its left/top edges.
struct ExternalResizeGeometry {
    int32_t x = 0, y = 0, width = 0, height = 0;
    static bool Valid(uint32_t edge) {
        return edge == 1 || edge == 2 || edge == 4 || edge == 5 ||
               edge == 6 || edge == 8 || edge == 9 || edge == 10;
    }
    ExternalResizeGeometry Drag(uint32_t edge, int64_t dx, int64_t dy) const {
        ExternalResizeGeometry result = *this;
        if (!Valid(edge)) return result;
        if (edge & (4 | 8)) {
            result.width = static_cast<int32_t>(std::clamp<int64_t>(width + ((edge & 4) ? -dx : dx), 320, 4096));
        }
        if (edge & (1 | 2)) {
            result.height = static_cast<int32_t>(std::clamp<int64_t>(height + ((edge & 1) ? -dy : dy), 200, 4096));
        }
        // Match the Wine/Xwayland viewport allocation limit on both axes.
        if (int64_t(result.width) * result.height > 8388608) {
            if (edge & (1 | 2)) result.height = 8388608 / result.width;
            else result.width = 8388608 / result.height;
        }
        const auto coordinate = [](int64_t value) {
            return static_cast<int32_t>(std::clamp<int64_t>(value,
                std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
        };
        if (edge & 4) result.x = coordinate(int64_t(x) + width - result.width);
        if (edge & 1) result.y = coordinate(int64_t(y) + height - result.height);
        return result;
    }
};

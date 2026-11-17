#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

// A positioner is copied into each popup. Reusing/destroying the protocol
// object must not mutate an already configured menu.
struct XdgPositionerState {
    int32_t width = 0, height = 0, x = 0, y = 0, rectWidth = 0, rectHeight = 0;
    int32_t offsetX = 0, offsetY = 0, parentWidth = 0, parentHeight = 0;
    uint32_t anchor = 0, gravity = 0, adjustment = 0, parentConfigure = 0;
    bool reactive = false;
    bool Complete() const { return width > 0 && height > 0 && rectWidth > 0 && rectHeight > 0; }
    static int Direction(uint32_t value, bool horizontal) {
        if (horizontal) {
            if (value == 3 || value == 5 || value == 6) return -1;
            if (value == 4 || value == 7 || value == 8) return 1;
        } else {
            if (value == 1 || value == 5 || value == 7) return -1;
            if (value == 2 || value == 6 || value == 8) return 1;
        }
        return 0;
    }
    static int32_t Coordinate(int32_t origin, int32_t extent, int32_t size,
                              int anchor, int gravity, int32_t offset) {
        const int64_t point = int64_t(origin) + (anchor < 0 ? 0 : anchor > 0 ? extent : extent / 2);
        const int64_t result = point - (gravity < 0 ? size : gravity > 0 ? 0 : size / 2) + offset;
        return static_cast<int32_t>(std::clamp(result, int64_t(INT32_MIN), int64_t(INT32_MAX)));
    }
    int32_t X() const { return Coordinate(x, rectWidth, width, Direction(anchor, true), Direction(gravity, true), offsetX); }
    int32_t Y() const { return Coordinate(y, rectHeight, height, Direction(anchor, false), Direction(gravity, false), offsetY); }
};

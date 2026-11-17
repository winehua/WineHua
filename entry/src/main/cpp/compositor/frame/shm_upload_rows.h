#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace winehua {

struct ShmUploadRows {
    uint32_t first = 0, count = 0;
    size_t offset = 0, bytes = 0;
};

// Compare with this image's own ordinary CPU snapshot, after its submission fence.
// Never compare by reading a mapped GPU allocation: it may be uncached.
// A frame slot may miss any number of commits; protocol damage from only the
// newest commit cannot safely describe the difference to its older texture.
// Preserve every BGRA byte (including alpha) and upload a contiguous row span.
inline ShmUploadRows PlanShmUploadRows(const uint8_t* previous,
                                      const uint8_t* current,
                                      uint32_t width, uint32_t height,
                                      bool initialized) {
    const size_t stride = static_cast<size_t>(width) * 4;
    uint32_t first = 0, end = height;
    if (initialized) {
        while (first < end && !std::memcmp(previous + first * stride,
                                           current + first * stride, stride)) ++first;
        while (end > first && !std::memcmp(previous + (end - 1) * stride,
                                           current + (end - 1) * stride, stride)) --end;
    }
    return {first, end - first, first * stride, (end - first) * stride};
}

} // namespace winehua

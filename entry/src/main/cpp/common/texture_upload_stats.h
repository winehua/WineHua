#pragma once
#include <cstdint>

namespace winehua {
// Counts issued GL upload calls, including calls in frames whose swap fails.
// cpuUs is CPU API submission time when explicitly enabled, never GPU DMA time.
// An issued call is not proof of successful GL execution or unique content.
struct TextureUploadStats {
    uint64_t calls = 0, bytes = 0, cpuUs = 0;
    bool timeEnabled = false;
    void Issued(int width, int height, uint64_t elapsedUs) {
        ++calls;
        bytes += static_cast<uint64_t>(width) * height * 4;
        cpuUs += elapsedUs;
    }
};
} // namespace winehua

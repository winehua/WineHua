#pragma once
#include <cstdint>
#include <string_view>

namespace winehua {
// Kept false until device correctness and paired performance gates pass.
// Candidate builds change this compile-time value, never the launch environment.
inline constexpr bool kGlesDirectQualified = false;

struct GlesBufferOwner {
    uint64_t surfaceKey = 0, generation = 0;
    uintptr_t display = 0, context = 0;
    uint32_t width = 0, height = 0;
    int32_t format = 0;
    bool operator==(const GlesBufferOwner& other) const
    {
        return surfaceKey == other.surfaceKey && generation == other.generation &&
            display == other.display && context == other.context &&
            width == other.width && height == other.height && format == other.format;
    }
};

inline bool HasGlExtension(std::string_view list, std::string_view extension)
{
    if (extension.empty() || extension.find(' ') != std::string_view::npos) return false;
    size_t start = 0;
    while ((start = list.find(extension, start)) != std::string_view::npos) {
        const size_t end = start + extension.size();
        if ((!start || list[start - 1] == ' ') && (end == list.size() || list[end] == ' ')) return true;
        start = end;
    }
    return false;
}
} // namespace winehua

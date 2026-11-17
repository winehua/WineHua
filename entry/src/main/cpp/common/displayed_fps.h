#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace winehua {

inline constexpr const char* kDisplayedFpsBasePath =
    "/data/storage/el2/base/files/.wine/drive_c/windows/temp/winehua_display_fps.txt";
inline constexpr uint64_t kDisplayedFpsMaxAgeUs = 2500000;

inline std::string DisplayedFpsPath(const char* basePath, uint32_t toplevelId)
{
    return std::string(basePath) + "." + std::to_string(toplevelId);
}

inline bool PublishDisplayedFpsFile(const std::string& path, uint32_t toplevelId,
                                    uint64_t sequence, double fps, uint64_t nowUs)
{
    if (!toplevelId || !std::isfinite(fps) || fps < 0 || fps > 1000) return false;
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) return false;
    char payload[128];
    const int length = std::snprintf(payload, sizeof(payload), "%llu %.3f %u %llu\n",
        static_cast<unsigned long long>(sequence), fps, toplevelId,
        static_cast<unsigned long long>(nowUs));
    const bool written = length > 0 && length < static_cast<int>(sizeof(payload)) &&
        write(fd, payload, static_cast<size_t>(length)) == length;
    close(fd);
    const bool published = written && rename(temporary.c_str(), path.c_str()) == 0;
    if (!published) unlink(temporary.c_str());
    return published;
}

inline bool PublishDisplayedFpsSample(const char* basePath, uint32_t toplevelId,
                                     uint64_t sequence, double fps, uint64_t nowUs,
                                     bool desktopCompatibility = false)
{
    const bool published = PublishDisplayedFpsFile(DisplayedFpsPath(basePath, toplevelId),
        toplevelId, sequence, fps, nowUs);
    // Bundled OpenGL smoke versions read the desktop sample at the original
    // unsuffixed path. Only the desktop owner publishes that compatibility
    // sample; individual windows must not overwrite each other's statistics.
    if (published && desktopCompatibility)
        PublishDisplayedFpsFile(basePath, toplevelId, sequence, fps, nowUs);
    return published;
}

inline bool ReadDisplayedFpsSample(const char* basePath, uint32_t toplevelId,
                                  uint64_t nowUs, double& fps)
{
    if (!toplevelId) return false;
    FILE* file = fopen(DisplayedFpsPath(basePath, toplevelId).c_str(), "r");
    if (!file) return false;
    unsigned long long sequence = 0, sampledUs = 0;
    unsigned parsedId = 0;
    double parsedFps = 0;
    const int fields = fscanf(file, "%llu %lf %u %llu", &sequence, &parsedFps,
                              &parsedId, &sampledUs);
    fclose(file);
    if (fields != 4 || !sequence || parsedId != toplevelId ||
        !std::isfinite(parsedFps) || parsedFps < 0 || parsedFps > 1000 ||
        nowUs < sampledUs || nowUs - sampledUs > kDisplayedFpsMaxAgeUs)
        return false;
    fps = parsedFps;
    return true;
}

} // namespace winehua

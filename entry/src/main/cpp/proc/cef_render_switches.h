#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace winehua {

// Only fixed rendering switches and allowlisted values can reach the log.
// Presence describes the spawn request, not successful GPU initialization.
inline std::string CefRenderSwitchSummary(const std::vector<std::string>& argv,
                                          const std::vector<std::string>& env) {
    std::string summary;
    auto append = [&](const std::string& field) {
        if (!summary.empty()) summary += ' ';
        summary += field;
    };
    for (const char* flag : {"--disable-gpu", "--disable-gpu-compositing",
         "--disable-software-rasterizer", "--in-process-gpu", "--single-process",
         "--disable-gpu-vsync", "--disable-frame-rate-limit", "--enable-zero-copy",
         "-cef-force-gpu", "-cef-disable-gpu"}) {
        const bool found = std::any_of(argv.begin(), argv.end(), [&](const std::string& arg) {
            return arg == flag || arg.rfind(std::string(flag) + '=', 0) == 0;
        });
        if (found) append(flag);
    }
    auto safeValues = [&](const std::vector<std::string>& entries, const char* prefix,
                          const std::vector<std::string>& allowed) {
        for (const auto& arg : entries) {
            const std::string key(prefix);
            if (arg.rfind(key, 0) != 0) continue;
            const std::string value = arg.substr(key.size());
            append(key + (std::find(allowed.begin(), allowed.end(), value) != allowed.end()
                ? value : "<other>"));
        }
    };
    safeValues(argv, "--use-angle=", {"vulkan", "d3d11", "gl", "gles", "swiftshader", "swiftshader-webgl", "default"});
    safeValues(argv, "--use-gl=", {"angle", "desktop", "egl", "swiftshader", "disabled", "default"});
    safeValues(env, "WINEHUA_CEF_FORCE_SOFTWARE=", {"", "0", "1", "both", "gpu", "compositing"});
    safeValues(env, "WINEHUA_CEF_FORCE_GPU=", {"", "0", "1"});
    safeValues(env, "WINEHUA_CEF_STEAM_DISABLE_GPU=", {"", "0", "1"});
    safeValues(env, "WINEHUA_CEF_ANGLE_BACKEND=", {"", "vulkan", "d3d11", "gl", "swiftshader"});
    return summary.empty() ? "none-recorded" : summary;
}

} // namespace winehua

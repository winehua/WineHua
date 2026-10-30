#ifndef WINEHUA_MEWGENICS_GRAPHICS_H
#define WINEHUA_MEWGENICS_GRAPHICS_H

#include <string>
#include <utility>
#include <vector>
#include <cstring>
#include <strings.h>

namespace winehua {

inline bool IsMewgenicsGpuCandidate(const char* device, const char* exe,
    const char* driver, const char* override, bool qualified, bool ready)
{
    if (!device || strcmp(device, "phone") || !exe || !driver || strcmp(driver, "virpipe")) return false;
    const char* base = exe;
    for (const char* p = exe; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    return !strcasecmp(base, "Mewgenics.exe") && qualified && ready &&
        (!override || (!strcmp(override, "zink")));
}

inline bool IsDarkestWin64(const char* executable)
{
    if (!executable) return false;
    std::string path(executable);
    for (char& ch : path) {
        if (ch == '\\') ch = '/';
        else if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
    const std::string suffix = "/_windows/win64/darkest.exe";
    return path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Reuse the qualified managed Zink runtime. Automatic selection remains
// limited to programs actually validated on this phone/driver combination.
inline bool IsManagedZinkCandidate(const char* device, const char* executable,
    const char* driver, const char* renderer, bool qualified, bool ready,
    const char* legacyMewgenicsOverride = nullptr)
{
    if (!device || strcmp(device, "phone") || !driver || strcmp(driver, "virpipe") ||
        !qualified || !ready || !executable) return false;
    if (renderer && !strcmp(renderer, "zink")) return true;
    if (renderer && strcmp(renderer, "auto")) return false;
    return IsDarkestWin64(executable) || IsMewgenicsGpuCandidate(device, executable,
        driver, legacyMewgenicsOverride, qualified, ready);
}

// Paths are supplied by the managed-runtime owner, never by the game.
inline std::vector<std::pair<std::string, std::string>> MewgenicsGpuEnvironment(
    const std::string& root, const std::string& bin)
{
    const std::string icd = bin + "/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json";
    return {
        {"GALLIUM_DRIVER", "zink"},
        {"MESA_LOADER_DRIVER_OVERRIDE", "zink"},
        {"WINEHUA_EGL_LIBRARY_PATH", root + "/lib/libEGL.so"},
        {"LIBGL_DRIVERS_PATH", root + "/lib/dri"},
        {"BOX64_LD_LIBRARY_PATH", root + "/lib:" + bin + "/guest_vulkan/lib:" +
            bin + "/guest_gfx/lib:" + bin + ":" + bin + "/x86_64-unix:" + bin + "/../lib/x86_64"},
        {"BOX64_EMULATED_LIBS", "libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8"},
        // Avoid the disk-cache hash fault in RecordEnvMappings/ReadDynaCache.
        // This does not disable the runtime dynarec or change its tuning.
        {"BOX64_DYNACACHE", "0"},
        {"VK_DRIVER_FILES", icd}, {"VK_ICD_FILENAMES", icd},
        {"VN_DEBUG", "vtest"},
        {"VN_PERF", "no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring"},
        {"VN_WINEHUA_STRONG_RING_BARRIER", "1"},
        {"VN_WINEHUA_REMOTE_MEMORY_SYNC", "1"},
        // A readback reply must wake the ring even inside the normal batching
        // window. Asynchronous draws keep their existing notification policy.
        {"VN_WINEHUA_SYNC_NOTIFY_RING", "1"},
        {"VN_WINEHUA_PERSISTENT_MAP_SYNC", "0"},
        {"VN_WINEHUA_DIRECT_FENCE_WAIT", "1"},
        {"LIBGL_ALWAYS_SOFTWARE", "0"},
        {"VK_LOADER_LAYERS_DISABLE", "~implicit~"},
        {"ZINK_DEBUG", "nobgc,noreorder"},
        {"ZINK_WINEHUA_SHADOW_TRANSFERS", "true"},
        {"ZINK_WINEHUA_BO_CACHE_MB", "64"},
        {"ZINK_WINEHUA_READBACK_CACHE", "true"},
        // Keep cache behavior identical to the qualified v6 runtime.
        {"MESA_SHADER_CACHE_DISABLE", "true"},
        {"WINEHUA_OPENGL_PROFILE", "mewgenics-zink-v6"}
    };
}

} // namespace winehua
#endif

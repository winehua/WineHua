#include "wine_spawn_compatibility.h"
#include "wine_image_policy.h"
#include "graphics/graphics_profile.h"
#include "wine/wine_env.h"
#include "wine/wine_constants.h"
#include <unistd.h>

namespace winehua::spawn {
FreshWineGraphics ConfigureFreshWineGraphics(Request& request, bool fresh,
                                             const std::string& binDir) {
    if (!fresh || ImageEnvironment(request, "WINEHUA_VULKAN_BACKEND") != "direct")
        return FreshWineGraphics::Unchanged;
    const std::string icd = binDir + "/guest_vulkan/share/vulkan/icd.d/venus_icd."
        WINE_WINE_ARCH ".json";
    if (access(icd.c_str(), R_OK) != 0) return FreshWineGraphics::MissingRuntime;
    // Opt-in A/B for the validated legacy DXVK range-publication contract.
    // HOST_COHERENT clients without these explicit upload hooks still need
    // the whole-map fallback. Capture the request before Venus policy rebuilds it.
    const bool preciseMaps =
        ImageEnvironment(request, "WINEHUA_FRESH_VENUS_PRECISE_MAP") == "1" &&
        ImageEnvironment(request, "WINEHUA_D3D_BACKEND") == "dxvk_legacy" &&
        ImageEnvironment(request, "DXVK_WINEHUA_PRECISE_SHADOW") == "1" &&
        ImageEnvironment(request, "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED") == "1";
    // Preserve the route while observing the existing rate-limited ring
    // counters. LAB observation profiles may otherwise rebuild that route.
    const bool ringPerf =
        ImageEnvironment(request, "WINEHUA_FRESH_VENUS_PERF_SUMMARY") == "1";
    auto environment = request.env;
    // Start NCP has a clean low address space but no verified native Vulkan
    // driver/producer IPC. Keep GPU work in the existing renderer process.
    AppendVulkanRuntimeEnv(environment, binDir);
    std::vector<std::string> venus;
    if (winehua::BuildVenusGuestGraphicsEnvironment(
            winehua::ParseD3dBackend(ImageEnvironment(request, "WINEHUA_D3D_BACKEND")), &venus))
        for (const auto& value : venus) UpsertEnvLine(environment, value);
    UpsertEnvLine(environment, "WINEHUA_VULKAN_BACKEND=venus");
    UpsertEnvLine(environment, preciseMaps ? "WINEHUA_VK_PRECISE_MAP=1"
                                         : "WINEHUA_VK_PRECISE_MAP=0");
    UpsertEnvLine(environment, ringPerf ? "VN_WINEHUA_PERF_SUMMARY=1"
                                      : "VN_WINEHUA_PERF_SUMMARY=0");
    UpsertEnvLine(environment, "WINEHUA_PRESENT_BACKEND=venus_broker_present");
    UpsertEnvLine(environment, "WINEHUA_VULKAN_PRESENT=1");
    request.env = std::move(environment);
    return FreshWineGraphics::Venus;
}
}

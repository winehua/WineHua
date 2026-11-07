#ifndef WINEHUA_DXVK_PROFILE_H
#define WINEHUA_DXVK_PROFILE_H

#include <string>

namespace winehua {

struct DxvkBackendProfile {
    bool supported;
    bool modern;
    bool d3d9;
    const char* runtimeDirectory;
    const char* version;
    const char* dllOverrides;
};

// Keep the D3D9 overlay opt-in. Some older games work better with WineD3D,
// while the standard DXVK legacy profile remains a D3D10/11 route.
inline DxvkBackendProfile GetDxvkBackendProfile(const std::string& backend)
{
    if (backend == "dxvk_legacy_d3d9")
        return {true, false, true, "legacy", "1.10.3",
                "d3d9=n;d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n"};
    if (backend == "dxvk_legacy")
        return {true, false, false, "legacy", "1.10.3",
                "d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n"};
    if (backend == "dxvk_modern_2_6")
        return {true, true, false, "modern-2.6", "2.6.2", "d3d11=n;dxgi=n"};
    return {false, false, false, "", "", ""};
}

} // namespace winehua

#endif // WINEHUA_DXVK_PROFILE_H

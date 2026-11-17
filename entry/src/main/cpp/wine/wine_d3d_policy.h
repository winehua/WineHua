#ifndef WINE_D3D_POLICY_H
#define WINE_D3D_POLICY_H

namespace winehua {

// Existing DXVK profiles retain WineD3D for D3D9; DXVK9 is explicit. Callers
// can still merge explicit per-game overrides after this default policy.
inline const char* DxvkDllOverrides(bool modern26, bool d3d9Native = false)
{
    if (d3d9Native && !modern26)
        return "ddraw=b;d3d8=b;d3d9=n;d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n;vulkan-1=b";
    return modern26
        ? "ddraw=b;d3d8=b;d3d9=b;d3d11=n;dxgi=n;vulkan-1=b"
        : "ddraw=b;d3d8=b;d3d9=b;d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n;vulkan-1=b";
}

} // namespace winehua
#endif

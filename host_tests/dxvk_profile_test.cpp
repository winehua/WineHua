#include "dxvk_profile.h"

#include <cassert>
#include <string>

int main()
{
    using winehua::GetDxvkBackendProfile;

    const auto d3d9 = GetDxvkBackendProfile("dxvk_legacy_d3d9");
    assert(d3d9.supported && !d3d9.modern && d3d9.d3d9);
    assert(std::string(d3d9.runtimeDirectory) == "legacy");
    assert(std::string(d3d9.version) == "1.10.3");
    assert(std::string(d3d9.dllOverrides) ==
           "d3d9=n;d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n");

    const auto legacy = GetDxvkBackendProfile("dxvk_legacy");
    assert(legacy.supported && !legacy.modern && !legacy.d3d9);
    assert(std::string(legacy.dllOverrides).find("d3d9=") == std::string::npos);

    const auto modern = GetDxvkBackendProfile("dxvk_modern_2_6");
    assert(modern.supported && modern.modern && !modern.d3d9);
    assert(std::string(modern.runtimeDirectory) == "modern-2.6");
    assert(std::string(modern.dllOverrides) == "d3d11=n;dxgi=n");

    assert(!GetDxvkBackendProfile("wined3d").supported);
    assert(!GetDxvkBackendProfile("dxvk_unknown").supported);
    return 0;
}

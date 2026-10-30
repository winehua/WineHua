#include "wine/host_integration_policy.h"
#include <cassert>
#include <cstdio>

int main()
{
    using winehua::WineHostDllOverrides;
    assert(WineHostDllOverrides("phone", nullptr) == "winemenubuilder.exe=d");
    assert(WineHostDllOverrides("phone", "") == "winemenubuilder.exe=d");
    assert(WineHostDllOverrides("phone", "d3d11,dxgi=n") == "d3d11,dxgi=n;winemenubuilder.exe=d");
    assert(WineHostDllOverrides("phone", "d3d9=n;") == "d3d9=n;winemenubuilder.exe=d");
    for (const char* value : {"winemenubuilder.exe=d", "winemenubuilder=n", "WINEMENUBUILDER.EXE=b", "foo, winemenubuilder.exe =n;dxgi=n"})
        assert(WineHostDllOverrides("phone", value) == value);
    assert(WineHostDllOverrides("phone", "mywinemenubuilder.exe=n") == "mywinemenubuilder.exe=n;winemenubuilder.exe=d");
    assert(WineHostDllOverrides("2in1", "dxgi=n") == "dxgi=n");
    assert(WineHostDllOverrides("tablet", "") == "");
    assert(WineHostDllOverrides(nullptr, nullptr) == "");
    const auto once = WineHostDllOverrides("phone", "d3d11=n");
    assert(WineHostDllOverrides("phone", once.c_str()) == once);
    std::puts("PASS: phone menu-helper default, explicit overrides, other devices and idempotence");
}

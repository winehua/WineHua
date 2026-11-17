#include "../entry/src/main/cpp/wine/wine_cpu_policy.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main()
{
    using namespace winehua;
    for (const char* input : {static_cast<const char*>(nullptr), "", "fex", "box64", "invalid"})
        assert(!std::strcmp(WineWow64CpuModule(input), "libwow64fex.dll"));
    assert(!std::strcmp(WineWow64CpuModule("box"), "wowbox64.dll"));
    for (const char* input : {"WINEHUA_WOW64_ENGINE=box", "WINEHUA_WOW64_ENGINE=fex",
                            "BOX64_DYNAREC_SAFEFLAGS=2", "BOX64_DYNAREC_FORWARD=512",
                            "BOX64_DYNAREC_X87DOUBLE=1", "BOX64_DYNAREC_INTERP_SIGNAL=1"})
    {
        assert(IsWineCpuEnvironmentLine(input));
        assert(IsWineLaunchEnvironmentLine(input));
    }
    for (const char* input : {"", "WINEHUA_WOW64_ENGINE=box64", "WINEHUA_WOW64_ENGINE=",
                            "LD_PRELOAD=/tmp/test.so", "HODLL=other.dll", "FEX_CONFIG=1",
                            "BOX64_DYNAREC_=1", "BOX64_DYNAREC_FORWARD=-1",
                            "BOX64_DYNAREC_FORWARD=4097", "BOX64_DYNAREC_FORWARD=10000",
                            "BOX64_DYNAREC_FOO-BAR=1", "BOX64_DYNAREC_FORWARD=1;X=2",
                            "BOX64_DYNAREC_FORWARD=1\n", "BOX64_DYNAREC_forward=1"})
    {
        assert(!IsWineCpuEnvironmentLine(input));
        assert(!IsWineLaunchEnvironmentLine(input));
    }
    // Every ordinary session sends an explicit off/on value, including FEX.
    // The CPU-only validator rejected this and prevented any native launch.
    for (const char* input : {"WINEHUA_DXVK_D24_STORAGE_D32=0", "WINEHUA_DXVK_D24_STORAGE_D32=1"}) {
        assert(!IsWineCpuEnvironmentLine(input));
        assert(IsWineLaunchEnvironmentLine(input));
    }
    for (const char* input : {"WINEHUA_DXVK_D24_STORAGE_D32=", "WINEHUA_DXVK_D24_STORAGE_D32=2",
                            "WINEHUA_DXVK_D24_STORAGE_D32=true", "WINEHUA_DXVK_D24_STORAGE_D32=01",
                            "WINEHUA_DXVK_D24_STORAGE_D32=1\n", "WINEHUA_DXVK_D24_STORAGE_D32=1;X=2"})
        assert(!IsWineLaunchEnvironmentLine(input));
    assert(!IsWineLaunchEnvironmentLine(std::string("WINEHUA_DXVK_D24_STORAGE_D32=1\0junk", 32)));
    assert(!IsWineCpuEnvironmentLine(std::string("WINEHUA_WOW64_ENGINE=box\0junk", 28)));
    assert(!IsWineCpuEnvironmentLine("BOX64_DYNAREC_" + std::string(260, 'A') + "=1"));
    std::cout << "Wine CPU policy: backend mapping and bounded environment validation PASS\n";
}

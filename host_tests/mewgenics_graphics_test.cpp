#include "wine/mewgenics_graphics.h"
#include <cassert>
#include <map>

int main()
{
    using winehua::IsMewgenicsGpuCandidate;
    using winehua::IsManagedZinkCandidate;
    const char* darkest = "Z:\\games\\DarkestDungeon\\DarkestDungeon\\_windows\\win64\\Darkest.exe";
    assert(IsManagedZinkCandidate("phone", darkest, "virpipe", "auto", true, true));
    assert(!IsManagedZinkCandidate("phone", darkest, "virpipe", "virpipe", true, true));
    assert(!IsManagedZinkCandidate("phone", darkest, "virpipe", "auto", false, true));
    assert(!IsManagedZinkCandidate("phone", darkest, "virpipe", "auto", true, false));
    assert(!IsManagedZinkCandidate("other", darkest, "virpipe", "auto", true, true));
    assert(!IsManagedZinkCandidate("phone", "Z:/games/_windowsnosteam/win64/Darkest.exe", "virpipe", "auto", true, true));
    assert(!IsManagedZinkCandidate("phone", "Z:/games/_windows/win32/Darkest.exe", "virpipe", "auto", true, true));
    assert(!IsManagedZinkCandidate("phone", "Z:/games/Other.exe", "virpipe", "auto", true, true));
    assert(IsManagedZinkCandidate("phone", "Z:/games/Other.exe", "virpipe", "zink", true, true));
    assert(!IsManagedZinkCandidate("phone", darkest, "softpipe", "auto", true, true));
    assert(IsMewgenicsGpuCandidate("phone", "Z:\\games\\MewGenics\\MEWGENICS.EXE", "virpipe", nullptr, true, true));
    assert(!IsMewgenicsGpuCandidate("phone", "game.exe", "virpipe", nullptr, true, true));
    assert(!IsMewgenicsGpuCandidate("other", "Mewgenics.exe", "virpipe", nullptr, true, true));
    assert(!IsMewgenicsGpuCandidate("phone", "Mewgenics.exe", "softpipe", nullptr, true, true));
    assert(!IsMewgenicsGpuCandidate("phone", "Mewgenics.exe", "virpipe", "softpipe", true, true));
    assert(!IsMewgenicsGpuCandidate("phone", "Mewgenics.exe", "virpipe", nullptr, false, true));
    assert(!IsMewgenicsGpuCandidate("phone", "Mewgenics.exe", "virpipe", nullptr, true, false));
    auto entries = winehua::MewgenicsGpuEnvironment("/managed/gfx", "/managed/wine/bin");
    const std::map<std::string,std::string> env(entries.begin(), entries.end());
    assert(env.size() == entries.size());
    assert(env.at("GALLIUM_DRIVER") == "zink");
    assert(env.at("ZINK_WINEHUA_SHADOW_TRANSFERS") == "true");
    assert(env.at("VN_WINEHUA_PERSISTENT_MAP_SYNC") == "0");
    assert(env.at("VN_WINEHUA_SYNC_NOTIFY_RING") == "1");
    assert(env.at("BOX64_DYNACACHE") == "0");
    assert(!env.count("BOX64_DYNAREC"));
    assert(!env.count("WINEHUA_GL_READBACK_BATCH"));
    assert(!env.count("WINEHUA_GL_READBACK_PBO"));
    assert(env.at("ZINK_WINEHUA_BO_CACHE_MB") == "64");
    assert(env.at("ZINK_WINEHUA_READBACK_CACHE") == "true");
    assert(env.at("LIBGL_ALWAYS_SOFTWARE") == "0");
    assert(env.at("WINEHUA_EGL_LIBRARY_PATH") == "/managed/gfx/lib/libEGL.so");
    assert(env.at("VK_DRIVER_FILES") == env.at("VK_ICD_FILENAMES"));
    assert(!env.count("WINEUNIXDIR") && !env.count("WINEPREFIX"));
    assert(IsMewgenicsGpuCandidate("phone", "C:\\games\\Mewgenics.exe",
                                   "virpipe", nullptr, true, true));
}

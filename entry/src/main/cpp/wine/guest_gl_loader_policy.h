#pragma once
#include <cstring>

namespace winehua {
// Zink explicitly loading the system Vulkan library must not inherit the
// packaged Venus ICD manifest. This does not select Direct presentation.
inline bool UsesSystemZinkLoader(const char* galliumDriver, const char* library) {
    return galliumDriver && library && !std::strcmp(galliumDriver, "zink") &&
        !std::strcmp(library, "/system/lib64/libvulkan.so");
}
}

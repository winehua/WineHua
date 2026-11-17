#pragma once

#include <string>

namespace winehua {

// launchClient accepts only the original Wine CPU settings here. In
// particular, this is not a second arbitrary preload/graphics environment.
inline bool IsWineCpuEnvironmentLine(const std::string& line)
{
    if (line.empty() || line.size() > 256 || line.find('\0') != std::string::npos)
        return false;
    const auto equals = line.find('=');
    if (equals == std::string::npos) return false;
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "WINEHUA_WOW64_ENGINE") return value == "box" || value == "fex";
    if (key.rfind("BOX64_DYNAREC_", 0) != 0 || key.size() == 14 ||
        value.empty() || value.size() > 4)
        return false;
    for (size_t i = 14; i < key.size(); ++i)
        if (!((key[i] >= 'A' && key[i] <= 'Z') || (key[i] >= '0' && key[i] <= '9') || key[i] == '_')) return false;
    unsigned number = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return false;
        number = number * 10 + static_cast<unsigned>(ch - '0');
    }
    return number <= 4096;
}

// launchClient has a restricted session environment, not an arbitrary
// preload/graphics override channel. Keep CPU-only validation reusable.
inline bool IsWineLaunchEnvironmentLine(const std::string& line)
{
    return IsWineCpuEnvironmentLine(line) ||
        line == "WINEHUA_DXVK_D24_STORAGE_D32=0" ||
        line == "WINEHUA_DXVK_D24_STORAGE_D32=1";
}

inline const char* WineWow64CpuModule(const char* selection)
{
    return selection && std::string(selection) == "box" ? "wowbox64.dll" : "libwow64fex.dll";
}

} // namespace winehua

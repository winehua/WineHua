#pragma once
#include <string>
#include <cstring>

namespace winehua {

// Text compatibility is independent from the WineD3D scheduling policy.
// Opt in only when diagnosing a synchronous-GDI hang; the default must match
// an Explorer launch instead of silently disabling the command-stream thread.
inline const char* DuanwuD3dOverride(const char* synchronousGdi)
{
    return synchronousGdi && !std::strcmp(synchronousGdi, "1") ? "csmt=0" : nullptr;
}

// The Duanwu launcher builds a 16-bit glyph mask and discards gray pixels.
// Match the known game directory, rather than every executable named launcher.
inline bool UsesDuanwuBitmapText(std::string executable, std::string directory = {})
{
    for (char& ch : executable) if (ch == '\\') ch = '/';
    for (char& ch : directory) if (ch == '\\') ch = '/';
    const auto slash = executable.find_last_of('/');
    if (executable.substr(slash == std::string::npos ? 0 : slash + 1) != "启动器.exe") return false;
    if (slash != std::string::npos) directory = executable.substr(0, slash);
    while (!directory.empty() && directory.back() == '/') directory.pop_back();
    const auto parent = directory.find_last_of('/');
    return directory.substr(parent == std::string::npos ? 0 : parent + 1) == "唯美端午节补丁版";
}

} // namespace winehua

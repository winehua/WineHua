#pragma once

#include <map>
#include <string>
#include <vector>

namespace winehua {

inline bool IsDisplayEnvironmentKey(const std::string& name)
{
    return name == "DISPLAY" || name == "WAYLAND_DISPLAY" || name == "WAYLAND_SOCKET" ||
           name == "XAUTHORITY" || name == "XMODIFIERS" || name == "XLOCALEDIR" || name == "XDG_RUNTIME_DIR";
}

struct WineDisplayEnvironment {
    bool x11 = false;
    std::string brokerSuffix;
};

// Serialize the HGS endpoint once for every child created by the Wine broker.
// Empty values in this display-only contract represent unset.
inline bool BuildWineDisplayEnvironment(const std::vector<std::string>& lines,
                                        WineDisplayEnvironment& result)
{
    std::map<std::string, std::string> values;
    for (const auto& line : lines) {
        const auto split = line.find('=');
        if (split == std::string::npos || line.find_first_of("|\r\n") != std::string::npos ||
            line.find('\0') != std::string::npos)
            return false;
        const auto name = line.substr(0, split);
        if (!IsDisplayEnvironmentKey(name) || !values.emplace(name, line.substr(split + 1)).second)
            return false;
    }
    const bool x11 = !values["DISPLAY"].empty();
    if (x11) {
        const auto& display = values["DISPLAY"];
        if (display.size() < 2 || display.front() != ':' ||
            display.find_first_not_of("0123456789", 1) != std::string::npos ||
            values["XAUTHORITY"].empty() || values["XAUTHORITY"].front() != '/' ||
            !values["WAYLAND_DISPLAY"].empty() || !values["XDG_RUNTIME_DIR"].empty())
            return false;
    } else if (values["WAYLAND_DISPLAY"].empty() || values["WAYLAND_DISPLAY"].find('/') != std::string::npos ||
               values["XDG_RUNTIME_DIR"].empty() || values["XDG_RUNTIME_DIR"].front() != '/' ||
               !values["XAUTHORITY"].empty()) {
        return false;
    }
    if (!values["WAYLAND_SOCKET"].empty())
        return false;
    const auto& modifiers = values["XMODIFIERS"];
    const auto& localeDir = values["XLOCALEDIR"];
    if (!modifiers.empty() && (!x11 || modifiers.rfind("@im=", 0) != 0 || modifiers.size() <= 4 ||
        modifiers.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-", 4) !=
            std::string::npos || localeDir.empty() || localeDir.front() != '/')) return false;
    if (modifiers.empty() && !localeDir.empty()) return false;

    // Keep Wine's own runtime directory for X11. HGS publishes only DISPLAY and
    // XAUTHORITY for this transport. Force-clear keys from the previous route.
    if (x11) values.erase("XDG_RUNTIME_DIR");
    result.x11 = x11;
    result.brokerSuffix = x11 ? "|__env=WINEHUA_DISPLAY_ROUTE=x11" : "|__env=WINEHUA_DISPLAY_ROUTE=wayland";
    for (const auto& [key, value] : values)
        result.brokerSuffix += "|__env=" + key + "=" + value;
    return true;
}

} // namespace winehua

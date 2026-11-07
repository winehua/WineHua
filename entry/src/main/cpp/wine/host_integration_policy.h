#ifndef WINEHUA_HOST_INTEGRATION_POLICY_H
#define WINEHUA_HOST_INTEGRATION_POLICY_H

#include <algorithm>
#include <cctype>
#include <string>

namespace winehua {

// Wine's XDG menu builder has no desktop integration to perform on a phone.
// A failing helper can loop in the PE exception dispatcher during wineboot,
// preventing a usable desktop from being painted. Keep game DLL overrides and
// explicit helper overrides intact; apply this default after per-run settings.
inline std::string WineHostDllOverrides(const char* device, const char* overrides)
{
    const std::string original = overrides ? overrides : "";
    if (!device || std::string(device) != "phone") return original;
    size_t group = 0;
    while (group < original.size()) {
        const size_t end = original.find(';', group);
        const size_t limit = end == std::string::npos ? original.size() : end;
        const size_t equals = original.find('=', group);
        if (equals != std::string::npos && equals < limit) {
            size_t name = group;
            while (name < equals) {
                const size_t comma = original.find(',', name);
                const size_t next = comma != std::string::npos && comma < equals ? comma : equals;
                std::string module = original.substr(name, next - name);
                module.erase(std::remove_if(module.begin(), module.end(), [](unsigned char c) {
                    return std::isspace(c) != 0;
                }), module.end());
                std::transform(module.begin(), module.end(), module.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (module == "winemenubuilder" || module == "winemenubuilder.exe") return original;
                name = next + 1;
            }
        }
        group = limit + 1;
    }
    return original + (original.empty() || original.back() == ';' ? "" : ";") + "winemenubuilder.exe=d";
}

} // namespace winehua
#endif

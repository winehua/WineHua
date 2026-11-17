#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

namespace winehua {

// wineboot creates registry files and .update-timestamp before wine.inf has
// finished registering DLLs. Require the shell COM servers in both views;
// otherwise ExplorerBrowser cannot open and the prefix needs an update.
inline bool HasPrefixShellRegistrations(const std::string& registryPath)
{
    static const char* keys[] = {
        R"([Software\\Classes\\CLSID\\{71F96385-DDD6-48D3-A0C1-AE06E8B055FB}\\InprocServer32])",
        R"([Software\\Classes\\CLSID\\{9BA05972-F6A8-11CF-A442-00A0C90A8F39}\\InprocServer32])",
        R"([Software\\Classes\\Wow6432Node\\CLSID\\{71F96385-DDD6-48D3-A0C1-AE06E8B055FB}\\InprocServer32])",
        R"([Software\\Classes\\Wow6432Node\\CLSID\\{9BA05972-F6A8-11CF-A442-00A0C90A8F39}\\InprocServer32])",
    };
    bool registered[4] = {};
    int currentKey = -1;
    FILE* file = fopen(registryPath.c_str(), "r");
    if (!file) return false;
    char line[4096];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '[') {
            currentKey = -1;
            for (int i = 0; i < 4; ++i) {
                if (!strncasecmp(line, keys[i], strlen(keys[i]))) {
                    currentKey = i;
                    break;
                }
            }
        } else if (currentKey >= 0 && !strncmp(line, "@=\"", 3) &&
                   line[3] && line[3] != '"') {
            registered[currentKey] = true;
        }
    }
    const bool readOk = !ferror(file);
    fclose(file);
    return readOk && registered[0] && registered[1] && registered[2] && registered[3];
}

} // namespace winehua

#pragma once
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace winehua {
// A private Steam install is persistent user data, even during a clean Wine
// reset. Keep it in place; no multi-GB copy, transient move or link is required.
inline bool RemovePrefixTree(const std::string& path, const std::string& keep) {
    struct stat status{};
    if (lstat(path.c_str(), &status)) return errno == ENOENT;
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) { errno = ENOTDIR; return false; }
    if (path == keep) return true;
    DIR* directory = opendir(path.c_str());
    if (!directory) return false;
    bool ok = true;
    while (auto* entry = readdir(directory)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        const std::string child = path + '/' + entry->d_name;
        if (lstat(child.c_str(), &status)) { ok = false; continue; }
        if (S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode)) {
            if (!RemovePrefixTree(child, keep)) ok = false;
        } else if (unlink(child.c_str())) ok = false;
    }
    closedir(directory);
    // Leave only the two ancestors of the exact protected directory.
    const bool ancestor = !keep.empty() && keep.compare(0, path.size() + 1, path + '/') == 0;
    if (!ancestor && rmdir(path.c_str()) && errno != ENOENT) ok = false;
    return ok;
}
}

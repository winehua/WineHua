#include "fs_utils.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <dlfcn.h>

namespace winehua {

bool EnsureDir(const std::string& path)
{
    if (path.empty()) return false;
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool FileExists(const std::string& path)
{
    return !path.empty() && access(path.c_str(), F_OK) == 0;
}

bool DirExists(const std::string& path)
{
    struct stat st = {};

    if (path.empty()) return false;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool DirHasSharedObjectWithPrefix(const std::string& dir, const std::string& prefix)
{
    DIR* handle = nullptr;
    struct dirent* entry = nullptr;

    if (dir.empty() || prefix.empty()) return false;
    handle = opendir(dir.c_str());
    if (!handle) return false;

    while ((entry = readdir(handle)))
    {
        std::string name = entry->d_name;
        if (name.rfind(prefix, 0) != 0) continue;
        if (name.find(".so") == std::string::npos) continue;
        closedir(handle);
        return true;
    }

    closedir(handle);
    return false;
}

std::string DirNameCopy(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

const char* BasenameOfPath(const char* path)
{
    if (!path || !path[0]) return path;
    const char* slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

void RemoveDir(const char* path)
{
    DIR* d = opendir(path);
    if (!d) return;
    dirent* e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        std::string full = std::string(path) + "/" + e->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) RemoveDir(full.c_str());
            else unlink(full.c_str());
        }
    }
    closedir(d);
    rmdir(path);
}

std::string CurrentSharedObjectDir()
{
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(&CurrentSharedObjectDir), &info) && info.dli_fname && info.dli_fname[0])
        return DirNameCopy(info.dli_fname);
    return "";
}

} // namespace winehua

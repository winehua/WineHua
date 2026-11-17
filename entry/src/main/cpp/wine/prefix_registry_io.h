#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace winehua {

// Called only by the wineserver startup hook, after acquiring its session lock.
inline bool ReadPrefixRegistry(const char* path, std::string& out)
{
    out.clear();
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode);
    char buf[65536];
    while (ok) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (!n) break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (!ok || out.size() != static_cast<size_t>(st.st_size) ||
        out.compare(0, 24, "WINE REGISTRY Version 2\n")) {
        out.clear();
        return false;
    }
    return true;
}

// Never truncate the inode a registry loader may already have open. A failed
// write leaves the old hive intact; rename publishes only a complete file.
inline bool WritePrefixRegistry(const char* path, const std::string& data)
{
    struct stat st;
    if (stat(path, &st)) return false;
    std::string temporary = std::string(path) + ".winehua-XXXXXX";
    int fd = mkstemp(&temporary[0]);
    if (fd < 0) return false;
    bool ok = !fchmod(fd, st.st_mode & 0777);
    size_t off = 0;
    while (ok && off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        off += static_cast<size_t>(n);
    }
    if (ok) {
        int result;
        do { result = fsync(fd); } while (result < 0 && errno == EINTR);
        ok = result == 0;
    }
    if (close(fd)) ok = false;
    if (ok) ok = !rename(temporary.c_str(), path);
    if (!ok) unlink(temporary.c_str());
    return ok;
}

} // namespace winehua

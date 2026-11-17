#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace winehua {
constexpr off_t kStderrFileBytes = 16 * 1024 * 1024;

inline bool WriteStderrBytes(int fd, const char* data, size_t size) {
    while (size) {
        const ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n; size -= static_cast<size_t>(n);
    }
    return true;
}

// All children open the same daily inode independently. Lock before checking
// the quota so simultaneous Wine/wineserver writers cannot grow it unbounded.
// Truncate in place: renaming a file leaves other children's open FDs writing
// to the old inode. Keep a clear marker when older output has been discarded.
inline bool AppendStderrFile(int fd, const char* data, size_t size,
                             off_t limit = kStderrFileBytes) {
    if (fd < 0 || limit < 256) return false;
    int result;
    do { result = flock(fd, LOCK_EX); } while (result < 0 && errno == EINTR);
    if (result < 0) return false;
    struct stat st{};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    static constexpr char marker[] = "\n=== older Wine stderr discarded (size limit) ===\n";
    if (size > static_cast<size_t>(limit) - sizeof(marker)) {
        data += size - (static_cast<size_t>(limit) - sizeof(marker));
        size = static_cast<size_t>(limit) - sizeof(marker);
    }
    if (ok && st.st_size + static_cast<off_t>(size) > limit) {
        ok = ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) >= 0 &&
             WriteStderrBytes(fd, marker, sizeof(marker) - 1);
    }
    if (ok) ok = WriteStderrBytes(fd, data, size);
    flock(fd, LOCK_UN);
    return ok;
}

inline bool IsStderrFileName(const char* name) {
    if (strlen(name) != 24 || strncmp(name, "wine_stderr_", 12) || strcmp(name + 20, ".log")) return false;
    for (int i = 12; i < 20; ++i) if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

inline int OpenStderrFile(const char* directory, time_t now) {
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    const int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    struct tm local{};
    localtime_r(&now, &local);
    char name[32];
    strftime(name, sizeof(name), "wine_stderr_%Y%m%d.log", &local);
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat opened{};
    if (fd >= 0 && (fstat(fd, &opened) < 0 || !S_ISREG(opened.st_mode))) {
        close(fd); fd = -1;
    }
    // Delete only our own regular daily files older than seven local dates.
    // No symlink following and no traversal into user/game directories.
    local.tm_mday -= 6; local.tm_isdst = -1; mktime(&local);
    char cutoff[32]; strftime(cutoff, sizeof(cutoff), "wine_stderr_%Y%m%d.log", &local);
    DIR* entries = fdopendir(dup(dir));
    if (entries) {
        while (auto* entry = readdir(entries)) {
            struct stat st{};
            if (IsStderrFileName(entry->d_name) && strcmp(entry->d_name, cutoff) < 0 &&
                fstatat(dir, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode))
                unlinkat(dir, entry->d_name, 0);
        }
        closedir(entries);
    }
    close(dir);
    return fd;
}
} // namespace winehua

#include "common/app_log.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>
#include <deque>
#include <mutex>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr size_t kMaxLineBytes = 4096;
constexpr size_t kMaxFileBytes = 20u * 1024u * 1024u;  // 20MB same-day safety valve
constexpr int kRetentionDays = 7;
constexpr size_t kPreInitRingLines = 256;

std::mutex gMutex;
std::string gDir;
int gFd = -1;
std::string gDateKey;  // YYYYMMDD
int gSeq = 0;
size_t gBytes = 0;
std::deque<std::string> gPending;

std::string TodayKey() {
    time_t t = time(nullptr);
    struct tm ltm;
    localtime_r(&t, &ltm);
    char buf[16] = {};
    snprintf(buf, sizeof(buf), "%04d%02d%02d",
             ltm.tm_year + 1900, ltm.tm_mon + 1, ltm.tm_mday);
    return buf;
}

void StampInto(char* buf, size_t len) {
    time_t t = time(nullptr);
    struct tm ltm;
    localtime_r(&t, &ltm);
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             ltm.tm_year + 1900, ltm.tm_mon + 1, ltm.tm_mday,
             ltm.tm_hour, ltm.tm_min, ltm.tm_sec, tv.tv_usec / 1000);
}

std::string FilePathLocked(const std::string& date, int seq) {
    std::string name = "native-" + date;
    if (seq > 0) name += "-" + std::to_string(seq);
    name += ".log";
    return gDir + "/" + name;
}

void CloseFdLocked() {
    if (gFd >= 0) {
        ::close(gFd);
        gFd = -1;
    }
}

void OpenCurrentLocked() {
    CloseFdLocked();
    gBytes = 0;
    int fd = ::open(FilePathLocked(gDateKey, gSeq).c_str(),
                    O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            gBytes = static_cast<size_t>(st.st_size);
        }
        gFd = fd;
    }
}

bool EnsureDirRecursive(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' && cur.size() > 1) {
            ::mkdir(cur.c_str(), 0755);
        }
    }
    ::mkdir(path.c_str(), 0755);
    return ::access(path.c_str(), F_OK) == 0;
}

void CleanupOldLocked() {
    if (gDir.empty()) return;
    time_t cutoffTime = time(nullptr) - static_cast<time_t>(kRetentionDays) * 86400;
    struct tm ltm;
    localtime_r(&cutoffTime, &ltm);
    char cutoff[16] = {};
    snprintf(cutoff, sizeof(cutoff), "%04d%02d%02d",
             ltm.tm_year + 1900, ltm.tm_mon + 1, ltm.tm_mday);

    DIR* d = opendir(gDir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const char* n = e->d_name;
        if (strncmp(n, "native-", 7) == 0 && strstr(n, ".log") != nullptr &&
            strlen(n) >= 18) {
            char dateStr[16] = {};
            memcpy(dateStr, n + 7, 8);
            dateStr[8] = '\0';
            if (strcmp(dateStr, cutoff) < 0) {
                ::unlink((gDir + "/" + n).c_str());
            }
        }
    }
    closedir(d);
}

void WriteLineLocked(const std::string& line) {
    if (gDir.empty()) {
        gPending.push_back(line);
        if (gPending.size() > kPreInitRingLines) gPending.pop_front();
        return;
    }
    const std::string today = TodayKey();
    if (today != gDateKey) {
        gDateKey = today;
        gSeq = 0;
        OpenCurrentLocked();
    } else if (gFd < 0) {
        OpenCurrentLocked();
    }
    if (gFd >= 0 && gBytes > 0 && gBytes + line.size() > kMaxFileBytes) {
        gSeq += 1;
        OpenCurrentLocked();
        CleanupOldLocked();
    }
    if (gFd >= 0) {
        ssize_t w = ::write(gFd, line.data(), line.size());
        if (w > 0) gBytes += static_cast<size_t>(w);
    }
}

/* Converts a hilog format string into a printf-compatible one.
 * %{public}x keeps the specifier (argument consumed), %{private}x becomes the
 * literal "<private>" (argument skipped, matching hilog system output). */
std::string ConvertFormat(const char* fmt) {
    std::string out;
    for (const char* p = fmt; *p; ++p) {
        if (*p == '%' && p[1] == '{') {
            const char* close = strchr(p + 2, '}');
            if (close != nullptr) {
                const size_t specLen = static_cast<size_t>(close - (p + 2));
                const bool isPrivate = specLen >= 7 &&
                    strncmp(p + 2, "private", 7) == 0;
                if (isPrivate) {
                    out += "<private>";
                } else {
                    out += '%';
                }
                p = close;
                continue;
            }
        }
        out += *p;
    }
    return out;
}

const char* LevelChar(int level) {
    switch (level) {
        case LOG_DEBUG: return "D";
        case LOG_WARN: return "W";
        case LOG_ERROR: return "E";
        case LOG_FATAL: return "F";
        case LOG_INFO:
        default: return "I";
    }
}

}  // namespace

void WineHuaLogAppend(int level, const char* tag, const char* fmt, ...) {
    char msg[kMaxLineBytes] = {};
    if (fmt != nullptr) {
        const std::string conv = ConvertFormat(fmt);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), conv.c_str(), ap);
        va_end(ap);
        msg[sizeof(msg) - 1] = '\0';
    }

    char stamp[40] = {};
    StampInto(stamp, sizeof(stamp));
    const char* tagSafe = tag != nullptr ? tag : "(null)";
    char lineBuf[kMaxLineBytes + 96] = {};
    snprintf(lineBuf, sizeof(lineBuf), "%s [%s] [%s] %s\n",
             stamp, LevelChar(level), tagSafe, msg);

    std::lock_guard<std::mutex> lock(gMutex);
    WriteLineLocked(lineBuf);
}

void WineHuaLogInit(const char* dirPath) {
    if (dirPath == nullptr || dirPath[0] == '\0') return;
    std::lock_guard<std::mutex> lock(gMutex);
    if (!EnsureDirRecursive(dirPath)) return;
    gDir = dirPath;
    gDateKey = TodayKey();
    gSeq = 0;
    OpenCurrentLocked();
    for (const auto& line : gPending) {
        WriteLineLocked(line);
    }
    gPending.clear();
    CleanupOldLocked();
}

void WineHuaLogDirectory(char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) return;
    std::lock_guard<std::mutex> lock(gMutex);
    if (gDir.empty()) {
        out[0] = '\0';
        return;
    }
    snprintf(out, outSize, "%s", gDir.c_str());
}

void WineHuaLogClear() {
    std::lock_guard<std::mutex> lock(gMutex);
    CloseFdLocked();
    if (!gDir.empty()) {
        DIR* d = opendir(gDir.c_str());
        if (d != nullptr) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                const char* n = e->d_name;
                if (strncmp(n, "native-", 7) == 0 && strstr(n, ".log") != nullptr) {
                    ::unlink((gDir + "/" + n).c_str());
                }
            }
            closedir(d);
        }
    }
    gDateKey.clear();
    gSeq = 0;
    gBytes = 0;
    gPending.clear();
}

/**
 * cef_utility_probe.cpp — CEF utility 子进程生命周期观测 (只诊断, 不改行为)
 *
 * 详见 cef_utility_probe.h。核心是两件事:
 *   1. 创建侧把 "谁创建了哪个 CEF 子进程" 记下来 (含父进程本机 pid、subtype、HODLL);
 *   2. 退出侧用系统 NCP 退出回调配对, 算出 lifetimeMs 与 signal, 并累计 subtype 计数。
 * 这样一来 "NetworkService 389 次 / StorageService 1384 次" 就从孤立计数变成
 * "每个 subtype 的存活时长分布 + 退出方式分布", 才能区分:
 *   启动→几十毫秒→exit0 (Mojo/handshake 不满足)   vs
 *   启动→几十毫秒→SIGSEGV/SIGILL (ARM64EC/FEX 翻译故障)。
 */
#include "cef_utility_probe.h"
#include "wine/wine_constants.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "WL_CEFUTIL"
#include <hilog/log.h>

namespace {

// 单个已创建、尚未退出的 CEF 子进程。
struct UtilityEntry {
    int32_t parentHostPid = -1;
    std::string key;          // "browser" / "network.mojom.NetworkService" / "gpu-process" / ...
    std::string type;         // --type= 的值 ("utility" / "gpu-process" / "renderer" / "browser")
    std::string subtype;      // --utility-sub-type= 的值 (无则空)
    std::string sandboxType;  // --service-sandbox-type=
    std::string cefDir;       // cef.win64 / cef.win7 —— x64 与 32 位对照的关键字段
    std::string hodll;        // __env=HODLL=   (WOW64 侧翻译器)
    std::string hodll64;      // __env=HODLL64= (ARM64EC 侧翻译器)
    std::string image;        // steamwebhelper.exe 完整路径
    std::string mojoHandle;   // --mojo-platform-channel-handle=
    int64_t createMs = 0;     // 绝对时间 (system_clock), 便于与 Chromium 日志对齐
};

std::mutex gMutex;
std::map<int32_t, UtilityEntry> gLive;   // childHostPid -> entry
std::map<std::string, int> gSpawnCount;  // key -> 创建次数 (restart loop 规模)
std::map<std::string, int> gExitCount;   // key -> 已配对退出的次数

int gFd = -2;                 // -2 未初始化, -1 不可用/关闭
bool gEnabled = true;

int64_t NowMs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ContainsCI(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] && ((p[i] | 0x20) == (needle[i] | 0x20))) ++i;
        if (i == nl) return true;
    }
    return false;
}

// entryParams 是 '|' 分隔的 token 串: homeDir|binDir|[wine]|argv...|__env=K=V|...
// 返回以 prefix 开头的那一段 (去掉 prefix); 没有则空串。只认整段前缀匹配,
// 避免 "…x--type=utility" 这类粘连误判。
std::string TokenValue(const std::string &params, const char *prefix) {
    size_t pl = strlen(prefix);
    size_t pos = 0;
    while (pos <= params.size()) {
        size_t end = params.find('|', pos);
        if (end == std::string::npos) end = params.size();
        const std::string seg = params.substr(pos, end - pos);
        if (seg.compare(0, pl, prefix) == 0) return seg.substr(pl);
        if (end == params.size()) break;
        pos = end + 1;
    }
    return std::string();
}

bool HasToken(const std::string &params, const char *prefix) {
    size_t pl = strlen(prefix);
    size_t pos = 0;
    while (pos <= params.size()) {
        size_t end = params.find('|', pos);
        if (end == std::string::npos) end = params.size();
        if (params.compare(pos, pl, prefix) == 0) return true;
        if (end == params.size()) break;
        pos = end + 1;
    }
    return false;
}

std::string FindTokenContaining(const std::string &params, const char *needle) {
    size_t pos = 0;
    while (pos <= params.size()) {
        size_t end = params.find('|', pos);
        if (end == std::string::npos) end = params.size();
        std::string seg = params.substr(pos, end - pos);
        if (ContainsCI(seg.c_str(), needle)) return seg;
        if (end == params.size()) break;
        pos = end + 1;
    }
    return std::string();
}

// 从 "...\Steam\bin\cef\cef.win64\steamwebhelper.exe" 取 "cef.win64"。
// 这是 x64 客户端与 32 位客户端在同一份日志里最直接的区分字段。
std::string CefDirOf(const std::string &image) {
    size_t at = image.find("\\cef\\");
    if (at == std::string::npos) at = image.find("/cef/");
    if (at == std::string::npos) return std::string();
    size_t start = at + 5;
    size_t end = image.find_first_of("\\/", start);
    if (end == std::string::npos) end = image.size();
    return image.substr(start, end - start);
}

// 调用方必须持有 gMutex。
void EnsureOpenLocked() {
    if (gFd != -2) return;
    const char *opt = getenv("WINEHUA_CEF_UTILITY_PROBE");
    gEnabled = !(opt && opt[0] == '0');
    if (!gEnabled) {
        gFd = -1;
        OH_LOG_INFO(LOG_APP, "[CEF-UTILITY] probe disabled (WINEHUA_CEF_UTILITY_PROBE=0)");
        return;
    }
    mkdir(WINE_LOG_DIR, 0755);
    const std::string path = std::string(WINE_LOG_DIR) + "/cef-utility-diag.log";
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 4 * 1024 * 1024) {
        const std::string rot = path + ".1";
        unlink(rot.c_str());
        rename(path.c_str(), rot.c_str());
    }
    gFd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (gFd >= 0) {
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "=== CEF-UTILITY probe session start brokerPid=%d t=%lld ===\n",
                         (int)getpid(), (long long)NowMs());
        if (n > 0) {
            ssize_t ignored = write(gFd, hdr, (size_t)n);
            (void)ignored;
        }
    } else {
        OH_LOG_WARN(LOG_APP, "[CEF-UTILITY] probe log open failed errno=%{public}d", errno);
    }
}

// 调用方必须持有 gMutex。
void EmitLocked(const char *line) {
    if (gFd >= 0) {
        size_t len = strlen(line);
        ssize_t n = write(gFd, line, len);
        (void)n;
    }
    OH_LOG_INFO(LOG_APP, "%{public}s", line);
}

}  // namespace

bool WineHuaCefUtilityProbeEnabled() {
    std::lock_guard<std::mutex> lock(gMutex);
    EnsureOpenLocked();
    return gEnabled;
}

void WineHuaCefUtilityProbeNoteSpawn(int32_t childPid, int32_t parentHostPid,
                                     const char *entryParams) {
    if (childPid <= 0 || !entryParams || !*entryParams) return;
    // 只关心 CEF 与 Steam 客户端服务: 其余进程一个字节都不落盘。
    // steamservice.exe 是 Steam 的客户端服务 (/RunAsService), 观测轮实测它每
    // 1.5s 上下就被重新拉起一次 —— 与 CEF 无关但同属 "客户端子进程不断重启" 家族,
    // 顺手一起观测 (同一份 create/exit 记录即可算出它的 lifetime/signal 分布)。
    const bool isWebHelper = ContainsCI(entryParams, "steamwebhelper");
    const bool isSteamService = ContainsCI(entryParams, "steamservice");
    if (!isWebHelper && !isSteamService) return;

    const std::string params(entryParams);
    UtilityEntry e;
    e.createMs = NowMs();
    e.parentHostPid = parentHostPid;
    e.image = FindTokenContaining(params, "steamwebhelper");
    e.type = TokenValue(params, "--type=");
    e.subtype = TokenValue(params, "--utility-sub-type=");
    e.sandboxType = TokenValue(params, "--service-sandbox-type=");
    e.mojoHandle = TokenValue(params, "--mojo-platform-channel-handle=");
    e.hodll = TokenValue(params, "__env=HODLL=");
    e.hodll64 = TokenValue(params, "__env=HODLL64=");
    e.cefDir = CefDirOf(e.image);

    // 主 steamwebhelper (CEF browser 进程) 没有 --type=, 但它正是所有 utility 的父进程,
    // 必须一起记录, 否则 parentHostPid 指向的 pid 在日志里查不到身份。
    if (!isWebHelper) e.type = e.type.empty() ? "steamservice" : e.type;
    else if (e.type.empty() && e.subtype.empty()) e.type = "browser";
    if (!isWebHelper) e.key = "steamservice";
    else if (!e.subtype.empty()) e.key = e.subtype;
    else if (!e.type.empty()) e.key = e.type;
    else e.key = "unknown";

    std::lock_guard<std::mutex> lock(gMutex);
    EnsureOpenLocked();
    if (!gEnabled) return;

    gLive[childPid] = e;
    int spawned = ++gSpawnCount[e.key];

    char line[1024];
    int n = snprintf(line, sizeof(line),
                     "CEF-UTILITY-CREATE brokerPid=%d childHostPid=%d parentHostPid=%d "
                     "key=%s type=%s subtype=%s cefDir=%s sandbox=%s "
                     "HODLL=%s HODLL64=%s mojo=%s t=%lld image=\"%s\"\n",
                     (int)getpid(), (int)childPid, (int)parentHostPid,
                     e.key.c_str(), e.type.c_str(),
                     e.subtype.empty() ? "-" : e.subtype.c_str(),
                     e.cefDir.empty() ? "-" : e.cefDir.c_str(),
                     e.sandboxType.empty() ? "-" : e.sandboxType.c_str(),
                     e.hodll.empty() ? "-" : e.hodll.c_str(),
                     e.hodll64.empty() ? "-" : e.hodll64.c_str(),
                     e.mojoHandle.empty() ? "-" : e.mojoHandle.c_str(),
                     (long long)e.createMs, e.image.c_str());
    if (n > 0) EmitLocked(line);

    // 每 25 次给一条累计行: restart loop 的规模不用等分析脚本就能从日志直接看出来。
    if (spawned % 25 == 0) {
        char stat[256];
        int m = snprintf(stat, sizeof(stat),
                         "CEF-UTILITY-COUNT key=%s spawned=%d exited=%d t=%lld\n",
                         e.key.c_str(), spawned, gExitCount[e.key], (long long)NowMs());
        if (m > 0) EmitLocked(stat);
    }
}

void WineHuaCefUtilityProbeNoteExit(int32_t childPid, int32_t signal) {
    if (childPid <= 0) return;
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gLive.find(childPid);
    if (it == gLive.end()) return;   // 不是 CEF 子进程, 或已配对过 (幂等)
    const UtilityEntry e = it->second;
    gLive.erase(it);

    const int64_t now = NowMs();
    const int64_t lifetime = now - e.createMs;
    const int exited = ++gExitCount[e.key];

    const char *sigName = (signal > 0) ? strsignal(signal) : "-";
    char line[768];
    int n = snprintf(line, sizeof(line),
                     "CEF-UTILITY-EXIT childHostPid=%d parentHostPid=%d key=%s type=%s "
                     "subtype=%s cefDir=%s lifetimeMs=%lld signal=%d(0x%x=%s) "
                     "createMs=%lld exitMs=%lld\n",
                     (int)childPid, (int)e.parentHostPid, e.key.c_str(), e.type.c_str(),
                     e.subtype.empty() ? "-" : e.subtype.c_str(),
                     e.cefDir.empty() ? "-" : e.cefDir.c_str(),
                     (long long)lifetime, (int)signal, (unsigned)(signal >= 0 ? signal : 0),
                     sigName, (long long)e.createMs, (long long)now);
    if (n > 0) EmitLocked(line);

    if (exited % 25 == 0) {
        char stat[256];
        int m = snprintf(stat, sizeof(stat),
                         "CEF-UTILITY-COUNT key=%s spawned=%d exited=%d t=%lld\n",
                         e.key.c_str(), gSpawnCount[e.key], exited, (long long)now);
        if (m > 0) EmitLocked(stat);
    }
}

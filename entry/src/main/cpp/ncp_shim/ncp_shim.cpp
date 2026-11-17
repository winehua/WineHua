/*
 * ncp_shim.cpp — fork 版 native_child_process 实现
 *
 * 鸿蒙 phone/TV 平台限制 OH_Ability_*NativeChildProcess*（仅 2in1 可用），
 * 本文件提供可选的 fork 后端：
 *   - OH_Ability_StartNativeChildProcess: fork + dlopen + dlsym(entry) + entry(args)
 *
 * 与官方 NCP 的语义差异处理：
 *   1. fork 子进程继承 Ark 主进程低 4GB 映射 → child 里 UnmapLowAnonRegions()
 *   2. NCP 的 fd 所有权转移 → fork 后 parent 显式 close，防泄漏/EOF 语义错乱
 *   3. NCP 子进程由 appspawn 收尸 → 安装 SIGCHLD reaper 防僵尸
 *   4. NCP 同步返回 so 加载结果 → 握手 pipe 模拟同步错误语义
 *
 * VirGL 不走此 shim。phone/TV 在应用进程内运行 VirGL host，从而继续直接使用
 * OHNativeWindow/NativeBuffer；tablet/2in1/PC 仍使用系统 NCP + Binder。
 */
#include "native_child_process.h"

#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "NCP_Shim"
#include <hilog/log.h>

namespace {

std::atomic<bool> gForkBackendEnabled{false};

// ---- 僵尸回收：NCP 由 appspawn 收尸，fork 后主进程必须自己 reap ----
void InstallReaperOnce() {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] {
        struct sigaction sa{};
        sa.sa_handler = [](int) {
            int saved = errno;
            while (waitpid(-1, nullptr, WNOHANG) > 0) {}
            errno = saved;
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, nullptr);
    });
}

// ---- 关闭除 keep 外的所有继承 fd ----
void CloseAllFdsExcept(const std::vector<int>& keep) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return;
    int dfd = dirfd(d);
    struct dirent* e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (fd <= 2 || fd == dfd) continue;
        bool k = false;
        for (int f : keep) {
            if (f == fd) { k = true; break; }
        }
        if (!k) close(fd);
    }
    closedir(d);
}

// ---- 释放继承自 Ark 主进程的低 4GB anon/ark 映射 ----
void UnmapLowAnonRegions() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;

    struct Region { unsigned long start, end; };
    std::vector<Region> targets;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long start = 0, end = 0;
        char prot[8] = {0};
        char tag[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]",
                       &start, &end, prot, tag);
        if (n < 3) continue;
        if (start >= 0x100000000UL) continue;
        bool isArk      = strstr(tag, "[anon:ark") != nullptr;
        bool isPureAnon = (n < 4) || (tag[0] == 0);
        if (isArk || isPureAnon) targets.push_back({start, end});
    }
    fclose(f);

    for (auto& r : targets) {
        munmap((void*)r.start, r.end - r.start);
    }
}

// ---- 握手 pipe：child 解析出入口函数后写 1 字节；parent 同步等待 ----
constexpr int kHandshakeTimeoutMs = 10000;

void ChildHandshakeOk(int wfd) {
    uint8_t b = 1;
    ssize_t unused = write(wfd, &b, 1);
    (void)unused;
    close(wfd);
}

bool ParentWaitHandshake(int rfd) {
    struct pollfd pfd{rfd, POLLIN, 0};
    int pr;
    do { pr = poll(&pfd, 1, kHandshakeTimeoutMs); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) { close(rfd); return false; }
    uint8_t b;
    bool ok = (read(rfd, &b, 1) == 1 && b == 1);
    close(rfd);
    return ok;
}

void* DlopenWithFallback(const std::string& so) {
    void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&DlopenWithFallback), &info) &&
            info.dli_fname && info.dli_fname[0]) {
            std::string dir(info.dli_fname);
            const size_t slash = dir.find_last_of('/');
            if (slash != std::string::npos) {
                h = dlopen((dir.substr(0, slash + 1) + so).c_str(), RTLD_NOW | RTLD_GLOBAL);
            }
        }
    }
    return h;
}

// ---- Start 版 child：复刻官方伪代码 dlopen → dlsym(func) → func(args) ----
[[noreturn]] void StartChildMain(std::string so, std::string func,
                                 NativeChildProcess_Args args, int handshakeWfd) {
    for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);

    std::vector<int> keep{handshakeWfd};
    for (auto* p = args.fdList.head; p; p = p->next) keep.push_back(p->fd);
    CloseAllFdsExcept(keep);
    UnmapLowAnonRegions();
    prctl(PR_SET_NAME, func.substr(0, 15).c_str(), 0, 0, 0);

    void* h = DlopenWithFallback(so);
    if (!h) {
        fprintf(stderr, "[ncp_shim] dlopen %s failed: %s\n", so.c_str(), dlerror());
        _exit(125);
    }
    using EntryFn = void (*)(NativeChildProcess_Args);
    auto fn = (EntryFn)dlsym(h, func.c_str());
    if (!fn) {
        fprintf(stderr, "[ncp_shim] dlsym %s failed\n", func.c_str());
        _exit(126);
    }

    ChildHandshakeOk(handshakeWfd);
    fn(args);
    _exit(0);
}

} // namespace

static Ability_NativeChildProcess_ErrCode ForkStartNativeChildProcess(
    const char* entry, NativeChildProcess_Args args,
    NativeChildProcess_Options /* options 忽略：fork 天然同域 = NORMAL */, int32_t* pid)
{
    if (!entry || !pid) return NCP_ERR_INVALID_PARAM;
    std::string e(entry);
    auto pos = e.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= e.size()) {
        return NCP_ERR_INVALID_PARAM;
    }

    InstallReaperOnce();
    int hs[2];
    if (pipe(hs) != 0) return NCP_ERR_INTERNAL;

    pid_t child = fork();
    if (child < 0) {
        close(hs[0]);
        close(hs[1]);
        return NCP_ERR_INTERNAL;
    }
    if (child == 0) {
        close(hs[0]);
        StartChildMain(e.substr(0, pos), e.substr(pos + 1), args, hs[1]);  // 不返回
    }

    // ---- parent ----
    close(hs[1]);
    bool ok = ParentWaitHandshake(hs[0]);
    // NCP "fd 所有权转移"在 fork 下要显式实现：关闭 parent 侧拷贝
    for (auto* p = args.fdList.head; p; p = p->next) close(p->fd);
    if (!ok) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        *pid = -1;
        return NCP_ERR_LIB_LOADING_FAILED;
    }
    *pid = child;
    return NCP_NO_ERROR;
}

namespace winehua::ncp {

void SetForkBackendEnabled(bool enabled)
{
    gForkBackendEnabled.store(enabled, std::memory_order_release);
    OH_LOG_INFO(LOG_APP, "[ncp] backend=%{public}s", enabled ? "fork" : "system");
}

bool UsesForkBackend()
{
    return gForkBackendEnabled.load(std::memory_order_acquire);
}

Ability_NativeChildProcess_ErrCode StartNativeChildProcess(
    const char* entry, NativeChildProcess_Args args,
    NativeChildProcess_Options options, int32_t* pid)
{
    if (!UsesForkBackend()) {
        return OH_Ability_StartNativeChildProcess(entry, args, options, pid);
    }
    return ForkStartNativeChildProcess(entry, args, options, pid);
}

} // namespace winehua::ncp

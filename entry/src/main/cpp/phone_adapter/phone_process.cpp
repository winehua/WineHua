/*
 * phone_process.cpp — 手机适配层：fork 进程创建实现
 *
 * 鸿蒙手机上 OH_Ability_*NativeChildProcess* 不可用（仅 2in1 支持），
 * 本文件提供 fork 版替代实现。
 * 路由逻辑在 ncp_dispatch.cpp，virgl relay/dispatch 在 phone_virgl_relay.cpp/dispatch.cpp。
 */
#include "phone_process.h"
#include "phone_adapter.h"                     // PHONE_ADAPTER_DUMMY_PROXY
#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>

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
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "PhoneAdapt"
#include <hilog/log.h>

namespace {

// ---- 僵尸回收：NCP 由 appspawn 收尸，fork 后主进程必须自己 reap ----
void InstallReaperOnce() {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] {
        struct sigaction existing{};
        if (sigaction(SIGCHLD, nullptr, &existing) != 0 ||
            (existing.sa_flags & SA_SIGINFO) || existing.sa_handler != SIG_DFL) {
            // The Wine process registry may already own SIGCHLD so it can
            // preserve a child exit status for automation evidence.
            return;
        }
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
        std::string abs = "/data/storage/el1/bundle/libs/arm64/" + so;
        h = dlopen(abs.c_str(), RTLD_NOW | RTLD_GLOBAL);
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
        fprintf(stderr, "[PhoneAdapt] dlopen %s failed: %s\n", so.c_str(), dlerror());
        _exit(125);
    }
    using EntryFn = void (*)(NativeChildProcess_Args);
    auto fn = (EntryFn)dlsym(h, func.c_str());
    if (!fn) {
        fprintf(stderr, "[PhoneAdapt] dlsym %s failed\n", func.c_str());
        _exit(126);
    }

    ChildHandshakeOk(handshakeWfd);
    fn(args);
    _exit(0);
}

// ---- Create 版 child：dlopen + NativeChildProcess_MainProc ----
// 跳过 OnConnect（Binder stub 无人连接）；配置 socket fd 经环境变量传给 MainProc
[[noreturn]] void CreateChildMain(std::string so, int handshakeWfd, int cfgFd) {
    for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
    CloseAllFdsExcept({handshakeWfd, cfgFd});
    UnmapLowAnonRegions();
    prctl(PR_SET_NAME, so.substr(0, 15).c_str(), 0, 0, 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", cfgFd);
    setenv("WINEHUA_PHONE_CFG_FD", buf, 1);

    void* h = DlopenWithFallback(so);
    if (!h) {
        fprintf(stderr, "[PhoneAdapt] dlopen %s failed: %s\n", so.c_str(), dlerror());
        _exit(125);
    }
    using MainProcFn = void (*)();
    auto fn = (MainProcFn)dlsym(h, "NativeChildProcess_MainProc");
    if (!fn) {
        fprintf(stderr, "[PhoneAdapt] dlsym NativeChildProcess_MainProc failed\n");
        _exit(126);
    }

    ChildHandshakeOk(handshakeWfd);
    fn();
    _exit(0);
}

int g_cfgSockParent = -1;   // virgl server 同时只有一个

} // namespace

// ====== fork 进程创建实现 ======

Ability_NativeChildProcess_ErrCode Phone_StartNativeChildProcess(
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
        StartChildMain(e.substr(0, pos), e.substr(pos + 1), args, hs[1]);
    }

    close(hs[1]);
    bool ok = ParentWaitHandshake(hs[0]);
    for (auto* p = args.fdList.head; p; p = p->next) close(p->fd);
    if (!ok) { *pid = -1; return NCP_ERR_LIB_LOADING_FAILED; }
    *pid = child;
    return NCP_NO_ERROR;
}

int Phone_CreateNativeChildProcess(
    const char* libName, OH_Ability_OnNativeChildProcessStarted onProcessStarted)
{
    if (!libName || !onProcessStarted) return NCP_ERR_INVALID_PARAM;
    InstallReaperOnce();

    int hs[2], cfg[2];
    if (pipe(hs) != 0) return NCP_ERR_INTERNAL;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, cfg) != 0) {
        close(hs[0]); close(hs[1]);
        return NCP_ERR_INTERNAL;
    }

    pid_t child = fork();
    if (child < 0) {
        close(hs[0]); close(hs[1]); close(cfg[0]); close(cfg[1]);
        return NCP_ERR_INTERNAL;
    }
    if (child == 0) {
        close(hs[0]); close(cfg[0]);
        CreateChildMain(libName, hs[1], cfg[1]);
    }

    close(hs[1]); close(cfg[1]);
    bool ok = ParentWaitHandshake(hs[0]);
    if (ok) {
        if (g_cfgSockParent >= 0) close(g_cfgSockParent);
        g_cfgSockParent = cfg[0];
    } else {
        close(cfg[0]);
    }
    int err = ok ? NCP_NO_ERROR : NCP_ERR_LIB_LOADING_FAILED;
    std::thread([onProcessStarted, err] {
        onProcessStarted(err, err == NCP_NO_ERROR
            ? (OHIPCRemoteProxy*)PHONE_ADAPTER_DUMMY_PROXY : nullptr);
    }).detach();
    return NCP_NO_ERROR;
}

// ====== Proxy 查询接口 ======

bool PhoneAdapter_IsDummyProxy(const void* p) {
    return p == (const void*)PHONE_ADAPTER_DUMMY_PROXY;
}
int PhoneAdapter_GetConfigSocket() { return g_cfgSockParent; }
void PhoneAdapter_CloseConfigSocket() {
    if (g_cfgSockParent >= 0) { close(g_cfgSockParent); g_cfgSockParent = -1; }
}

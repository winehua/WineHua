/*
 * phone_process.cpp — 手机适配层：fork 进程创建实现
 *
 * 鸿蒙手机上 OH_Ability_*NativeChildProcess* 不可用（仅 2in1 支持），
 * 本文件提供 fork 版替代实现。
 * 路由逻辑在 ncp_dispatch.cpp，virgl relay/dispatch 在 phone_virgl_relay.cpp/dispatch.cpp。
 */
#include "phone_process.h"
#include "phone_adapter.h"                     // PHONE_ADAPTER_DUMMY_PROXY
#include "proc/wine_process.h"
#include <AbilityKit/native_child_process.h>
#include <IPCKit/ipc_kit.h>

#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#include <mutex>
#include <thread>
#include <unordered_map>
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
                                 NativeChildProcess_Args args, int handshakeWfd,
                                 bool cleanServerChild = false) {
    int traceFd = -1;
    for (auto* node = args.fdList.head; node; node = node->next)
        if (node->fdName && strcmp(node->fdName, "direct_probe_stage") == 0) traceFd = node->fd;
    const auto trace = [traceFd](char stage) {
        if (traceFd >= 0) (void)write(traceFd, &stage, 1);
    };
    trace('1');
    if (!cleanServerChild) {
        for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
        std::vector<int> keep{handshakeWfd};
        for (auto* p = args.fdList.head; p; p = p->next) keep.push_back(p->fd);
        CloseAllFdsExcept(keep);
    }
    trace('2');
    // Isolated native P0 A/B: distinguish SDK state removed by the Wine low-VA
    // cleanup from registration/IPC restrictions. Wine and all other probes
    // retain their existing address-space cleanup.
    const bool preserveProbeMappings = cleanServerChild && so == "libdirect_shared_buffer_probe.so" &&
        args.entryParams && (strcmp(args.entryParams, "shared-buffer-p0-preserve-maps") == 0 ||
                            strcmp(args.entryParams, "shared-buffer-p0-standard-fork") == 0);
    if (!preserveProbeMappings) UnmapLowAnonRegions();
    trace('3');
    prctl(PR_SET_NAME, func.substr(0, 15).c_str(), 0, 0, 0);

    void* h = DlopenWithFallback(so);
    trace('4');
    if (!h) {
        fprintf(stderr, "[PhoneAdapt] dlopen %s failed: %s\n", so.c_str(), dlerror());
        _exit(125);
    }
    using EntryFn = void (*)(NativeChildProcess_Args);
    auto fn = (EntryFn)dlsym(h, func.c_str());
    trace('5');
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

// A phone Direct child must not inherit ArkUI's initialized GPU driver. This
// opt-in server is forked before the page loads and never initializes graphics.
// Only spawn control data and named descriptors cross these sockets.
constexpr uint32_t kForkMagic = 0x57484653;
constexpr size_t kForkMaxFds = 16;
struct ForkRequest {
    uint32_t magic = kForkMagic;
    uint32_t fdCount = 0;
    char entry[128]{};
    char params[16385]{};
    char names[kForkMaxFds][21]{};
};
struct ForkReply {
    uint32_t magic = kForkMagic;
    int32_t pid = -1;
    int32_t code = NCP_ERR_INTERNAL;
};
struct ForkExit {
    uint32_t magic = kForkMagic;
    int32_t pid = -1;
    int32_t status = 0;
};
std::mutex g_forkMutex;
int g_forkControl = -1;
int32_t g_forkPid = -1;
std::mutex g_forkExitMutex;
struct ForkChildRecord {
    int status = 0;
    bool exited = false;
    bool registered = false;
    bool reported = false;
};
std::unordered_map<int32_t, ForkChildRecord> g_forkChildren;

void SetSocketTimeout(int fd, int seconds) {
    timeval value{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
}

bool SendForkRequest(int fd, const ForkRequest& request, const std::vector<int>& fds) {
    union { char bytes[CMSG_SPACE(sizeof(int) * kForkMaxFds)]; cmsghdr align; } control{};
    iovec data{const_cast<ForkRequest*>(&request), sizeof(request)};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    if (!fds.empty()) {
        message.msg_control = control.bytes;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        memcpy(CMSG_DATA(header), fds.data(), sizeof(int) * fds.size());
    }
    ssize_t sent;
    do { sent = sendmsg(fd, &message, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
    return sent == sizeof(request);
}

bool ReceiveForkRequest(int fd, ForkRequest& request, std::vector<int>& fds) {
    union { char bytes[CMSG_SPACE(sizeof(int) * kForkMaxFds)]; cmsghdr align; } control{};
    iovec data{&request, sizeof(request)};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    ssize_t received;
    do { received = recvmsg(fd, &message, MSG_CMSG_CLOEXEC); }
    while (received < 0 && errno == EINTR);
    for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0)) continue;
        const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const auto* items = reinterpret_cast<const int*>(CMSG_DATA(header));
        fds.insert(fds.end(), items, items + count);
    }
    if (received != sizeof(request) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        request.magic != kForkMagic || request.fdCount != fds.size() ||
        request.fdCount > kForkMaxFds || !memchr(request.entry, 0, sizeof(request.entry)) ||
        !memchr(request.params, 0, sizeof(request.params))) return false;
    for (uint32_t i = 0; i < request.fdCount; ++i)
        if (!request.names[i][0] || !memchr(request.names[i], 0, sizeof(request.names[i])))
            return false;
    return true;
}

using ForkChildFn = pid_t (*)();
[[noreturn]] void DirectForkServerMain(int controlFd, int exitFd, int readyFd,
                                     pid_t parent, ForkChildFn forkChild) {
    for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(127);
    prctl(PR_SET_NAME, "WineDirectFork", 0, 0, 0);
    std::vector<int> runtimeFds{controlFd, exitFd, readyFd};
    DIR* fdDir = opendir("/proc/self/fd");
    if (!fdDir) _exit(127);
    {
        while (auto* item = readdir(fdDir)) {
            if (item->d_name[0] == '.') continue;
            char path[128], target[512];
            snprintf(path, sizeof(path), "/proc/self/fd/%s", item->d_name);
            const ssize_t size = readlink(path, target, sizeof(target) - 1);
            if (size >= 0) {
                target[size] = 0;
                // System tracing libraries cache these fd numbers across
                // fork. Closing them lets Wine reuse a number, so later GPU
                // trace writes can corrupt its wineserver protocol socket.
                if (strcmp(target, "/sys/kernel/debug/tracing/trace_marker") == 0 ||
                    strcmp(target, "/sys/kernel/tracing/trace_marker") == 0)
                    runtimeFds.push_back(atoi(item->d_name));
            }
        }
    }
    closedir(fdDir);
    CloseAllFdsExcept(runtimeFds);
    SetSocketTimeout(exitFd, 2);
    ChildHandshakeOk(readyFd);
    std::vector<pid_t> children;
    bool running = true;
    while (running) {
        int status = 0;
        pid_t ended;
        while ((ended = waitpid(-1, &status, WNOHANG)) > 0) {
            for (auto it = children.begin(); it != children.end(); ++it) {
                if (*it == ended) { children.erase(it); break; }
            }
            ForkExit event{kForkMagic, ended, status};
            if (send(exitFd, &event, sizeof(event), MSG_NOSIGNAL) != sizeof(event)) {
                running = false;
                break;
            }
        }
        if (!running) break;
        pollfd input{controlFd, POLLIN, 0};
        int ready = poll(&input, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (input.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
        if (!ready || !(input.revents & POLLIN)) continue;
        ForkRequest request{};
        std::vector<int> fds;
        const bool valid = ReceiveForkRequest(controlFd, request, fds);
        ForkReply reply{};
        const std::string entry = valid ? request.entry : "";
        const size_t colon = entry.find(':');
        if (valid && children.size() < 64 && colon != std::string::npos &&
            colon > 0 && colon + 1 < entry.size()) {
            const bool standardProbeFork = entry == "libdirect_shared_buffer_probe.so:Main" &&
                strcmp(request.params, "shared-buffer-p0-standard-fork") == 0;
            auto promoteFd = [](int& fd) {
                const int higher = fcntl(fd, F_DUPFD_CLOEXEC, 256);
                if (higher < 0) return false;
                close(fd);
                fd = higher;
                return true;
            };
            bool bootstrapFdsReady = true;
            // P0 normal-fork A/B only: don't reuse cached App fd numbers in
            // bootstrap/handshake channels before SDK atfork hooks run.
            if (standardProbeFork)
                for (auto& fd : fds) if (!promoteFd(fd)) bootstrapFdsReady = false;
            NativeChildProcess_Fd nodes[kForkMaxFds]{};
            for (size_t i = 0; i < fds.size(); ++i) {
                nodes[i].fdName = request.names[i];
                nodes[i].fd = fds[i];
                nodes[i].next = i + 1 < fds.size() ? &nodes[i + 1] : nullptr;
            }
            NativeChildProcess_Args args{};
            args.entryParams = request.params;
            args.fdList.head = fds.empty() ? nullptr : nodes;
            int handshake[2];
            if (pipe(handshake) == 0) {
                if (standardProbeFork && (!promoteFd(handshake[0]) || !promoteFd(handshake[1])))
                    bootstrapFdsReady = false;
                if (!bootstrapFdsReady) {
                    close(handshake[0]);
                    close(handshake[1]);
                    reply.code = NCP_ERR_INTERNAL;
                } else {
                    const pid_t serverPid = getpid();
                    // The server is single-threaded. _Fork performs libc's child
                    // reset without replaying ArkUI/App pthread_atfork callbacks
                    // inherited by the first fork.
                    pid_t child = standardProbeFork ? fork() : forkChild();
                    if (child == 0) {
                        prctl(PR_SET_PDEATHSIG, SIGKILL);
                        if (getppid() != serverPid) _exit(127);
                        close(handshake[0]);
                        // The single-threaded server already reset signals and
                        // removed App fds while preserving cached tracing fds.
                        // Close the server control sockets before running Wine.
                        close(controlFd);
                        close(exitFd);
                        StartChildMain(entry.substr(0, colon), entry.substr(colon + 1), args, handshake[1], true);
                    }
                    close(handshake[1]);
                    if (child > 0) {
                        children.push_back(child);
                        reply.pid = child;
                        if (ParentWaitHandshake(handshake[0])) {
                            reply.code = NCP_NO_ERROR;
                        } else {
                            kill(child, SIGKILL);
                            reply.code = NCP_ERR_LIB_LOADING_FAILED;
                        }
                    } else close(handshake[0]);
                }
            }
        } else reply.code = NCP_ERR_INVALID_PARAM;
        for (int fd : fds) close(fd);
        if (send(controlFd, &reply, sizeof(reply), MSG_NOSIGNAL) != sizeof(reply)) break;
    }
    for (pid_t child : children) kill(child, SIGKILL);
    while (waitpid(-1, nullptr, 0) > 0 || errno == EINTR) {}
    _exit(0);
}

} // namespace

int Phone_PrepareDirectForkServer() {
    std::lock_guard<std::mutex> lock(g_forkMutex);
    if (g_forkControl >= 0) return NCP_NO_ERROR;
    // The SDK stub omits this symbol; resolve the installed musl implementation.
    // Do not silently fall back to a late App fork when it is unavailable.
    auto forkChild = reinterpret_cast<ForkChildFn>(dlsym(RTLD_DEFAULT, "_Fork"));
    if (!forkChild) return NCP_ERR_NOT_SUPPORTED;
    InstallReaperOnce();
    int control[2], events[2], ready[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control) != 0)
        return NCP_ERR_INTERNAL;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, events) != 0) {
        close(control[0]); close(control[1]); return NCP_ERR_INTERNAL;
    }
    if (pipe(ready) != 0) {
        for (int fd : {control[0], control[1], events[0], events[1]}) close(fd);
        return NCP_ERR_INTERNAL;
    }
    const pid_t parent = getpid();
    const pid_t child = fork();
    if (child == 0) DirectForkServerMain(control[1], events[1], ready[1], parent, forkChild);
    close(control[1]); close(events[1]); close(ready[1]);
    if (child < 0 || !ParentWaitHandshake(ready[0])) {
        if (child < 0) close(ready[0]);
        else kill(child, SIGKILL);
        close(control[0]); close(events[0]);
        return NCP_ERR_LIB_LOADING_FAILED;
    }
    SetSocketTimeout(control[0], 12);
    g_forkControl = control[0];
    g_forkPid = child;
    std::thread([fd = events[0], child] {
        ForkExit event{};
        ssize_t count;
        while ((count = recv(fd, &event, sizeof(event), 0)) > 0 || (count < 0 && errno == EINTR)) {
            if (count < 0) continue;
            if (count != sizeof(event) || event.magic != kForkMagic || event.pid <= 0) break;
            OH_LOG_INFO(LOG_APP, "[DirectFork] server=%{public}d child=%{public}d waitStatus=%{public}d",
                        child, event.pid, event.status);
            bool report = false;
            {
                std::lock_guard<std::mutex> exitLock(g_forkExitMutex);
                auto& record = g_forkChildren[event.pid];
                record.status = event.status;
                record.exited = true;
                report = record.registered && !record.reported;
                if (report) record.reported = true;
                // Keep bounded diagnostic history; live records are never evicted.
                for (auto it = g_forkChildren.begin(); g_forkChildren.size() > 512 &&
                     it != g_forkChildren.end();) {
                    if (it->second.exited && (!it->second.registered || it->second.reported) &&
                        it->first != event.pid) it = g_forkChildren.erase(it);
                    else ++it;
                }
            }
            if (report) NotePhoneForkServerChildExit(event.pid, event.status);
        }
        close(fd);
        std::lock_guard<std::mutex> lock(g_forkMutex);
        if (g_forkPid == child) {
            close(g_forkControl);
            g_forkControl = -1;
            g_forkPid = -1;
        }
    }).detach();
    return NCP_NO_ERROR;
}

int32_t Phone_GetDirectForkServerPid() {
    std::lock_guard<std::mutex> lock(g_forkMutex);
    return g_forkPid;
}

bool Phone_QueryDirectForkChildExit(int32_t pid, int* waitStatus) {
    std::lock_guard<std::mutex> lock(g_forkExitMutex);
    auto it = g_forkChildren.find(pid);
    if (it == g_forkChildren.end() || !it->second.exited) return false;
    if (waitStatus) *waitStatus = it->second.status;
    return true;
}

bool Phone_IsDirectForkServerChild(int32_t pid) {
    std::lock_guard<std::mutex> lock(g_forkExitMutex);
    return g_forkChildren.count(pid) != 0;
}

void Phone_MarkDirectForkChildRegistered(int32_t pid) {
    bool report = false;
    int status = 0;
    {
        std::lock_guard<std::mutex> lock(g_forkExitMutex);
        auto it = g_forkChildren.find(pid);
        if (it == g_forkChildren.end()) return;
        auto& record = it->second;
        record.registered = true;
        report = record.exited && !record.reported;
        if (report) record.reported = true;
        status = record.status;
    }
    if (report) NotePhoneForkServerChildExit(pid, status);
}

Ability_NativeChildProcess_ErrCode Phone_StartViaDirectForkServer(
    const char* entry, NativeChildProcess_Args args, int32_t* pid) {
    if (!entry || !pid || strlen(entry) >= sizeof(ForkRequest::entry) ||
        !args.entryParams || strlen(args.entryParams) >= sizeof(ForkRequest::params))
        return NCP_ERR_INVALID_PARAM;
    *pid = -1;
    ForkRequest request{};
    strcpy(request.entry, entry);
    strcpy(request.params, args.entryParams);
    std::vector<int> fds;
    for (auto* node = args.fdList.head; node; node = node->next) {
        if (fds.size() >= kForkMaxFds || !node->fdName || !node->fdName[0] ||
            strlen(node->fdName) > 20 || node->fd < 0) return NCP_ERR_INVALID_PARAM;
        strcpy(request.names[fds.size()], node->fdName);
        fds.push_back(node->fd);
    }
    request.fdCount = fds.size();
    std::lock_guard<std::mutex> lock(g_forkMutex);
    if (g_forkControl < 0) return NCP_ERR_NOT_SUPPORTED;
    ForkReply reply{};
    ssize_t count = -1;
    if (SendForkRequest(g_forkControl, request, fds)) {
        do { count = recv(g_forkControl, &reply, sizeof(reply), 0); }
        while (count < 0 && errno == EINTR);
    }
    if (count != sizeof(reply) || reply.magic != kForkMagic) {
        // Never reuse a socket after a timeout: an eventual reply belongs to
        // the abandoned request. Terminate this opt-in session instead.
        close(g_forkControl);
        g_forkControl = -1;
        kill(g_forkPid, SIGKILL);
        return NCP_ERR_CONNECTION_FAILED;
    }
    *pid = reply.pid;
    if (reply.pid > 0) {
        std::lock_guard<std::mutex> exitLock(g_forkExitMutex);
        g_forkChildren.try_emplace(reply.pid);
    }
    return static_cast<Ability_NativeChildProcess_ErrCode>(reply.code);
}

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

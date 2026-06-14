#include <napi/native_api.h>
#include "wayland_server.h"
#include "plugin_manager.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

// ── 全局状态 ──
static pid_t gClientPid = -1;
static std::atomic<bool> gReaderRunning{false};
static napi_threadsafe_function gStateTsfn = nullptr;
static std::string gSockPath;
static napi_env gEnv = nullptr;
static napi_ref gExports = nullptr;

// ── fork/exec 后关闭继承的 fd ──
static void CloseInheritedFds(int keep1, int keep2) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return;
    int dfd = dirfd(d);
    dirent* e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (fd > 2 && fd != dfd && fd != keep1 && fd != keep2) close(fd);
    }
    closedir(d);
}

// ── 客户端 stdout/stderr 读取线程 ──
static void ReaderThread(int fd) {
    char buf[2048];
    std::string pending;
    while (gReaderRunning) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            pending.append(buf, n);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                OH_LOG_INFO(LOG_APP, "[wine] %{public}s", line.c_str());
                pending.erase(0, pos + 1);
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }
    if (!pending.empty()) {
        OH_LOG_INFO(LOG_APP, "[wine] %{public}s", pending.c_str());
    }
    close(fd);
    // 子进程已退出，通知 ArkTS
    if (gClientPid > 0) {
        int status;
        waitpid(gClientPid, &status, 0);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        OH_LOG_INFO(LOG_APP, "[wine] process %{public}d exited with code %{public}d", gClientPid, code);
        gClientPid = -1;
        if (gStateTsfn) {
            char* msg = strdup("exited");
            napi_call_threadsafe_function(gStateTsfn, msg, napi_tsfn_blocking);
        }
    }
}

// ── State 回调 → ArkTS ──
static void CallJsState(napi_env env, napi_value cb, void*, void* data) {
    char* msg = static_cast<char*>(data);
    if (env && cb && msg) {
        napi_value undef, arg;
        napi_get_undefined(env, &undef);
        napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &arg);
        napi_call_function(env, undef, cb, 1, &arg, nullptr);
    }
    free(msg);
}

// ── NAPI: setStateCallback ──
static napi_value SetStateCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gStateTsfn) {
        napi_release_threadsafe_function(gStateTsfn, napi_tsfn_release);
        gStateTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WLState", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsState, &gStateTsfn);

    WaylandServer::GetInstance()->SetStateCallback([](const char* s) {
        if (gStateTsfn) {
            napi_call_threadsafe_function(gStateTsfn, strdup(s), napi_tsfn_blocking);
        }
    });
    return nullptr;
}

// ── NAPI: startServer ──
static napi_value StartServer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    char path[512] = {};
    napi_get_value_string_utf8(env, args[0], path, sizeof(path), nullptr);

    OH_LOG_INFO(LOG_APP, "[NAPI] startServer: %{public}s", path);
    gSockPath = path;
    bool ok = WaylandServer::GetInstance()->Start(path);
    OH_LOG_INFO(LOG_APP, "[NAPI] startServer result: %{public}s", ok ? "OK" : "FAIL");
    // 确认 socket 文件存在
    if (ok) {
        struct stat st;
        int sr = stat(path, &st);
        OH_LOG_INFO(LOG_APP, "[NAPI] wayland socket stat=%{public}d (errno=%{public}d)",
                    sr, sr == 0 ? 0 : errno);
    }

    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// ── NAPI: launchClient (异步: wineserver→wineboot→wine 在后台线程, 不阻塞 JS 主线程) ──
struct LaunchParams {
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string sockDir;
    std::string sockName;
    std::string wineboxBin;
    std::vector<std::string> envStrs;  // 持有 envp 字符串
    std::vector<char*> envp;           // 指向 envStrs
    int pipefd[2] = {-1, -1};
};

static void LaunchThreadFunc(LaunchParams* p) {
    OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver + wineboot + wine starting in background");

    // 组装 envp 环境变量
    p->envStrs = {
        "XDG_RUNTIME_DIR=" + p->sockDir,
        "WAYLAND_DISPLAY=" + p->sockName,
        "LD_LIBRARY_PATH=" + p->libPath,
        "HOME=/storage/Users/currentUser",
        "WINEPREFIX=/data/storage/el2/base/files",
        "BOX64_LOG=1",
        "BOX64_NOBANNER=0",
        "PATH=/usr/local/bin:/data/app/bin:/data/service/hnp/bin:/usr/bin:/vendor/bin",
        "TMPDIR=/storage/Users/currentUser",
    };
    for (auto& s : p->envStrs) p->envp.push_back((char*)s.c_str());
    p->envp.push_back(nullptr);

    mkdir("/storage/Users/currentUser/workspace/wine", 0755);
    mkdir("/data/storage/el2/base/files", 0755);

    // 通知 ArkTS: wineserver 启动中
    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-starting"), napi_tsfn_blocking);
    }

    // ── 启动 wineserver ──
    {
        std::vector<std::string> wsEnvStrs = p->envStrs;
        wsEnvStrs.push_back("BOX64_DYNAREC=0");
        std::vector<char*> wsEnvp;
        for (auto& s : wsEnvStrs) wsEnvp.push_back((char*)s.c_str());
        wsEnvp.push_back(nullptr);

        int wsErrFd = open("/storage/Users/currentUser/workspace/wine/wineserver_logs.txt",
                           O_WRONLY|O_CREAT|O_TRUNC, 0644);
        pid_t wsPid = fork();
        if (wsPid == 0) {
            dup2(wsErrFd, STDERR_FILENO);
            if (wsErrFd > 2) close(wsErrFd);
            CloseInheritedFds(STDOUT_FILENO, STDERR_FILENO);
            for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
            prctl(PR_SET_NAME, "wl-wineserver", 0, 0, 0);
            chdir(p->wineboxBin.c_str());
            const char* wsArgv[] = {"./box64", "./wineserver", nullptr};
            execve("./box64", (char* const*)wsArgv, wsEnvp.data());
            _exit(127);
        }
        if (wsErrFd > 2) close(wsErrFd);

        if (wsPid > 0) {
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d, waiting 1.5s...", wsPid);
            usleep(1500000);
        } else {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver fork failed");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-failed"), napi_tsfn_blocking);
            delete p;
            return;
        }
    }

    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-starting"), napi_tsfn_blocking);
    }

    // ── wineboot --init ──
    {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] running wineboot --init...");
        pid_t bootPid = fork();
        if (bootPid == 0) {
            CloseInheritedFds(STDOUT_FILENO, STDERR_FILENO);
            for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
            prctl(PR_SET_NAME, "wl-wineboot", 0, 0, 0);
            chdir(p->wineboxBin.c_str());
            const char* bootArgv[] = {"./box64", "./wine", "./wineboot", "--init", nullptr};
            int bootFd = open("/storage/Users/currentUser/workspace/wine/wineboot_logs.txt",
                              O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (bootFd >= 0) dup2(bootFd, STDERR_FILENO);
            execve("./box64", (char* const*)bootArgv, p->envp.data());
            _exit(127);
        }
        if (bootPid > 0) {
            int bootStatus = 0;
            for (int i = 0; i < 300; i++) {
                pid_t wr = waitpid(bootPid, &bootStatus, WNOHANG);
                if (wr > 0) break;
                if (wr < 0 && errno != EINTR) break;
                if (i > 0 && i % 30 == 0)
                    OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot --init still running (%{public}ds)...", i);
                sleep(1);
            }
            if (kill(bootPid, 0) == 0) {
                OH_LOG_WARN(LOG_APP, "[Launch-Async] wineboot --init timeout (300s), killing");
                kill(bootPid, SIGKILL);
                waitpid(bootPid, &bootStatus, 0);
            }
            int code = WIFEXITED(bootStatus) ? WEXITSTATUS(bootStatus) : -1;
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot --init done, exit=%{public}d", code);
        }
    }

    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wine-starting"), napi_tsfn_blocking);
    }

    // ── 启动 wine ──
    pid_t pid = fork();
    if (pid == 0) {
        close(p->pipefd[0]);
        dup2(p->pipefd[1], STDOUT_FILENO);
        int errfd = open("/storage/Users/currentUser/workspace/wine/notepad_logs.txt",
                         O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (errfd >= 0) dup2(errfd, STDERR_FILENO);
        if (p->pipefd[1] > 2) close(p->pipefd[1]);
        prctl(PR_SET_NAME, "wl-client", 0, 0, 0);
        CloseInheritedFds(STDOUT_FILENO, STDERR_FILENO);
        for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
        chdir(p->wineboxBin.c_str());
        const char* cargv[] = {"./box64", "./wine", "./notepad.exe", nullptr};
        execve("./box64", (char* const*)cargv, p->envp.data());
        _exit(127);
    }

    if (pid > 0) {
        close(p->pipefd[1]);
        gClientPid = pid;
        gReaderRunning = true;
        std::thread(ReaderThread, p->pipefd[0]).detach();
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wine pid=%{public}d, reader started", pid);

        if (gStateTsfn) {
            napi_call_threadsafe_function(gStateTsfn, strdup("wine-running"), napi_tsfn_blocking);
        }
    } else {
        close(p->pipefd[0]);
        close(p->pipefd[1]);
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] wine fork failed");
        if (gStateTsfn)
            napi_call_threadsafe_function(gStateTsfn, strdup("wine-failed"), napi_tsfn_blocking);
    }

    delete p;
}

static napi_value LaunchClient(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    auto* p = new LaunchParams();

    char buf[1024] = {};
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), nullptr);
    p->exePath = buf;
    napi_get_value_string_utf8(env, args[2], buf, sizeof(buf), nullptr);
    p->sockPath = buf;
    napi_get_value_string_utf8(env, args[3], buf, sizeof(buf), nullptr);
    p->libPath = buf;

    OH_LOG_INFO(LOG_APP, "[Launch] exe=%{public}s sock=%{public}s lib=%{public}s (async)",
                p->exePath.c_str(), p->sockPath.c_str(), p->libPath.c_str());

    // 保证可执行
    if (access(p->exePath.c_str(), X_OK) != 0) chmod(p->exePath.c_str(), 0755);

    // 提取 sockDir, sockName, wineboxBin
    auto pos = p->sockPath.find_last_of('/');
    p->sockDir = (pos == std::string::npos) ? "/tmp" : p->sockPath.substr(0, pos);
    p->sockName = (pos == std::string::npos) ? p->sockPath : p->sockPath.substr(pos + 1);
    pos = p->exePath.find_last_of('/');
    p->wineboxBin = (pos != std::string::npos) ? p->exePath.substr(0, pos) : p->exePath;

    // 创建 pipe
    if (pipe(p->pipefd) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Launch] pipe failed: %{public}s", strerror(errno));
        delete p;
        napi_value r;
        napi_create_int32(env, -1, &r);
        return r;
    }
    fcntl(p->pipefd[0], F_SETFD, FD_CLOEXEC);
    signal(SIGCHLD, SIG_IGN);

    // 启动后台线程: wineserver → wineboot --init → wine
    std::thread(LaunchThreadFunc, p).detach();

    OH_LOG_INFO(LOG_APP, "[Launch] background thread started, returning to JS");

    napi_value r;
    napi_create_int32(env, 0, &r);
    return r;
}

// ── NAPI: mmap 全量权限测试 (覆盖 Box64 + Wine 所有模式) ──
#include <sys/mman.h>
#include <spawn.h>
#include <sys/syscall.h>

static const char* PN(int prot) {
    static char b[8];
    if (prot == 0) return "NONE";
    snprintf(b, sizeof(b), "%s%s%s",
             (prot & PROT_READ) ? "R" : "",
             (prot & PROT_WRITE) ? "W" : "",
             (prot & PROT_EXEC) ? "X" : "");
    return b;
}

static void L(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void L(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OH_LOG_INFO(LOG_APP, "[MMAP] %{public}s", buf);
}

// 测试单个 mmap 组合并输出结果
static void TestOne(const char* desc, void* p, size_t sz) {
    L("  %-20s -> %s (err=%d, %s)",
      desc, p == MAP_FAILED ? "FAIL" : "OK", errno, strerror(errno));
    if (p != MAP_FAILED) munmap(p, sz);
}

static napi_value RunMmapTests(napi_env env, napi_callback_info) {
    const size_t pg = 4096;
    L("===== OHOS mmap 全量测试 (Box64/Wine 模式覆盖) =====");

    // ─── 1a. 匿名 MAP_PRIVATE ───
    L("--- 1a. ANON|PRIV 4KB ---");
    for (int p = 0; p < 8; p++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "ANON|PRIV %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), pg);
    }

    // ─── 1b. 匿名 MAP_SHARED ───
    L("--- 1b. ANON|SHARED 4KB ---");
    for (int p = 0; p < 8; p++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "ANON|SHAR %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_SHARED | MAP_ANONYMOUS, -1, 0), pg);
    }

    // ─── 1c. 文件 memfd SHARED ───
    L("--- 1c. FILE|SHARED 4KB ---");
    for (int p = 0; p < 8; p++) {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd < 0) continue;
        ftruncate(fd, pg);
        char desc[32];
        snprintf(desc, sizeof(desc), "FILE|SHAR %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_SHARED, fd, 0), pg);
        close(fd);
    }

    // ─── 1d. 文件 memfd PRIVATE ───
    L("--- 1d. FILE|PRIV 4KB ---");
    for (int p = 0; p < 8; p++) {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd < 0) continue;
        ftruncate(fd, pg);
        char desc[32];
        snprintf(desc, sizeof(desc), "FILE|PRIV %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_PRIVATE, fd, 0), pg);
        close(fd);
    }

    // ─── 2. 大尺寸 (Wine virtual alloc / ELF 加载) ───
    L("--- 2. ANON|PRIV 大尺寸 RWX ---");
    size_t big[] = {0x10000, 0x100000, 0x1000000, 0x10000000};
    for (int i = 0; i < 4; i++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "sz=0x%zx RWX", big[i]);
        TestOne(desc, mmap(NULL, big[i], PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), big[i]);
    }

    // ─── 3. MAP_FIXED (Box64 ELF loader / Wine prereserve) ───
    L("--- 3a. MAP_FIXED ANON|PRIV ---");
    {
        void* base = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base != MAP_FAILED) {
            munmap(base, pg * 16);
            int prots[] = {PROT_READ | PROT_WRITE,
                           PROT_READ | PROT_EXEC,
                           PROT_READ | PROT_WRITE | PROT_EXEC};
            const char* names[] = {"RW", "RX", "RWX"};
            for (int i = 0; i < 3; i++) {
                char desc[64];
                snprintf(desc, sizeof(desc), "FIXED %s @%p", names[i], base);
                void* m = mmap(base, pg * 16, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                L("  %-24s -> %s (err=%d)", desc,
                  m == MAP_FAILED ? "FAIL" : "OK", errno);
                if (m != MAP_FAILED) munmap(m, pg * 16);
            }
        } else {
            L("  SKIP (base mmap failed)");
        }
    }

    L("--- 3b. MAP_FIXED FILE|SHARED ---");
    {
        void* base = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base != MAP_FAILED) {
            munmap(base, pg * 16);
            int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
            if (fd >= 0) {
                ftruncate(fd, pg * 16);
                int prots[] = {PROT_READ | PROT_WRITE,
                               PROT_READ | PROT_EXEC,
                               PROT_READ | PROT_WRITE | PROT_EXEC};
                const char* names[] = {"RW", "RX", "RWX"};
                for (int i = 0; i < 3; i++) {
                    char desc[64];
                    snprintf(desc, sizeof(desc), "FIXED FILE %s @%p", names[i], base);
                    void* m = mmap(base, pg * 16, prots[i],
                                   MAP_SHARED | MAP_FIXED, fd, 0);
                    L("  %-30s -> %s (err=%d)", desc,
                      m == MAP_FAILED ? "FAIL" : "OK", errno);
                    if (m != MAP_FAILED) munmap(m, pg * 16);
                }
                close(fd);
            }
        } else {
            L("  SKIP (base mmap failed)");
        }
    }

    // ─── 4. MAP_NORESERVE (Box64 ELF pre-allocation) ───
    L("--- 4. MAP_NORESERVE ANON|PRIV ---");
    {
        int prots[] = {0, PROT_READ, PROT_READ | PROT_WRITE,
                       PROT_READ | PROT_EXEC,
                       PROT_READ | PROT_WRITE | PROT_EXEC};
        for (int i = 0; i < 5; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "NORESV %s", PN(prots[i]));
            TestOne(desc, mmap(NULL, pg * 256, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                               -1, 0), pg * 256);
        }
    }

    // ─── 5. 高地址 hint (模拟 Box64 find47bitBlock) ───
    L("--- 5. 高地址 hint RWX ---");
    {
        void* hints[] = {nullptr,
                         (void*)0x100000000ULL,
                         (void*)0x3f00000000ULL,
                         (void*)0x7f00000000ULL,
                         (void*)0x7fff00000000ULL};
        const char* hnames[] = {"NULL", "4G", "0x3f00000000",
                                "0x7f00000000", "0x7fff00000000"};
        for (int i = 0; i < 5; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "hint=%s", hnames[i]);
            void* m = mmap(hints[i], pg * 16,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            L("  %-22s -> %s @%p (err=%d)", desc,
              m == MAP_FAILED ? "FAIL" : "OK", m, errno);
            if (m != MAP_FAILED) munmap(m, pg * 16);
        }
    }

    // ─── 6. MAP_32BIT (ARM64 不支持的 x86 flag) ───
    L("--- 6. MAP_32BIT (0x40) ---");
    {
        int prots[] = {0, PROT_READ | PROT_WRITE,
                       PROT_READ | PROT_EXEC,
                       PROT_READ | PROT_WRITE | PROT_EXEC};
        for (int i = 0; i < 4; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "0x40 %s", PN(prots[i]));
            TestOne(desc, mmap(NULL, pg * 16, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | 0x40, -1, 0),
                    pg * 16);
        }
    }

    // ─── 7. mprotect ───
    L("--- 7a. mprotect ANON RW -> ... ---");
    {
        void* m = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            for (int tp = 0; tp < 8; tp++) {
                int ret = mprotect(m, pg * 16, tp);
                L("  RW->%-3s -> %s (err=%d)", PN(tp),
                  ret == 0 ? "OK" : "FAIL", errno);
            }
            munmap(m, pg * 16);
        } else {
            L("  SKIP");
        }
    }

    L("--- 7b. mprotect FILE SHARED RW -> RX/RWX ---");
    {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd >= 0) {
            ftruncate(fd, pg * 16);
            void* m = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) {
                int t1[] = {PROT_READ | PROT_EXEC,
                            PROT_READ | PROT_WRITE | PROT_EXEC};
                const char* n1[] = {"RX", "RWX"};
                for (int i = 0; i < 2; i++) {
                    int ret = mprotect(m, pg * 16, t1[i]);
                    L("  FILE RW->%-3s -> %s (err=%d)", n1[i],
                      ret == 0 ? "OK" : "FAIL", errno);
                }
                munmap(m, pg * 16);
            }
            close(fd);
        }
    }

    // ─── 8 + 9. 可执行代码测试 ───
    L("--- 8. ANON RWX + exec ---");
    {
        void* m = mmap(NULL, pg, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            ((uint32_t*)m)[0] = 0xD65F03C0; // ARM64 ret
            __builtin___clear_cache((char*)m, (char*)m + 4);
            L("  ANON RWX + exec -> OK");
            munmap(m, pg);
        } else {
            L("  SKIP (mmap RWX failed)");
        }
    }

    L("--- 9. Dynarec: mmap RW + mprotect RX + exec ---");
    {
        void* m = mmap(NULL, pg, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            ((uint32_t*)m)[0] = 0xD65F03C0; // ARM64 ret
            int ret = mprotect(m, pg, PROT_READ | PROT_EXEC);
            L("  RW + mprotect RX -> %s (err=%d)",
              ret == 0 ? "OK" : "FAIL", errno);
            if (ret == 0) {
                __builtin___clear_cache((char*)m, (char*)m + 4);
                L("  exec -> OK");
            }
            munmap(m, pg);
        }
    }

    // ─── 10. fork mmap (用 pipe 捕获子进程输出) ───
    L("--- 10. fork mmap ---");
    {
        int fd[2]; pipe(fd);
        pid_t pid = fork();
        if (pid == 0) {
            close(fd[0]); dup2(fd[1], STDOUT_FILENO); dup2(fd[1], STDERR_FILENO);
            void* m = mmap(NULL, pg, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            printf("fork: ANON RWX -> %s (err=%d)\n", m==MAP_FAILED?"FAIL":"OK", errno);
            if (m != MAP_FAILED) {
                munmap(m, pg);
            } else {
                void* m2 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                if (m2 != MAP_FAILED) {
                    int r = mprotect(m2, pg, PROT_READ|PROT_WRITE|PROT_EXEC);
                    printf("fork: RW+mprotect->RWX -> %s (err=%d)\n", r==0?"OK":"FAIL", errno);
                    munmap(m2, pg);
                }
            }
            close(fd[1]); _exit(0);
        }
        close(fd[1]);
        char buf[512]={0}; read(fd[0], buf, sizeof(buf)-1); close(fd[0]);
        for (char* p=strtok(buf,"\n"); p; p=strtok(NULL,"\n")) L("  %s", p);
        waitpid(pid, NULL, 0);
    }

    // ─── 10b. 嵌套 fork (孙进程) — 模拟 wineserver 场景: APP→fork→box64→fork→wineserver ───
    L("--- 10b. nested fork (grandchild) ---");
    {
        int fd[2]; pipe(fd);
        pid_t pid = fork();
        if (pid == 0) {
            // child: fork again to create grandchild
            int fd2[2]; pipe(fd2);
            pid_t pid2 = fork();
            if (pid2 == 0) {
                // grandchild — 这才是 wineserver 等效层级
                close(fd2[0]); dup2(fd2[1], STDOUT_FILENO); dup2(fd2[1], STDERR_FILENO);
                setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
                printf("gc: starting mmap tests\n");
                void* gc1 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                printf("gc: ANON RW  -> %s (err=%d)\n", gc1==MAP_FAILED?"FAIL":"OK", errno);
                if(gc1!=MAP_FAILED) munmap(gc1, pg);
                printf("gc: about to try RWX...\n");
                void* gc2 = mmap(NULL, pg, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                printf("gc: ANON RWX -> %s (err=%d)\n", gc2==MAP_FAILED?"FAIL":"OK", errno);
                if(gc2!=MAP_FAILED) munmap(gc2, pg);
                if(gc2==MAP_FAILED) {
                    printf("gc: RWX failed, trying RW+mprotect...\n");
                    void* gc3 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                    if(gc3!=MAP_FAILED){
                        int r=mprotect(gc3, pg, PROT_READ|PROT_WRITE|PROT_EXEC);
                        printf("gc: RW+mprotect->RWX -> %s (err=%d)\n", r==0?"OK":"FAIL", errno);
                        munmap(gc3, pg);
                    }
                }
                printf("gc: done\n");
                close(fd2[1]); _exit(0);
            }
            // child: collect grandchild output, relay to parent
            close(fd2[1]);
            char buf2[512]={0}; read(fd2[0], buf2, sizeof(buf2)-1); close(fd2[0]);
            int status;
            waitpid(pid2, &status, 0);
            close(fd[0]); dup2(fd[1], STDOUT_FILENO); dup2(fd[1], STDERR_FILENO);
            printf("%s", buf2);
            if(WIFSIGNALED(status)) printf("gc_child: KILLED by signal %d\n", WTERMSIG(status));
            else if(WIFEXITED(status)) printf("gc_child: exited=%d\n", WEXITSTATUS(status));
            close(fd[1]); _exit(0);
        }
        close(fd[1]);
        char buf[512]={0}; read(fd[0], buf, sizeof(buf)-1); close(fd[0]);
        for (char* p=strtok(buf,"\n"); p; p=strtok(NULL,"\n")) L("  %s", p);
        waitpid(pid, NULL, 0);
    }

    // ─── 11. posix_spawn mmap_test (pipe 捕获) ───
    L("--- 11. posix_spawn mmap_test ---");
    {
        int fd[2]; pipe(fd);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fd[1], STDERR_FILENO);
        pid_t pid;
        const char* spawn_bin = "/data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox/bin/mmap_test";
        char* argv[] = { (char*)spawn_bin, NULL };
        extern char** environ;
        int ret = posix_spawn(&pid, spawn_bin, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fd[1]);
        if (ret == 0) {
            char buf[4096]={0}; ssize_t n = read(fd[0], buf, sizeof(buf)-1);
            if (n > 0) {
                char* save; char* line = strtok_r(buf, "\n", &save);
                while (line) {
                    // 只显示 RWX 相关行
                    if (strstr(line, "RWX") || strstr(line, "mprotect") || strstr(line, "FAIL")) L("  %s", line);
                    line = strtok_r(NULL, "\n", &save);
                }
            }
            waitpid(pid, NULL, 0);
        } else {
            L("  FAILED err=%d(%s)", ret, strerror(ret));
        }
        close(fd[0]);
    }

    L("===== 测试完成 =====");
    return nullptr;
}

// ── NAPI: stopClient ──
static napi_value StopClient(napi_env, napi_callback_info) {
    gReaderRunning = false;
    if (gClientPid > 0) {
        kill(gClientPid, SIGTERM);
        gClientPid = -1;
    }
    WaylandServer::GetInstance()->ResetFirstFrame();
    return nullptr;
}

// ── NAPI: stopAll ──
static napi_value StopAll(napi_env, napi_callback_info) {
    gReaderRunning = false;
    if (gClientPid > 0) {
        kill(gClientPid, SIGTERM);
        gClientPid = -1;
    }
    WaylandServer::GetInstance()->Stop();
    return nullptr;
}

// ── 终端实现 — fork + pipe (不用 pty, OHOS 沙箱不允许 /dev/ptmx) ──
static int gTermOutFd = -1;  // 读子进程 stdout
static int gTermInFd = -1;   // 写子进程 stdin
static pid_t gTermPid = -1;
static napi_threadsafe_function gTermOnData = nullptr;
static napi_threadsafe_function gTermOnExit = nullptr;
static std::atomic<bool> gTermRunning{false};

static void TermOnDataCB(napi_env env, napi_value cb, void*, void* data) {
    char* buf = static_cast<char*>(data);
    size_t len = strlen(buf);
    napi_value ab; char* out;
    napi_create_arraybuffer(env, len, (void**)&out, &ab);
    memcpy(out, buf, len);
    napi_value global;
    napi_get_global(env, &global);
    napi_call_function(env, global, cb, 1, &ab, nullptr);
    free(buf);
}

static void TermOnExitCB(napi_env env, napi_value cb, void*, void*) {
    napi_value global;
    napi_get_global(env, &global);
    napi_call_function(env, global, cb, 0, nullptr, nullptr);
}

static void TerminalWorker() {
    OH_LOG_INFO(LOG_APP, "[TERM] worker started, outFd=%{public}d", gTermOutFd);
    while (gTermRunning) {
        pollfd fds[1] = {{gTermOutFd, POLLIN, 0}};
        int pr = poll(fds, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        char buf[1024];
        ssize_t r = read(gTermOutFd, buf, sizeof(buf) - 1);
        if (r > 0) {
            buf[r] = 0;
            if (gTermOnData) {
                char* copy = strdup(buf);
                napi_call_threadsafe_function(gTermOnData, copy, napi_tsfn_nonblocking);
            }
        } else break;
    }
    OH_LOG_INFO(LOG_APP, "[TERM] worker exiting, calling onExit");
    if (gTermOnExit)
        napi_call_threadsafe_function(gTermOnExit, nullptr, napi_tsfn_nonblocking);
}

static napi_value TermRun(napi_env env, napi_callback_info info) {
    // 先关掉旧的
    if (gTermRunning) {
        gTermRunning = false;
        if (gTermPid > 0) { kill(gTermPid, SIGKILL); gTermPid = -1; }
        if (gTermOutFd >= 0) { close(gTermOutFd); gTermOutFd = -1; }
        if (gTermInFd >= 0) { close(gTermInFd); gTermInFd = -1; }
        if (gTermOnData) { napi_release_threadsafe_function(gTermOnData, napi_tsfn_release); gTermOnData = nullptr; }
        if (gTermOnExit) { napi_release_threadsafe_function(gTermOnExit, napi_tsfn_release); gTermOnExit = nullptr; }
    }

    size_t argc = 4; napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t cols, rows;
    napi_get_value_int32(env, args[0], &cols);
    napi_get_value_int32(env, args[1], &rows);
    OH_LOG_INFO(LOG_APP, "[TERM] TermRun cols=%{public}d rows=%{public}d", cols, rows);

    int pin[2], pout[2];
    if (pipe(pin) < 0 || pipe(pout) < 0) {
        OH_LOG_INFO(LOG_APP, "[TERM] pipe failed errno=%{public}d", errno);
        return nullptr;
    }

    pid_t pid = fork();
    OH_LOG_INFO(LOG_APP, "[TERM] fork pid=%{public}d errno=%{public}d", pid, errno);
    if (pid == 0) {
        close(pin[1]); close(pout[0]);
        dup2(pin[0], STDIN_FILENO);
        dup2(pout[1], STDOUT_FILENO);
        dup2(pout[1], STDERR_FILENO);
        close(pin[0]); close(pout[1]);
        CloseInheritedFds(STDIN_FILENO, STDOUT_FILENO);
        setenv("HOME", "/storage/Users/currentUser", 1);
        setenv("PATH", "/data/app/bin:/data/service/hnp/bin:/bin:/usr/local/bin:/usr/bin:/system/bin:/vendor/bin", 1);
        setenv("LD_LIBRARY_PATH", "/data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox/bin/x86_64-unix", 1);
        setenv("TERM", "xterm", 1);
        chdir("/storage/Users/currentUser");
        execl("/bin/sh", "/bin/sh", nullptr);
        _exit(127);
    }

    close(pin[0]); close(pout[1]);
    gTermInFd = pin[1];
    gTermOutFd = pout[0];
    gTermPid = pid;

    fcntl(gTermOutFd, F_SETFL, fcntl(gTermOutFd, F_GETFL) | O_NONBLOCK);

    napi_value cbname;
    napi_create_string_utf8(env, "termOnData", NAPI_AUTO_LENGTH, &cbname);
    napi_create_threadsafe_function(env, args[2], nullptr, cbname, 0, 1, nullptr, nullptr, nullptr,
                                    TermOnDataCB, &gTermOnData);
    napi_create_string_utf8(env, "termOnExit", NAPI_AUTO_LENGTH, &cbname);
    napi_create_threadsafe_function(env, args[3], nullptr, cbname, 0, 1, nullptr, nullptr, nullptr,
                                    TermOnExitCB, &gTermOnExit);

    gTermRunning = true;
    std::thread(TerminalWorker).detach();

    napi_value ret; napi_create_int32(env, pid, &ret);
    return ret;
}

static napi_value TermSend(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    void* data; size_t len;
    napi_get_arraybuffer_info(env, args[0], &data, &len);
    if (gTermInFd >= 0) {
        // hterm 发的是 \r, 没有 PTY 不会自动转 \n, 这里手动转换
        char* buf = (char*)data;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '\r') buf[i] = '\n';
        }
        ssize_t w = write(gTermInFd, data, len);
        OH_LOG_INFO(LOG_APP, "[TERM] TermSend len=%{public}zu written=%{public}zd", len, w);
    }
    return nullptr;
}

static napi_value TermResize(napi_env env, napi_callback_info info) {
    // pipe 模式不支持 resize, 只打日志
    size_t argc = 2; napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    OH_LOG_INFO(LOG_APP, "[TERM] TermResize (no-op in pipe mode)");
    return nullptr;
}

static napi_value TermClose(napi_env, napi_callback_info) {
    OH_LOG_INFO(LOG_APP, "[TERM] TermClose");
    gTermRunning = false;
    if (gTermPid > 0) { kill(gTermPid, SIGKILL); gTermPid = -1; }
    if (gTermOutFd >= 0) { close(gTermOutFd); gTermOutFd = -1; }
    if (gTermInFd >= 0) { close(gTermInFd); gTermInFd = -1; }
    if (gTermOnData) { napi_release_threadsafe_function(gTermOnData, napi_tsfn_release); gTermOnData = nullptr; }
    if (gTermOnExit) { napi_release_threadsafe_function(gTermOnExit, napi_tsfn_release); gTermOnExit = nullptr; }
    return nullptr;
}

// ── Toplevel 回调 → ArkTS ──
static napi_threadsafe_function gToplevelTsfn = nullptr;

struct ToplevelEvent {
    uint32_t id;
    std::string event;
    std::string data;
};

static void CallJsToplevel(napi_env env, napi_value cb, void*, void* raw) {
    auto* ev = static_cast<ToplevelEvent*>(raw);
    if (env && cb && ev) {
        OH_LOG_INFO(LOG_APP, "[MW-TSCB] calling JS toplevel cb: id=%{public}u event=%{public}s data=%{public}s",
                    ev->id, ev->event.c_str(), ev->data.c_str());
        napi_value undef, args[3];
        napi_get_undefined(env, &undef);
        napi_create_uint32(env, ev->id, &args[0]);
        napi_create_string_utf8(env, ev->event.c_str(), NAPI_AUTO_LENGTH, &args[1]);
        napi_create_string_utf8(env, ev->data.c_str(), NAPI_AUTO_LENGTH, &args[2]);
        napi_call_function(env, undef, cb, 3, args, nullptr);
    }
    delete ev;
}

// ── NAPI: setToplevelCallback ──
static napi_value SetToplevelCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gToplevelTsfn) {
        napi_release_threadsafe_function(gToplevelTsfn, napi_tsfn_release);
        gToplevelTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WLToplevel", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsToplevel, &gToplevelTsfn);

    WaylandServer::GetInstance()->SetToplevelCallback([](uint32_t id, const char* event, const char* data) {
        if (gToplevelTsfn) {
            OH_LOG_INFO(LOG_APP, "[MW-TSCB] enqueue toplevel cb: id=%{public}u event=%{public}s", id, event);
            auto* ev = new ToplevelEvent{id, event ? event : "", data ? data : "{}"};
            napi_call_threadsafe_function(gToplevelTsfn, ev, napi_tsfn_blocking);
        } else {
            OH_LOG_WARN(LOG_APP, "[MW-TSCB] toplevel cb dropped (tsfn not ready): id=%{public}u event=%{public}s",
                        id, event);
        }
    });

    return nullptr;
}

// ── NAPI: setPendingToplevel ── (ArkTS 创建子窗口前调用, 告诉 C++ 下一个 XComponent 属于哪个 toplevel)
static napi_value SetPendingToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    PluginManager::GetInstance()->SetPendingToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] setPendingToplevel id=%{public}u", id);
    return nullptr;
}

// ── NAPI: destroyToplevel ── (ArkTS 关闭子窗口后调用)
static napi_value DestroyToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    PluginManager::GetInstance()->DestroyToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyToplevel id=%{public}u", id);
    return nullptr;
}

// ── NAPI: sendToplevelClose ── (通知 Wine 关闭窗口) ──
static napi_value SendToplevelClose(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] sendToplevelClose id=%{public}u (stub)", id);
    return nullptr;
}

// ── NAPI: initXComponent ── (从 aboutToAppear 调用, 此时 XComponent 已就绪)
static napi_value InitXComponent(napi_env env, napi_callback_info /*info*/) {
    if (gEnv && gExports) {
        napi_value exports;
        napi_get_reference_value(gEnv, gExports, &exports);
        OH_LOG_INFO(LOG_APP, "[NAPI] initXComponent: retrying PluginManager::Export");
        PluginManager::GetInstance()->Export(gEnv, exports);
    }
    return nullptr;
}

// ── 模块注册 ──
EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"startServer",    nullptr, StartServer,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"launchClient",   nullptr, LaunchClient,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopClient",     nullptr, StopClient,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAll",        nullptr, StopAll,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStateCallback", nullptr, SetStateCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelCallback", nullptr, SetToplevelCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPendingToplevel", nullptr, SetPendingToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyToplevel", nullptr, DestroyToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendToplevelClose", nullptr, SendToplevelClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initXComponent", nullptr, InitXComponent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runMmapTests",  nullptr, RunMmapTests,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"termRun",       nullptr, TermRun,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"termSend",      nullptr, TermSend,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"termResize",    nullptr, TermResize,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"termClose",     nullptr, TermClose,    nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // libraryname='entry' 保证 XComponent 在 exports 中, Init 时即可注册
    PluginManager::GetInstance()->Export(env, exports);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule() {
    napi_module_register(&demoModule);
}

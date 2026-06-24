/**
 * wine_child.cpp - Wine 子进程入口 (libwine_child.so)
 *
 * 通过 OH_Ability_StartNativeChildProcess 启动，入口函数 Main()。
 * 子进程从 appspawn 创建，全局状态干净，ntdll.so 首次 dlopen 构造正常执行。
 *
 * entryParams 格式: "binDir|arg0|arg1|..."
 *   binDir  = /data/storage/el2/base/files/wine/bin
 *   后续    = argv (如 "wineboot --init")
 *
 * fdList 的第一个 fd 为 wineserver socket，设为 WINESERVERSOCKET。
 */
#include <AbilityKit/native_child_process.h>
#include <hilog/log.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <dlfcn.h>
#include <sys/stat.h>
#include "wine_constants.h"
#include <fcntl.h>
#include <pthread.h>
#include <time.h>

// 从 stderr pipe 读取 Wine 内部日志，同时转发到 hilog 和文件
struct stderr_ctx { int fd; int fileFd; };
static void* stderr_reader_thread(void* arg) {
    auto* ctx = (stderr_ctx*)arg;
    char buf[4096];
    ssize_t n;
    while ((n = read(ctx->fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';  // strtok_r 需要终止符, read() 不添加
        // 写文件
        if (ctx->fileFd >= 0) write(ctx->fileFd, buf, n);
        // 转发到 hilog（多行拆开）
        char *save = nullptr, *tok = strtok_r(buf, "\n", &save);
        while (tok) {
            OH_LOG_INFO(LOG_APP, "[WineChild-stderr] %{public}s", tok);
            tok = strtok_r(nullptr, "\n", &save);
        }
    }
    if (ctx->fileFd >= 0) close(ctx->fileFd);
    delete ctx;
    return nullptr;
}

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WineChild"

static void setup_wine_env(const char* binDir)
{
    std::string shareDir = std::string(binDir) + "/../share";
    std::string libDir = std::string(binDir) + "/x86_64-unix";

#ifdef __aarch64__
    // ARM64: x86_64 .so 由 Box64 加载，不在系统 LD_LIBRARY_PATH
    // LD_LIBRARY_PATH 只包含 ARM64 原生 .so
    setenv("LD_LIBRARY_PATH",
           "/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64", 1);
    // Box64 用它自己的搜索路径加载 x86_64 .so
    setenv("BOX64_LD_LIBRARY_PATH", libDir.c_str(), 1);
#else
    // x86_64: 系统 linker 直接加载 x86_64 OHOS .so
    setenv("LD_LIBRARY_PATH", libDir.c_str(), 1);
#endif

    setenv("HOME", "/storage/Users/currentUser/Download/app.hackeris.winehua", 1);
    setenv("XDG_RUNTIME_DIR", WINE_PREFIX, 1);
    setenv("WAYLAND_DISPLAY", "wine-wayland", 1);
    setenv("WINEPREFIX", WINE_PREFIX, 1);
    // PROCESSBROKER: appspawn 子进程不继承父 env，从 WINEPREFIX 推导 broker socket 路径，
    // 保证 wineserver / wineboot 等所有子进程都能拿到，与主进程 napi_init.cpp 中的值一致。
    {
        char brokerPath[512];
        snprintf(brokerPath, sizeof(brokerPath), "%s/../.wine_broker",
                 getenv("WINEPREFIX"));
        setenv("PROCESSBROKER", brokerPath, 1);
    }
    setenv("WINEDATADIR", (shareDir + "/wine").c_str(), 1);
    // WINEBINDIR/WINEUNIXDIR 覆盖 init_paths() 中基于 dladdr(ntdll.so) 推算的错误路径
    // ntdll.so 在 bundle libs 目录，而 PE DLL / Unix SO 数据都在 wine/bin/ 下
    setenv("WINEBINDIR", binDir, 1);   // wine/bin/
    setenv("WINEUNIXDIR", binDir, 1);  // wine/bin/ (含 x86_64-unix/x86_64-windows)
    setenv("WINEDLLDIR", libDir.c_str(), 1);
    setenv("WINEDLLDIR0", (std::string(binDir) + "/x86_64-windows").c_str(), 1);
    setenv("WINEDLLDIR1", binDir, 1);
    setenv("WINEDLLPATH", (std::string(binDir) + "/x86_64-windows:" + binDir).c_str(), 1);
    // Box64 日志: 0=关闭 (3=DEBUG 会产生海量 I/O)
    setenv("BOX64_LOG", "0", 1);
    setenv("BOX64_NOBANNER", "1", 1);
    setenv("BOX64_SHOWSEGV", "1", 1);
    // Box64 DYNAREC 性能调优 (对标 Winlator Performance 预设)
    // SAFEFLAGS=0: 不保留 x86 flags, 用原生 ARM flags, 最大提速
    setenv("BOX64_DYNAREC_SAFEFLAGS", "0", 1);
    // BIGBLOCK=3: 最大翻译块 (更多优化机会, 更少跳转)
    setenv("BOX64_DYNAREC_BIGBLOCK", "3", 1);
    // CALLRET=2: 用 ARM 原生 call/ret 代替模拟
    setenv("BOX64_DYNAREC_CALLRET", "2", 1);
    // FORWARD=1024: 最大推测执行步数
    setenv("BOX64_DYNAREC_FORWARD", "1024", 1);
    // WEAKBARRIER=2: 最弱化内存屏障 (x86_64 Wine 无需强序)
    setenv("BOX64_DYNAREC_WEAKBARRIER", "2", 1);
    // AVX=0: 禁用 AVX (Wine 不用 SIMD, 省去翻译)
    setenv("BOX64_AVX", "0", 1);
#ifdef __aarch64__
    // 标记 Box64 in-process 模式，供 x86_64 wine 代码 (process.c) 运行时判断
    setenv("USE_LIBBOX64", "1", 1);
#endif
    setenv("PATH", (std::string("/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:")
                    + binDir + "/x86_64-windows:" + binDir).c_str(), 1);
    setenv("TMPDIR", WINE_TMPDIR, 1);
    setenv("XDG_RUNTIME_DIR", WINE_PREFIX, 1);
    setenv("WINEDEBUG", "-all", 1);  // -all=关闭全部调试频道
}

extern "C" void Main(NativeChildProcess_Args args)
{
    OH_LOG_INFO(LOG_APP, "[WineChild] Main() ENTER pid=%{public}d entryParams=%{public}s",
                getpid(), args.entryParams ? args.entryParams : "(null)");

    // 1. 解析 entryParams: "binDir|arg0|arg1|..."
    const char* entryParams = args.entryParams ? args.entryParams : "";
    char* buf = strdup(entryParams);
    char* binDir = strtok(buf, "|");
    if (!binDir) { OH_LOG_ERROR(LOG_APP, "[WineChild] entryParams parse failed"); free(buf); return; }

    // 统计 argc, argv
    int argc = 0;
    char* argv[64];
    char* tok;
    while ((tok = strtok(nullptr, "|")) && argc < 63)
        argv[argc++] = tok;
    argv[argc] = nullptr;

    OH_LOG_INFO(LOG_APP, "[WineChild] binDir=%{public}s argc=%{public}d argv[0]=%{public}s",
                binDir, argc, argc > 0 ? argv[0] : "(none)");

    // 2. 设置 WINESERVERSOCKET (从父进程传来的 fd)
    if (args.fdList.head) {
        int sockFd = args.fdList.head->fd;
        char sockEnv[64];
        snprintf(sockEnv, sizeof(sockEnv), "%d", sockFd);
        setenv("WINESERVERSOCKET", sockEnv, 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] WINESERVERSOCKET=%{public}s", sockEnv);
    } else {
        OH_LOG_WARN(LOG_APP, "[WineChild] NO fdList.head, WINESERVERSOCKET NOT SET! "
                    "Child will try server_connect() -> start_server() -> posix_spawn to find wineserver");
    }

    // 3. 设置 Wine 环境变量
    setup_wine_env(binDir);

    // 确保 WINEPREFIX 目录存在
    mkdir(WINE_PREFIX, 0755);

    // Wine 期望从 bin 目录运行（相对路径等）
    chdir(binDir);

    // 启动 stderr reader：Wine 内部 write(2)/WINE_ERR → pipe → hilog + 文件
    int errPipe[2];
    pipe(errPipe);
    dup2(errPipe[1], STDERR_FILENO);
    close(errPipe[1]);
    mkdir(WINE_LOG_DIR, 0755);
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char logPath[128];
    snprintf(logPath, sizeof(logPath),
             WINE_LOG_DIR "/wine_stderr_%04d%02d%02d.log",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    int errFile = open(logPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    // 写分隔标记，确认本进程的日志从哪开始
    if (errFile >= 0) {
        dprintf(errFile, "\n=== PID=%d entryParams=%s ===\n", getpid(),
                args.entryParams ? args.entryParams : "(null)");
    }
    auto* ctx = new stderr_ctx{errPipe[0], errFile};
    pthread_t tid;
    pthread_create(&tid, nullptr, stderr_reader_thread, ctx);
    pthread_detach(tid);

#ifdef __aarch64__
    // ARM64 Pad: dlopen box64.so → Box64 模拟 x86_64 wine ELF
    OH_LOG_INFO(LOG_APP, "[WineChild] dlopen box64.so (ARM64 Box64 path)...");
    void* box64_lib = dlopen("box64.so", RTLD_NOW);
    if (!box64_lib) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlopen(box64.so) failed: %{public}s", dlerror());
        free(buf);
        return;
    }

    auto* box64_main = (int (*)(int, const char**, char**))dlsym(box64_lib, "box64_hmos_main");
    if (!box64_main) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlsym(box64_hmos_main) failed: %{public}s", dlerror());
        dlclose(box64_lib);
        free(buf);
        return;
    }

    // Build argv: ["box64-hmos-inprocess", "/path/to/wine", wine_args...]
    // argv[0] = box64 marker, argv[1] = x86_64 wine ELF full path, argv[2+] = wine arguments
    std::string winePath = std::string(binDir) + "/wine";
    int box64_argc = argc + 2;
    const char** box64_argv = new const char*[box64_argc + 1];
    box64_argv[0] = "box64";
    box64_argv[1] = winePath.c_str();
    for (int i = 0; i < argc; i++)
        box64_argv[i + 2] = argv[i];
    box64_argv[box64_argc] = nullptr;

    OH_LOG_INFO(LOG_APP, "[WineChild] calling box64_hmos_main argc=%{public}d wine=%{public}s",
                box64_argc, winePath.c_str());

    int box64_rc = box64_main(box64_argc, box64_argv, environ);
    OH_LOG_INFO(LOG_APP, "[WineChild] box64_hmos_main returned rc=%{public}d", box64_rc);

    delete[] box64_argv;
    // 不 dlclose(box64_lib): Box64 内部注册了 atexit handler / 包装函数指针,
    // dlclose 卸载 Box64 代码后, 后续 atexit 回调引用这些地址 → SIGSEGV。
    // 进程即将退出, OS 会回收一切, 无需手动卸载。
    free(buf);
#else
    // x86_64 Pad: dlopen ntdll.so → __wine_main (原生系统 linker 加载)
    OH_LOG_INFO(LOG_APP, "[WineChild] dlopen ntdll.so...");
    void* ntdll = dlopen("ntdll.so", RTLD_NOW);
    if (!ntdll) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlopen(ntdll.so) failed: %{public}s", dlerror());
        free(buf);
        return;
    }

    auto* wine_main = (void (*)(int, char**))dlsym(ntdll, "__wine_main");
    if (!wine_main) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlsym(__wine_main) failed: %{public}s", dlerror());
        dlclose(ntdll);
        free(buf);
        return;
    }

    OH_LOG_INFO(LOG_APP, "[WineChild] calling __wine_main");
    wine_main(argc, argv);

    // __wine_main → start_main_thread → server_init_process_done →
    // signal_start_thread (汇编实现, 劫持控制流跳入 Wine 代码)
    // → 正常情况下永不返回。走到这里说明 Wine 启动异常。
    // 不能 dlclose(ntdll): __wine_main 在调用 signal_start_thread 之前
    // 已执行 virtual_init/init_environment/server_init_process_done,
    // 这些可能注册了 atexit 回调 → dlclose 后退出时 SIGSEGV。
    OH_LOG_ERROR(LOG_APP, "[WineChild] __wine_main returned unexpectedly! Wine init FAILED");
    free(buf);
#endif
}

// wineserver 子进程入口
// entryParams: "binDir|wineserver|-f" (binDir 用于 chdir)
extern "C" void WineserverMain(NativeChildProcess_Args args)
{
    OH_LOG_INFO(LOG_APP, "[WineChild] WineserverMain() ENTER pid=%{public}d", getpid());

    const char* ep = args.entryParams ? args.entryParams : "";
    char* buf = strdup(ep);
    char* binDir = strtok(buf, "|");
    if (!binDir) { free(buf); return; }

    OH_LOG_INFO(LOG_APP, "[WineChild] ws step1: setting env...");
    setenv("WINEPREFIX", WINE_PREFIX, 1);
    setenv("WINEDEBUG", "-all", 1);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step2: mkdir...");
    mkdir(WINE_PREFIX, 0755);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step3: chdir(%{public}s)...", binDir);
    chdir(binDir);

    // stderr → pipe → hilog (appspawn 子进程中 stdio 不可用)
    int errPipe[2];
    pipe(errPipe);
    dup2(errPipe[1], STDERR_FILENO);
    close(errPipe[1]);
    auto* ctx = new stderr_ctx{errPipe[0], -1};  // 只写 hilog，不写文件
    pthread_t tid;
    pthread_create(&tid, nullptr, stderr_reader_thread, ctx);
    pthread_detach(tid);

    // 收集 argv: "wineserver" "-f" ...
    int argc2 = 0;
    char* argv2[8];
    char* t;
    while ((t = strtok(nullptr, "|")) && argc2 < 7)
        argv2[argc2++] = t;
    argv2[argc2] = nullptr;
    if (argc2 == 0) {
        argv2[0] = (char*)"wineserver";
        argv2[1] = (char*)"-f";
        argv2[2] = nullptr;
        argc2 = 2;
    }
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step4: argv argc=%d argv[0]=%s", argc2, argv2[0]);

#ifdef __aarch64__
    // ARM64 Pad: dlopen box64.so → Box64 模拟 x86_64 wineserver ELF
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step5: dlopen box64.so (ARM64 Box64 path)...");
    void* box64_lib = dlopen("box64.so", RTLD_NOW);
    if (!box64_lib) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlopen(box64.so) failed: %{public}s", dlerror());
        free(buf);
        return;
    }
    auto* box64_main = (int (*)(int, const char**, char**))dlsym(box64_lib, "box64_hmos_main");
    if (!box64_main) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlsym(box64_hmos_main) failed: %{public}s", dlerror());
        dlclose(box64_lib);
        free(buf);
        return;
    }

    // Box64 env for wineserver (x86_64 wineserver ELF inside Box64).
    std::string libDir = std::string(binDir) + "/x86_64-unix";
    setenv("BOX64_LD_LIBRARY_PATH", libDir.c_str(), 1);
    setenv("BOX64_LOG", "0", 1);
    setenv("BOX64_NOBANNER", "1", 1);
    setenv("BOX64_SHOWSEGV", "1", 1);
    setenv("BOX64_DYNAREC_SAFEFLAGS", "0", 1);
    setenv("BOX64_DYNAREC_BIGBLOCK", "3", 1);
    setenv("BOX64_DYNAREC_CALLRET", "2", 1);
    setenv("BOX64_DYNAREC_FORWARD", "1024", 1);
    setenv("BOX64_DYNAREC_WEAKBARRIER", "2", 1);
    setenv("BOX64_AVX", "0", 1);

    // Build argv: ["box64", "/path/to/wineserver", "wineserver", "-f", "-p"]
    std::string wsPath = std::string(binDir) + "/wineserver";
    int box64_argc = argc2 + 2;
    const char** box64_argv = new const char*[box64_argc + 1];
    box64_argv[0] = "box64";
    box64_argv[1] = wsPath.c_str();
    for (int i = 0; i < argc2; i++)
        box64_argv[i + 2] = argv2[i];
    box64_argv[box64_argc] = nullptr;

    OH_LOG_INFO(LOG_APP, "[WineChild] ws step6: calling box64_hmos_main argc=%d ws=%s",
                box64_argc, wsPath.c_str());
    int wsRc = box64_main(box64_argc, box64_argv, environ);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step7: box64_hmos_main returned rc=%d", wsRc);

    delete[] box64_argv;
    // 不 dlclose(box64_lib): 原因同上 (atexit handler 引用已卸载代码)
#else
    // x86_64 Pad: dlopen libwineserver.so (原生)
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step5: dlopen libwineserver.so...");
    void* h = dlopen("libwineserver.so", RTLD_NOW);
    if (!h) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlopen(libwineserver.so) failed: %{public}s", dlerror());
        free(buf);
        return;
    }
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step6: dlsym main...");
    auto* ws_main = (int (*)(int, char**))dlsym(h, "main");
    if (!ws_main) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlsym(main) failed: %{public}s", dlerror());
        dlclose(h);
        free(buf);
        return;
    }
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step7: calling ws_main(%d, [...]), WINEPREFIX=%{public}s",
                argc2, getenv("WINEPREFIX"));
    int wsRc = ws_main(argc2, argv2);
    // server_main() 是无限事件循环, 正常情况下永不返回
    // 不能 dlclose(h): server_main 内部已注册 atexit 回调,
    // dlclose 后进程退出时引用已卸载代码 → SIGSEGV.
    OH_LOG_ERROR(LOG_APP, "[WineChild] ws_main returned rc=%{public}d — wineserver died unexpectedly",
                  wsRc);
#endif
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step9: wineserver process exiting");
    free(buf);
}

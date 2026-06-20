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
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <dlfcn.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

// 从 stderr pipe 读取 Wine 内部日志，同时转发到 hilog 和文件
struct stderr_ctx { int fd; int fileFd; };
static void* stderr_reader_thread(void* arg) {
    auto* ctx = (stderr_ctx*)arg;
    char buf[4096];
    ssize_t n;
    while ((n = read(ctx->fd, buf, sizeof(buf) - 1)) > 0) {
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

    // 关键：系统 linker 搜索 ntdll.so 和依赖 .so 的路径
    setenv("LD_LIBRARY_PATH", libDir.c_str(), 1);

    setenv("HOME", "/storage/Users/currentUser/Download/app.hackeris.winehua", 1);
    setenv("XDG_RUNTIME_DIR", "/data/storage/el2/base/files/.wine", 1);
    setenv("WAYLAND_DISPLAY", "wine-wayland", 1);
    setenv("WINEPREFIX", "/data/storage/el2/base/files/.wine", 1);
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
    setenv("BOX64_LOG", "1", 1);
    setenv("BOX64_NOBANNER", "0", 1);
    setenv("PATH", (std::string("/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:")
                    + binDir + "/x86_64-windows:" + binDir).c_str(), 1);
    setenv("TMPDIR", "/data/storage/el2/base/cache", 1);
    setenv("XDG_RUNTIME_DIR", "/data/storage/el2/base/files/.wine", 1);
    setenv("WINEDEBUG", "+wineboot,+module", 1);
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
    mkdir("/data/storage/el2/base/files/.wine", 0755);

    // Wine 期望从 bin 目录运行（相对路径等）
    chdir(binDir);

    // 启动 stderr reader：Wine 内部 write(2)/WINE_ERR → pipe → hilog + 文件
    int errPipe[2];
    pipe(errPipe);
    dup2(errPipe[1], STDERR_FILENO);
    close(errPipe[1]);
    int errFile = open("/data/storage/el2/base/files/wine_stderr.log",
                       O_WRONLY | O_CREAT | O_APPEND, 0666);
    // 写分隔标记，确认本进程的日志从哪开始
    if (errFile >= 0) {
        dprintf(errFile, "\n=== PID=%d entryParams=%s ===\n", getpid(),
                args.entryParams ? args.entryParams : "(null)");
    }
    auto* ctx = new stderr_ctx{errPipe[0], errFile};
    pthread_t tid;
    pthread_create(&tid, nullptr, stderr_reader_thread, ctx);
    pthread_detach(tid);

    // 4. dlopen ntdll.so → __wine_main
    //    ntdll.so 在 libs/x86_64/ 中，系统 linker 自动搜索
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

    // __wine_main → start_main_thread 启动了 Wine 主线程后立即返回。
    // Main() 不能返回——appspawn 会立即杀进程，Wine 主线程没机会跑。
    // Wine 主线程跑完 wineboot 后会调用 ExitProcess(0) 杀掉整个进程，
    // 所以我们只需等待。设置 10 分钟超时兜底。
    OH_LOG_INFO(LOG_APP, "[WineChild] __wine_main returned, waiting for Wine main thread...");
    for (int i = 0; i < 600; i++) sleep(1);

    OH_LOG_INFO(LOG_APP, "[WineChild] timeout waiting for Wine exit");
    dlclose(ntdll);
    free(buf);
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
    setenv("WINEPREFIX", "/data/storage/el2/base/files/.wine", 1);
    setenv("WINEDEBUG", "+server", 1);  // wineserver 自身 debug 输出
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step2: mkdir...");
    mkdir("/data/storage/el2/base/files/.wine", 0755);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step3: chdir(%{public}s)...", binDir);
    chdir(binDir);

    // stderr → 直接写文件 (appspawn 子进程的副线程 hilog 不可用)
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step4: open stderr file...");
    int wsErrFile = open("/data/storage/el2/base/files/wine_stderr.log",
                         O_WRONLY | O_CREAT | O_APPEND, 0666);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step4: open ret=%{public}d errno=%{public}d",
                wsErrFile, wsErrFile < 0 ? errno : 0);
    if (wsErrFile >= 0) {
        dprintf(wsErrFile, "\n=== ws pid=%d ===\n", getpid());
        dup2(wsErrFile, STDERR_FILENO);
        close(wsErrFile);
    }

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
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step5: argv argc=%d argv[0]=%s", argc2, argv2[0]);

    OH_LOG_INFO(LOG_APP, "[WineChild] ws step6: dlopen libwineserver.so...");
    void* h = dlopen("libwineserver.so", RTLD_NOW);
    if (!h) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlopen(libwineserver.so) failed: %{public}s", dlerror());
        free(buf);
        return;
    }
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step7: dlsym main...");
    auto* ws_main = (int (*)(int, char**))dlsym(h, "main");
    if (!ws_main) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlsym(main) failed: %{public}s", dlerror());
        dlclose(h);
        free(buf);
        return;
    }
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step8: calling ws_main(%d, [...]), WINEPREFIX=%{public}s",
                argc2, getenv("WINEPREFIX"));
    int wsRc = ws_main(argc2, argv2);
    fprintf(stderr, "OHOS-WS-POST: ws_main returned rc=%d\n", wsRc);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step9: ws_main returned rc=%{public}d errno=%{public}d",
                wsRc, errno);
    // ws_main 不应返回，若返回则等一阵再退出方便抓日志
    for (int i = 0; i < 30; i++) sleep(1);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step10: wineserver process exiting");
    dlclose(h);
    free(buf);
}

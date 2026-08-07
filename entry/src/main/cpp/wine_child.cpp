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
#include <strings.h>
#include <string>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "wine_constants.h"
#include "wine_env.h"
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

static const char *default_winedebug_profile(void)
{
    return "-all";
}

static const char *midi_diag_winedebug_profile(void)
{
    return "-all,trace+driver,trace+winmm,trace+mmdevapi,"
           "trace+ohosaudio,warn+ohosaudio,warn+module";
}

static const char *sdl_audio_diag_winedebug_profile(void)
{
    return "-all,trace+driver,trace+winmm,trace+mmdevapi,"
           "warn+mmdevapi,err+mmdevapi,trace+dsound,warn+dsound,"
           "err+dsound,trace+ohosaudio,warn+ohosaudio,err+ohosaudio,"
           "warn+module,err+module";
}

static const char *basename_of_path(const char *path)
{
    const char *slash;

    if (!path || !path[0]) return path;
    slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

static bool is_audio_test_exe(int argc, char *argv[])
{
    const char *base;

    if (argc <= 0 || !argv[0]) return false;
    base = basename_of_path(argv[0]);
    return !strcasecmp(base, "winehua_audio_test.exe") ||
           !strcasecmp(base, "winehua_audio_test32.exe");
}

static bool is_sdl_audio_test_exe(int argc, char *argv[])
{
    const char *base;

    if (argc <= 0 || !argv[0]) return false;
    base = basename_of_path(argv[0]);
    return !strcasecmp(base, "mj_x86.exe") || !strcasecmp(base, "mj_x86d.exe");
}

static bool program_is(const char *program, const char *name)
{
    if (!program || !name) return false;
    if (!strcasecmp(program, name)) return true;

    char withExe[128];
    int n = snprintf(withExe, sizeof(withExe), "%s.exe", name);
    return n > 0 && n < (int)sizeof(withExe) && !strcasecmp(program, withExe);
}

static bool arg_equals(int argc, char *argv[], const char *value)
{
    for (int i = 1; i < argc; ++i)
        if (argv[i] && !strcasecmp(argv[i], value))
            return true;
    return false;
}

static bool arg_starts_with(int argc, char *argv[], const char *prefix)
{
    size_t prefixLen = prefix ? strlen(prefix) : 0;
    if (!prefixLen) return false;

    for (int i = 1; i < argc; ++i)
        if (argv[i] && !strncasecmp(argv[i], prefix, prefixLen))
            return true;
    return false;
}

static bool has_windows_dir(const char *path)
{
    return path && (strchr(path, '\\') || strchr(path, '/'));
}

static bool is_launchable_path(const char *path)
{
    const char *base;
    const char *dot;

    if (!has_windows_dir(path)) return false;
    base = basename_of_path(path);
    dot = strrchr(base, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".exe") || !strcasecmp(dot, ".bat") || !strcasecmp(dot, ".cmd");
}

static std::string trim_quotes(const char *value)
{
    std::string s = value ? value : "";
    while (!s.empty() && (s.front() == '"' || s.front() == '\'')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '"' || s.back() == '\'')) s.pop_back();
    return s;
}

static bool append_windows_path_tail(std::string *out, const std::string& tail)
{
    for (char ch : tail)
    {
        if (ch == '\\') out->push_back('/');
        else out->push_back(ch);
    }
    return !out->empty();
}

static const char *active_wine_prefix()
{
    const char *prefix = getenv("WINEPREFIX");
    return prefix && prefix[0] ? prefix : WINE_PREFIX;
}

static bool wine_directory_to_native(const char *path, const char *homeDir, std::string *out)
{
    std::string dir = trim_quotes(path);
    if (dir.size() >= 3 && dir[1] == ':' && (dir[2] == '\\' || dir[2] == '/'))
    {
        char drive = (char)tolower((unsigned char)dir[0]);
        std::string tail = dir.substr(3);
        if (drive == 'z')
        {
            if (!homeDir || !homeDir[0]) return false;
            *out = homeDir;
            if (!out->empty() && out->back() != '/') out->push_back('/');
            return append_windows_path_tail(out, tail);
        }
        if (drive == 'c')
        {
            *out = std::string(active_wine_prefix()) + "/drive_c/";
            return append_windows_path_tail(out, tail);
        }
        return false;
    }
    if (!dir.empty() && dir[0] == '/')
    {
        *out = dir;
        return true;
    }
    return false;
}

static bool wine_file_parent_to_native(const char *path, const char *homeDir, std::string *out)
{
    std::string file = trim_quotes(path);
    size_t slash = file.find_last_of("\\/");
    std::string dir;

    if (slash == std::string::npos) return false;
    dir = file.substr(0, slash);
    return wine_directory_to_native(dir.c_str(), homeDir, out);
}

static bool derive_launch_cwd(int argc, char *argv[], const char *homeDir, std::string *out)
{
    const char *program;

    if (argc <= 0 || !argv[0]) return false;
    program = basename_of_path(argv[0]);

    if (!strcasecmp(program, "wineboot") ||
        !strcasecmp(program, "explorer") ||
        !strcasecmp(program, "services.exe") ||
        !strcasecmp(program, "wineserver"))
        return false;

    if (!strcasecmp(program, "cmd.exe") || !strcasecmp(program, "cmd"))
    {
        for (int i = argc - 1; i >= 1; --i)
            if (is_launchable_path(argv[i]) && wine_file_parent_to_native(argv[i], homeDir, out))
                return true;
    }

    if (is_launchable_path(argv[0]) && wine_file_parent_to_native(argv[0], homeDir, out))
        return true;

    return false;
}

static const char *select_winedebug_profile(int argc, char *argv[])
{
    const char *override = getenv("WINEHUA_WINEDEBUG");

    if (override && override[0]) return override;
    if (is_audio_test_exe(argc, argv)) return midi_diag_winedebug_profile();
    if (is_sdl_audio_test_exe(argc, argv)) return sdl_audio_diag_winedebug_profile();
    return default_winedebug_profile();
}

static void setup_wine_env(const char* binDir, const char* homeDir, const char *winedebug)
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

    if (homeDir && homeDir[0])
        setenv("HOME", homeDir, 1);
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
    setenv("XKB_CONFIG_ROOT", (shareDir + "/X11/xkb").c_str(), 1);
    // WINEBINDIR/WINEUNIXDIR 覆盖 init_paths() 中基于 dladdr(ntdll.so) 推算的错误路径
    // ntdll.so 在 bundle libs 目录，而 PE DLL / Unix SO 数据都在 wine/bin/ 下
    setenv("WINEBINDIR", binDir, 1);   // wine/bin/
    setenv("WINEUNIXDIR", binDir, 1);  // wine/bin/ (含 x86_64-unix/x86_64-windows)
    setenv("WINEDLLDIR", libDir.c_str(), 1);
    setenv("WINEDLLDIR0", (std::string(binDir) + "/x86_64-windows").c_str(), 1);
    setenv("WINEDLLDIR1", (std::string(binDir) + "/i386-windows").c_str(), 1);
    setenv("WINEDLLDIR2", binDir, 1);
    {
        std::string dllPath = std::string(binDir) + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir;
#ifndef __aarch64__
        dllPath += ":/data/storage/el1/bundle/libs/x86_64";
#endif
        setenv("WINEDLLPATH", dllPath.c_str(), 1);
    }
    // Box64 日志: 0=关闭 (3=DEBUG 会产生海量 I/O)
    SetBox64PerfEnv();
#ifdef __aarch64__
    // 标记 Box64 in-process 模式，供 x86_64 wine 代码 (process.c) 运行时判断
    setenv("USE_LIBBOX64", "1", 1);
#endif
    setenv("PATH", (std::string("/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:")
                    + binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir).c_str(), 1);
    setenv("TMPDIR", WINE_TMPDIR, 1);
    setenv("XDG_RUNTIME_DIR", WINE_PREFIX, 1);
    std::string midiSoundfontPath = std::string(binDir) + "/../audio/winehua-gm.sf2";
    setenv("MIDI_SOUNDFONT_PATH", midiSoundfontPath.c_str(), 1);
    setenv("WINEDEBUG", winedebug && winedebug[0] ? winedebug : default_winedebug_profile(), 1);
    setenv("LANG", "zh_CN.UTF-8", 1);
    // musl 场景: setlocale 返回 "C", Wine 只读 LC_ALL 兜底, 必须与 LANG 同设
    setenv("LC_ALL", "zh_CN.UTF-8", 1);
}

static void apply_entry_param_env_overrides(const std::vector<std::string>& envOverrides)
{
    for (const std::string& envLine : envOverrides)
    {
        size_t sep = envLine.find('=');
        if (sep == std::string::npos || sep == 0)
        {
            OH_LOG_WARN(LOG_APP, "[WineChild] ignoring malformed __env token: %{public}s",
                        envLine.c_str());
            continue;
        }

        std::string key = envLine.substr(0, sep);
        std::string value = envLine.substr(sep + 1);
        setenv(key.c_str(), value.c_str(), 1);
        if (key == "WINEHUA_BOOTSTRAP_PHASE")
            OH_LOG_INFO(LOG_APP, "[WineChild] env override %{public}s=%{public}s",
                        key.c_str(), value.c_str());
    }
}

static void log_d3d_environment_summary()
{
    const char* backend = getenv("WINEHUA_D3D_BACKEND");
    const char* dxvkRoot = getenv("WINEHUA_DXVK_ROOT");
    const char* dxvkVersion = getenv("WINEHUA_DXVK_VERSION");
    const char* dllOverrides = getenv("WINEDLLOVERRIDES");
    const char* dllPath = getenv("WINEDLLPATH");
    const char* profile = getenv("WINEHUA_PERF_PROFILE");
    const char* logLevel = getenv("DXVK_LOG_LEVEL");
    const char* logPath = getenv("DXVK_LOG_PATH");
    const char* dumpPath = getenv("DXVK_SHADER_DUMP_PATH");
    const char* traceSampled = getenv("DXVK_WINEHUA_TRACE_SAMPLED");
    const char* traceFlow = getenv("DXVK_WINEHUA_TRACE_FLOW");
    const char* vnPerfSummary = getenv("VN_WINEHUA_PERF_SUMMARY");
    const char* vnPerfLog = getenv("VN_WINEHUA_PERF_LOG");
    const char* mesaLogLevel = getenv("MESA_LOG_LEVEL");
    const char* batchMappedFlush = getenv("DXVK_WINEHUA_BATCH_MAPPED_FLUSH");
    const char* rgba8SnormRt = getenv("DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT");

    std::string root = dxvkRoot && dxvkRoot[0] ? dxvkRoot : "";
    const std::string x64D3d11 = root + "/x64/d3d11.dll";
    const std::string x64Dxgi = root + "/x64/dxgi.dll";
    const std::string x86D3d11 = root + "/x86/d3d11.dll";
    const std::string x86Dxgi = root + "/x86/dxgi.dll";
    auto present = [](const std::string& path) {
        return !path.empty() && access(path.c_str(), R_OK) == 0 ? "present" : "missing";
    };
    OH_LOG_INFO(LOG_APP,
                "[WineChild] final D3D env backend=%{public}s dxvkVersion=%{public}s "
                "override=%{public}s dllPath=%{public}s root=%{public}s "
                "x64=(%{public}s,%{public}s) x86=(%{public}s,%{public}s) "
                "profile=%{public}s logLevel=%{public}s "
                "logPath=%{public}s dumpPath=%{public}s "
                "traceSampled=%{public}s traceFlow=%{public}s "
                "vnPerfSummary=%{public}s vnPerfLog=%{public}s mesaLogLevel=%{public}s "
                "batchMappedFlush=%{public}s rgba8SnormRt=%{public}s",
                backend ? backend : "", dxvkVersion ? dxvkVersion : "",
                dllOverrides ? dllOverrides : "", dllPath ? dllPath : "",
                dxvkRoot ? dxvkRoot : "", present(x64D3d11), present(x64Dxgi),
                present(x86D3d11), present(x86Dxgi),
                profile ? profile : "", logLevel ? logLevel : "",
                logPath ? logPath : "", dumpPath ? dumpPath : "",
                traceSampled ? traceSampled : "", traceFlow ? traceFlow : "",
                vnPerfSummary ? vnPerfSummary : "",
                vnPerfLog ? vnPerfLog : "",
                mesaLogLevel ? mesaLogLevel : "",
                batchMappedFlush ? batchMappedFlush : "",
                rgba8SnormRt ? rgba8SnormRt : "");
}

static void prepare_host_elf_environment(const char *homeDir)
{
    std::vector<std::string> removeKeys;
    for (char **entry = environ; entry && *entry; ++entry) {
        const char *separator = strchr(*entry, '=');
        if (!separator) continue;
        std::string key(*entry, (size_t)(separator - *entry));
        if (key.rfind("BOX64_", 0) == 0 || key.rfind("VN_", 0) == 0 ||
            key == "USE_LIBBOX64" || key == "VK_DRIVER_FILES" ||
            key == "VK_ICD_FILENAMES" || key == "MESA_LOADER_DRIVER_OVERRIDE" ||
            key == "LIBGL_DRIVERS_PATH")
            removeKeys.push_back(std::move(key));
    }
    for (const std::string& key : removeKeys) unsetenv(key.c_str());

    setenv("LD_LIBRARY_PATH",
           "/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64", 1);
    setenv("PATH", "/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin", 1);
    setenv("TMPDIR", WINE_TMPDIR, 1);
    if (homeDir && homeDir[0]) setenv("HOME", homeDir, 1);
}

extern "C" void Main(NativeChildProcess_Args args)
{
    OH_LOG_INFO(LOG_APP, "[WineChild] Main() ENTER pid=%{public}d entryParams=%{public}s",
                getpid(), args.entryParams ? args.entryParams : "(null)");

    // 1. 解析 entryParams: "homeDir|binDir|arg0|arg1|...|__env=KEY=VALUE|..."
    const char* entryParams = args.entryParams ? args.entryParams : "";
    char* buf = strdup(entryParams);
    char* homeDir = strtok(buf, "|");
    char* binDir = strtok(nullptr, "|");
    if (!binDir) { OH_LOG_ERROR(LOG_APP, "[WineChild] entryParams parse failed (no binDir)"); free(buf); return; }

    // 统计 argc, argv, 收集 __env= 覆盖
    int argc = 0;
    char* argv[64];
    char* tok;
    std::vector<std::string> envOverrides;
    while ((tok = strtok(nullptr, "|")) && argc < 63)
    {
        if (strncmp(tok, "__env=", 6) == 0)
        {
            envOverrides.emplace_back(tok + 6);
            continue;
        }
        argv[argc++] = tok;
    }
    argv[argc] = nullptr;

    bool guestElfMode = argc >= 2 && !strcmp(argv[0], "__winehua_guest_elf__");
    bool hostElfMode = argc >= 2 && !strcmp(argv[0], "__winehua_host_elf__");
    if (guestElfMode || hostElfMode) {
        for (int i = 0; i < argc; ++i) argv[i] = argv[i + 1];
        argc--;
        if (hostElfMode)
            OH_LOG_INFO(LOG_APP, "[HostChild] isolated native ELF=%{public}s", argv[0]);
        else
            OH_LOG_INFO(LOG_APP, "[GuestChild] isolated x86_64 ELF=%{public}s", argv[0]);
    }

    // 检查 __winehua_desktop__ 标记: 有 → desktop 模式, 需要传 env 给 wine
    {
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "__winehua_desktop__") == 0) {
                setenv("WINEHUA_DESKTOP_MODE", "1", 1);
                OH_LOG_INFO(LOG_APP, "[WineChild] __winehua_desktop__ → WINEHUA_DESKTOP_MODE=1");
                for (int j = i; j < argc; j++) argv[j] = argv[j + 1];
                argc--;
                break;
            }
        }
    }

    OH_LOG_INFO(LOG_APP, "[WineChild] homeDir=%{public}s binDir=%{public}s argc=%{public}d argv[0]=%{public}s",
                homeDir ? homeDir : "(null)", binDir, argc, argc > 0 ? argv[0] : "(none)");

    // 2. Step A: 设置 Wine 环境变量 baseline (硬编码默认值, 确保非 broker 路径可用)
    const char *winedebug = select_winedebug_profile(argc, argv);
    if (!hostElfMode) {
        OH_LOG_INFO(LOG_APP, "[WineChild] WINEDEBUG=%{public}s", winedebug);
        setup_wine_env(binDir, homeDir, winedebug);
    }

    // 3. 从父进程 fdList 读取 fds (按 fdName 区分)
    int wsSockFd = -1;   // wineserver fd (per-process)
    int audioFd = -1;    // audio bootstrap fd
    for (auto* node = args.fdList.head; node; node = node->next) {
        if (node->fdName && strcmp(node->fdName, "wine_audio_bootstrap") == 0) {
            audioFd = node->fd;
            OH_LOG_INFO(LOG_APP, "[WineChild] audio bootstrap fd=%{public}d", audioFd);
        } else if (node->fdName && strcmp(node->fdName, "wineserver_sock") == 0) {
            wsSockFd = node->fd;
            OH_LOG_INFO(LOG_APP, "[WineChild] wineserver fd=%{public}d (via Broker)", wsSockFd);
        } else {
            OH_LOG_INFO(LOG_APP, "[WineChild] fdList fd=%{public}d name=%{public}s (unrecognized, ignoring)",
                        node->fd, node->fdName ? node->fdName : "(null)");
        }
    }

    // Step B: entryParams 中的环境覆盖应用。
    apply_entry_param_env_overrides(envOverrides);

    if (!hostElfMode) log_d3d_environment_summary();

    // 覆盖 per-process fd 变量 (__env__ 中的是父进程 fd 号, 本进程无效)
    if (wsSockFd >= 0) {
        char wsEnv[64];
        snprintf(wsEnv, sizeof(wsEnv), "%d", wsSockFd);
        setenv("WINESERVERSOCKET", wsEnv, 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] WINESERVERSOCKET=%{public}d (own fd)", wsSockFd);
    }
    if (audioFd >= 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", audioFd);
        setenv("WINE_OHOS_AUDIO_ENABLE", "1", 1);
        setenv("WINE_OHOS_AUDIO_BOOTSTRAP_FD", buf, 1);
        setenv("WINE_OHOS_AUDIO_PROTOCOL_VERSION", "1", 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] AUDIO fd=%{public}d (own fd)", audioFd);
    }

    if (hostElfMode) {
        prepare_host_elf_environment(homeDir);
        const char *requestedCwd = getenv("WINEHUA_WORKING_DIRECTORY");
        if (requestedCwd && requestedCwd[0] && chdir(requestedCwd) != 0) {
            OH_LOG_ERROR(LOG_APP, "[HostChild] chdir(%{public}s) failed: %{public}s",
                         requestedCwd, strerror(errno));
            free(buf);
            return;
        }
        OH_LOG_INFO(LOG_APP,
                    "[HostChild] loading signed replay module for=%{public}s loader=system-vulkan",
                    argv[0]);
        void *module = dlopen("libwinehua_host_heaven_replay.so", RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            OH_LOG_ERROR(LOG_APP, "[HostChild] replay module load failed: %{public}s", dlerror());
            free(buf);
            return;
        }
        auto replayMain = reinterpret_cast<int (*)(int, char **)>(
            dlsym(module, "winehua_host_replay_main"));
        if (!replayMain) {
            OH_LOG_ERROR(LOG_APP, "[HostChild] replay entry lookup failed: %{public}s", dlerror());
            free(buf);
            return;
        }
        int replayResult = replayMain(argc, argv);
        OH_LOG_INFO(LOG_APP, "[HostChild] replay module returned rc=%{public}d", replayResult);
        free(buf);
        return;
    }

    // 确保 WINEPREFIX 目录存在
    mkdir(active_wine_prefix(), 0755);

    // Wine 期望从 bin 目录运行（相对路径等）
    chdir(binDir);

    // 启动 stderr reader：Wine 内部 write(2)/WINE_ERR → pipe → hilog + 文件
    {
        std::string launchCwd;
        const char *requestedCwd = getenv("WINEHUA_WORKING_DIRECTORY");
        bool hasCwd = requestedCwd && requestedCwd[0]
            ? wine_directory_to_native(requestedCwd, homeDir, &launchCwd)
            : derive_launch_cwd(argc, argv, homeDir, &launchCwd);
        if (hasCwd && chdir(launchCwd.c_str()) == 0)
            OH_LOG_INFO(LOG_APP, "[WineChild] cwd=%{public}s", launchCwd.c_str());
    }

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

    // Guest probes use the same isolated NCP/Box64 boundary as Wine but run a
    // concrete x86_64 OHOS ELF.  Keeping this marker out of argv ensures the
    // probe observes a normal argv[0] and cannot accidentally enter Wine.
    std::string winePath = guestElfMode ? std::string(argv[0]) : std::string(binDir) + "/wine";
    int box64_argc = guestElfMode ? argc + 1 : argc + 2;
    const char** box64_argv = new const char*[box64_argc + 1];
    box64_argv[0] = "box64";
    box64_argv[1] = winePath.c_str();
    if (guestElfMode) {
        for (int i = 1; i < argc; i++) box64_argv[i + 1] = argv[i];
    } else {
        for (int i = 0; i < argc; i++) box64_argv[i + 2] = argv[i];
    }
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
    if (guestElfMode) {
        execve(argv[0], argv, environ);
        OH_LOG_ERROR(LOG_APP, "[GuestChild] execve failed: %{public}s", strerror(errno));
        free(buf);
        return;
    }
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
// entryParams: "homeDir|binDir|wineserver|-f"... (homeDir 跳过)
extern "C" void WineserverMain(NativeChildProcess_Args args)
{
    OH_LOG_INFO(LOG_APP, "[WineChild] WineserverMain() ENTER pid=%{public}d", getpid());

    const char* ep = args.entryParams ? args.entryParams : "";
    char* buf = strdup(ep);
    strtok(buf, "|");              // skip homeDir
    char* binDir = strtok(nullptr, "|");
    if (!binDir) { free(buf); return; }

    // Collect argv and per-session environment overrides.  In particular this
    // keeps a clean smoke prefix isolated from the user's normal prefix.
    int argc2 = 0;
    char* argv2[8];
    char* t;
    std::vector<std::string> envOverrides;
    while ((t = strtok(nullptr, "|")))
    {
        if (!strncmp(t, "__env=", 6))
            envOverrides.emplace_back(t + 6);
        else if (argc2 < 7)
            argv2[argc2++] = t;
    }
    argv2[argc2] = nullptr;

    OH_LOG_INFO(LOG_APP, "[WineChild] ws step1: setting env...");
    setenv("WINEPREFIX", WINE_PREFIX, 1);
    setenv("WINEDEBUG", "-all", 1);
    apply_entry_param_env_overrides(envOverrides);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step2: mkdir prefix=%{public}s...", active_wine_prefix());
    mkdir(active_wine_prefix(), 0755);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step3: chdir(%{public}s)...", binDir);
    chdir(binDir);

    // stderr → pipe → hilog + 文件 (与普通 wine child 相同的落盘通道,
    // hilog 转发实际不可靠, wineserver 排障依赖文件)
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
    if (errFile >= 0) {
        dprintf(errFile, "\n=== PID=%d entryParams=%s ===\n", getpid(),
                args.entryParams ? args.entryParams : "(null)");
    }
    auto* ctx = new stderr_ctx{errPipe[0], errFile};
    pthread_t tid;
    pthread_create(&tid, nullptr, stderr_reader_thread, ctx);
    pthread_detach(tid);

    // 收集 argv: "wineserver" "-f" ...
    if (argc2 == 0) {
        argv2[0] = (char*)"wineserver";
        argv2[1] = (char*)"-f";
        argv2[2] = nullptr;
        argc2 = 2;
    }
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step4: argv argc=%{public}d argv[0]=%{public}s", argc2, argv2[0]);

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
    SetBox64PerfEnv();

    // Build argv: ["box64", "/path/to/wineserver", "wineserver", "-f", "-p"]
    std::string wsPath = std::string(binDir) + "/wineserver";
    int box64_argc = argc2 + 2;
    const char** box64_argv = new const char*[box64_argc + 1];
    box64_argv[0] = "box64";
    box64_argv[1] = wsPath.c_str();
    for (int i = 0; i < argc2; i++)
        box64_argv[i + 2] = argv2[i];
    box64_argv[box64_argc] = nullptr;

    OH_LOG_INFO(LOG_APP, "[WineChild] ws step6: calling box64_hmos_main argc=%{public}d ws=%{public}s",
                box64_argc, wsPath.c_str());
    int wsRc = box64_main(box64_argc, box64_argv, environ);
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step7: box64_hmos_main returned rc=%{public}d", wsRc);

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
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step7: calling ws_main(%{public}d, [...]), WINEPREFIX=%{public}s",
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

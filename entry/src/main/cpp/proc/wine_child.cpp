/**
 * wine_child.cpp - Wine 子进程入口 (libwine_child.so)
 *
 * 通过 OH_Ability_StartNativeChildProcess 启动，入口函数 Main()
 * (broker spawn 的唯一入口, 见 broker.cpp)。
 * 子进程从 appspawn 创建，全局状态干净，ntdll.so 首次 dlopen 构造正常执行。
 *
 * entryParams 格式: "homeDir|binDir|arg0|arg1|...|__env=K=V|..."
 *   binDir  = /data/storage/el2/base/files/wine/bin
 *   后续    = argv (如 "wineboot --init")
 *   特判    = argv[0]=="wineserver" → RunWineserver 本体 (纯 Unix ELF,
 *             不能走 wine loader 的 PE 解析)
 *
 * fdList 按 fdName 区分: wineserver_sock (wineserver socket, 设为
 * WINESERVERSOCKET) / wine_audio_bootstrap (音频 bootstrap)。
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
#include <sys/prctl.h>
#include "wine/wine_constants.h"
#include "wine_scheme.h"
#include "wine/wine_env.h"
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <link.h>
#include <sys/ucontext.h>

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
           "trace+ohosaudio,warn+ohosaudio,warn+module,"
           /* +seh: 音频链路真机崩溃时输出未处理异常的寄存器与回栈
            * (含模块名+偏移), 是定位 "mmdevapi 循环后页错误" 的唯一手段。 */
           "err+seh";
}

static const char *sdl_audio_diag_winedebug_profile(void)
{
    return "-all,trace+driver,trace+winmm,trace+mmdevapi,"
           "warn+mmdevapi,err+mmdevapi,trace+dsound,warn+dsound,"
           "err+dsound,trace+ohosaudio,warn+ohosaudio,err+ohosaudio,"
           "warn+module,err+module";
}

static const char *steam_webhelper_diag_winedebug_profile(void)
{
    /* Opt-in only: trace+dwrite emits tens of thousands of lines while CEF
       builds its font fallback list and can delay the first Steam window. */
    return "-all,trace+dwrite,warn+dwrite,err+seh,warn+module";
}

static bool steam_webhelper_diag_enabled(void)
{
    const char *value = getenv("WINEHUA_STEAM_WEBHELPER_DIAG");
    return value && !strcmp(value, "1");
}

static const char *basename_of_path(const char *path)
{
    const char *slash;

    if (!path || !path[0]) return path;
    slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

static void normalize_basename(const char *path, char *out, size_t out_size)
{
    const char *base = basename_of_path(path);
    size_t j = 0;

    if (!base || !out || !out_size) return;
    for (size_t i = 0; base[i] && j < out_size - 1; ++i)
    {
        char c = base[i];
        /* Match executable stems rather than their filesystem spelling.  The
           desktop smoke is named winehua_audio_smoke.exe, so keeping '_'
           makes the audio diagnostic probe miss its own profile. */
        if (c == ' ' || c == '\t' || c == '_' || c == '-') continue;
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        out[j++] = c;
    }
    out[j] = '\0';
}

static bool is_audio_test_exe(int argc, char *argv[])
{
    char norm[128];

    /* argv[0] 是 wine 加载器, 实际程序从 argv[1] 开始。
       保留 audio_test 兼容性，并匹配当前打包并由 SmokeRunner 启动的
       winehua_audio_smoke.exe；模糊匹配容忍空格、下划线与连字符变体。 */
    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i]) continue;
        normalize_basename(argv[i], norm, sizeof(norm));
        if (strstr(norm, "audiotest") != NULL ||
            strstr(norm, "audiosmoke") != NULL) return true;
    }
    return false;
}

static bool is_sdl_audio_test_exe(int argc, char *argv[])
{
    char norm[128];

    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i]) continue;
        normalize_basename(argv[i], norm, sizeof(norm));
        if (strstr(norm, "mjx86") != NULL) return true;
    }
    return false;
}

static bool is_steam_webhelper_exe(int argc, char *argv[])
{
    char norm[128];

    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i]) continue;
        normalize_basename(argv[i], norm, sizeof(norm));
        if (!strcasecmp(norm, "steamwebhelper.exe") || !strcasecmp(norm, "steamwebhelper"))
            return true;
    }
    return false;
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

static void apply_game_address_space_compatibility(int argc, char **argv)
{
    if (getenv("WINE_LARGE_ADDRESS_AWARE")) return;
    for (int i = 1; i < argc; ++i)
    {
        char name[128];
        if (!argv[i]) continue;
        normalize_basename(argv[i], name, sizeof(name));
        if (!program_is(name, "pal4")) continue;
        // PAL4 does not declare LARGE_ADDRESS_AWARE. Proton's forced 4 GB
        // override corrupts its UI initialization; honor the PE flag as the
        // previous Wine runtime did. Keep explicit overrides for diagnosis.
        setenv("WINE_LARGE_ADDRESS_AWARE", "0", 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] PAL4: honoring image address-space limit");
        return;
    }
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

static void refresh_wine_session_paths()
{
    const char *prefix = active_wine_prefix();
    std::string brokerPath(prefix);
    const size_t slash = brokerPath.find_last_of('/');

    setenv("XDG_RUNTIME_DIR", prefix, 1);
    if (slash == std::string::npos)
        brokerPath = ".wine_broker";
    else
        brokerPath.replace(slash + 1, std::string::npos, ".wine_broker");
    setenv("PROCESSBROKER", brokerPath.c_str(), 1);
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
    // entryParams 下发的 WINEDEBUG 若带通道 (非纯 "-all" 静默), 尊重之 —
    // BuildWineEnv 基线是 -all,+err,+winediag, 否则此处会把它覆盖回 -all,
    // 子进程出错时 stderr 全盲 (2026-08 方案② explorer 猝死零日志即因此)
    const char *existing = getenv("WINEDEBUG");
    if (existing && existing[0] && strcmp(existing, "-all") != 0) return existing;
    if (is_audio_test_exe(argc, argv)) return midi_diag_winedebug_profile();
    if (is_sdl_audio_test_exe(argc, argv)) return sdl_audio_diag_winedebug_profile();
    if (is_steam_webhelper_exe(argc, argv) && steam_webhelper_diag_enabled())
        return steam_webhelper_diag_winedebug_profile();
    return default_winedebug_profile();
}

static void setup_wine_env(const char* binDir, const char* homeDir, const char *winedebug)
{
    const std::string libDir = std::string(binDir) + "/" WINE_UNIX_SUBDIR;

    // 分歧键: 库搜索路径 (主进程 BuildWineEnv 按图形后端另算 runtimeLibPath)
#ifdef __aarch64__
    static constexpr const char* native_lib_dir = "arm64";
#else
    static constexpr const char* native_lib_dir = "x86_64";
#endif

#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
    // 方案② box64+wine (arm64 设备 + x86_64 wine 全转译): Wine .so 是 x86_64,
    // 由 box64 转译加载 (BOX64_LD_LIBRARY_PATH), 不放系统 LD_LIBRARY_PATH
    // (架构不符会加载失败)。LD_LIBRARY_PATH 只含 arm64 原生 .so。
    setenv("LD_LIBRARY_PATH",
           "/data/app/bin:/usr/local/lib:/system/lib64/module:/system/lib64", 1);
    setenv("BOX64_LD_LIBRARY_PATH", libDir.c_str(), 1);
    winehua::SetBox64PerfEnv();
    setenv("USE_LIBBOX64", "1", 1);  // 供 wine process.c 识别 in-process box64
#elif defined(__aarch64__)
    // 方案③ arm64 原生 wine: Harmony musl 会因 el2 目录拒绝整条
    // LD_LIBRARY_PATH 或跳过后续 el1 项。guest GL/Vulkan 已复制到 HAP
    // native libs, 只保留 el1 供系统 dlopen。
    setenv("LD_LIBRARY_PATH",
           (std::string("/data/storage/el1/bundle/libs/") + native_lib_dir).c_str(), 1);
#else
    // 方案① x86_64 原生 wine
    setenv("LD_LIBRARY_PATH",
           (libDir + ":/data/storage/el1/bundle/libs/" + native_lib_dir).c_str(), 1);
#endif

    // 公共基线: 与主进程 BuildWineEnv 同一张表 (wine_env_baseline.h), 增键只改一处
    winehua::ApplyEnvLinesToEnviron(winehua::BuildWineBaselineLines(
        {binDir, homeDir && homeDir[0] ? homeDir : "", WINE_PREFIX}));
    // 分歧键: 合成器 socket 固定名 (主进程侧是 sockName 参数)
    setenv("WAYLAND_DISPLAY", "wine-wayland", 1);
    // 读 WINEPREFIX 设 XDG_RUNTIME_DIR/PROCESSBROKER, 必须在基线 (WINEPREFIX) 之后
    refresh_wine_session_paths();
    // WINEBINDIR/WINEUNIXDIR 覆盖 init_paths() 中基于 dladdr(ntdll.so) 推算的错误路径
    // ntdll.so 在 bundle libs 目录，而 PE DLL / Unix SO 数据都在 wine/bin/ 下
    setenv("WINEBINDIR", binDir, 1);   // wine/bin/
    setenv("WINEUNIXDIR", binDir, 1);  // wine/bin/ (含 aarch64-unix/aarch64-windows 或 x86_64-*)
    // WINEDLLDIR*/WINEDLLPATH/PATH/TMPDIR/MIDI 已由公共基线表覆盖 (wine_env_baseline.h)
#if defined(__aarch64__) && !defined(WINEHUA_WINE_ARCH_IS_X86_64)
    // 方案③ arm64 原生 wine: 指定 FEX 模拟器 DLL (HODLL64), 由 ntdll loader 加载转译 x86_64 应用
    setenv("HODLL64", "libarm64ecfex.dll", 1);
    // 32 位 x86 应用: HODLL 由 wow64.dll get_cpu_dll_name() 读取, 转译 i386 PE。
    // 引擎可选: box=Box64 wowbox64.dll (默认), fex=FEX libwow64fex.dll。
    // Box64 是 arm64 产品基线，Steam/CEF 等高负载 Win32 子进程先走这条
    // 已验证路径；FEX 保留为显式诊断覆盖，避免把未完成的 FEX JIT fault
    // 路由作为所有用户进程的默认行为。
    // 注: HODLL 的最终选择放在 apply_entry_param_env_overrides() 之后
    // (见 select_wow64_backend)，否则从 Want 传入的 WINEHUA_WOW64_ENGINE
    // 会被这里的默认值覆盖。
#endif
    setenv("WINEDEBUG", winedebug && winedebug[0] ? winedebug : default_winedebug_profile(), 1);
}

// 32 位 CPU 后端选择 (HODLL)。必须放在 entryParams 的 __env 覆盖之后执行，
// 否则 Want 传入的 WINEHUA_WOW64_ENGINE 会被 setup_wine_env() 的默认值吃掉。
// 背景: Wine 的 wow64.dll 通过 get_cpu_dll_name() 读取 HODLL 加载后端, 并要求
// 后端导出完整 BTCpu* 契约 (含 BTCpuSuspendLocalThread)。wowbox64.dll 目前缺该
// 导出, 而 libwow64fex.dll 完整提供。
static bool is_steam_game_exe(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i]) continue;
        if (strstr(argv[i], "\\steamapps\\common\\") ||
            strstr(argv[i], "/steamapps/common/") ||
            strstr(argv[i], "\\steamapps\\downloading\\") ||
            strstr(argv[i], "/steamapps/downloading/"))
            return true;
    }
    return false;
}

// 大小写不敏感子串匹配 (仅 ASCII, Windows 路径大小写不敏感需要它)。
static bool contains_ascii_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return false;
    for (const char *p = hay; *p; ++p)
    {
        const char *h = p;
        const char *n = needle;
        while (*h && *n)
        {
            char hc = (*h >= 'A' && *h <= 'Z') ? char(*h + 32) : *h;
            char nc = (*n >= 'A' && *n <= 'Z') ? char(*n + 32) : *n;
            if (hc != nc) break;
            ++h;
            ++n;
        }
        if (!*n) return true;
    }
    return false;
}

// Steam 客户端进程树: steam.exe / bootstrap / bin\cef\steamwebhelper.exe /
// gldriverquery*.exe / steamerrorreporter*.exe 等。它们都在 ...\Steam\ 目录下。
// 为什么不能只依赖 WINEHUA_WOW64_ENGINE: Steam 自更新结束后由 updater 用干净环境
// 重新拉起 steam.exe, 那时会话里的 Want 环境已丢失, 于是整棵树退回 box64, 32 位
// 子进程 (steamsysinfo/gldriverquery/vulkandriverquery) 会立刻 SIGSEGV, 客户端再也
// 建不出 UI。按路径兜底可以保证任何一次自重启后仍然是 FEX。
static bool is_steam_client_exe(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i]) continue;
        if (contains_ascii_ci(argv[i], "\\steam\\") ||
            contains_ascii_ci(argv[i], "/steam/"))
            return true;
        /* 32 位时代的老坑重现 (2026-09-18): Steam 的 GPU 探测工具是用**相对路径**
         * 拉起的 —— `.\bin\gldriverquery.exe` / `.\bin\vulkandriverquery.exe`,
         * 路径里没有 "steam" 字样, 于是按路径兜底的 FEX 选择不命中, 落到 box64,
         * 这两个 i386 工具就立刻 SIGSEGV (与 09-16 记录的 steamsysinfo/
         * gldriverquery/vulkandriverquery 秒死同一现象)。
         * 兜底: 相对路径 + 启动目录在 Steam 树下 → 同样算 Steam 客户端进程。 */
        const char *path = argv[i];
        bool relative = (path[0] == '.' && (path[1] == '\\' || path[1] == '/')) ||
                        (!strchr(path, '\\') && !strchr(path, '/'));
        if (relative)
        {
            const char *cwd = getenv("WINEHUA_WORKING_DIRECTORY");
            if (cwd && (contains_ascii_ci(cwd, "\\steam") || contains_ascii_ci(cwd, "/steam")))
                return true;
        }
    }
    return false;
}

static void select_wow64_backend(int argc, char **argv)
{
#if defined(__aarch64__) && !defined(WINEHUA_WINE_ARCH_IS_X86_64)
    /* 2026-09-22: 32 位默认基座改为 FEX (libwow64fex.dll)。
     * 背景: PAL4 在 box64 下必须退到 BOX64_DYNAREC_BIGBLOCK=0 才能跑完场景,
     * 而默认档 (BIGBLOCK=3) 会卡死在加载; 同一游戏在 FEX 下直接以 48 FPS 通过
     * 加载并进入场景, 与用户实测"FEX 明显流畅"一致。box64 不再是 Proton 侧
     * 的运行基座, 但仍保留为显式回退: WINEHUA_WOW64_ENGINE=box。
     * steamapps\common 下曾经秒崩的现象尚未在 FEX 基座下复查, 先保留路径判定
     * 只作为日志线索, 不再据此强制换引擎。 */
    if (is_steam_client_exe(argc, argv))
    {
        setenv("HODLL", "libwow64fex.dll", 1);
        OH_LOG_INFO(LOG_APP,
                    "[WineChild] HODLL=libwow64fex.dll (steam-client override, exe=%{public}s)",
                    argv[1] ? argv[1] : "(null)");
        return;
    }

    /* 日志线索: steamapps\common 下的游戏过去在 FEX 上秒崩, 因而被强制 box64。
     * 现在基座换成 FEX, 这里只记录, 便于按 exe 归类崩溃样本。 */
    if (is_steam_game_exe(argc, argv))
        OH_LOG_INFO(LOG_APP, "[WineChild] steamapps game on FEX base (exe=%{public}s)",
                    argv[1] ? argv[1] : "(null)");

    const char *wow64_engine = getenv("WINEHUA_WOW64_ENGINE");

    if (wow64_engine && strcmp(wow64_engine, "box") == 0)
        setenv("HODLL", "wowbox64.dll", 1);
    else
        setenv("HODLL", "libwow64fex.dll", 1);
    OH_LOG_INFO(LOG_APP, "[WineChild] HODLL=%{public}s (WINEHUA_WOW64_ENGINE=%{public}s)",
                getenv("HODLL") ? getenv("HODLL") : "?", wow64_engine ? wow64_engine : "(unset)");
#endif
}

// Steam 客户端 (…\Steam\steam.exe) 的必备 CEF 参数是否已由 Want 给出。
static bool is_steam_bootstrap_exe(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!argv[i]) continue;
        if (contains_ascii_ci(argv[i], "\\steam\\steam.exe") ||
            contains_ascii_ci(argv[i], "/steam/steam.exe"))
            return true;
    }
    return false;
}

// Steam 客户端在 Wine 下的默认参数。缺了 -no-cef-sandbox 时 webhelper 的
// CEF 沙箱初始化会卡死: 日志停在 "Startup - webhelper launched pid", 连
// gpu-process/utility 子进程都不会起, 表现就是登录窗口永远不出现 (2026-09-17
// 21:52 / 22:17 两次复现), 所以这条无条件注入。
//
// -cef-force-gpu 在 2026-09-18 之后改成**可选**: 它会强制 CEF 走 GPU 后端,
// 与 WineHua 注入的 --disable-gpu* 直接冲突, 形成"半 GPU 状态" (P0-GL 计划 §18)。
// 默认不再注入, 让 CEF 自己选 backend (计划 §19 Run A "Clean");
// 需要复现历史 GPU 配置时用 WINEHUA_CEF_FORCE_GPU=1 (Run B)。
// 这里按 exe 路径补默认值, 与 select_wow64_backend 的路径兜底同理: Steam 自更新
// 结束后的 updater 自重启、以及应用 UI 不带参数的直接启动, 都不会传这些开关。
static void apply_steam_client_default_args(int &argc, char **argv)
{
    if (!is_steam_bootstrap_exe(argc, argv)) return;
    if (argc > 61) return;   // argv[64], 末尾保留 nullptr 槽位

    static char kForceGpu[] = "-cef-force-gpu";
    static char kDisableGpu[] = "-cef-disable-gpu";
    static char kNoSandbox[] = "-no-cef-sandbox";

    bool hasSandboxSwitch = arg_equals(argc, argv, kNoSandbox) ||
                            arg_equals(argc, argv, "-cef-disable-sandbox");
    if (!hasSandboxSwitch)
    {
        argv[argc++] = kNoSandbox;
        OH_LOG_INFO(LOG_APP, "[WineChild] steam default arg injected: %{public}s", kNoSandbox);
    }
    const char* forceGpu = getenv("WINEHUA_CEF_FORCE_GPU");
    if (forceGpu && forceGpu[0] == '1' && !arg_equals(argc, argv, kForceGpu))
    {
        argv[argc++] = kForceGpu;
        OH_LOG_INFO(LOG_APP, "[WineChild] steam default arg injected: %{public}s", kForceGpu);
    }
    /* WINEHUA_CEF_STEAM_DISABLE_GPU=1 -> Steam 自己的 -cef-disable-gpu。
     * 32 位客户端当初能出画面时的 GPU 进程命令行就是 `--use-gl=disabled`
     * (2026-09-17 22:25 webhelper.txt), 即"关掉 GL、走非 GL 路径"; 复刻到 win64 时
     * 只加这一条 Steam 原生开关, 不再叠 WineHua 的 --disable-gpu-compositing
     * (实测二者叠加会把 CEF 推成半 GPU 状态: 窗口整体黑 + renderer 反复崩)。 */
    const char* disableGpu = getenv("WINEHUA_CEF_STEAM_DISABLE_GPU");
    if (disableGpu && disableGpu[0] == '1' && !arg_equals(argc, argv, kDisableGpu))
    {
        argv[argc++] = kDisableGpu;
        OH_LOG_INFO(LOG_APP, "[WineChild] steam default arg injected: %{public}s", kDisableGpu);
    }
    argv[argc] = nullptr;
}

/* 2026-09-20 诊断: 摘掉 webhelper 系 (browser / gpu-process / renderer / utility) 的
 * 进程内崩溃处理器。
 *
 * 背景 (ROUND3 §9.6): renderer / gpu-process 以 signal=11 退出, 但 wine_stderr 里
 * **没有对应的 sig=11 记录**, 只有崩溃前成串被 FEX 正常处理的 sig=7(未对齐) 故障
 * —— 说明致命 fault 被 CEF 自己的崩溃处理器截获后再终止进程, 我们的 SMC/early-fault
 * 看不到"为什么崩"。关掉 breakpad/crash-reporter 后, fault 会回到 Wine 信号路径,
 * 现成的 [SMC-MAP]/[SMC-stack]/[fault-map] 归属就能直接给出模块与调用来源。
 *
 * 默认关闭, 只有 WINEHUA_CEF_NO_CRASH_HANDLER=1 才注入; 只影响崩溃报告, 不影响渲染。 */
static void apply_steam_webhelper_diag_args(int &argc, char **argv)
{
    const char* flag = getenv("WINEHUA_CEF_NO_CRASH_HANDLER");
    if (!flag || flag[0] != '1') return;
    if (!is_steam_webhelper_exe(argc, argv)) return;
    if (argc > 60) return;

    static char kNoBreakpad[] = "--disable-breakpad";
    static char kNoCrashReporter[] = "--disable-crash-reporter";
    static char kNoErrDialogs[] = "--noerrdialogs";

    if (!arg_equals(argc, argv, kNoBreakpad)) argv[argc++] = kNoBreakpad;
    if (!arg_equals(argc, argv, kNoCrashReporter)) argv[argc++] = kNoCrashReporter;
    if (!arg_equals(argc, argv, kNoErrDialogs)) argv[argc++] = kNoErrDialogs;
    argv[argc] = nullptr;
    OH_LOG_INFO(LOG_APP,
                "[WineChild] webhelper diag args injected: breakpad/crash-reporter disabled");
}

// ---- 前缀字体/代码页自愈 (产品化, 2026-09-18) ----
// 为什么必须做进代码: 本 runtime 里 Wine 的默认 UI 字体链是断的 ——
// system.reg 的 FontSubstitutes 把 Arial / Calibri / Candara / Comic Sans MS / … 指向
// **并不存在的** family "HarmonyOS Sans SC" (HarmonyOS_Sans_SC.ttf 的真实注册名是
// "鸿蒙黑体"), 而 Nls\CodePage 段经常在 Wine 重写注册表时被抹掉。
// 实测症状 (32 位客户端先踩到, win64 客户端同样):
//   src\vgui2\src\surface_gdiwin32.cpp (1336) : winFont   -> 界面无文字 / 客户端卡在启动画面
//   blink remote_font_face_source.cc NOTREACHED          -> CEF UI 起不来
// 之前只在一台设备的 prefix 里手工修过, 换机或重置 prefix 就会重现, 所以放在会话启动时补齐。
static const char kWineHuaFontTarget[] = "\\x9e3f\\x8499\\x9ed1\\x4f53"; // 鸿蒙黑体 (registry 转义)

static const char* const kWineUiFontFamilies[] = {
    "Arial", "Arial Black", "Arial Narrow", "Calibri", "Cambria", "Candara",
    "Chicago", "Comic Sans MS", "Consolas", "Constantia", "Corbel", "Courier",
    "Courier New", "DengXian", "FangSong", "Fixedsys", "Geneva", "Georgia",
    "Helvetica", "KaiTi", "Lucida Console", "Marlett", "Meiryo",
    "Microsoft Sans Serif", "MS Gothic", "MS Mincho", "MS Sans Serif",
    "MS Shell Dlg", "MS UI Gothic", "Motiva Sans", "Palatino Linotype", "Roboto",
    "Segoe UI", "Segoe UI Semibold", "SimHei", "SimSun", "Tahoma",
    "Times New Roman", "Trebuchet MS", "Verdana", "Yu Gothic", "sans", "sans-serif",
};

struct WineHuaRegEntry { const char* key; const char* value; };

static bool winehua_read_text_file(const char* path, std::string& out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char buf[65536];
    ssize_t n;
    out.clear();
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(fd);
    return !out.empty();
}

static bool winehua_write_text_file(const char* path, const std::string& data)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size())
    {
        ssize_t w = write(fd, data.data() + off, data.size() - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
    return off == data.size();
}

// 已知"指向不存在 family"的值: 这些目标名在本 runtime 里没有对应字体。
static bool winehua_font_target_is_broken(const std::string& value)
{
    return value.find("HarmonyOS Sans") != std::string::npos ||
           value.find("Noto Sans CJK") != std::string::npos ||
           value.find("Noto Serif") != std::string::npos ||
           value.find("Noto Sans Mono") != std::string::npos;
}

// 在 [section] 段内补齐缺失项 / 纠正失效目标; 段不存在且 add_missing 时追加该段。
static int winehua_patch_reg_section(std::string& text, const char* section,
                                     const WineHuaRegEntry* entries, size_t count,
                                     bool add_missing, bool replace_broken)
{
    const std::string header = std::string("[") + section + "]";
    size_t pos = text.find(header);
    if (pos == std::string::npos)
    {
        if (!add_missing) return 0;
        std::string add = "\n" + header + " 0\n#time=0\n";
        for (size_t i = 0; i < count; ++i)
            add += std::string("\"") + entries[i].key + "\"=\"" + entries[i].value + "\"\n";
        text += add;
        return (int)count;
    }

    size_t body = text.find('\n', pos);
    if (body == std::string::npos) return 0;
    body += 1;
    size_t end = text.find("\n[", body);
    if (end == std::string::npos) end = text.size();
    std::string body_text = text.substr(body, end - body);
    std::string additions;
    int changed = 0;

    for (size_t i = 0; i < count; ++i)
    {
        const std::string key = std::string("\"") + entries[i].key + "\"=\"";
        size_t kp = body_text.find(key);
        if (kp == std::string::npos)
        {
            if (!add_missing) continue;
            if (!additions.empty()) additions += "\n";
            additions += key + entries[i].value + "\"";
            changed++;
            continue;
        }
        if (!replace_broken) continue;
        size_t vstart = kp + key.size();
        size_t vend = body_text.find('"', vstart);
        if (vend == std::string::npos) continue;
        if (winehua_font_target_is_broken(body_text.substr(vstart, vend - vstart)))
        {
            body_text.replace(vstart, vend - vstart, entries[i].value);
            changed++;
        }
    }
    if (!changed) return 0;
    text = text.substr(0, body) + body_text + additions + text.substr(end);
    return changed;
}

// 会话启动时自愈 prefix: 字体替换链 + 中文代码页。幂等, 只在有改动时写回。
static void ensure_prefix_fonts_and_codepage()
{
    const char* prefix = getenv("WINEPREFIX");
    if (!prefix || !*prefix) return;

    WineHuaRegEntry font_entries[sizeof(kWineUiFontFamilies) / sizeof(kWineUiFontFamilies[0])];
    for (size_t i = 0; i < sizeof(kWineUiFontFamilies) / sizeof(kWineUiFontFamilies[0]); ++i)
    {
        font_entries[i].key = kWineUiFontFamilies[i];
        font_entries[i].value = kWineHuaFontTarget;
    }
    const size_t font_count = sizeof(kWineUiFontFamilies) / sizeof(kWineUiFontFamilies[0]);

    std::string path = std::string(prefix) + "/user.reg";
    std::string text;
    if (winehua_read_text_file(path.c_str(), text))
    {
        int n = winehua_patch_reg_section(text, "Software\\\\Wine\\\\Fonts\\\\Replacements",
                                         font_entries, font_count, true, true);
        if (n > 0)
        {
            std::string backup = path + ".winehua.bak";
            int bfd = open(backup.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (bfd >= 0) close(bfd);
            if (winehua_write_text_file(path.c_str(), text))
                OH_LOG_INFO(LOG_APP, "[WineChild] font Replacements patched (%{public}d entries)", n);
            else
                OH_LOG_WARN(LOG_APP, "[WineChild] font Replacements write failed");
        }
    }

    static const WineHuaRegEntry codepage_entries[] = {
        { "ACP", "936" }, { "OEMCP", "936" }, { "MACCP", "10008" },
    };
    path = std::string(prefix) + "/system.reg";
    text.clear();
    if (winehua_read_text_file(path.c_str(), text))
    {
        int n = winehua_patch_reg_section(
            text, "Software\\\\Microsoft\\\\Windows NT\\\\CurrentVersion\\\\FontSubstitutes",
            font_entries, font_count, false, true);
        n += winehua_patch_reg_section(text, "System\\\\CurrentControlSet\\\\Control\\\\Nls\\\\CodePage",
                                       codepage_entries,
                                       sizeof(codepage_entries) / sizeof(codepage_entries[0]),
                                       true, false);
        if (n > 0)
        {
            std::string backup = path + ".winehua.bak";
            int bfd = open(backup.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (bfd >= 0) close(bfd);
            if (winehua_write_text_file(path.c_str(), text))
                OH_LOG_INFO(LOG_APP, "[WineChild] system.reg font/codepage patched (%{public}d entries)", n);
            else
                OH_LOG_WARN(LOG_APP, "[WineChild] system.reg write failed");
        }
    }
}

// Recover files hidden by older WineHua builds. DLL selection belongs to Wine's
// builtin override, not the Steam installation: removing a verified client file
// makes the updater download it again on every launch. Recover the backup before
// starting the Steam updater. Hard links are denied by the device's sandbox.
static void restore_shadowed_vulkan_loaders()
{
    const char* prefix = getenv("WINEPREFIX");
    if (!prefix || !*prefix) return;

    static const char* kShadowDirs[] = {
        "/drive_c/Program Files (x86)/Steam/bin/cef/cef.win64",
        "/drive_c/Program Files (x86)/Steam/bin/cef",
        "/drive_c/Program Files/Steam/bin/cef/cef.win64",
        "/drive_c/Program Files/Steam/bin/cef",
    };

    for (const char* rel : kShadowDirs)
    {
        std::string path = std::string(prefix) + rel + "/vulkan-1.dll";
        if (access(path.c_str(), F_OK) == 0) continue;
        std::string backup = path + ".winehua-shadow";
        if (rename(backup.c_str(), path.c_str()) == 0)
            OH_LOG_INFO(LOG_APP, "[WineChild] restored Steam Vulkan loader path=%{public}s", path.c_str());
        else if (errno != ENOENT && errno != EEXIST)
            OH_LOG_WARN(LOG_APP, "[WineChild] Vulkan loader restore failed path=%{public}s errno=%{public}d",
                        path.c_str(), errno);
    }
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
        // WINEDEBUG 的决策点在本文件的 select_winedebug_profile (它才知道是不是
        // audio 诊断 exe、有没有 WINEHUA_WINEDEBUG 覆盖), 通用 __env 覆盖会让
        // 它恒等于 App 侧下发的值, profile 选择失效。显式诊断请走 WINEHUA_WINEDEBUG。
        if (key == "WINEDEBUG")
        {
            OH_LOG_INFO(LOG_APP, "[WineChild] __env WINEDEBUG ignored: %{public}s",
                        value.c_str());
            continue;
        }
        setenv(key.c_str(), value.c_str(), 1);
        if (key == "WINEHUA_BOOTSTRAP_PHASE" || key.rfind("BOX64_DYNAREC_", 0) == 0)
            OH_LOG_INFO(LOG_APP, "[WineChild] env override %{public}s=%{public}s",
                        key.c_str(), value.c_str());
    }
}

#ifdef __aarch64__
static constexpr const char kChildNativeLibDir[] = "arm64";
#else
static constexpr const char kChildNativeLibDir[] = "x86_64";
#endif

static bool path_has_component(const std::string& path, const std::string& dir)
{
    if (dir.empty() || path.empty()) return false;
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        if (path.compare(start, end - start, dir) == 0) return true;
        if (end == path.size()) break;
        start = end + 1;
    }
    return false;
}

static void append_path_component(std::string& path, const std::string& dir)
{
    if (dir.empty() || path_has_component(path, dir)) return;
    if (!path.empty()) path += ':';
    path += dir;
}

static void replace_all(std::string& haystack, const char* from, const char* to)
{
    if (!from || !from[0] || !to) return;
    const size_t fromLen = strlen(from);
    const size_t toLen = strlen(to);
    size_t pos = 0;
    while ((pos = haystack.find(from, pos)) != std::string::npos)
    {
        haystack.replace(pos, fromLen, to);
        pos += toLen;
    }
}

/* Parent __env may overlay WINEDLLPATH with DXVK PE dirs and drop the HAP
 * native-lib directory where wineohos.so is packaged. ntdll redirects dll_dir
 * to WINEUNIXDIR (wine/bin), so unixlib search must still include bundle libs. */
static void reassert_arch_wine_runtime_env(const char* binDir)
{
    if (!binDir || !binDir[0]) return;

    const std::string unixDir = std::string(binDir) + "/" WINE_UNIX_SUBDIR;
    const std::string peDir = std::string(binDir) + "/" WINE_PE_SUBDIR;
    const std::string i386Dir = std::string(binDir) + "/i386-windows";
    const std::string bundleDir = std::string("/data/storage/el1/bundle/libs/") + kChildNativeLibDir;

    setenv("WINEBINDIR", binDir, 1);
    setenv("WINEUNIXDIR", binDir, 1);
    setenv("WINEDLLDIR", unixDir.c_str(), 1);

    const char* dllDir0 = getenv("WINEDLLDIR0");
    if (!dllDir0 || !dllDir0[0] ||
        (strstr(dllDir0, "x86_64-windows") != nullptr &&
         strstr(dllDir0, "/dxvk/") == nullptr))
        setenv("WINEDLLDIR0", peDir.c_str(), 1);

    const char* existing = getenv("WINEDLLPATH");
    std::string dllPath = existing ? existing : "";
#ifdef __aarch64__
    replace_all(dllPath, "x86_64-windows", WINE_PE_SUBDIR);
    replace_all(dllPath, "x86_64-unix", WINE_UNIX_SUBDIR);
#endif
    append_path_component(dllPath, peDir);
    append_path_component(dllPath, i386Dir);
    append_path_component(dllPath, binDir);
    append_path_component(dllPath, unixDir);
    append_path_component(dllPath, bundleDir);
    setenv("WINEDLLPATH", dllPath.c_str(), 1);

#if defined(__aarch64__) && !defined(WINEHUA_WINE_ARCH_IS_X86_64)
    /* Parent __env still serializes el2 guest_gfx/wine/bin first. That
     * poisons musl ICD/loader scans on 方案③; force the el1-only path.
     * 方案② keeps the box64 host LD_LIBRARY_PATH from setup_wine_env. */
    setenv("LD_LIBRARY_PATH", bundleDir.c_str(), 1);
#endif

    const char* path = getenv("PATH");
    if (path && strstr(path, "x86_64-windows"))
    {
        std::string p = path;
        replace_all(p, "x86_64-windows", WINE_PE_SUBDIR);
        setenv("PATH", p.c_str(), 1);
    }

    OH_LOG_INFO(LOG_APP,
                "[WineChild] reassert WINEDLLDIR=%{public}s WINEDLLDIR0=%{public}s "
                "WINEDLLPATH=%{public}s LD_LIBRARY_PATH=%{public}s",
                unixDir.c_str(),
                getenv("WINEDLLDIR0") ? getenv("WINEDLLDIR0") : "",
                dllPath.c_str(),
                getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "");
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
    const std::string overlay64 = root + "/arm64x";
    const bool useArm64x = !overlay64.empty() && access((overlay64 + "/d3d11.dll").c_str(), R_OK) == 0;
    const std::string dxgi64Dir = useArm64x ? overlay64 : (root + "/x64");
    const std::string x64D3d11 = dxgi64Dir + "/d3d11.dll";
    const std::string x64Dxgi = dxgi64Dir + "/dxgi.dll";
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
    const char* ldPath = getenv("LD_LIBRARY_PATH");
    const char* icd = getenv("VK_ICD_FILENAMES");
    const char* drivers = getenv("VK_DRIVER_FILES");
    OH_LOG_INFO(LOG_APP,
                "[WineChild] vulkan scan LD_LIBRARY_PATH=%{public}s "
                "VK_ICD_FILENAMES=%{public}s VK_DRIVER_FILES=%{public}s",
                ldPath ? ldPath : "",
                icd ? icd : "",
                drivers ? drivers : "");
#ifdef __aarch64__
    void* vulkan = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan)
    {
        const char* err = dlerror();
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlopen libvulkan.so.1 failed: %{public}s",
                     err ? err : "(null)");
    }
    else
    {
        Dl_info info;
        void* sym = dlsym(vulkan, "vkCreateInstance");
        if (sym && dladdr(sym, &info) && info.dli_fname)
            OH_LOG_INFO(LOG_APP, "[WineChild] libvulkan.so.1 loaded from %{public}s",
                        info.dli_fname);
        else
            OH_LOG_INFO(LOG_APP, "[WineChild] libvulkan.so.1 loaded, path unknown");
        dlclose(vulkan);
    }
#endif
}

// wineserver 本体 (文件后部定义); Main 截获 argv[0]=="wineserver" 转入
static void RunWineserver(char* binDir, int argc2, char** argv2,
                          const std::vector<std::string>& envOverrides,
                          const char* entryParamsForLog);

// ---- TEMP-DIAG(STALL-DUMP): 空闲卡死时的线程等待点观测 (2026-09-18) ----
// 背景: win64 CEF browser 在 "CreateBrowser → AfterCreated" 之后静止 (状态 S, 不烧 CPU)。
// 外部读 /proc/<pid>/stack 被 SELinux 拒绝; ITIMER_PROF 采样器只在烧 CPU 时触发, 对
// "空闲等待"零输出。这个看门狗在**进程内部**观测: 若一个窗口期内本进程 utime+stime 完全
// 没有增长, 就把每个线程的 comm/wchan/内核栈落到 stderr (即 wine_stderr 文件)。
// 开关: WINEHUA_STALL_DUMP=<秒窗口> (默认 20), 0/未设 = 关闭。
static void WineHuaStallSampleAllThreads(void);   // 定义在 EARLY-FAULT 段之后 (要用模块表)
static long WineHuaProcCpuTicks()
{
    char buf[1024];
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!n) return -1;
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    if (!p) return -1;
    p++;
    long vals[16];
    int i = 0;
    for (char *tok = strtok(p, " "); tok && i < 16; tok = strtok(nullptr, " "))
        vals[i++] = atol(tok);
    if (i < 15) return -1;
    return vals[11] + vals[12];   /* utime + stime */
}

static void WineHuaDumpSelfThreads(const char *why)
{
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    char line[512];
    int n = snprintf(line, sizeof(line), "[stall-dump] pid=%d why=%s begin\n", getpid(), why);
    if (n > 0) write(2, line, (size_t)n);
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr)
    {
        if (ent->d_name[0] == '.') continue;
        char comm[64] = {0}, wchan[64] = {0}, path[96];
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", ent->d_name);
        FILE *f = fopen(path, "r");
        if (f) { if (!fgets(comm, sizeof(comm), f)) comm[0] = 0; fclose(f); }
        char *nl = strchr(comm, '\n'); if (nl) *nl = 0;
        snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", ent->d_name);
        f = fopen(path, "r");
        if (f) { if (!fgets(wchan, sizeof(wchan), f)) wchan[0] = 0; fclose(f); }
        nl = strchr(wchan, '\n'); if (nl) *nl = 0;
        n = snprintf(line, sizeof(line), "[stall-dump] tid=%s comm=%s wchan=%s\n",
                     ent->d_name, comm, wchan);
        if (n > 0) write(2, line, (size_t)n);
        snprintf(path, sizeof(path), "/proc/self/task/%s/stack", ent->d_name);
        f = fopen(path, "r");
        if (f)
        {
            int k = 0;
            while (k < 6 && fgets(line, sizeof(line), f))
            {
                size_t len = strlen(line);
                if (len && write(2, line, len) > 0) k++;
            }
            fclose(f);
        }
    }
    closedir(d);
    const char *end = "[stall-dump] end\n";
    write(2, end, strlen(end));
}

static void *WineHuaStallWatchdog(void *)
{
    const char *env = getenv("WINEHUA_STALL_DUMP");
    int window = (env && env[0] && env[0] != '0') ? atoi(env) : 0;
    if (window <= 0) return nullptr;
    if (window > 120) window = 120;
    int dumps = 0;
    long prev = WineHuaProcCpuTicks();
    for (;;)
    {
        sleep((unsigned)window);
        long cur = WineHuaProcCpuTicks();
        /* 判据用"CPU 占用率低"而不是"零增长": 看门狗自身 (fopen/fread) 也会推进 utime/stime,
         * 浏览器还会周期性地烧掉一点点 CPU, 用零增长会把真正卡住的进程漏掉 (实测: 只有
         * gpu/storage 被 dump, 浏览器从没触发)。HZ 按 100 估: 窗口内 ticks 增量 < window*1
         * (即 <10% 一个核) 就算"卡住"。*/
        if (cur >= 0 && prev >= 0 && (cur - prev) < window && dumps < 3)
        {
            dumps++;
            WineHuaDumpSelfThreads("idle-no-cpu-progress");
            WineHuaStallSampleAllThreads();
        }
        prev = cur;
    }
    return nullptr;
}

// ---- TEMP-DIAG(EARLY-FAULT) ----
// 宿主 linker (ld-musl) 内的崩溃发生在 ntdll 安装 sigchain 之前，Wine 侧看不到。
// 这里在 Main 最早处挂一个"只记录、不接管"的 action: 打印 sig/si_addr/pc/lr/sp
// 并对栈做一次有界扫描(把命中宿主模块的地址解析成 模块+偏移)。
namespace {
struct OhosEarlyMod { uintptr_t start; uintptr_t end; char name[96]; };
struct OhosEarlyCtx { OhosEarlyMod mods[80]; int count; };

int OhosEarlyPhdrCb(struct dl_phdr_info* info, size_t /*size*/, void* data) {
    auto* ctx = static_cast<OhosEarlyCtx*>(data);
    if (ctx->count >= 80) return 0;
    uintptr_t start = 0, end = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type != PT_LOAD) continue;
        uintptr_t s = (uintptr_t)info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        uintptr_t e = s + info->dlpi_phdr[i].p_memsz;
        if (!start || s < start) start = s;
        if (e > end) end = e;
    }
    if (!end) return 0;
    OhosEarlyMod& m = ctx->mods[ctx->count++];
    m.start = start;
    m.end = end;
    const char* n = (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name : "<main>";
    snprintf(m.name, sizeof(m.name), "%s", n);
    return 0;
}

// ---- TEMP-DIAG(FAULT-MAP) ----
// 2026-09-17 修正: 旧实现只 read() 一次 /proc/self/maps, 在 4KB seq_file 边界被截断
// (实测每次只拿到 ~3.5KB, 读到 0x14001a000 就停), 于是 0x6ffcxxxxxxxx /
// 0x7ffexxxxxxxx 这些高位区一律被误判成 not-in-maps。现在循环读到 EOF, 落盘 + 回显
// 整张 maps, 并补上 fault 指令机器码与通用寄存器, 让本地 objdump 能直接对上号。
static size_t OhosReadFullMaps(char* buf, size_t cap)
{
    if (!buf || cap < 2) return 0;
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return 0;
    size_t total = 0;
    for (;;)
    {
        if (total + 1 >= cap) break;
        ssize_t r = read(fd, buf + total, cap - 1 - total);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

struct OhosVma { uintptr_t lo; uintptr_t hi; char perms[8]; };

// 找出 maps 里覆盖 addr 的那一段 (地址空间排序, 命中即止)。
static bool OhosVmaBounds(const char* maps, uintptr_t addr, OhosVma* out)
{
    if (!maps) return false;
    const char* p = maps;
    while (p && *p)
    {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        unsigned long long s = 0, e = 0;
        char perm[8] = {0};
        if (len && sscanf(p, "%llx-%llx %7s", &s, &e, perm) == 3 &&
            addr >= (uintptr_t)s && addr < (uintptr_t)e)
        {
            if (out)
            {
                out->lo = (uintptr_t)s;
                out->hi = (uintptr_t)e;
                snprintf(out->perms, sizeof(out->perms), "%s", perm);
            }
            return true;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

// pc / lr / sp / si_addr 各自落在哪一段 VMA、什么权限。区分:
// 页面完全不存在 / 页面存在但权限不符 (SEGV_ACCERR) / guard / reserved。
static void OhosEmitFaultMap(const char* what, uintptr_t addr, const char* maps)
{
    if (!addr || !maps) return;
    char hit[384] = {0};
    char below[384] = {0};
    char above[384] = {0};
    const char* p = maps;
    while (p && *p)
    {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && len < sizeof(hit))
        {
            unsigned long long s = 0, e = 0;
            char perms[8] = {0};
            if (sscanf(p, "%llx-%llx %7s", &s, &e, perms) == 3)
            {
                if (addr >= (uintptr_t)s && addr < (uintptr_t)e)
                {
                    memcpy(hit, p, len);
                    hit[len] = '\0';
                    break;
                }
                if ((uintptr_t)e <= addr)
                {
                    memcpy(below, p, len);
                    below[len] = '\0';
                }
                else if (!above[0])
                {
                    memcpy(above, p, len);
                    above[len] = '\0';
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    char out[1400];
    int m;
    if (hit[0])
        m = snprintf(out, sizeof(out), "[fault-map] %s=%p HIT %s\n",
                     what, (void*)addr, hit);
    else
        m = snprintf(out, sizeof(out),
                     "[fault-map] %s=%p NOT-IN-MAPS\n"
                     "[fault-map]   below: %s\n"
                     "[fault-map]   above: %s\n",
                     what, (void*)addr, below[0] ? below : "(none)",
                     above[0] ? above : "(none)");
    if (m > 0) write(2, out, (size_t)((m < (int)sizeof(out)) ? m : (int)sizeof(out) - 1));
}

// 首次 fault 时把整张 maps 落盘 (设备可直接 pull) 并回显到 stderr。
static void OhosDumpSelfMaps(const char* maps, size_t n)
{
    if (!maps || !n) return;
    char path[192];
    snprintf(path, sizeof(path), "/data/storage/el2/base/temp/fault-maps-%d.txt", (int)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        snprintf(path, sizeof(path), "/data/local/tmp/fault-maps-%d.txt", (int)getpid());
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd >= 0)
    {
        size_t off = 0;
        while (off < n)
        {
            ssize_t w = write(fd, maps + off, n - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        close(fd);
        char out[224];
        int m = snprintf(out, sizeof(out), "[fault-maps-file] %s bytes=%zu\n", path, n);
        if (m > 0) write(2, out, (size_t)m);
    }
    write(2, "[fault-maps-begin]\n", 19);
    size_t echo = (n > (64u << 10)) ? (64u << 10) : n;
    write(2, maps, echo);
    write(2, "[fault-maps-end]\n", 17);
}

// 打印 fault 现场机器码 (pc-16 起 64 字节), 让本地 objdump 反汇编出访问语义。
static void OhosDumpFaultCode(uintptr_t pc)
{
    char out[512];
    int fd = open("/proc/self/mem", O_RDONLY);
    if (fd < 0)
    {
        int m = snprintf(out, sizeof(out), "[fault-code] open(/proc/self/mem) failed\n");
        if (m > 0) write(2, out, (size_t)m);
        return;
    }
    unsigned char code[64];
    uintptr_t base = (pc - 16) & ~(uintptr_t)3;
    ssize_t r = pread(fd, code, sizeof(code), (off_t)base);
    close(fd);
    if (r <= 0)
    {
        int m = snprintf(out, sizeof(out), "[fault-code] pread(pc=%p) failed\n", (void*)pc);
        if (m > 0) write(2, out, (size_t)m);
        return;
    }
    int off = snprintf(out, sizeof(out), "[fault-code] base=%p pc=%p n=%zd\n",
                       (void*)base, (void*)pc, r);
    for (ssize_t i = 0; i < r; i += 4)
    {
        unsigned w = 0;
        for (ssize_t k = 0; k < 4 && (i + k) < r; k++)
            w |= (unsigned)code[i + k] << (8 * k);
        if (off < (int)sizeof(out) - 16)
            off += snprintf(out + off, sizeof(out) - off, "%s%08x",
                            ((uintptr_t)(base + i) == pc) ? " *" : " ", w);
    }
    if (off < (int)sizeof(out) - 2) { out[off++] = '\n'; out[off] = '\0'; }
    write(2, out, (size_t)off);
}

// 打印 AArch64 通用寄存器: 判断 fault 指令用的是哪个 base/offset 只能靠它。
static void OhosDumpRegs(ucontext_t* uc)
{
#if defined(__aarch64__)
    if (!uc) return;
    char out[768];
    int off = 0;
    for (int i = 0; i < 31 && off < (int)sizeof(out) - 96; i += 4)
    {
        off += snprintf(out + off, sizeof(out) - off, "[fault-regs]");
        for (int k = 0; k < 4 && (i + k) < 31; k++)
            off += snprintf(out + off, sizeof(out) - off, " x%d=%016llx", i + k,
                            (unsigned long long)uc->uc_mcontext.regs[i + k]);
        off += snprintf(out + off, sizeof(out) - off, "\n");
    }
    if (off < (int)sizeof(out) - 96)
        off += snprintf(out + off, sizeof(out) - off,
                        "[fault-regs] sp=%016llx pc=%016llx\n",
                        (unsigned long long)uc->uc_mcontext.sp,
                        (unsigned long long)uc->uc_mcontext.pc);
    write(2, out, (size_t)off);
#else
    (void)uc;
#endif
}

// 运行时开关: 注册时 entryParams 的 env 还没应用, 所以 handler 里再看这个标志。
static volatile int g_early_fault_enabled = 1;

int OhosEarlyFault(int sig, siginfo_t* info, void* uctx) {
    static int logged = 0;
    static volatile int in_handler = 0;
    ucontext_t* uc = static_cast<ucontext_t*>(uctx);
    uintptr_t pc = 0, lr = 0, sp = 0;
#if defined(__aarch64__)
    if (uc) {
        pc = uc->uc_mcontext.pc;
        lr = uc->uc_mcontext.regs[30];
        sp = uc->uc_mcontext.sp;
    }
#endif
    if (!g_early_fault_enabled) return 0;

    /* 真嵌套: handler 自己又 fault (Wine 会 abort_thread, 必须能分辨) */
    if (in_handler)
    {
        if (logged <= 8)
        {
            char out[224];
            int m = snprintf(out, sizeof(out),
                             "[early-fault] nested#%d sig=%d code=%d addr=%p pc=%p lr=%p sp=%p\n",
                             logged, sig, info ? info->si_code : 0,
                             info ? info->si_addr : nullptr, (void*)pc, (void*)lr, (void*)sp);
            if (m > 0) write(2, out, (size_t)m);
        }
        return 0;
    }
    /* 去重而不是"前 4 颗都打" (2026-09-18 观测轮实测):
     * arm64ec 路径下**每个 x64 进程启动阶段都会先吃 4 颗形态完全相同的
     * sig=7(SIGBUS) 探针 fault** (FEX/SMC/SEH, 见 [SMC] result=wine_seh),
     * 旧的"前 4 颗"名额被它们吃光 —— 后面真正的崩溃 (CEF renderer 的
     * SIGSEGV, 实测每 7~8 秒死一次) 反而一条现场都没有, 直接漏掉根因。
     * SIGBUS 探针不能占用后来 SIGSEGV 的诊断名额。 */
    static int seenSig[16];
    static uintptr_t seenPc[16];
    static int seenCount[2];
    const int group = sig == SIGSEGV ? 1 : 0;
    const int first = group * 8;
    for (int i = first; i < first + seenCount[group]; i++)
        if (seenSig[i] == sig && seenPc[i] == pc) return 0;
    if (seenCount[group] >= 8) return 0;
    const int index = first + seenCount[group]++;
    seenSig[index] = sig;
    seenPc[index] = pc;
    logged = index + 1;
    in_handler = 1;

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "[early-fault] #%d pid=%d tid=%ld sig=%d code=%d addr=%p pc=%p lr=%p sp=%p\n",
                     logged, getpid(), (long)syscall(SYS_gettid), sig, info ? info->si_code : 0,
                     info ? info->si_addr : nullptr, (void*)pc, (void*)lr, (void*)sp);
    if (n > 0) write(2, buf, (size_t)n);

    static char maps[512 * 1024];
    size_t mapsBytes = OhosReadFullMaps(maps, sizeof(maps));
    n = snprintf(buf, sizeof(buf), "[fault-maps] bytes=%zu\n", mapsBytes);
    if (n > 0) write(2, buf, (size_t)n);

    OhosDumpRegs(uc);
    OhosDumpFaultCode(pc);
    OhosEmitFaultMap("pc", pc, maps);
    OhosEmitFaultMap("lr", lr, maps);
    if (sp) OhosEmitFaultMap("sp", sp, maps);
    if (info && info->si_addr) OhosEmitFaultMap("addr", (uintptr_t)info->si_addr, maps);

    OhosEarlyCtx ctx;
    ctx.count = 0;
    dl_iterate_phdr(OhosEarlyPhdrCb, &ctx);
    auto emit = [&ctx](const char* what, uintptr_t addr) {
        char line[220];
        for (int i = 0; i < ctx.count; i++) {
            if (addr >= ctx.mods[i].start && addr < ctx.mods[i].end) {
                int m = snprintf(line, sizeof(line), "[early-fault] %s=%p %s+0x%lx\n", what,
                                 (void*)addr, ctx.mods[i].name,
                                 (unsigned long)(addr - ctx.mods[i].start));
                if (m > 0) write(2, line, (size_t)m);
                return;
            }
        }
        int m = snprintf(line, sizeof(line), "[early-fault] %s=%p <unmapped>\n", what, (void*)addr);
        if (m > 0) write(2, line, (size_t)m);
    };
    emit("pc-elf", pc);
    emit("lr-elf", lr);

    OhosDumpSelfMaps(maps, mapsBytes);

    /* WineHua: 把 Wine 侧 GL proc 解析 ring 一并 dump (win32u.so 以 RTLD_GLOBAL 加载)。
     * 用来回答"崩溃前最后解析了哪些 GL/EGL 入口、哪一个返回了 NULL"。 */
    {
        typedef void (*gl_proc_dump_fn)(const char *);
        gl_proc_dump_fn dump = (gl_proc_dump_fn)dlsym(RTLD_DEFAULT, "winehua_gl_proc_trace_dump");
        if (dump) dump("early-fault");
    }

    /* 栈扫描只经 /proc/self/mem, 且要求 VMA 可读 — 见 2026-09-18 的 nested 事故。 */
    if (sp)
    {
        OhosVma vma = {0, 0, {0}};
        if (OhosVmaBounds(maps, sp, &vma) && vma.perms[0] == 'r')
        {
            uintptr_t start = sp & ~(uintptr_t)7;
            uintptr_t limit = (vma.hi > start) ? (vma.hi - start) : 0;
            size_t bytes = (limit < 96 * sizeof(uintptr_t)) ? (size_t)limit : 96 * sizeof(uintptr_t);
            bytes &= ~(size_t)7;
            if (bytes)
            {
                int fd = open("/proc/self/mem", O_RDONLY);
                if (fd >= 0)
                {
                    static uintptr_t words[96];
                    ssize_t got = pread(fd, words, bytes, (off_t)start);
                    close(fd);
                    if (got > 0)
                    {
                        int count = (int)(got / (ssize_t)sizeof(uintptr_t));
                        for (int i = 0; i < count; i++)
                        {
                            uintptr_t v = words[i];
                            for (int j = 0; j < ctx.count; j++)
                            {
                                if (v >= ctx.mods[j].start && v < ctx.mods[j].end)
                                {
                                    emit("stack", v);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    const char* freeze = getenv("WINEHUA_FAULT_FREEZE");
    if (freeze && freeze[0] == '1')
    {
        int hold = atoi(freeze + 1);
        if (hold <= 0) hold = 30;
        if (hold > 300) hold = 300;
        char out[160];
        int m = snprintf(out, sizeof(out),
                         "[fault-freeze] pid=%d tid=%ld hold=%ds\n",
                         getpid(), (long)syscall(SYS_gettid), hold);
        if (m > 0) write(2, out, (size_t)m);
        for (int i = 0; i < hold; i++) sleep(1);
    }
    write(2, "[early-fault] logger-exit\n", 26);
    in_handler = 0;
    return 0;
}

void OhosInstallEarlyFaultLogger() {
    /* 诊断器本身也可能是 nested exception 的来源, 所以留一个 A/B 开关:
     * WINEHUA_EARLY_FAULT=0 完全不注册 (回到纯 Wine 处理路径)。 */
    const char* enable = getenv("WINEHUA_EARLY_FAULT");
    if (enable && enable[0] == '0')
    {
        OH_LOG_INFO(LOG_APP, "[WineChild] early fault logger disabled (WINEHUA_EARLY_FAULT=0)");
        return;
    }
    struct ohos_sca { int (*sca_sigaction)(int, siginfo_t*, void*); sigset_t sca_mask; uint64_t sca_flags; };
    auto add_special = (void (*)(int, struct ohos_sca*))dlsym(RTLD_DEFAULT, "AddSpecialSignalHandlerFn");
    if (!add_special) add_special = (void (*)(int, struct ohos_sca*))dlsym(RTLD_DEFAULT, "add_special_signal_handler");
    if (!add_special) {
        OH_LOG_WARN(LOG_APP, "[WineChild] early fault logger: no sigchain API");
        return;
    }
    struct ohos_sca sca;
    memset(&sca, 0, sizeof(sca));
    sca.sca_sigaction = OhosEarlyFault;
    sigfillset(&sca.sca_mask);
    sigdelset(&sca.sca_mask, SIGSEGV);
    sigdelset(&sca.sca_mask, SIGBUS);
    sigdelset(&sca.sca_mask, SIGILL);
    add_special(SIGSEGV, &sca);
    add_special(SIGBUS, &sca);
    add_special(SIGILL, &sca);
    OH_LOG_INFO(LOG_APP, "[WineChild] early fault logger installed");
}
}  // namespace
// ---- TEMP-DIAG(EARLY-FAULT) end ----

// ---- 用户态栈采样: 卡住线程到底停在哪个函数 (2026-09-19) ----
// wchan 只说明"在 futex 上等", 看不到调用者。这里对每个线程 tgkill(SIGPROF),
// 在 handler 里记录 pc/lr/fp, 再用 dl_iterate_phdr 的模块表解析成 模块+偏移。
// 只在 WINEHUA_STALL_DUMP 打开、且判定为卡住时触发。
#define WINEHUA_STALL_MAX_SAMPLES 256

struct WineHuaStallSample {
    int tid;
    uintptr_t pc, lr, fp;
};

static WineHuaStallSample g_stallSamples[WINEHUA_STALL_MAX_SAMPLES];
static int g_stallSampleCount;
static volatile sig_atomic_t g_stallSampling;
// Wine 侧还装了一个 SIGPROF 采样器 (WINEHUA_PROF_SAMPLE=1 时), 它负责读
// ARM64EC 的 guest(x64) 上下文。这里保存它原来的 handler 并链式调用,
// 否则本文件安装 handler 会把对方顶掉 (实测: 只能拿到 host 侧 pc).
static struct sigaction g_stallPrevSa;
static int g_stallChainPrev;

static void WineHuaStallSampleHandler(int sig, siginfo_t* info, void* uctx)
{
    (void)sig; (void)info;
    if (!g_stallSampling) return;
    ucontext_t* uc = static_cast<ucontext_t*>(uctx);
    if (uc)
    {
        int slot = g_stallSampleCount;
        if (slot >= 0 && slot < WINEHUA_STALL_MAX_SAMPLES)
        {
            g_stallSampleCount = slot + 1;
            g_stallSamples[slot].tid = (int)syscall(SYS_gettid);
#if defined(__aarch64__)
            g_stallSamples[slot].pc = (uintptr_t)uc->uc_mcontext.pc;
            g_stallSamples[slot].lr = (uintptr_t)uc->uc_mcontext.regs[30];
            g_stallSamples[slot].fp = (uintptr_t)uc->uc_mcontext.regs[29];
#else
            g_stallSamples[slot].pc = g_stallSamples[slot].lr = g_stallSamples[slot].fp = 0;
#endif
        }
    }
    // 链式调用 wine 侧采样器 (它会在 ARM64EC 进程里打 [prof-guest] ...)
    if (g_stallChainPrev)
    {
        if (g_stallPrevSa.sa_flags & SA_SIGINFO)
        {
            if (g_stallPrevSa.sa_sigaction) g_stallPrevSa.sa_sigaction(sig, info, uctx);
        }
        else if (g_stallPrevSa.sa_handler && g_stallPrevSa.sa_handler != SIG_DFL &&
                 g_stallPrevSa.sa_handler != SIG_IGN)
        {
            g_stallPrevSa.sa_handler(sig);
        }
    }
}

static void WineHuaStallEmitPc(const char* what, uintptr_t v, const OhosEarlyCtx& ctx)
{
    char line[256];
    for (int i = 0; i < ctx.count; i++)
    {
        if (v >= ctx.mods[i].start && v < ctx.mods[i].end)
        {
            int n = snprintf(line, sizeof(line), "[stall-pc] %s=%p %s+0x%lx\n", what,
                             (void*)v, ctx.mods[i].name, (unsigned long)(v - ctx.mods[i].start));
            if (n > 0) write(2, line, (size_t)n);
            return;
        }
    }
    int n = snprintf(line, sizeof(line), "[stall-pc] %s=%p <unmapped>\n", what, (void*)v);
    if (n > 0) write(2, line, (size_t)n);
}

static void WineHuaStallSampleAllThreads(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = WineHuaStallSampleHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, &g_stallPrevSa) != 0) return;
    g_stallChainPrev = 1;

    g_stallSampleCount = 0;
    g_stallSampling = 1;
    DIR* d = opendir("/proc/self/task");
    if (d)
    {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr)
        {
            if (ent->d_name[0] == '.') continue;
            int tid = atoi(ent->d_name);
            if (tid <= 0) continue;
            syscall(SYS_tgkill, getpid(), tid, SIGPROF);
            /* 目标线程可能正阻塞在 futex 上, 需要被唤醒并调度到才会跑 handler;
             * 窗口太短会一条都收不到 (实测 1.5ms 时 0 条)。 */
            usleep(20000);
        }
        closedir(d);
    }
    usleep(200000);   /* 收尾: 等最后几个线程把 handler 跑完 */
    g_stallSampling = 0;

    OhosEarlyCtx ctx;
    ctx.count = 0;
    dl_iterate_phdr(OhosEarlyPhdrCb, &ctx);

    char comm[64] = {0}, path[96], line[160];
    for (int i = 0; i < g_stallSampleCount; i++)
    {
        comm[0] = 0;
        snprintf(path, sizeof(path), "/proc/self/task/%d/comm", g_stallSamples[i].tid);
        FILE* f = fopen(path, "r");
        if (f) { if (!fgets(comm, sizeof(comm), f)) comm[0] = 0; fclose(f); }
        char* nl = strchr(comm, '\n'); if (nl) *nl = 0;
        int n = snprintf(line, sizeof(line), "[stall-pc] tid=%d comm=%s\n",
                         g_stallSamples[i].tid, comm);
        if (n > 0) write(2, line, (size_t)n);
        WineHuaStallEmitPc("pc", g_stallSamples[i].pc, ctx);
        WineHuaStallEmitPc("lr", g_stallSamples[i].lr, ctx);
        WineHuaStallEmitPc("fp", g_stallSamples[i].fp, ctx);
    }
}

extern "C" void Main(NativeChildProcess_Args args)
{
    OH_LOG_INFO(LOG_APP, "[WineChild] Main() ENTER pid=%{public}d entryParams=%{public}s",
                getpid(), args.entryParams ? args.entryParams : "(null)");
    LogWineScheme("libwine_child.so Main");
    OhosInstallEarlyFaultLogger();

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

    // 线程名 = argv[0] basename (prctl 最长 15 字符)。崩溃记录 (DfxSignalHandler
    // threadName) 直接显示进程身份, 即使 hilog 日志丢失也能从 tombstone 认出是谁。
    if (argc > 0 && argv[0] && argv[0][0])
        prctl(PR_SET_NAME, basename_of_path(argv[0]));

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

    // wineserver 截获 (重构第 5 步): 所有 wineserver 启动经 broker → Main;
    // wine loader 无法把 "wineserver" 解析为 PE (纯 Unix ELF), 必须在此转入
    // wineserver 本体 (与旧 loader 自启 ohos_broker_spawn_wineserver 同路)。
    if (argc > 0 && !strcmp(argv[0], "wineserver")) {
        // broker 会为每个请求挂 audio bootstrap fd; wineserver 用不到, 关掉防泄漏
        for (auto* node = args.fdList.head; node; node = node->next) close(node->fd);
        RunWineserver(binDir, argc, argv, envOverrides, entryParams);
        free(buf);
        return;
    }

    OH_LOG_INFO(LOG_APP, "[WineChild] homeDir=%{public}s binDir=%{public}s argc=%{public}d argv[0]=%{public}s",
                homeDir ? homeDir : "(null)", binDir, argc, argc > 0 ? argv[0] : "(none)");

    // 2. Step A: 设置 Wine 环境变量 baseline (硬编码默认值, 确保非 broker 路径可用)
    const char *winedebug = select_winedebug_profile(argc, argv);
    OH_LOG_INFO(LOG_APP, "[WineChild] WINEDEBUG=%{public}s", winedebug);
    setup_wine_env(binDir, homeDir, winedebug);

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
    apply_game_address_space_compatibility(argc, argv);
    // entryParams 覆盖之后再选一次 WINEDEBUG 档位: 上一次调用发生在
    // apply_entry_param_env_overrides() 之前, 取不到 entryParams 里的覆盖。
    // 档位来源是 WINEHUA_WINEDEBUG 与内置 profile —— WINEDEBUG 键本身在
    // apply_entry_param_env_overrides() 里被拦下 (决策点收口到设备端的
    // select_winedebug_profile, 见该函数内注释)。
    {
        const char* profile = select_winedebug_profile(argc, argv);
        setenv("WINEDEBUG", profile, 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] final WINEDEBUG=%{public}s (after entry env)", profile);
    }
    // early-fault 诊断器同理: 注册时 entryParams 的 env 还没生效, 运行时再确认一次。
    {
        const char* ef = getenv("WINEHUA_EARLY_FAULT");
        const char* quiet = getenv("WINEHUA_DIAG_QUIET");
        g_early_fault_enabled = (ef && ef[0] == '0') ||
                                (quiet && quiet[0] == '1' && !(ef && ef[0] == '1')) ? 0 : 1;
        OH_LOG_INFO(LOG_APP, "[WineChild] early fault logger=%{public}s",
                    g_early_fault_enabled ? "on" : "off");
    }
    // CEF 渲染后端不再由 WineHua 单方面决定 (P0-GL 计划 §19):
    //   默认 (clean)  -> 不注入任何 CEF GPU/软件开关, 让 CEF 自己选 backend,
    //                    kernelbase 侧也只在显式设置时才追加 (Run A)
    //   FORCE_GPU=1   -> 只注入 Steam 自己的 -cef-force-gpu (Run B)
    //   FORCE_SOFTWARE=1|both|compositing|gpu
    //                 -> 注入 --disable-gpu / --disable-gpu-compositing (Run C, 兼容性回退)
    // 历史上软件开关默认打开, 但实测它会把 CEF 推到"半 GPU 状态"(窗口整体黑、renderer
    // 反复 SIGSEGV), 而 clean 模式至少能出 VGUI 外框 —— 所以默认回到 clean,
    // 软件模式保留为显式回退项。
    const char* softDefault = getenv("WINEHUA_CEF_FORCE_SOFTWARE");
    OH_LOG_INFO(LOG_APP, "[WineChild] CEF backend policy: force_gpu=%{public}s force_software=%{public}s",
                (getenv("WINEHUA_CEF_FORCE_GPU") && getenv("WINEHUA_CEF_FORCE_GPU")[0] == '1') ? "1" : "0",
                (softDefault && softDefault[0]) ? softDefault : "(unset/clean)");
    // entryParams 覆盖之后再决定 32 位 CPU 后端 (HODLL)，保证 Want 里的
    // WINEHUA_WOW64_ENGINE 生效。
    select_wow64_backend(argc, argv);
    // Steam 客户端缺省参数兜底 (CEF 沙箱/gpu), 理由见函数注释。
    apply_steam_client_default_args(argc, argv);
    // 诊断: webhelper 系崩溃处理器开关 (默认关闭, 见函数注释)。
    apply_steam_webhelper_diag_args(argc, argv);
    // WINEPREFIX is a per-session override. Derive paths only after the final
    // value is known, and avoid a "prefix/../" path whose intermediate prefix
    // may not exist after a clean install.
    refresh_wine_session_paths();
    // 前缀字体/代码页自愈 (必须在 Wine 起来之前): 新装/重置 prefix 后 UI 字体链是断的,
    // 会让 win64/32 位 Steam 的 VGUI2 断言 surface_gdiwin32.cpp:1336 winFont 并卡在启动画面。
    ensure_prefix_fonts_and_codepage();
    // Migrate the old file-hiding workaround before Steam verifies its files.
    if (is_steam_bootstrap_exe(argc, argv)) restore_shadowed_vulkan_loaders();

    // 父进程 __env 可能用 DXVK PE 目录覆盖 WINEDLLPATH 并丢掉 HAP native-lib
    // 目录 (wineohos.so 所在) — 按方案重Assert运行期路径 (arm64 三方案修复)。
    reassert_arch_wine_runtime_env(binDir);
    /* Parent serializes WINEDEBUG=-all,+opengl,... which clobbers the
     * audio diagnostic profile selected in setup_wine_env(). Restore it
     * so mmdevapi/wineohos traces actually appear for audio tests. */
    if (is_audio_test_exe(argc, argv) || is_sdl_audio_test_exe(argc, argv) ||
        (is_steam_webhelper_exe(argc, argv) && steam_webhelper_diag_enabled()))
    {
        const char *profile = select_winedebug_profile(argc, argv);
        setenv("WINEDEBUG", profile, 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] restored diagnostic WINEDEBUG=%{public}s", profile);
    }
    log_d3d_environment_summary();
    OH_LOG_INFO(LOG_APP,
                "[WineChild] final WINEDLLDIR=%{public}s WINEDEBUG=%{public}s",
                getenv("WINEDLLDIR") ? getenv("WINEDLLDIR") : "",
                getenv("WINEDEBUG") ? getenv("WINEDEBUG") : "");

    // 覆盖 per-process fd 变量 (__env__ 中的是父进程 fd 号, 本进程无效)
    if (wsSockFd >= 0) {
        char wsEnv[64];
        snprintf(wsEnv, sizeof(wsEnv), "%d", wsSockFd);
        setenv("WINESERVERSOCKET", wsEnv, 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] WINESERVERSOCKET=%{public}d (own fd)", wsSockFd);
    }
    if (audioFd >= 0) {
        /* 保护 bootstrap fd: dup 到高位, 避免 wine ntdll 启动时复用低 fd */
        int saved = fcntl(audioFd, F_DUPFD, 512);
        if (saved >= 0)
        {
            OH_LOG_INFO(LOG_APP, "[WineChild] audio bootstrap fd dup %{public}d -> %{public}d (guard high)",
                        audioFd, saved);
            audioFd = saved;
        }
        else
        {
            OH_LOG_WARN(LOG_APP, "[WineChild] audio bootstrap fd dup failed errno=%{public}d", errno);
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", audioFd);
        setenv("WINE_OHOS_AUDIO_ENABLE", "1", 1);
        setenv("WINE_OHOS_AUDIO_BOOTSTRAP_FD", buf, 1);
        setenv("WINE_OHOS_AUDIO_PROTOCOL_VERSION", "1", 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] AUDIO fd=%{public}d (own fd)", audioFd);
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

    // 空闲卡死看门狗 (WINEHUA_STALL_DUMP=<秒> 才启动): 必须在 stderr 已重定向到
    // wine_stderr 之后启动, 这样 dump 才落在同一个文件里。
    {
        pthread_t watchdog;
        if (pthread_create(&watchdog, nullptr, WineHuaStallWatchdog, nullptr) == 0)
            pthread_detach(watchdog);
    }

#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
    // 方案② box64+wine: dlopen box64.so → box64_hmos_main, box64 转译 x86_64 wine ELF。
    // guest 程序 (guest_vulkan/bin/*.so) 也是 x86_64 ELF, 同样经 box64 加载 (argv[0] 指向 .so)。
    OH_LOG_INFO(LOG_APP, "[WineChild] dlopen box64.so (box64+wine 方案②)...");
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

    std::string winePath = std::string(binDir) + "/wine";
    int box64_argc = argc + 2;
    const char** box64_argv = new const char*[box64_argc + 1];
    box64_argv[0] = "box64";
    box64_argv[1] = winePath.c_str();
    for (int i = 0; i < argc; i++) box64_argv[i + 2] = argv[i];
    box64_argv[box64_argc] = nullptr;

    OH_LOG_INFO(LOG_APP, "[WineChild] calling box64_hmos_main argc=%{public}d wine=%{public}s",
                box64_argc, winePath.c_str());

    int box64_rc = box64_main(box64_argc, box64_argv, environ);
    OH_LOG_INFO(LOG_APP, "[WineChild] box64_hmos_main returned rc=%{public}d", box64_rc);

    delete[] box64_argv;
    // 不 dlclose(box64_lib): box64 内部注册 atexit handler / 包装函数指针,
    // 卸载后回调引用已卸载代码 → SIGSEGV。进程即将退出, OS 回收。
    free(buf);
    return;
#else
    // Wine 与设备同架构 (方案① x86_64 / 方案③ arm64 原生): dlopen ntdll.so → __wine_main
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

// wineserver 本体 — 统一入口 (重构第 5 步)。
// 所有 wineserver 启动都经 broker → Main 截获 argv[0]=="wineserver" 到此
// (wine loader 无法把 "wineserver" 解析为 PE — 它是纯 Unix ELF; loader 自启
// ohos_broker_spawn_wineserver 同样走 broker→Main, 由本函数兜底)。
// env 基线刻意精简 (非完整 setup_wine_env): wineserver 几乎不加载库,
// 省 entryParams 长度; WINEPREFIX 由 __env 会话权威最后覆盖。
// argv/argvOverrides 来自调用方已解析的 token (argv[0]="wineserver" ...)。
static void RunWineserver(char* binDir, int argc2, char** argv2,
                          const std::vector<std::string>& envOverrides,
                          const char* entryParamsForLog)
{
    LogWineScheme("libwine_child.so RunWineserver");
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step1: setting env...");
    setenv("WINEPREFIX", WINE_PREFIX, 1);
    setenv("WINEDEBUG", "-all", 1);
#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
    // 方案② box64 基线必须先于 __env apply: 会话档位 (BOX64_DYNAREC_*) 经 __env
    // 下发, apply 最后执行才能保证 "后写胜出"。方案③ wineserver 为 arm64 原生,
    // 不涉 box64。
    setenv("BOX64_LD_LIBRARY_PATH", (std::string(binDir) + "/" WINE_UNIX_SUBDIR).c_str(), 1);
    winehua::SetBox64PerfEnv();
#endif
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
                entryParamsForLog ? entryParamsForLog : "(null)");
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
    // 线程名 = wineserver, 崩溃记录 threadName 直接可见
    prctl(PR_SET_NAME, "wineserver");
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step4: argv argc=%{public}d argv[0]=%{public}s", argc2, argv2[0]);

#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
    // 方案② box64+wine: dlopen box64.so → box64_hmos_main, box64 转译 x86_64 PIE wineserver
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step5: dlopen box64.so (box64+wine 方案②)...");
    void* box64_lib = dlopen("box64.so", RTLD_NOW);
    if (!box64_lib) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlopen(box64.so) failed: %{public}s", dlerror());
        return;
    }
    auto* box64_main = (int (*)(int, const char**, char**))dlsym(box64_lib, "box64_hmos_main");
    if (!box64_main) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlsym(box64_hmos_main) failed: %{public}s", dlerror());
        dlclose(box64_lib);
        return;
    }

    // Box64 env (BOX64_LD_LIBRARY_PATH / 性能基线) 已在 step1 先于 __env
    // apply 设置, 此处直接拼 argv。
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
    // 不 dlclose(box64_lib): box64 内部注册 atexit handler, 卸载后引用已卸载代码 → SIGSEGV
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step9: wineserver process exiting");
    return;
#else
    // Wine 与设备同架构 (arm64 原生 aarch64 / x86_64): dlopen libwineserver.so (原生)
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step5: dlopen libwineserver.so...");
    void* h = dlopen("libwineserver.so", RTLD_NOW);
    if (!h) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlopen(libwineserver.so) failed: %{public}s", dlerror());
        return;
    }
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step6: dlsym main...");
    auto* ws_main = (int (*)(int, char**))dlsym(h, "main");
    if (!ws_main) {
        OH_LOG_ERROR(LOG_APP, "[WineChild] dlsym(main) failed: %{public}s", dlerror());
        dlclose(h);
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
    OH_LOG_INFO(LOG_APP, "[WineChild] ws step9: wineserver process exiting");
#endif
}

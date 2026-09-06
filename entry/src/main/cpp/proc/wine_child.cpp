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
    // 通过 WINEHUA_WOW64_ENGINE=fex 切换 (与 WINEHUA_WINEDEBUG 同机制)。
    const char *wow64_engine = getenv("WINEHUA_WOW64_ENGINE");
    if (wow64_engine && strcmp(wow64_engine, "fex") == 0)
        setenv("HODLL", "libwow64fex.dll", 1);
    else
        setenv("HODLL", "wowbox64.dll", 1);
#endif
    setenv("WINEDEBUG", winedebug && winedebug[0] ? winedebug : default_winedebug_profile(), 1);
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

extern "C" void Main(NativeChildProcess_Args args)
{
    OH_LOG_INFO(LOG_APP, "[WineChild] Main() ENTER pid=%{public}d entryParams=%{public}s",
                getpid(), args.entryParams ? args.entryParams : "(null)");
    LogWineScheme("libwine_child.so Main");

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
    // WINEPREFIX is a per-session override. Derive paths only after the final
    // value is known, and avoid a "prefix/../" path whose intermediate prefix
    // may not exist after a clean install.
    refresh_wine_session_paths();

    // 父进程 __env 可能用 DXVK PE 目录覆盖 WINEDLLPATH 并丢掉 HAP native-lib
    // 目录 (wineohos.so 所在) — 按方案重Assert运行期路径 (arm64 三方案修复)。
    reassert_arch_wine_runtime_env(binDir);
    /* Parent serializes WINEDEBUG=-all,+opengl,... which clobbers the
     * audio diagnostic profile selected in setup_wine_env(). Restore it
     * so mmdevapi/wineohos traces actually appear for audio tests. */
    if (is_audio_test_exe(argc, argv) || is_sdl_audio_test_exe(argc, argv))
    {
        const char *profile = select_winedebug_profile(argc, argv);
        setenv("WINEDEBUG", profile, 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] restored audio WINEDEBUG=%{public}s", profile);
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

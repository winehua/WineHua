#include "wine_exe.h"

#include "broker.h"
#include "graphics_broker.h"
#include "wayland_server.h"
#include "wine_constants.h"
#include "wine_env.h"
#include "wine_process.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

extern napi_threadsafe_function gStateTsfn;

namespace {

struct GuestProgramOptions {
    std::string executablePath;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string workingDirectory;
    bool automationMode = true;
};

struct HostProgramOptions {
    std::string executablePath;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string workingDirectory;
    bool automationMode = true;
};

using HostReplayMain = int (*)(int, char**);
static std::atomic<bool> gHostReplayRunning{false};

static bool HasUnsafeProtocolChar(const std::string& value)
{
    return value.find('|') != std::string::npos || value.find('\n') != std::string::npos ||
           value.find('\r') != std::string::npos;
}

static std::string ReadString(napi_env env, napi_value value)
{
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
    std::vector<char> buffer(length + 1);
    if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &length) != napi_ok) return {};
    return std::string(buffer.data(), length);
}

static bool GetNamed(napi_env env, napi_value object, const char* name, napi_value* out)
{
    bool has = false;
    if (napi_has_named_property(env, object, name, &has) != napi_ok || !has) return false;
    return napi_get_named_property(env, object, name, out) == napi_ok;
}

static std::string GetString(napi_env env, napi_value object, const char* name,
                             const std::string& fallback = {})
{
    napi_value value;
    napi_valuetype type;
    if (!GetNamed(env, object, name, &value) || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_string)
        return fallback;
    std::string result = ReadString(env, value);
    return result.empty() ? fallback : result;
}

static bool GetBool(napi_env env, napi_value object, const char* name, bool fallback)
{
    napi_value value;
    napi_valuetype type;
    bool result = fallback;
    if (GetNamed(env, object, name, &value) && napi_typeof(env, value, &type) == napi_ok &&
        type == napi_boolean)
        napi_get_value_bool(env, value, &result);
    return result;
}

static void ReadStringArray(napi_env env, napi_value object, const char* name,
                            std::vector<std::string>* out)
{
    napi_value array;
    bool isArray = false;
    uint32_t length = 0;
    if (!GetNamed(env, object, name, &array) || napi_is_array(env, array, &isArray) != napi_ok || !isArray ||
        napi_get_array_length(env, array, &length) != napi_ok)
        return;
    for (uint32_t i = 0; i < length; ++i)
    {
        napi_value item;
        napi_valuetype type;
        if (napi_get_element(env, array, i, &item) == napi_ok &&
            napi_typeof(env, item, &type) == napi_ok && type == napi_string)
            out->push_back(ReadString(env, item));
    }
}

static bool IsValidEnvKey(const std::string& key)
{
    if (key.empty() || !(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_')) return false;
    return std::all_of(key.begin() + 1, key.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    });
}

static void ReadEnvironment(napi_env env, napi_value object, std::vector<std::string>* out)
{
    napi_value record;
    napi_valuetype type;
    if (!GetNamed(env, object, "environment", &record) ||
        napi_typeof(env, record, &type) != napi_ok || type != napi_object)
        return;

    napi_value keys;
    uint32_t length = 0;
    if (napi_get_property_names(env, record, &keys) != napi_ok ||
        napi_get_array_length(env, keys, &length) != napi_ok)
        return;
    for (uint32_t i = 0; i < length; ++i)
    {
        napi_value keyValue, value;
        napi_valuetype valueType;
        if (napi_get_element(env, keys, i, &keyValue) != napi_ok) continue;
        std::string key = ReadString(env, keyValue);
        if (!IsValidEnvKey(key) || napi_get_property(env, record, keyValue, &value) != napi_ok ||
            napi_typeof(env, value, &valueType) != napi_ok || valueType != napi_string)
            continue;
        std::string line = key + "=" + ReadString(env, value);
        if (!HasUnsafeProtocolChar(line)) out->push_back(std::move(line));
    }
}

static std::string EnvKey(const std::string& line)
{
    size_t separator = line.find('=');
    return separator == std::string::npos ? line : line.substr(0, separator);
}

// UpsertEnvLine 在 wine_env.h 中声明，统一读写 env vector

static std::string PrefixForMode(const std::string& mode)
{
    return mode == "clean" ? WINE_SMOKE_PREFIX : WINE_PREFIX;
}

static std::string NativePathToWindows(const std::string& path, const std::string& prefix)
{
    const std::string driveRoot = prefix + "/drive_c/";
    if (path.rfind(driveRoot, 0) != 0) return path;
    std::string result = "C:\\" + path.substr(driveRoot.size());
    std::replace(result.begin(), result.end(), '/', '\\');
    return result;
}

static napi_value MakeProcessObject(napi_env env, const WineProcessEntry* entry, bool found)
{
    napi_value object;
    napi_create_object(env, &object);

    auto setBool = [&](const char* name, bool value) {
        napi_value item; napi_get_boolean(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setInt = [&](const char* name, int32_t value) {
        napi_value item; napi_create_int32(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setDouble = [&](const char* name, double value) {
        napi_value item; napi_create_double(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setString = [&](const char* name, const std::string& value) {
        napi_value item; napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &item);
        napi_set_named_property(env, object, name, item);
    };

    setBool("found", found);
    if (!found || !entry)
    {
        setInt("pid", -1);
        setString("status", "unknown");
        setString("exitCodeSource", "unknown");
        napi_value nullValue; napi_get_null(env, &nullValue);
        napi_set_named_property(env, object, "exitCode", nullValue);
        return object;
    }

    setInt("pid", entry->pid);
    setString("status", entry->running ? "running" : "exited");
    setDouble("startTimestamp", static_cast<double>(entry->startTimestampMs));
    setDouble("endTimestamp", static_cast<double>(entry->endTimestampMs));
    setString("exitCodeSource", entry->exitCodeSource);
    if (entry->exitCode >= 0) setInt("exitCode", entry->exitCode);
    else
    {
        napi_value nullValue; napi_get_null(env, &nullValue);
        napi_set_named_property(env, object, "exitCode", nullValue);
    }
    return object;
}

static int SpawnWineProgramImpl(const ProgramOptions& options)
{
    if (options.windowsExePath.empty() || HasUnsafeProtocolChar(options.windowsExePath)) return -1;
    for (const std::string& arg : options.argv) if (HasUnsafeProtocolChar(arg)) return -1;

    const std::string binDir = WINE_RUNTIME_BIN;
    const std::string prefixDir = PrefixForMode(options.prefixMode);
    const std::string homeDir = options.automationMode ? WINE_AUTOMATION_HOME
        : (gBrokerHomeDir.empty() ? "/storage/Users/currentUser/Download" : gBrokerHomeDir);
    const std::string sockDir = prefixDir;
    const std::string sockName = "wine-wayland";
    const std::string libPath = binDir + ":" + binDir + "/" WINE_UNIX_SUBDIR;
    const std::string exePath = NativePathToWindows(options.windowsExePath, prefixDir);

    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    winehua::GraphicsBroker::GetInstance().SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    if (!winehua::GraphicsBroker::GetInstance().EnsureStarted(prefixDir)) return -1;
    winehua::GraphicsBroker::GetInstance().SetVulkanPresentMode(
        options.presentBackend == "venus_broker_present" ||
        options.presentBackend == "venus_direct_present");

    std::vector<std::string> envStrs = BuildWineEnv(
        sockDir, sockName, libPath, binDir, -1, homeDir, prefixDir);
    /* Product defaults first, then per-run settings. Smoke and game launches
     * must be able to select their own log directory and diagnostics. */
    AppendD3dBackendEnv(envStrs, options.d3dBackend, options.dxvkBackend, binDir);
    for (const std::string& line : options.environment) UpsertEnvLine(envStrs, line);
    UpsertEnvLine(envStrs, "WINEHUA_D3D_BACKEND=" + options.d3dBackend);
    UpsertEnvLine(envStrs, "WINEHUA_PRESENT_BACKEND=" + options.presentBackend);
    UpsertEnvLine(envStrs, std::string("WINEHUA_AUTOMATION=") +
                  (options.automationMode ? "1" : "0"));
    /* desktop 模式: 将进程接入 explorer 创建的 shell desktop, 使其窗口
     * 出现在任务栏 (与 RunWineExe 路径对称, 重构 runWineProgram 时遗漏). */
    if (WaylandServer::GetInstance()->IsDesktopMode())
        UpsertEnvLine(envStrs,"WINEHUA_DESKTOP=shell");
    /* DXVK is a managed WineHua runtime overlay, never a game-provided DLL. */
    if (options.d3dBackend.rfind("dxvk_", 0) == 0 ||
        options.d3dBackend == "vkd3d_limited_500k")
        OH_LOG_INFO(LOG_APP, "[WineProgram] managed D3D backend=%{public}s",
                    options.d3dBackend.c_str());
#ifdef __aarch64__
    UpsertEnvLine(envStrs,"WINEHUA_WINE_UNIX_ARCH=aarch64");
#else
    UpsertEnvLine(envStrs,"WINEHUA_WINE_UNIX_ARCH=x86_64");
#endif
    UpsertEnvLine(envStrs,"WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    if (!options.workingDirectory.empty())
        UpsertEnvLine(envStrs,"WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);

    // wine 前缀 token: box64 (方案②) 不带 — argv[0] 会落到 guest main_argv[1] 被
    // 当作程序名 (start.exe fallback → exit(1)); 原生 (方案①③) __wine_main 直启
    // 需要 argv[0]=wine 作 loader 名。
#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
    std::string entryParams = binDir + "|" + exePath;
#else
    std::string entryParams = binDir + "|wine|" + exePath;
#endif
    for (const std::string& arg : options.argv) entryParams += "|" + arg;

    const pid_t pid = SpawnViaBroker(entryParams, envStrs);
    if (pid <= 0) return -1;
    AddProcess(pid, options.windowsExePath, -1);
    OH_LOG_INFO(LOG_APP,
                "[WineProgram] pid=%{public}d exe=%{public}s prefix=%{public}s d3d=%{public}s present=%{public}s automation=%{public}s",
                pid, exePath.c_str(), prefixDir.c_str(), options.d3dBackend.c_str(),
                options.presentBackend.c_str(), options.automationMode ? "true" : "false");
    if (gStateTsfn)
    {
        char state[64];
        // broker 已受理 spawn — 仅表示进程拉起, 不代表窗口出现 (闪退检测靠
        // evt:proc-exited, 见 ArkTS 启动反馈状态机)
        snprintf(state, sizeof(state), "evt:launch-accepted:%d", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(state), napi_tsfn_blocking);
    }
    return pid;
}

static pid_t SpawnGuestProgram(const GuestProgramOptions& options)
{
    const std::string guestRoot = std::string(WINE_RUNTIME_BIN) + "/guest_vulkan";
    if (options.executablePath.rfind(guestRoot + "/", 0) != 0 ||
        HasUnsafeProtocolChar(options.executablePath))
        return -1;
    for (const std::string& arg : options.argv) if (HasUnsafeProtocolChar(arg)) return -1;

    const std::string binDir = WINE_RUNTIME_BIN;
    const std::string gfxLib = binDir + "/guest_gfx/lib";
    const std::string unixLib = binDir + "/" WINE_UNIX_SUBDIR;
    // 需要 dlopen 的 guest 原生库 (el1 bundle): venus loader/ICD/smoke 平铺目录,
    // 加入 LD_LIBRARY_PATH 让 smoke .so 的 DT_NEEDED libvulkan.so.1 能解析.
    // guest Vulkan Loader 就在 el1 顶层 (assemble.sh); host vkr 进程用绝对路径
    // dlopen("/system/lib64/libvulkan.so") 加载系统 Vulkan, 不走名字搜索, 所以
    // 顶层 guest loader 不会遮蔽 host vkr.
    //
    // 注意: LD_LIBRARY_PATH 绝不能含 el2 data 区的 guest_vulkan/lib — 那里的
    // libvulkan.so.1 open() 能成功 (fd>=0) 但 mmap/加载被沙箱拦截 (EINVAL),
    // musl path_open 对首个 open 成功的路径不再回退后续路径 → 整体失败.
    // el2 guest_vulkan/lib 的 3 个库在 el1 均有副本, 故直接排除 el2 路径.
    const std::string el1Lib = std::string("/data/storage/el1/bundle/libs/") +
#ifdef __aarch64__
        "arm64";
#else
        "x86_64";
#endif
    // LD_LIBRARY_PATH 只留 el1 顶层: 鸿蒙 musl namespace 对含 el2 data 区路径的
    // LD_LIBRARY_PATH 可能整体拒绝搜索 (check ns accessible failed). smoke
    // DT_NEEDED 仅 libvulkan.so.1 + libc.so, el2 的 guest_gfx/lib、wine/bin、
    // aarch64-unix 全无用.
    const std::string libraryPath = el1Lib;
    const std::string icd = guestRoot +
        "/share/vulkan/icd.d/venus_icd." WINE_WINE_ARCH ".json";

    std::vector<std::string> envStrs = BuildWineEnv(
        WINE_PREFIX, "wine-wayland", libraryPath, binDir, -1,
        WINE_AUTOMATION_HOME, WINE_PREFIX);
    // Wine 与设备同架构: LD_LIBRARY_PATH 统一 (arm64 原生 / x86_64)
    UpsertEnvLine(envStrs,"LD_LIBRARY_PATH=" + libraryPath);
    UpsertEnvLine(envStrs,"VK_DRIVER_FILES=" + icd);
    UpsertEnvLine(envStrs,"VK_ICD_FILENAMES=" + icd);
    UpsertEnvLine(envStrs,"VN_DEBUG=vtest,result");
    // OHOS Host Vulkan memory uses an explicit SHM shadow when the driver
    // cannot export dma-buf/opaque-fd memory. GPU fence and query feedback
    // writes only the Host mapping, so query the real Host objects instead
    // of polling stale Guest feedback slots.
    UpsertEnvLine(envStrs,"VN_PERF=no_fence_feedback,no_query_feedback");
    UpsertEnvLine(envStrs,"WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    UpsertEnvLine(envStrs,std::string("WINEHUA_AUTOMATION=") +
              (options.automationMode ? "1" : "0"));
    if (!options.workingDirectory.empty())
        UpsertEnvLine(envStrs,"WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);
    for (const std::string& line : options.environment) UpsertEnvLine(envStrs,line);

    std::string entryParams = binDir + "|__winehua_guest_elf__|" + options.executablePath;
    for (const std::string& arg : options.argv) entryParams += "|" + arg;

    const pid_t pid = SpawnViaBroker(entryParams, envStrs);
    if (pid <= 0) return -1;
    AddProcess(pid, options.executablePath, -1);
    OH_LOG_INFO(LOG_APP, "[GuestProgram] pid=%{public}d elf=%{public}s icd=%{public}s",
                pid, options.executablePath.c_str(), icd.c_str());
    return pid;
}

static bool ResolveManagedHostExecutable(const std::string& requested,
                                         std::string* resolved)
{
    const std::string managedRoot = std::string(WINE_RUNTIME_BIN) + "/host_vulkan";
    char rootPath[PATH_MAX] = {};
    char executablePath[PATH_MAX] = {};
    struct stat info = {};

    if (!realpath(managedRoot.c_str(), rootPath) ||
        !realpath(requested.c_str(), executablePath))
        return false;
    const std::string rootPrefix = std::string(rootPath) + "/";
    if (std::string(executablePath).rfind(rootPrefix, 0) != 0 ||
        stat(executablePath, &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    *resolved = executablePath;
    return true;
}

static pid_t SpawnHostProgram(const HostProgramOptions& options)
{
    std::string executablePath;
    if (options.executablePath.empty() || HasUnsafeProtocolChar(options.executablePath) ||
        !ResolveManagedHostExecutable(options.executablePath, &executablePath))
        return -1;
    for (const std::string& arg : options.argv)
        if (HasUnsafeProtocolChar(arg)) return -1;

    std::vector<std::string> envStrs = options.environment;
    UpsertEnvLine(envStrs,"HOME=" + std::string(WINE_AUTOMATION_HOME));
    UpsertEnvLine(envStrs,"TMPDIR=" + std::string(WINE_TMPDIR));
    UpsertEnvLine(envStrs,"WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    UpsertEnvLine(envStrs,std::string("WINEHUA_AUTOMATION=") +
              (options.automationMode ? "1" : "0"));
    if (!options.workingDirectory.empty())
        UpsertEnvLine(envStrs,"WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);

    std::string entryParams = std::string(WINE_RUNTIME_BIN) +
        "|__winehua_host_elf__|" + executablePath;
    for (const std::string& arg : options.argv) entryParams += "|" + arg;

    const pid_t pid = SpawnViaBroker(entryParams, envStrs);
    if (pid <= 0) return -1;
    AddProcess(pid, executablePath, -1);
    OH_LOG_INFO(LOG_APP, "[HostProgram] pid=%{public}d elf=%{public}s",
                pid, executablePath.c_str());
    return pid;
}

} // namespace

// 经 broker Unix socket 发送 SPAWN 请求, 返回子进程 pid, <= 0 表示失败。
// 全局作用域 (wine_exe.h 声明): wine_exe.cpp 内部与 wine_launch.cpp
// (explorer 桌面模式) 共用同一实现, 避免复制第二份 broker 协议代码。
pid_t SpawnViaBroker(const std::string& entryParams,
                     const std::vector<std::string>& environment)
{
    const char* brokerPath = getenv("PROCESSBROKER");
    if (!brokerPath || !brokerPath[0]) brokerPath = WINE_BROKER_SOCKET;
    int brokerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (brokerFd < 0) return -1;

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (strlen(brokerPath) >= sizeof(address.sun_path))
    {
        close(brokerFd);
        return -1;
    }
    strcpy(address.sun_path, brokerPath);
    if (connect(brokerFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Program] broker connect failed: %{public}s", strerror(errno));
        close(brokerFd);
        return -1;
    }

    /* The broker protocol has one authoritative environment channel:
     * |__env=KEY=VALUE segments embedded in entryParams.  The old ENV blob
     * trailer was removed with the broker-global session environment; leaving
     * it here makes children silently inherit only Wine's baseline and causes
     * DXVK/Venus smoke to resolve the builtin d3d11.dll. */
    const std::string requestParams = entryParams + SerializeEnvToEntryParams(environment);
    static constexpr char header[] = "SPAWN\n";
    std::string requestTail = requestParams + "\n";
    iovec iov[2] = {
        {const_cast<char*>(header), sizeof(header) - 1},
        {const_cast<char*>(requestTail.data()), requestTail.size()},
    };
    msghdr message = {};
    message.msg_iov = iov;
    message.msg_iovlen = 2;
    if (sendmsg(brokerFd, &message, MSG_NOSIGNAL) < 0)
    {
        close(brokerFd);
        return -1;
    }

    int32_t response[2] = {-1, -1};
    ssize_t received = recv(brokerFd, response, sizeof(response), MSG_WAITALL);
    close(brokerFd);
    if (received != sizeof(response) || response[1] != 0 || response[0] <= 0) return -1;
    return response[0];
}

// 公开入口 (wine_launch.cpp 自动拉起 explorer 复用): 转发到匿名
// namespace 内的实现, 后者依赖 PrefixForMode/SpawnViaBroker 等内部函数。
int SpawnWineProgram(const ProgramOptions& options)
{
    return SpawnWineProgramImpl(options);
}

napi_value RunWineProgram(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeProcessObject(env, nullptr, false);

    napi_valuetype type;
    if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object)
        return MakeProcessObject(env, nullptr, false);

    ProgramOptions options;
    options.windowsExePath = GetString(env, args[0], "windowsExePath");
    options.workingDirectory = GetString(env, args[0], "workingDirectory");
    options.prefixMode = GetString(env, args[0], "prefixMode", "reuse");
    options.d3dBackend = GetString(env, args[0], "d3dBackend", "vkd3d_limited_500k");
    const std::string impliedDxvkBackend = options.d3dBackend == "dxvk_modern_2_6"
        ? "dxvk_modern_2_6" : "dxvk_legacy";
    options.dxvkBackend = GetString(env, args[0], "dxvkBackend", impliedDxvkBackend.c_str());
    if (options.dxvkBackend != "dxvk_legacy" &&
        options.dxvkBackend != "dxvk_modern_2_6")
        options.dxvkBackend = impliedDxvkBackend;
    options.presentBackend = GetString(env, args[0], "presentBackend", "virgl_compositor");
    options.automationMode = GetBool(env, args[0], "automationMode", false);
    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);
    OH_LOG_INFO(LOG_APP,
                "[WineProgram] parsed options exe=%{public}s argc=%{public}zu env=%{public}zu",
                options.windowsExePath.c_str(), options.argv.size(), options.environment.size());

    const pid_t pid = SpawnWineProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value RunGuestProgram(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeProcessObject(env, nullptr, false);

    napi_valuetype type;
    if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object)
        return MakeProcessObject(env, nullptr, false);

    GuestProgramOptions options;
    options.executablePath = GetString(env, args[0], "executablePath");
    options.workingDirectory = GetString(env, args[0], "workingDirectory");
    options.automationMode = GetBool(env, args[0], "automationMode", true);
    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);

    const pid_t pid = SpawnGuestProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value RunHostProgram(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeProcessObject(env, nullptr, false);

    napi_valuetype type;
    if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object)
        return MakeProcessObject(env, nullptr, false);

    HostProgramOptions options;
    options.executablePath = GetString(env, args[0], "executablePath");
    options.workingDirectory = GetString(env, args[0], "workingDirectory");
    options.automationMode = GetBool(env, args[0], "automationMode", true);
    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);

    const pid_t pid = SpawnHostProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value RunHostReplay(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    bool started = false;
    napi_valuetype type;
    if (argc >= 1 && napi_typeof(env, args[0], &type) == napi_ok && type == napi_object)
    {
        HostProgramOptions options;
        options.executablePath = GetString(env, args[0], "executablePath");
        ReadStringArray(env, args[0], "argv", &options.argv);
        std::string managedPath;
        if (ResolveManagedHostExecutable(options.executablePath, &managedPath) &&
            !gHostReplayRunning.exchange(true, std::memory_order_acq_rel))
        {
            void *module = dlopen("libwinehua_host_heaven_replay.so", RTLD_NOW | RTLD_LOCAL);
            HostReplayMain replayMain = module ? reinterpret_cast<HostReplayMain>(
                dlsym(module, "winehua_host_replay_main")) : nullptr;
            if (!replayMain)
            {
                const char *loadError = dlerror();
                OH_LOG_ERROR(LOG_APP, "[HostReplay] signed module unavailable: %{public}s",
                             loadError ? loadError : "unknown");
                gHostReplayRunning.store(false, std::memory_order_release);
            }
            else
            {
                std::thread([managedPath = std::move(managedPath),
                             replayArgs = std::move(options.argv), replayMain]() mutable {
                    std::vector<char*> argv;
                    argv.reserve(replayArgs.size() + 2);
                    argv.push_back(const_cast<char*>(managedPath.c_str()));
                    for (std::string& argument : replayArgs)
                        argv.push_back(const_cast<char*>(argument.c_str()));
                    argv.push_back(nullptr);
                    OH_LOG_INFO(LOG_APP, "[HostReplay] main-process worker started argc=%{public}zu",
                                argv.size() - 1);
                    const int result = replayMain(static_cast<int>(argv.size() - 1), argv.data());
                    OH_LOG_INFO(LOG_APP, "[HostReplay] main-process worker finished rc=%{public}d",
                                result);
                    gHostReplayRunning.store(false, std::memory_order_release);
                }).detach();
                started = true;
            }
        }
    }

    napi_value result;
    napi_get_boolean(env, started, &result);
    return result;
}

napi_value IsHostReplayRunning(napi_env env, napi_callback_info)
{
    napi_value result;
    napi_get_boolean(env, gHostReplayRunning.load(std::memory_order_acquire), &result);
    return result;
}

napi_value QueryWineProcess(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    int32_t pid = -1;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) napi_get_value_int32(env, args[0], &pid);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value TerminateWineProcess(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    int32_t pid = -1;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) napi_get_value_int32(env, args[0], &pid);
    OH_LOG_WARN(LOG_APP,
                "[WineProgram] terminateWineProcess requested pid=%{public}d signal=SIGKILL",
                pid);
    const bool ok = pid > 0 && kill(pid, SIGKILL) == 0;
    if (!ok) {
        OH_LOG_WARN(LOG_APP,
                    "[WineProgram] terminateWineProcess failed pid=%{public}d errno=%{public}d(%{public}s)",
                    pid, errno, strerror(errno));
    }
    if (ok) RemoveProcess(pid, -1, "unknown");
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

napi_value RunWineExe(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value args[5] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) return MakeProcessObject(env, nullptr, false);

    char binDir[512] = {}, sockPath[512] = {}, libPath[2048] = {}, wineExe[1024] = {}, homePath[1024] = {};
    napi_get_value_string_utf8(env, args[0], binDir, sizeof(binDir), nullptr);
    napi_get_value_string_utf8(env, args[1], sockPath, sizeof(sockPath), nullptr);
    napi_get_value_string_utf8(env, args[2], libPath, sizeof(libPath), nullptr);
    napi_get_value_string_utf8(env, args[3], wineExe, sizeof(wineExe), nullptr);
    if (argc >= 5) {
        napi_get_value_string_utf8(env, args[4], homePath, sizeof(homePath), nullptr);
    }

    std::string homeDir(homePath);
    if (homeDir.empty()) homeDir = gBrokerHomeDir;
    if (homeDir.empty()) homeDir = "/storage/Users/currentUser/Download";

    std::string exePath(wineExe);
    {
        std::string lower = exePath;
        for (auto& c : lower) c = tolower(c);
        if (lower.find("/drive_c/") != std::string::npos) {
            auto slash = exePath.find_last_of('/');
            if (slash != std::string::npos) exePath = exePath.substr(slash + 1);
        }
    }

    OH_LOG_INFO(LOG_APP, "[Wine] runWineExe bin=%{public}s exe=%{public}s (final=%{public}s) home=%{public}s",
                binDir, wineExe, exePath.c_str(), homeDir.c_str());

    std::string sockStr(sockPath);
    auto pos = sockStr.find_last_of('/');
    std::string sockDir = (pos == std::string::npos) ? "/tmp" : sockStr.substr(0, pos);
    std::string sockName = (pos == std::string::npos) ? sockStr : sockStr.substr(pos + 1);

    int audioBootstrapFd = -1;  // broker 会为每个子进程创建 audio fd, 此处无需传递

    std::vector<std::string> wineEnv = BuildWineEnv(sockDir, sockName, libPath, binDir, audioBootstrapFd, homeDir);

    // desktop 模式: 将进程接入 explorer 创建的 shell desktop,
    // 使其窗口出现在任务栏, 且能与其他 shell 进程互相访问
    if (WaylandServer::GetInstance()->IsDesktopMode())
        wineEnv.push_back("WINEHUA_DESKTOP=shell");

    {
        // wine 前缀 token 的规则同 LaunchWineProgram: 仅 box64 (方案②) 不带;
        // 方案③ arm64 原生 (__aarch64__ 但非 WINEHUA_WINE_ARCH_IS_X86_64) 走
        // __wine_main 直启, 仍需 argv[0]=wine —— 不能用纯 __aarch64__ 判定。
#if defined(__aarch64__) && defined(WINEHUA_WINE_ARCH_IS_X86_64)
        std::string entryParams = std::string(binDir) + "|" + exePath;
#else
        std::string entryParams = std::string(binDir) + "|wine|" + exePath;
#endif
        // NOTE: RunWineExe 不调 AppendD3dBackendEnv —— 此路径从 ArkTS Index.ets
        // 手动启动 Wine exe（如 explorer 文件管理器），D3D 后端由调用者通过
        // d3dLaunchEnvironment 单独指定；explorer 本身不需要 DXVK overlay。
        // env 序列化由 SpawnViaBroker 内部完成（区别于旧代码在本函数内手动拼接）。
        OH_LOG_INFO(LOG_APP, "[Wine] runWineExe via broker: %{public}s|__env=...", entryParams.c_str());

        pid_t pid = SpawnViaBroker(entryParams, wineEnv);
        if (pid <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker spawn failed");
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("evt:launch-failed"), napi_tsfn_blocking);
            return nullptr;
        }

        AddProcess(pid, wineExe, -1);
        OH_LOG_INFO(LOG_APP, "[Wine] wine pid=%{public}d exe=%{public}s (via broker)", pid, wineExe);
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "evt:launch-accepted:%d", pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
    }
    return nullptr;
}

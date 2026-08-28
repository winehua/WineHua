#include "wine_exe.h"

#include "broker.h"
#include "env_profiles.h"
#include "spawner.h"
#include "graphics_broker.h"
#include "wayland_server.h"
#include "wine_constants.h"
#include "wine_env.h"
#include "wine_process.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

extern napi_threadsafe_function gStateTsfn;

namespace {

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

// UpsertEnvLine 在 wine_env.h 中声明，统一读写 env vector

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
    const std::string prefixDir = WINE_PREFIX;
    const std::string homeDir = gBrokerHomeDir.empty() ?
        "/storage/Users/currentUser/Download" : gBrokerHomeDir;
    const std::string sockDir = prefixDir;
    const std::string sockName = "wine-wayland";
    const std::string libPath = binDir + ":" + binDir + "/x86_64-unix";
    const std::string exePath = NativePathToWindows(options.windowsExePath, prefixDir);

    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    winehua::GraphicsBroker::GetInstance().SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    if (!winehua::GraphicsBroker::GetInstance().EnsureStarted(prefixDir)) return -1;
    winehua::GraphicsBroker::GetInstance().SetVulkanPresentMode(
        options.presentBackend == "venus_broker_present" ||
        options.presentBackend == "venus_direct_present");

    // 声明式 env 管线 (env_profiles.cpp): 基线+D3D overlay 由 policy 字段声明,
    // per-run 覆盖 (options.environment) 与进程标记经 extraEnv 最后写入
    // (与旧顺序一致: 产品默认在前, per-run 设置可压过它们, 进程标记再后)。
    winehua::SessionEnvPolicy policy;
    policy.sockDir = sockDir;
    policy.sockName = sockName;
    policy.libPath = libPath;
    policy.binDir = binDir;
    policy.homeDir = homeDir;
    policy.prefixDir = prefixDir;
    policy.d3dBackend = options.d3dBackend;
    policy.dxvkBackend = options.dxvkBackend;
    policy.desktopShellFlag = WaylandServer::GetInstance()->IsDesktopMode();
    policy.extraEnv = options.environment;
    policy.extraEnv.push_back("WINEHUA_D3D_BACKEND=" + options.d3dBackend);
    policy.extraEnv.push_back("WINEHUA_PRESENT_BACKEND=" + options.presentBackend);
    /* desktop 模式: 将进程接入 explorer 创建的 shell desktop, 使其窗口
     * 出现在任务栏 (与 RunWineExe 路径对称, 重构 runWineProgram 时遗漏). */
    /* DXVK is a managed WineHua runtime overlay, never a game-provided DLL. */
    if (options.d3dBackend.rfind("dxvk_", 0) == 0 ||
        options.d3dBackend == "vkd3d_limited_500k")
        OH_LOG_INFO(LOG_APP, "[WineProgram] managed D3D backend=%{public}s",
                    options.d3dBackend.c_str());
    policy.extraEnv.push_back("WINEHUA_WINE_UNIX_ARCH=x86_64");
    policy.extraEnv.push_back("WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    if (!options.workingDirectory.empty())
        policy.extraEnv.push_back("WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);
    std::vector<std::string> envStrs = winehua::BuildSessionEnv(policy);

    winehua::SpawnRequest req{winehua::SpawnKind::WineExe};
    req.argv.push_back(exePath);
    req.argv.insert(req.argv.end(), options.argv.begin(), options.argv.end());
    req.env = std::move(envStrs);

    const pid_t pid = winehua::Spawner::Spawn(req);
    if (pid <= 0) return -1;
    AddProcess(pid, options.windowsExePath, -1);
    OH_LOG_INFO(LOG_APP,
                "[WineProgram] pid=%{public}d exe=%{public}s prefix=%{public}s d3d=%{public}s present=%{public}s",
                pid, exePath.c_str(), prefixDir.c_str(), options.d3dBackend.c_str(),
                options.presentBackend.c_str());
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

} // namespace

// 经 broker Unix socket 发送 SPAWN 请求, 返回子进程 pid, <= 0 表示失败。
// 调用方收口在 spawner.cpp (SpawnKind::DesktopShell/WineExe 等);
// 保留全局函数只因 broker 协议实现不应复制第二份。
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
     * DXVK/Venus 程序错误地解析到内置 d3d11.dll。 */
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
    options.d3dBackend = GetString(env, args[0], "d3dBackend", "vkd3d_limited_500k");
    const std::string impliedDxvkBackend = options.d3dBackend == "dxvk_modern_2_6"
        ? "dxvk_modern_2_6" : "dxvk_legacy";
    options.dxvkBackend = GetString(env, args[0], "dxvkBackend", impliedDxvkBackend.c_str());
    if (options.dxvkBackend != "dxvk_legacy" &&
        options.dxvkBackend != "dxvk_modern_2_6")
        options.dxvkBackend = impliedDxvkBackend;
    options.presentBackend = GetString(env, args[0], "presentBackend", "virgl_compositor");
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

    // 声明式 env 管线 (env_profiles.cpp)。d3dBackend 留空 = 不注入 D3D
    // overlay: 此路径从 ArkTS 手动启动 Wine exe（如 explorer 文件管理器），
    // D3D 后端由调用者通过 d3dLaunchEnvironment 单独指定；explorer 本身
    // 不需要 DXVK overlay。
    winehua::SessionEnvPolicy policy;
    policy.sockDir = sockDir;
    policy.sockName = sockName;
    policy.libPath = libPath;
    policy.binDir = binDir;
    policy.homeDir = homeDir;
    // desktop 模式: 将进程接入 explorer 创建的 shell desktop,
    // 使其窗口出现在任务栏, 且能与其他 shell 进程互相访问
    policy.desktopShellFlag = WaylandServer::GetInstance()->IsDesktopMode();
    std::vector<std::string> wineEnv = winehua::BuildSessionEnv(policy);

    {
        // ArkTS 显式传的 binDir 经 SpawnRequest.binDir 透传 (不依赖会话默认);
        // env 序列化由 broker 通道内部完成。
        winehua::SpawnRequest req{winehua::SpawnKind::WineExe};
        req.binDir = binDir;
        req.argv.push_back(exePath);
        req.env = std::move(wineEnv);
        OH_LOG_INFO(LOG_APP, "[Wine] runWineExe via broker: %{public}s|...", exePath.c_str());

        pid_t pid = winehua::Spawner::Spawn(req);
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

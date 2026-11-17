#include "proc/spawn_codec.h"
#include "wine_exe.h"

#include "proc/broker.h"
#include "env_profiles.h"
#include "proc/spawner.h"
#include "graphics/graphics_broker.h"
#include "compositor/wayland_server.h"
#include "wine_constants.h"
#include "container_session.h"
#include "graphics/graphics_profile.h"
#include "direct/direct_vulkan_desktop_compositor.h"
#include "wine_env.h"
#include "proc/wine_process.h"

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

static bool HasEmbeddedNul(const std::string& value)
{
    return value.find('\0') != std::string::npos;
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
        if (!HasEmbeddedNul(line)) out->push_back(std::move(line));
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
    setString("sessionId", WineSessionId(*entry));
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
    if (options.windowsExePath.empty() || HasEmbeddedNul(options.windowsExePath)) return -1;
    for (const std::string& arg : options.argv) if (HasEmbeddedNul(arg)) return -1;

    const winehua::ContainerSession session = options.containerId.empty()
        ? winehua::GetActiveContainerSession()
        : [&options]() {
            winehua::ContainerSession resolved;
            if (!winehua::ResolveContainerSession(options.containerId, &resolved)) return winehua::ContainerSession{};
            return resolved;
        }();
    if (session.id.empty() ||
        (!options.containerId.empty() && !winehua::IsActiveContainerSession(session.id))) {
        OH_LOG_ERROR(LOG_APP, "[WineProgram] rejected inactive or invalid container=%{public}s",
                     options.containerId.c_str());
        return -1;
    }

    const std::string binDir = WINE_RUNTIME_BIN;
    const std::string prefixDir = session.prefixDir;
    const std::string homeDir = gBrokerHomeDir.empty() ?
        "/storage/Users/currentUser/Download" : gBrokerHomeDir;
    const std::string sockDir = prefixDir;
    const std::string sockName = "wine-wayland";
    const std::string libPath = binDir + ":" + binDir + "/" WINE_UNIX_SUBDIR;
    const std::string exePath = NativePathToWindows(options.windowsExePath, prefixDir);

    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    winehua::GraphicsBroker::GetInstance().SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    if (!winehua::GraphicsBroker::GetInstance().EnsureStarted(prefixDir)) return -1;
    // Child programs cannot select a different consumer for the active
    // desktop. Old callers still pass Venus here even in a Direct session.
    const bool vulkanD3d = winehua::UsesVenusPresent(
        winehua::ParseD3dBackend(options.d3dBackend));
    const std::string presentBackend = vulkanD3d
        ? (winehua::direct::DirectDesktopVulkanEnabled()
            ? "direct_vulkan_present" : "venus_broker_present")
        : options.presentBackend;
    winehua::GraphicsBroker::GetInstance().SetVulkanPresentMode(
        !winehua::direct::DirectDesktopVulkanEnabled() &&
        (presentBackend == "venus_broker_present" ||
         presentBackend == "venus_direct_present"));

    // 声明式 env 管线 (env_profiles.cpp): 基线+D3D overlay+稳定化 overlay 由
    // policy 字段声明, per-run 覆盖 (options.environment) 与进程标记经 extraEnv
    // 最后写入 (与旧顺序一致: 产品默认在前, per-run 设置可压过它们, 进程标记再后)。
    winehua::SessionEnvPolicy policy;
    policy.sockDir = sockDir;
    policy.sockName = sockName;
    policy.libPath = libPath;
    policy.binDir = binDir;
    policy.homeDir = homeDir;
    policy.prefixDir = prefixDir;
    // Managed child programs inherit the active desktop/container locale.
    // Per-launch LC_ALL/LANG overrides still take precedence in extraEnv.
    policy.wineLang = winehua::GetActiveContainerSession().wineLang;
    policy.d3dBackend = options.d3dBackend;
    policy.dxvkBackend = options.dxvkBackend;
    // DXVK 稳定化默认值 (DXVK_LOG/perf profile/WEAKBARRIER clamp 等) 与桌面
    // 会话链同一来源 (AppendStableDxvkEnv) — 历史上由 ArkTS
    // d3dLaunchEnvironment 平行维护一份拷贝, 已收口; 非 DXVK 后端
    // overlay 内 early-return, extraEnv 最后写入仍可压过产品默认。
    policy.applyStableOverlay = true;
    policy.desktopShellFlag = WaylandServer::GetInstance()->IsDesktopMode();
    policy.extraEnv = options.environment;
    policy.extraEnv.push_back("WINEHUA_D3D_BACKEND=" + options.d3dBackend);
    policy.extraEnv.push_back("WINEHUA_PRESENT_BACKEND=" + presentBackend);
    /* desktop 模式: 将进程接入 explorer 创建的 shell desktop, 使其窗口
     * 出现在任务栏 (与 RunWineExe 路径对称, 重构 runWineProgram 时遗漏). */
    /* DXVK is a managed WineHua runtime overlay, never a game-provided DLL. */
    if (options.d3dBackend == "dxvk_legacy" ||
        options.d3dBackend == "dxvk_modern_2_6")
        OH_LOG_INFO(LOG_APP, "[WineProgram] managed D3D backend=%{public}s",
                    options.d3dBackend.c_str());
    // WINEHUA_WINE_UNIX_ARCH 描述 wine unix 侧架构 (= WINE_ARCH), 方案①② 为
    // x86_64, 方案③ 为 aarch64 — 按 .wine_arch 编译宏判定 (不能用 __aarch64__:
    // 方案② 宿主机是 arm64 但 wine 是 x86_64)。
#ifdef WINEHUA_WINE_ARCH_IS_X86_64
    policy.extraEnv.push_back("WINEHUA_WINE_UNIX_ARCH=x86_64");
#else
    policy.extraEnv.push_back("WINEHUA_WINE_UNIX_ARCH=aarch64");
#endif
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
    BeginRegisteredWineSession(pid);
    OH_LOG_INFO(LOG_APP,
                "[WineProgram] pid=%{public}d exe=%{public}s prefix=%{public}s d3d=%{public}s present=%{public}s",
                pid, exePath.c_str(), prefixDir.c_str(), options.d3dBackend.c_str(),
                presentBackend.c_str());
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
pid_t SpawnViaBroker(const std::string& binDir, const std::vector<std::string>& argv,
                     const std::vector<std::string>& environment)
{
    winehua::spawn::Request request{"", binDir, argv, {}, {}};
    for (const auto& line : environment)
        if (!vp_is_process_env(line.c_str())) request.env.push_back(line);
    std::vector<unsigned char> bytes;
    if (!winehua::spawn::Encode(request, bytes)) {
        OH_LOG_ERROR(LOG_APP, "[Program] invalid or oversized startup request");
        return -1;
    }
    const char* path = getenv("PROCESSBROKER");
    if (!path || !path[0]) path = WINE_BROKER_SOCKET;
    const int64_t deadline = vp_now_ms() + 45000;
    int fd = vp_connect_path(path, deadline);
    if (fd < 0) return -1;
    vp_spawn_spec spec{};
    int pid = -1, status = -1;
    bool ok = vp_decode(bytes.data(), bytes.size(), &spec) == 0 &&
        vp_send_request(fd, &spec, nullptr, deadline) == 0 &&
        vp_recv_reply(fd, &pid, &status, deadline) == 0;
    vp_free_spec(&spec);
    const int error = errno;
    close(fd);
    if (!ok || status || pid <= 0) {
        OH_LOG_ERROR(LOG_APP, "[Program] broker request failed errno=%{public}d status=%{public}d", error, status);
        return -1;
    }
    return pid;
}


int SpawnWineProgram(const ProgramOptions& options)
{
    return SpawnWineProgramImpl(options);
}

// Default for old callers; SpawnWineProgram pairs Vulkan presentation with
// the consumer already selected by the session owner.
static std::string DerivePresentBackend(const std::string& d3dBackend)
{
    const bool gpuBackend = winehua::UsesVenusPresent(winehua::ParseD3dBackend(d3dBackend));
    return gpuBackend ? "venus_broker_present" : "virgl_compositor";
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
    options.containerId = GetString(env, args[0], "containerId");
    options.d3dBackend = GetString(env, args[0], "d3dBackend", "dxvk_legacy");
    if (options.d3dBackend != "dxvk_legacy" && options.d3dBackend != "dxvk_modern_2_6" &&
        options.d3dBackend != "wined3d" && options.d3dBackend != "wined3d_zink" &&
        options.d3dBackend != "dxvk_legacy_d3d9")
        options.d3dBackend = "dxvk_legacy";
    options.dxvkBackend = options.d3dBackend == "dxvk_modern_2_6" ?
        "dxvk_modern_2_6" : "dxvk_legacy";
    options.presentBackend = GetString(env, args[0], "presentBackend");
    if (options.presentBackend.empty())
        options.presentBackend = DerivePresentBackend(options.d3dBackend);

    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);
    OH_LOG_INFO(LOG_APP,
                "[WineProgram] parsed options exeBytes=%{public}zu argc=%{public}zu envCount=%{public}zu "
                "d3d=%{public}s dxvk=%{public}s",
                options.windowsExePath.size(), options.argv.size(), options.environment.size(),
                options.d3dBackend.c_str(), options.dxvkBackend.c_str());

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
    WineProcessEntry entry;
    const bool ok = pid > 0 && QueryProcessSnapshot(pid, &entry) &&
        StopRegisteredWineSession(WineSessionId(entry));
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value MakeLaunchResult(napi_env env, int32_t pid,
                                   const std::string& sessionId, bool reused)
{
    napi_value result, pidValue, sessionValue, reusedValue;
    napi_create_object(env, &result);
    napi_create_int32(env, pid, &pidValue);
    napi_create_string_utf8(env, sessionId.c_str(), NAPI_AUTO_LENGTH, &sessionValue);
    napi_get_boolean(env, reused, &reusedValue);
    napi_set_named_property(env, result, "pid", pidValue);
    napi_set_named_property(env, result, "sessionId", sessionValue);
    napi_set_named_property(env, result, "reused", reusedValue);
    return result;
}


// ================= VPP 10-arg RunWineExe (ours parsing x theirs proton pipeline) =================
napi_value RunWineExe(napi_env env, napi_callback_info info)
{
    size_t argc = 10;
    napi_value args[10] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) return MakeLaunchResult(env, -1, "", false);

    char binDir[512] = {}, sockPath[512] = {}, libPath[2048] = {}, wineExe[1024] = {},
         homePath[1024] = {}, workingDirectoryPath[1024] = {}, d3dBackend[64] = "dxvk_legacy";
    napi_get_value_string_utf8(env, args[0], binDir, sizeof(binDir), nullptr);
    napi_get_value_string_utf8(env, args[1], sockPath, sizeof(sockPath), nullptr);
    napi_get_value_string_utf8(env, args[2], libPath, sizeof(libPath), nullptr);
    napi_get_value_string_utf8(env, args[3], wineExe, sizeof(wineExe), nullptr);
    if (argc >= 5) {
        napi_get_value_string_utf8(env, args[4], homePath, sizeof(homePath), nullptr);
    }
    std::vector<std::string> launchArguments;
    bool argumentArray = false;
    if (argc >= 6) napi_is_array(env, args[5], &argumentArray);
    if (argumentArray) {
        uint32_t length = 0;
        napi_get_array_length(env, args[5], &length);
        for (uint32_t index = 0; index < length; index++) {
            napi_value item;
            napi_get_element(env, args[5], index, &item);
            size_t size = 0;
            napi_get_value_string_utf8(env, item, nullptr, 0, &size);
            std::string value(size + 1, '\0');
            napi_get_value_string_utf8(env, item, value.data(), value.size(), &size);
            value.resize(size);
            launchArguments.push_back(value);
        }
    }
    if (argc >= 7) {
        napi_get_value_string_utf8(env, args[6], workingDirectoryPath,
                                   sizeof(workingDirectoryPath), nullptr);
    }
    if (argc >= 8) {
        char requestedBackend[64] = {};
        napi_get_value_string_utf8(env, args[7], requestedBackend,
                                   sizeof(requestedBackend), nullptr);
        const winehua::D3dBackendKind backend =
            winehua::ParseD3dBackend(requestedBackend);
        if (backend == winehua::D3dBackendKind::WineD3d ||
            backend == winehua::D3dBackendKind::WineD3dZink ||
            winehua::IsDxvkBackend(backend)) {
            strncpy(d3dBackend, requestedBackend, sizeof(d3dBackend) - 1);
        } else {
            OH_LOG_ERROR(LOG_APP,
                         "[Wine] rejected unsupported d3d backend=%{public}s",
                         requestedBackend);
            return MakeLaunchResult(env, -1, "", false);
        }
    }
    std::vector<std::string> envOverrides;
    bool envArray = false;
    if (argc >= 9) napi_is_array(env, args[8], &envArray);
    if (envArray) {
        uint32_t length = 0;
        napi_get_array_length(env, args[8], &length);
        for (uint32_t index = 0; index < length; index++) {
            napi_value item;
            napi_get_element(env, args[8], index, &item);
            size_t size = 0;
            napi_get_value_string_utf8(env, item, nullptr, 0, &size);
            std::string value(size + 1, '\0');
            napi_get_value_string_utf8(env, item, value.data(), value.size(), &size);
            value.resize(size);
            if (!value.empty()) envOverrides.push_back(value);
        }
    }

    std::string wineLang = "zh_CN";
    if (argc >= 10) {
        const std::string requested = ReadString(env, args[9]);
        if (requested == "zh_CN" || requested == "zh_TW" ||
            requested == "ja_JP" || requested == "en_US")
            wineLang = requested;
    }

    std::string homeDir(homePath);
    if (homeDir.empty()) homeDir = gBrokerHomeDir;
    if (homeDir.empty()) homeDir = "/storage/Users/currentUser/Download";

    const winehua::ContainerSession session = winehua::GetActiveContainerSession();
    const std::string exePath = NativePathToWindows(wineExe, session.prefixDir);

    OH_LOG_INFO(LOG_APP, "[Wine] runWineExe bin=%{public}s exe=%{public}s (final=%{public}s) home=%{public}s",
                binDir, wineExe, exePath.c_str(), homeDir.c_str());

    std::string sockStr(sockPath);
    auto pos = sockStr.find_last_of('/');
    std::string sockDir = (pos == std::string::npos) ? "/tmp" : sockStr.substr(0, pos);
    std::string sockName = (pos == std::string::npos) ? sockStr : sockStr.substr(pos + 1);

    // proton 声明式 env 管线 + VPP 10 参映射 (backend/lang/extraEnv)
    winehua::SessionEnvPolicy policy;
    policy.sockDir = sockDir;
    policy.sockName = sockName;
    policy.libPath = libPath;
    policy.binDir = binDir;
    policy.homeDir = homeDir;
    policy.prefixDir = session.prefixDir;
    policy.desktopShellFlag = WaylandServer::GetInstance()->IsDesktopMode();
    policy.d3dBackend = d3dBackend;
    policy.dxvkBackend = d3dBackend;
    policy.applyStableOverlay = true;
    policy.wineLang = wineLang;
    policy.extraEnv = envOverrides;
    if (workingDirectoryPath[0])
        policy.extraEnv.push_back("WINEHUA_WORKING_DIRECTORY=" + std::string(workingDirectoryPath));
    std::vector<std::string> wineEnv = winehua::BuildSessionEnv(policy);

    winehua::SpawnRequest req{winehua::SpawnKind::WineExe};
    req.binDir = binDir;
    req.argv.push_back(exePath);
    for (const auto& a : launchArguments) req.argv.push_back(a);
    req.env = std::move(wineEnv);

    pid_t pid = winehua::Spawner::Spawn(req);
    if (pid <= 0) {
        OH_LOG_ERROR(LOG_APP, "[Wine] broker spawn failed");
        if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("evt:launch-failed"), napi_tsfn_blocking);
        return MakeLaunchResult(env, -1, "", false);
    }

    AddProcess(pid, wineExe, -1);
    if (gStateTsfn) {
        char msg[64];
        snprintf(msg, sizeof(msg), "evt:launch-accepted:%d", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
    }
    return MakeLaunchResult(env, pid, BeginRegisteredWineSession(pid), false);
}

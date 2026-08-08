#include "wine_launch.h"
#include "wine_exe.h"
#include "wine_process.h"
#include "wine_env.h"
#include "wine_constants.h"
#include "wayland_server.h"
#include "audio_ipc_protocol.h"
#include "graphics_broker.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

#include "broker.h"
#include "wait_utils.h"

#include <AbilityKit/native_child_process.h>

// -- prefix 初始化检测辅助函数 --
static bool FileHasData(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool DirExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsWinePrefixInitialized(const std::string& prefixDir) {
    const std::string prefix = prefixDir.empty() ? WINE_PREFIX : prefixDir;
    return FileHasData((prefix + "/system.reg").c_str()) &&
           FileHasData((prefix + "/user.reg").c_str()) &&
           DirExists((prefix + "/drive_c/windows/system32").c_str()) &&
           DirExists((prefix + "/drive_c/windows/temp").c_str()) &&
           DirExists((prefix + "/drive_c/users").c_str());
}

bool IsWinePrefixInitialized() {
    return IsWinePrefixInitialized(WINE_PREFIX);
}

// IsProcessAliveNotZombie 已移至 wine_process.cpp (ProcMon / 停止编排共用),
// 声明见 wine_process.h。

// -- WoW64 syswow64 预填充辅助 --
static bool EnsureDir(const std::string& path, mode_t mode)
{
    if (DirExists(path.c_str())) return true;
    if (mkdir(path.c_str(), mode) == 0 || errno == EEXIST) return DirExists(path.c_str());
    OH_LOG_ERROR(LOG_APP, "[Launch-Async] mkdir %{public}s failed: %{public}s",
                 path.c_str(), strerror(errno));
    return false;
}

static bool EnsureDirRecursive(const std::string& path, mode_t mode)
{
    if (path.empty() || path == "/") return true;
    if (DirExists(path.c_str())) return true;

    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
    {
        if (!EnsureDirRecursive(path.substr(0, slash), mode)) return false;
    }
    return EnsureDir(path, mode);
}

static bool EnsureExternalPePrefixSkeleton(const std::string& prefixDir)
{
    // The external-PE runtime resolves 64-bit Windows binaries from
    // x86_64-windows instead of copying them into drive_c.  wineboot therefore
    // does not necessarily materialize directories which Windows services and
    // diagnostics still use as working/output directories.
    static const char* const suffixes[] = {
        "/drive_c/windows/system32",
        "/drive_c/windows/system32/drivers",
        "/drive_c/windows/system32/spool",
        "/drive_c/windows/system32/tasks",
        "/drive_c/windows/temp",
    };

    bool ok = true;
    for (const char* suffix : suffixes)
        ok = EnsureDirRecursive(prefixDir + suffix, 0777) && ok;

    OH_LOG_INFO(LOG_APP, "[Launch-Async] external-PE prefix skeleton %{public}s",
                ok ? "ready" : "failed");
    return ok;
}

static bool HasRuntimeFileExtension(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".dll") ||
           !strcasecmp(dot, ".drv") ||
           !strcasecmp(dot, ".sys") ||
           !strcasecmp(dot, ".exe");
}

static bool CopyFileIfNeeded(const std::string& src, const std::string& dst)
{
    struct stat srcSt;
    struct stat dstSt;
    if (stat(src.c_str(), &srcSt) != 0 || !S_ISREG(srcSt.st_mode)) return false;
    if (stat(dst.c_str(), &dstSt) == 0 && S_ISREG(dstSt.st_mode) &&
        dstSt.st_size == srcSt.st_size && dstSt.st_mtime >= srcSt.st_mtime)
        return true;

    int inFd = open(src.c_str(), O_RDONLY);
    if (inFd < 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] open src %{public}s failed: %{public}s",
                     src.c_str(), strerror(errno));
        return false;
    }

    std::string temporaryTemplate = dst + ".winehua.tmp.XXXXXX";
    std::vector<char> temporary(temporaryTemplate.begin(), temporaryTemplate.end());
    temporary.push_back('\0');
    int outFd = mkstemp(temporary.data());
    if (outFd < 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] create temporary for %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
        close(inFd);
        return false;
    }
    fchmod(outFd, 0666);

    char buffer[64 * 1024];
    bool ok = true;
    ssize_t n;
    while ((n = read(inFd, buffer, sizeof(buffer))) > 0)
    {
        char* p = buffer;
        ssize_t remaining = n;
        while (remaining > 0)
        {
            ssize_t w = write(outFd, p, remaining);
            if (w < 0)
            {
                ok = false;
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] write dst %{public}s failed: %{public}s",
                             dst.c_str(), strerror(errno));
                break;
            }
            p += w;
            remaining -= w;
        }
        if (!ok) break;
    }
    if (n < 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] read src %{public}s failed: %{public}s",
                     src.c_str(), strerror(errno));
    }

    if (ok && fsync(outFd) != 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] fsync dst %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
    }
    close(outFd);
    close(inFd);
    if (ok && rename(temporary.data(), dst.c_str()) != 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] replace dst %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
    }
    if (!ok) unlink(temporary.data());
    return ok;
}

static bool EnsureWow64Files(const std::string& binDir, const std::string& prefixDir)
{
    const std::string srcDir = binDir + "/i386-windows";
    const std::string dstDir = prefixDir + "/drive_c/windows/syswow64";

    DIR* src = opendir(srcDir.c_str());
    if (!src)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] wow64 source missing %{public}s: %{public}s",
                     srcDir.c_str(), strerror(errno));
        return false;
    }
    if (!EnsureDirRecursive(dstDir, 0777))
    {
        closedir(src);
        return false;
    }

    int total = 0;
    int copied = 0;
    int failed = 0;
    while (dirent* entry = readdir(src))
    {
        if (entry->d_name[0] == '.' || !HasRuntimeFileExtension(entry->d_name)) continue;
        total++;
        std::string srcPath = srcDir + "/" + entry->d_name;
        std::string dstPath = dstDir + "/" + entry->d_name;
        if (CopyFileIfNeeded(srcPath, dstPath)) copied++;
        else failed++;
    }
    closedir(src);

    OH_LOG_INFO(LOG_APP, "[Launch-Async] wow64 syswow64 total=%{public}d ok=%{public}d failed=%{public}d",
                total, copied, failed);
    return total > 0 && failed == 0;
}
static bool IsWineserverSocketReady(const std::string& prefix) {
    char sockDir[512];
    snprintf(sockDir, sizeof(sockDir), "%s/.wineserver", prefix.c_str());
    DIR* d = opendir(sockDir);
    if (!d) return false;
    bool found = false;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char sockPath[1024];
        snprintf(sockPath, sizeof(sockPath), "%s/%s/socket", sockDir, de->d_name);
        struct stat st;
        if (stat(sockPath, &st) == 0 && S_ISSOCK(st.st_mode)) { found = true; break; }
    }
    closedir(d);
    return found;
}

static std::string FindLaunchEnvironmentValue(const LaunchParams& params,
                                              const char* key)
{
    const std::string prefix = std::string(key) + "=";
    for (auto it = params.envStrs.rbegin(); it != params.envStrs.rend(); ++it) {
        if (it->rfind(prefix, 0) == 0)
            return it->substr(prefix.size());
    }
    return {};
}

static void AppendStableDesktopDxvkEnv(std::vector<std::string>& env,
                                       const LaunchParams& params)
{
    if (params.d3dBackend.rfind("dxvk_", 0) != 0) return;

    /* SetHostShadowProfile carries the selected diagnostic profile through
     * the host-side broker environment before Explorer is launched.  Keep
     * the desktop descendants on that explicit profile instead of replacing
     * it with the product default below. */
    const char* shadowTrace = getenv("VKR_WINEHUA_SHADOW_TRACE");
    const bool guestPerf = shadowTrace && !strcmp(shadowTrace, "perf");
    std::string selectedProfile =
        FindLaunchEnvironmentValue(params, "WINEHUA_PERF_PROFILE");
    if (selectedProfile.empty()) {
        if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-frame-assoc-trace"))
            selectedProfile = "shadow-precise-dirty-ring-frame-assoc-trace";
        else if (shadowTrace && !strcmp(shadowTrace, "present-image-trace"))
            selectedProfile = "shadow-precise-dirty-ring-present-image-trace";
        else if (shadowTrace && !strcmp(shadowTrace, "gpu-frame-profile"))
            selectedProfile = "shadow-precise-dirty-ring-gpu-frame-profile";
        else if (shadowTrace && !strcmp(shadowTrace, "frame-timeline"))
            selectedProfile = "shadow-precise-dirty-ring-frame-timeline";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-descriptor-serialized"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-descriptor-serialized";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-serialized"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-serialized";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-alias-cover"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-alias-cover";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload-coverage-sort"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload-coverage-sort";
        else if (shadowTrace && !strcmp(shadowTrace, "inline-gpu-upload"))
            selectedProfile = "shadow-precise-dirty-ring-inline-upload";
        else if (shadowTrace && !strcmp(shadowTrace, "no-gpu-upload-fast"))
            selectedProfile = "shadow-precise-dirty-ring-no-upload-fast";
        else if (shadowTrace && !strcmp(shadowTrace, "no-gpu-upload"))
            selectedProfile = "shadow-precise-dirty-ring-no-upload";
        else
            selectedProfile = guestPerf ? "shadow-precise-strong-ring-perf"
                                        : "shadow-precise-dirty-ring-inline-upload-coverage-sort";
    }

    /* Explorer-launched programs inherit the desktop process environment and
     * bypass Index.d3dLaunchEnvironment(). Keep the product-correct settings
     * here without changing the explicit A/B profiles used by runWineProgram. */
    /* Product sessions retain warnings and errors without formatting DXVK's
     * informational startup stream. Smoke and explicit diagnostics override
     * this through runWineProgram's per-process environment. */
    UpsertEnvLine(env, "DXVK_LOG_LEVEL=warn");
    UpsertEnvLine(env, "DXVK_LOG_PATH=C:\\windows\\temp");
#ifdef __aarch64__
    UpsertEnvLine(env, "BOX64_DYNAREC_WEAKBARRIER=0");
#endif
    UpsertEnvLine(env, "WINEHUA_PERF_PROFILE=" + selectedProfile);
    UpsertEnvLine(env, "DXVK_WINEHUA_PRECISE_SHADOW=1");
    if (selectedProfile == "shadow-precise-dirty-ring-inline-upload-descriptor-serialized") {
        UpsertEnvLine(env, "VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=1");
    }
    UpsertEnvLine(env, "VN_WINEHUA_STRONG_RING_BARRIER=1");
    if (guestPerf) {
        UpsertEnvLine(env, "VN_WINEHUA_PERF_SUMMARY=1");
        UpsertEnvLine(env, "VN_WINEHUA_PERF_LOG=/storage/Users/currentUser/Download/app.hackeris.winehua/winehua_guest_ring_perf.log");
        /* vn_log uses MESA_LOG_DEBUG.  Raise only the explicit diagnostic
         * profile so the Guest ring summary survives the OHOS logger filter. */
        UpsertEnvLine(env, "MESA_LOG_LEVEL=debug");
    }
}

static void PrepareDesktopSessionGraphicsEnv(const LaunchParams& params)
{
    OH_LOG_INFO(LOG_APP, "[Launch-Async] preparing graphics env for child processes");
    auto& gb = winehua::GraphicsBroker::GetInstance();
    gb.SetWineRuntimeBinaryDir(params.winehuaBin);
    gb.SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    gb.SetVulkanPresentMode(params.d3dBackend.rfind("dxvk_", 0) == 0);
    gb.EnsureStarted(params.sockDir);

    winehua::GraphicsBackendState state = gb.GetState();
    if (state.active != winehua::GraphicsBackend::Virgl) {
        OH_LOG_ERROR(LOG_APP,
                     "[Launch-Async] GL env unavailable: requested=%{public}s active=%{public}s error=%{public}s",
                     winehua::GraphicsBroker::BackendName(state.requested),
                     winehua::GraphicsBroker::BackendName(state.active),
                     state.lastError.c_str());
        return;
    }

    std::vector<std::string> env;
    gb.AppendWineEnv(env);
    AppendD3dBackendEnv(env, params.d3dBackend, params.winehuaBin);
    AppendStableDesktopDxvkEnv(env, params);
    /* The broker now receives the finalized environment through the
     * serialized __env entryParams channel. Keep this helper side-effect
     * free so the old broker-global environment path cannot diverge from
     * Explorer and smoke launches. */
    LogGraphicsBackendStateForLaunch("DesktopSession");
}

// broker SPAWN 请求统一走 wine_exe.h 的 SpawnViaBroker (与手动启动共用),
// 避免在 wine_launch.cpp 复制第二份 broker 协议实现。

static bool LaunchPadMode(LaunchParams* p, int audioBootstrapFd,
                          const std::string& serializedEnv, bool* desktopDegraded) {
    // serializedEnv 之前用于 NCP 直启 explorer 时嵌入 entryParams；
    // explorer 桌面模式已改为走 broker（通过 SpawnViaBroker 传输 env），
    // 此参数不再使用。
    (void)serializedEnv;

    // audioFdNode 之前用于 explorer NCP fdList；改为 broker 后,
    // broker::HandleRequest 自动为每个请求创建 audio bootstrap fd。
    (void)audioBootstrapFd;

    // Prefix registry and user data survive runtime upgrades, while the
    // syswow64 PE files are managed copies. Validate them before wineserver
    // starts so an interrupted prior refresh cannot leave a zero-length DLL.
    if (!EnsureExternalPePrefixSkeleton(p->prefixDir) ||
        !EnsureWow64Files(p->winehuaBin, p->prefixDir)) {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] external-PE prefix preparation failed");
        if (gStateTsfn)
            napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"), napi_tsfn_blocking);
        return false;
    }

    // -- wineserver via NCP --
    // wineserver 走 WineserverMain 入口 (wine_child.cpp), __env= 覆盖会被解析
    {
        std::string wsEntryParams = p->homeDir + "|" + p->winehuaBin + "|wineserver|-f|-p";
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver args=%{public}s", wsEntryParams.c_str());
        NativeChildProcess_Args wsArgs = {};
        wsArgs.entryParams = const_cast<char*>(wsEntryParams.c_str());
        NativeChildProcess_Options wsOpts = {};
        wsOpts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t wsChildPid = -1;
        auto wsRet = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:WineserverMain", wsArgs, wsOpts, &wsChildPid);
        if (wsRet != NCP_NO_ERROR) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver StartNativeChildProcess FAILED ret=%{public}d", (int)wsRet);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineserver"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d (via appspawn)", wsChildPid);
        // 登记主 wineserver 为会话锚点: 入进程注册表 (PC 窗口 / Pad 桌面两条
        // 路径共用此唯一 spawn 点, 一处登记全覆盖) → ProcMon 监视其存活,
        // 非预期死亡上报 state:failed:wineserver; KillAllProcesses 也能杀到它
        RegisterWineserver(wsChildPid);
        if (!WaitFor("wineserver socket", [p]() { return IsWineserverSocketReady(p->prefixDir); }, 5000, 100)) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not detected, "
                        "wineboot will recover via server_connect retry+start_server");
        }
    }

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("state:starting:wineboot"), napi_tsfn_blocking);

    gBrokerHomeDir = p->homeDir;
    StartBrokerServer();
    setenv("PROCESSBROKER", WINE_BROKER_SOCKET, 1);

    // -- wineboot --init --
    const std::string initMarker = p->prefixDir + "/.winehua-init-in-progress";
    bool prefixReady = IsWinePrefixInitialized(p->prefixDir)
        && access(initMarker.c_str(), F_OK) != 0;

    if (!prefixReady) {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix not initialized, preparing WoW64 and running wineboot --init...");
        if (FILE* marker = fopen(initMarker.c_str(), "w")) {
            fputs("wineboot\n", marker);
            fclose(marker);
        } else {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] cannot create prefix init marker: %{public}s",
                         initMarker.c_str());
            /* 失败必须发声: 此前这里静默 return false, LaunchThreadFunc 不再发任何
             * 消息, ArkTS 永久停在 "正在初始化" spinner 且无重试入口。 */
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"),
                                              napi_tsfn_blocking);
            return false;
        }
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(false);
        // wineboot creates shell-owned helper windows while initializing a fresh
        // prefix.  Keep those helpers on the desktop path even when the smoke
        // suite itself uses managed windows; otherwise the first clean-prefix
        // session can leave Wayland/audio/graphics services half initialized.
        const char* desktopTag =
            (ws->IsDesktopMode() || p->automationMode) ? "__winehua_desktop__|" : "";
#ifdef __aarch64__
        std::string entryParams = p->homeDir + "|" + p->winehuaBin + "|" + desktopTag +
            "wineboot|--init|__env=WINEPREFIX=" + p->prefixDir;
#else
        std::string entryParams = p->homeDir + "|" + p->winehuaBin + "|" + desktopTag +
            "wine|wineboot|--init|__env=WINEPREFIX=" + p->prefixDir;
#endif
        // 注意: wineboot --init 只需要初始化 prefix, 不传完整环境变量以节省 entryParams 长度
        NativeChildProcess_Args childArgs = {};
        childArgs.entryParams = const_cast<char*>(entryParams.c_str());
        NativeChildProcess_Options options = {};
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t childPid = -1;
        auto ret = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:Main", childArgs, options, &childPid);
        if (ret != NCP_NO_ERROR) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot FAILED ret=%{public}d", (int)ret);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot started, pid=%{public}d", childPid);
        /* 首次初始化 (wine.inf 的 PreInstall/DefaultInstall/Wow64Install + 可选 Mono)
         * 耗时与设备性能强相关, 模拟器上可超过 30s — 固定死线会把仍在正常初始化的
         * wineboot 误判为失败。改为进程活性驱动: wineboot 活着就继续等,
         * 大超时仅作挂死安全网; 真正的失败由进程退出后 prefix 不完整触发。 */
        char procPath[64];
        snprintf(procPath, sizeof(procPath), "/proc/%d", childPid);
        constexpr int kWinebootHangMs = 3 * 60 * 1000;
        int aliveMs = 0;
        while (IsProcessAliveNotZombie(childPid) && aliveMs < kWinebootHangMs) {
            usleep(500000);
            aliveMs += 500;
            if (aliveMs % 10000 == 0)
                OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot still initializing (%{public}d s)",
                            aliveMs / 1000);
        }
        if (aliveMs >= kWinebootHangMs) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot hung for %{public}d s, abort",
                         kWinebootHangMs / 1000);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"), napi_tsfn_blocking);
            return false;
        }
        /* wineboot 已退出: registry 仍在 wineserver flush 途中 (实测落盘延迟
         * 稳定 ~13s), 宽限窗口等文件就绪 — 文件到位即通过, 不会满等 */
        if (!WaitFor("wine prefix",
                     [&p]() { return IsWinePrefixInitialized(p->prefixDir); },
                     60000, 200)) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot exited but prefix incomplete, abort");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot completed (%{public}d s)", aliveMs / 1000);
        unlink(initMarker.c_str());
        ws->SetDesktopRootRecognitionEnabled(true);
        ws->PromotePendingDesktopRoot();
    } else {
        /* 二启 (prefix 已初始化): 显式播种 wineboot boot 事件。
         * 每个新 wineserver 会话的内核对象全空, 第一个客户端 (explorer) 会
         * 在 ntdll run_wineboot 里触发 wineboot --init——该路径继承 appspawn
         * 环境 (LD_PRELOAD=libappspawn_helper.z.so 等), 实测 wineboot 卡死
         * (注册表已写但 .update-timestamp 不更新), SetEvent 永不执行, 之后
         * 所有 Wine 进程都卡在 boot 事件等待, 窗口全部出不来。这里用与首启
         * 相同的 NCP 干净环境显式跑一次 wineboot: 正常完成后事件 signaled,
         * explorer 的 run_wineboot 检查事件已存在, 立即放行。
         * 参数必须用 --init: wineboot.c 的 wWinMain 传 update_wineprefix(update),
         * 而 update_wineprefix 的参数名就是 force——--update 会让 force=true,
         * 无条件重装 wine.inf 并弹出 "Setting up Wine" 等待窗; --init 传
         * force=false, 仅当 wine.inf 时间戳变化 (升级) 才重装。 */
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix ready; seeding wineboot boot event (--init)...");
        std::string entryParams = p->homeDir + "|" + p->winehuaBin + "|" +
            "wine|wineboot|--init|__env=WINEPREFIX=" + p->prefixDir;
        NativeChildProcess_Args childArgs = {};
        childArgs.entryParams = const_cast<char*>(entryParams.c_str());
        NativeChildProcess_Options options = {};
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t childPid = -1;
        auto ret = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:Main", childArgs, options, &childPid);
        if (ret != NCP_NO_ERROR) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot --init FAILED ret=%{public}d",
                         (int)ret);
            /* 播种失败必须上报, 不能再只记日志放行: 缺少这次 wineboot,
             * explorer 的 run_wineboot 永远等不到 boot 事件, 桌面出不来但
             * 状态机曾照样发 state:ready (静默失败)。注意 NCP 子进程由
             * appspawn 立即 reap, host 拿不到退出码 — 可判定的终点就是
             * "spawn 成功 + wineboot 退出后 wineserver 仍存活"。 */
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"),
                                              napi_tsfn_blocking);
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot --init pid=%{public}d", childPid);
        /* 与首启相同的等待纪律: wineboot 活着就继续等, 大超时仅作挂死
         * 安全网。wineboot 退出即 SetEvent, explorer 即可放行。 */
        constexpr int kWinebootHangMs = 3 * 60 * 1000;
        int aliveMs = 0;
        while (IsProcessAliveNotZombie(childPid) && aliveMs < kWinebootHangMs) {
            usleep(500000);
            aliveMs += 500;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot --init done (%{public}d ms)",
                    aliveMs);
        // wineboot 已退出: 若它因 wineserver 死亡而失败, 后续 explorer/ready 全是空转。
        // 判定用"wineserver socket 就绪"而非 GetWineserverPid() 存活: 热重启时旧
        // wineserver 可能仍存活 (wine 单实例, 新 spawn 的 wineserver 连接旧实例后
        // 正常退出), 此时 GetWineserverPid 指向新 pid 已死但 socket 仍在旧实例手里 —
        // 以 socket 就绪为准 (master 同款判定, 不误报热重启为 failed)。
        if (!IsWineserverSocketReady(p->prefixDir)) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver socket not ready after wineboot seed, abort");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineserver"),
                                              napi_tsfn_blocking);
            return false;
        }
    }

    // -- explorer desktop shell (仅 desktop 模式) --
    PrepareDesktopSessionGraphicsEnv(*p);

    if (p->automationMode)
    {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] automation session ready; Explorer intentionally skipped");
    }
    else if (WaylandServer::GetInstance()->IsDesktopMode())
    // -- explorer (Desktop 或 Pad 模式均启动, 走 broker 统一路径) --
    {
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(true);
        int dw = ws->outputW_ > 0 ? ws->outputW_ : 1280;
        int dh = ws->outputH_ > 0 ? ws->outputH_ : 720;
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop size: outputW=%{public}d outputH=%{public}d → %{public}dx%{public}d",
                    ws->outputW_, ws->outputH_, dw, dh);
        char desktopArg[128];
        /* 附带 winehua_keep.exe: 加入 shell desktop 并持久运行,
         * 避免最后一个用户应用退出后 wineserver 自动关闭桌面.
         * 仅 Pad 桌面模式需要, Phone 模式走单窗口, 无需此逻辑. */
        snprintf(desktopArg, sizeof(desktopArg), "/desktop=shell,%dx%d|winehua_keep.exe", dw, dh);

        // 构造 env: 基线 + 刷新图形状态 + DXVK overlay + 桌面稳定性配置.
        // 这相当于之前 AppendDesktopD3dEntryEnv 的逻辑，收敛到 broker env channel。
        std::vector<std::string> explorerEnv = p->envStrs;
        {
            std::vector<std::string> freshGraphics;
            winehua::GraphicsBroker::GetInstance().AppendWineEnv(freshGraphics);
            for (const auto& line : freshGraphics) UpsertEnvLine(explorerEnv, line);
        }
        AppendD3dBackendEnv(explorerEnv, p->d3dBackend, p->winehuaBin);
        AppendStableDesktopDxvkEnv(explorerEnv, *p);

#ifdef __aarch64__
        std::string exEntry = p->winehuaBin + "|__winehua_desktop__|explorer|" + std::string(desktopArg);
#else
        std::string exEntry = p->winehuaBin + "|__winehua_desktop__|wine|explorer|" + std::string(desktopArg);
#endif
        // broker 自动添加 homeDir 前缀、序列化 env、创建 audio bootstrap fd
        int32_t exPid = SpawnViaBroker(exEntry, explorerEnv);
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop pid=%{public}d (via broker)", exPid);
        if (exPid <= 0) {
            // desktop shell spawn 失败: root 永远不会出现, 白等 15s 也是降级 —
            // 直接失败上报 (静默失败修复: 此前仅记日志, 照样发 state:ready)
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] explorer desktop spawn FAILED");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:desktop"),
                                              napi_tsfn_blocking);
            return false;
        }
        AddProcess(exPid, "desktop", -1);
        ws->PromotePendingDesktopRoot();
        /* broker 返回 pid 只表示 appspawn 接受了 Explorer 请求。
         * 暖 prefix 下子进程仍需数秒连接 wineserver 并提交 desktop
         * surface。等待桌面根 toplevel 就绪后再发 state:ready,
         * 否则自动化游戏可能抢跑而死于 DXGI 初始化。 */
        if (!WaitFor("explorer desktop root", [ws]() {
                return ws->GetDesktopRootToplevelId() != 0;
            }, 15000, 100)) {
            /* 超时不再装死放行: 降级 ready-degraded (UI 显示"桌面准备中…"),
             * root 出现后由 desktop_root 钩子补发 evt:desktop-ready,
             * ArkTS 据此升级为正式 ready (慢设备自救, 不再谎称已就绪)。 */
            OH_LOG_WARN(LOG_APP, "[Launch-Async] explorer desktop root not ready in 15s; "
                        "launch will report ready-degraded");
            *desktopDegraded = true;
        }
    }
    else
    {
        // 非桌面模式: 启动 explorer 文件管理器窗口 — 与 Index.ets 手动启动
        // explorer (runWineProgram → SpawnWineProgram) 走完全相同的 broker
        // 路径 (绝对路径 + per-process env + broker 通道)。早期 NCP 直启
        // 裸名 "wine explorer" 在非桌面模式被 Wine 当作 shell 启动, 不创建
        // 文件管理器窗口。
        ProgramOptions options;
        options.windowsExePath = "C:\\windows\\explorer.exe";
        options.prefixMode = (p->prefixDir == WINE_SMOKE_PREFIX) ? "clean" : "reuse";
        options.d3dBackend = p->d3dBackend;
        // 语言设置: 桌面模式 explorer 由 p->envStrs 携带 LANG, 此路径绕开了它,
        // 必须逐进程注入, 否则 PC 窗口模式的 explorer 永远是基线中文
        options.environment.push_back("LANG=" + p->wineLang + ".UTF-8");
        options.automationMode = false;
        int32_t exPid = SpawnWineProgram(options);
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer window pid=%{public}d (broker path)",
                    exPid);
        // PC 窗口模式 explorer spawn 零判定修复: 失败必须上报而非静默 ready
        if (exPid <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] explorer window spawn FAILED");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:desktop"),
                                              napi_tsfn_blocking);
            return false;
        }
    }
    return true;
}

void LaunchThreadFunc(LaunchParams* p) {
    OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver + wineboot + wine starting in background");
    OH_LOG_INFO(LOG_APP, "[Launch-Async] XKB_CONFIG_ROOT=%{public}s",
                (p->winehuaBin + "/../share/X11/xkb").c_str());

    auto& graphicsBroker = winehua::GraphicsBroker::GetInstance();
    graphicsBroker.SetWineRuntimeBinaryDir(p->winehuaBin);
    graphicsBroker.SetVulkanPresentMode(p->d3dBackend.rfind("dxvk_", 0) == 0);
    graphicsBroker.EnsureStarted(p->sockDir);

    int audioBootstrapFd = CreateAudioBootstrapFd(p->sockDir);
    // Resolve VirGL/backend state before serializing the environment so NCP
    // children inherit the active receiver rather than an early SHM snapshot.
    PrepareDesktopSessionGraphicsEnv(*p);
    p->envStrs = BuildWineEnv(p->sockDir, p->sockName, p->libPath, p->winehuaBin,
                               audioBootstrapFd, p->homeDir, p->prefixDir, p->wineLang);
    AppendD3dBackendEnv(p->envStrs, p->d3dBackend, p->winehuaBin);
    const std::string serializedEnv = SerializeEnvToEntryParams(p->envStrs);

    mkdir(p->prefixDir.c_str(), 0755);

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("state:starting:wineserver"), napi_tsfn_blocking);

    bool ok = false;
    bool desktopDegraded = false;
    ok = LaunchPadMode(p, audioBootstrapFd, serializedEnv, &desktopDegraded);

    /* state:ready 前最后一次健康复查: explorer 桌面根等待可达 15s, 期间
     * wineserver 若已崩溃, 此前照样发 ready → UI 显示"已就绪"但引擎已死
     * (静默失败)。stage 命名 wineserver, 与 ProcMon 的非预期死亡上报一致。
     * 该复查优先于 ready-degraded: wineserver 已死时绝不补发降级把 failed 盖掉。
     * 判定用 socket 就绪而非 GetWineserverPid 存活: 热重启复用旧 wineserver 时,
     * 新 spawn 的实例连接旧实例后正常退出, GetWineserverPid 指向新 pid 会误判 —
     * socket 是否就绪才真正代表"当前有 wineserver 在服务" (master 同款语义)。 */
    if (ok) {
        if (!IsWineserverSocketReady(p->prefixDir)) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver socket not ready at ready checkpoint, refuse ready");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineserver"),
                                              napi_tsfn_blocking);
            ok = false;
        }
    }

    // 状态迁移统一在此收口: ready / ready-degraded 只此一个发射点
    if (ok && gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn,
            strdup(desktopDegraded ? "state:ready-degraded" : "state:ready"),
            napi_tsfn_blocking);

    delete p;
}

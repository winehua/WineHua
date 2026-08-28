#include "wine_launch.h"
#include "wine_exe.h"
#include "wine_process.h"
#include "wine_env.h"
#include "env_profiles.h"
#include "spawner.h"
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

// NCP 直启细节已收口到 spawner.cpp (重构第 4 步), 本文件不再直接触碰
// AbilityKit NCP 接口。

// -- prefix 初始化检测辅助函数 --
static bool FileHasData(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool FileContainsExactRecord(const std::string& path, const char* expected)
{
    FILE* file = fopen(path.c_str(), "r");
    if (!file) return false;
    char record[64] = {};
    const size_t expectedLength = strlen(expected);
    size_t length = fread(record, 1, sizeof(record), file);
    fclose(file);

    // Older test builds wrote the token through the Windows text CRT, which
    // transparently appended CRLF. Keep those valid completed prefixes usable
    // while requiring the exact token itself.
    while (length > expectedLength &&
           (record[length - 1] == '\n' || record[length - 1] == '\r'))
        --length;
    return length == expectedLength && !memcmp(record, expected, expectedLength);
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

static bool CopyFileIfNeeded(const std::string& src, const std::string& dst);

static bool EnsureExternalPePrefixSkeleton(const std::string& binDir,
                                           const std::string& prefixDir)
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

    /* Shell explorer resolves programs supplied after /desktop through the
     * Windows search path, not the external x86_64-windows PE directory.
     * Keep the one desktop-only helper in the native system32 location so a
     * missing helper cannot make wineserver close an otherwise valid shell. */
    const std::string keepSrc = binDir + "/winehua_keep.exe";
    const std::string keepDst = prefixDir + "/drive_c/windows/system32/winehua_keep.exe";
    if (!CopyFileIfNeeded(keepSrc, keepDst)) {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] desktop keep helper missing or stale: %{public}s -> %{public}s",
                     keepSrc.c_str(), keepDst.c_str());
        ok = false;
    }

    OH_LOG_WARN(LOG_APP, "[Launch-Async] external-PE prefix skeleton %{public}s",
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

    OH_LOG_WARN(LOG_APP, "[Launch-Async] wow64 syswow64 total=%{public}d ok=%{public}d failed=%{public}d",
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

// FindLaunchEnvironmentValue / UsesDxvkOverlay / 兼容档位三函数 /
// AppendStableDesktopDxvkEnv 已迁入 env_profiles.cpp (策略集中, 重构第 3 步)

static bool UsesVulkanD3dBackend(const std::string& backend)
{
    return backend.rfind("dxvk_", 0) == 0 ||
           backend == "vkd3d_limited_500k";
}

// 兼容模式全局档位 (FilterCompatLines / AppendCompatEnvLines) 与
// AppendStableDesktopDxvkEnv 已迁入 env_profiles.cpp, 签名从 LaunchParams 解耦
// (重构第 3 步); 第 5 步起档位统一经 SpawnRequest.env 下发

static void PrepareDesktopSessionGraphicsEnv(const LaunchParams& params)
{
    OH_LOG_WARN(LOG_APP, "[Launch-Async] preparing graphics env for child processes");
    auto& gb = winehua::GraphicsBroker::GetInstance();
    gb.SetWineRuntimeBinaryDir(params.winehuaBin);
    gb.SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    gb.SetVulkanPresentMode(UsesVulkanD3dBackend(params.d3dBackend));
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

    /* The broker receives the finalized environment through the serialized
     * __env entryParams channel; env 组装统一走 BuildSessionEnv
     * (env_profiles.cpp), 此处只确保 graphics broker 就绪并记录状态。 */
    LogGraphicsBackendStateForLaunch("DesktopSession");
}

// LaunchParams → SessionEnvPolicy 适配: 管线输入声明 (env_profiles.cpp)
static winehua::SessionEnvPolicy SessionPolicyFromLaunch(const LaunchParams& p, int audioFd)
{
    winehua::SessionEnvPolicy s;
    s.sockDir = p.sockDir;
    s.sockName = p.sockName;
    s.libPath = p.libPath;
    s.binDir = p.winehuaBin;
    s.homeDir = p.homeDir;
    s.prefixDir = p.prefixDir;
    s.wineLang = p.wineLang;
    s.audioBootstrapFd = audioFd;
    s.d3dBackend = p.d3dBackend;
    s.dxvkBackend = p.dxvkBackend;
    s.compatEnvStr = p.compatEnvStr;
    return s;
}

// 进程启动统一走 winehua::Spawner (spawner.cpp, 重构第 4-5 步):
// kind 推导 token 布局, 全部 kind 经 broker 单一通道 spawn, 本文件只声明意图。

static bool LaunchPadMode(LaunchParams* p, int audioBootstrapFd, bool* desktopDegraded) {
    // 会话上下文: binDir 默认
    winehua::Spawner::ConfigureSession(p->homeDir, p->winehuaBin);

    // Prefix registry and user data survive runtime upgrades, while the
    // syswow64 PE files are managed copies. Validate them before wineserver
    // starts so an interrupted prior refresh cannot leave a zero-length DLL.
    if (!EnsureExternalPePrefixSkeleton(p->winehuaBin, p->prefixDir) ||
        !EnsureWow64Files(p->winehuaBin, p->prefixDir)) {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] external-PE prefix preparation failed");
        if (gStateTsfn)
            napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"), napi_tsfn_blocking);
        return false;
    }

    // -- broker 先于 wineserver 启动 (重构第 5 步) --
    // broker 是主进程内的线程, 启动不依赖 wineserver; 自此所有进程
    // (wineserver/wineboot/explorer/exe) 统一经 broker 单一通道 spawn,
    // homeDir 前缀 / WINEPREFIX 权威 / audio fd 由 broker 服务端补齐。
    gBrokerHomeDir = p->homeDir;
    gBrokerPrefixDir = p->prefixDir;
    StartBrokerServer();
    setenv("PROCESSBROKER", WINE_BROKER_SOCKET, 1);

    // -- wineserver via broker --
    // broker → wine_child Main → 截获 argv[0]=="wineserver" 转入本体
    // (wineserver 是纯 Unix ELF, 不能走 wine loader 的 PE 解析)。
    {
        winehua::SpawnRequest wsReq{winehua::SpawnKind::Wineserver};
#ifdef __aarch64__
        winehua::AppendCompatEnvLines(wsReq.env, p->compatEnvStr);
#endif
        const pid_t wsChildPid = winehua::Spawner::Spawn(wsReq);
        if (wsChildPid <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver spawn FAILED");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineserver"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver pid=%{public}d (via broker)", wsChildPid);
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

    // -- wineboot --init --
    const std::string initMarker = p->prefixDir + "/.winehua-init-in-progress";
    bool prefixReady = IsWinePrefixInitialized(p->prefixDir)
        && access(initMarker.c_str(), F_OK) != 0;

    if (!prefixReady) {
        OH_LOG_WARN(LOG_APP, "[Launch-Async] prefix not initialized, preparing WoW64 and running wineboot --init...");
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
        // wineboot derives this prefix-root path from WINECONFIGDIR, the same
        // dynamic Windows path it already uses for .update-timestamp. Do not
        // rely on WINEPREFIX or a custom launcher variable being imported.
        const std::string winebootStatus =
            p->prefixDir + "/.winehua-wineboot-init-status";
        unlink(winebootStatus.c_str());
        if (FILE* request = fopen(winebootStatus.c_str(), "wb")) {
            static constexpr char token[] = "wineboot-init-request";
            const bool written = fwrite(token, 1, sizeof(token) - 1, request) == sizeof(token) - 1;
            const bool closed = fclose(request) == 0;
            if (!written || !closed) {
                OH_LOG_ERROR(LOG_APP,
                             "[Launch-Async] cannot publish wineboot completion request: %{public}s",
                             winebootStatus.c_str());
                unlink(winebootStatus.c_str());
                return false;
            }
        } else {
            OH_LOG_ERROR(LOG_APP,
                         "[Launch-Async] cannot create wineboot completion request: %{public}s",
                         winebootStatus.c_str());
            return false;
        }
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(false);
        // 首启 wineboot 期间抑制窗口创建事件 (PC 窗口模式): 初始化等待窗
        // 不创建独立 OHOS 窗口 — 与 Pad 桌面模式对齐。wineboot 完成后恢复。
        ws->SetToplevelEventSuppressed(true);
        // 注意: wineboot --init 只需要初始化 prefix, 不传完整环境变量以节省 entryParams 长度
        // (argv/兼容档位由 Spawner 按 kind 注入; aarch64 归一为不带 wine 加载器
        // token — Main 的 box64 路径自注 binDir/wine ELF, 与旧 reseed 路径的
        // "wine|wineboot" 布局等价)
        winehua::SpawnRequest wbReq{winehua::SpawnKind::Wineboot};
        wbReq.desktopSurface = ws->IsDesktopMode();
        wbReq.env = {"LANG=" + p->wineLang + ".UTF-8",
                     "LC_ALL=" + p->wineLang + ".UTF-8"};
        // 兼容模式全局档位 (wineboot Main 的 apply overrides 晚于 setup_wine_env)
#ifdef __aarch64__
        winehua::AppendCompatEnvLines(wbReq.env, p->compatEnvStr);
#endif
        const pid_t childPid = winehua::Spawner::Spawn(wbReq);
        if (childPid <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot spawn FAILED");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_WARN(LOG_APP, "[Launch-Async] wineboot started, pid=%{public}d", childPid);
        /* 首次初始化 (wine.inf 的 PreInstall/DefaultInstall/Wow64Install + 可选 Mono)
         * 耗时与设备性能强相关, 模拟器上可超过 30s。先等 wineboot 进程退出
         * (NCP 退出回调确认 — 沙箱 /proc 对 NCP 不可见, 进程存活轮询不可用),
         * 再等 prefix 落盘 (wineboot 退出后 registry flush 延迟 ~13s)。
         * 两步缺一不可: 只等 prefix 就绪会过早 spawn explorer, 与 wineboot 收尾
         * 并发拖慢桌面 root (首启实测 >15s 触发 ready-degraded)。4 分钟封顶
         * 仅作挂死安全网。 */
        constexpr int kWinebootTimeoutMs = 4 * 60 * 1000;
        int aliveMs = 0;
        while (!IsLaunchChildExited(childPid) && aliveMs < kWinebootTimeoutMs) {
            usleep(500000);
            aliveMs += 500;
            if (aliveMs % 10000 == 0)
                OH_LOG_WARN(LOG_APP, "[Launch-Async] wineboot still initializing (%{public}d s)",
                            aliveMs / 1000);
        }
        if (!IsLaunchChildExited(childPid)) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot hung for %{public}d s, abort",
                         kWinebootTimeoutMs / 1000);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"), napi_tsfn_blocking);
            return false;
        }
        /* The NCP wrapper can finish before the broker-launched wineboot.exe.
         * Require both Wine's final success marker and the durable prefix
         * contents; early registry files alone are not a valid init result. */
        if (!WaitFor("wineboot completion",
                     [&p, &winebootStatus]() {
                         return FileContainsExactRecord(winebootStatus, "wineboot-init-ok") &&
                                IsWinePrefixInitialized(p->prefixDir);
                     }, 60000, 200)) {
            OH_LOG_ERROR(LOG_APP,
                         "[Launch-Async] wineboot did not report successful completion; prefix remains unready");
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"), napi_tsfn_blocking);
            return false;
        }
        OH_LOG_WARN(LOG_APP, "[Launch-Async] wineboot completed (%{public}d s)", aliveMs / 1000);
        unlink(winebootStatus.c_str());
        unlink(initMarker.c_str());
        ws->SetDesktopRootRecognitionEnabled(true);
        ws->SetToplevelEventSuppressed(false);
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
        OH_LOG_WARN(LOG_APP, "[Launch-Async] prefix ready; seeding wineboot boot event (--init)...");
        winehua::SpawnRequest wbReq{winehua::SpawnKind::Wineboot};
        wbReq.env = {"LANG=" + p->wineLang + ".UTF-8",
                     "LC_ALL=" + p->wineLang + ".UTF-8"};
#ifdef __aarch64__
        winehua::AppendCompatEnvLines(wbReq.env, p->compatEnvStr);
#endif
        const pid_t childPid = winehua::Spawner::Spawn(wbReq);
        if (childPid <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot --init spawn FAILED");
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
        OH_LOG_WARN(LOG_APP, "[Launch-Async] wineboot --init pid=%{public}d", childPid);
        /* 播种: wineboot 退出即 SetEvent, explorer 即可放行。等待按设备模式
         * 分流 (IsLaunchChildExited): 手机 fork 走 /proc 判活, NCP 走退出回调。
         * 3 分钟大超时仅作挂死安全网; 超时未退出必须判失败上报: 缺少这次
         * wineboot, explorer 的 run_wineboot 永远等不到 boot 事件 (静默失败,
         * 不能放行)。 */
        constexpr int kWinebootTimeoutMs = 3 * 60 * 1000;
        int aliveMs = 0;
        while (!IsLaunchChildExited(childPid) && aliveMs < kWinebootTimeoutMs) {
            usleep(500000);
            aliveMs += 500;
            if (aliveMs % 10000 == 0)
                OH_LOG_WARN(LOG_APP, "[Launch-Async] wineboot --init still running (%{public}d s)",
                            aliveMs / 1000);
        }
        if (!IsLaunchChildExited(childPid)) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot --init hung for %{public}d s, abort",
                         kWinebootTimeoutMs / 1000);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineboot"),
                                              napi_tsfn_blocking);
            return false;
        }
        OH_LOG_WARN(LOG_APP, "[Launch-Async] wineboot --init done (waited for exit)");
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

    if (WaylandServer::GetInstance()->IsDesktopMode())
    // -- explorer (Desktop 或 Pad 模式均启动, 走 broker 统一路径) --
    {
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(true);
        // 新桌面会话开始: 清除上次会话的桌面 shell 标记守卫, 使本次 root 出现时
        // 重新标记基础进程 (desktop + explorer 等) 为不可由用户结束。
        BeginDesktopSession();
        int dw = ws->outputW_ > 0 ? ws->outputW_ : 1280;
        int dh = ws->outputH_ > 0 ? ws->outputH_ : 720;
        OH_LOG_WARN(LOG_APP, "[Launch-Async] explorer desktop size: outputW=%{public}d outputH=%{public}d → %{public}dx%{public}d",
                    ws->outputW_, ws->outputH_, dw, dh);
        /* 附带 winehua_keep.exe: 加入 shell desktop 并持久运行,
         * 避免最后一个用户应用退出后 wineserver 自动关闭桌面.
         * 仅 Pad 桌面模式需要, Phone 模式走单窗口, 无需此逻辑. */
        char desktopSize[64];
        snprintf(desktopSize, sizeof(desktopSize), "/desktop=shell,%dx%d", dw, dh);

        // 会话 env 管线统一在 env_profiles (重构第 3 步): 桌面链 = 完整管线 +
        // 稳定化 overlay。graphics broker 状态在此刻读取, 天然是刷新后的快照
        // (替代旧逻辑: 先建 envStrs 再 Upsert freshGraphics 两段式)。
        // WINE_OHOS_AUDIO_* fd 行会被序列化契约过滤 (fd 跨进程无效), broker
        // 会为子进程自动创建 audio bootstrap fd。
        winehua::SessionEnvPolicy explorerPolicy = SessionPolicyFromLaunch(*p, audioBootstrapFd);
        explorerPolicy.stableDesktopOverlay = true;
        std::vector<std::string> explorerEnv = winehua::BuildSessionEnv(explorerPolicy);

        winehua::SpawnRequest exReq{winehua::SpawnKind::DesktopShell};
        exReq.argv = {desktopSize, "C:\\windows\\system32\\winehua_keep.exe"};
        exReq.env = std::move(explorerEnv);
        // broker 自动添加 homeDir 前缀、WINEPREFIX 权威、创建 audio bootstrap fd
        const pid_t exPid = winehua::Spawner::Spawn(exReq);
        OH_LOG_WARN(LOG_APP, "[Launch-Async] explorer desktop pid=%{public}d (via broker)", exPid);
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
        // 非桌面模式 (PC/2in1): 程序是独立窗口, 无需文件管理器窗口 — 用户
        // 需要时从应用库/文件浏览器手动启动 explorer。拉起引擎只保证
        // wineserver + wine 数据就绪, 不再自动 spawn explorer 窗口。
        OH_LOG_WARN(LOG_APP, "[Launch-Async] managed mode: explorer not auto-spawned");
    }
    return true;
}

void LaunchThreadFunc(LaunchParams* p) {
    OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver + wineboot + wine starting in background");
    OH_LOG_WARN(LOG_APP, "[Launch-Async] XKB_CONFIG_ROOT=%{public}s",
                (p->winehuaBin + "/../share/X11/xkb").c_str());

    auto& graphicsBroker = winehua::GraphicsBroker::GetInstance();
    graphicsBroker.SetWineRuntimeBinaryDir(p->winehuaBin);
    graphicsBroker.SetVulkanPresentMode(UsesVulkanD3dBackend(p->d3dBackend));
    graphicsBroker.EnsureStarted(p->sockDir);

    int audioBootstrapFd = CreateAudioBootstrapFd(p->sockDir);
    // Resolve VirGL/backend state before the explorer session env is built so
    // NCP children inherit the active receiver rather than an early SHM snapshot.
    PrepareDesktopSessionGraphicsEnv(*p);
    // 会话 env 不再预先构建: 唯一消费者是 explorer 桌面链, 它在 LaunchPadMode
    // 内用 BuildSessionEnv (SessionPolicyFromLaunch) 现取现建, 图形状态更新鲜。

    mkdir(p->prefixDir.c_str(), 0755);

    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("state:starting:wineserver"), napi_tsfn_blocking);

    bool ok = false;
    bool desktopDegraded = false;
    ok = LaunchPadMode(p, audioBootstrapFd, &desktopDegraded);

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

#include <napi/native_api.h>
#include "wayland_server.h"
#include "plugin_manager.h"
#include "input_manager.h"
#include "egl_renderer.h"
#include "audio_broker.h"
#include "wine_constants.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <dlfcn.h>

#undef LOG_TAG
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

#include "broker.h"

#include "wait_utils.h"

// -- 全局状态 --
// -- 多进程注册表 (替换旧的 gClientPid/gReaderRunning) --
struct WineProcessEntry {
    pid_t pid;
    std::string exeBasename;       // "notepad.exe"
    std::string exeFullPath;       // drive_c 完整路径
    bool running;
    int stdoutFd;
    std::shared_ptr<std::atomic<bool>> readerActive;
};

static std::mutex gProcMutex;
static std::vector<WineProcessEntry> gProcRegistry;

// -- 注册表辅助函数 --
static WineProcessEntry* AddProcess(pid_t pid, const std::string& exeFullPath, int stdoutFd) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    std::string basename = exeFullPath;
    auto slash = basename.find_last_of('/');
    if (slash != std::string::npos) basename = basename.substr(slash + 1);
    gProcRegistry.push_back({
        .pid = pid,
        .exeBasename = basename,
        .exeFullPath = exeFullPath,
        .running = true,
        .stdoutFd = stdoutFd,
        .readerActive = std::make_shared<std::atomic<bool>>(true)
    });
    OH_LOG_INFO(LOG_APP, "[ProcReg] add pid=%{public}d name=%{public}s total=%{public}zu",
                pid, basename.c_str(), gProcRegistry.size());
    return &gProcRegistry.back();
}

static void RemoveProcess(pid_t pid) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (auto it = gProcRegistry.begin(); it != gProcRegistry.end(); ++it) {
        if (it->pid == pid) {
            OH_LOG_INFO(LOG_APP, "[ProcReg] remove pid=%{public}d name=%{public}s total=%{public}zu→%{public}zu",
                        pid, it->exeBasename.c_str(), gProcRegistry.size(), gProcRegistry.size() - 1);
            it->running = false;
            if (it->stdoutFd >= 0) { close(it->stdoutFd); it->stdoutFd = -1; }
            gProcRegistry.erase(it);
            return;
        }
    }
}

static void KillAllProcesses() {
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (auto& entry : gProcRegistry) {
        if (entry.running) {
            OH_LOG_INFO(LOG_APP, "[ProcReg] killAll pid=%{public}d name=%{public}s",
                        entry.pid, entry.exeBasename.c_str());
            *(entry.readerActive) = false;
            kill(entry.pid, SIGKILL);
            if (entry.stdoutFd >= 0) { close(entry.stdoutFd); entry.stdoutFd = -1; }
        }
    }
}

static napi_threadsafe_function gStateTsfn = nullptr;
static std::string gSockPath;

static int CreateAudioBootstrapFd(const std::string& runtimeDir) {
    if (!winehua::AudioBroker::GetInstance().EnsureStarted(runtimeDir)) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }

    int fd = winehua::AudioBroker::GetInstance().CreateBootstrapHandle();
    if (fd < 0) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to create bootstrap FD for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }

    OH_LOG_INFO(LOG_APP, "[AudioBroker] bootstrap ready runtimeDir=%{public}s", runtimeDir.c_str());
    return fd;
}

// -- fork/exec 后关闭继承的 fd --
static void CloseInheritedFds(std::initializer_list<int> keepFds) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return;
    int dfd = dirfd(d);
    dirent* e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (fd <= 2 || fd == dfd) continue;
        if (std::find(keepFds.begin(), keepFds.end(), fd) != keepFds.end()) continue;
        close(fd);
    }
    closedir(d);
}

// -- 进程退出状态日志 (统一 WIFSIGNALED/WIFEXITED 检测) --
static void LogProcessExit(const char* tag, pid_t pid, int status) {
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        OH_LOG_ERROR(LOG_APP, "[%{public}s] CRASH pid=%{public}d signal=%{public}d(%{public}s) core=%{public}d",
                     tag, pid, sig, strsignal(sig), WCOREDUMP(status) ? 1 : 0);
    } else if (WIFEXITED(status)) {
        OH_LOG_INFO(LOG_APP, "[%{public}s] process %{public}d exited code=%{public}d",
                    tag, pid, WEXITSTATUS(status));
    } else {
        OH_LOG_WARN(LOG_APP, "[%{public}s] process %{public}d terminated status=0x%{public}x",
                    tag, pid, status);
    }
}

static bool ShouldSuppressWineLogLine(const std::string& line) {
    return line.find("OHOS-DBG:") != std::string::npos ||
           line.find("Wine cannot find the FreeType font library.") != std::string::npos ||
           line.find("use TrueType fonts please install a version of FreeType greater than") != std::string::npos ||
           line.find("or equal to 2.0.5.") != std::string::npos ||
           line.find("http://www.freetype.org") != std::string::npos;
}

static void NotifyProcessExited(pid_t pid) {
    if (gStateTsfn) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d:exited", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
    }
}

static void StartDetachedProcessWatcher(pid_t pid, const char* tag) {
    std::thread([pid, tag]() {
        int status = 0;
        pid_t waited = waitpid(pid, &status, 0);
        if (waited < 0) {
            OH_LOG_WARN(LOG_APP, "[%{public}s] waitpid failed for %{public}d: %{public}s",
                        tag, pid, strerror(errno));
        } else {
            LogProcessExit(tag, pid, status);
        }
        RemoveProcess(pid);
        NotifyProcessExited(pid);
    }).detach();
}

// -- stderr pipe reader (后台线程, 逐行日志) --
// done 为 nullptr 时永不停止 (适合 wineserver 等持久进程)
static void StartStderrLogger(int fd, const char* tag,
                              std::shared_ptr<std::atomic<bool>> done = nullptr) {
    std::thread([fd, tag, done]() {
        char buf[4096];
        std::string pending;
        while (true) {
            if (done && *done) break;
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = 0;
                pending.append(buf, n);
                size_t pos;
                while ((pos = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, pos);
                    if (!line.empty() && !ShouldSuppressWineLogLine(line))
                        OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}s", tag, line.c_str());
                    pending.erase(0, pos + 1);
                }
            } else {
                if (n == 0 || (n < 0 && errno != EINTR)) break;
            }
        }
        if (!pending.empty() && !ShouldSuppressWineLogLine(pending))
            OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}s", tag, pending.c_str());
        close(fd);
    }).detach();
}

static const char* GetWineDebugValue() {
    const char* overrideValue = std::getenv("WINEHUA_WINEDEBUG");
    if (overrideValue && overrideValue[0]) return overrideValue;
    return "-all";
}

// -- 构建 Wine 环境变量 (wineserver/wineboot/wine 共用) --
static std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                              const std::string& sockName,
                                              const std::string& libPath,
                                              const std::string& binDir,
                                              int audioBootstrapFd) {
    std::string shareDir = binDir + "/../share";
    std::string xkbDir = shareDir + "/X11/xkb";
    std::string runtimeLibPath = binDir + ":" + binDir + "/x86_64-unix:" + binDir + "/../lib/x86_64";
    std::vector<std::string> env = {
        "XDG_RUNTIME_DIR=" + sockDir,
        "WAYLAND_DISPLAY=" + sockName,
        "HOME=/storage/Users/currentUser/Download/app.hackeris.winehua",
        "WINEPREFIX=" WINE_PREFIX,
        "WINEDATADIR=" + shareDir + "/wine", // Wine 数据文件 (nls, wine.inf)
        "WINEBINDIR=" + binDir,
        "WINEUNIXDIR=" + binDir,
        "WINEDLLDIR=" + binDir + "/x86_64-unix", // Wine Unix .so
        "WINEDLLDIR0=" + binDir + "/x86_64-windows", // PE DLL 目录
        "WINEDLLDIR1=" + binDir,                      // PE EXE 目录
        "WINEDLLPATH=" + binDir + "/x86_64-windows:" + binDir, // PE DLL + EXE
        "WINEDLLOVERRIDES=mscoree,mshtml=",
        // Box64 日志级别: 0=无 (3=DEBUG 会产生 300K+ 行 stderr, 拖慢初始化)
        "BOX64_LOG=0",
        "BOX64_NOBANNER=1",
        "BOX64_SHOWSEGV=1",
        // Box64 DYNAREC 性能调优 (对标 Winlator Performance 预设)
        "BOX64_DYNAREC_SAFEFLAGS=0",
        "BOX64_DYNAREC_BIGBLOCK=3",
        "BOX64_DYNAREC_CALLRET=2",
        "BOX64_DYNAREC_FORWARD=1024",
        "BOX64_DYNAREC_WEAKBARRIER=2",
        "BOX64_AVX=0",
        // Wine 调试通道: -all=关闭全部 (+wineboot,+module 等会产生大量 trace)
        "WINEDEBUG=" + std::string(GetWineDebugValue()),
        "WINE_MONO=never",  // 跳过 Mono 安装 (OHOS 无网络, 会卡住)
        "XKB_CONFIG_ROOT=" + xkbDir,
        "PATH=/usr/local/bin:/data/app/bin:/data/service/hnp/bin:/usr/bin:/vendor/bin:" + binDir + "/x86_64-windows:" + binDir,
        "TMPDIR=" WINE_TMPDIR,
    };
    if (audioBootstrapFd >= 0) {
        env.push_back("WINE_OHOS_AUDIO_ENABLE=1");
        env.push_back("WINE_OHOS_AUDIO_BOOTSTRAP_FD=" + std::to_string(audioBootstrapFd));
        env.push_back("WINE_OHOS_AUDIO_PROTOCOL_VERSION=" + std::to_string(WINEHUA_AUDIO_PROTOCOL_VERSION));
    }
#ifdef __aarch64__
    // ARM64: x86_64 .so 由 Box64 加载，不在系统 LD_LIBRARY_PATH
    // LD_LIBRARY_PATH 只包含 ARM64 原生 .so
    env.push_back("LD_LIBRARY_PATH=" + libPath);
    env.push_back("BOX64_LD_LIBRARY_PATH=" + runtimeLibPath);
#else
    // x86_64: 系统 linker 直接加载 x86_64 OHOS .so
    env.push_back("LD_LIBRARY_PATH=" + runtimeLibPath);
#endif
    return env;
}

// -- 客户端 stdout/stderr 读取线程 (每个进程独立) --
static bool CopyFileContents(const std::string& src, const std::string& dst, mode_t mode = 0644) {
    int inFd = open(src.c_str(), O_RDONLY);
    if (inFd < 0) {
        OH_LOG_WARN(LOG_APP, "[Files] open src failed %{public}s: %{public}s", src.c_str(), strerror(errno));
        return false;
    }

    int outFd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (outFd < 0) {
        OH_LOG_WARN(LOG_APP, "[Files] open dst failed %{public}s: %{public}s", dst.c_str(), strerror(errno));
        close(inFd);
        return false;
    }

    char buf[8192];
    bool ok = true;
    while (true) {
        ssize_t n = read(inFd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            OH_LOG_WARN(LOG_APP, "[Files] read failed %{public}s: %{public}s", src.c_str(), strerror(errno));
            break;
        }

        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(outFd, buf + written, n - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                ok = false;
                OH_LOG_WARN(LOG_APP, "[Files] write failed %{public}s: %{public}s", dst.c_str(), strerror(errno));
                break;
            }
            written += w;
        }
        if (!ok) break;
    }

    close(outFd);
    close(inFd);
    if (ok) chmod(dst.c_str(), mode);
    return ok;
}

static bool EnsureDirRecursive(const std::string& path, mode_t mode = 0755) {
    std::string current;
    size_t pos = 0;

    if (path.empty()) return false;

    if (path[0] == '/') {
        current = "/";
        pos = 1;
    }

    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        std::string part = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);

        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') current.push_back('/');
            current.append(part);
            if (mkdir(current.c_str(), mode) != 0 && errno != EEXIST) {
                OH_LOG_WARN(LOG_APP, "[Files] mkdir failed %{public}s: %{public}s", current.c_str(), strerror(errno));
                return false;
            }
        }

        if (next == std::string::npos) break;
        pos = next + 1;
    }

    return true;
}

static bool InstallBundledFile(const std::string& src,
                               const std::string& dst,
                               mode_t mode,
                               const char* label) {
    size_t slash;

    if (access(src.c_str(), R_OK) != 0) {
        OH_LOG_WARN(LOG_APP, "[Files] bundled %{public}s missing: %{public}s", label, src.c_str());
        return false;
    }

    slash = dst.find_last_of('/');
    if (slash != std::string::npos && !EnsureDirRecursive(dst.substr(0, slash))) return false;

    return CopyFileContents(src, dst, mode);
}

static void CleanupLegacyDriveZSampleMirror() {
    const std::string driveZRoot = "/data/storage/el2/base/files/.wine/dosdevices/z:";
    const std::string legacySample = driveZRoot + "/Documents/Media/Alarm01.wav";
    const std::string mediaDir = driveZRoot + "/Documents/Media";
    const std::string docsDir = driveZRoot + "/Documents";

    if (unlink(legacySample.c_str()) == 0) {
        OH_LOG_INFO(LOG_APP, "[Files] removed legacy Z: sample mirror %{public}s", legacySample.c_str());
    }

    if (rmdir(mediaDir.c_str()) == 0) {
        OH_LOG_INFO(LOG_APP, "[Files] removed empty legacy dir %{public}s", mediaDir.c_str());
    }
    if (rmdir(docsDir.c_str()) == 0) {
        OH_LOG_INFO(LOG_APP, "[Files] removed empty legacy dir %{public}s", docsDir.c_str());
    }
    if (rmdir(driveZRoot.c_str()) == 0) {
        OH_LOG_INFO(LOG_APP, "[Files] restored Z: fallback mapping by removing %{public}s", driveZRoot.c_str());
    }
}

static void InstallBundledWineSamples(const std::string& binDir) {
    const std::string sampleExeSrc = binDir + "/x86_64-windows/winehua_audio_smoke.exe";
    const std::string sampleExeDst = "/data/storage/el2/base/files/.wine/drive_c/winehua_audio_smoke.exe";
    const std::string alarmSrc = binDir + "/x86_64-windows/Alarm01.wav";
    const std::string alarmDriveCDst = "/data/storage/el2/base/files/.wine/drive_c/Documents/Media/Alarm01.wav";
    const std::string alarmLocalDst = "/data/storage/el2/base/files/.wine/drive_c/Alarm01.wav";

    CleanupLegacyDriveZSampleMirror();
    InstallBundledFile(sampleExeSrc, sampleExeDst, 0755, "sample exe");
    InstallBundledFile(alarmSrc, alarmDriveCDst, 0644, "Alarm01.wav");
    InstallBundledFile(alarmSrc, alarmLocalDst, 0644, "Alarm01.wav");
}

static void ReaderThread(int fd, pid_t pid, std::shared_ptr<std::atomic<bool>> active) {
    char buf[2048];
    std::string pending;
    while (*active) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            pending.append(buf, n);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                if (!ShouldSuppressWineLogLine(line))
                    OH_LOG_INFO(LOG_APP, "[wine:%{public}d] %{public}s", pid, line.c_str());
                pending.erase(0, pos + 1);
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }
    if (!pending.empty() && !ShouldSuppressWineLogLine(pending)) {
        OH_LOG_INFO(LOG_APP, "[wine:%{public}d] %{public}s", pid, pending.c_str());
    }
    close(fd);

    // 回收子进程
    int status = 0;
    pid_t waited = waitpid(pid, &status, 0);
    if (waited < 0) {
        OH_LOG_WARN(LOG_APP, "[wine] waitpid failed for %{public}d: %{public}s", pid, strerror(errno));
    } else {
        LogProcessExit("wine", pid, status);
    }

    // 从注册表移除
    RemoveProcess(pid);

    // 通知 ArkTS
    NotifyProcessExited(pid);
}

// -- State 回调 -> ArkTS --
static void CallJsState(napi_env env, napi_value cb, void*, void* data) {
    char* msg = static_cast<char*>(data);
    if (env && cb && msg) {
        napi_value undef, arg;
        napi_get_undefined(env, &undef);
        napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &arg);
        napi_call_function(env, undef, cb, 1, &arg, nullptr);
    }
    free(msg);
}

// -- NAPI: setStateCallback --
static napi_value SetStateCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gStateTsfn) {
        napi_release_threadsafe_function(gStateTsfn, napi_tsfn_release);
        gStateTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WLState", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsState, &gStateTsfn);

    WaylandServer::GetInstance()->SetStateCallback([](const char* s) {
        if (gStateTsfn) {
            napi_call_threadsafe_function(gStateTsfn, strdup(s), napi_tsfn_blocking);
        }
    });
    return nullptr;
}

// -- NAPI: startServer --
static napi_value StartServer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    char path[512] = {};
    napi_get_value_string_utf8(env, args[0], path, sizeof(path), nullptr);

    OH_LOG_INFO(LOG_APP, "[NAPI] startServer: %{public}s", path);
    // 确保 socket 父目录存在 (WINEPREFIX=.wine/)
    {
        std::string sockDir = path;
        auto pos = sockDir.find_last_of('/');
        if (pos != std::string::npos) {
            sockDir = sockDir.substr(0, pos);
            mkdir(sockDir.c_str(), 0755);
        }
    }
    gSockPath = path;
    bool ok = WaylandServer::GetInstance()->Start(path);
    OH_LOG_INFO(LOG_APP, "[NAPI] startServer result: %{public}s", ok ? "OK" : "FAIL");
    // 确认 socket 文件存在
    if (ok) {
        struct stat st;
        int sr = stat(path, &st);
        OH_LOG_INFO(LOG_APP, "[NAPI] wayland socket stat=%{public}d (errno=%{public}d)",
                    sr, sr == 0 ? 0 : errno);
    }

    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// -- NAPI: launchClient (异步: wineserver->wineboot->wine 在后台线程, 不阻塞 JS 主线程) --
#ifdef PAD_MODE
// Pad: 从 envp 重建 environ (fork 后 __wine_main / main 从 environ 读环境变量)
static void rebuild_environ(char* const* envp) {
    // 直接替换 environ 指针 (musl 不支持 clearenv)
    extern char** environ;
    environ = (char**)envp;
}

#endif // PAD_MODE
struct LaunchParams {
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string sockDir;
    std::string sockName;
    std::string winehuaBin;
    std::vector<std::string> envStrs;  // 持有 envp 字符串
    std::vector<char*> envp;           // 指向 envStrs
};

#ifdef PAD_MODE
#include <AbilityKit/native_child_process.h>
#endif

// 轮询 wineserver socket 是否就绪 (信号驱动, 避免盲等)
static bool IsWineserverSocketReady() {
    const char* prefix = WINE_PREFIX;
    char sockDir[512];
    snprintf(sockDir, sizeof(sockDir), "%s/.wineserver", prefix);
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

static bool IsWinePrefixReadyInternal() {
    DIR* d = opendir(WINE_PREFIX "/drive_c");
    if (!d) return false;

    bool ready = false;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        ready = true;
        break;
    }

    closedir(d);
    return ready;
}

static void LaunchThreadFunc(LaunchParams* p) {
    OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver + wineboot starting in background");
    OH_LOG_INFO(LOG_APP, "[Launch-Async] XKB_CONFIG_ROOT=%{public}s",
                (p->winehuaBin + "/../share/X11/xkb").c_str());

    // 组装 envp 环境变量
    int audioBootstrapFd = -1;
#ifndef PAD_MODE
    audioBootstrapFd = CreateAudioBootstrapFd(p->sockDir);
#endif
    p->envStrs = BuildWineEnv(p->sockDir, p->sockName, p->libPath, p->winehuaBin, audioBootstrapFd);
    for (auto& s : p->envStrs) p->envp.push_back((char*)s.c_str());
    p->envp.push_back(nullptr);

    mkdir(WINE_PREFIX, 0755);
    mkdir("/storage/Users/currentUser/Download/app.hackeris.winehua", 0755);

    // 通知 ArkTS: wineserver 启动中
    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-starting"), napi_tsfn_blocking);
    }

    // -- 启动 wineserver --
#ifdef PAD_MODE
    {
        // -f: 前台模式, -p: 持久模式 (无客户端也不退出，避免竞态)
        std::string wsEntryParams = p->winehuaBin + "|wineserver|-f|-p";
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver args=%{public}s", wsEntryParams.c_str());
        NativeChildProcess_Args wsArgs = {};
        wsArgs.entryParams = const_cast<char*>(wsEntryParams.c_str());
        NativeChildProcess_Options wsOpts = {};
        wsOpts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
        int32_t wsChildPid = -1;
        auto wsRet = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:WineserverMain", wsArgs, wsOpts, &wsChildPid);
        if (wsRet == NCP_NO_ERROR) {
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d (via appspawn)", wsChildPid);
        } else {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver StartNativeChildProcess FAILED ret=%{public}d", (int)wsRet);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-failed"), napi_tsfn_blocking);
            delete p;
            return;
        }
        // 等待 wineserver socket 就绪 (信号驱动)
        if (!WaitFor("wineserver socket", IsWineserverSocketReady, 5000, 100)) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not detected, "
                        "wineboot will recover via server_connect retry+start_server");
        }
    }
#else
    // PC/x86_64: fork + execve native wineserver directly (no Box64 emulation)
    {
        std::vector<std::string> wsEnvStrs = p->envStrs;
        std::vector<char*> wsEnvp;
        for (auto& s : wsEnvStrs) wsEnvp.push_back((char*)s.c_str());
        wsEnvp.push_back(nullptr);

        int wsPipe[2];
        bool wsPipeOk = (pipe(wsPipe) == 0);
        if (!wsPipeOk)
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver pipe failed: %{public}s", strerror(errno));

        pid_t wsPid = fork();
        if (wsPid == 0) {
            if (wsPipeOk) {
                close(wsPipe[0]);
                dup2(wsPipe[1], STDERR_FILENO);
                if (wsPipe[1] > 2) close(wsPipe[1]);
            }
            CloseInheritedFds({STDOUT_FILENO, STDERR_FILENO, audioBootstrapFd});
            for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
            prctl(PR_SET_NAME, "wl-wineserver", 0, 0, 0);
            chdir(p->winehuaBin.c_str());
            const char* wsArgv[] = {"./wineserver", nullptr};
            execve("./wineserver", (char* const*)wsArgv, wsEnvp.data());
            _exit(127);
        }
        if (wsPid > 0) {
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d", wsPid);
            if (wsPipeOk) {
                close(wsPipe[1]);
                StartStderrLogger(wsPipe[0], "wineserver-stderr");
            }
            // 等待 wineserver socket 就绪 (信号驱动)
            if (!WaitFor("wineserver socket (PC)", IsWineserverSocketReady, 5000, 100)) {
                OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not detected, "
                            "wineboot will recover via server_connect retry");
            }
        } else {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver fork failed");
            if (wsPipeOk) { close(wsPipe[0]); close(wsPipe[1]); }
            if (audioBootstrapFd >= 0) close(audioBootstrapFd);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-failed"), napi_tsfn_blocking);
            delete p;
            return;
        }
    }
#endif

    // 启动 Process Broker（在主进程上下文监听请求，
    // 让 wineboot 内部的 spawn_process 可以通过 Unix socket 请求子进程创建）
    StartBrokerServer();

    // 设置 PROCESSBROKER 环境变量，所有 Wine 进程统一从 env 读取 broker socket 路径
    setenv("PROCESSBROKER", WINE_BROKER_SOCKET, 1);

    bool prefixReady = IsWinePrefixReadyInternal();
    if (prefixReady) {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wine prefix already initialized, skipping wineboot");
        if (audioBootstrapFd >= 0) {
            close(audioBootstrapFd);
            audioBootstrapFd = -1;
        }
    } else if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-starting"), napi_tsfn_blocking);
    }

#ifdef PAD_MODE
    // -- wineboot --init via OH_Ability_StartNativeChildProcess --
    if (!prefixReady) {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] running wineboot --init via appspawn...");

#ifdef __aarch64__
        // ARM64 Box64: argv[0]=winePath (Box64 提供), argv[1]=wineboot
        std::string entryParams = p->winehuaBin + "|wineboot|--init";
#else
        // x86_64: argv[0]=wine, argv[1]=wineboot
        std::string entryParams = p->winehuaBin + "|wine|wineboot|--init";
#endif

        NativeChildProcess_Args childArgs = {};
        childArgs.entryParams = const_cast<char*>(entryParams.c_str());
        childArgs.fdList.head = nullptr;

        NativeChildProcess_Options options = {};
        options.isolationMode = NCP_ISOLATION_MODE_NORMAL;

        int32_t childPid = -1;
        auto ret = OH_Ability_StartNativeChildProcess(
            "libwine_child.so:Main", childArgs, options, &childPid);

        if (ret == NCP_NO_ERROR) {
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot done, pid=%{public}d", childPid);

            // 启动 explorer desktop shell，尺寸匹配输出
            // 等待 wineboot 创建 drive_c (explorer 需要枚举桌面文件)
            if (!WaitFor("wine prefix drive_c", []() {
                DIR* d = opendir(WINE_PREFIX "/drive_c");
                if (d) { closedir(d); return true; }
                return false;
            }, 10000, 200)) {
                OH_LOG_WARN(LOG_APP, "[Launch-Async] drive_c not ready, launching explorer anyway");
            }
            auto* ws = WaylandServer::GetInstance();
            int dw = ws->outputW_ > 0 ? ws->outputW_ : 1280;
            int dh = ws->outputH_ > 0 ? ws->outputH_ : 720;
            char desktopArg[128];
            snprintf(desktopArg, sizeof(desktopArg), "/desktop=shell,%dx%d", dw, dh);
#ifdef __aarch64__
            std::string exEntry = p->winehuaBin + "|explorer|" + desktopArg;
#else
            std::string exEntry = p->winehuaBin + "|wine|explorer|" + desktopArg;
#endif
            NativeChildProcess_Args exArgs = {};
            exArgs.entryParams = const_cast<char*>(exEntry.c_str());
            NativeChildProcess_Options exOpts = {};
            exOpts.isolationMode = NCP_ISOLATION_MODE_NORMAL;
            int32_t exPid = -1;
            auto exRet = OH_Ability_StartNativeChildProcess(
                "libwine_child.so:Main", exArgs, exOpts, &exPid);
            OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop pid=%{public}d ret=%{public}d",
                        exPid, (int)exRet);
        } else
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot FAILED ret=%{public}d", (int)ret);
    }
#else
    // -- wineboot --init (PC/x86_64: fork + execve native wine) --
    if (!prefixReady) {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] running wineboot --init...");
        pid_t bootPid = fork();
        if (bootPid == 0) {
            CloseInheritedFds({STDOUT_FILENO, STDERR_FILENO, audioBootstrapFd});
            for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
            prctl(PR_SET_NAME, "wl-wineboot", 0, 0, 0);
            chdir(p->winehuaBin.c_str());
            const char* bootArgv[] = {"./wine", "wineboot", "--init", nullptr};
            execve("./wine", (char* const*)bootArgv, p->envp.data());
            _exit(127);
        }
        if (audioBootstrapFd >= 0) {
            close(audioBootstrapFd);
            audioBootstrapFd = -1;
        }
        if (bootPid > 0) {
            int bootStatus = 0;
            waitpid(bootPid, &bootStatus, 0);
            LogProcessExit("wineboot", bootPid, bootStatus);
        }

        if (!WaitFor("wine prefix drive_c", []() {
            DIR* d = opendir(WINE_PREFIX "/drive_c");
            if (d) { closedir(d); return true; }
            return false;
        }, 10000, 200)) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] drive_c not ready after wineboot");
        }
    }
#endif

    InstallBundledWineSamples(p->winehuaBin);

    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wine-ready"), napi_tsfn_blocking);
    }

    delete p;
}

static napi_value LaunchClient(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    auto* p = new LaunchParams();

    char buf[1024] = {};
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), nullptr);
    p->exePath = buf;
    napi_get_value_string_utf8(env, args[2], buf, sizeof(buf), nullptr);
    p->sockPath = buf;
    napi_get_value_string_utf8(env, args[3], buf, sizeof(buf), nullptr);
    p->libPath = buf;

    OH_LOG_INFO(LOG_APP, "[Launch] exe=%{public}s sock=%{public}s lib=%{public}s (async)",
                p->exePath.c_str(), p->sockPath.c_str(), p->libPath.c_str());

    // 保证可执行
    if (access(p->exePath.c_str(), X_OK) != 0) chmod(p->exePath.c_str(), 0755);

    // 提取 sockDir, sockName, winehuaBin
    auto pos = p->sockPath.find_last_of('/');
    p->sockDir = (pos == std::string::npos) ? "/tmp" : p->sockPath.substr(0, pos);
    p->sockName = (pos == std::string::npos) ? p->sockPath : p->sockPath.substr(pos + 1);
    pos = p->exePath.find_last_of('/');
    p->winehuaBin = (pos != std::string::npos) ? p->exePath.substr(0, pos) : p->exePath;

    // 启动后台线程: wineserver -> wineboot --init
    std::thread(LaunchThreadFunc, p).detach();

    OH_LOG_INFO(LOG_APP, "[Launch] background thread started, returning to JS");

    napi_value r;
    napi_create_int32(env, 0, &r);
    return r;
}

// -- NAPI: runWineExe -- 启动 wine <exe> (需先完成初始化) --
static napi_value RunWineExe(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) return nullptr;

    char binDir[512] = {}, sockPath[512] = {}, libPath[2048] = {}, wineExe[1024] = {};
    napi_get_value_string_utf8(env, args[0], binDir, sizeof(binDir), nullptr);
    napi_get_value_string_utf8(env, args[1], sockPath, sizeof(sockPath), nullptr);
    napi_get_value_string_utf8(env, args[2], libPath, sizeof(libPath), nullptr);
    napi_get_value_string_utf8(env, args[3], wineExe, sizeof(wineExe), nullptr);

    std::string exePath(wineExe);
#ifdef PAD_MODE
    // Broker 路径: wine_child.cpp 的 __wine_main 需要 NT 路径来定位 exe
    // 把 Unix 沙箱路径转成 Z:\ 格式让 Wine 搜索
    {
        std::string lower = exePath;
        for (auto& c : lower) c = tolower(c);
        if (lower.find("/drive_c/") != std::string::npos) {
            // /data/storage/el2/base/files/.wine/drive_c/windows/notepad.exe
            // → notepad.exe (Wine 自动搜索 C:\windows\system32)
            auto slash = exePath.find_last_of('/');
            if (slash != std::string::npos) {
                exePath = exePath.substr(slash + 1);  // 只用 basename
            }
        }
    }
#endif

    OH_LOG_INFO(LOG_APP, "[Wine] runWineExe bin=%{public}s exe=%{public}s (final=%{public}s)", binDir, wineExe, exePath.c_str());

    std::string sockStr(sockPath);
    auto pos = sockStr.find_last_of('/');
    std::string sockDir = (pos == std::string::npos) ? "/tmp" : sockStr.substr(0, pos);
    std::string sockName = (pos == std::string::npos) ? sockStr : sockStr.substr(pos + 1);
    int audioBootstrapFd = -1;
#ifndef PAD_MODE
    audioBootstrapFd = CreateAudioBootstrapFd(sockDir);
#endif

    std::vector<std::string> envStrs = BuildWineEnv(sockDir, sockName, libPath, binDir, audioBootstrapFd);
    std::vector<char*> envp;
    for (auto& s : envStrs) envp.push_back((char*)s.c_str());
    envp.push_back(nullptr);

#ifdef PAD_MODE
    // Pad: 通过 Process Broker + StartNativeChildProcess 创建子进程
    {
#ifdef __aarch64__
        std::string entryParams = std::string(binDir) + "|" + exePath;
#else
        std::string entryParams = std::string(binDir) + "|wine|" + exePath;
#endif
        OH_LOG_INFO(LOG_APP, "[Wine] runWineExe via broker: %{public}s", entryParams.c_str());

        int broker_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (broker_fd < 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker socket failed: %{public}s", strerror(errno));
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return nullptr;
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, getenv("PROCESSBROKER"));
        if (connect(broker_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker connect failed: %{public}s", strerror(errno));
            close(broker_fd);
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return nullptr;
        }

        // 发送请求: "SPAWN\n{entryParams}\n" (不传 fd，子进程自己连 wineserver)
        char req_hdr[32];
        int hdr_len = snprintf(req_hdr, sizeof(req_hdr), "SPAWN\n");
        size_t ep_len = entryParams.size();
        struct iovec iov[3] = {
            {req_hdr, (size_t)hdr_len},
            {(void*)entryParams.c_str(), ep_len},
            {(void*)"\n", 1}
        };
        struct msghdr msg = {};
        msg.msg_iov = iov;
        msg.msg_iovlen = 3;

        if (sendmsg(broker_fd, &msg, MSG_NOSIGNAL) < 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker sendmsg failed: %{public}s", strerror(errno));
            close(broker_fd);
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return nullptr;
        }

        // 接收响应: childPid + status
        int32_t response[2] = {-1, -1};
        ssize_t n = recv(broker_fd, response, sizeof(response), MSG_WAITALL);
        close(broker_fd);
        if (n != sizeof(response) || response[1] != 0 || response[0] <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker spawn failed pid=%d status=%d", response[0], response[1]);
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return nullptr;
        }

        pid_t pid = response[0];
        AddProcess(pid, wineExe, -1);
        StartDetachedProcessWatcher(pid, "broker-child");
        OH_LOG_INFO(LOG_APP, "[Wine] wine pid=%{public}d exe=%{public}s (via broker)", pid, wineExe);
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%d:wine-running", pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
    }
#else
    // PC/x86_64: fork + execve native wine directly (no Box64 emulation)
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Wine] pipe failed: %{public}s", strerror(errno));
        return nullptr;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] > 2) close(pipefd[1]);
        for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
        prctl(PR_SET_NAME, "wl-client", 0, 0, 0);
        CloseInheritedFds({STDOUT_FILENO, STDERR_FILENO, audioBootstrapFd});
        chdir(binDir);
        const char* cargv[] = {"./wine", exePath.c_str(), nullptr};
        execve("./wine", (char* const*)cargv, envp.data());
        _exit(127);
    }

    if (audioBootstrapFd >= 0) close(audioBootstrapFd);

    if (pid > 0) {
        close(pipefd[1]);
        auto* entry = AddProcess(pid, wineExe, pipefd[0]);
        std::thread(ReaderThread, pipefd[0], pid, entry->readerActive).detach();
        OH_LOG_INFO(LOG_APP, "[Wine] wine pid=%{public}d exe=%{public}s reader started", pid, wineExe);
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%d:wine-running", pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
    } else {
        close(pipefd[0]);
        close(pipefd[1]);
        OH_LOG_ERROR(LOG_APP, "[Wine] wine fork failed");
        if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
    }
#endif
    return nullptr;
}

// -- NAPI: checkWinePrefix -- 检测 .wine/drive_c 是否已初始化且有内容 --
static napi_value CheckWinePrefix(napi_env env, napi_callback_info info) {
    bool ok = IsWinePrefixReadyInternal();
    OH_LOG_INFO(LOG_APP, "[Wine] checkWinePrefix: drive_c ready=%{public}s", ok ? "yes" : "no");
    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// -- NAPI: resetWinePrefix -- 一键清空 files/.wine 目录
static void RmDir(const char* path) {
    DIR* d = opendir(path);
    if (!d) return;
    dirent* e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        std::string full = std::string(path) + "/" + e->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) RmDir(full.c_str());
            else unlink(full.c_str());
        }
    }
    closedir(d);
    rmdir(path);
}

static napi_value ResetWinePrefix(napi_env env, napi_callback_info info) {
    OH_LOG_INFO(LOG_APP, "[NAPI] resetWinePrefix called");
    KillAllProcesses();
    const char* prefix = WINE_PREFIX;
    RmDir(prefix);
    mkdir(prefix, 0755);
    OH_LOG_INFO(LOG_APP, "[NAPI] resetWinePrefix: %{public}s cleared and recreated", prefix);
    return nullptr;
}


#ifdef DEBUG_MMAP_TEST
// -- NAPI: mmap 全量权限测试 (覆盖 Box64 + Wine 所有模式) --
// 仅调试时启用: cmake -DDEBUG_MMAP_TEST=ON
#include <sys/mman.h>
#include <spawn.h>
#include <sys/syscall.h>

static const char* PN(int prot) {
    static char b[8];
    if (prot == 0) return "NONE";
    snprintf(b, sizeof(b), "%s%s%s",
             (prot & PROT_READ) ? "R" : "",
             (prot & PROT_WRITE) ? "W" : "",
             (prot & PROT_EXEC) ? "X" : "");
    return b;
}

static void L(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void L(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OH_LOG_INFO(LOG_APP, "[MMAP] %{public}s", buf);
}

// 测试单个 mmap 组合并输出结果
static void TestOne(const char* desc, void* p, size_t sz) {
    L("  %-20s -> %s (err=%d, %s)",
      desc, p == MAP_FAILED ? "FAIL" : "OK", errno, strerror(errno));
    if (p != MAP_FAILED) munmap(p, sz);
}

static napi_value RunMmapTests(napi_env env, napi_callback_info) {
    const size_t pg = 4096;
    L("===== OHOS mmap 全量测试 (Box64/Wine 模式覆盖) =====");

    // --- 1a. 匿名 MAP_PRIVATE ---
    L("--- 1a. ANON|PRIV 4KB ---");
    for (int p = 0; p < 8; p++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "ANON|PRIV %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), pg);
    }

    // --- 1b. 匿名 MAP_SHARED ---
    L("--- 1b. ANON|SHARED 4KB ---");
    for (int p = 0; p < 8; p++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "ANON|SHAR %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_SHARED | MAP_ANONYMOUS, -1, 0), pg);
    }

    // --- 1c. 文件 memfd SHARED ---
    L("--- 1c. FILE|SHARED 4KB ---");
    for (int p = 0; p < 8; p++) {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd < 0) continue;
        ftruncate(fd, pg);
        char desc[32];
        snprintf(desc, sizeof(desc), "FILE|SHAR %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_SHARED, fd, 0), pg);
        close(fd);
    }

    // --- 1d. 文件 memfd PRIVATE ---
    L("--- 1d. FILE|PRIV 4KB ---");
    for (int p = 0; p < 8; p++) {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd < 0) continue;
        ftruncate(fd, pg);
        char desc[32];
        snprintf(desc, sizeof(desc), "FILE|PRIV %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_PRIVATE, fd, 0), pg);
        close(fd);
    }

    // --- 2. 大尺寸 (Wine virtual alloc / ELF 加载) ---
    L("--- 2. ANON|PRIV 大尺寸 RWX ---");
    size_t big[] = {0x10000, 0x100000, 0x1000000, 0x10000000};
    for (int i = 0; i < 4; i++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "sz=0x%zx RWX", big[i]);
        TestOne(desc, mmap(NULL, big[i], PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), big[i]);
    }

    // --- 3. MAP_FIXED (Box64 ELF loader / Wine prereserve) ---
    L("--- 3a. MAP_FIXED ANON|PRIV ---");
    {
        void* base = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base != MAP_FAILED) {
            munmap(base, pg * 16);
            int prots[] = {PROT_READ | PROT_WRITE,
                           PROT_READ | PROT_EXEC,
                           PROT_READ | PROT_WRITE | PROT_EXEC};
            const char* names[] = {"RW", "RX", "RWX"};
            for (int i = 0; i < 3; i++) {
                char desc[64];
                snprintf(desc, sizeof(desc), "FIXED %s @%p", names[i], base);
                void* m = mmap(base, pg * 16, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                L("  %-24s -> %s (err=%d)", desc,
                  m == MAP_FAILED ? "FAIL" : "OK", errno);
                if (m != MAP_FAILED) munmap(m, pg * 16);
            }
        } else {
            L("  SKIP (base mmap failed)");
        }
    }

    L("--- 3b. MAP_FIXED FILE|SHARED ---");
    {
        void* base = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base != MAP_FAILED) {
            munmap(base, pg * 16);
            int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
            if (fd >= 0) {
                ftruncate(fd, pg * 16);
                int prots[] = {PROT_READ | PROT_WRITE,
                               PROT_READ | PROT_EXEC,
                               PROT_READ | PROT_WRITE | PROT_EXEC};
                const char* names[] = {"RW", "RX", "RWX"};
                for (int i = 0; i < 3; i++) {
                    char desc[64];
                    snprintf(desc, sizeof(desc), "FIXED FILE %s @%p", names[i], base);
                    void* m = mmap(base, pg * 16, prots[i],
                                   MAP_SHARED | MAP_FIXED, fd, 0);
                    L("  %-30s -> %s (err=%d)", desc,
                      m == MAP_FAILED ? "FAIL" : "OK", errno);
                    if (m != MAP_FAILED) munmap(m, pg * 16);
                }
                close(fd);
            }
        } else {
            L("  SKIP (base mmap failed)");
        }
    }

    // --- 4. MAP_NORESERVE (Box64 ELF pre-allocation) ---
    L("--- 4. MAP_NORESERVE ANON|PRIV ---");
    {
        int prots[] = {0, PROT_READ, PROT_READ | PROT_WRITE,
                       PROT_READ | PROT_EXEC,
                       PROT_READ | PROT_WRITE | PROT_EXEC};
        for (int i = 0; i < 5; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "NORESV %s", PN(prots[i]));
            TestOne(desc, mmap(NULL, pg * 256, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                               -1, 0), pg * 256);
        }
    }

    // --- 5. 高地址 hint (模拟 Box64 find47bitBlock) ---
    L("--- 5. 高地址 hint RWX ---");
    {
        void* hints[] = {nullptr,
                         (void*)0x100000000ULL,
                         (void*)0x3f00000000ULL,
                         (void*)0x7f00000000ULL,
                         (void*)0x7fff00000000ULL};
        const char* hnames[] = {"NULL", "4G", "0x3f00000000",
                                "0x7f00000000", "0x7fff00000000"};
        for (int i = 0; i < 5; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "hint=%s", hnames[i]);
            void* m = mmap(hints[i], pg * 16,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            L("  %-22s -> %s @%p (err=%d)", desc,
              m == MAP_FAILED ? "FAIL" : "OK", m, errno);
            if (m != MAP_FAILED) munmap(m, pg * 16);
        }
    }

    // --- 6. MAP_32BIT (ARM64 不支持的 x86 flag) ---
    L("--- 6. MAP_32BIT (0x40) ---");
    {
        int prots[] = {0, PROT_READ | PROT_WRITE,
                       PROT_READ | PROT_EXEC,
                       PROT_READ | PROT_WRITE | PROT_EXEC};
        for (int i = 0; i < 4; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "0x40 %s", PN(prots[i]));
            TestOne(desc, mmap(NULL, pg * 16, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | 0x40, -1, 0),
                    pg * 16);
        }
    }

    // --- 7. mprotect ---
    L("--- 7a. mprotect ANON RW -> ... ---");
    {
        void* m = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            for (int tp = 0; tp < 8; tp++) {
                int ret = mprotect(m, pg * 16, tp);
                L("  RW->%-3s -> %s (err=%d)", PN(tp),
                  ret == 0 ? "OK" : "FAIL", errno);
            }
            munmap(m, pg * 16);
        } else {
            L("  SKIP");
        }
    }

    L("--- 7b. mprotect FILE SHARED RW -> RX/RWX ---");
    {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd >= 0) {
            ftruncate(fd, pg * 16);
            void* m = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) {
                int t1[] = {PROT_READ | PROT_EXEC,
                            PROT_READ | PROT_WRITE | PROT_EXEC};
                const char* n1[] = {"RX", "RWX"};
                for (int i = 0; i < 2; i++) {
                    int ret = mprotect(m, pg * 16, t1[i]);
                    L("  FILE RW->%-3s -> %s (err=%d)", n1[i],
                      ret == 0 ? "OK" : "FAIL", errno);
                }
                munmap(m, pg * 16);
            }
            close(fd);
        }
    }

    // --- 8 + 9. 可执行代码测试 ---
    L("--- 8. ANON RWX + exec ---");
    {
        void* m = mmap(NULL, pg, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            ((uint32_t*)m)[0] = 0xD65F03C0; // ARM64 ret
            __builtin___clear_cache((char*)m, (char*)m + 4);
            L("  ANON RWX + exec -> OK");
            munmap(m, pg);
        } else {
            L("  SKIP (mmap RWX failed)");
        }
    }

    L("--- 9. Dynarec: mmap RW + mprotect RX + exec ---");
    {
        void* m = mmap(NULL, pg, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            ((uint32_t*)m)[0] = 0xD65F03C0; // ARM64 ret
            int ret = mprotect(m, pg, PROT_READ | PROT_EXEC);
            L("  RW + mprotect RX -> %s (err=%d)",
              ret == 0 ? "OK" : "FAIL", errno);
            if (ret == 0) {
                __builtin___clear_cache((char*)m, (char*)m + 4);
                L("  exec -> OK");
            }
            munmap(m, pg);
        }
    }

    // --- 10. fork mmap (用 pipe 捕获子进程输出) ---
    L("--- 10. fork mmap ---");
    {
        int fd[2]; pipe(fd);
        pid_t pid = fork();
        if (pid == 0) {
            close(fd[0]); dup2(fd[1], STDOUT_FILENO); dup2(fd[1], STDERR_FILENO);
            void* m = mmap(NULL, pg, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            printf("fork: ANON RWX -> %s (err=%d)\n", m==MAP_FAILED?"FAIL":"OK", errno);
            if (m != MAP_FAILED) {
                munmap(m, pg);
            } else {
                void* m2 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                if (m2 != MAP_FAILED) {
                    int r = mprotect(m2, pg, PROT_READ|PROT_WRITE|PROT_EXEC);
                    printf("fork: RW+mprotect->RWX -> %s (err=%d)\n", r==0?"OK":"FAIL", errno);
                    munmap(m2, pg);
                }
            }
            close(fd[1]); _exit(0);
        }
        close(fd[1]);
        char buf[512]={0}; read(fd[0], buf, sizeof(buf)-1); close(fd[0]);
        for (char* p=strtok(buf,"\n"); p; p=strtok(NULL,"\n")) L("  %s", p);
        waitpid(pid, NULL, 0);
    }

    // --- 10b. 嵌套 fork (孙进程) -- 模拟 wineserver 场景: APP->fork->box64->fork->wineserver ---
    L("--- 10b. nested fork (grandchild) ---");
    {
        int fd[2]; pipe(fd);
        pid_t pid = fork();
        if (pid == 0) {
            // child: fork again to create grandchild
            int fd2[2]; pipe(fd2);
            pid_t pid2 = fork();
            if (pid2 == 0) {
                // grandchild -- 这才是 wineserver 等效层级
                close(fd2[0]); dup2(fd2[1], STDOUT_FILENO); dup2(fd2[1], STDERR_FILENO);
                setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
                printf("gc: starting mmap tests\n");
                void* gc1 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                printf("gc: ANON RW  -> %s (err=%d)\n", gc1==MAP_FAILED?"FAIL":"OK", errno);
                if(gc1!=MAP_FAILED) munmap(gc1, pg);
                printf("gc: about to try RWX...\n");
                void* gc2 = mmap(NULL, pg, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                printf("gc: ANON RWX -> %s (err=%d)\n", gc2==MAP_FAILED?"FAIL":"OK", errno);
                if(gc2!=MAP_FAILED) munmap(gc2, pg);
                if(gc2==MAP_FAILED) {
                    printf("gc: RWX failed, trying RW+mprotect...\n");
                    void* gc3 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                    if(gc3!=MAP_FAILED){
                        int r=mprotect(gc3, pg, PROT_READ|PROT_WRITE|PROT_EXEC);
                        printf("gc: RW+mprotect->RWX -> %s (err=%d)\n", r==0?"OK":"FAIL", errno);
                        munmap(gc3, pg);
                    }
                }
                printf("gc: done\n");
                close(fd2[1]); _exit(0);
            }
            // child: collect grandchild output, relay to parent
            close(fd2[1]);
            char buf2[512]={0}; read(fd2[0], buf2, sizeof(buf2)-1); close(fd2[0]);
            int status;
            waitpid(pid2, &status, 0);
            close(fd[0]); dup2(fd[1], STDOUT_FILENO); dup2(fd[1], STDERR_FILENO);
            printf("%s", buf2);
            if(WIFSIGNALED(status)) printf("gc_child: KILLED by signal %d\n", WTERMSIG(status));
            else if(WIFEXITED(status)) printf("gc_child: exited=%d\n", WEXITSTATUS(status));
            close(fd[1]); _exit(0);
        }
        close(fd[1]);
        char buf[512]={0}; read(fd[0], buf, sizeof(buf)-1); close(fd[0]);
        for (char* p=strtok(buf,"\n"); p; p=strtok(NULL,"\n")) L("  %s", p);
        waitpid(pid, NULL, 0);
    }

    // --- 11. posix_spawn mmap_test (pipe 捕获) ---
    L("--- 11. posix_spawn mmap_test ---");
    {
        int fd[2]; pipe(fd);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fd[1], STDERR_FILENO);
        pid_t pid;
        const char* spawn_bin = "/data/service/hnp/winehua.org/winehua_0.1.0/opt/winehua/bin/mmap_test";
        char* argv[] = { (char*)spawn_bin, NULL };
        extern char** environ;
        int ret = posix_spawn(&pid, spawn_bin, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fd[1]);
        if (ret == 0) {
            char buf[4096]={0}; ssize_t n = read(fd[0], buf, sizeof(buf)-1);
            if (n > 0) {
                char* save; char* line = strtok_r(buf, "\n", &save);
                while (line) {
                    // 只显示 RWX 相关行
                    if (strstr(line, "RWX") || strstr(line, "mprotect") || strstr(line, "FAIL")) L("  %s", line);
                    line = strtok_r(NULL, "\n", &save);
                }
            }
            waitpid(pid, NULL, 0);
        } else {
            L("  FAILED err=%d(%s)", ret, strerror(ret));
        }
        close(fd[0]);
    }

    L("===== 测试完成 =====");
    return nullptr;
}
#endif // DEBUG_MMAP_TEST

// -- NAPI: stopClient — 杀掉所有 Wine 进程 --
static napi_value StopClient(napi_env, napi_callback_info) {
    KillAllProcesses();
    WaylandServer::GetInstance()->ResetFirstFrame();
    return nullptr;
}

// -- NAPI: stopAll — 杀掉所有 Wine 进程 + 停 Wayland server --
static napi_value StopAll(napi_env, napi_callback_info) {
    KillAllProcesses();
    WaylandServer::GetInstance()->Stop();
    return nullptr;
}

// -- Toplevel 回调 -> ArkTS --
static napi_threadsafe_function gToplevelTsfn = nullptr;

struct ToplevelEvent {
    uint32_t id;
    std::string event;
    std::string data;
};

static void CallJsToplevel(napi_env env, napi_value cb, void*, void* raw) {
    auto* ev = static_cast<ToplevelEvent*>(raw);
    if (env && cb && ev) {
        OH_LOG_INFO(LOG_APP, "[MW-TSCB] calling JS toplevel cb: id=%{public}u event=%{public}s data=%{public}s",
                    ev->id, ev->event.c_str(), ev->data.c_str());
        napi_value undef, args[3];
        napi_get_undefined(env, &undef);
        napi_create_uint32(env, ev->id, &args[0]);
        napi_create_string_utf8(env, ev->event.c_str(), NAPI_AUTO_LENGTH, &args[1]);
        napi_create_string_utf8(env, ev->data.c_str(), NAPI_AUTO_LENGTH, &args[2]);
        napi_call_function(env, undef, cb, 3, args, nullptr);
    }
    delete ev;
}

// -- NAPI: setToplevelCallback --
static napi_value SetToplevelCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gToplevelTsfn) {
        napi_release_threadsafe_function(gToplevelTsfn, napi_tsfn_release);
        gToplevelTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WLToplevel", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsToplevel, &gToplevelTsfn);

    WaylandServer::GetInstance()->SetToplevelCallback([](uint32_t id, const char* event, const char* data) {
        if (gToplevelTsfn) {
            OH_LOG_INFO(LOG_APP, "[MW-TSCB] enqueue toplevel cb: id=%{public}u event=%{public}s", id, event);
            auto* ev = new ToplevelEvent{id, event ? event : "", data ? data : "{}"};
            napi_call_threadsafe_function(gToplevelTsfn, ev, napi_tsfn_blocking);
        } else {
            OH_LOG_WARN(LOG_APP, "[MW-TSCB] toplevel cb dropped (tsfn not ready): id=%{public}u event=%{public}s",
                        id, event);
        }
    });

    return nullptr;
}

// -- NAPI: getCurrentToplevelId -- (WineWindow.aboutToAppear 同步读取, 无竞态)
static napi_value GetCurrentToplevelId(napi_env env, napi_callback_info info) {
    uint32_t id = PluginManager::GetInstance()->DequeuePendingToplevel();
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] getCurrentToplevelId = %{public}u", id);
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- NAPI: setPendingToplevel -- (WineWindowAbility 在 loadContent 前调用)
static napi_value SetPendingToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    PluginManager::GetInstance()->SetPendingToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] setPendingToplevel id=%{public}u", id);
    return nullptr;
}

// -- NAPI: destroyToplevel -- (ArkTS 关闭子窗口后调用)
static napi_value DestroyToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    PluginManager::GetInstance()->DestroyToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyToplevel id=%{public}u", id);
    return nullptr;
}

// -- NAPI: sendToplevelClose -- (通知 Wine 关闭窗口)
static napi_value SendToplevelClose(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] sendToplevelClose id=%{public}u", id);
    WaylandServer::GetInstance()->SendToplevelClose(id);
    return nullptr;
}

// -- NAPI: createRenderer -- (XComponentController.onSurfaceCreated 调用)
static napi_value CreateRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: need 2 args (toplevelId, surfaceId)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int64_t surfaceId = 0;
    bool lossless = true;
    napi_status s = napi_get_value_bigint_int64(env, args[1], &surfaceId, &lossless);
    if (s != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: BIGINT parse failed status=%{public}d", s);
        return nullptr;
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] createRenderer tl=%{public}u surfaceId=%{public}ld", tid, surfaceId);
    PluginManager::GetInstance()->CreateRenderer(tid, surfaceId);
    return nullptr;
}

// -- NAPI: resizeRenderer -- (XComponentController.onSurfaceChanged 调用)
static napi_value ResizeRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] resizeRenderer: need 3 args (toplevelId, w, h)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int32_t w = 0, h = 0;
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] resizeRenderer tl=%{public}u %{public}dx%{public}d", tid, w, h);
    PluginManager::GetInstance()->ResizeRenderer(tid, w, h);
    return nullptr;
}

// -- NAPI: destroyRenderer -- (XComponentController.onSurfaceDestroyed 调用)
static napi_value DestroyRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t tid = 0;
    if (argc >= 1) {
        napi_get_value_uint32(env, args[0], &tid);
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyRenderer tl=%{public}u", tid);
    PluginManager::GetInstance()->DestroyToplevel(tid);
    return nullptr;
}

// -- NAPI: setDisplayScale -- (传入设备 densityPixels, 供渲染层计算 viewport)
static napi_value SetOutputSize(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t w, h;
    napi_get_value_int32(env, args[0], &w);
    napi_get_value_int32(env, args[1], &h);
    WaylandServer::GetInstance()->SetOutputSize(w, h);
    return nullptr;
}

static napi_value SetDisplayScale(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double scale;
    napi_get_value_double(env, args[0], &scale);
    EglRenderer::SetGlobalDisplayScale((float)scale);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] setDisplayScale = %.2f", scale);
    return nullptr;
}

static napi_value SetDesktopMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        bool on;
        napi_get_value_bool(env, args[0], &on);
        WaylandServer::GetInstance()->SetDesktopMode(on);
        OH_LOG_INFO(LOG_APP, "[MW-NAPI] setDesktopMode = %{public}s", on ? "true" : "false");
    }
    return nullptr;
}

static napi_value GetDesktopRootId(napi_env env, napi_callback_info) {
    uint32_t id = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- Input forwarding NAPI (unified InputManager path) --
static napi_value SendPointerEvent(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 5) return nullptr;
    uint32_t tl; int32_t action; double px, py; int32_t button;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &action);
    napi_get_value_double(env, args[2], &px);
    napi_get_value_double(env, args[3], &py);
    napi_get_value_int32(env, args[4], &button);
    if (action != 1) {  // 跳过 MOVE (高频), 只记录 button/enter/leave
        OH_LOG_INFO(LOG_APP, "[PIPE] ptr tl=%{public}u a=%{public}d btn=0x%{public}x "
                    "px=(%{public}.0f,%{public}.0f)",
                    tl, action, button, px, py);
    }
    InputManager::GetInstance()->SendPointerEvent(tl, action, px, py, button);
    return nullptr;
}

static napi_value SendKeyEvent(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t evdevCode; bool pressed;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &evdevCode);
    napi_get_value_bool(env, args[2], &pressed);
    OH_LOG_INFO(LOG_APP, "[PIPE] key tl=%{public}u evdev=%{public}d down=%{public}s",
                tl, evdevCode, pressed ? "true" : "false");
    InputManager::GetInstance()->SendKeyEvent(tl, evdevCode, pressed);
    return nullptr;
}

static napi_value SendScrollEvent(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value args[6];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 6) return nullptr;
    uint32_t tl; int32_t axis; double value; int32_t scrollStep; double px; double py;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &axis);
    napi_get_value_double(env, args[2], &value);
    napi_get_value_int32(env, args[3], &scrollStep);
    napi_get_value_double(env, args[4], &px);
    napi_get_value_double(env, args[5], &py);
    OH_LOG_INFO(LOG_APP, "[PIPE] scroll tl=%{public}u axis=%{public}s val=%{public}.1f step=%{public}d px=(%{public}.0f,%{public}.0f)",
                tl, axis == 0 ? "VERT" : "HORIZ", value, scrollStep, px, py);
    InputManager::GetInstance()->SendScrollEvent(tl, axis, value, scrollStep, px, py);
    return nullptr;
}

static napi_value NotifyToplevelResize(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t w, h;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    WaylandServer::GetInstance()->NotifyToplevelResize(tl, w, h);
    return nullptr;
}

// Desktop 模式: 将 toplevel 提到 Z-order 最顶层
static napi_value RaiseToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    uint32_t tl;
    napi_get_value_uint32(env, args[0], &tl);
    WaylandServer::GetInstance()->RaiseToplevel(tl);
    return nullptr;
}

// Desktop 模式: 查找 Wine 桌面坐标 (x,y) 处最上层的 toplevel
static napi_value FindToplevelAt(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t x, y;
    napi_get_value_int32(env, args[0], &x);
    napi_get_value_int32(env, args[1], &y);
    uint32_t id = WaylandServer::GetInstance()->FindToplevelAt(x, y);
    napi_value result;
    napi_create_uint32(env, id, &result);
    return result;
}

static napi_value SetToplevelVisible(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    uint32_t tl; bool visible;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_bool(env, args[1], &visible);
    InputManager::GetInstance()->SetToplevelVisible(tl, visible);
    if (visible) {
        WaylandServer::GetInstance()->NotifyWindowRestored(tl);
    }
    return nullptr;
}

// -- NAPI: getProcessList — 返回运行中进程列表 --
static napi_value GetProcessList(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(gProcMutex);

    napi_value arr;
    napi_create_array_with_length(env, gProcRegistry.size(), &arr);

    for (size_t i = 0; i < gProcRegistry.size(); i++) {
        const auto& entry = gProcRegistry[i];
        napi_value obj;
        napi_create_object(env, &obj);

        napi_value pidVal, nameVal, pathVal, stateVal;
        napi_create_int32(env, entry.pid, &pidVal);
        napi_create_string_utf8(env, entry.exeBasename.c_str(), NAPI_AUTO_LENGTH, &nameVal);
        napi_create_string_utf8(env, entry.exeFullPath.c_str(), NAPI_AUTO_LENGTH, &pathVal);
        napi_create_string_utf8(env, entry.running ? "running" : "exited",
                                NAPI_AUTO_LENGTH, &stateVal);

        napi_property_descriptor props[] = {
            {"pid",   nullptr, nullptr, nullptr, nullptr, pidVal,   napi_default, nullptr},
            {"name",  nullptr, nullptr, nullptr, nullptr, nameVal,  napi_default, nullptr},
            {"path",  nullptr, nullptr, nullptr, nullptr, pathVal,  napi_default, nullptr},
            {"state", nullptr, nullptr, nullptr, nullptr, stateVal, napi_default, nullptr},
        };
        napi_define_properties(env, obj, sizeof(props)/sizeof(props[0]), props);
        napi_set_element(env, arr, i, obj);
    }

    OH_LOG_INFO(LOG_APP, "[NAPI] getProcessList returned %{public}zu processes", gProcRegistry.size());
    return arr;
}

// -- NAPI: killProcess — 杀掉指定进程 --
static napi_value KillProcess(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;

    int32_t pid = 0;
    napi_get_value_int32(env, args[0], &pid);
    OH_LOG_INFO(LOG_APP, "[NAPI] killProcess pid=%{public}d", pid);

    {
        std::lock_guard<std::mutex> lock(gProcMutex);
        for (auto& entry : gProcRegistry) {
            if (entry.pid == pid && entry.running) {
                OH_LOG_INFO(LOG_APP, "[NAPI] killProcess found pid=%{public}d name=%{public}s",
                            pid, entry.exeBasename.c_str());
                *(entry.readerActive) = false;
                kill(pid, SIGKILL);
                if (entry.stdoutFd >= 0) { close(entry.stdoutFd); entry.stdoutFd = -1; }
                break;
            }
        }
    }

    napi_value r;
    napi_get_boolean(env, true, &r);
    return r;
}

// -- 模块注册 --
EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_INFO(LOG_APP, "[MW-NAPI]  Init called, env=%{public}p", env);

    napi_property_descriptor desc[] = {
        {"startServer",    nullptr, StartServer,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"launchClient",   nullptr, LaunchClient,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopClient",     nullptr, StopClient,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAll",        nullptr, StopAll,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStateCallback", nullptr, SetStateCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelCallback", nullptr, SetToplevelCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCurrentToplevelId", nullptr, GetCurrentToplevelId, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPendingToplevel", nullptr, SetPendingToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyToplevel", nullptr, DestroyToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendToplevelClose", nullptr, SendToplevelClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineExe",     nullptr, RunWineExe,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"checkWinePrefix",nullptr, CheckWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resetWinePrefix",nullptr, ResetWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        // surfaceId 驱动的渲染器管理 (XComponentController 回调)
        {"createRenderer",  nullptr, CreateRenderer,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeRenderer",  nullptr, ResizeRenderer,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyRenderer", nullptr, DestroyRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
#ifdef DEBUG_MMAP_TEST
        {"runMmapTests",  nullptr, RunMmapTests,  nullptr, nullptr, nullptr, napi_default, nullptr},
#endif
        {"setOutputSize",   nullptr, SetOutputSize,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDisplayScale",  nullptr, SetDisplayScale,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDesktopMode",   nullptr, SetDesktopMode,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDesktopRootId", nullptr, GetDesktopRootId, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ArkTS input forwarding (unified InputManager path)
        {"sendPointerEvent", nullptr, SendPointerEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendKeyEvent",     nullptr, SendKeyEvent,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendScrollEvent",   nullptr, SendScrollEvent,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"notifyToplevelResize",nullptr,NotifyToplevelResize,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"findToplevelAt",   nullptr, FindToplevelAt,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"raiseToplevel",    nullptr, RaiseToplevel,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelVisible", nullptr, SetToplevelVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getProcessList",   nullptr, GetProcessList,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"killProcess",     nullptr, KillProcess,     nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // surfaceId 架构: 不再使用 libraryname='entry', XComponent 通过
    // 自定义 Controller 回调拿到 surfaceId, 由 createRenderer/renderer 管理。
    // 不再需要保存 gEnv/gExports, 不再依赖 XComponent exports 对象。
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] Init complete OK");
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule() {
    napi_module_register(&demoModule);
}

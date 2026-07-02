#include "wine_launch.h"
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
#include <thread>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

#include "broker.h"
#include "wait_utils.h"

#ifdef PAD_MODE
#include <AbilityKit/native_child_process.h>

// Pad: 从 envp 重建 environ (fork 后 __wine_main / main 从 environ 读环境变量)
static void rebuild_environ(char* const* envp) {
    extern char** environ;
    environ = (char**)envp;
}
#endif

// 轮询 wineserver socket 是否就绪
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

void LaunchThreadFunc(LaunchParams* p) {
    OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver + wineboot + wine starting in background");
    OH_LOG_INFO(LOG_APP, "[Launch-Async] XKB_CONFIG_ROOT=%{public}s",
                (p->winehuaBin + "/../share/X11/xkb").c_str());

    // 初始化 GraphicsBroker
    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(p->winehuaBin);
    winehua::GraphicsBroker::GetInstance().EnsureStarted(p->sockDir);

    // 组装 envp 环境变量
    int audioBootstrapFd = CreateAudioBootstrapFd(p->sockDir);
    p->envStrs = BuildWineEnv(p->sockDir, p->sockName, p->libPath, p->winehuaBin, audioBootstrapFd);
    for (auto& s : p->envStrs) p->envp.push_back((char*)s.c_str());
    p->envp.push_back(nullptr);

    mkdir(WINE_PREFIX, 0755);
    mkdir("/storage/Users/currentUser/Download/app.hackeris.winehua", 0755);

    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wineserver-starting"), napi_tsfn_blocking);
    }

    // -- 启动 wineserver --
#ifdef PAD_MODE
    {
        std::string wsEntryParams = p->winehuaBin + "|wineserver|-f|-p|--no-auto-close";
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
        if (!WaitFor("wineserver socket", IsWineserverSocketReady, 5000, 100)) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not detected, "
                        "wineboot will recover via server_connect retry+start_server");
        }
    }
#else
    // PC: fork + execve
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
            const char* wsArgv[] = {"./box64", "./wineserver", nullptr};
            execve("./box64", (char* const*)wsArgv, wsEnvp.data());
            _exit(127);
        }
        if (wsPid > 0) {
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d", wsPid);
            if (wsPipeOk) {
                close(wsPipe[1]);
                StartStderrLogger(wsPipe[0], "wineserver-stderr");
            }
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

    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-starting"), napi_tsfn_blocking);
    }

#ifdef PAD_MODE
    StartBrokerServer();
    setenv("PROCESSBROKER", WINE_BROKER_SOCKET, 1);

    bool prefixReady = []() {
        DIR* d = opendir(WINE_PREFIX "/drive_c");
        if (d) { closedir(d); return true; }
        return false;
    }();

    if (!prefixReady) {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix not ready, running wineboot --init...");

#ifdef __aarch64__
        std::string entryParams = p->winehuaBin + "|wineboot|--init";
#else
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
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot started, pid=%{public}d", childPid);
            if (!WaitFor("wine prefix drive_c", []() {
                DIR* d = opendir(WINE_PREFIX "/drive_c");
                if (d) { closedir(d); return true; }
                return false;
            }, 30000, 200)) {
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot drive_c timeout, abort");
                if (gStateTsfn)
                    napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
                delete p;
                return;
            }
            char procPath[64];
            snprintf(procPath, sizeof(procPath), "/proc/%d", childPid);
            OH_LOG_INFO(LOG_APP, "[Launch-Async] waiting for wineboot pid=%{public}d to exit...", childPid);
            for (int i = 0; i < 120; i++) {
                if (access(procPath, F_OK) != 0) break;
                usleep(500000);
            }
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot completed");
        } else {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot FAILED ret=%{public}d", (int)ret);
            if (gStateTsfn)
                napi_call_threadsafe_function(gStateTsfn, strdup("wineboot-failed"), napi_tsfn_blocking);
            delete p;
            return;
        }
    } else {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix already initialized, skipping wineboot");
    }

    // -- explorer desktop shell --
    {
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
    }
#else
    // -- wineboot --init (PC: fork + execve ./box64) --
    {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] running wineboot --init...");
        pid_t bootPid = fork();
        if (bootPid == 0) {
            CloseInheritedFds({STDOUT_FILENO, STDERR_FILENO, audioBootstrapFd});
            for (int s = 1; s < 32; ++s) signal(s, SIG_DFL);
            prctl(PR_SET_NAME, "wl-wineboot", 0, 0, 0);
            chdir(p->winehuaBin.c_str());
            const char* bootArgv[] = {"./box64", "./wine", "./wineboot", "--init", nullptr};
            execve("./box64", (char* const*)bootArgv, p->envp.data());
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
    }
#endif

    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("wine-ready"), napi_tsfn_blocking);
    }

    delete p;
}

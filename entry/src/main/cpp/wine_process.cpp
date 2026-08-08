#include "wine_process.h"
#include "wine_constants.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

// -- 全局状态 --
static std::mutex gProcMutex;
static std::vector<WineProcessEntry> gProcRegistry;

// -- 主 wineserver 会话锚点 --
// gShutdownRequested: 任何 KillAllProcesses (stopAll/stopClient/resetWinePrefix)
// 都意味着主 wineserver 的死亡是主动停止而非引擎故障, ProcMon 据此抑制
// state:failed:wineserver 上报; 只在 RegisterWineserver (新会话建立) 时清除,
// 停止编排完成后仍保持 — 此后注册表里已无存活会话, 迟到的死亡检测同样不该报失败。
static std::atomic<pid_t> gWineserverPid{-1};
static std::atomic<bool> gShutdownRequested{false};
// gDesktopSessionEnded: desktop 根 toplevel 已销毁 (explorer 主动结束桌面会话)。
// 桌面主动退出时 wineserver 会跟随退出 (explorer 先走、wineserver 后走) — 这是
// 正常会话终结而非崩溃, ProcMon 据此把锚点死亡按 state:stopped 收口而非误报
// state:failed:wineserver。只在 RegisterWineserver (新会话建立) 时清除。
// PC 窗口模式没有 desktop root, 此标记永不置位, 崩溃判定行为不变。
static std::atomic<bool> gDesktopSessionEnded{false};

static uint64_t TimestampMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 前向声明
static void EnsureMonitorRunning();

bool IsProcessAliveNotZombie(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE* f = fopen(path, "r");
    if (!f) return false;                       // /proc 消失 = 已退出
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char* rp = strrchr(buf, ')');               // state 字段在最后一个 ')' 之后
    return !(rp && rp[2] == 'Z');               // 僵尸 = 已退出
}

void RegisterWineserver(pid_t pid) {
    /* 热重启/复用场景: 若已有存活的旧 wineserver (旧会话锚点), 保持旧锚点。
     * wine 单实例语义下, 新 spawn 的 wineserver 检测到旧实例会连接后正常退出
     * —— 这不是引擎故障 (master 同款: wineserver 不登记, 新实例退出无感)。
     * 仅当旧锚点已死 (冷启动 / stopAll 杀干净 / 引擎崩溃后重建) 才接管为新锚点,
     * 否则 ProcMon 会把"新实例正常退出"误判为 state:failed:wineserver。
     * 新 pid 仍 AddProcess 登记 (ProcMon 监视 + KillAllProcesses 可杀),
     * 只是不作为会话锚点判定死法。 */
    pid_t existing = gWineserverPid.load(std::memory_order_acquire);
    if (existing > 0 && IsProcessAliveNotZombie(existing)) {
        OH_LOG_INFO(LOG_APP, "[ProcReg] wineserver %{public}d alive, keep anchor; new spawn %{public}d will attach",
                    existing, pid);
        AddProcess(pid, "wineserver", -1);
        return;
    }
    gWineserverPid.store(pid);
    gShutdownRequested.store(false);
    gDesktopSessionEnded.store(false);
    AddProcess(pid, "wineserver", -1);
}

pid_t GetWineserverPid() {
    return gWineserverPid.load();
}

void MarkDesktopSessionEnded() {
    gDesktopSessionEnded.store(true);
}

// -- 注册表辅助函数 --
WineProcessEntry* AddProcess(pid_t pid, const std::string& exeFullPath, int stdoutFd) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    std::string basename = exeFullPath;
    // 兼容 Windows 反斜杠路径 (C:\game\game.exe), 否则完整路径会显示为进程名
    auto slash = basename.find_last_of("/\\");
    if (slash != std::string::npos) basename = basename.substr(slash + 1);
    gProcRegistry.erase(std::remove_if(gProcRegistry.begin(), gProcRegistry.end(),
        [pid](const WineProcessEntry& entry) { return entry.pid == pid; }), gProcRegistry.end());
    while (gProcRegistry.size() >= 128) {
        auto ended = std::find_if(gProcRegistry.begin(), gProcRegistry.end(),
            [](const WineProcessEntry& entry) { return !entry.running; });
        if (ended == gProcRegistry.end()) break;
        gProcRegistry.erase(ended);
    }
    gProcRegistry.push_back({
        .pid = pid,
        .exeBasename = basename,
        .exeFullPath = exeFullPath,
        .running = true,
        .startTimestampMs = TimestampMs(),
        .endTimestampMs = 0,
        .exitCode = -1,
        .exitCodeSource = "unknown",
        .stdoutFd = stdoutFd,
        .readerActive = std::make_shared<std::atomic<bool>>(true)
    });
    OH_LOG_INFO(LOG_APP, "[ProcReg] add pid=%{public}d name=%{public}s total=%{public}zu",
                pid, basename.c_str(), gProcRegistry.size());
    EnsureMonitorRunning();
    // 通知 ArkTS 刷新进程列表: wine 内部自启的子进程 (走 broker 登记) 没有
    // 调用者发 evt:launch-accepted, 必须在这里统一发一次, 否则新程序不出现在任务列表。
    // ArkTS 侧对 proc-updated 做了节流, 高频进出的系统进程不会触发大量刷新。
    if (gStateTsfn) {
        napi_call_threadsafe_function(gStateTsfn, strdup("evt:proc-updated"), napi_tsfn_blocking);
    }
    return &gProcRegistry.back();
}

void RemoveProcess(pid_t pid, int exitCode, const std::string& exitCodeSource) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (auto& entry : gProcRegistry) {
        if (entry.pid == pid) {
            OH_LOG_INFO(LOG_APP,
                        "[ProcReg] complete pid=%{public}d name=%{public}s exit=%{public}d source=%{public}s",
                        pid, entry.exeBasename.c_str(), exitCode, exitCodeSource.c_str());
            entry.running = false;
            entry.endTimestampMs = TimestampMs();
            entry.exitCode = exitCode;
            entry.exitCodeSource = exitCodeSource;
            if (entry.stdoutFd >= 0) { close(entry.stdoutFd); entry.stdoutFd = -1; }
            return;
        }
    }
}

void KillAllProcesses() {
    // 主动停止标记: 之后主 wineserver 的死亡检测 (ProcMon) 不再报 state:failed
    gShutdownRequested.store(true);
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

// stopAll 末尾调用: 后台 zombie 感知等待注册表进程 (含主 wineserver) 全部死亡,
// 完成发一次 state:stopped (蓝图「完全退出」判据的进程侧; Wayland server 由
// 调用方在此之前同步停掉)。30s 封顶兜底: SIGKILL 下进程卡 D-state 不死时
// 不至于永不发声 (ArkTS 侧另有超时硬放行, 双保险)。
void NotifyWhenSessionDrained() {
    std::thread([]() {
        constexpr int kDrainTimeoutMs = 30000;
        int waitedMs = 0;
        while (waitedMs < kDrainTimeoutMs) {
            bool anyAlive = false;
            {
                std::lock_guard<std::mutex> lock(gProcMutex);
                for (const auto& entry : gProcRegistry) {
                    if (entry.running && IsProcessAliveNotZombie(entry.pid)) {
                        anyAlive = true;
                        break;
                    }
                }
            }
            if (!anyAlive) break;
            usleep(100000);
            waitedMs += 100;
        }
        if (waitedMs >= kDrainTimeoutMs) {
            OH_LOG_ERROR(LOG_APP, "[ProcReg] drain wait timeout: process survived SIGKILL");
        }
        gWineserverPid.store(-1);
        OH_LOG_INFO(LOG_APP, "[ProcReg] session drained in %{public}d ms, emit state:stopped",
                    waitedMs);
        if (gStateTsfn) {
            napi_call_threadsafe_function(gStateTsfn, strdup("state:stopped"),
                                          napi_tsfn_blocking);
        }
    }).detach();
}

// -- 进程退出状态日志 --
void LogProcessExit(const char* tag, pid_t pid, int status) {
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

// -- fork/exec 后关闭继承的 fd --
void CloseInheritedFds(std::initializer_list<int> keepFds) {
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

// -- SIGCHLD handler: reap NCP child processes spawned by broker --
void sigchld_handler(int) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        LogProcessExit("broker-child", pid, status);
        RemoveProcess(pid);
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "evt:proc-exited:%d", pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
    }
}

// -- NCP 进程存活监控 --
// NCP 子进程由 appspawn 创建，不是主进程的 fork() 子进程，
// SIGCHLD 收不到它们的退出事件。通过 /proc/<pid> 轮询检测退出。
// 判活必须 zombie 感知 (fork 模式子进程退出后 /proc 不立即消失),
// 否则已退出的进程会被长期误判为存活。
static std::atomic<bool> gMonitorRunning{false};
static std::thread gMonitorThread;

static void ProcessMonitorLoop() {
    OH_LOG_INFO(LOG_APP, "[ProcMon] started");
    while (gMonitorRunning.load(std::memory_order_relaxed)) {
        sleep(1);

        std::vector<pid_t> exitedPids;
        {
            std::lock_guard<std::mutex> lock(gProcMutex);
            for (const auto& entry : gProcRegistry) {
                if (!entry.running) continue;
                if (!IsProcessAliveNotZombie(entry.pid)) {
                    exitedPids.push_back(entry.pid);
                }
            }
        }

        for (pid_t pid : exitedPids) {
            OH_LOG_INFO(LOG_APP, "[ProcMon] pid=%{public}d no longer alive", pid);
            RemoveProcess(pid);
            if (gStateTsfn) {
                char msg[64];
                snprintf(msg, sizeof(msg), "evt:proc-exited:%d", pid);
                napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
            }
            // 主 wineserver 死亡, 按死法分三种收口:
            // 1) gShutdownRequested (stopAll/reset 主动停止): StopAll 末尾的
            //    drain 会发 state:stopped, 这里不动
            // 2) gDesktopSessionEnded (桌面主动退出带动 wineserver 跟随退出,
            //    explorer 先走 wineserver 后走): 正常会话终结不是崩溃 — 扫尾
            //    残余进程 (winehua_keep 等) 后按正常停止发 state:stopped
            // 3) 其余 = 非预期死亡: 报 state:failed:wineserver (引擎故障)
            if (pid == gWineserverPid.load(std::memory_order_acquire)) {
                gWineserverPid.store(-1, std::memory_order_release);
                if (gShutdownRequested.load(std::memory_order_acquire)) {
                    // 主动停止编排中, state:stopped 由 StopAll 的 drain 发
                } else if (gDesktopSessionEnded.load(std::memory_order_acquire)) {
                    OH_LOG_INFO(LOG_APP, "[ProcMon] wineserver exited after desktop session end, clean stop");
                    KillAllProcesses();
                    NotifyWhenSessionDrained();
                } else if (gStateTsfn) {
                    OH_LOG_ERROR(LOG_APP, "[ProcMon] wineserver died unexpectedly, emit state:failed:wineserver");
                    napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineserver"),
                                                  napi_tsfn_blocking);
                }
            }
        }
    }
    OH_LOG_INFO(LOG_APP, "[ProcMon] stopped");
}

static void EnsureMonitorRunning() {
    if (!gMonitorRunning.load(std::memory_order_acquire)) {
        gMonitorRunning.store(true, std::memory_order_release);
        gMonitorThread = std::thread(ProcessMonitorLoop);
        gMonitorThread.detach();
    }
}

// -- 客户端 stdout/stderr 读取线程 (每个进程独立) --
void ReaderThread(int fd, pid_t pid, std::shared_ptr<std::atomic<bool>> active) {
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
    if (!pending.empty()) {
        OH_LOG_INFO(LOG_APP, "[wine:%{public}d] %{public}s", pid, pending.c_str());
    }
    close(fd);

    int status;
    waitpid(pid, &status, 0);
    LogProcessExit("wine", pid, status);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    RemoveProcess(pid, exitCode, "process");

    if (gStateTsfn) {
        char msg[64];
        snprintf(msg, sizeof(msg), "evt:proc-exited:%d", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
    }
}

// -- stderr pipe reader (后台线程, 逐行日志) --
void StartStderrLogger(int fd, const char* tag,
                       std::shared_ptr<std::atomic<bool>> done) {
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
                    if (!line.empty())
                        OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}s", tag, line.c_str());
                    pending.erase(0, pos + 1);
                }
            } else {
                if (n == 0 || (n < 0 && errno != EINTR)) break;
            }
        }
        if (!pending.empty())
            OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}s", tag, pending.c_str());
        close(fd);
    }).detach();
}

std::vector<WineProcessEntry> GetProcessListSnapshot() {
    std::lock_guard<std::mutex> lock(gProcMutex);
    return gProcRegistry;
}

bool QueryProcessSnapshot(pid_t pid, WineProcessEntry* outEntry) {
    if (!outEntry) return false;
    std::lock_guard<std::mutex> lock(gProcMutex);
    auto it = std::find_if(gProcRegistry.begin(), gProcRegistry.end(),
        [pid](const WineProcessEntry& entry) { return entry.pid == pid; });
    if (it == gProcRegistry.end()) return false;
    *outEntry = *it;
    return true;
}

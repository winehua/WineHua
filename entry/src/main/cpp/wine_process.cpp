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

static uint64_t TimestampMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 前向声明
static void EnsureMonitorRunning();

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
                char procPath[64];
                snprintf(procPath, sizeof(procPath), "/proc/%d", entry.pid);
                if (access(procPath, F_OK) != 0) {
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

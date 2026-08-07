#pragma once

#include <napi/native_api.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <initializer_list>
#include <cstdint>

// -- 进程注册表入口 --
struct WineProcessEntry {
    pid_t pid;
    std::string exeBasename;
    std::string exeFullPath;
    bool running;
    uint64_t startTimestampMs;
    uint64_t endTimestampMs;
    int exitCode;
    std::string exitCodeSource;
    int stdoutFd;
    std::shared_ptr<std::atomic<bool>> readerActive;
};

// -- NAPI threadsafe 回调 (由 napi_init.cpp 设置) --
// 消息协议 (native → ArkTS, 单向): 分两类, ArkTS 侧按前缀分发, 不做启发式猜测。
//   state:<name>[:<detail>]  引擎持久状态迁移 (状态机语义):
//     state:starting:wineserver|wineboot  state:ready  state:failed:<stage>
//   evt:<name>[:<pid>]       瞬时事件 (不迁移状态):
//     evt:launch-accepted:<pid>  evt:launch-failed
//     evt:proc-updated  evt:proc-exited:<pid>
extern napi_threadsafe_function gStateTsfn;
extern std::string gSockPath;

// -- 进程注册表 --
WineProcessEntry* AddProcess(pid_t pid, const std::string& exeFullPath, int stdoutFd);
void RemoveProcess(pid_t pid, int exitCode = -1,
                   const std::string& exitCodeSource = "unknown");
void KillAllProcesses();

// -- 进程注册表只读访问 (供 NAPI handler) --
std::vector<WineProcessEntry> GetProcessListSnapshot();
bool QueryProcessSnapshot(pid_t pid, WineProcessEntry* outEntry);

// -- 辅助函数 --
void LogProcessExit(const char* tag, pid_t pid, int status);
void CloseInheritedFds(std::initializer_list<int> keepFds);

// -- 信号处理 --
void sigchld_handler(int);

// -- 子进程日志读取 --
void ReaderThread(int fd, pid_t pid, std::shared_ptr<std::atomic<bool>> active);
void StartStderrLogger(int fd, const char* tag,
                       std::shared_ptr<std::atomic<bool>> done = nullptr);

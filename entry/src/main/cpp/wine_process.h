#pragma once

#include <napi/native_api.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <initializer_list>

// -- 进程注册表入口 --
struct WineProcessEntry {
    pid_t pid;
    std::string exeBasename;
    std::string exeFullPath;
    bool running;
    int stdoutFd;
    std::shared_ptr<std::atomic<bool>> readerActive;
};

// -- NAPI threadsafe 回调 (由 napi_init.cpp 设置) --
extern napi_threadsafe_function gStateTsfn;
extern std::string gSockPath;

// -- 进程注册表 --
WineProcessEntry* AddProcess(pid_t pid, const std::string& exeFullPath, int stdoutFd);
void RemoveProcess(pid_t pid);
void KillAllProcesses();

// -- 进程注册表只读访问 (供 NAPI handler) --
std::vector<WineProcessEntry> GetProcessListSnapshot();

// -- 辅助函数 --
void LogProcessExit(const char* tag, pid_t pid, int status);
void CloseInheritedFds(std::initializer_list<int> keepFds);

// -- 信号处理 --
void sigchld_handler(int);

// -- 子进程日志读取 --
void ReaderThread(int fd, pid_t pid, std::shared_ptr<std::atomic<bool>> active);
void StartStderrLogger(int fd, const char* tag,
                       std::shared_ptr<std::atomic<bool>> done = nullptr);

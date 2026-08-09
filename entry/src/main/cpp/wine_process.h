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
    // 桌面 root 出现前加入的会话基础进程 (desktop + 桌面出现前的 explorer 等),
    // 由 MarkDesktopShellProcesses 在桌面首次出现时打标。ArkTS 运行中页据此隐藏
    // "结束"操作 — 结束任一都会破坏桌面运行。桌面出现后启动的进程不受影响。
    bool desktopShell = false;
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
//     state:stopped   会话终结 (注册表进程全死 zombie 感知 + wineserver 死);
//         两个触发点: stopAll 编排完成 / 桌面主动退出带动 wineserver 跟随退出
//         (正常终结, desktop root 先销毁 — 由 gDesktopSessionEnded 判别)
//     state:ready-degraded   桌面根 15s 未就绪的降级 ready (仅 desktop 模式;
//         root 出现后由 evt:desktop-ready 升级为正式 ready)
//   state:failed:wineserver 的另一触发点: ProcMon 检测到主 wineserver 非预期
//     死亡 (stopAll/KillAllProcesses 主动停止期间不上报)
//   evt:<name>[:<pid>]       瞬时事件 (不迁移状态):
//     evt:launch-accepted:<pid>  evt:launch-failed
//     evt:proc-updated  evt:proc-exited:<pid>
//     evt:desktop-ready   桌面根 toplevel 出现 (WaylandServer FireToplevelEvent
//         的 desktop_root 钩子补发; 唯一例外: ArkTS 用它把 ready-degraded 升级为 ready)
extern napi_threadsafe_function gStateTsfn;
extern std::string gSockPath;

// -- 进程注册表 --
WineProcessEntry* AddProcess(pid_t pid, const std::string& exeFullPath, int stdoutFd);
void RemoveProcess(pid_t pid, int exitCode = -1,
                   const std::string& exitCodeSource = "unknown");
void KillAllProcesses();

// -- 主 wineserver 会话锚点 --
// LaunchPadMode spawn wineserver 成功后登记: 加入进程注册表 (ProcMon 监视,
// KillAllProcesses 可杀) 并记为会话锚点, 其非预期死亡 → state:failed:wineserver
void RegisterWineserver(pid_t pid);
pid_t GetWineserverPid();
// desktop 根 toplevel 销毁时调用 (WaylandServer::OnToplevelDestroyed):
// 标记桌面会话已由 explorer 主动结束 — 随后 wineserver 跟随退出属正常终结,
// ProcMon 按 state:stopped 收口而非误报 state:failed:wineserver
void MarkDesktopSessionEnded();
// -- 桌面 shell 进程标记 (防误操作结束桌面基础进程) --
// 桌面会话启动 (LaunchPadMode spawn explorer 前) 调用: 清除上次会话的标记守卫,
// 使新会话首次 desktop_root 出现时能重新标记 (热重启复用旧 wineserver 也生效)。
void BeginDesktopSession();
// 桌面 root 首次出现 (FireToplevelEvent desktop_root) 调用: 把当前 running 的
// 进程标记为桌面 shell 基础进程 (desktop + 桌面出现前加入的 explorer 等)。
// root 重建不重复标记, 避免把已在跑的用户程序误标为不可结束。
void MarkDesktopShellProcesses();
// 后台 zombie 感知等待注册表进程 (含 wineserver) 全部死亡, 完成发一次
// state:stopped — ArkTS 重启/重置/停止编排的继续条件 (stopAll 末尾调用)
void NotifyWhenSessionDrained();

// fork 模式下子进程退出先变僵尸、/proc/<pid> 不消失（NCP 模式由 appspawn 立即 reap）。
// 存活检测必须识别僵尸，否则把已退出的进程误判为存活 (wineboot 等待/ProcMon 共用)。
bool IsProcessAliveNotZombie(pid_t pid);

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

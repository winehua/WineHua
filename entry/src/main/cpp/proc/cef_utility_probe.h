#pragma once

#include <cstdint>

/*
 * cef_utility_probe — CEF/steamwebhelper 子进程生命周期观测 (2026-09-18)
 *
 * 背景: win64 Steam 的 UI 卡在启动画面。SteamUI 的 JS 报 "AxiosError: Network
 * Error" / "Still no friends list" / server clock drift 超时, 而客户端自身的
 * HTTPS/WebSocket 是好的 (bootstrap_log 304 / connection_log Logged On)。同时
 * webhelper.txt 累计: StorageService 1384 次、NetworkService 389 次、chrome
 * utility 81 次 —— CEF 的 utility 子进程在反复创建又（几乎立刻）退出, 所以
 * SteamUI 永远等不到数据。
 *
 * 要判断"为什么退"必须把四层证据按时间戳串起来, 缺一层就只能猜:
 *   1. Chromium verbose log     --enable-logging=stderr --v=1 (kernelbase 注入)
 *   2. CEF-UTILITY-CREATE/EXIT  本探针 (pid / 父 pid / subtype / lifetimeMs / signal)
 *   3. Wine [exit] RtlExitUserProcess status=...   (ntdll loader.c, 子进程侧)
 *   4. [early-fault] 现场          (wine_child.cpp, 子进程侧)
 *
 * 只观测, 不改变任何行为:
 *   - 创建侧: broker 收到 SPAWN 请求时解析 entryParams, 认出 steamwebhelper
 *     进程 (--type= / --utility-sub-type= / cef.win64|cef.win7), 记录
 *     childHostPid / parentHostPid (SO_PEERCRED) / createTime / HODLL。
 *   - 退出侧: NCP 退出回调 (OnNcpChildExit) 给出 pid + signal, 与创建侧配对算出
 *     lifetimeMs, 并累计每个 subtype 的启动次数 (restart loop 的规模)。
 * 输出同时进 hilog 与 temp/cef-utility-diag.log —— hilog 环形缓冲会滚掉历史,
 * 文件才是权威。文件 >4MB 自动轮转 .1, 长时间运行不会无界增长。
 *
 * 开关: 默认开启 (诊断成本低、且要保证"第一次安装就有"), 主进程 env 里
 * WINEHUA_CEF_UTILITY_PROBE=0 可关闭。注意 Want 的 env 只下发到 wine 子进程,
 * 正常路径下主进程看不到该变量, 也就是默认开启。
 */

// 子进程创建 (broker 侧, 主进程上下文)。entryParams 为 broker 实际派发的完整参数串。
// parentHostPid 取 broker socket 的 SO_PEERCRED —— 即调用 CreateProcess 的那个 Wine
// 进程的本机 pid, 取不到时传 -1。
void WineHuaCefUtilityProbeNoteSpawn(int32_t childPid, int32_t parentHostPid,
                                     const char *entryParams);

// 子进程退出 (NCP 退出回调 / ProcMon 死亡处置侧)。signal 为系统回调给出的信号,
// 未知时传 -1。只对创建侧登记过的 pid 生效, 重复调用是幂等的。
void WineHuaCefUtilityProbeNoteExit(int32_t childPid, int32_t signal);

// 探针是否启用 (供 broker 决定要不要传 SO_PEERCRED 之类的附加信息)。
bool WineHuaCefUtilityProbeEnabled();

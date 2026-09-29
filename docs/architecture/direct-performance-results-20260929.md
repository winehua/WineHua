# Direct/Venus 第一组性能数据（2026-09-29）

> 本页保留旧包历史。第二版启动锁修复后已完成同包 12/12 正式负载，详见 [最新实机与性能报告](direct-startup-fix-validation-20260929.md)；新旧样本不混算。

**目前没有证明显著性能收益。** 真实 AMD64/FEX 基准已在 MatePad Mini 上运行，两条短工具检查通过；固定 60 FPS 的第一组 ABBA 四轮也通过。计划的十二轮在第五轮 Venus 程序启动后超时中止，不能把第一组当作完整性能验收。

当前功能测试证明的是 Guest 系统 Vulkan → 共享 GPU NativeBuffer/fence → HAP Vulkan 合成能够运行。窗口、resize、输入和 fence 正确性不直接证明性能提升。比较的是整条 Direct 与 Venus 显示架构，包含驱动、队列和合成器的差别，不能把所有差值归因于 Vulkan 命令传输。

## 身份与测量条件

- 同一 signed HAP：`b741c40f669798a6311dc7f4c383c061891406ccafb64b0165148044d24f9f0d`。
- 同一 host payload：`smoke-v2-474c8f475ecd`；原有 34 EXE 字节不变，新增两份 benchmark。实际 AMD64 PE Machine 为 `0x8664`，SHA-256 为 `77e797a1bbc41094986f2276927509b95cbf8345148444a83748b66adcc08f58`。
- 同一 Wine、FEX、prefix、ARM64X DXVK 1.10.3；客户区 960×640。Direct 的 HAP Vulkan 输出日志为 2560×1600。
- fixed60：每帧 100 draws，第一 draw 覆盖正常 cube，后续交替 2×2 scissor；两边同一显式 60 FPS 限帧器。每轮预热 15 秒、测量约 60 秒，测量期间无截图观察器或逐帧磁盘写入。
- 游戏 `GetProcessTimes` 与 HAP `CLOCK_PROCESS_CPUTIME_ID` 均实际可用。HAP 按 500 ms 观察测量边界，CPU 对齐存在边界误差；尚无 Wine server、系统图形服务、GPU 执行时间或可靠功率数据。

## 全部四个完整样本

CPU 两进程合计按游戏进程与 HAP CPU 时间增量计算，并用 HAP 的实际观察时长对齐。下表 CPU 分母是 HAP accepted present，**不是物理扫描帧**。

| 顺序 | 后端 | 游戏 Present/s | HAP accepted present/s | 游戏帧 P99 ms | 两进程 CPU ms/accepted present（估算） |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | Venus | 59.994 | 59.622 | 20.836 | 11.520 |
| 2 | Direct | 60.011 | 59.648 | 20.146 | 12.066 |
| 3 | Direct | 60.008 | 59.630 | 20.106 | 12.113 |
| 4 | Venus | 60.013 | 59.562 | 20.884 | 11.559 |

每条路径目前只有两个完整样本。中位数：

| 指标 | Venus | Direct |
| --- | ---: | ---: |
| HAP accepted present/s | 59.592 | 59.639 |
| 游戏帧 P95 ms | 18.727 | 18.430 |
| 游戏帧 P99 ms | 20.860 | 20.126 |
| 游戏 CPU ms/游戏帧 | 4.503 | 6.493 |
| HAP CPU ms/游戏帧（估算） | 6.957 | 5.522 |
| 两进程 CPU ms/accepted present（估算） | 11.540 | 12.089 |

这个场景中，Direct 的 HAP 成本下降，但游戏进程成本上升，两进程合计约高 4.8%，没有显示 CPU 节省。P99 稍低，尚不足以确认稳定收益。Direct 把驱动工作移到 Wine 进程，Venus 在 HAP 执行部分驱动/重放工作，因此不能只选 HAP CPU 下降这一项作为优化结论。

短工具检查只预热 3 秒、测量约 9 秒：游戏提交约 79.1 / 90.1 Present/s，HAP 接受约 72.3 / 85.5 Present/s。这些短样本只验证计量链路；没有完整热状态/刷新公平性记录，不用于报告速度提升。现有 EGL 请求 NativeVSync 60..120/expected=120，Direct 用 FIFO；无上限吞吐还会受到输出调度差别影响。

## 中止、前台和工具修正

第五轮 `perf-amd64-fixed60-20260929-201057-5-venus` 的桌面就绪等待仅 311 ms，程序被接受启动为 PID 2294，随后 240 秒内没有 benchmark JSON 或有效测量窗口，设备最终写出 `stage=timeout`。原第五轮没有挂起栈。后续诊断候选复现相同的初始化症状，捕获到游戏等待桌面 WM_NULL 回复，以及主 Explorer 的 USER/surface 锁循环，详见 [启动等待定位](direct-startup-wait-20260929.md)。这些后续证据不能当成原 PID 2294 的直接栈证据，也不计入旧包性能样本。

宿主外层超时原来同样是 240 秒，抢先终止 smoke 的失败归档；已取回设备原始失败 summary、保留启动日志。d371 第一版在正式首轮再次超时，且 host 从 aa start 开始计时，仍先于设备 summary 数秒退出。runner 最终明确分层预算为设备测试 240 秒、host 轮询 300 秒、外层命令 360 秒，失败后记录有界证据并保留 App供诊断，不自动重试；专用 payload 路径和工作量核对保留。新版完整序列使用第二版修复 `4b92988e` 单独归档，不能混入本页的旧包统计。

原 runner 的 `appLifecycle.backgroundObserved` 来自 EntryAbility。它切到 DesktopAbility 时正常记录后台事件，不能当作游戏 App 被切后台。四个成功样本的 DWA 日志均记录桌面进入前台，保留日志中未出现桌面后台事件；第二轮另通过 `aa dump` 和窗口焦点核对 DesktopAbility 前台。后续 runner 分别记录两个 Ability，并保存当前 App Ability 状态。没有计时中截图，因此这次不替代完整显示矩阵或实际 GPU viewport 裁剪验收。

## 接下来测什么，怎样决定投入

1. 取得这次程序初始化超时的证据并解决稳定性问题，再完成每条路径至少六轮的固定 60 FPS ABBA；报告全部失败和样本离散度。
2. 用 commands profile（1000 draws、轻像素负载）放大 CPU/命令开销；用 fill profile（100 draws 完整区域 overdraw）检查 GPU 受限时的吞吐和帧时间。无上限比较先补齐刷新/输出调度条件，不能只比游戏提交次数。
3. 用一个真实 x64 D3D11 游戏的固定片段验证相关性，保持同版本、画质、分辨率、存档和路线；ARM64 基准作为区分 FEX 成本的辅助对照。
4. 最后补十分钟长测、资源增长和输入/画面回归。有可靠功率计量时再评价能耗，不能由 CPU 或电池百分比直接推断。

建议真实游戏 FPS 提升至少约 10%，或同等输出工作下 CPU 成本降低至少约 15%，且 P95/P99 无持续退化，作为扩大默认覆盖的初始工程门槛。当前数据未达到这个收益门槛；保留 Direct 为可选路径，继续定位受益场景，不提前切换默认后端。

原始 CSV、两份 Native 快照、全部结果及失败见 [evidence](evidence/direct-performance-pad-20260929/manifest.json)。固定帧率复核见 [review.json](evidence/direct-performance-pad-20260929/fixed60/review.json)；完整方案见 [性能验收方案](direct-performance-gate-20260929.md)。

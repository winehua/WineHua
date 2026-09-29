# 平板 Direct 性能验收方案（2026-09-29）

目前的双窗口、resize、fence、透明遮挡、触摸与冷启动测试证明共享 GPU 图像链路成立，**没有证明显著性能收益**。游戏帧路径不再编码、传输和重放 Vulkan 命令，并不自动等于 FPS 大幅提高；FEX/DXVK、GPU 负载、BufferQueue 等待与 HAP 合成仍可能限制吞吐。viewport 裁剪是显示正确性工作。

## 对照对象与固定条件

先用已连接的 MatePad Mini 和可运行的 DXVK 1.10.3。手机 2.6.2 的验证保持搁置，不改变 Steam/FEX。

本轮核对了旧 payload 的 PE header：`x64/winehua_d3d_switch_cube.exe`、`winehua_d3d_resize_cube.exe` 和 `winehua_direct_desktop_scene.exe` 均为 `0xaa64`（ARM64 PE）。`x64` 目录和原 JSON 的 `peArchitecture=x64` 是 64 位槽标签，不证明 AMD64/FEX 覆盖。它们的显示结果继续有效，但不能拿来推断 x64 游戏性能。新 benchmark 显式使用 `llvm-x86_64` 生成 `0x8664` AMD64 PE，同时用 `llvm-aarch64` 生成 ARM64 对照。

在同一个 signed HAP、相同 Wine/runtime/prefix、同一测试 EXE、同一 FEX 和 ARM64X DXVK 上比较：

| A：兼容路径 | B：Direct 路径 |
| --- | --- |
| Guest Venus → vtest/Host Vulkan → 当前 EGL 桌面输出 | Guest 系统 Vulkan → 共享 NativeBuffer/fence → HAP Vulkan 桌面输出 |

两边固定客户区、逻辑桌面及实际输出尺寸、Present 参数、窗口布局、纹理/绘制内容、屏幕亮度、刷新率、电源条件。用新 App 会话切换后端，核对各自 PID 的实际 renderer 日志与 DLL/EXE hash，不能只看 Want 请求参数。这个对照测的是**整条显示架构**的收益，包含合成器变化；不能把全部差值归因于 Vulkan 命令协议。

每次先做同场景预热，性能计时排除 prefix 初始化、shader/pipeline 编译与首次资源导入。按 ABBA 顺序交错运行，至少各 5 个有效样本；建议每轮预热 15 秒、测量 60 秒。记录前后设备温度/系统热状态与是否前台，切后台、锁屏、过热或后端身份不符的样本保留并标无效。两边都关闭逐帧日志、截图和诊断读回；截图只在测量前后用于正确性核查。

## 场景矩阵

| 场景 | 用途 |
| --- | --- |
| 轻 GPU、较多 draw/state/resource 操作的固定场景 | 放大 CPU 与协议开销，判断去掉 Venus/vtest 的潜在收益；保持资源复用，另列上传压力场景 |
| 相同分辨率、较重 GPU 的场景 | 确认 GPU 受限时 Direct 没有引入帧时间或合成退化 |
| 固定 60 FPS 场景 | 比较完成同样工作需要的 CPU 时间及能耗，避免 FPS 封顶掩盖收益 |
| 双窗口并行与半透明 UI | 评估 HAP Vulkan 合成和共享队列的成本，以及降低标题栏/GDI 更新频率的收益 |
| 一个真实 x64 D3D11 游戏的可重复片段 | 决定对实际游戏是否值得继续投入；合成 cube 不能代替游戏结论 |

现有 cube 有每轮 `Sleep(1)`，会更新标题栏，JSON 的帧数没有稳态帧时间分布。因此它适合显示回归，不能直接用 `frames / --seconds` 宣称精确性能。正式基准应记录每帧 QPC，排除预热后输出实际计时区间、有效帧数、P50/P95/P99、超过 16.67/33.33 ms 的比例和 Present 调用耗时；不要把 CPU 帧时间叫 GPU 执行时间。

无帧率限制的微基准应去掉主动 sleep，冻结标题栏和 GDI 刷新。固定 60 FPS 测试则显式使用相同限帧器，单独统计限帧等待。绘制次数、资源更新量、窗口尺寸均须写入结果，不能为 Direct 单独降负载。

## 需要采集的数据

- 平均 FPS 与 P50/P95/P99 帧时间、长帧比例、Present 阻塞时间；同时记录 HAP 的消费/提交帧数，区分游戏 Present 次数与真正输出的帧。
- 游戏进程、HAP、相关 Wine/图形服务的 CPU 时间增量，除以有效帧数；记录进程组总 CPU ms/frame 与主要进程占比。系统服务若无法按权限采集，明确标缺失。
- App/游戏 RSS 起终值与峰值、NativeBuffer import/reuse、GPU fence 等待与输出重建次数；另外做 10 分钟持续运行与 100 次生命周期循环。
- 同场景固定 FPS 的能耗/热状态（有可靠平台 profiler 或功率数据时）；不能用电池百分比的短时变化或 CPU 用量推断精确功率。

`gameCpuReadBytes=0/gameCpuUploadBytes=0` 当前是代码路径断言，不是硬件流量计数。fd/CPU/GPU counter 若权限拒绝应写 `unavailable`，不能填 0。性能测量时不要同时运行高频 HDC/截图观察器；优先进程内累计和低频平台采样。

## 是否继续投入的门槛

建议把 **FPS ≥10% 提升，或固定 FPS 下进程组 CPU ms/frame ≥15% 降低** 作为继续扩大 Direct 默认覆盖的初始工程门槛，并要求 P95/P99 无超过 5% 的持续退化、长测无资源增长与画面/输入回归。这些是建议的验收阈值，不是对结果的承诺；需要与样本噪声和用户常用游戏结合判断。

报告所有样本与中位数/离散度，提升若接近样本波动则延长或增加样本，不选最快一轮。只有微基准通过而游戏无收益时，保留 Direct 为可选后端，明确受益场景；不为了架构纯度提前切默认，也不引入游戏图像 CPU 回读 fallback。

当前顺序：GPU sampling 与稳态基准已编译并安装；先完成新包显示回归和真实 AMD64/FEX 工具检查，再做固定 60 FPS 的 ABBA 成本对照，随后扩展命令压力、GPU 压力和真实游戏。进程组统计仍需补齐；先在平板上得出 Direct/Venus 的 A/B 数据，再评估后续 Zink、Audio 和 Input 的投入。

## 已实现的基准与边界

`winehua_d3d_benchmark.exe` 独立复用 cube 渲染代码，关掉主动 `Sleep(1)` 和每秒标题刷新，保留初始窗口创建。先预热 15 秒，再测 60 秒；记录每次成功 Present 后的 QPC 间隔、Present 调用耗时，以及 `GetProcessTimes` 的游戏进程 CPU 时间。逐帧记录只存在内存中，结束测量后写 CSV；没有游戏图像读回。

四个可重复 profile：light（1 draw）、commands（1000 draws，交替 2×2 scissor）、fill（100 draws，完整区域与 always-depth 形成 overdraw）、fixed60（100 draws，小 scissor，显式 60 FPS 限帧）。这些是合成工作量，不是游戏结论。JSON 的 `averageFps` 表示游戏成功提交 Present 的吞吐，不能叫物理显示 FPS。

HAP 的 `captureNativePerformance()` 在两次边界观察时读取 CLOCK_PROCESS_CPUTIME_ID、CLOCK_MONOTONIC、getrusage high water 和 accepted-present 计数。外部 `/data/local/tmp` 的 sysconf ELF 被设备权限拒绝，不再用猜测的 tick 频率；Native API 自身返回 sysconf，CPU 计量直接用纳秒时钟。Venus/EGL 计数要求实际接收新的 zero-copy GPU frame；Direct 计数要求成功提交的画面含 Direct 图像。它们证明 HAP 接受 present，不证明物理扫描输出。

SmokeRunner 每 500 ms 观察一次测量 boundary，因而 HAP 的窗口与游戏计时区间有至多约一个轮询周期的偏移，结果保存两个原始快照与实际观察时长。校验器检查同 PID、计时区间、实际 compositor 和非零 GPU presents，HAP CPU/game frame 仅作为带边界误差的估计。Wine server/系统图形服务 CPU 与能耗仍未覆盖，不能宣称进程组总 CPU 或精确功率收益。

另发现 EGL 会请求 NativeVSync 60..120 / expected=120，而 Direct 当前用 FIFO pacing。无上限 profile 的吞吐差值会包含这部分调度差异，不能全归因于命令协议；必须核对 accepted-output FPS 与屏幕刷新状态。先做 fixed60 的同工作量成本对照，并在扩大吞吐结论前补齐匹配刷新条件。

`automation/checks/benchmark.py` 从原始 CSV 重算帧数、实际计时、FPS、P50/P95/P99、长帧比例，校验 HAP telemetry；PASS 只表示测量数据完整。`automation/run_direct_performance.py` 可用 `--plan-only` 输出准确命令，正式默认 3 个 ABBA block（各 6 轮），`--sanity --profile light` 只跑两次短工具验证。每次安装同一指定 HAP、推同一完整 payload、停止 App 并确认该 UID 进程为空后切换 backend，全程复用 prefix，结束恢复熄屏设置。

当前准备状态与身份记录见 [基准准备](direct-performance-preparation-20260929.md)。真实 AMD64 工具检查及第一组 fixed60 ABBA 已运行，计划的十二轮在第五轮超时中止。首组没有显示两进程 CPU 成本节省，尚无显著速度提升结论，见 [初步数据与失败](direct-performance-results-20260929.md)。

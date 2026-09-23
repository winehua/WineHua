# 性能分析

> 适用场景：遇到卡顿、掉帧、画面滞后时；想知道某个改动对性能的影响时。
> 最后核实：2026-09-22
> 相关代码：`entry/src/main/cpp/common/perf_utils.h`（帧级诊断门）、`thirdparty/mesa/src/virtio/vulkan/vn_ring.c`、`thirdparty/virglrenderer/src/venus/vkr_queue.c`
> 相关文档：[observability.md](observability.md)（日志通道）、[../architecture/graphics-matrix.md](../architecture/graphics-matrix.md)（图形链路）

## 性能统计开关

### 帧级诊断总开关

`WINEHUA_FRAME_TRACE=1` 打开后，下面这些帧级日志才会输出（关掉时零开销）：

| 日志 | 内容 |
|---|---|
| `[GL-TAKE]` | 一次合成的分段耗时（6 段），定位瓶颈段用 |
| `[MW-SWAP]` | 单帧的取帧与交换耗时 |
| `[MW-TAKE]` | 取帧结果（尺寸、子表面数量） |
| `[DBG-CPU]` | CPU 路径是否在使用 |

**注意**：这个开关在进程启动时读一次，运行中改无效。

另有 `VKR_WINEHUA_SHADOW_TRACE=1` 作为运行期可设的兼容通道。

### 常开的分位统计

`[GL-PERF]` 不需要开关，每积累 120 个样本自动输出一次：取帧 / 上传 / 交换 / 总耗时的 50/95/99 分位值、帧率、上传字节数。**这是最常用的性能数据**。

### 各组件统计

| 组件 | 开关 | 输出 |
|---|---|---|
| 宿主渲染器（virglrenderer） | `VKR_WINEHUA_PERF_SUMMARY=1` | 提交数、影子内存拷贝量、六个阶段的耗时 |
| guest Mesa（Venus 命令提交） | `VN_WINEHUA_PERF_SUMMARY=1` | 提交热路径各环节耗时，按命令类型分类 |
| guest Mesa 写文件 | `VN_WINEHUA_PERF_LOG=<路径>` | 上面统计写入文件（默认在下载目录） |
| Venus 呈现阶段 | `WINEHUA_VKR_PRESENT_STAGE_TRACE` | 呈现各阶段跟踪 |
| GPU 侧耗时 | `WINEHUA_VENUS_GPU_FRAME_PROFILE=1` | 给命令缓冲加 GPU 时间戳，120 帧抽样一次 |

### 影响性能的行为开关

这些不是统计开关，但和性能直接相关，排查时要知道它们的存在：

- `VN_WINEHUA_DIRECT_FENCE_WAIT` / `VN_WINEHUA_EVENT_FENCE_WAIT`——fence 等待的三种模式。
- `VN_WINEHUA_REMOTE_MEMORY_SYNC` / `PERSISTENT_MAP_SYNC`——内存同步通道（这套开关有已知的两难问题，见 [../architecture/graphics-matrix.md](../architecture/graphics-matrix.md)）。
- `WINEHUA_VENUS_PRESENT_MODE`——呈现模式（mailbox / fifo）。

## 卡顿怎么定位

按"先分段、再分侧、再分层"三步走。

### 第一步：看时间花在哪一段

打开 `WINEHUA_FRAME_TRACE`，看 `[GL-TAKE]` 的六段耗时。典型的例子：某次排查中"合成"段占了 70-85 毫秒、而其他段都是几毫秒——瓶颈直接定位到 CPU 合成。

### 第二步：CPU 还是 GPU

- **CPU 侧**：看 `[DBG-CPU]`（CPU 路径在跑）和合成段耗时（CPU 合成是纯软件绘制，耗时大就是它）。
- **GPU 侧**：用 `WINEHUA_VENUS_GPU_FRAME_PROFILE` 的 GPU 时间戳，或看宿主渲染日志里的呈现次数。

### 第三步：宿主还是 guest

这一步最关键，判断错了方向会白查很久：

| 现象 | 说明 |
|---|---|
| **帧提交日志（MW-COMMIT）断流** | guest 不再提交帧了——问题在 Wine 那边，宿主合成是正常的 |
| 宿主渲染日志停更 + guest 进程还活着（有 CPU 占用） | 渲染线程卡死，guest 提交不出来 |
| 输入链路很快（1-2 毫秒）但画面滞后 | 滞后在画面的处理或呈现，不在输入 |
| wineserver 进程 CPU 占用很高 | 引擎在打 wineserver（见下面的案例） |

**看 wineserver 的 CPU 占用**是一个很有用的手段：在冻结发生时，隔几秒采一次 `/proc/<pid>/stat`，如果 wineserver 在几秒内烧掉接近一秒的 CPU，说明它在被某个客户端狂打。

## 对比实验怎么组织

### 先确认开关真的生效

这是踩过的坑：曾经用环境变量做 A/B 对照（开日志 vs 关日志），结果**两次实际都是关的**（那个环境变量下发不生效），把随机抖动误判成了"日志的影响"。

**所以做对照实验前，先确认两边真的不一样**——看日志里有没有出现预期的新内容。现在自动化测试工具会拒绝这类无效的环境变量（`smoke.py` 里有检查）。

### 增量测试法

与其对着一个大 diff 分析，不如**逐个提交单独部署实测**。配合分层诊断日志（界面层 → NAPI → 原生决策 → Wine），每一步都能看到变化，比一次性对比快得多。

### 负向对照

验证一个修复是否真的起作用时，**把修复的开关关掉，看问题是否复现**。如果不复现，说明你的"修复"可能只是掩盖了问题，或者有别的东西在兜底。

### 验证"画面对"不能只看帧数

帧数通过不代表画面正确。验证渲染问题时，判定条件要包含**画面内容维度**（比如非背景像素占比、画面是否在变化），不能只看"有帧输出"。

## 案例：一次卡死是怎么定位到 wineserver 的

**症状**：某游戏启动后偶发整个桌面冻结，表现多样（窗口重建后只画一帧、反复重试、桌面进程空转）。

**排查过程**：

1. 先看帧日志——帧提交（MW-COMMIT）断流了，说明 guest 不再提交。
2. 看进程现场——guest 进程都活着，宿主合成循环也在跑（不是死循环）。
3. 采样各进程 CPU——**wineserver 在 3 秒内烧掉约 1.9 秒 CPU**，明显异常。正常情况 wineserver 应该基本空闲。
4. 确认机制：游戏里有一个"拿锁-立即释放"的高频轮询线程（Windows 上也是这样），每一圈都要和 wineserver 往返一次通信。wineserver 是单线程处理请求的，被这个高频请求占满后，其他线程的请求（包括渲染和输入）全部排队等待——表现出来就是整个界面冻结。

**定位手段**：wineserver 自己带跟踪参数（`-d1`），打开后能打印每个请求，是分析请求风暴的放大器。跟踪日志每行的行首标识是客户端线程，可以和系统线程号交叉对照。

**教训**：这类问题的可靠判据是"帧提交断流 + wineserver CPU 占用高"，不要靠"点菜单看有没有反应"这类人工探测（不可靠）。

# 性能分析

> 适用场景：遇到卡顿、掉帧、画面滞后时；想知道某个改动对性能的影响时。
> 最后核实：2026-09-24
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

打开 `WINEHUA_FRAME_TRACE`，看 `[GL-TAKE]` 的六段耗时。正常时各段都在几毫秒量级，**哪一段明显高出就说明瓶颈在那**——比如合成段比其余各段高一个数量级，瓶颈就在 CPU 合成。

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

## 系统级采样（hiperf）

前面那些开关都要改代码或设环境变量、还得重启进程才生效。设备自带的 `hiperf` 什么都不用改，直接从外部采样，适合先摸底"到底是谁在烧 CPU"。

```bash
# 硬件计数器：cycles、IPC、分支预测失败、上下文切换
hdc -t <设备IP> shell "hiperf stat -p <pid> -d 3"

# 采样（-s dwarf 必须加，见下）
hdc -t <设备IP> shell "hiperf record -p <pid> -d 5 -s dwarf -o /data/local/tmp/perf.data"

# 热点函数排名
hdc -t <设备IP> shell "hiperf report -i /data/local/tmp/perf.data"

# 调用链树（每层带占比）
hdc -t <设备IP> shell "hiperf report -i /data/local/tmp/perf.data -s"
```

**`-s dwarf` 是关键**：不加它只采到热点函数（`comm` 列是线程名，能看出哪个线程占 CPU）；加上它，`report -s` 会输出**从根一路展开到具体函数的调用链树**，每一层都带占比——这才是定位"时间花在哪个调用路径"的东西。

实测输出长这样（一次音频写入路径的采样）：

```
22.22%  OS_AudioWriteCB  37576  ld-musl-aarch64.so.1  write
  |- 99.00% libaudio_stream_client.z.so+0x65200
            OHOS::AudioStandard::RendererInClientInner::WriteCallbackFunc()
      |- 36.19% WaitForBufferNeedOperate()
          |- 94.10% OHAudioBufferBase::WaitFor(long, ...)
              |- 47.48% libaudio_common.z.so+0x79450
                  |- 93.44% CheckBufferNeedWrite()
                      ...
```

符号是**混合**的：有全局符号的函数能显示名字（我们自己的 `libentry.so` 里能出 `IsProcessAliveNotZombie(int)`），静态的、被 strip 掉的只剩 `libentry.so+0x141f04` 这种偏移。要全部还原得用 `--symbol-dir` 指向带符号的库。

**限制**（都实测过）：

- `-p <pid>` **只对 debug / profileable 应用有效**。采 shell 进程报 `-p option only support debug or profileable application`；应用市场版（release 签名）同理不行。
- `-a`（全系统采样）需要 root，普通 shell 报 `-a option needs root privilege`。
- **完全空闲的进程采不到样本**（`Samples Count: 0`）——采样是按 CPU 事件触发的，没有事件就没有样本。

采样文件落在 `/data/local/tmp`，用完记得删。

**卡死排查怎么用它**：进程卡住时，采样点会全部落在同一个调用链上，所以 3~5 秒的采样就等价于"卡住那一瞬间的栈"。再看哪一层占了 100% 就知道卡在哪。这是设备上唯一免改代码、能拿到运行中进程调用栈的办法，详见 [observability.md](observability.md) 的"运行中进程的调用栈"。

## 对比实验怎么组织

### 先确认开关真的生效

环境变量在我们的下发通道里不一定真的生效。开关没能落到目标进程时，"开"和"关"两次对照跑的其实是同一套配置，差别只是抖动——很容易被当成"日志带来的差异"。

**所以做对照实验前，先确认两边真的不一样**——看日志里有没有出现预期的新内容。现在自动化测试工具会拒绝这类无效的环境变量（`smoke.py` 里有检查）。

### 翻译器侧（box64）的开关

box64 自己的 dump 开关（`BOX64_DYNAREC_DUMP` / `BOX64_DYNAREC_DUMP_RANGE` / `BOX64_DYNAREC_LOG`）和调档位 A/B 的手法（换 `BIGBLOCK` 看故障地址是否改变），见 [fault-forensics.md](fault-forensics.md) 第 6 节。

### 增量测试法

与其对着一个大 diff 分析，不如**逐个提交单独部署实测**。配合分层诊断日志（界面层 → NAPI → 原生决策 → Wine），每一步都能看到变化，比一次性对比快得多。

### 负向对照

验证一个修复是否真的起作用时，**把修复的开关关掉，看问题是否复现**。如果不复现，说明你的"修复"可能只是掩盖了问题，或者有别的东西在兜底。

### 验证"画面对"不能只看帧数

帧数通过不代表画面正确。验证渲染问题时，判定条件要包含**画面内容维度**（比如非背景像素占比、画面是否在变化），不能只看"有帧输出"。

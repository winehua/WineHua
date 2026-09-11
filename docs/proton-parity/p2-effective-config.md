# P2 收尾：FEX 生效配置落地与 SHM 统计实测

> 执行：2026-09-11（第二轮真机验证）
> 结论：**FEX 配置文件已被真实读取并生效**；SHM 统计在设备上创建成功。P2 的可观测性目标达成。

## 1. 为什么要改 app 代码

FEX 的配置**只能由环境变量定位**，共三条路（`Source/Common/Config.cpp:656`）：

```text
FEX_APP_CONFIG_LOCATION   → 配置目录 (最终读 <dir>/Config.json)
FEX_APP_CONFIG            → 直接指定 Config.json 文件
无覆盖时                  → $WINEHOMEDIR/.fex-emu/ 或 $WINEHOMEDIR/.config/fex-emu/
```

PE 侧 `GetHomeDirectory()`（`_WIN32` 分支）取的是 `WINEHOMEDIR` / `LOCALAPPDATA`，
都在 Wine prefix 里（`C:\users\100\...`），而 prefix 在应用沙箱内、hdc 不可写。
所以**必须由 app 侧设置环境变量**，这也符合方案"prefix / 配置由平台层负责"的边界。

顺带纠正一个此前的假设：`ENABLE_FEXCORE_PROFILER` 只控制 **Tracy 时间线剖析**
（`FEXCore/Source/Utils/Profiler.cpp`），**不是** SHM 统计的开关。
SHM 统计由配置项 `ProfileStats` 驱动（`Source/Windows/{ARM64EC,WOW64}/Module.cpp`
里的 `if (IsWine && ProfileStats()) StatAllocHandler = ...`）。

## 2. 本次实现

| 文件 | 变更 |
| --- | --- |
| `FEX_Config.json`（新增，仓库根） | Proton 参考配置：`ProfileStats=1`、`X87ReducedPrecision=1`、`TSOEnabled=1`、`VectorTSOEnabled=0`、`MemcpySetTSOEnabled=0`、`HalfBarrierTSOEnabled=1`、`MaxInst=500`、`Multiblock=1` |
| `scripts/assemble.sh` | 把 `FEX_Config.json` 打进运行时包 `share/fex-emu/Config.json` |
| `entry/src/main/cpp/proc/wine_child.cpp` | `reassert_arch_wine_runtime_env()` 内设 `FEX_APP_CONFIG_LOCATION=<binDir>/../share/fex-emu/`（文件存在才设，`overwrite=0` 保留上层覆盖能力），并把该变量加入日志 |

## 3. 真机结果

HAP：`418 977 859 B`，`sha256 a83e826c3447c567e6103859f63904a8cd397b1b20740f7a1a870e0749dd1cc5`，
`payloadSha256 ddce2eb2…77802c`。

```text
# FEX 配置被读取 → SHM 统计被创建（UnixLib 路径）
[WINEHUA-FEX-PROBE] shm-stats name=fex-21806-stats map=4096 max=4194304 base=0x6ffca85000
[WINEHUA-FEX-PROBE] shm-stats name=fex-21854-stats map=4096 max=4194304 base=0x6ffca85000

# UnixLib 仍被加载
[WINEHUA-FEX-PROBE] unixlib-dotso loaded pid=21806
[WINEHUA-FEX-PROBE] unixlib-dotso loaded pid=21854

# 内核能力结论不变
[WINEHUA-FEX-PROBE] hardware-TSO enable get=0xffffffff already errno=22 result=NOT_SUPPORTED
[WINEHUA-FEX-PROBE] unaligned-atomic flags=0x3 ret=-1 errno=22 result=NOT_SUPPORTED
```

`shm-stats` 这一行只在 `ProfileStats` 生效时才会出现：它证明配置**确实被 FEX 读到**，
并且 `shm_open` / `ftruncate` / `mmap` 全部成功（`map=4096` 是我们传入的页大小，
`max=4194304` 是预留的可增长窗口）。

同时 `HODLL=libwow64fex.dll` × 29，说明 32 位默认也走 FEX 了（见 §4）。

## 4. 顺带并入的一项本地产出

原工作树 `feature/arm64-heaven-port` 里有一处未提交改动（`wine_child.cpp`），
内容是把 **32 位默认引擎从 wowbox64 切到 FEX**（`apply_wow64_cpu_dll()`，
`WINEHUA_WOW64_ENGINE=box` 才退回 wowbox64），并放开 `FEX_*` 环境变量透传日志。

第一轮我们用**干净基线**出包时把这处行为 regress 回了 `wowbox64.dll`（探针 0 次命中），
这也反过来证明了这处改动是当前运行时的真实行为。现已在 parity 分支原样并入
（`git apply` 自原工作树 diff，未改动原工作树）。

## 5. 仍未完成

| 项 | 状态 |
| --- | --- |
| SLOT 扩容 (`shm-grow`) | 未触发。容量 1 页 = (4096-64)/80-1 = 49 槽，本次运行线程数远小于它。代码路径存在（探针已埋），需要一个多线程负载才能观察 |
| 退出清理 (`DeleteSHMStatsFile`) | 未观察。进程多为被杀而不是正常退出 |
| 生效值逐项导出 | 只能证明配置文件被读取 + 统计头自洽；`MaxInst=500` 等键的进程内实际取值还没有导出手段 |
| `ENABLE_FEXCORE_PROFILER` / Release 对齐 | 未做；已确认与 SHM 统计无关，属独立优化项 |

## 6. 统计内容读取（已打通）

`/dev/shm/fex-<pid>-stats` 在应用私有命名空间，hdc shell 读不到（`Permission denied`）。
改在 FEX 侧读：`StatAlloc::StatAlloc` 在 `SaveHeader()` **之后** dump 统计头，
`StatAllocBase::AllocateSlot` dump 前两次线程槽登记。

输出通道也是踩出来的：PE 侧的 `stderr` / `write(2)` **不可见**——Wine 在进程启动时
就缓存了 PE std handle，而 NCP 子进程是在其后才把 unix fd 2 重定向到 wine_stderr 日志，
所以只有 UnixLib（raw `dprintf(2)`）写得到日志。最终 PE 侧改为写 Wine 文件系统里的
`C:\windows\temp\winehua_fex_probe.txt`，hdc 直接可读。

### 真机结果

```text
[WINEHUA-FEX-PROBE] shm-header version=2 app_type=3 slot_size=80 capacity=4096 size=4096 fex_version=FEX-2604-99-g86ff33b
[WINEHUA-FEX-PROBE] shm-slot alloc tid=304 slot_off=64 head_off=64
[WINEHUA-FEX-PROBE] shm-slot alloc tid=308 slot_off=144 head_off=64
[WINEHUA-FEX-PROBE] shm-slot alloc tid=320 slot_off=64 head_off=64
[WINEHUA-FEX-PROBE] shm-slot alloc tid=324 slot_off=144 head_off=64
```

逐项核对（对照 `FEXCore/include/FEXCore/Utils/SHMStats.h`）：

| 字段 | 实测 | 期望 | 结论 |
| --- | --- | --- | --- |
| `Version` | 2 | `STATS_VERSION = 2` | ✅ |
| `app_type` | 3 | `WIN_WOW64`（枚举 0..3） | ✅ 32 位链路 |
| `ThreadStatsSize` | 80 | 4+4+9×8 = 80 | ✅ 结构与读取方一致 |
| `capacity/size` | 4096 | `FEX_PAGE_SIZE` 一页 | ✅ |
| `fex_version` | `FEX-2604-99-g86ff33b` | 构建时写入 | ✅ 版本可反查 |
| 槽布局 | `slot_off=64` → `144` | header 64B，槽步长 80B | ✅ 链表头/第二槽均正确 |

> `fex_version` 一开始是空的：FEX 从 `git_version.h` 取，而 Proton 用
> `.git_describe/.git_rev` 传 `-DOVERRIDE_VERSION/-DOVERRIDE_HASH`。补上后发现
> 容器内 git 因 "dubious ownership" 拒绝执行（root 构建、源码属主非 root），
> 需要 `-c safe.directory=*`；且**拿不到真实版本时必须不传**，否则 FEX 的
> `git_version.h` 生成器会把 `unknown` 逐字节拼成 `0x??` 常量，直接编译失败。

### 产物

| 产物 | SHA-256 |
| --- | --- |
| `libarm64ecfex.dll` | `bfe47099d6d61389bc1a0e3d8cd10394f23c2c979e6e5d06600c813769b447ee` |
| `libwow64fex.dll` | `43a76838d5e73cb8809bb183f65cb3e4292acf85b0da3c4b25f9cc02cd2f7edd` |
| HAP（已装到 MLR-AL10） | `85c62630d008c3ca31644b7ceac4901443b6820a9223d031c01d84a02f93a367`（418 981 995 B） |
| HAP payloadSha256 | `2be7af51a2ae16f0df2f8324b86f065ff8a85d94d314e2899f7db2f0449768d5` |

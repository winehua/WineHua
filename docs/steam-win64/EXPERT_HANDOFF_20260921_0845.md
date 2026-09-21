# Steam win64 / OHOS 专家交接（2026-09-21，设备快照截至 08:43:47 CST）

本文接续 [2026-09-20 原交接](F:/WineHua/proton-ohos-worktree/docs/steam-win64/EXPERT_HANDOFF_20260920_2130.md) 和 [本轮逐次复核记录](F:/WineHua/proton-ohos-worktree/docs/steam-win64/FEX_NATIVE_FAULT_REVIEW_20260921.md)。遇到冲突，以本文的新实测和纠正为准。文件名中的 0845 是交接截点标识；整理期间只核对本地资料、构建环境与补丁，不继续构建、部署或设备实验。

## 0. 接手先看

**问题尚未修复。设备最后安装的是 v20；v21 链接失败，未完成打包、签名、部署。游戏已下载，但没有确认 Game.exe 实际启动，更没有确认游戏画面。**

- 原先修好的窗口绑定、接管、退役、消费者自愈继续保留。本轮没有新证据要求改动显示链。
- FEX 只读插桩已证明：观测到的 guest JIT 页由 tracker 正常认领，SMC detection 没被关闭。不要再按旧交接“三级链都不认领”的假设修 tracker，也不要恢复故障后猜 W/X 的权限互切。
- 一个明确修复已落地：Wine 信号诊断直接读取 FEX guard 栈会造成二次 SIGSEGV，现改为安全的内核读取。此修复没有解决全部 CEF 崩溃。
- 剩余现象必须分开追踪：**v18 的 V8 反优化帧校验断点、v20 的 IPC 超时后整组 CEF 终止、ProcessorMetrics 的退出清理崩溃**。最终 signal 11 不能代替首发异常。
- v19 的 jitless 对照中，主库 renderer 连续存活约 15 分钟并可切页；其他 renderer 和 Metrics 仍退出，启动选项弹窗仍黑，不能当产品修复。
- 下一项已准备的实验是 v21“返回目标缓存绕过对照”，应配 **v18 ntdll（无 JS 关闭后缀）**。当前 Wine 源码还是 v20 的 no-opt，不能直接 make 后声称恢复了正常 JIT。

## 1. 环境、设备与版本身份

| 项目 | 当前交接值 |
| --- | --- |
| Windows 根目录 / Shell | `F:\WineHua` / PowerShell |
| 开发树 | `F:\WineHua\proton-ohos-worktree` |
| WSL | `Ubuntu-22.04`，用户 `liufeng`，`/home/liufeng/src/WineHua-proton-ohos` |
| 分支 / HEAD | `feature/proton-wine-ohos` / `32efc9c709d19a973942f967e96cb062bd84f2f7` |
| 构建容器 / 挂载 | `wineohos-build` / `/data/src/winehua` |
| 无线 target | **`192.168.180.71:36875`**；还有其他 USB target，所有设备命令显式加 `-t` |
| Bundle / Ability | `app.hackeris.winehua` / `EntryAbility` |
| 设备应用物理根 | `/data/app/el2/100/base/app.hackeris.winehua` |
| 应用内部根 | `/data/storage/el2/base` |
| Wine prefix | 物理根下 `files/.wine`；运行时在 `files/wine` |
| 原始证据目录 | `F:\WineHua\device-evidence-20260920-fex-rwx` |
| 装机候选 | `F:\WineHua\artifacts\fex-rwx-v20-signed.hap` |

工作树有大量未提交修改、未跟踪补丁和工具。**不要 reset、clean 或用新的 staging 覆盖现有源码；不要删除用户游戏数据。** 普通 git status 会因坏掉的 dxvk submodule gitdir 报错；在 WSL 使用：

```bash
cd /home/liufeng/src/WineHua-proton-ohos
git status --short --ignore-submodules=all
```

### 1.1 最后设备快照（不是持续在线状态）

证据：[handoff-current-status.log](F:/WineHua/device-evidence-20260920-fex-rwx/handoff-current-status.log)，设备时间 **2026-09-21 08:43:47 CST**。

- 数据分区 453 GB，已用 390 GB，剩约 **63 GB**。用户清理后曾剩 109 GB，那是历史值，不能继续用作当前余量。本轮未执行设备文件删除。
- 宿主/broker PID **53235**，Steam 主进程 host PID **54407**，后继 browser host PID **56181**；renderer **57741** 在快照中仍存活。
- renderer **56347** 已在存活 **276705 ms** 后 signal 11；还有 renderer/gpu/utility 的退出与重建，不能因为列表中有进程就判稳定。
- `ps` 的 STIME/ETIME 显示有约八小时时差，不能用于跨日志精确排序。优先使用设备 `date`、CEF 日志时间和 `createMs/exitMs/lifetimeMs`。
- 下文无特别说明的进程号均为 **host PID**；FEX/Wine 日志内的 `pid/tid` 可是 Wine ID，须通过生命周期日志映射。

### 1.2 SHA256（完整值）

本地 v18–v20 signed HAP、stripped ntdll、FEX 和 libcef 已在整理时重新计算哈希，记录见 [handoff-artifact-checks.json](F:/WineHua/device-evidence-20260920-fex-rwx/handoff-artifact-checks.json)。v20 安装成功由 `v20-install.log` 佐证；没有将本地 HAP 的哈希冒充从设备重新提取的安装文件哈希。

| 对象 | SHA256 |
| --- | --- |
| v18 ntdll | `4fa7ccf3e9b0d576e697abb6e5a0379db0ef994197e241035c290b3bff0801bd` |
| v18 signed HAP | `efdd75af9173721c6a17cd4dab3d41fca688a61959bc4548781de74acf911391` |
| v19 ntdll | `0101519d81b335653d621e87d6f8d3cd2c17bca070f1fed712261f0a0c067026` |
| v19 signed HAP | `637fd9ff3883b18666c195b15564830b4740cb4a62db217ad7fb9c0c27ff3a13` |
| **v20 ntdll** | `685e7fd1500a2b30ebfa951148cfef8420eb08d7cd350be45c19d0fdb00a1449` |
| **v20 signed HAP** | `427e74de82e948646f8cc88f013876d924dd424d58bb72c4b0f9ee1bf4a8ae25` |
| v13–v20 FEX 只读探针 | `e68c312b5667e867484ed610ca035d8da7e8f6d0f6891b956100d87fc1ff51ed` |
| v13–v20 payload | `824faa3e23bd8006c9db76a365438a3ffd7c862f691a7af8495e6f417208ea21` |
| 设备原版 FEX | `8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc` |
| v12 原 payload | `b651fac62b107e0235fd06c2da8ea977ecde862c766cad0111f109a8c4269d1e` |

v20 打包报告 `artifacts/fex-rwx-probe/fex-rwx-v20-unsigned.json` 确认以上 payload/FEX；设备 manifest 最近已保存的核对见 `v18-cef-b.log`（约 07:57）。随后 v19/v20 只更换顶层 ntdll，打包报告中的 payload 相同；08:43 快照没有重新读取 manifest，接手需要时按 §7 复核。

## 2. 已确认修复，以及必须纠正的旧结论

### 2.1 原版 FEX 已有 ThreadTerm 补丁

`build/fex-ec/Bin/libarm64ecfex.dll` 经 llvm-strip 后，精确等于原装 FEX 的 `8aba586e…`。旧交接将未 strip/strip 的哈希差异误当成行为差异。**不能反向撤掉 ThreadTerm 的 context / get-context-right 补丁来制造所谓干净基线。** 原装基线和独立探针构建校验已写入 `scripts/build_fex_rwx_probe.sh`。

### 2.2 观测到的 guest SMC 被 FEX 正常认领

v13 只读插桩结果（Wine PID 1352，host PID 22475）：

```text
[FEX-RWX-TRACK] pid=1352 tid=1436 addr=0x6fbb680570 x=1 rwx=1 smc_disabled=0 query_base=0x6fbb680000 query_size=0x40000 query_write=1 vm_prot=0x20
[FEX-RWX-UNPROTECT] ... status=0x0 old=0x20
[FEX-RWX-CHAIN] ... callret=0 guard=0 tracker=1 handled=1 jit=1
```

Wine 的 `[SMC] result=not_mine` 在后续 FEX SEH 之前，不能视为最终未认领。这里证实了相应页的 interval 覆盖、SMC 开关和恢复链正常；这不等于证明 FEX 所有路径都正确。另有 `x=0 rwx=0`、`MEM_RESERVE/Protect=0` 的真实 guest read AV，不应对其盲目开放写执行权限。

### 2.3 信号诊断自身导致二次 SIGSEGV：已修复

FEX native SP 可以在 emulator 栈顶 guard 页。旧 `ohos_smc_dump_stack()` / `ohos_smc_attribute_stack()` 直接解引用该 SP，在处理第一次故障期间再触发 SIGSEGV，使后续 FEX 处理根本没有机会执行。

v14 起使用 `SYS_process_vm_readv` 读取到本地数组；失败/部分读取时只使用成功内容，保留 errno，不回退到直接解引用。

- 补丁：`patches/wine/0002-ntdll-ohos-signal-safe-stack-read.patch`。
- **已接入** `scripts/build_wine.sh`。
- `tools/steam-rwx/check_safe_stack_read.py` 已 PASS：可读页、跨 guard 部分读取、signal handler 中读取 guard、null/invalid/unmapped、errno。
- v14 后仍有 CEF 崩溃，所以该修复解决的是诊断器的二次故障，不是全部稳定性问题。

### 2.4 call-ret 尚未排除

旧 v4 只是分类日志零命中，没有真正禁用返回缓存做 A/B。v13 之后已经看到正常 `callret=1 handled=1`。因此“call-ret 已排除”证据不足。v21 准备的是保留栈记账、绕过缓存 host 返回目标的独立对照，尚无实机结论。

## 3. 三类故障与证据边界

### A. v18 renderer：V8 反优化帧大小校验失败

renderer **38401**，Wine tid **0488**，直接日志先出现：

```text
EXCEPTION_BREAKPOINT 0x80000003
guest RIP 0x6fecd9d83a = libcef.dll + 0x2e2d83a
上层返回地址 = libcef.dll + 0x32e6ba6
eax = 0xc8 (200), ebp = 0x70 (112)
```

随后才终止其他线程，最终生命周期报告 signal 11。首发异常是 CHECK 断点，不能直接把最终 SIGSEGV 当第一现场。

实机 `libcef.dll` 已保存于证据目录，219859608 字节，SHA256：
`c85bf94462b3c79baeac63823db52d15e116eead71ea414e5d3a112c59e434d3`。
CEF/Chromium **126.0.6478.183**，PE 时间戳 2024-07-15，imagebase `0x180000000`；未下载 PDB。

```asm
182e2d82c  lea eax,[rax+rcx*8]
182e2d82f  add eax,-16
182e2d832  cmp eax,ebp
182e2d834  je ...
182e2d83a  int3
182e2d83b  ud2
```

结合周围 `0xBEEDDEAD` frame marker、FrameDescription 分配及公开 V8 12.6 源码，匹配 `Deoptimizer::ComputeInputFrameSize()` 的 CHECK_EQ。**已证明这里的帧元数据/栈状态不一致，尚未证明哪条 FEX 翻译或其他环节造成它。**

证据：`v18-renderer-38401.log`、`v18-renderer-final-tail.log`、`libcef.dll`、`v8-deoptimizer.cc`。源码来源：[V8 12.6.228 deoptimizer.cc](https://raw.githubusercontent.com/v8/v8/12.6.228/src/deoptimizer/deoptimizer.cc)；是辅助比对，不能冒充此 CEF 构建的精确 PDB 符号。

### B. v20：IPC 超时后整组 CEF 终止

`v20-args-and-copy.log` 已确认实际 Windows/CEF 命令行有 **`--js-flags=--no-opt`**。不能因为另一个 arg-verify 文件没有命中，就说参数未生效。

第一组进程退出：

| 角色 / host PID | lifetimeMs | exitMs | 结果 |
| --- | ---: | ---: | --- |
| browser 54557 | 266674 | 1789951090545 | signal 11 |
| gpu 54590 | 264706 | 1789951090562 | signal 11 |
| renderer 54744 | 260162 | 1789951090583 | signal 11 |

`console_log.txt` 先在 **08:37:58** 出现：

```text
Timed out waiting for mutex in PutInternal()
src\common\html\chrome_ipc_client.cpp (1493) : Failure pushing command to webhelper
```

browser 和 renderer 的直接日志尾部是 `[QUIT-PROBE] signal-enter / abort-thread / pthread-exit-wrapper`。完整 renderer 导出没有 v18 那个 fatal breakpoint；后期 dispatch_exception 为媒体相关 DBG_PRINTEXCEPTION。browser 尾部的 `RtlCaptureStackBackTrace` 也不足以归因。

**结论：no-opt 在当前配置下没有通过稳定启动/操作验证；这次先记录到的是 IPC 超时，不能认定复现了同一个 V8 断点，也不能将 IPC 超时本身认定为已查明根因。** 后继 browser 56181 的 renderer 56347 又退出，见最后快照。

证据：`v20-exit-order.log`、`v20-console-tail.log`、完整 `v20-renderer54744.log`（1934442 字节）、`v20-renderer54744-tail.log`、`v20-browser54557-tail.log`。

完整 browser 直接日志已导出到设备的 `files/.wine/drive_c/windows/temp/wine-native-54557-export.log`；本地目前只保存尾部。若继续追这类故障，优先取回完整导出，不要把尾部当作完整首发现场。

### C. ProcessorMetrics：退出清理仍崩溃

v15 **8821**：TEB `0x150000` 确实提交成功，多轮 TLS clear 成功；进入 `RtlExitUserProcess(status=0)` 后，清 index `0x0a` 时读取 `0x15180c` 出现 SEGV_MAPERR。按 ntdll ELF **PT_LOAD 虚拟地址**解算，定位 `virtual_clear_tls_index()` → `get_wow_teb()`，不是仅减文件偏移。

v16 在保留索引合法性检查后增加 `if (process_exiting) return STATUS_SUCCESS;`，跳过退出阶段对已终止线程的 TLS 遍历。补丁 `0004-ntdll-skip-dead-thread-tls-during-shutdown.patch` 和 `check_tls_shutdown.py` 已存在，测试 PASS，但**未接产品构建，也没有消除全部 Metrics 退出故障**。

v17/v18 直接日志最后到 `RtlExitUserProcess(status=0)`、`NtTerminateProcess(NULL)` 的 `self=1`；没有最终 FAULT-MIN，也没到 `[EXIT-PROBE] phase=libc-exit`。因此不能声称 libc finalizer 有问题。v18 改用 raw rt_sigaction 的对照仍崩，不能把它当产品修复。

后续复现：v19 **44625** / 1412 ms；v20 **55007** / 1522 ms、**56913** / 1501 ms，均 signal 11。应独立追退出链；不能直接据此解释库 renderer 首发故障。

## 4. v13–v21 进展与游戏状态

| 版本 | 主要变化 | 已知结果 |
| --- | --- | --- |
| v13 | 原行为基线上的 FEX 只读插桩 | SMC 正常认领；发现 Wine 诊断二次故障 |
| v14 | 安全 stack reader | 可进入登录/主界面；库操作和下载/启动时仍有 CEF 退出 |
| v15 | FAULT-MIN / TEB 临时探针；maps 非阻塞保护；移除 W/X 互切 | 明确 Metrics 的退出 TLS 访问；renderer 仍退出 |
| v16 | 退出 TLS guard；修正真正 Windows CommandLine 的诊断参数 | 参数生效；Metrics 和 renderer 仍崩 |
| v17 | 显式诊断时 direct stderr 与 EXIT-PROBE；Want 参数工具 | 避免 pipe 丢尾误判；Metrics 首因仍未补齐 |
| v18 | 显式诊断时 raw rt_sigaction、QUIT-PROBE、更高 FAULT-MIN 配额 | Metrics 未解；renderer 首发 V8 帧校验已定位 |
| v19 | 相对 v18 仅加 `--js-flags=--jitless` | 主库约 15 分钟可用；仍有其他退出和黑色启动弹窗 |
| **v20 装机** | 相对 v19 将后缀改为 `--js-flags=--no-opt` | 已生效；IPC 超时后整组 CEF 退出，后继 renderer 继续退出 |
| **v21 未部署** | FEX 返回目标缓存绕过；计划配 v18 ntdll | 三处源文件对象编译通过；链接失败，无成功候选 |

v19 browser **44179**、主库 renderer **44384**（08:09:51 创建）到 **08:25:15** 仍存活，约 **15 分 24 秒**。其间库主页、游戏详情切换正常。但另一个 renderer **46106** 在 61328 ms 后 signal 11，尾部是线程终止，没有同一个 V8 断点；Metrics 也仍崩。jitless 只能作为诊断参考。

游戏是 **At Home Alone Final / 独自在家，AppID 1740100**，安装路径：

```text
C:\Program Files (x86)\Steam\steamapps\common\At Home Alone Final\Game.exe
```

这是 **32 位 RPG Maker VX Ace / RGSS3 游戏，不是 NW.js/V8 游戏**。09-20 22:16:16 已记录安装完成；09-21 08:16:02 点击开始后，当前 console 明确停在：

```text
LaunchApp changed task to ShowLaunchOption
LaunchApp waiting for user response to ShowLaunchOption
```

最小化 Steam 主窗后可见“独自在家”任务栏项，对应弹窗内容黑色。点击该项、Enter 没推进。**没有确认 Game.exe 创建，不能把任务栏标题或点击开始当作游戏运行成功。** `gameprocess_log.txt` 尾部是旧会话，本轮应先看 `console_log.txt`。

截图已保存：`v19-library.png`、`v19-detail.png`、`v19-game.png`、`v19-minimized.png`，均位于上述证据目录。

## 5. 源码与补丁：哪些可以保留，哪些只是诊断

以下相对路径均以 `F:\WineHua\proton-ohos-worktree` 为根；容器内同一位置为 `/data/src/winehua`。**当前工作源码不是已清理的产品版本。**

| 文件 / 产物 | 用途与接入状态 |
| --- | --- |
| `patches/wine/0002-ntdll-ohos-signal-safe-stack-read.patch` | 已确认的安全 stack reader，已接 `scripts/build_wine.sh` |
| `patches/wine/0003-ntdll-ohos-serialize-fault-maps.patch` | maps 缓存非阻塞 atomic gate；去掉已证伪 W/X 互切及关闭互切时仍发生的共享 maps 刷新；未接产品构建 |
| `patches/wine/0004-ntdll-skip-dead-thread-tls-during-shutdown.patch` | 退出阶段 TLS 遍历 guard，测试通过；Metrics 仍有崩溃，未接产品构建 |
| `patches/wine/0005-ntdll-ohos-cef-diagnostic-command-line.patch` | 修复仅 native argv 注入无效的问题；在实际 Windows CommandLine 恢复处追加显式诊断 flags，完整 basename 匹配 steamwebhelper.exe 并扩容；未接产品构建 |
| `scripts/patches/wine-native-teb-probe.patch` | 单独的临时诊断集合，未接产品构建；含寄存器/TEB/direct stderr/EXIT/QUIT/raw sigaction/JS 对照等 |
| `tools/steam-rwx/export_native_fixes.py` | 导出 0003–0005、去诊断源码和独立诊断补丁；同时剥离 jitless/no-opt |
| `artifacts/fex-rwx-probe/native-fixes-source/` | 导出器保存的去诊断源码；仍含待评估修复候选，不代表最终产品已验收 |
| `scripts/patches/fex-arm64ec-rwx-probe.patch` | v13–v20 所用 FEX 只读探针 |
| `scripts/patches/fex-arm64ec-callret-bypass-probe.patch` | v21 返回目标缓存绕过，已应用于隔离 FEX 源码 |
| `tools/steam-rwx/prepare_callret_probe.py` | v21 源码准备及补丁生成器，保留应用前备份 |

当前 Wine `thirdparty/wine-valve/dlls/ntdll/unix/` 重点位置：

- [ohos_virtual.c:296](F:/WineHua/proton-ohos-worktree/thirdparty/wine-valve/dlls/ntdll/unix/ohos_virtual.c:296)：安全 stack reader；同文件还留有 FAULT-MIN、direct stderr、raw rt_sigaction 临时分支。
- [virtual.c:4651](F:/WineHua/proton-ohos-worktree/thirdparty/wine-valve/dlls/ntdll/unix/virtual.c:4651)：`process_exiting` guard；文件中仍有 TEB-PROBE。
- [env.c:2183](F:/WineHua/proton-ohos-worktree/thirdparty/wine-valve/dlls/ntdll/unix/env.c:2183)：当前诊断后缀为 `--js-flags=--no-opt`。
- [server.c:1614](F:/WineHua/proton-ohos-worktree/thirdparty/wine-valve/dlls/ntdll/unix/server.c:1614)：EXIT-PROBE；`thread.c` / `signal_arm64.c` 还留有 QUIT-PROBE。
- [FEX Module.cpp:901](F:/WineHua/proton-ohos-worktree/build/fex-rwx-probe/source/Source/Windows/ARM64EC/Module.cpp:901)：三级 AV 处理和结果插桩；`Source/Windows/Common/InvalidationTracker.{h,cpp}` 是区间查询/恢复处。

v20 时已通过 Wine 临时诊断补丁的反向适用性检查；该命令只检查，不改源码：

```bash
cd /home/liufeng/src/WineHua-proton-ohos/thirdparty/wine-valve
git apply -R --check ../../scripts/patches/wine-native-teb-probe.patch
```

**不要重跑旧 `prepare_native_probe.py`**，它是旧轮次的临时准备器。移除探针应使用与当前源对应的导出补丁，并先检查。v20 源码快照在 `artifacts/fex-rwx-probe/v20-source/`；对应符号库在 `artifacts/fex-rwx-probe/ntdll-v20-unstripped.so`。v18/v19 也保留各自 stripped ntdll 和候选 HAP，不能混用符号解地址。

## 6. v21 停在哪里，如何接续

### 6.1 已完成与未完成

隔离源码：`build/fex-rwx-probe/source`；修改前备份：`artifacts/fex-rwx-probe/v21-fex-source-before/`。

v21 的设计是：

1. ARM64EC guest return 仍 pop shadow/call-ret stack，但不直接使用缓存的 host 返回目标，走正常 L1/dispatcher 查找。
2. EC→JIT 入口命中返回栈时仍 pop，改跳 `LoopTop`，不执行原来的 `ret(TMP2)`。
3. 加启动标记：

```text
[FEX-CALLRET-CONTROL] guest_returns=lookup ec_returns=lookup accounting=preserved
```

**这不是关闭全部 call/ret 机制，也没有禁用 L1 缓存。准确名称是“返回目标缓存绕过对照”。** 与它比较的基线应是 v18（正常 JS JIT + 原只读 FEX 探针），以保持 ntdll 和显式诊断配置一致。

已经运行的构建是直接执行 `cmake --build build/fex-rwx-probe/ec --target arm64ecfex -j8`。BranchOps、Dispatcher、Module 对象编译成功，只有已有 unused-template warning；到链接时报：

```text
[100%] Linking CXX shared library ../../../Bin/libarm64ecfex.dll
Error running link command: no such file or directory
```

**这是构建失败，不是仅被用户中断。没有成功的 v21 DLL/HAP/设备结果；输出目录中的旧 DLL 不可当成新产物。** 整理资料期间没有继续 build。

### 6.2 已核对的环境缺口

[构建完整日志](F:/WineHua/device-evidence-20260920-fex-rwx/v21-fex-build.log) 已从容器 `/tmp/v21-fex-build.log` 复制到本地，避免临时文件丢失。

[链接环境核对](F:/WineHua/device-evidence-20260920-fex-rwx/v21-link-environment-review.log) 确认：

- `build/fex-rwx-probe/ec/Source/Windows/ARM64EC/CMakeFiles/arm64ecfex.dir/link.txt` 第一行调用 `/usr/bin/cmake`，第二行调用**裸命令** `arm64ec-w64-mingw32-ar`，第三行使用 clang++ 绝对路径。
- 容器默认 PATH 为 `/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`，无法查找到这个 `ar`。
- 实际 `ar` 文件存在且可执行：`/data/src/winehua/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/bin/arm64ec-w64-mingw32-ar`；将该 bin 加入 PATH 后可找到。
- 因此已确认链接环境缺少 LLVM MinGW PATH；**补齐后能否完整链接尚未重建验证**，不要提前写构建成功。
- v21 三个文件的 `patch -R --dry-run` 已通过，证据为 [v21-patch-reverse-check.log](F:/WineHua/device-evidence-20260920-fex-rwx/v21-patch-reverse-check.log)。

`scripts/build_fex_rwx_probe.sh` 包含正确的 PATH 初始化，但会把构建结果复制为通用 `libarm64ecfex-rwx-probe.dll`。接手 v21 应保存独立文件名，保留已验证的 v13–v20 探针二进制。不要改用 `build_fex.sh` 重 stage 现有诊断源码。

### 6.3 接手后的构建命令（尚未执行）

先进入已有构建容器；以下是接续模板，**不表示本次交接已执行成功**：

```powershell
wsl -d Ubuntu-22.04 -u liufeng
```

```bash
docker exec -it -w /data/src/winehua wineohos-build bash
```

容器内：

```bash
set -euo pipefail
export TOOL_HOME=/apps/harmony
source scripts/env.sh >/dev/null 2>&1
export PATH="$LLVM_MINGW/bin:$PATH"
command -v arm64ec-w64-mingw32-ar
patch -d build/fex-rwx-probe/source -p1 -R --dry-run --batch \
  < scripts/patches/fex-arm64ec-callret-bypass-probe.patch
cmake --build build/fex-rwx-probe/ec --target arm64ecfex -j8 \
  > /tmp/v21-fex-rebuild.log 2>&1
cp build/fex-rwx-probe/ec/Bin/libarm64ecfex.dll \
  artifacts/fex-rwx-probe/libarm64ecfex-callret-v21-unstripped.dll
cp artifacts/fex-rwx-probe/libarm64ecfex-callret-v21-unstripped.dll \
  artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll
"$LLVM_MINGW/bin/llvm-strip" artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll
python3 -c 'from pathlib import Path; p=Path("artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll"); assert b"[FEX-CALLRET-CONTROL]" in p.read_bytes()'
sha256sum artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll
sha256sum artifacts/fex-rwx-probe/ntdll-v18.so
```

确认 ntdll 为 §1.2 的 **v18** 哈希，然后打包：

```bash
python3 scripts/package_fex_rwx_candidate.py \
  --ntdll artifacts/fex-rwx-probe/ntdll-v18.so \
  --fex artifacts/fex-rwx-probe/libarm64ecfex-callret-v21.dll \
  --output artifacts/fex-rwx-probe/fex-rwx-v21-unsigned.hap
```

打包脚本会核对 v12 baseline，并断言 payload 只换 FEX；HAP 相对该 baseline 仅 ntdll、payload 和 runtime manifest 三项内容变化。v21 将产生**新的** FEX/payload/HAP 哈希，本文没有这些未生成值。

## 7. 构建、部署、启动与取证入口

### 7.1 Wine 与 HAP 层级

在 WSL 运行：

```bash
docker exec -w /data/src/winehua wineohos-build bash -lc \
  'make -C build/wine-ohos-aarch64 -j8 dlls/ntdll/ntdll.so > /tmp/ntdll-candidate-build.log 2>&1'
```

源二进制为容器内 `build/wine-ohos-aarch64/dlls/ntdll/ntdll.so`。保存 unstripped 后再对候选副本执行：

```text
/apps/harmony/sdk/default/openharmony/native/llvm/bin/llvm-strip
```

**Unix ntdll 是 HAP 顶层 `libs/arm64-v8a/ntdll.so`，不在 wine-data.zip payload 内。** FEX 则在 payload 的 `bin/aarch64-windows/libarm64ecfex.dll`。不要直接安装主工程 assembleHap 的旧 payload。

App native/ArkTS 构建仍参考旧交接 §4 的 `w1-m3-build-hap.sh` 与隔离 `scripts/package_probe_candidate.py` 流程；本轮没有要求重建显示链。

容器内签名模式如下；输入输出示例为接续 v21，**本次未签名 v21**：

```bash
export TOOL_HOME=/apps/harmony
source scripts/env.sh >/dev/null 2>&1
python3 sign.py \
  artifacts/fex-rwx-probe/fex-rwx-v21-unsigned.hap \
  artifacts/fex-rwx-probe/fex-rwx-v21-signed.hap \
  > /tmp/fex-v21-sign.log 2>&1
```

签名 stdout/stderr 必须完全重定向；不打印签名配置、口令或原始签名日志。最终若恢复原 FEX，需要给打包器新增“保持原 payload/manifest 原字节”的路径：当前 `package_fex_rwx_candidate.py` 断言 FEX 必须相对原版有变化，直接传原 FEX 会失败。

### 7.2 设备只读核对与启动顺序

PowerShell：

```powershell
hdc tconn 192.168.180.71:36875
hdc list targets -v
hdc -t 192.168.180.71:36875 shell date
hdc -t 192.168.180.71:36875 shell df -h /data
hdc -t 192.168.180.71:36875 shell cat /data/app/el2/100/base/app.hackeris.winehua/files/wine/.winehua-runtime-manifest.json
hdc -t 192.168.180.71:36875 shell sha256sum /data/app/el2/100/base/app.hackeris.winehua/files/wine/bin/aarch64-windows/libarm64ecfex.dll
```

替换安装用 `hdc -t TARGET file send LOCAL_HAP /data/local/tmp/UNIQUE.hap`，再 `hdc -t TARGET shell bm install -p /data/local/tmp/UNIQUE.hap -r`。先核对本地/设备包哈希和剩余空间；不得靠删除游戏目录腾空间。用唯一候选文件名保留版本对应关系。

v20 初次冷启动过早发 Steam 时，host **50754/51790** 已创建但无 CEF，私有日志仅 5925 字节；不能一律叫 Want 丢失。实测恢复顺序是：

1. `hdc -t 192.168.180.71:36875 shell aa force-stop app.hackeris.winehua`
2. `hdc -t 192.168.180.71:36875 shell aa start -b app.hackeris.winehua -a EntryAbility`
3. **等待 Wine 桌面初始化完成**，当时约一分钟；不要只看 `aa start successfully`。
4. 再用下述 Want 工具启动 Steam，随后核对 `GameTest` 的 host PID 和 CEF 日志。

```powershell
python -X utf8 F:\WineHua\proton-ohos-worktree\tools\steam-rwx\launch_want.py `
  --target 192.168.180.71:36875 `
  --artifacts F:\WineHua\device-evidence-20260920-fex-rwx `
  --label resume-steam `
  --exe 'C:\Program Files (x86)\Steam\steam.exe' `
  --env 'WINEHUA_CEF_NO_CRASH_HANDLER=1' `
  --env 'WINEHUA_WINEDEBUG=-all,+seh'
```

这里开启的是显式诊断。v18 ntdll 不追加 JS 关闭后缀，v19 追加 jitless，v20 追加 no-opt；接手必须看实际 `webhelper.txt`/Windows CommandLine，不能只看 native 注入日志。

### 7.3 Want 与日志陷阱

- `game_argN`、`d3d_env_valueN` **不做 URI 解码**。以前 `%2Fc`、`%2Dall%2C%2Bseh` 会被原样传入。`launch_want.py` 使用带 argc 的 encoded JSON argv/env，已处理这个问题。
- env 必须进入 `entry/src/main/ets/game/GameHook.ets` 白名单，否则静默丢弃。
- GameHook 的 hilog tag 实际是 **`GameTest`**：`hdc -t 192.168.180.71:36875 shell hilog -x -T GameTest`。
- `aa start successfully` 不是目标 exe 成功运行的证据；先确认进程创建，再看后续日志和退出。
- 共用 `wine_stderr` 会跨进程交错。`wine_child.cpp` 的 pipe reader 在进程骤死时可能丢尾，首发异常优先用该进程的 direct native 日志。
- `device_command.py` 是有界命令 wrapper，默认超时 45 秒，可设 `--timeout 60`，并写 `commands.jsonl`。`--show` 会显示尾部 5000 字符，账号/环境原始日志不要 show。hdc 返回 0 仍可能包含内部失败，需看内容。

示例：

```powershell
python -X utf8 F:\WineHua\proton-ohos-worktree\tools\steam-rwx\device_command.py `
  --target 192.168.180.71:36875 `
  --artifacts F:\WineHua\device-evidence-20260920-fex-rwx `
  --label resume-disk --show -- shell 'df -h /data'
```

### 7.4 私有 native 日志导出（已实测路径）

`temp/wine-native-<hostPID>.log` 权限 0600，普通 hdc shell/file recv 不能读；本轮尝试 `hdc shell -b bundle` 仍 Permission denied。应在 Wine 应用身份下复制：

```powershell
python -X utf8 F:\WineHua\proton-ohos-worktree\tools\steam-rwx\launch_want.py `
  --target 192.168.180.71:36875 `
  --artifacts F:\WineHua\device-evidence-20260920-fex-rwx `
  --label export-browser54557 `
  --exe 'C:\windows\system32\cmd.exe' `
  --arg /c --arg copy --arg /b --arg /y `
  --arg '\\?\unix\data\storage\el2\base\temp\wine-native-54557.log' `
  --arg 'C:\windows\temp\wine-native-54557-export.log'
```

确认复制真正完成，再接收；该 browser 导出已有一次成功记录，可先检查已有目标，避免无谓重发：

```powershell
hdc -t 192.168.180.71:36875 file recv /data/app/el2/100/base/app.hackeris.winehua/files/.wine/drive_c/windows/temp/wine-native-54557-export.log F:\WineHua\device-evidence-20260920-fex-rwx\v20-browser54557.log
```

`/b` 不能省，文本 copy 会添加 0x1a。上面的完整本地 `v20-browser54557.log` 是下一步取证目标，**截至交接尚未保存**，已有的是 `v20-browser54557-tail.log`。

### 7.5 UI 与调试工具状态

- `tools/steam-rwx/window_snapshot.c` 已编译为 `artifacts/fex-rwx-probe/window-snapshot.exe` 并传到 `/data/local/tmp/winehua-window-snapshot.exe`，但 Z 路径启动、cmd 执行、复制 C 盘均未得到预期输出。**没有证明该 helper 运行成功**，不应盲目重复。
- 可先检查现有 `smoke/winehua_win32_driver.c`、`tools/steam-boundary/window-response.c` 和设备已有 `C:\smoke\x64\winehua_win32_driver.exe`；本轮没有用它完成启动弹窗操作。
- 截图用 `uitest screenCap -p /data/local/tmp/NAME.png` 再 file recv。实际分辨率 2560×1600，查看工具可能缩成 1996×1248，点击须换算坐标；不要直接照缩图点。
- OHOS Enter keycode 为 **2054**，已按 SDK 核对。
- lldb-server 已传，但执行被 Permission denied，未成功 attach；processdump 不可用。`hidumper -e` 未提供本轮 CEF 记录。
- `hilog -x -t kmsg` 可读短缓冲；`-t 2500` 不是行数选项，t 是类型。

## 8. 证据入口与交接校验

除特别注明，以下文件均在 [本轮证据目录](F:/WineHua/device-evidence-20260920-fex-rwx)：

| 要回答的问题 | 优先查看 |
| --- | --- |
| 截止状态、余量与后继 renderer 退出 | `handoff-current-status.log` |
| v13 tracker 是否认领、二次故障前后 | `wine-v13-a.log`、`wine-v14-cold.log`、`wine-v14-end.log`、本轮逐次复核记录 |
| 用户下载/启动期间黑屏 | `game-crash-hilog.log`、`game-crash-wine.log`、`game-crash-additional.log`、`game-crash.png` |
| v18 首发 V8 断点 | `v18-renderer-38401.log`、`v18-renderer-final-tail.log`、`libcef.dll`、`v8-deoptimizer.cc` |
| Metrics 退出链 | `v18-metrics-38620.log`、`v18-metrics-tail.log`，早期 v15/v16 细节见逐次复核记录 |
| v19 真正参数与存活窗口 | `v19-webhelper-args.log`、`v19-final-snapshot.log`、`v19-renderer46106-tail.log` |
| 游戏启动停在何处 | `v19-game-launch-logs.log`、`v19-launch-last.log`、`v19-minimized.png`；也可在 `v20-console-tail.log` 看到 08:16 的保留条目 |
| v20 参数、IPC 与退出顺序 | `v20-args-and-copy.log`、`v20-console-tail.log`、`v20-exit-order.log`、`v20-renderer54744.log`、`v20-browser54557-tail.log` |
| v21 构建失败与工具 PATH | `v21-fex-build.log`、`v21-link-environment-review.log` |
| v21 补丁已应用、可反向检查 | `v21-patch-reverse-check.log` |
| 本地二进制身份 | `handoff-artifact-checks.json`；打包差异报告在开发树 `artifacts/fex-rwx-probe/fex-rwx-v18/19/20-unsigned.json`（分别三个文件） |
| 设备命令和输出对应关系 | `commands.jsonl`；有些 ledger target 写成脱敏的 `handoff-tablet`，实际本轮 target 是 §1 的无线地址 |

原始日志、截图可能含账号与环境信息，只保留本地；交接正文只摘录技术必要片段。没有新上传或发给外部人员。

本轮已完成的工程验证：安全读取测试 PASS、TLS shutdown 测试 PASS、v20 Wine 编译与隔离打包断言通过、v20 Wine 诊断补丁反向检查通过。整理交接时另核对本地二进制哈希、v21 链接工具路径和 v21 补丁反向 dry-run；**没有重新跑实机稳定性测试，也没有把测试通过等同于故障已修复**。

## 9. 接手建议顺序与验收

1. 按 §6 补齐 v21 构建 PATH，确认成功链接、新产物启动标记和新哈希；保留 v18 原只读 FEX 探针作为对照。
2. v21 搭配 **ntdll-v18.so**，核对实际命令行无 jitless/no-opt，在相同显式诊断配置下做返回缓存绕过 A/B。多次比较库操作、游戏详情、启动选项；一次不崩不能证明根因。
3. 分别记录三类事件的**首发证据**：V8 CHECK 的寄存器/帧状态；IPC 超时前 browser/renderer 的完整直接日志；Metrics 退出到 NtTerminateProcess 后的执行路径。不要把所有 signal 11 汇总成同一原因。
4. 将 jitless 保留作参考配置。当前证据不支持把 jitless/no-opt、raw rt_sigaction、强制 RWX 或宽泛吞异常直接产品化。
5. 库与弹窗稳定后，再确认 `Game.exe` 的实际进程、RGSS3 游戏窗口和可操作画面。本例是 32 位游戏，还需核实其实际运行链，不能仅以 Steam win64/CEF 存活代替游戏验收。
6. 根因修复有独立实证后，再移除诊断、评估接入 0003–0005 中适用部分，恢复原 FEX/正常 JS 设置复测；保留已经确认的安全 stack reader。最终应验证多轮冷启动、持续界面操作、游戏下载/启动和退出清理，并记录每轮版本身份与结果。

**给下一位专家的起点：先解决 v21 的链接环境，再用 v18 ntdll 做返回目标缓存绕过对照；同时将 V8 帧校验、IPC 超时、Metrics 退出清理分开取证。当前仍是 v20 装机，未修复，未确认游戏运行。**

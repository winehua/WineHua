# Steam win64：FEX 与 native 故障复核（2026-09-21）

> 最新交接入口：[EXPERT_HANDOFF_20260921_0845.md](F:/WineHua/proton-ohos-worktree/docs/steam-win64/EXPERT_HANDOFF_20260921_0845.md)。
> 设备快照截至 08:43:47：装机 v20，CEF 仍退出；游戏未确认运行；v21 链接失败，未部署。

## 已证实的修复与纠正

显示链保持原样。本轮重点是 Wine/FEX 信号处理和 CEF 退出。

1. `build/fex-ec/Bin/libarm64ecfex.dll` 经 llvm-strip 后，SHA256 精确等于设备原版
   `8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc`。
   原交接把未 strip/strip 的哈希差异误判为行为差异；不能反向撤掉已有 ThreadTerm 补丁作为“基线”。
2. v13 独立 FEX 只读插桩证明 tracker 和 call-ret 正常认领相应故障。
   Wine 的 `[SMC] result=not_mine` 出现在后续 FEX SEH 之前。
3. Wine `ohos_smc_dump_stack()` / `ohos_smc_attribute_stack()` 原来在信号处理器中直接读取故障 SP。
   FEX SP 可以恰在 emulator 栈顶 guard 页，诊断读取会触发第二个 SIGSEGV。
   v14 改为 `SYS_process_vm_readv` 内核拷贝，读取失败/部分成功时跳过不可读内容，保存 errno；不再回退直接读取。
   可重放修复：`patches/wine/0002-ntdll-ohos-signal-safe-stack-read.patch`，已接入 `scripts/build_wine.sh`。

FEX 实测片段（Wine PID 1352，host PID 22475）：

```text
[FEX-RWX-TRACK] pid=1352 tid=1436 addr=0x6fbb680570 x=1 rwx=1 smc_disabled=0 query_base=0x6fbb680000 query_size=0x40000 query_write=1 vm_prot=0x20
[FEX-RWX-UNPROTECT] ... status=0x0 old=0x20
[FEX-RWX-CHAIN] ... callret=0 guard=0 tracker=1 handled=1 jit=1
```

v13 renderer 最后的 native 故障 SP 为 `0x20e90000`，日志在栈邻居输出后中断，未进入对应 FEX ENTRY，7380 ms 后 signal 11。
v14 能到登录/主界面，call-ret 记录出现 `callret=1 handled=1`；但这不是完整修复。

## 剩余复现

- v14 普通诊断：renderer host PID 28038 存活 130682 ms 后 signal 11。
- quiet 对照（宿主 29729）：browser 30438 在 22:13:34 退出，renderer 30650 也退出，
  之前约四分钟存活只能视为中间状态。随后 browser 32779 重启，多名 renderer 又反复退出。
- 用户实测下载、启动游戏与操作 Steam 界面仍崩溃。
  本地取证显示《At Home Alone Final》（AppID 1740100）22:16:16 安装完成；
  22:16:21 renderer 32917 退出，22:16:39 后继 renderer 36195 退出。
  22:18:43 Steam 主进程 30245 与宿主 29729 仍存活，截图为上下栏保留、内容区全黑。
  尚未从 `gameprocess_log.txt` 确认实际游戏进程启动，不能把“点击启动”当成已运行游戏。
- ProcessorMetrics 重复 signal 11。v13 host PID 23050：`addr=0x15180c pc=0x7e922c9a0c`。
  根据 ntdll ELF 的 PT_LOAD 虚拟地址（不是仅减文件偏移）定位到 `virtual_clear_tls_index()` 内
  `get_wow_teb()` 对 `TEB.WowTebOffset` 的读取，目标 TEB 约 `0x150000`。
  早期 maps 是首次异常快照，不能证明退出时该页仍未提交。
  `signal_alloc_thread()` 在 ARM64 实现里直接成功；其错误路径的 list_remove 问题不能解释此实机故障。
- 另有 FEX 返回未认领的真实 guest read AV，例如 `0xd0d000424`、`0x13ffffffff`；
  这些不是应当放开 RWX 权限的代码写入。

## 候选与验证

| 候选 | 内容 | ntdll SHA256 |
| --- | --- | --- |
| v13 | 仅 FEX 只读插桩 | 沿用 v12 |
| v14 | FEX 插桩 + 安全 stack reader | ab1b8013749a35fde44bc123ce69c1d95ec6d556bd51a6a82e84026b52bce47a |
| v15 | v14 + native 寄存器/TEB 生命周期临时插桩；maps 非阻塞保护；移除已证伪 W/X 互切分支 | 74c69098c15cb42918c8a61aec2e12f5fc7d0d5bceff014b1273485e73504cb3 |

v13–v15 的 FEX SHA256 均为 `e68c312b5667e867484ed610ca035d8da7e8f6d0f6891b956100d87fc1ff51ed`，
payload 均为 `824faa3e23bd8006c9db76a365438a3ffd7c862f691a7af8495e6f417208ea21`。
v15 相对 v14 只改 HAP 顶层 ntdll，payload 不变。

`tools/steam-rwx/check_safe_stack_read.py` 从实际源文件提取 helper，测试真实 mmap 的可读、跨 guard 部分读取、
signal handler 内 guard 读取、null、invalid、unmapped、errno，已 PASS。
OHOS ntdll 编译通过，存在该树原有 warning；临时 TEB 插桩另有 declaration-after-statement warning。

v15 的只读插桩由 `tools/steam-rwx/prepare_native_probe.py` 临时应用，未接入产品构建。
应用前源码在 `artifacts/fex-rwx-probe/v15-source-before/`，用于生成诊断补丁和最终移除插桩。
`[FAULT-MIN]` 仅记录保存的寄存器，每进程上限 1024；`self` 可用于无 maps 时解算 ntdll load bias。
v15 符号：`ohos_route_host_fault=0x53b48`、`virtual_clear_tls_index=0x8a004`。

## 设备与证据

显式无线 target：`192.168.180.71:36875`。断开后 `hdc tconn` 已重连。
2026-09-21 06:58 核对 payload 仍匹配，游戏目录仍在，系统已清理前日 temp 日志。
07:01 安装 v15 首次失败：设备数据分区 453 GB，剩余约 2.6 GB；`/data/local/tmp` 占 75 GB。
核对了 16 个临时安装包与本地备份的 SHA256，但尚未执行删除时用户已主动清理。
复核剩余 109 GB，随后 v15 安装成功；本轮没有执行设备文件删除。
v15 signed HAP SHA256：`a6f6f329dfa5a089580f9c9c870076077ad4b57815d5b520f471916e4d1eeb31`。

## v15 实测与 v16 修复候选

v15（宿主 6311）ProcessorMetrics host PID 8821 的日志确认：TEB `0x150000` 提交成功，
多次 TLS clear（索引 0x15…0x0c）正常；`RtlExitUserProcess(status=0)` 后，索引 0x0a 清理
仍遍历该 TEB，读取 `0x15180c` 得到 `SEGV_MAPERR`，2018 ms 后以 signal 11 退出。
这说明至少此例是退出清理时的崩溃，不能作为 CEF 启动失败的直接证据。
renderer 8432 也在 92700 ms 后退出，但尚未证明同因。

v16 在 `virtual_clear_tls_index()` 保留索引检查，进程进入 `process_exiting` 时跳过遍历已终止线程。
这与 `NtTerminateProcess(NULL)` 先终止其他线程再执行 DLL detach 的顺序相符；后续若 TLS 索引复用，
`TlsAlloc` 自身会清空调用线程的新槽。`tools/steam-rwx/check_tls_shutdown.py` 用实际函数验证
正常 native/WOW 清理、扩展槽、失效 TEB 的退出路径、非法索引，已 PASS。
v16 ntdll SHA256：`55c05d81720e755e8bea5819a4e398b7b51b8751d40ce8ee60516e0433a8be20`。

还发现旧 `wine_child.cpp` 的诊断参数仅改变 native argv；Wine 子进程的
`init_startup_info()` 会从 wineserver 恢复原 Windows CommandLine，因此不能据“args injected”日志
声称 CEF 实际关闭了 crash handler。v16 在实际 Windows 参数恢复处应用同一显式诊断开关，
记录 `[CEF-CMDLINE] startup_info=1 crash_handler_disabled=1`；默认未开启时不追加。
v16 实测仍未解决：ProcessorMetrics 21558 在 2730 ms 后 signal 11，原 index 0x0a 的
TEB fault 未再出现在管道日志里；不能据此断言已修复。renderer 21198 在 111259 ms 后退出，
最终读取 `0x1400e689c0`，FEX 查询为 `MEM_RESERVE`、`Protect=0`、`x=0 rwx=0 smc_disabled=0`，
三级链返回 `handled=0`。这不是应被放开写执行权限的代码页。

07:38 使用先启动 EntryAbility、再发 game Want（`game_argc=0`，仅开启
`WINEHUA_CEF_NO_CRASH_HANDLER=1`）成功进入 CEF。
browser 25523、renderer 25717 等均有 `[CEF-CMDLINE] startup_info=1 crash_handler_disabled=1`；
诊断参数确认写入 Windows CommandLine，但 ProcessorMetrics 25964 仍在 1899 ms 后 signal 11。
自动入口先前卡住的原因尚未确定；不要再宣称该诊断参数尚未实测生效。

## v17：补齐进程退出前日志

`wine_child.cpp` 通过 pipe 和同进程 reader 线程转存 stderr。进程骤死可能丢掉管道尾部，
所以“没有最终 FAULT-MIN”不等于“没有经过故障处理器”。v17 隔离诊断补丁在显式
`WINEHUA_CEF_NO_CRASH_HANDLER=1` 时把 stderr 直接写入
`temp/wine-native-<hostPID>.log`，并在 libc `exit()` 前记录 `[EXIT-PROBE]`。
临时块只存在独立 `scripts/patches/wine-native-teb-probe.patch`，不接产品构建。
导出器会剥离寄存器、TEB、直接 stderr、exit 探针，WSL `git apply -R --check` 已通过。

v17 ntdll SHA256：`078a32ac5200a14d09b45b59864d2a573cefe9d501510db58fc7365246006535`。
signed HAP：`05936e9543d25afbb2dde118cd36b7114140d0882d70a6e9edcf1687b701aecc`。
payload/FEX 与 v13–v16 相同，仅顶层 ntdll 变化；07:43 已安装并启动。
v17 符号：`ohos_route_host_fault=0x53fec`、`process_exit_wrapper=0x64938`、
`virtual_clear_tls_index=0x8a4f4`。
CEF 命令行匹配还补了文件名分隔符检查，避免误匹配其他以 steamwebhelper.exe 结尾的名称。

v17 直接日志确认 ProcessorMetrics 30117 最后停在 `RtlExitUserProcess(status=0)`，
没有 `[EXIT-PROBE] phase=libc-exit`。启用真正解码后的 `-all,+seh,+process` 再测，
ProcessorMetrics 34884 在 1394 ms 后 signal 11；最后记录为
`NtTerminateProcess(handle=0)` 返回 `self=1`，随后一条 `RtlInitializeExtendedContext2`。
仍未出现最终 FAULT-MIN，故不能把它归因到 libc exit finalizer。

注意 Want 的 `game_argN`、`d3d_env_valueN` **不做 URI 解码**。
此前传 `%2Fc`、`%2Dall%2C%2Bseh` 会把这些字面值传给 Wine。
新增 `tools/steam-rwx/launch_want.py` 总是发送带 argc 的 encoded JSON argv/env，避免此陷阱。
`game_arg_encN` 也支持解码；单独 `game_argN` 要传原文。

私有的 `wine-native-<PID>.log` 为 0600，hdc 无法直接读取。
可用上述工具启动 `cmd.exe /c copy /b`，将精确指定的
`\\?\unix\data\storage\el2\base\temp\wine-native-<PID>.log`
复制到 `C:\windows\temp\wine-native-<PID>-export.log` 后读取，已实测成功。
不要使用文本 copy：它会添加 0x1a。

## v18：原生信号入口对照（诊断候选）

为区分 Wine 处理器自身与 OHOS sigchain 的漏报，v18 仅在显式 CEF 诊断开关存在时
选用文件内既有的 `rt_sigaction` 后备入口（SA_ONSTACK / SA_NODEFER），并在 SIGQUIT、
abort_thread、pthread_exit_wrapper 增加常量 write 标记。
FAULT-MIN 配额提高到 65536，避免 renderer 的正常 SMC 再次耗尽配额。
这些都是独立诊断补丁内容，不是已确认的产品修复；FEX 与页面权限逻辑未变。

v18 ntdll：`4fa7ccf3e9b0d576e697abb6e5a0379db0ef994197e241035c290b3bff0801bd`。
signed HAP：`efdd75af9173721c6a17cd4dab3d41fca688a61959bc4548781de74acf911391`。
符号：`ohos_route_host_fault=0x5404c`、`quit_handler=0x679b8`、
`pthread_exit_wrapper=0x817f0`、`virtual_clear_tls_index=0x8a59c`。

v18 已确认原生入口生效，但 ProcessorMetrics 38620 仍在 1663 ms 后 signal 11。
日志显示多个线程进入 abort_thread / pthread_exit_wrapper；仍无最终 FAULT-MIN。
这次对照不支持把 sigchain 改成原生入口作为产品修复。

### renderer 的首发故障已定位到 V8 反优化栈帧检查

v18 renderer 38401 的直接 SEH 日志先出现断点异常 `0x80000003`：
guest PC `0x6fecd9d83a` = `libcef.dll + 0x2e2d83a`，上层返回地址为 `libcef.dll + 0x32e6ba6`。
之后才进入终止线程阶段并被报告为 SIGSEGV，不能把最终 signal 11 当成首发故障。

实机 libcef.dll 已取回（219859608 字节，PE 时间戳 2024-07-15；CEF/Chromium 126），
反汇编 `0x182e2d82c`：`lea eax,[rax+rcx*8]; add eax,-16; cmp eax,ebp; je ...; int3; ud2`。
故障寄存器给出 `eax=0xc8`、`ebp=0x70`。
结合周围 `0xbeeddead` frame marker、FrameDescription 分配及 V8 12.6 源码，
匹配 `Deoptimizer::ComputeInputFrameSize()` 的栈大小 CHECK_EQ。
这指向 JS JIT/反优化路径中的帧元数据或栈状态不一致；尚未证明是哪一条 FEX 翻译导致。

v19 仅在已有显式 CEF 诊断开关开启时追加 `--js-flags=--jitless` 做对照，
写入实际 Windows CommandLine。该后缀由导出器剥离，不进入产品 0005 补丁。
ntdll：`0101519d81b335653d621e87d6f8d3cd2c17bca070f1fed712261f0a0c067026`；
signed HAP：`637fd9ff3883b18666c195b15564830b4740cb4a62db217ad7fb9c0c27ff3a13`。

HDC shell 对下载的 lldb-server 执行被设备权限拒绝，未成功附加调试器；
`hidumper -e --list app.hackeris.winehua` 未提供本轮 CEF native 记录。

### v19 实机：主库存活约 15 分钟，启动选项弹窗仍未完成

08:09:45 的 `webhelper.txt` 确认实际命令行包含 `--js-flags=--jitless`。
browser 44179、主 renderer 44384（08:09:51 创建）到 08:25:15 仍存活，
其间库主页与《独自在家》详情页切换正常，未看到主库内容再次消失。
这支持优化执行路径相关假设，但不是完整修复：ProcessorMetrics 44625 仍 1412 ms 后 signal 11；
renderer 46106 在 61328 ms 后 signal 11，直接日志尾部是终止线程流程，未出现 v18 的 V8 断点。
不能把所有 renderer 的最终 signal 11 都判为相同首因。

08:16:02 点击开始游戏，`console_log.txt` 明确停在 `ShowLaunchOption` 等待用户响应。
最小化主窗口后，桌面任务栏可见游戏标题，对应弹窗内容为黑色；没有证据表明 Game.exe 已创建。
`gameprocess_log.txt` 的尾部还是旧会话，不能用它代替当前 `console_log.txt`。
截图：`v19-library.png`、`v19-detail.png`、`v19-game.png`、`v19-minimized.png`。

原交接 v4 所谓排除 call-ret 仅依据故障分类日志零命中，未做禁用 call-ret 的对照。
v13 后已实际观察到正常 call-ret 故障被处理，所以原来的排除结论证据不足；此处予以纠正。

### v20：只关闭 V8 优化编译的隔离对照

相对 v19 仅将后缀改为 `--js-flags=--no-opt`，保留基础 JIT。
仍属于显式 CEF 诊断模式，导出器同时剥离 jitless/no-opt，未接产品构建。
ntdll：`685e7fd1500a2b30ebfa951148cfef8420eb08d7cd350be45c19d0fdb00a1449`；
signed HAP：`427e74de82e948646f8cc88f013876d924dd424d58bb72c4b0f9ee1bf4a8ae25`。
payload/FEX 与 v19 相同；08:25 后已安装启动，实际命令行已确认包含 `--js-flags=--no-opt`。
Wine 编译、隔离打包内容断言、临时探针 `git apply -R --check` 均通过。

#### v20 实测：IPC 超时后整组 CEF 退出，尚未证明同一 V8 断点

首次冷启动后过早发 Steam 时，host 50754/51790 已创建但无 CEF，私有日志仅 5925 字节；
不能一律归为 Want 丢失。force-stop 后先启动 EntryAbility，等 Wine 桌面初始化约一分钟，
再发 Steam Want，得到 Steam 54407、browser 54557、renderer 54744。

`v20-args-and-copy.log` 确认 no-opt 已进入实际命令行。08:37:58 的 console 先记录
`Timed out waiting for mutex in PutInternal()` 和 `chrome_ipc_client.cpp (1493): Failure pushing command to webhelper`。
随后 browser 54557 / gpu 54590 / renderer 54744 分别在 266674 / 264706 / 260162 ms 后 signal 11；
exitMs 为 1789951090545 / 1789951090562 / 1789951090583。

browser/renderer 直接日志尾部是 QUIT-PROBE 线程终止流程；完整 renderer 导出没有 v18 的 fatal breakpoint，
后期 dispatch_exception 是媒体相关 DBG_PRINTEXCEPTION。browser 尾部的 RtlCaptureStackBackTrace 不能独立归因。
因此 no-opt 未通过稳定性验证，但不能声称这轮已经复现相同 V8 CHECK，也不能将 IPC 超时本身视为已查明根因。
证据：`v20-exit-order.log`、`v20-console-tail.log`、`v20-renderer54744.log`、`v20-browser54557-tail.log`。

08:43:47 快照：数据分区 453 GB、已用 390 GB、剩约 63 GB；109 GB 是用户刚清理后的历史值。
宿主 53235、Steam 54407、后继 browser 56181 仍在；renderer 56347 又在 276705 ms 后 signal 11。
Metrics 55007 / 56913 分别在 1522 / 1501 ms 后 signal 11。证据：`handoff-current-status.log`。

### v21：返回目标缓存绕过已准备，链接失败，未部署

`tools/steam-rwx/prepare_callret_probe.py` 和
`scripts/patches/fex-arm64ec-callret-bypass-probe.patch` 已修改隔离 `build/fex-rwx-probe/source`。
guest return 和 EC→JIT 返回仍保留 shadow/call-ret stack 的 pop 记账，绕过缓存 host 返回目标，
走正常 L1/dispatcher；不是关闭全部 call-ret 或 L1。启动标记为 `[FEX-CALLRET-CONTROL]`。
备份在 `artifacts/fex-rwx-probe/v21-fex-source-before/`，反向 patch dry-run 已通过。

BranchOps、Dispatcher、Module 对象编译成功，链接报 `Error running link command: no such file or directory`。
日志已保存为证据目录 `v21-fex-build.log`。只读核对确认 link.txt 第二行调用裸命令
`arm64ec-w64-mingw32-ar`，默认容器 PATH 查找不到；工具存在于 LLVM MinGW bin，加入该目录后可查找到。
详见 `v21-link-environment-review.log`。尚未重新构建，不能声称补齐 PATH 后完整链接已验证。

v21 计划搭配无 JS 关闭后缀的 **v18 ntdll** 做独立 A/B；当前 Wine 源码仍为 v20 no-opt。
没有成功 v21 DLL/HAP、签名或装机结果，不可将输出目录残留旧 DLL 当新候选。
交接请求后仅做资料整理和只读核对，没有继续构建、部署或设备实验。

原始证据仅保留本地：`F:\WineHua\device-evidence-20260920-fex-rwx`。
关键文件：`wine-v13-a.log`、`wine-v14-cold.log`、`wine-v14-end.log`、
`hilog-quiet-b.log`、`game-crash-hilog.log`、`game-crash-wine.log`、`game-crash.png`、
`game-crash-additional.log`、`commands.jsonl`。
原始日志可能包含账号/环境信息，对外只使用筛选后的摘要。

构建/签名/部署继续使用隔离候选流程；不要直接安装主工程 assembleHap 的旧 payload。
签名 stdout/stderr 仅重定向到本地私有日志，不回显口令。

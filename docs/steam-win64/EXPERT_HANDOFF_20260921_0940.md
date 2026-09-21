# Steam win64 / OHOS v21 返回目标缓存绕过对照：进展与阻塞（2026-09-21，截点 09:36:54 CST）

本文接续 [2026-09-21 0845 交接](F:/WineHua/proton-ohos-worktree/docs/steam-win64/EXPERT_HANDOFF_20260921_0845.md)。遇到冲突以本文的新实测为准。文件名 0940 是交接截点标识；本轮**已执行设备实验**，没有再改动 Wine 源码或显示链。

## 0. 接手先看

**v21 的构建阻塞已解决，候选已签名、已装机，并且首次在设备上确认 v21 的 FEX 真正生效。**

| 事项 | 结论 |
| --- | --- |
| v21 FEX 链接 | **成功**（根因是容器 PATH 缺 LLVM MinGW，见 §2） |
| v21 候选签名 | 通过 SDK `hap-sign-tool verify-app`（codesign 与 digest 校验均为 true） |
| 设备当前装机 | **v21 已覆盖安装**（不再是最新的 v20） |
| v21 控制标记 | **已在设备上确认**：私有 native 日志出现 `[FEX-CALLRET-CONTROL]`（见 §3） |
| Steam 会话 | 到达登录界面，CEF browser/gpu/renderer 等 23–25 个 Wine 进程**存活 ≥10 分钟** |
| 库操作 A/B | **未完成**：界面里看不到 Steam 窗口，无法点击（见 §6） |
| 游戏验收 | **未做**。没有确认 `Game.exe`、没有确认游戏画面 |

两条必须带走的经验：

1. **共享 `wine_stderr_YYYYMMDD.log` 不能用来判断 FEX 版本。** 它跨会话累积，且本次会话的 FEX 初始化输出没有落进去。判版本必须看私有 `temp/wine-native-<hostPID>.log`（§3）。
2. **v21 会话的画面里没有 Steam 窗口**，而同一张“空桌面”截图在 v16/v17 就出现过（字节完全相同）。这既不是本轮引入的显示改动，也不能当作 CEF 稳定的充分条件（§6）。

## 1. 环境、设备与版本身份

| 项目 | 本轮值 |
| --- | --- |
| 开发树 / 容器挂载 | `F:\WineHua\proton-ohos-worktree` / 容器内 `/data/src/winehua` |
| WSL 仓库 | `/home/liufeng/src/WineHua-proton-ohos`，`feature/proton-wine-ohos` @ `32efc9c709d19a973942f967e96cb062bd84f2f7` |
| 构建容器 | `wineohos-build`（运行中） |
| 实验 target | **`192.168.180.71:36875`**（另有 USB target，命令仍须显式 `-t`） |
| 设备时间 / 余量 | 09:36:54 CST / `/data` 剩 63 G（未删除任何设备文件） |
| 本轮证据目录 | `F:\WineHua\device-evidence-20260921-v21` |
| 本轮产物 | `artifacts/fex-rwx-probe/fex-rwx-v21-{unsigned,signed}.hap`（本地另存一份到 `F:\WineHua\artifacts\`） |

### 1.1 新版本哈希（完整值）

| 对象 | 大小 | SHA256 |
| --- | ---: | --- |
| v21 FEX（stripped） | 3866624 | `a9b75e5e22a83779b2c65f2a6c7f095fe2eeb37778dc3dbc08ca77d2796e5ee3` |
| v21 FEX（unstripped） | 41730048 | `aa6b5c1747114e54e8bdef740163997de46f32db618a5a656c8a3281c5777cd1` |
| v21 payload（wine-data.zip） | — | `d6c6002054f389b3ab775e7745bd591c9ff9854de1e40cec20b323ed32afaa29` |
| v21 unsigned HAP | 353250236 | `cf5b8a5aeaaed4365ac1af6363095055bcc94d68b7e464a91856f163dc04ef24` |
| **v21 signed HAP** | 354553635 | `76556fe7fe8982b0c6c5a06c4fa3aa9a7144c9025443ad96a43adca939b61b01` |
| ntdll（配 v21 用的 v18 版） | 624464 | `4fa7ccf3e9b0d576e697abb6e5a0379db0ef994197e241035c290b3bff0801bd` |

打包报告 `artifacts/fex-rwx-probe/fex-rwx-v21-unsigned.json` 断言：相对 v12 baseline 只有 `libs/arm64-v8a/ntdll.so`、`resources/rawfile/wine-data.zip`、`resources/rawfile/wine-runtime-manifest.json` 三项变化；payload 内只换 `bin/aarch64-windows/libarm64ecfex.dll`。设备端 `/data/local/tmp/fex-rwx-v21-signed.hap` 的 sha256 与本地一致后才安装。

## 2. v21 链接失败的真实根因与修复（已解决）

0845 交接记录的是“链接报 `no such file or directory`”，只查到“缺 PATH”的线索。本轮确认并修好：

- `build/fex-rwx-probe/ec/.../link.txt` 第 2 行是**裸命令** `arm64ec-w64-mingw32-ar`，第 1、3 行用绝对路径；
- 容器默认 `PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`，`scripts/env.sh` 只解析 `LLVM_MINGW` 变量、**不导出它的 bin 目录**；
- 因此 gmake 调用 `ar` 时 ENOENT。补上 `export PATH="$LLVM_MINGW/bin:$PATH"` 后一次链接成功。

新增脚本（未提交，勿覆盖）：

- `scripts/build_fex_callret_v21.sh`：只在**已配置**的 `build/fex-rwx-probe/ec` 上重建；构建前断言三个候选标记已存在（Module.cpp / Dispatcher.cpp / BranchOps.cpp），构建后断言 stripped DLL 含 `[FEX-CALLRET-CONTROL]`，并打印哈希。它不 stage 补丁、不改 `build/fex-src`、不覆盖 v13–v20 探针产物。
- `scripts/verify_fex_callret_v21.sh`、`scripts/verify_hap_candidate.py`、`scripts/verify_hap_signature.sh`、`scripts/inspect_fex_callret_source.sh`：身份、签名、源码一致性核对。

构建成功日志（`v21-fex-rebuild.log` 结尾）：

```text
[ 98%] Linking CXX shared library ../../../Bin/libarm64ecfex.dll
[100%] Built target arm64ecfex
```

标记核对：`Module.cpp:791-792` 两行相邻（startup 与 control），stripped DLL 内 `FEX-CALLRET-CONTROL` 命中 1 次，v13–v20 探针 DLL 命中 0 次，故两者确为不同二进制。

**不要**用 `scripts/build_fex_rwx_probe.sh` 来接 v21（它会重跑 rwx 探针流程并把结果写成通用文件名 `libarm64ecfex-rwx-probe.dll`）。

## 3. 取证陷阱：共享 `wine_stderr` 不能判 FEX 版本

装 v21 后，共享日志 `temp/wine_stderr_20260921.log`（53 MB）里：

```text
grep -a -c "starting FEX based"      -> 57
grep -a -c "FEX-RWX-INIT"            -> 57
grep -a -o "FEX-CALLRET-CONTROL"     -> 0
grep -a -o "CALLRET"                 -> 252    (其它字符串，如 callret=1)
```

如果据此判断“v21 没生效”就错了。逐行核对发现该文件里 `starting FEX based` 的最后一批在 464316/474606 行附近，上下文是 pid=6850/11078 的旧会话；本会话的 FEX 初始化输出根本没进这个文件。

正确做法：导出该进程的私有日志。本轮用 0845 交接 §7.4 的 cmd.exe 复制通道导出 Steam 主进程（host PID 13642）：

```text
[NATIVE-PROBE] pid=13642 direct-stderr=1
[ntdll] OHOS rt_sigaction installed SIGSEGV/SIGBUS/SIGILL/SIGTRAP (bypass DFX)
[dlopen-trace] load_builtin_unixlib ... libarm64ecfex.so
starting FEX based libarm64ecfex.dll
[FEX-CALLRET-CONTROL] guest_returns=lookup ec_returns=lookup accounting=preserved
[WX-MPROTECT] pid=13642 req=0x7 rwx=1 jit_rc=0 first=OK base=0x7ffffe0000 size=16384
[FEX-RWX-INIT] pid=620 tid=624 smc_disabled=0
```

结论：v21 的返回目标缓存绕过在设备上**确实生效**，而且只读 rwx 探针与 ThreadTerm 基线同时在场。同时注意日志里 Windows pid（620）与 host pid（13642）混用，跨日志排序仍要用 `createMs/exitMs/lifetimeMs`。

另注：`wine_stderr` 里的 FEX 行**可能整段属于旧会话**；引用前先确认 `pid=` 属于当前会话，或直接改用私有日志。

## 4. 补齐的缺失证据与三类故障边界

0845 交接列为“尚未保存”的两份日志本轮已取回并分析。

### A. v20 browser 完整日志（host PID 54557，2069386 字节）

- 全文**没有** `FAULT-MIN`、`EXCEPTION_BREAKPOINT`，只有 1 次 SIGSEGV（rt_sigaction 安装行）；
- `--js-flags=--no-opt` 在 renderer 命令行中可见（与 0845 交接一致）；
- 尾部是 `[QUIT-PROBE] signal-enter / abort-thread / pthread-exit-wrapper`，与已知 IPC 超时（08:37:58 `Timed out waiting for mutex in PutInternal()`）后整组终止相符。

**边界：browser 自身日志里没有首发异常，不能把 CEF 终止当成根因证据；那只是终止动作。**

### B. v19 renderer 完整日志（host PID 46106，475462 字节）

- 无 `EXCEPTION_BREAKPOINT`、无 `FAULT-MIN`、无 `EXIT-PROBE`；
- 93 次 `[SMC] ... result=not_mine`，`prot=---`（真实未映射或无权限访问），其后仍继续运行；
- 尾部同样是 `abort-thread / pthread-exit-wrapper`。

**边界：v19 这个 renderer 的退出不是 V8 帧校验那一类；它是“无首发异常的线程终止序列”。**

### C. v18 renderer 首发断点（本地复核，未新增设备实验）

`v18-renderer-38401.log:13335` 起：

```text
dispatch_exception code=80000003 (EXCEPTION_BREAKPOINT) addr=0000006FECD9D83A
 rax=0x00000000000000c8  rbp=0x0000000000000070      (expected=200, actual=112)
virtual_unwind backtrace: libcef.dll + 0x2E2D83A
virtual_unwind backtrace: libcef.dll + 0x32E6BA6
```

随后才进入 `[QUIT-PROBE]` 终止序列。与 0845 交接的判定一致：这是 `Deoptimizer::ComputeInputFrameSize()` 的 CHECK_EQ，属**栈帧元数据不一致**，首发点在 libcef 内。

### D. v20 renderer 日志的补充事实（host PID 54744）

该 renderer 有 **547** 条 `[FAULT-MIN] ... code=2`（SEGV_ACCERR），地址集中在 `0x6fbb64xxxx` 到 `0x6fbb68xxxx`（FEX JIT/SMC 区），全部被处理，进程继续运行；没有 breakpoint、没有 EXCEPTION_ACCESS_VIOLATION 记录。

**即：RWX/SMC 类 ACCERR 在该 renderer 里是常态事件，不能与致命故障混为一谈。**

## 5. v21 A/B 现状（部分完成）

配置（与 0845 交接 §9.1/§9.2 要求一致）：

- ntdll = **v18**（无 jitless/no-opt 后缀）、FEX = **v21 callret 绕过**；
- 实际命令行核对：本会话 renderer 命令行**没有** `--js-flags`（对照 v20 有 `--js-flags=--no-opt`）；
- 启动方式：`aa force-stop` → `aa start EntryAbility` → game Want（`launch_want.py`，`WINEHUA_CEF_NO_CRASH_HANDLER=1`、`WINEHUA_WINEDEBUG=-all,+seh`）。因为 HAP 内 payload 变了，Want 触发了 `applyRuntimeUpdate(false)` 自动重解压（约 3 分钟），随后才启动 Steam。

时间线（设备本地时间）：

| 时间 | 事件 |
| --- | --- |
| 09:24:03 | app 启动，检测到运行时可升级（`upgrade pending`），未自动起引擎 |
| 09:26:51 | game Want 到达，此时容器未就绪，`game launch blocked` |
| 09:26:54 / 09:27:06 | 运行时 FEX 重解压完成（`files/wine/...` 与 prefix `system32` 两份均为 v21 哈希） |
| 09:27:25 | `launching ... steam.exe` 到 `game launch done pid=13642` |
| 09:27:29–09:27:37 | webhelper 启动、`CreateBrowser`、renderer 启动（命令行无 `--js-flags`） |
| 09:27:47 | SteamUI `OnLoginStateChange 0 1 0 0`（登录界面状态） |
| 09:36:54 | 23–25 个 Wine 子进程仍全部存活，私有日志仍在增长 |

存活对照（同类“启动后静置”条件）：

| 版本 | JS 配置 | 观察结果 |
| --- | --- | --- |
| v18 | 正常 JIT | renderer 38401 在约 4.5 分钟后 V8 CHECK 断点退出 |
| v19 | `--jitless` | 主库 renderer 存活约 15 分钟；其它 renderer 61 s 退出 |
| v20 | `--no-opt` | 约 4.4 分钟后 browser/gpu/renderer 整组退出（IPC 超时） |
| **v21** | **正常 JIT + callret 绕过** | **≥10 分钟全部存活**（含 4 个 renderer/gpu 角色） |

**边界：这是“静置”对照，不是库操作对照。0845 交接 §9.2 要求的库操作、游戏详情、启动选项 A/B 本轮没能执行**，原因是 §6。

## 6. 新阻塞：会话在跑，但界面里看不到 Steam 窗口

现象与证据：

1. 三张相隔数分钟的截图**字节完全相同**（`v21-shot2/3/5.png`，sha256 `05b987daf05f…`，80465 字节）：只有蓝色桌面加底部任务栏（“开始”），**没有任何 Steam 窗口**；同一时刻 CEF 进程在活跃写日志。
2. WL_Server 周期性窗口报告只有任务栏：

```text
STEAM-WINDOW: window=(13535,24) toplevel=2 geometry=1280x20+0,780 visible=1 binding=none producer=0x0 extent=0x0 contentSource=SHM
BIND-PRODUCER-STATE: producer=0x36b90000002c extent=32x32 ... binding=none lastReject=NO_WINDOW_ROLE
BIND-PRODUCER-STATE: producer=0x36610000002a extent=110x32 ... binding=none lastReject=NO_WINDOW_ROLE
```

   即桌面 1280x800 生产者缺失，任务栏窗口 `producer=0x0`，两个陈旧小生产者一直被 `NO_WINDOW_ROLE` 拒绝。
3. WineWM 侧能看到 toplevel：本会话存在 `#4 event=created` 与 `event=title data={"title":"Steam"}`，且是 `Desktop mode ... skipping Ability`；也就是说 **Wine 认为窗口在，Wayland 侧那份 surface 没有可用缓冲**。
4. 跨版本比较：`v16-current.png`、`v17-a.png` 与本次 `v21-shot5.png` **SHA256 完全相同**；v20 的 `v20-ui3.png` 也是空桌面（但 Steam 主窗在 `v20-steam.png` 里是黑矩形）；只有 v19（jitless）的 `v19-library.png` 是正常库界面。

**结论（明确区分已证与未证）**：已证“本会话 OHOS 可见画面不含任何 Wine 窗口，且该状态在 v16/v17/v20 也出现过”；未证“桌面 surface 是否真的冻结”（也可能是不含窗口的确定性桌面帧），也未证该现象与 FEX 或 JS JIT 的因果关系。它**阻断了 UI 驱动的 A/B**，本身就是待查问题。

另外记录一个行为：应用会自行把 `DesktopAbility` 切到后台（`isForeground: false, state 4`），截图因此可能拍到 HarmonyOS 桌面（`v21-shot1/4.png`）。判断界面状态前先确认前台 ability。

## 7. 接手建议（按优先级）

1. **先解决“窗口不显示”**，否则 §9.2 的库操作 A/B 无法做。建议顺序：先用一个 Windows 侧窗口枚举器（`EnumWindows` 加 `IsWindowVisible` 加 `GetWindowRect`）判断 Steam 主窗在 Win32 侧是否 visible、尺寸是否正常，从而把问题切成“Wine 侧没画”还是“Wayland/绑定侧没接”。现有 `smoke/winehua_win32_driver.exe` 只做“找窗口加点击”，不能枚举；`tools/steam-rwx/window_snapshot.exe` 仍未证明可运行，别重复盲试。
2. UI 可用后再做 v21 与 **v18 baseline**（`fex-rwx-v18-signed.hap`）的返回缓存绕过 A/B，多次比较库操作、详情、启动选项，并按 0845 交接 §9.3 分别记录三类首发证据。
3. 判 FEX/诊断版本一律用私有 `wine-native-<hostPID>.log`；`wine_stderr` 只能当交叉参考。
4. 未做的游戏验收仍在队列里：本例是 32 位 RPG Maker VX Ace/RGSS3，需要单独确认 `Game.exe` 进程与游戏窗口。
5. v21 只是诊断对照：不要因为“静置 10 分钟没崩”就把它或 callret 绕过当成产品修复。

## 8. 证据索引

证据目录 `F:\WineHua\device-evidence-20260921-v21`（命令与输出对应关系见 `commands.jsonl`）：

| 想回答的问题 | 文件 |
| --- | --- |
| v21 是否真的加载 | `v21-steam13642-native.log`（第 6–7 行）、`export-steam13642.log`、`check-export-13642.log` |
| 装机前后身份 | `v21-pre-status.log`、`v21-pre-df.log`、`v21-device-hash.log`、`v21-install.log`、`v21-fex-check.log`、`v21-fex-two-copies.log` |
| 启动与参数 | `v21-launch-steam.log`、`v21-gametest-log.log`、`v21-cef-js-tail.log`、`v21-console-tail2.log` |
| 存活与进程 | `v21-ps-all.log`、`v21-state1/2/3.log`、`v21-final-state.log`、`v21-temp-list.log` |
| 界面不显示 | `v21-shot2/3/5.png`、`v21-shot1/4.png`、`v21-hilog-bind.log`、`v21-binddiag.log`、`v21-wm-tail-ts.log`、`v21-steamwindow-now.log` |
| 补齐的旧证据 | `v20-browser54557.log`、`v19-renderer46106.log` |
| v21 构建 | `v21-fex-rebuild.log` |

关键文件哈希：

| 文件 | SHA256 |
| --- | --- |
| `v21-steam13642-native.log` | `b4c9389feb4fbe554c86173a452531b439ac12993a6cfe429fe5431973ad65b9` |
| `v20-browser54557.log` | `62738c41085bff0ae23e45d653f890b415f2cd117baff4643d0749802fe04598` |
| `v19-renderer46106.log` | `4492d0a4eb4ec930c1a85874c33b156fb571f42c7bb65b120f6814afde03715d` |
| `v21-shot5.png` | `05b987daf05f5b1316b2b2ad4d427dc8d4ecc3443458f52cc2cd15964845183a` |
| `v21-fex-rebuild.log` | `252d5f81929696e3bd61453c24f691df6880147fac9976a551c6292e101bc4de` |

本轮未删除设备文件、未改用户游戏数据、未重建显示链、未改 Wine 源码；新增的都是本地脚本与证据（`scripts/build_fex_callret_v21.sh`、`scripts/verify_fex_callret_v21.sh`、`scripts/verify_hap_candidate.py`、`scripts/verify_hap_signature.sh`、`scripts/inspect_fex_callret_source.sh`，均未提交）。

## 9. 补充（09:39–10:06 实测）：“有进程但没有画面”的现场判定与恢复

§6 的“空桌面”在本轮被查清并恢复。结论是：**这不是合成器丢帧，而是 Windows 侧根本没有窗口**；恢复后画面正常（`v21-library-restored.png`：Steam 库 + 游戏详情页）。

### 9.1 三步判定（可复用）

1. **Wayland 服务端有几个 toplevel**：`hilog -x -T WL_Server | grep WL-STAT` → 当时是 `toplevels=3 surfaces=3 renderers=1`，只有 explorer 的桌面 root（#1，按设计不算可见 toplevel）、任务栏（#2，1280x20）、以及一份无帧的 1280x800（#3）。Steam 的窗口不在其中。
2. **Windows 侧到底有没有可见窗口**：用 payload 自带的驱动做只读探测（不改状态）：

```powershell
python -X utf8 F:\WineHua\proton-ohos-worktree\tools\steam-rwx\launch_want.py `
  --target 192.168.180.71:36875 --artifacts <证据目录> --label drv-probe-steam `
  --exe 'C:\smoke\x64\winehua_win32_driver.exe' `
  --arg=--attach --arg=--title-prefix --arg=Steam `
  --arg=--client-x-permille --arg=500 --arg=--client-y-permille --arg=500 --arg=--timeout-ms --arg=20000
```

   结果落在 `C:\windows\temp\winehua_win32_driver.log`：`target window or button not found` —— **Wine 层没有标题以 Steam 开头的可见顶层窗口**。
3. **进程身份**：`cat /proc/<hostPid>/comm` 可读（`ls /proc` 遍历 + `comm`），本轮确认 13535/13548=explorer.exe、13642=steam.exe、13772=CrBrowserMain、13810=CrGpuMain、14020=CrRendererMain 等。`ps -ef` 只显示 `Native_libwine_childNN`，**不能**靠它识别 Wine 进程。

### 9.2 根因：Steam 客户端卡死，窗口被销毁后没有重建

- WineWM 的 toplevel 事件：`09:27:27.342 #4 created + title="Steam"` → `09:27:29.438 #4 destroyed`，之后**再没有任何 created 事件**（直到恢复操作）。
- Steam 自己的日志 `logs\steamui.txt` / `transport_steamui.txt`：

```text
[09:32:27] Warning: SteamUI thread frame stalled for: 30720 ms
[09:36:11] Warning: SteamUI thread frame stalled for: 132050 ms
[09:43:01] Warning: SteamUI thread frame stalled for: 349428 ms
```

   SteamUI 线程从约 09:31:56 起不再出帧，**累计 349 秒仍未恢复**（历史会话同类告警只有 5–14 秒）。
- 再发一次 `steam.exe` 没有用：单实例握手把请求交给已卡死的客户端，9:54 那次重启**没有产生任何新 toplevel**。
- `tasklist /v` 走 Wine 自带实现，**不输出窗口标题列**，不能用它判窗口；`winehua_win32_driver.exe` 的探测才是有效证据。

### 9.3 恢复步骤（已实测有效）

1. 在会话内结束卡死的客户端（Wine 运行时 bin 里自带 `taskkill.exe`）：

```text
C:\windows\system32\cmd.exe /c taskkill /f /im steam.exe
```

2. 再用 game Want 重新拉起 `C:\Program Files (x86)\Steam\steam.exe`（同一套显式诊断 env）。
3. 新会话立刻重建窗口：`#6 "登录 Steam"`、`#7 "Steam"`(1280x800)、`#8 登录弹窗`(300x650)，WL_Server 显示 `binding=bound producer=0x… extent=1280x800`。
4. 点一次窗口内的“库”标签后，UI 正式加载（JS 日志出现 `libraries~…js`、`Fetching PlayNext`），画面恢复为正常库页面。

**注意**：重启后有一段“窗口在但内容全黑”的过渡期（10:00–10:04），此时 `steamui_html.txt` 在重复 `RetryCreateBrowser id:… type:12`，主窗显示占位页 `data:text/html,<body></body>`。它会自行走完并加载真实 UI，不要误判成永久黑屏。

### 9.4 黑内容归因证据（区分“客户端没画” vs “合成器没上屏”）

合成器有 `FRAME-DUMP` 诊断，会把收到的 SHM 帧落盘（`temp/frame-w<W>-h<H>-own<pid>-<surf>.raw`，同一窗口覆盖）。本轮取回并解码：

| 帧文件 | 头部 | 内容 |
| --- | --- | --- |
| `frame-w1280-h800-own26322-30.raw` | `W 1280 H 800 FMT 1 TLV 7 PIX 4096000 SERIAL 4` | **100% 纯黑** |
| `frame-w300-h650-own26322-37.raw` | `W 300 H 650 FMT 1 TLV 8 PIX 780000 SERIAL 2` | **100% 纯黑** |

同时 STEAM-WINDOW 里主窗 `binding=bound` 但 `producerAgeMs` 持续增长（>120 s 无新帧），登录弹窗 `producerAgeMs=0`。**即黑内容来自客户端交付的像素，不在合成器**；应用自己也会把它归类为 `CEFBlackContent`。

### 9.5 工具与路径经验（给下一次省时间）

- **Z: 盘 = `/storage/Users/currentUser/Download/app.hackeris.winehua/`**（用 `winepath -u Z:\` 得到）。Wine 能读写它，但 **hdc shell 看不到这个路径**。
- hdc **不能**写入 prefix（`files/.wine/...`）和应用 `temp/`（SELinux 拒绝），所以“往会话里塞一个新 exe”最终要走 payload/重打包；`\\?\unix\data\local\tmp\...` 经 Want 传参时前导 `\\` 会被吃掉（cmd 收到的变成 `C:\?\unix\...`），**不要再用该前缀做复制**。
- 想验证 Windows 侧进程/窗口，优先用 payload 自带工具：`tasklist.exe`（无窗口标题列）、`taskkill.exe`（可结束卡死客户端）、`winepath.exe`（路径换算）、`winehua_win32_driver.exe`（找可见窗口）。
- 本轮为此新增 `tools/steam-rwx/window_enum.c` + `scripts/build_window_enum.sh`（x64，含不可见窗口枚举 + 进程 exe 名 + 样式），但**尚未成功投入设备**（受上面的写入限制）；若下次要长期使用，应把它并入 payload 的 `smoke/x64/`。

### 9.6 对 §5/§6 结论的修正

- §6 说的“画面停在桌面帧”修正为：**Windows 侧无窗口**导致合成器只有桌面背景与任务栏可画；`v16/v17` 出现同一张桌面图是同类现象，不能当作 v21 的显示回归。
- v21 的 CEF 存活结论不变（静置 ≥10 分钟全部存活），但**它不代表 UI 可用**：本会话的 UI 线程最终彻底卡死，需要人工重启客户端才恢复。

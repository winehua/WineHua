# 专家交接：Steam win64 显示中断的真实原因定位（截至 2026-09-20 21:30 CST）

> **最新交接：[2026-09-21 交接资料](F:/WineHua/proton-ohos-worktree/docs/steam-win64/EXPERT_HANDOFF_20260921_0845.md)。**
> 装机 v20 仍有 CEF 退出；v18 V8 帧校验、v20 IPC 超时、Metrics 退出故障须分开分析；
> v21 返回目标缓存绕过对照链接失败、未部署；游戏尚未确认运行。本文以下保留历史记录。

> **2026-09-21 实测纠正：本文保留为历史记录，不能继续照 §4.4/§7 的假设修改 FEX。**
> staged FEX 经 strip 后与设备原版 SHA256 完全一致，设备已包含 ThreadTerm 补丁。
> v13 的 FEX 只读插桩确认 guest JIT 页 `x=1 rwx=1 smc_disabled=0`，tracker 成功处理 SMC；
> call-ret 也有 `handled=1`。Wine 的 `result=not_mine` 是 FEX SEH 执行之前的日志，不能作为最终判定。
> 已确认并修复 Wine 诊断器直接读取 FEX 栈顶 guard 页导致的二次 SIGSEGV；
> 后续仍有 CEF/native 崩溃，参见 [本轮修复与证据](FEX_NATIVE_FAULT_REVIEW_20260921.md)。

> 对象：接手继续推进的专家。本文只写**已核实的事实**、**可照抄的构建/部署方法**、
> **本轮全部中间结果**，以及**下一步的两个候选修复点**。前一份交接
> `AI_HANDOFF_20260920_FONT_INPUT_WINDOWS.md` 仍然有效（本文件是它的延续与升级）。

## 0. 一句话现状

显示链（合成/窗口绑定）已经修好并可自愈；**唯一的阻塞**是：
**CEF 子进程（renderer / gpu-process / browser）在 ARM64EC（x64 via FEX）下反复 SIGSEGV**，
表现就是"主界面/好友列表黑、跑一会儿不显示"。

已把故障动作收敛到：guest 访问**匿名可执行代码页**时
`SIGSEGV, si_code=SEGV_ACCERR(2)`：

- **写 `r-xp`（可执行、不可写）页** —— renderer 崩溃前实测 12 次；
- **从 `rw-p`（可写、不可执行）页取指** —— 同一轮另有若干次；
- 两类都发生在 `0x6fbb6xxxxx` / `0x37bb6xxxxx` 这类**匿名小块**里（maps 里 r-xp/rwxp 交替）；
- **OHOS 并不禁止 RWX**（实测 `mprotect(PROT_READ|PROT_WRITE|PROT_EXEC)` 成功，见 §5 v12）；
- FEX 侧的三级 RWX 处理链（CallRetStack / JITGuardPage / InvalidationTracker）**一级都没认领**这些 fault。

## 1. 环境与入口

| 项目 | 值 |
| --- | --- |
| Windows 工作区 | `F:\WineHua`，PowerShell |
| 开发树 | `F:\WineHua\proton-ohos-worktree`（WSL：`/home/liufeng/src/WineHua-proton-ohos`） |
| 分支/HEAD | `feature/proton-wine-ohos` / `32efc9c`（**工作树大量未提交修改，本轮新增改动都在里面**） |
| 构建容器 | `wineohos-build`，源码挂载 `/data/src/winehua` |
| OHOS 工具 | 容器内 `/apps/harmony`（`TOOL_HOME`）、SDK `…/sdk/default/openharmony`、strip 工具 `…/native/llvm/bin/llvm-strip` |
| 平板 | hdc target `5KPBB25818203996`，bundle `app.hackeris.winehua`，Ability `EntryAbility` |
| App 文件根 | `/data/app/el2/100/base/app.hackeris.winehua/files`（prefix `.wine`，container `default`） |
| Wine stderr（关键日志） | `temp/wine_stderr_20260920.log`（全部 wine 子进程共用，**行会交错**） |
| 证据目录 | `F:\WineHua\device-evidence-20260920-steam-review`（本轮全部原始证据） |

## 2. 已确认结论（分级）

### 2.1 显示链：已修好，勿回退

改动（App native 侧）：

| 位置 | 改动 | 作用 |
| --- | --- | --- |
| `entry/src/main/cpp/compositor/frame/zc_bridge.{h,cpp}` | 新增 `NoteProducerPresent()`（真实 present 时间表，独立小锁）/`NoteLayerConsumed()`；`ResolvePresentBinding` **不再**用查询时刻刷新 `lastProducerUs` | 修复"失效 producer 永远显示活跃 → 新 present 表面永远无法接管窗口" |
| 同上 | `PruneStaleWindowBindings()`；接管守卫读真实 present 时间；**退役收尾只在窗口仍指向该被退役 producer 时才删窗口映射** | 修复"BIND-RETIRE 后窗口立刻 binding=none → 回退黑 SHM"（现场 17:14:02.745 → 17:14:03.332） |
| `entry/src/main/cpp/graphics/egl_renderer.{h,cpp}` | 连续 ≥2 次 `update failed` → `ReleaseZeroCopyBinding()` 重建消费者；保守的陈旧消费者重建；像素诊断 `DumpZeroCopyLayerPixels()`（落 `temp/zc-pixel-dump.txt`） | 打破"app 取走 frameAvailable 后不再重试 / guest 队列占满"的互等；提供"纹理黑 vs 合成黑"像素证据 |

证据：`BIND-TAKEOVER … (old producer stalled)`、`BIND-RETIRE … replaced_by=…`、`consumer attached` 均出现；
`zc-pixel-dump.txt` 健康段 `nonblack=16384/16384 mean_luma=41..107`；用户实测"库界面能显示了"。

### 2.2 崩溃链：已定位到 FEX 的 RWX 处理路径，尚未修完

观测事实（均有原始证据）：

1. renderer/gpu-process 以 `signal=11` 退出，且崩溃前有大量 `sig=7`（未对齐，FEX 正常处理）；
2. **摘掉 CEF 崩溃处理器**（§4.5 的 `WINEHUA_CEF_NO_CRASH_HANDLER=1`）后，致命 fault 才出现在
   Wine 信号路径：`[early-fault] #N pid=… sig=11 code=2 addr=0x6fbb6xxxxx`；
3. `fault-maps-<pid>.txt` 显示这些地址落在**匿名 r-xp / rw-p 小块**（如 `6fbb600000-6fbb6c2000`
   一串交替 r-xp/rwxp，其后 512MB `---p`）；
4. `[SMC-CODEWRITE]`（只读对照）：renderer 崩溃前 **12 次写 `r-xp` 页**；
5. v10 提升 maps 刷新后，同一 renderer 的致命 fault 出现**第二类**：`addr` 落在 `rw-p`（取指故障）；
6. `[WX-MPROTECT]` 实测：`req=0x7`(RWX) `first=OK`、`jit_rc=0` —— **RWX 没被拒**（多个子进程同形）；
7. FEX 源码（`build/fex-src/Source/Windows/ARM64EC/Module.cpp:890-930`）在 `EXCEPTION_ACCESS_VIOLATION`
   上有三级处理：`CallRetStack::HandleAccessViolation` → `JITGuardPage::HandleJITGuardPage` →
   `InvalidationTracker->HandleRWXAccessViolation(Thread, Pc, FaultAddress)`；第三级**只认自己跟踪区间**
   （`Source/Windows/Common/InvalidationTracker.h`：`XIntervals`/`RWXIntervals`，另有 `DisableSMCDetection()`）。

已排除的假设（都有实测反证）：

| 假设 | 反证 |
| --- | --- |
| 窗口绑定/合成器黑屏 | 绑定全程 `bound`，且已能自愈（`BIND-TAKEOVER`/重绑成功） |
| FEX call-ret 栈失衡 | 专门加 `[CALLRET-FAULT]` 分类，多轮 0 命中 |
| OHOS 拒绝 RWX（权限降级） | `[WX-MPROTECT] req=0x7 first=OK`（RWX 成功） |
| 我们自己的"权限互切"能修 | v9/v10/v11 实测：互切把故障从"写 r-x"搬到"执行 rw-"，进程照死 |
| DXVK 2.6 | 设备不支持 Vulkan 1.3，按用户决定不测 |
| CEF 软件模式（`--use-gl=disabled`） | 实测 SHM 全黑 + CEF 全家 SIGSEGV，且 CEF126 报 `Requested GLES 3.0 > max 2.0` |

## 3. 设备当前状态

- 已安装候选：**v12** `probe-candidate-signed-v12.hap`
  SHA256 `e6634214d0773b0712c89ae0058d1ee515dcd52f86c65184ab64f4aec1ca0a61`
- 设备 `files/wine/.winehua-runtime-manifest.json` 的 `payloadSha256` 仍为
  `b651fac62b107e0235fd06c2da8ea977ecde862c766cad0111f109a8c4269d1e`
  （所有候选都**没有**触发 runtime 重解包）
- App 宿主在跑（21:25 快照）；Steam 是否在跑取决于最近一次 want（见 §4.5 陷阱 1）

## 4. 构建 / 打包 / 部署（可照抄）

### 4.1 App native + ArkTS

```bash
docker exec -w /data/src/winehua wineohos-build bash scripts/w1-m3-build-hap.sh
# 产物：entry/build/default/outputs/default/entry-default-signed.hap（增量 ~10-15s）
```

注意：这条打出的 HAP 内 **wine payload 是旧的**，直接装会把设备运行时换成旧版 →
**必须用 §4.3 的隔离打包**。

### 4.2 Wine unix builtin（`libs/arm64-v8a/*.so`）

```bash
docker exec -w /data/src/winehua/build/wine-ohos-aarch64 wineohos-build make -j8 dlls/ntdll/ntdll.so
docker exec -w /data/src/winehua wineohos-build bash -lc \
  "cp build/wine-ohos-aarch64/dlls/ntdll/ntdll.so artifacts/probe-candidate/ntdll-probe.so && \
   /apps/harmony/sdk/default/openharmony/native/llvm/bin/llvm-strip artifacts/probe-candidate/ntdll-probe.so"
```

Wine 的 unix builtin（`ntdll.so`/`win32u.so`）是 **HAP 顶层条目** `libs/arm64-v8a/<name>.so`，
不在 `wine-data.zip` 里；PE dll 才在 payload（`bin/aarch64-windows/…`）。

### 4.3 隔离候选打包（本轮一直用的方式，payload 不动）

```bash
docker exec -w /data/src/winehua wineohos-build python3 scripts/package_probe_candidate.py
# 基线：artifacts/steam-font-contract/font-candidate-unsigned.hap（payload == 设备当前运行版）
# 替换：libs/arm64-v8a/libentry.so、libwine_child.so、ntdll.so（artifacts/probe-candidate/ntdll-probe.so）
#       + ets/modules.abc（ArkTS 白名单等改动）
# 输出：artifacts/probe-candidate/probe-candidate-unsigned.hap（脚本断言"只有这些条目变化"）
```

签名（**输出含签名口令，只能重定向到私有日志，勿回显/提交**）：

```bash
docker exec -w /data/src/winehua wineohos-build bash -lc \
 "export TOOL_HOME=/apps/harmony; source scripts/env.sh >/dev/null 2>&1; \
  python3 sign.py artifacts/probe-candidate/probe-candidate-unsigned.hap \
  artifacts/probe-candidate/probe-candidate-signed.hap > /tmp/sign.log 2>&1; echo rc=\$?"
```

部署 + 校验：

```powershell
wsl -d Ubuntu-22.04 -u root -- bash -lc 'cp -f /home/liufeng/src/WineHua-proton-ohos/artifacts/probe-candidate/probe-candidate-signed.hap /mnt/f/WineHua/artifacts/<name>.hap'
hdc -t 5KPBB25818203996 file send F:\WineHua\artifacts\<name>.hap /data/local/tmp/<name>.hap
hdc -t 5KPBB25818203996 shell 'bm install -p /data/local/tmp/<name>.hap -r'
hdc -t 5KPBB25818203996 shell 'grep payloadSha256 /data/app/el2/100/base/app.hackeris.winehua/files/wine/.winehua-runtime-manifest.json'
# 期望 b651fac6…（不变）
```

### 4.4 FEX（`libarm64ecfex.dll`）——**必须先建"行为干净"基线**

| 对象 | SHA256 | 说明 |
| --- | --- | --- |
| 设备/payload `bin/aarch64-windows/libarm64ecfex.dll` | `8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc` | **实机原版**（= `artifacts/steam-font-contract/libarm64ecfex-original.dll`） |
| `build/fex-ec/Bin/libarm64ecfex.dll` | `89297b45b4fabd49226d6ce968f59177bd07e3d8493f0929d42472705d425c1d` | **诊断版**（勿直接上机） |
| `build/fex-src/` | — | staged 源码：含 `[FEX-UNALIGNED]` 诊断 + Steam trace，**并含 `ThreadTerm` 行为补丁**（`Source/Windows/ARM64EC/Module.cpp:1221`） |

步骤建议：

1. 拷贝 `build/fex-src` 到 scratch（勿在原地改），反向应用行为补丁
   （`scripts/patches/fex-arm64ec-threadterm-context.patch`、`…-get-context-right.patch`
   以及任何非日志补丁），保留日志类补丁；
2. `scripts/build_fex.sh` 重建，产出与实机 `8aba586e` **对照**（至少确认差异只在日志/诊断）；
3. 再加只读插桩（§7），最后用"payload 内单文件替换"打包：在 `scripts/package_probe_candidate.py`
   基础上加一段 —— 解压 `resources/rawfile/wine-data.zip` → 替换
   `bin/aarch64-windows/libarm64ecfex.dll` → 重算 `wine-runtime-manifest.json` 的 `payloadSha256`
   → 重新签名。**装机会触发 runtime 重解包**（payload 变了），可接受。

### 4.5 起 Steam 与观测（两个已知陷阱）

```powershell
hdc -t 5KPBB25818203996 shell 'aa start -b app.hackeris.winehua -a EntryAbility \
 --ps winehua.mode game --ps winehua.game_path C%3A%5CProgram%20Files%20%28x86%29%5CSteam%5Csteam.exe \
 --ps winehua.container_id default \
 --ps winehua.d3d_env_count 1 --ps winehua.d3d_env_key0 WINEHUA_CEF_NO_CRASH_HANDLER --ps winehua.d3d_env_value0 1'
```

- **陷阱 1（want 被丢）**：安装后第一次 `aa start` 常常只起了 `desktop`；判定 =
  `Steam/logs/webhelper.txt` 是否出现新的 `Startup - webhelper launched`；没起就**再发一次同样 want**。
- **陷阱 2（env 白名单）**：`winehua.d3d_env_*` 的键必须在
  `entry/src/main/ets/game/GameHook.ets` 白名单里，否则日志出现
  `GameTest: ignoring invalid D3D environment key=…` 并静默丢弃（本轮踩过，
  `WINEHUA_CEF_NO_CRASH_HANDLER` 已加入）。
- `WINEHUA_CEF_NO_CRASH_HANDLER=1` 的作用：给所有 `steamwebhelper.exe` 系进程追加
  `--disable-breakpad --disable-crash-reporter --noerrdialogs`；**否则致命 fault 被 CEF 自己的
  崩溃处理器截获，Wine 侧看不到 sig=11 现场**（诊断专用，勿产品化）。

观测点：

| 目标 | 命令/文件 |
| --- | --- |
| CEF 子进程生死 | `hdc … shell 'hilog -x -P <appBrokerPid>'` → `CEF-UTILITY-CREATE/EXIT`（含 lifetimeMs/signal） |
| Wine 侧故障 | `hdc … shell "grep -E 'SMC|CODEWRITE|WX-MPROTECT|early-fault' /…/temp/wine_stderr_20260920.log \| tail"` |
| 崩溃进程 maps | `temp/fault-maps-<pid>.txt`（early-fault 时写，**是首次故障时的快照**） |
| 合成器像素 | `temp/zc-pixel-dump.txt` |
| 屏幕 | `hdc … shell uitest screenCap -p /data/local/tmp/x.png`（2560×1600；Wine 桌面 1280×800，坐标 ×2） |

## 5. 本轮中间结果汇总（全部候选只换 HAP 条目，payload 始终 `b651fac6…`）

| 候选 | 关键改动 | 中间结果 |
| --- | --- | --- |
| v1 `b5bd4848…`（libentry `984ac5b9…`） | 绑定真实活性 + 接管/退役修正 + 消费者自愈 | **用户实测：库界面能显示**；拖窗口后变黑 |
| v2 `bff0867d…`（libentry `aa1f4d2d…`） | 修"退役收尾误删新绑定" | 两窗稳定 `binding=bound`；不再回退黑 SHM |
| v3 `97559727…`（ntdll `ff097457…`） | `[SMC]` 加 `pid=`；归属配额放宽；解析 addr/x27/pc/lr | 崩溃首次可按进程归属 |
| v4 `ace1b985…`（ntdll `00507c3d…`） | 所有 SIGSEGV 现场提到限流前；加 call-ret 分类 | `[CALLRET-FAULT]` 0 命中 → **call-ret 假设排除** |
| v5 `857f505a…` | 同上精修（复位归属计数） | 同上 |
| v6 `ead5460f…` | `libwine_child` 加 webhelper 诊断参数注入 | 注入未生效 → 发现 **Want env 白名单**问题 |
| v7 `e7dbea73…` | `GameHook.ets` 白名单 + 候选带 `ets/modules.abc` | 注入生效（`[WineChild] webhelper diag args injected` ×2） |
| v8 `44645701…` | `[SMC-CODEWRITE]` 只读对照 | renderer 崩溃前 **12 次写 r-xp 页**（0x6fbb64xx–68xx） |
| v9 `def7dda2…` | 写故障恢复（r-xp→RW 后重试） | `mprotect_rw=0` 成功；**暴露"恢复用缓存 maps → 新页被跳过"的真 bug**；renderer 仍死 |
| v10 `9774bdb2…` | 恢复前**现刷 maps** | 恢复生效；致命 fault 出现第二类（`addr` 在 `rw-p` 取指）；renderer 仍死 |
| v11 `797d1838…` | **双向 W^X 互切**（r-x 写→RW；rw- 取指→R+X） | 互切成功但把故障搬到另一方向；renderer 仍死 |
| v12 `e6634214…`（当前装机版） | `[WX-MPROTECT]` 记录 RWX mprotect 结果；互切默认关闭（`WINEHUA_WX_RECOVER=1` 才开） | **`req=0x7 first=OK`：RWX 没被拒** → "OHOS 禁 RWX"排除 |

关键日志样本（便于专家快速定位格式）：

```text
[SMC] enter pid=22687 tid=22742 sig=11 code=2 addr=0x6fbb648194 pc_in=0x7d6f77c4c0 x27_in=0x0 ...
[early-fault] #12 pid=22687 tid=22687 sig=11 code=2 addr=0x6fbb648194 pc=0x7d6f77c4c0 lr=0x7d6f77fbf4 sp=0x20e90000
[fault-map] addr=0x6fbb648194 HIT 6fbb648000-6fbb649000 r-xp 00000000 00:00 0
[fault-map] pc=0x7d6f77c4c0 HIT 7d6f090000-7d73090000 rwxp [anon:FEXMemJIT]
[SMC-CODEWRITE] pid=12912 tid=12963 addr=0x6fbb643430 perms=r-xp region=0 xrip=0x87c pc=0x7d6eb341c4 sp=0x68ef0000
[SMC-WX-RECOVER] pid=26158 tid=26271 page=0x6fbb642000 was=r-xp now=3 region=0 rc=0
[WX-MPROTECT] pid=31149 req=0x7 rwx=1 jit_rc=0 first=OK base=0x7ffffe0000 size=16384
```

## 6. 关键代码位置

| 文件 | 内容 |
| --- | --- |
| `entry/src/main/cpp/compositor/frame/zc_bridge.{h,cpp}` | `NoteProducerPresent`/`NoteLayerConsumed`、`PruneStaleWindowBindings`、接管守卫、退役收尾 |
| `entry/src/main/cpp/graphics/egl_renderer.{h,cpp}` | 消费者自愈、像素 dump、`presentAgeMs` 归因 |
| `entry/src/main/cpp/proc/wine_child.cpp` | `apply_steam_webhelper_diag_args()`（崩溃处理器摘除，默认关） |
| `entry/src/main/ets/game/GameHook.ets` | d3d env 白名单（含 `WINEHUA_CEF_NO_CRASH_HANDLER`） |
| `thirdparty/wine-valve/dlls/ntdll/unix/ohos_virtual.{c,h}` | `[SMC]`/`[SMC-MAP]`/`[SMC-CODEWRITE]`/`[SMC-WX-RECOVER]`/`[WX-MPROTECT]`；`ohos_jit_enable_rc()`；`ohos_mprotect_exec()`；链式入口 `ohos_route_host_fault()` |
| `thirdparty/wine-valve/dlls/ntdll/unix/virtual.c` | `mprotect_exec()`（`force_exec_prot`/OHOS 分支）、`get_unix_prot()`（`PAGE_EXECUTE_READWRITE → VPROT_EXEC\|READ\|WRITE`） |
| FEX `Source/Windows/ARM64EC/Module.cpp` | `:890-930` 三级 ACCESS_VIOLATION 处理；`:1221 ThreadTerm`（行为补丁）；`:303/:814 InvalidationTracker` |
| FEX `Source/Windows/Common/InvalidationTracker.h` | `HandleRWXAccessViolation` / `XIntervals` / `RWXIntervals` / `DisableSMCDetection` |
| FEX `Source/Windows/Common/CallRetStack.h` | call-ret 栈 guard（已排除） |
| `scripts/package_probe_candidate.py` | 隔离候选打包（HAP libs + abc；断言其余条目字节不变） |
| `docs/steam-win64/ROUND3_20260920_BINDING_AND_32BIT_REF.md` | 本轮逐条证据与时间线（§7–§9.13） |

## 7. 专家下一步（按优先级）

先做 **FEX 只读插桩**（在 §4.4 的行为干净基线上），对 `0x6fbb6xxxxx` 这类地址打印：

1. `InvalidationTracker::QueryExecutableRange(addr)` 的结果（FEX 眼里这是不是可执行区间）；
2. `XIntervals` / `RWXIntervals` 是否命中、`HandleRWXAccessViolation` 的返回值；
3. `SMCDetectionDisabled` 当前状态、`DisableSMCDetection()` 何时被调用；
4. 前两级（`CallRetStack`、`JITGuardPage`）的返回值。

然后按结果修（二选一，或都做）：

- **区间不覆盖 guest JIT 页** → 让 guest 运行时分配的可执行页进入 invalidation/writable-page 跟踪；
  或在 `HandleRWXAccessViolation` 里用 `QueryExecutableRange` 做"兜底认领"；
- **SMC 检测被关闭** → 找 `DisableSMCDetection()` 触发条件并放宽/延后；
- 备选（不动 FEX）：在 Wine 侧让 guest 申请的 `PAGE_EXECUTE_READWRITE` 真拿到 RWX（实测内核允许）。
  **不要再用"故障后猜 W/X"的互切**——v9–v11 已证伪。

## 8. 其它陷阱清单

- 不要用 `entry/build/.../entry-default-signed.hap` 直接装机（payload 是旧的，会覆盖设备运行时）。
- 不要直接把 `build/fex-ec/Bin/libarm64ecfex.dll`（诊断版 `89297b45…`）替换进去。
- `sign.py` 会打印含口令的命令 → 只重定向到私有日志。
- 设备上遗留大量 `hilog … | grep` 挂起进程；别再用不退出的 hilog 管道。
- `wine_stderr` 是多进程共享、**行会交错**；按 `pid=` 过滤，别按行号定位。
- `ps STIME` 与设备 `date` 不一致；用 `date` + 日志时间戳 + 进程身份交叉判定。
- hdc shell（UID2000）读不到 `data/storage/el1/bundle/libs/...`（HAP libs 目录），
  只能读 app 自己的 `files/` 与 `/data/app/el2/100/base/<bundle>/temp/`。
- 抓图 2560×1600 是物理分辨率；Wine 桌面 1280×800，输入坐标按 ×2 换算。
- `WINEHUA_CEF_NO_CRASH_HANDLER`、`WINEHUA_WX_RECOVER` 都是**诊断开关**，默认关闭，勿产品化。

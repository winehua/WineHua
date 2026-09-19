# Steam win64 / ARM64EC-FEX 交接文档（2026-09-19 13:30）

> 目的：把"代码状态 / 已确认事实 / 已排除假设 / 下一步怎么做 / 怀疑点与验证方法"完整交给下一个
> Codex 会话，让它能直接接着干，不用重新摸索环境。
> 配套证据文档：`F:\WineHua\docs\STEAM_WIN64_FEX_ARM64EC_FIX_20260918.md`（§1–§20，持续更新）。
> 更早的两份：`F:\WineHua\steam-expert-handoff-20260916.md`、`F:\WineHua\steam-render-arch-review-20260917.md`。
>
> **落库位置**：本文件与证据链文档已复制进代码仓库
> `docs/steam-win64/`（主仓分支 `feature/proton-wine-ohos`），随仓库推送；
> `F:\WineHua\docs\` 下是同步的工作副本，改动后记得再复制进去提交。

---

## 0. 一句话现状

win64（x64 → ARM64EC/FEX）Steam **能启动、X64 执行链/字体/IPC 对象层都健康，但 UI 卡死在
"client 等 webhelper 就绪通知"这一步**：

```text
host(steam.exe)  : CreateBrowser → CreateResponse → ✗ BrowserReady（永远没有）
browser(webhelper): CreateBrowser → AfterCreated   → ✗ SetName / ✗ --type=renderer / ✗ popup
```

**最后一次成功是 09-18 14:52:50–14:55**（那次 VGUI 外框/菜单/页签可见、内容区黑）。
从 09-18 15:00 之后到今天（09-19）**每一轮都卡在同一处**，与启动参数/GPU 开关无关。

---

## 1. 环境与固定套路（这些别改，照抄）

| 项 | 值 |
| --- | --- |
| 工作目录 | `F:\WineHua` |
| 活跃源码树 | `F:\WineHua\proton-ohos-worktree` → WSL `/home/liufeng/src/WineHua-proton-ohos`（= 容器内 `/data/src/winehua`，已确认同 inode，bind mount） |
| 容器 | `docker exec wineohos-build bash -lc "cd /data/src/winehua && ..."` |
| 设备 | hdc 目标 `5KPBB25818203996`（USB，偶发掉线）；另有 TCP 目标 `192.168.180.71:36875`（**端口会变**） |
| hdc 路径 | `C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe` |
| 设备包名 | `app.hackeris.winehua` |
| prefix | `/data/app/el2/100/base/app.hackeris.winehua/files/.wine`（Steam 在 `drive_c/Program Files (x86)/Steam`） |

### 1.1 构建（**必须三步，缺一步就是旧运行时**）

```bash
#!/usr/bin/env bash
set -e
docker exec wineohos-build bash -lc "cd /data/src/winehua && NATIVE_ARCH=arm64-v8a bash scripts/build_wine.sh"        # wine 源码 ~20s
docker exec wineohos-build bash -lc "cd /data/src/winehua && NATIVE_ARCH=arm64-v8a bash scripts/w1-m2-assemble.sh"    # 运行时 payload ~45s
docker exec wineohos-build bash -lc "cd /data/src/winehua && NATIVE_ARCH=arm64-v8a bash scripts/w1-m3-build-hap.sh"   # HAP ~10s
```

- 只跑 `w1-m3` ⇒ wine/运行时没更新（踩过：`strings kernelbase.dll | grep enable-logging` 为空但日志无输出）。
- HAP 产物：`/home/liufeng/src/WineHua-proton-ohos/entry/build/default/outputs/default/entry-default-signed.hap`（≈347 MB）。
- guest Mesa 改动：`GUEST_GFX_MODE=virpipe bash scripts/build_ohos_guest_gfx.sh`；virglrenderer：`scripts/build_native.sh`。

### 1.2 装机 + 跑一轮（PowerShell 片段，实测可用）

```powershell
$hdc='C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
$dev='5KPBB25818203996'                     # 掉线时用 '192.168.180.71:36875'
& $hdc -t $dev file send 'F:\WineHua\tmp\winehua-wt.hap' /data/local/tmp/x.hap
& $hdc -t $dev shell 'bm install -p /data/local/tmp/x.hap -r'
```

装完**首次启动会重解包运行时（1–2 分钟）**，脚本里先 `force-stop` → `aa start` → `sleep 45` 再拉游戏。

```bash
# 运行一轮（12:49 那次"能出画面"的配置：force-gpu + no-cef-sandbox，参数走 game_arg_encN）
aa force-stop app.hackeris.winehua; sleep 8
aa start -b app.hackeris.winehua -a EntryAbility; sleep 45
aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode game \
  --ps winehua.game_path 'C%3A%5CProgram%20Files%20%28x86%29%5CSteam%5CSteam.exe' \
  --ps winehua.container_id default \
  --ps winehua.game_argc 2 \
  --ps winehua.game_arg_enc0 %2Dcef-force-gpu \
  --ps winehua.game_arg_enc1 %2Dno-cef-sandbox \
  --ps winehua.d3d_env_count N \
  --ps winehua.d3d_env_key0 KEY0 --ps winehua.d3d_env_value0 VAL0 ...
```

- `game_arg_encN` 是 **URL 编码**的参数（`game_argN` 传 `-cef-force-gpu` 这种会变空串，历史踩过）。
- env 必须过 `entry/src/main/ets/game/GameHook.ets` 里的**白名单**，否则静默丢弃。
- 屏幕休眠会让 `snapshot_display` 全黑：脚本里挂 `while true; do power-shell wakeup; sleep 15; done &`（App 侧也已产品化"前台常亮"）。

### 1.3 日志位置（注意按天滚动）

| 路径 | 内容 |
| --- | --- |
| `/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_YYYYMMDD.log` | Wine/我们所有 stderr（**按天滚动，取当天最新**） |
| 同上 `temp/cef-utility-diag.log` | CEF 子进程创建/退出（`key=browser/gpu-process/renderer/...`） |
| 同上 `temp/gl-capability.log`、`bind-diag-*.log`、`frame-*.raw`、`fault-maps-*.txt` | GL 能力、窗口绑定、落盘帧、fault 归属 |
| `…/Steam/logs/webhelper.txt` | browser(webhelper) 侧：`CreateBrowser / UB-<id>: AfterCreated / SetName / launching child --type=renderer / CreatingPopup` |
| `…/Steam/logs/steamui_html.txt`（+ `.previous`） | **host(steam.exe) 侧**：`CreateBrowser / CreateResponse / BrowserReady / PopupHTMLWindow / CreateMainWindow` |
| `…/Steam/logs/console_log.txt`、`connection_log.txt`、`cef_log.txt`、`shader_log.txt`、`steamui_system.txt` | Steam 自身日志 |

---

## 2. 代码状态

### 2.1 已落地并验证过的修复（**都要保留**）

| # | 修复 | 位置 | 证据 |
| --- | --- | --- | --- |
| 1 | FEX ARM64EC lookup cache 预提交 | `scripts/patches/fex-arm64ec-lookup-cache-commit.patch` + `build_fex.sh` | x64 探针可跑 |
| 2 | early-fault 诊断器不再自伤；按 (sig,pc) 去重、上限 12 | `entry/src/main/cpp/proc/wine_child.cpp` | 原来 4 个名额被启动期 SIGBUS 吃光 |
| 3 | tier0/vstdlib 导入重定向条件化 | `thirdparty/wine-valve/dlls/ntdll/loader.c` | 缺失导入 298→0 |
| 4 | 字体链 + 936 代码页自愈 `ensure_prefix_fonts_and_codepage()` | `wine_child.cpp` | winFont 断言 0 |
| 5 | `[SMC-MAP]` / `[SMC-NEIGHBOR]` fault 归属 | `dlls/ntdll/unix/ohos_virtual.c` | 判断 guard page 用 |
| 6 | CEF 子进程探针 `CEF-UTILITY-CREATE/EXIT/COUNT` | `entry/src/main/cpp/proc/cef_utility_probe.*` + broker/NCP | 落 `temp/cef-utility-diag.log` |
| 7 | GL 三层能力探测（P0-GL-1/2/3） | `graphics/gl_capability_probe.*`、`virglrenderer/vrend_winsys_egl.c` 等 | `temp/gl-capability.log` |
| 8 | **UBO +1 能力修复**（默认关，`WINEHUA_VIRGL_UBO_FIX=1` 开） | `thirdparty/virglrenderer/src/vrend/vrend_renderer.c` | 打开后 guest 才有 ES3 |
| 9 | **线程栈放大**（默认开） | `dlls/ntdll/unix/thread.c` | CEF 从"41/41 崩"变稳；**本轮 A/B 证明它不是当前卡点**，别回滚 |
| 10 | SDL2 补齐（随包官方 x86 SDL2 → `Steam/bin/`） | `entry/src/main/resources/rawfile/sdl2-x86.dll` + `WineEnvService.ensureSteamGpuProbeDeps()` | `gldriverquery.exe` 的 `0xC0000135` 消失 |
| 11 | 相对路径 → FEX 兜底（`is_steam_client_exe()`） | `wine_child.cpp` | `HODLL=libwow64fex.dll (steam-client override, exe=.\bin\vulkadriverquery.exe)` |
| 12 | 空闲卡死看门狗 `WINEHUA_STALL_DUMP=<秒>` | `wine_child.cpp` | `[stall-dump] tid/comm/wchan` |
| 13 | 前台屏幕常亮 | `entry/src/main/ets/service/ScreenAwake.ets` + `EntryAbility.ets` | 防锁屏导致 `aa start` 失败 |
| 14 | CEF 开关可配置 | `dlls/kernelbase/process.c` | `WINEHUA_CEF_FORCE_SOFTWARE=1/both/compositing/gpu/0`、`WINEHUA_CEF_LOGGING=1`（只加 `--v=1`）、`WINEHUA_CEF_ANGLE_BACKEND`、`WINEHUA_CEF_VMODULE`、`WINEHUA_CEF_STEAM_DISABLE_GPU`、`WINEHUA_CEF_UTILITY_PROBE` |
| 15 | GL proc dispatch 探针 `WINEHUA_GL_PROC_TRACE=1` | `dlls/win32u/winehua_gl_proc_trace.*`（挂 `egldrv_get_proc_address`） | 实测 893 条解析 / 507 入口 / NULL=0 |
| 16 | IPC 对象探针 `WINEHUA_IPC_TRACE=1/2` | `dlls/kernelbase/winehua_ipc_trace.*` | §13/§14：对象/事件层健康 |

### 2.2 2026-09-19 本轮新增（都在树上，已编译进 `winehua-wt.hap`）

| 改动 | 文件 | 作用 | 结果 |
| --- | --- | --- | --- |
| env 白名单补 `WINEHUA_MIN_THREAD_STACK_MB` / `WINEHUA_EMU_STACK_MB` / `WINEHUA_WAIT_TRACE` | `entry/src/main/ets/game/GameHook.ets` | 之前"栈回退"实验其实没生效（被白名单丢掉） | ✅ 修好 |
| 栈策略加"显式 0 = 上游行为" | `dlls/ntdll/unix/thread.c`（`winehua_env_mb`） | 支持 A/B：`WINEHUA_MIN_THREAD_STACK_MB=0 WINEHUA_EMU_STACK_MB=0` | ✅ 实测日志 `reserve=0 commit=0 min=0` |
| `[wait-trace]`：无超时 `NtWaitForSingleObject` 打点（每进程 64 条） | `dlls/ntdll/unix/sync.c`，env `WINEHUA_WAIT_TRACE=1` | 回答"谁在无限等" | ✅ 一轮 201 条；Steam.exe 主线程在不断无限等待（handle 0x40/0x7c） |
| guest(x64) 上下文采样 `[prof-guest]` + 栈扫描 | `dlls/ntdll/unix/thread.c: ohos_prof_dump_guest_context()`，在 `ohos_virtual.c` 的 `ohos_prof_handler` 里调用；app 侧 `wine_child.cpp` 把 SIGPROF 链式转给 wine 采样器 | 想拿"卡在哪个 Win32 API + 调用者" | ⚠️ 637 条全是 `src=none rip=0 rsp=0` ⇒ FEX 的 x64 上下文不在 CHPE v2 / WoW64 CPU area |

### 2.3 未提交改动范围（供 review）

- `thirdparty/wine-valve`：39 个文件 modified（`git status --short` 见下），从 09-12 起累积的 WineHua 移植改动 + 本轮探针。
- `entry/`（App 侧）：~30 个文件 modified，`entry/src/main/cpp/**`（compositor/graphics/proc/wine）与 `entry/src/main/ets/**`。
- `thirdparty/virglrenderer`：`src/vrend/vrend_renderer.c`、`vrend_winsys_egl.c`（+新文件 `winehua_gl_caps_log.h`）。
- `thirdparty/mesa`：`virgl_screen.c`、`dri_screen.c`、`vrgl_vtest_socket.c`、`main/version.c`、`st_extensions.c`、`vn_renderer_vtest.c` ——**全部是 `WINEHUA_GUEST_CAPS_LOG` 门控的只读诊断**（已核对 diff）。

---

## 3. 当前卡点：完整证据链

### 3.1 host 侧缺 `BrowserReady`（最硬的一条）

`steamui_html.txt`：

```text
# 最后成功（09-18 14:52:50 → 14:55）
14:52:50 CreateBrowser id:1646944249 type:12 flags:1000000 (-2147483648,-2147483648) 0x0
14:53:20 CreateResponse: id:1646944249 handle:65536
14:53:20 BrowserReady: handle:65536
14:53:30 GetDesiredSteamUIWindows: starting processing of 1 windows
14:53:31 PopupHTMLWindow: idx:131073 → BrowserReady:131073      # 登录窗 700x440
14:54:48 CreateMainWindow: UIMode -1 / creating shared JS context
14:54:52 PopupHTMLWindow: idx:196610 → BrowserReady:196610      # 主窗口 1280x800
（随后 friendslist / PopupWindow / contextmenu ×12）

# 现在（09-19 每一轮：09:58 / 10:12 / 10:16 / 10:27 / 10:34 / 10:38 / 11:01 / 11:10）
11:10:19 CreateBrowser id:2112057208 type:12 flags:1000000
11:10:29 CreateResponse: id:2112057208 handle:65536
（10 分钟无新行，没有 BrowserReady）
```

### 3.2 browser 侧停在 `AfterCreated`

两边前 7 行**逐字相同**，差别只在之后：

```text
可用: … CreateBrowser … → UB-65536: AfterCreated … → UB-65536: SetName: SP Shared JS Context
      → Browser - launching child process … --type=renderer → CreatingPopup …
现在: … CreateBrowser … → UB-65536: AfterCreated … → 静默
```

CEF 探针侧佐证：`key=renderer` 的创建请求 = **0**；`key=browser/gpu-process/network/storage` 各 1 次且长期存活。

### 3.3 所有线程都空闲（不是崩溃、不是忙等）

`WINEHUA_STALL_DUMP` dump：browser(113 线程)/gpu-process 全部 `wchan=hm_futex_wait_interruptible`，
`/proc/<pid>/task/<tid>/stack` 不可读（内核栈受限）。**两侧都在等对方** = 通知没送到。

### 3.4 `[wait-trace]` 实测

一轮 201 条无限等待；`Steam.exe`(pid 38419) 主线程反复 `handle=0x40 / 0x7c`，另有进程 39299 同样 64 条（到上限）。

### 3.5 时间线（回归窗口）

| 时间 | 事件 |
| --- | --- |
| 09-18 12:21–12:26 | native 模式：主窗口 `binding=bound`、producer 持续出帧，**内容区黑**、VGUI 外框可见 |
| 09-18 **14:52:50–14:55** | **最后一次完整可用**：登录窗 + 主窗口 + friendslist + contextmenu（`-no-cef-sandbox -cef-force-gpu`） |
| 09-18 15:00 起 | §19 P0-GL 工作开始；此后**再没出现过 SetName/renderer/popup**（最后一次 `CreatePopup` = 14:55:14） |
| 09-18 17:24–17:34 / 19:36–19:59 | webhelper 10s 一次的重启风暴（软件/ES3 实验期） |
| 09-18 20:50 | §19.7：栈修复后"CEF 不崩了、主窗口已建立但 visible=0"（**说明那一轮是走过去了的**） |
| 09-18 21:06 | §19.8：同配置却"renderer 从未启动" ⇒ **这条链有非确定性** |
| 09-19 09:58–11:30 | 8 轮全部卡在 AfterCreated，与参数/GPU/profile 无关 |

---

## 4. 已排除的假设（都有可复现的 A/B）

| 假设 | 实验 | 结论 |
| --- | --- | --- |
| 启动参数（缺 `-cef-force-gpu`） | `game_arg_enc0=%2Dcef-force-gpu` + `enc1=%2Dno-cef-sandbox` | browser 命令行**确实**出现 `--ignore-gpu-blocklist`（与 14:53 一致）→ **仍卡** |
| 关 GPU（产品默认软件路径） | `WINEHUA_CEF_FORCE_SOFTWARE=1` | 同样卡；`cef=4`、renderer 请求 0 |
| 线程栈放大引入回归 | `WINEHUA_MIN_THREAD_STACK_MB=0 WINEHUA_EMU_STACK_MB=0` | 日志确认真的回到上游 → **仍卡**；栈修复保留 |
| App 侧 Host GL 探测（14:28 新增、默认开） | `WINEHUA_GL_PROBE=0` | 仍卡 |
| CEF profile/缓存损坏 | 容器内 `ren htmlcache htmlcache.bak0919` | 仍卡（现在跑的是**全新 profile**） |
| 我们注入的 env | 零 env 基线 `dev_steam_clean.sh` | 仍卡 |
| WindowBinding / 尺寸规则 | 09-18 §15 探针：主窗口四套尺寸全相等、`binding=bound`、帧持续增长 | 不是 binding |
| GL proc / ES3 / UBO | §19.6：893 条解析 NULL=0；UBO 修复后 ES3 可用 | 已降级，不是当前卡点 |
| GL 日志通道注入副作用 | §18.6：`WINEHUA_CEF_LOGGING` 会自己制造 renderer 崩 | 评估 CEF 进程稳定性时**必须关掉它** |
| CEF utility 重启循环 | §17.2 按天拆计数：1384/389 是跨天累计；09-18 后各 1 次 | 不成立 |

---

## 5. 下一步建议（按性价比排序，都给了"怎么验"）

### P0 — 把"通知为什么没到"钉死（二选一或都做）

**P0-a 抓 x64→unix 转换点的 guest RIP（推荐先做）**

现状：`[prof-guest]` 读 CHPE v2 CPU area 与 WoW64 CPU area 都是 0 ⇒
`libarm64ecfex` 把 x64 上下文放在**自己的 CPUState** 里。可行做法：

1. 读 FEX 的 CPUState：在 `thirdparty/fex*`（或 payload 里的 `libarm64ecfex.dll`）找到 `CPUState` 结构，
   取 `RIP/RSP` 偏移，在 `dlls/ntdll/unix/thread.c: ohos_prof_dump_guest_context()` 里按该偏移读
   （CPUState 一般可从 TEB/CHPE v2 的 `EmulatorDataInline` 或 `NtCurrentTeb()` 关联处拿到）；
2. 或在 `__wine_unix_call`/ARM64EC thunk 入口记录 guest 上下文，再让 `NtWaitForSingleObject`
   的 `[wait-trace]` 带上调用者；
3. 拿到"谁在无限等"的调用者后，就能区分：**client 内部逻辑没发** vs **IPC 没送到**。

**P0-b 给 Steam HTML IPC 的"通知方向"打点**

- 已有 `WINEHUA_IPC_TRACE=1/2`（`dlls/kernelbase/winehua_ipc_trace.c`）能记录 Create/Open 时间线；
  复跑一轮，重点看：共享内存流（`SteamChrome_*` / `MasterStream`）是否被**反复重建**、
  事件对象是否在 CreateResponse 之后被 `SetEvent`、client 的 `HTMLController Commands` 线程
  （在 `steam.exe` 里，winedbg `info threads` 能看到该线程名）是否被唤醒。
- 注意 §12.2 早就记过"`SteamChrome_MasterStream` 反复重建"这条线，现在要把它接到"通知方向"上。

### P1 — 判定"是不是竞态/性能回归"（便宜，建议与 P0 并行）

1. **同一配置连跑 3 次**（§19.7 vs §19.8 的冲突说明有非确定性）；每次记录
   `steamui_html.txt` 是否出现 `BrowserReady`。
2. **诊断开销 A/B**：当前构建有大量**默认开启**的 stderr 打印（`[dlopen-trace]`、`[thread-start]`、
   `WineHuaWindowIdentity`、`[prof]/[prof-mod]`、`[SMC-*]`）。做一个"全关"变体（加 env 开关或
   直接编译期关掉），跑 3 次。若"全关"能恢复 → 说明是启动期时序/开销把某个竞态顶翻了，
   那就把这些打印改成默认关（顺便也是产品化该做的事）。

### P2 — 若 P0/P1 都没结论，用"构建变体二分"回到 14:55 的可用状态

14:55→17:24 之间**只有两处源码变化**（其余都是 17:24 之后）：

| 变化 | 时间 | 验证方式 |
| --- | --- | --- |
| `scripts/assemble.sh` 打包改动（GStreamer 插件 + `libav*` 进 `libs/<arch>/`、`runtime-components.json`、`cp -L`） | 14:56 | 用 `git stash`/`git checkout 2728523` 前的版本重出 payload，跑一轮；**这是该窗口内唯一动 payload 内容的改动** |
| App 侧 `gl_capability_probe.*` + `egl_renderer.cpp`(14:28)、`EntryAbility.ets`/`ScreenAwake.ets`(14:46) | 14:28/14:46 | 前者已用 `WINEHUA_GL_PROBE=0` 排除；后者（常亮开关）可临时注释掉再出一版 HAP |

> 提醒：不一定非得回到旧状态才能定位——如果 P0-a 拿到调用者，往往直接就能看出
> "这一步为什么没人做"，比二分更快。

### P3 — 产品层面的兜底（用户想要"能用的 Steam"时）

- 32 位客户端（i386）历史上是能出的（走 `--use-gl=disabled` 软件路径）。要阻断自更新需要
  `Steam/steam.cfg`（`BootStrapperInhibitAll=enable`）。**设备侧不能新建文件，但现在可以从容器内建**
  （见 §6.1 的 cmd 能力）：
  ```
  cmd /c "echo BootStrapperInhibitAll=enable> \"C:\Program Files (x86)\Steam\steam.cfg\""
  ```
  再用 i386 的 `steam.exe` 覆盖：**`Steam/steam.exe.old` 还在**（4 718 744 B，i386，09-18 13:29；
  当前 `steam.exe` 是 5 775 512 B 的 win64 版，09-03 10:37）。容器内：
  `copy /y "C:\Program Files (x86)\Steam\steam.exe.old" "C:\Program Files (x86)\Steam\steam.exe"`。
- 作为交付兜底可以，但**不是 win64 修复**，别把它当成结论。

---

## 6. 现成工具/能力（下一个会话可以直接用）

### 6.1 ✅ 容器内执行任意 Windows 程序（本轮新解锁，很好用）

`winehua.mode game` 只要求 `game_path` + `game_arg_enc1`（URL 编码的单条命令），就能在**同一个
prefix/wineserver**里跑命令；stdout/stderr 重定向到 `C:\`，再从设备侧读（SELinux 只拦"设备侧写
prefix"，不拦"Wine 内写"）。

```powershell
$cl='cd /d C:\users\steamuser\AppData\Local\Steam & ren htmlcache htmlcache.bak0919 > C:\mv2.txt 2>&1'
$enc=[uri]::EscapeDataString($cl)
& $hdc -t $dev shell "aa start -a EntryAbility -b app.hackeris.winehua --ps winehua.mode game --ps winehua.game_path C%3A%5Cwindows%5Csystem32%5Ccmd.exe --ps winehua.container_id default --ps winehua.game_argc 2 --ps winehua.game_arg_enc0 %2Fc --ps winehua.game_arg_enc1 $enc"
# 读回：hdc shell 'cat "/data/app/el2/100/base/app.hackeris.winehua/files/.wine/drive_c/mv2.txt"'
```

注意：**命令里不要带内层双引号**（App 会把整条参数再包一层引号，`cmd` 会解析错，实测
`move /y "a" "b"` 里什么都不干）；用 `cd /d <dir> & ren old new` 这种无引号写法。

### 6.2 winedbg 现状

- `winedbg.exe` 是真品（11.7 MB），`--help` / `--file` / `info process` 都能用，
  能列进程树（`Steam.exe` / `steamwebhelper.exe` ×4 …）。
- **`attach <wpid>` 会挂住**（只打印 `WineDbg attached to pid XXXX` 然后没有下文），
  所以拿不到反汇编级栈。若要用它，需要在别处解决"线程挂起不返回"（可能与我们改过的
  signal/sigchain 路由有关——本身也是一条值得查的线索）。

### 6.3 探针/开关清单（env 都在白名单里，除非另注）

| env | 作用 |
| --- | --- |
| `WINEHUA_CEF_FORCE_SOFTWARE=1/both/compositing/gpu/0` | 注入 `--disable-gpu*` |
| `WINEHUA_CEF_LOGGING=1` | 只加 `--v=1`（**会自己制造 renderer 崩，评估稳定性时必须关**） |
| `WINEHUA_CEF_VMODULE=<spec>` | 该 release 把 VLOG 裁了，实测无效 |
| `WINEHUA_CEF_ANGLE_BACKEND=vulkan/d3d11` | 指定 ANGLE 后端 |
| `WINEHUA_CEF_STEAM_DISABLE_GPU=1` | 注入 Steam 自己的 `-cef-disable-gpu` |
| `WINEHUA_CEF_UTILITY_PROBE=1` | CEF 子进程创建/退出探针 |
| `WINEHUA_IPC_TRACE=1/2` | Steam IPC 对象时间线 |
| `WINEHUA_GL_PROC_TRACE=1` | WGL/EGL proc 解析探针 |
| `WINEHUA_GL_PROBE=0` | 关掉 App 侧 Host GL 探测 |
| `WINEHUA_GUEST_CAPS_LOG=1` | guest Mesa caps/版本日志 |
| `WINEHUA_STALL_DUMP=<秒>` | 空闲卡死看门狗（线程 comm/wchan + 采样） |
| `WINEHUA_PROF_SAMPLE=1` | wine 侧 SIGPROF 采样器 arm（配 `WINEHUA_STALL_DUMP` 用） |
| `WINEHUA_WAIT_TRACE=1` | 无限等待打点 `[wait-trace]` |
| `WINEHUA_MIN_THREAD_STACK_MB=0` / `WINEHUA_EMU_STACK_MB=0` | **显式回退到上游栈策略**（A/B 用） |
| `WINEHUA_VIRGL_UBO_FIX=1` | 打开 UBO+1（guest 才有 ES3） |
| `WINEHUA_EARLY_FAULT` / `WINEHUA_FAULT_DUMP_MAPS` / `WINEHUA_FAULT_FREEZE` | fault 诊断 |
| `WINEDEBUG` / `WINEHUA_WINEDEBUG` | Wine debug 通道（`+relay` 会爆炸，慎用；`+sync` 可考虑） |

### 6.4 现成脚本（`F:\WineHua\tmp\`，用 `hdc file send` 推到 `/data/local/tmp/` 后 `sh` 跑）

| 脚本 | 用途 |
| --- | --- |
| `dev_steam_clean.sh` | 零 env 基线 |
| `dev_steam_fg2.sh` | 复刻 12:49 配置（`-cef-force-gpu -no-cef-sandbox`） |
| `dev_steam_soft3.sh` | 产品默认软件路径对照 |
| `dev_steam_stack0.sh` / `dev_steam_ab.sh` | 栈回退 / 栈+GL 探测两段 A/B |
| `dev_guest_sample.sh` | `WINEHUA_PROF_SAMPLE` + `STALL_DUMP` 抓 `[prof-guest]` |
| `dev_winedbg_stall.sh` | 起一轮并尝试 winedbg attach（attach 会挂，留作参考） |
| `dev_stall_watch.sh` | 看门狗轮 |
| `dev_query_probe.sh` | gldriverquery SDL2 验收 |
| `cef_util_report.py` / `raw2png.py` | utility 表 / 落盘帧转 PNG |

### 6.5 HAP 产物（`F:\WineHua\tmp\`）

| 文件 | 说明 |
| --- | --- |
| `winehua-wt.hap`（11:24，347 MB） | **当前设备上装的就是这个**：全部修复 + `[wait-trace]` + guest 采样 + 白名单补丁 |
| `winehua-gs.hap`（11:14） | 上一版（只有 guest 采样） |
| `winehua-ab.hap`（10:30） | 栈/GL 探测 A/B 版 |
| `winehua-stk0.hap`（10:19） | 栈策略可回退版 |
| `winehua-*-20260917.hap` | 09-17 各阶段（含 `winehua-fexec-l1commit` = FEX 缓存提交修复后的基线） |

### 6.6 设备当前状态（写文档时）

- USB 目标 `5KPBB25818203996` **又回来了**（之前掉线过；TCP 目标 `192.168.180.71:36875` 也在）。
- 有 **19 个 wine 子进程**还在（上一轮卡的会话没退），下次开工先 `aa force-stop`（如挂住，先 kill 掉设备侧脚本再 force-stop）。
- Steam 的 CEF profile 已被改名：`…\AppData\Local\Steam\htmlcache` → **`htmlcache.bak0919`**；
  现在每次都是全新 profile（要恢复就 `cd …\Steam & ren htmlcache htmlcache.bak0919` 反过来做）。

---

## 7. 坑（都踩过，别再踩）

1. **只跑 `w1-m3-build-hap.sh` 不会更新运行时**（wine/`wine-data.zip` 不变）。改 wine 必须三步全跑。
2. **App env 白名单**：`GameHook.ets` 里不在白名单的 `winehua.d3d_env_*` 会被静默丢弃（本轮踩过）。
3. **`--enable-logging` 会弹 conhost 控制台**抢焦点、污染"窗口是否上屏"的观察；**Steam 自带的 `--enable-logging=handle` 不弹**。评估 CEF 稳定性时必须 `WINEHUA_CEF_LOGGING=0`。
4. **改日志通道会制造 renderer 崩溃**（§18.6），别用日志结论去推断进程稳定性。
5. **`aa force-stop` 可能挂**（wine 会话僵住时）；先 kill 设备侧 `sh` 脚本再试，或重启 App 前端。
6. **设备侧不能往 prefix 里新建文件**（SELinux）；新文件只能由"容器内进程"创建（见 §6.1）。
7. **`/proc/<pid>/maps`、`/proc/<tid>/stack` 对 shell 不可读**（只能由进程自己读）；`comm`/`wchan` 可读。
8. **屏幕 2 分钟无操作会休眠**，`snapshot_display` 抓到的是全黑图（脚本里挂 keepawake）。
9. **`hdc` 目标会变**（USB 掉线/TCP 端口变），脚本里别写死；`ps -ef` 的 STIME 与设备 `date` 有时不同步，别用它算时间。
10. **`hdc file send` 到设备侧 prefix 基本是 permission denied**（覆盖已存在文件有时可以），
    所以"快速替换运行时 .so"这条路不可靠；目前 A/B 只能靠重出 HAP（一次约 4–6 分钟）。
11. **命令参数里的 URL 编码**：`game_arg_encN` 必须 `[uri]::EscapeDataString`/`encodeURIComponent`；
    内层不要用双引号（见 §6.1）。

---

## 8. 验收标准（做到哪一步算修好）

分阶段，别跳：

1. **Stage 1（当前目标）**：`steamui_html.txt` 里出现 `BrowserReady: handle:65536`，
   `webhelper.txt` 出现 `SetName: SP Shared JS Context` + `--type=renderer`。
2. **Stage 2**：CEF 探针里出现 `key=renderer` 且 renderer 存活；`CreateMainWindow` / `CreatingPopup SP Desktop_uid0 1280x800` 出现。
3. **Stage 3（09-18 14:53 的水平）**：屏幕上 VGUI 外框 + 菜单/页签 + 登录窗/主窗口可见。
4. **Stage 4（最终）**：主窗口内容区（HTML/CEF 页面）出画面、能操作。

## 9. 复现当前故障的最短路径（≈6 分钟）

```powershell
$hdc='C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'; $dev='5KPBB25818203996'
& $hdc -t $dev shell 'aa force-stop app.hackeris.winehua; sleep 8; aa start -b app.hackeris.winehua -a EntryAbility; sleep 45; aa start -a EntryAbility -b app.hackeris.winehua --ps winehua.mode game --ps winehua.game_path C%3A%5CProgram%20Files%20%28x86%29%5CSteam%5CSteam.exe --ps winehua.container_id default --ps winehua.game_argc 2 --ps winehua.game_arg_enc0 %2Dcef-force-gpu --ps winehua.game_arg_enc1 %2Dno-cef-sandbox'
Start-Sleep 150
& $hdc -t $dev shell 'S="/data/app/el2/100/base/app.hackeris.winehua/files/.wine/drive_c/Program Files (x86)/Steam"; tail -6 "$S/logs/steamui_html.txt"; tail -4 "$S/logs/webhelper.txt"'
# 期望复现：CreateResponse 之后没有 BrowserReady；AfterCreated 之后没有 SetName
```

## 10. 相关文档索引

| 文档 | 内容 |
| --- | --- |
| `F:\WineHua\docs\STEAM_WIN64_FEX_ARM64EC_FIX_20260918.md` | 主证据文档 §1–§20（含本轮 §20：host 缺 BrowserReady / 排除假设 / 新工具） |
| `F:\WineHua\docs\STEAM_WINDOW_IDENTITY_20260917.md` | 窗口身份/绑定探针 |
| `F:\WineHua\docs\WINDOWS_ARCH_SEMANTICS.md` | x64/ARM64EC/WoW64 语义 |
| `F:\WineHua\steam-expert-handoff-20260916.md` | 早期专家交接（CEF 开关、present 路径） |
| `F:\WineHua\steam-render-arch-review-20260917.md` | 渲染链路 review |
| `F:\WineHua\device-evidence-*` | 各轮证据目录（截图/日志/PNG） |

---

## 11. 代码提交与远端状态（2026-09-19 收尾，勿丢）

### 11.1 本地提交

| 仓库 | 路径 | 分支 | 提交 | 说明 |
| --- | --- | --- | --- | --- |
| 主仓 | `/home/liufeng/src/WineHua-proton-ohos`（= `F:\WineHua\proton-ohos-worktree`） | `feature/proton-wine-ohos` | `6bab3ff` | App/脚本/文档 + 三个子模块指针；107 个文件 |
| wine（核心） | `thirdparty/wine-valve` | `ohos-port` | `f90646a3c5a` | win64/ARM64EC-FEX 运行时适配 + 诊断探针（46 个文件） |
| mesa | `thirdparty/mesa` | `ohos-guest-caps-diag` | `330124bf18f` | guest caps/vtest 只读诊断（6 个文件） |
| virglrenderer | `thirdparty/virglrenderer` | `ohos-vrend-ubo-fix` | `1b2fa118` | UBO 能力修复（默认关）+ host EGL caps 日志 |
| box64 | `thirdparty/box64` | `ohos-wow64-smc-fs` | `a8e7ed5ca` | 32 位 FEX 路线下的 wow64 SMC/SIGILL + TEB FS |

工作区全部干净（`thirdparty/dxvk` 的 `.git` 指向一个缺失的 gitdir，属于本项目已知损坏项，
本次未触碰它）。

### 11.2 远端分支（都已推送并核对过 SHA）

| 远端 | 分支 | 提交 |
| --- | --- | --- |
| `https://github.com/winehua/WineHua.git` | `feature/proton-wine-ohos` | `6bab3ff`（从 `be4accd` fast-forward） |
| `https://github.com/winehua/mesa-ohos.git` | `ohos-guest-caps-diag` | `330124bf18f` |
| `https://github.com/winehua/virglrenderer.git` | `ohos-vrend-ubo-fix` | `1b2fa118` |
| `https://github.com/winehua/box64.git` | `ohos-wow64-smc-fs` | `a8e7ed5ca` |
| `https://github.com/winehua/wine.git` | `ohos-port-snapshot-20260919` | `8f5f22a8f25` |

> **注意**：上面 `ohos-port-snapshot-20260919` 是"无父提交"的快照分支，只用于留档；
> 给 CI 用的是另一条：`ohos-port-steam-win64` = `c0cc03e377e`（父提交 = 远端 `ohos-port`
> 旧尖端 `61a2f5d0`，树 = 本地 `wine-valve` 工作树）。见 §11.4。

### 11.4 线上可编译性（CI 取 wine 源码的缺口与修复）

**缺口（本轮发现）**：构建用的 wine 源码是 `thirdparty/wine-valve`（`Makefile: WINE_SRC=thirdparty/wine-valve`），
但它是 `thirdparty/wine`（注册 submodule）的一个 **worktree**，**从来没被主仓跟踪过**。
`.github/workflows/*.yml` 只做 `actions/checkout(submodules: recursive)` + `make ... hap`，
所以 CI/新 clone 里根本没有 `thirdparty/wine-valve` → `make wine/assemble/hap` 第一步就退出。

**修复（已提交）**：

1. 推送一条能在 CI 里正常 fetch/worktree 的 wine 分支：

   | 远端 | 分支 | 提交 | 说明 |
   | --- | --- | --- | --- |
   | `winehua/wine.git` | `ohos-port-steam-win64` | `c0cc03e377e` | 父提交 = `ohos-port@61a2f5d0`；树 = 本地 `wine-valve@f90646a3c5a` 的完整源码树 |

   验证方式（已实测）：`git -C thirdparty/wine fetch --depth 1 fork ohos-port-steam-win64`
   → `git -C thirdparty/wine worktree add --detach /tmp/wv-verify FETCH_HEAD`
   → 11211 个文件落地，`dlls/ntdll/unix/ohos_virtual.c`、`dlls/kernelbase/winehua_ipc_trace.c`、
   `dlls/win32u/winehua_gl_proc_trace.c` 等都在，`wait-trace`/sigchain 改动可 grep 到。

2. 新增 `scripts/prepare-wine-valve.sh`：CI / 新 clone 上物化 `thirdparty/wine-valve`
   （检测到已存在就直接返回；本地开发机无需改动）。
   可用 `WINE_VALVE_REF=<branch>` 覆盖（默认 `ohos-port-steam-win64`）。
3. `.github/workflows/build.yml` 与 `build-arm64-native.yml` 在
   "Ensure nested graphics submodules" 之后新增一步：
   `- name: Materialize Proton/Valve wine tree (thirdparty/wine-valve)` → `bash scripts/prepare-wine-valve.sh`。

**仍需注意**：

- CI 触发条件仍是 `master` / `main-ui` / `dev-*`/`rc-*` tag；`feature/proton-wine-ohos` 分支的 push
  不会自动跑 CI（合进 master 或手动 `workflow_dispatch` 才会）。
- 三个图形子模块（mesa / virglrenderer / box64）本次的提交是推在**新分支**上
  （`ohos-guest-caps-diag` / `ohos-vrend-ubo-fix` / `ohos-wow64-smc-fs`），
  与 `.gitmodules` 里写的 `main`/`master` 不一致。`actions/checkout` 是按主仓记录的
  **精确 gitlink SHA** 抓取的（已用 `ls-remote` 核对这三个 SHA 都可达），所以 CI 能编；
  但如果有人用 `git submodule update --remote` 就会拿到配置分支的旧代码——需要时把那三个
  分支 fast-forward 到对应配置分支即可。
- wine-valve 本地仍是"worktree of thirdparty/wine"的形态（与文档一致）；
  `.gitignore` 里已忽略 `/thirdparty/wine-valve/`，避免误提交整棵树。

> **wine-valve 是浅克隆**（`rev-parse --is-shallow-repository` = true，pack 1.21 GiB），
> 直接推历史会被服务端拒（`remote unpack failed: index-pack failed`），从浅仓库推
> 分支也会卡在 shallow 协商（`pack-objects --shallow` 空转）。
> 因此它用一个**无父提交的快照分支** `ohos-port-snapshot-20260919` 备份：
> 内容 = `ohos-port@f90646a3c5a` 的完整源码树。
> 取回方式：`git fetch <remote> ohos-port-snapshot-20260919`，然后 `git diff`/`checkout` 该树；
> 完整开发历史仍在本地 `thirdparty/wine-valve`（分支 `ohos-port`）及其本地 `origin` 里。
> 若以后要恢复完整历史，需要先 `git fetch --unshallow valve`（约 1.2 GiB，慢）。

### 11.3 推送到其它机器的注意事项（踩过）

- 本机 git 全局配置里有 `http.proxy/https.proxy = http://127.0.0.1:8080`，但那个代理**没在跑**；
  推送/拉取要显式绕过：`git -c http.proxy= -c https.proxy= push <remote> <branch>`。
  直连 GitHub 是通的（`curl https://github.com` → 200），凭据在 `~/.git-credentials`。
- `thirdparty/mesa/.git` 原本是 **root 所有**（导致 liufeng 无法提交/写 ref），已在容器里
  `chown -R 1000:1000 .git` 修好；以后再遇到"could not lock config file .git/config"先查归属。
- 主仓 `.git` 是 worktree 指针：`gitdir: /home/liufeng/src/WineHua-arm64ec/.git/worktrees/WineHua-proton-ohos`，
  所以 `git -C` 都必须指到 `/home/liufeng/src/WineHua-proton-ohos`（别用 Windows 侧的软链接路径做 git 操作）。
- 主仓 `.gitignore` 这次新增忽略：`/artifacts/`、`/replay-out/`、`/.tmp-hapcmp/`、`/signs/`（含私钥，
  绝不入库）、以及第三方本地检出 `/thirdparty/wine-valve/`、`/thirdparty/wine-proton/`。
  这些证据目录如果要留档，用 `git add -f` 显式加。

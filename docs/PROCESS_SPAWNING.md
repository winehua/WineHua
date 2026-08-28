# WineHua 进程启动架构

> 更新日期: 2026-08-26(第 5 步重构后: 全部进程统一走 broker)

## 概览

本项目运行在鸿蒙(OHOS)上,受系统约束:**appspawn 拉起的 NCP(Native Child Process)子进程不能嵌套调用 NCP API**。因此进程启动分 3 条路径:

```mermaid
flowchart TD
    A["App 主进程<br/>(compositor / audio / broker 都内嵌于此)"] -->|broker: SPAWN over unix socket| B["wineserver / wineboot<br/>explorer / 任意 exe<br/>wine 内部 CreateProcess 子进程"]
    A -->|NCP CreateNativeChildProcess| C["virgl host<br/>(Pad/2in1)"]
    A -->|dlopen + 线程(手机)| C
```

核心结论:

- **broker 是唯一通道**——所有 wine 体系进程都经过它,包括创世进程 wineserver/wineboot 与 wine 运行中自己 `CreateProcess` 出来的子进程。broker 是主进程内线程,启动不依赖 wineserver,无先后环。
- **wineserver 的特殊性只在子进程内部**: 它是纯 Unix ELF,不能走 wine loader 的 PE 解析,由 `wine_child Main` 截获 `argv[0]=="wineserver"` 转入本体 `RunWineserver`。
- **图形(virgl host)独立拉起**,不走 broker;手机形态下不建进程,主进程内 dlopen + 线程。
- **音频没有独立进程**,AudioBroker 是主进程内服务;**compositor 同样内嵌主进程**,零进程创建。

## 为什么需要 broker

NCP 子进程内无法再次调用 NCP API,而已运行的 wine 进程经常需要再拉子进程(游戏 launcher 起游戏本体、explorer 双击 exe、ntdll 补拉 wineserver 等)。解法:主进程内跑一个 Unix socket server 作为"启动代理",子进程把启动参数发回主进程,由主进程代为调用 NCP。

- 实现: `entry/src/main/cpp/broker.cpp` / `broker.h`
- socket 路径: `/data/storage/el2/base/files/.wine_broker`(`wine_constants.h:21`)
- 协议: `SPAWN\n{entryParams}\n[FDS:names]\n` + SCM_RIGHTS fd 传递,返回 `{childPid, status}`(`broker.cpp:9-12`)
- broker 代调: `OH_Ability_StartNativeChildProcess("libwine_child.so:Main", ...)`(`broker.cpp:234`)——**Main 是唯一 NCP 入口**
- broker 顺带完成: 注入 homeDir 前缀 + WINEPREFIX 会话权威(`broker.cpp:177-183`)、为每个子进程创建 audio bootstrap fd(`broker.cpp:215`)、登记进程注册表(`broker.cpp:245`)
- 启动入口: `StartBrokerServer()`(`broker.cpp:316`,后台线程),由 `wine_launch.cpp:368` 在 LaunchPadMode **最前**拉起(先于 wineserver);`setenv("PROCESSBROKER", ...)` 让 wine 子进程能找到它(`wine_child.cpp:194`)
- **就绪判定**: 必须真实 connect 探测,不能只看 socket 文件存在——bind 创建文件即满足 access,但 listen 未完成时 connect 拿 ECONNREFUSED,曾致紧随的 wineserver spawn 失败。探测连接在 HandleRequest 收 EOF 忽略(`broker.cpp:109`)

## 统一 spawn 通道: broker(wine 体系全部)

App 侧收口: `winehua::Spawner::Spawn(SpawnRequest)`(`spawner.cpp`,kind 推导 token 布局)→ `SpawnViaBroker()`(`wine_exe.cpp:434`,Unix socket 客户端)。wine 侧: `thirdparty/wine/dlls/ntdll/unix/ohos_broker.c`。

| 进程 | 位置 | 说明 |
|---|---|---|
| wineserver | `wine_launch.cpp:380` | kind=Wineserver → `binDir|wineserver|-f|-p`,Main 截获转入 RunWineserver,会话锚点 |
| wineboot --init | `wine_launch.cpp:471` / `542` | kind=Wineboot,prefix 初始化 / 播种 boot 事件 |
| explorer 桌面 shell | `wine_launch.cpp:631` | kind=DesktopShell,`/desktop=shell,...` |
| 任意 Windows exe | `wine_exe.cpp:281` (`SpawnWineProgramImpl`) | kind=WineExe,NAPI `RunWineProgram`,游戏/应用/内建程序 |
| 手动启动 wine exe | `wine_exe.cpp:734` | kind=WineExe,NAPI `RunWineExe`(备用通道) |
| guest x86_64 ELF | `wine_exe.cpp:366` (`SpawnGuestProgram`) | kind=GuestElf,NAPI `RunGuestProgram`,Venus 探针/冒烟测试 |
| host 原生 ELF | `wine_exe.cpp:421` (`SpawnHostProgram`) | kind=HostElf,NAPI `RunHostProgram`,heaven replay 等工具 |
| wine 内部子进程 | `thirdparty/wine/dlls/ntdll/unix/process.c:438` | `CreateProcess` → `ohos_broker_spawn_child` |
| wine 运行中补拉 wineserver | `thirdparty/wine/dlls/ntdll/unix/loader.c:572` | `ohos_broker_spawn_wineserver` → 同样由 Main 截获兜底 |

子进程内的执行载体(`wine_child.cpp`,**进程内 dlopen/exec,不产生新进程**):

- ARM64: `dlopen("box64.so")` → `box64_hmos_main` 模拟 x86_64 wine/wineserver(`wine_child.cpp:602/635/756/799`)
- x86_64: `dlopen("ntdll.so")` → `__wine_main`(`:652/668`);wineserver 走 `dlopen("libwineserver.so")`(RunWineserver `:782`)
- guest ELF: `execve`(`:645`)

## 路径 B: 图形进程 virgl host(不走 broker)

- Pad/2in1: `graphics_broker.cpp:1337` → `OH_Ability_CreateNativeChildProcess("libvirgl_child.so")`,独立 NCP 子进程跑 virgl_test_server(IPC proxy 配置见 `:1367`,入口 `virgl_child.cpp:534`)
- 手机: `StartVirglInProcessHostLocked`(`graphics_broker.cpp:179`)**不建进程**,主进程内 dlopen `libvirgl_child.so` + `std::thread` 跑 virgl host(`:238`)
- 触发点: `wine_launch.cpp:457`(`PrepareDesktopSessionGraphicsEnv`)、`wine_launch.cpp:846-849`(LaunchThreadFunc)

无 xserver;窗口显示靠主进程内嵌 wayland compositor(`wayland_server.cpp` + `compositor/`),非独立进程。Venus 在 guest 侧以库形式加载(venus_icd),host 侧复用同一 virgl/vtest 通道。

## 路径 C: 手机 fork 兜底

系统 NCP 在手机上不可用时的替代实现:

- `phone_adapter/phone_process.cpp:218` — `fork()` 实现 `Phone_StartNativeChildProcess`(等价 broker 代调的 NCP)
- `phone_adapter/phone_process.cpp:250` — `fork()` 实现 `Phone_CreateNativeChildProcess`(等价 virgl 的 NCP)
- 路由层 `ncp_dispatch.cpp:40/53` 符号覆盖 `OH_Ability_Start/CreateNativeChildProcess`,按 `g_isPhone` 分流到系统 `libchild_process.so` 或上面的 fork 实现

## 音频: 无独立进程

`AudioBroker`(`audio_broker.cpp:72` EnsureStarted)在主进程内起 `AudioIpcServer` 线程 + OH_AudioRenderer/Capturer;wine 子进程通过 broker 下发的 `wine_audio_bootstrap` fd(socketpair)接入(`wine_env.cpp:22 CreateAudioBootstrapFd`)。详见 `docs/AUDIO_ARCHITECTURE.md`。

## ArkTS 侧入口

ArkTS 侧**无任何** `spawn`/`child_process`/`@ohos.process` 调用,全部汇聚到 `libentry.so`(napi 名 `testNapi`):

| 功能 | ArkTS 入口 | napi 方法 | native 实现 |
|---|---|---|---|
| 启动/重启引擎 | `WineEnvService.ets:857` (startSession) | `startServer` + `launchClient` | `napi_init.cpp:318` → `LaunchThreadFunc`(`:412`) |
| 启动任意 exe(应用库/文件页/内建) | `AppLibraryService.ets:549` launch() | `runWineProgram` | `wine_exe.cpp:492` |
| 在 Wine 中打开目录 | `AppLibraryService.ets:586` | `runWineProgram`(explorer.exe &lt;dir&gt;) | 同上 |
| 自动化烟测 guest ELF | `SmokeRunner.ets:1178` | `runGuestProgram` | `wine_exe.cpp:529` |
| host replay 工具 | `SmokeRunner.ets:925` | `runHostReplay` | `wine_exe.cpp:579` |
| 恢复出厂 | `WineEnvService.ets:986` doReset | stopAll → resetWinePrefix → launchClient | — |

上层编排(三原语组合, 均汇聚到 startSession → launchClient):

- 原语: `startSession(foreground)`(:857, 幂等确保就绪, 内部按磁盘状态走首启/二启分支)、
  `stopSession`(:958, 无条件 stopAll → 等 state:stopped, 无会话也杀主 wineserver — 
  重启语义=全新引擎, 不走 wine 单实例热重连)、`wipeEnvironment`(:978, 删运行时+清 prefix)
- `WineEnvService.ets:1030` restartEngine = stop + start —— "重启引擎"(Index 失败重试 / EngineGuide 首启引导 / SettingsView 环境页)
- `WineEnvService.ets:987` doReset = stop + wipe + start —— 恢复出厂
- `WineEnvService.ets:1049` ensureEngineReady —— 启动 exe 前的懒拉起
- `WineEnvService.ets:1077` ensureDesktop —— 桌面模式下补 spawn explorer
- `WineEnvService.ets:611` autoStartIfIdle —— 二次进入后台自动拉起 (startSession(后台))

已注册但 ArkTS 未调用的遗留通道: `runWineExe`(`napi_init.cpp:1247`)、`runHostProgram`(`:1250`)。

注意: `startAbility`(拉起 WineWindowAbility / VirtualDesktopAbility / 系统 filemanager)是鸿蒙 Ability 启动,不是 wine 进程。

## 分类汇总

- **wine 体系进程**: wineserver、wineboot、explorer、用户 exe、wine 内部 CreateProcess 子进程——全部经 broker。ARM64 上全部由子进程内 box64 承载。
- **基础设施进程**: virgl_test_server(路径 B,图形)。音频/compositor 为主进程内服务,非进程。
- **工具/一次性进程**: guest/host ELF 探针、replay(broker);wineboot 兼具初始化工具性质。

## 环境变量机制

### 核心前提

OHOS 的 NCP 子进程**不继承主进程 environ**(`wine_launch.cpp:506` 注释)。因此主进程里的 `setenv` 对 wine 子进程不可见,wine 子进程的环境由两部分拼出:

1. **子进程入口的硬编码基线**: `wine_child.cpp:269 setup_wine_env()`(子进程起来后先自行 setenv)
2. **entryParams 尾部 `|__env=K=V` 段**: 主进程侧构造、随 NCP/broker SPAWN 传入,子进程 `apply_entry_param_env_overrides`(`wine_child.cpp:324`)逐条 `setenv(K,V,1)` 覆盖基线,**后写胜出**

### 6 条设置途径

**① 主进程自身 setenv(只影响主进程)**

- `PROCESSBROKER=WINE_BROKER_SOCKET`(`wine_launch.cpp:548`)——broker socket 路径,供 SpawnViaBroker 与注入 ntdll 的 ohos_broker.c 定位 broker
- `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY`(`wayland_server.cpp:56,72`)——compositor 自用
- `SetHostShadowProfile`(`napi_init.cpp:243-296`)——一组 `VKR_WINEHUA_*`/`WINEHUA_VENUS_PRESENT_MODE` 等 host 渲染开关(profile 字符串解码)
- virgl host 配置: graphics_broker 启动时 `getenv("WINEHUA_VIRGL_HOST_*")` 组 `VirglHostConfig`,经 **IPC parcel 显式传给 virgl 子进程**,不靠继承

**② 集中式工厂 BuildWineEnv(主通道)**

`wine_env.cpp:35 BuildWineEnv()` 构造 `vector<string>`(不进主进程 environ),分 6 层:

| 层 | 内容 |
|---|---|
| L0 硬基线 | XDG_RUNTIME_DIR/WAYLAND_DISPLAY/HOME/WINEPREFIX/WINEDLLDIR{0..2}/WINEDLLPATH/WINEDEBUG=-all/LANG+LC_ALL/PATH/TMPDIR/GST_PLUGIN_PATH/MIDI_SOUNDFONT_PATH |
| L1 Box64 调优(aarch64) | `AppendBox64PerfStrings`(wine_env.h:50): BOX64_LOG/NOBANNER/SAFEFLAGS=1/BIGBLOCK=3/CALLRET=2/FORWARD=1024/WEAKBARRIER=2/AVX=0/AES=0 等 |
| L2 库路径 | aarch64: LD_LIBRARY_PATH + BOX64_LD_LIBRARY_PATH;x86_64: 仅 LD_LIBRARY_PATH |
| L3 音频(条件) | WINE_OHOS_AUDIO_ENABLE/BOOTSTRAP_FD/PROTOCOL_VERSION |
| L4 桌面标记 | WINEHUA_DESKTOP_MODE、WINEHUA_SIMULATE_RESOLUTION、WINEWAYLAND_ENTER_SILENT=1 |
| L5 图形状态 | `GraphicsBroker::AppendWineEnv`(graphics_broker.cpp:897): WINEHUA_GRAPHICS_BACKEND/ACTIVE、VIRGL_*、VK_ICD_FILENAMES、EGL_*、BOX64_EMULATED_LIBS 等 |

追加器(均经 `UpsertEnvLine` 去重,后者胜出):

- `AppendD3dBackendEnv`(wine_env.cpp:152)——dxvk/vkd3d 后端时整组注入: VK_DRIVER_FILES、VN_*、WINEDLLOVERRIDES=d3d11=n;dxgi=n、WINEHUA_DXVK_*/VKD3D_* 等
- `AppendStableDesktopDxvkEnv`(env_profiles.cpp)——DXVK/VKD3D 稳定化收口(桌面会话链与 RunWineProgram 直启链同源): DXVK_LOG_LEVEL=warn、WEAKBARRIER=0 clamp、WINEHUA_PERF_PROFILE
- 兼容档位(wine_launch.cpp:318-364,aarch64)——ArkTS `compatEnvStr` 分号串,白名单只放行 `BOX64_DYNAREC_*`;`AppendCompatEnvLines` 统一注入 SpawnRequest.env(会话 env 与 wineserver/wineboot 同一通道);automation 模式跳过

**③ wine_child 子进程内重建**

- `Main()`(wine_child.cpp:410): 解析 entryParams → `setup_wine_env` 基线 → fdList 恢复(WINESERVERSOCKET/音频 fd)→ apply `__env` 覆盖 → aarch64 `dlopen box64.so` 显式传 environ
- wineserver 特判(`:470`): `argv[0]=="wineserver"` 转入 `RunWineserver`(`:692`)——精简基线(WINEPREFIX/WINEDEBUG=-all/BOX64_LD_LIBRARY_PATH),`__env` 覆盖最后应用(后写胜出,兼容档位压过 SetBox64PerfEnv 基线)
- host ELF 分支(wine_child.cpp:395): 逆向操作——先 unsetenv 所有 `BOX64_*`/`VN_*`/`VK_*` 图形变量再最小重建(host 原生工具不吃 guest 图形配置)

**④ broker SPAWN 协议内嵌**

env 无独立通道,全部序列化为 entryParams 尾部 `|__env=K=V`(现由 `winehua::EnvSpec::serializeEntryParams`,env_spec.cpp 统一实现;跳过含 `|`/`\n` 的行与 fd 变量)。broker 收到后统一: 前面补 `gBrokerHomeDir|`、尾部强制 `__env=WINEPREFIX=<会话prefix>`(会话权威,压掉请求残留)、追加 audio bootstrap fd。

**⑤ wine→wine 子进程完整继承**

wine 内 `CreateProcess` → `ohos_broker_spawn_child`(`ohos_broker.c:192`): 把**父进程当前 environ 全量**序列化转发,`env_forwardable`(:158)只剔除 4 个 per-process fd 变量(WINESERVERSOCKET/WINE_OHOS_AUDIO_*)。净效果等于父环境。wineserver 自启(`:176`)只发 `binDir|wineserver|-f|-p` 无 env,靠 RunWineserver 精简基线 + broker 补 WINEPREFIX 兜底。

**⑥ ArkTS 侧参数注入**

- 引擎级(launchClient 参数,设置页 → preferences/AppStorage → `WineEnvService.ets:857` startSession 组装): `lang` → LANG/LC_ALL;`d3d`/`dxvk` → AppendD3dBackendEnv 分支;`compatEnvStr` → 兼容档位;`prefixMode`/`automation` → 行为开关
- per-app(runWineProgram 的 `environment: Record<string,string>` 对象,`AppLibraryService.ets:549` launch()): LANG、11 个 BOX64_DYNAREC_* 档位(键清单/取值唯一来源 `Box64Dynarec.ets`,native 仅 `FilterCompatLines` 前缀门,不持键表)、`WINEHUA_WINDOWS_VERSION`(winver)、d3d/graphics 覆盖;native 端 `UpsertEnvLine` 压过基线(wine_exe.cpp:244-249)
- 自动化: smoke 设施已拆除(SmokeRunner/Want 协议/诊断 env 矩阵均移除),重建设计(瘦编排 + 随载荷版本化 manifest)见 SMOKE_INFRASTRUCTURE.md §4

### fd 变量的统一约定

`WINESERVERSOCKET`、`WINE_OHOS_AUDIO_*` 这类 fd 变量在**所有序列化点被一致排除**出文本通道(wine_env.cpp:376-448、ohos_broker.c:158 相同过滤逻辑)——fd 号跨进程失效,必须由 NCP fdList / SCM_RIGHTS 传 fd,子进程按本进程 fd 号重写变量(wine_child.cpp:510-523)。

### virgl host: 独立白名单体系

virgl 子进程不共享 BuildWineEnv: 主进程 `WINEHUA_VIRGL_HOST_*` → IPC parcel(`SendVirglConfigureLocked`,graphics_broker.cpp:301)→ 子进程先 `ClearGuestGraphicsEnv()`(virgl_child.cpp:315)清掉 guest 图形变量 → `BuildVirglHostLaunchConfig`(virgl_host_config.cpp:85)生成约 40 条 `__env` → `ApplyHostEnv` 按**白名单**(virgl_child.cpp:265 `IsAllowedHostEnv`,约 50 个 key)逐条 setenv。手机 in-process 模式经 `WINEHUA_PHONE_CFG_FD` 环境变量传 socket fd(phone_process.cpp:178)。

### 各路径 env 差异速查

| 路径 | env 来源 |
|---|---|
| wineserver | 仅 compat 档位经 SpawnRequest.env;其余靠 RunWineserver 精简基线 + broker 尾部 WINEPREFIX 会话权威 |
| wineboot | LANG/LC_ALL(+compat 档位;automation 时 WINEDLLOVERRIDES=mscoree,mshtml=);"节省 entryParams 长度"刻意不传全量 |
| explorer / RunWineProgram / RunWineExe / GuestELF | BuildWineEnv 全量 + AppendD3dBackendEnv(+AppendStableDesktopDxvkEnv: 桌面链与 RunWineProgram 链同源)→ broker |
| wine→wine 子进程 | 父进程 environ 全量转发(过滤 fd 变量)|
| virgl host | 独立白名单体系(见上),不共享 BuildWineEnv |
| host ELF | 清除 guest/box64 图形变量后最小重建 |

### Wine 的 Windows 侧环境块

标准上游逻辑,无项目定制: ntdll `env.c:1202 add_registry_environment` 建进程 env 时合并 `HKLM\...\Session Manager\Environment`、`HKCU\Environment`、`HKCU\Volatile Environment`,内容来自 wine.inf 默认值。语言/DPI 等均不经注册表 Environment 键下发。

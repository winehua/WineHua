# Smoke / 自动化设施全景与侵入点清理（entry 模块）

本文记录 entry 模块内 smoke（自动化烟测）相关设施各自是什么功能、入口在哪里，
以及这些设施对正常功能逻辑的侵入点与清理方案。

## 1. 设施清单

### 1.1 自动化启动通道

- **功能**：外部脚本（`automation/run_regression.py`）通过 `aa start` 携带
  `winehua.mode / winehua.suite / winehua.prefix / winehua.run_id ...` 等 Want
  参数启动 app，使其进入无人值守的测试会话。
- **入口**：`EntryAbility.ets` 的 `publishLaunchRequest(want)` —— 解析全部
  Want 参数，快照发布到 AppStorage（`winehua.smoke.request` 等），并返回是否
  自动化（`mode === 'smoke' || mode === 'experiment'`）。
- **数据流**：`WineEnvService.startSession` 读取 AppStorage 快照
  （`parseSmokeRequest`），派生出 `automationMode / gameTestMode / prefixMode`，
  prefix 就绪后由 `waitForPrefix` 触发 `startRequestedSmoke / startRequestedGame`。

### 1.2 SmokeRunner（套件运行器）

- **功能**：`entry/src/main/ets/service/SmokeRunner.ets`。按 suite 生成测试
  规格表（audio / opengl / d3d8 / d3d9 / wine-vulkan / venus / dxvk / dxvk-long /
  gpu-diagnostics / capabilities 等），逐个经 `runWineProgram / runGuestProgram /
  runHostReplay` 拉起 `C:\smoke\<arch>\winehua_*_smoke.exe`，轮询结果 JSON，
  汇总写出 suite-summary。
- **遥测键**：每个测试进程带 `WINEHUA_SMOKE_RUN_ID / WINEHUA_SMOKE_TEST_ID` 环境
  变量，结果写入 `C:\smoke\results\<run-id>\<test-id>.json`。

### 1.3 受管测试载荷 C:\smoke（syncManagedSmoke）

- **功能**：`SmokeRunner.ets` 的 `syncManagedSmoke(prefixMode)` 把 HAP 内置的
  `files/wine/smoke` manifest 树（x64+x86 两套 winehua_audio/graphics/vulkan/
  d3d8/d3d11/d3d12_smoke.exe、d3d_switch_cube、win32_driver 等）整树刷新到
  `<prefix>/drive_c/smoke`，并清理陈旧的受管 DXVK overlay。目的是覆盖安装后
  不继续跑旧的测试二进制。

### 1.4 隔离 prefix（.wine-smoke）

- **功能**：`--prefix clean` 的自动化会话使用独立 prefix
  `/data/storage/el2/base/files/.wine-smoke`（`WINE_SMOKE_PREFIX`，
  `wine_constants.h`），与用户 prefix `.wine` 生命周期完全隔离；
  native 侧（`napi_init.cpp`）对 clean prefix 强制 automationMode 兜底。

### 1.5 退出遥测

- **功能**：Box64 内嵌 Wine 的 wineserver Unix 包装进程会活到最终 SIGKILL，
  Unix 侧拿不到 Windows 逻辑退出码。smoke prefix 的 wineserver 带
  `WINEHUA_PROCESS_EXIT_TELEMETRY=1` 时会把 Windows 退出码记录到
  `<prefix>/.winehua-process-exit-status`；broker 的 SIGCHLD 处理
  （`wine_process.cpp` 的 `ReadWineServerExitTelemetry`）在该进程被 SIGKILL 时
  读回逻辑退出码上报。

### 1.6 game 模式与点击驱动

- **功能**：`winehua.mode=game` 直接拉起指定游戏 exe（Want 可携带 argv、
  D3D 环境、perf profile），并可按参数在延迟后运行
  `C:\smoke\x64\winehua_win32_driver.exe` 做标题匹配自动点击（无人值守
  过启动对话框）。因此 **game 模式同样依赖 C:\smoke 载荷**。

### 1.7 experiment 模式（当前半死状态）

- **功能设计**：下载经 sha256 校验的测试载荷（loopback HTTP），staging 到
  `C:\smoke\experiments\<id>\` 后运行其中 exe。native 侧
  `experiment_payload.cpp` 的 `StageExperimentPayload` 完整实现并经 NAPI 导出。
- **现状**：`EntryAbility` 解析 experiment 参数并把 names/hashes 发布到
  AppStorage，但 **entry 内没有任何 ArkTS 代码读取这些键或调用
  `stageExperimentPayload`**；且 `WineEnvService` 只认 `mode === 'smoke'/'game'`，
  experiment 既不 automation 也不 game —— 该模式目前在 ArkTS 侧断链，记录在案。

### 1.8 host-vulkan 套件与 HostVulkanSmoke 页面

- **功能**：`capabilities` 等套件需要 Host Vulkan 侧能力探针。页面
  `pages/HostVulkanSmoke.ets`（仅 `mode=smoke & suite=host-vulkan` 时由
  EntryAbility 路由加载）经 `runHostVulkanProbe(surfaceId)` 在 XComponent 上
  跑 host 探针；另有 `runHostProgram` 跑 `bin/host_vulkan` 受管目录下的
  host ELF（能力审计等）。

### 1.9 guest / host ELF 程序通道

- **功能**：`runGuestProgram`（guest ARM64 ELF，如 `winehua_guest_vulkan_smoke`）
  与 `runHostProgram`（host ELF，限定 `WINE_RUNTIME_BIN/host_vulkan` 受管目录，
  `ResolveManagedHostExecutable` 做 realpath  containment 校验）。两者经
  broker `SpawnKind::GuestElf / HostElf` 统一 spawn。

### 1.10 生产 UI 里的开发者测试入口（清理对象）

- 设置页"开发测试（Core）"按钮 → `WineEnvService.startDeveloperSmoke()`：
  以 automation 语义在用户 prefix 跑 core 套件（自动化通道
  `aa start mode=smoke suite=core` 完全覆盖此用途）。
- VKD3D 后端下的"运行 DX12 准确 Smoke（1000 帧）"面板（`Index.ets`）：
  在当前用户会话跑 `winehua_d3d12_smoke.exe` 并轮询 checkpoint。
  配套机制 `WineWindowManager.setManagedToplevelTitleFilter`（只让指定标题的
  窗口进入 managed 列表）仅此一处使用。
  **注意**：该 exe 目前唯一的运行入口就是这个按钮，移除后如需保留验证能力，
  应在 SmokeRunner 增加 d3d12 suite 走自动化通道。

## 2. 侵入点清单（smoke 逻辑渗入正常功能路径）

| # | 位置 | 侵入表现 |
|---|------|----------|
| 1 | `WineEnvService.ets` auto-init（约 929-935 行） | **每次正常启动/重启引擎都全量同步 C:\smoke 测试载荷到用户 prefix，同步失败直接 failInit 阻塞启动** —— 正常启动依赖测试设施 |
| 2 | `Index.ets` `d3dLaunchEnvironment`（235-264 行） | 正常启动 exe（文件管理器"启动"按钮 `doRun`）的 `DXVK_LOG_PATH` 写死 `C:\smoke\results\desktop-dxvk`，正常会话日志写入 smoke 目录 |
| 3 | `spawner.cpp`（87-91 行） | 通用 `Spawner::Spawn` 内嵌 smoke 特判：`Wineserver && prefix == WINE_SMOKE_PREFIX` 时附加退出遥测 env，smoke 知识漏进通用 spawn 层 |
| 4 | `wine_process.cpp`（423-425 行） | SIGCHLD 退出路径对每个被 SIGKILL 的子进程都先探 `.wine-smoke` 再探 `.wine` 的遥测文件；而遥测 env 只在 smoke prefix 设置，探 `.wine` 恒为无效开销 |
| 5 | `wine_launch.cpp`（451-466 行） | wineboot 共享启动路径里的 automation 特判（`bootstrapNeedsDesktopSurfaces`、`WINEDLLOVERRIDES=mscoree,mshtml=`、`WINEHUA_BOOTSTRAP_PHASE`）。正常会话不命中，属 automation 会话语义，保留 |
| 6 | `env_profiles.cpp`（36-43 行） | `AppendCompatEnvLines` 带 `automationMode` 形参：automation 跳过用户兼容档位（"回归必须跑出厂基线"）。有意语义，保留 |
| 7 | `WineWindowManager.ets`（150-156 行） | 桌面模式策略认识 automation：automation 会话在 PC 上强制融合模式。自动化窗口策略，保留 |
| 8 | `Index.ets`（868-872 行等） | 生产设置页内嵌"开发测试（Core）"按钮与 DX12 smoke 面板 |
| 9 | `main_pages.json` / `EntryAbility.ets:217` | HostVulkanSmoke 页面随产品发布（仅自动化路由可达，保留） |
| 10 | `host_vulkan_probe.cpp:27` | 生产 cpp `#include "../../../../smoke/vkd3d_capability_audit.h"`，构建期耦合 smoke/ 源树（同仓库相对路径，记录在案，暂不改） |

## 3. 拆除决定

曾按"正常会话零 smoke 依赖、自动化行为不变"做过逐点清理方案，评估后放弃：
侵入点遍布 ArkTS/native 两侧（上表 10 处），而 SmokeRunner 本体是一个约 1650
行、内含 200 余行硬编码 env 矩阵的 TS 文件，逐点修完后它依旧混乱。因此决定
**整体拆除 entry 内的 smoke 逻辑，后续按 §4 的设计重建**。

拆除范围：

- ArkTS：`SmokeRunner.ets`、`HostVulkanSmoke.ets` 整文件删除；
  `EntryAbility.publishLaunchRequest` 瘦身为只发布正常会话所需的最小状态
  （D3D/DXVK 后端 preference → AppStorage，`contentPage` 固定 `pages/Index`），
  删掉 mode/suite/prefix/experiment/game/click/perf-profile 的 Want 解析与
  `winehua.smoke.request / winehua.game.* / winehua.experiment.*` 发布；
  `WineEnvService` 删除 automation/game/prefixMode 分支（含 zHome 特判、
  clean prefix 重置、startRequestedSmoke/Game、点击驱动、startDeveloperSmoke、
  game 版 d3dLaunchEnvironment）；`Index.ets` 删除 smoke 面板与开发测试按钮，
  正常 DXVK 日志路径 `C:\smoke\results\desktop-dxvk` 改为 `C:\windows\temp`；
  `WineWindowManager` 删除 `managedToplevelTitleFilter` 配套逻辑；
  `main_pages.json` 移除 HostVulkanSmoke。
- native：删除 `experiment_payload.cpp/h`、`host_vulkan_probe.cpp/h` 及
  CMake 条目；`wine_exe.cpp` 删除 `SpawnGuestProgram`、
  `ResolveManagedHostExecutable + SpawnHostProgram`、`PrefixForMode`、
  `RunHostReplay/IsHostReplayRunning` 与 `WINEHUA_AUTOMATION` 注入；
  `spawner.h/cpp` 删除 `GuestElf/HostElf` SpawnKind 与 wineserver 遥测特判；
  `wine_child.cpp` 删除 guest/host ELF 分流（含 host replay dlopen 通道）与
  `WINEHUA_AUTOMATION` stdout 重定向；`broker.cpp` 删除 ELF 标记段解析；
  `napi_init.cpp` 删除 clean-prefix 兜底分支与上述 NAPI 注册，
  `launchClient/checkWinePrefix/resetWinePrefix` 去掉 automation/prefixMode
  参数；`wine_process.cpp` 删除 `ReadWineServerExitTelemetry` 及 SIGCHLD
  调用点；`wine_launch.cpp` 删除遥测 env 附加、wineboot 的
  `bootstrapNeedsDesktopSurfaces`/`WINEDLLOVERRIDES=mscoree,mshtml=`/
  `WINEHUA_BOOTSTRAP_PHASE` 分支与 automation 跳过 Explorer 分支（均为
  `automationMode && prefixDir == WINE_SMOKE_PREFIX` 判定，正常会话从不命中，
  随 automation 一并删除）；`wine_constants.h` 删除 `WINE_SMOKE_PREFIX` 与
  `WINE_AUTOMATION_HOME`；`env_profiles` 删除 `AppendCompatEnvLines` 的
  `automationMode` 形参与 `SessionEnvPolicy.automationMode` 字段；
  `LaunchParams.automationMode` 同删。
- 自动化兼容性不作约束：`automation/run_regression.py` 等旧脚本随之失效，
  重建时按新协议重写。

保留但改写为产品语义：

- perf profile：调查期白名单（EntryAbility 的 Want 解析与 Index/WineEnvService
  的两处 profile→env 展开链）删除；正常路径改为固定产品默认——vkd3d 混合路由
  `shadow-precise`，其余 `shadow-precise-dirty-ring-inline-upload-coverage-sort`
  （与 native `AppendStableDesktopDxvkEnv` 无显式 profile 时的缺省一致）。
- `runWineExe` NAPI（ArkTS 手动路径）：当前无 ArkTS 调用方，非 smoke 设施，
  本次保留未动，后续可单独决定去留。

## 4. 重建设计（后续搭建时遵循）

分层原则——三层的知识边界：

1. **主流程只持有两样东西（均已存在）**：
   - 产品环境管线（native `BuildSessionEnv`）：baseline、D3D 后端 overlay、
     兼容档位、perf profile。这些是产品语义，主流程本该懂；smoke 会话跑同一
     条管线，免费继承，不需自补。
   - 一条无语义的 per-process env 注入通道（`runWineProgram` 的 `environment`
     参数，`wine_exe.cpp` 逐行 `UpsertEnvLine`，最后写入者胜出）。通道不认识
     任何键名，不是 smoke 专用——应用库 per-app 配置也走它。
   - 反例警示：把"诊断 env 也收口到 native"等于把侵入从 ArkTS 挪到 C++，
     `BuildSessionEnv` 不应认识"suite=dxvk 时要开哪些 trace 键"这种纯测试语义。
2. **smoke 层 = 瘦编排**：读请求 → sync 载荷 → 读 manifest → 合并 env（经通用
   通道下发）→ 跑 exe → 收 JSON 汇总。它搬运 env 但不懂得 env；懂"跑哪些
   测试、按什么顺序、结果怎么判"，不硬编码 env。
3. **诊断 env 以数据存在**：随测试载荷版本化的 manifest——`C:\smoke` 里每个
   suite 一个 JSON，声明测试列表 + 每个测试的 env 键值，和测试 exe 一起构建、
   一起同步、一起失效。`DXVK_WINEHUA_TRACE_*`、`SHADER_DUMP`、各测试的
   evidence 开关、run-id 遥测键都进 manifest，永远不进主流程。

双头维护随之消失：native 管线已有的产品项（如 `WEAKBARRIER=0`）manifest 不再
重复声明，只有真正测试专属的诊断键进 manifest。

重建时的资产取舍：

- 保留：`smoke/` 下 C 测试程序源码、`automation/run_regression.py`（按新协议
  改造）、结果 JSON 协议（`C:\smoke\results\<run-id>\<test-id>.json`）。
- 砍掉：experiment 模式（拆除前已半死：native 实现完整但 ArkTS 侧断链）、
  legacy Want 编码、调查期 perf profile 白名单、开发测试按钮这类生产 UI 内嵌
  入口（验证能力应由自动化通道 suite 提供，如 d3d12 suite）。

## 5. 验证

- `make NATIVE_ARCH=arm64-v8a` 与 `make NATIVE_ARCH=x86_64` 均编译通过。
- 行为回归：正常首启/二启/重启引擎（无 C:\smoke 同步日志与失败路径）；
  文件管理器启动 exe（DXVK 日志落到 `C:\windows\temp`）。

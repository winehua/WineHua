# Wine 引擎状态机重构方案

> 来源：2026-08 对「首启成功 / 暖启动完毕 / 桌面完全退出」三个判定问题的分析。
> 本文档是施工蓝图：goal 驱动分阶段执行，每阶段双架构构建 + 设备回归后进入下一阶段。

## 1. 现状问题（已核实，含 file:line）

### 1.1 状态与事件混装（根因）
- `WineEnvService.onNativeState`（entry/src/main/ets/service/WineEnvService.ets:233）无条件
  `this.state = stateMsg`，`process-updated`/`exited` 等事件覆盖状态 →
  所有 `state == 'wine-running'` 判据失真：程序运行中收到 process-updated 时
  重启/重置按钮意外解锁（EnvPanel.ets:168/201）；标题栏状态点颜色闪变。
- 历史同类 bug：state 曾存原始串（"pid:wine-running"）导致比较永不成立。

### 1.2 失败路径断线
- native 发 `wineserver-failed`/`wineboot-failed`（wine_launch.cpp:411/469/492/502），
  ArkTS 无处理分支 → 首启失败永久 spinner，无重试入口。
- init marker 创建失败（wine_launch.cpp:438-441）静默 return，连消息都不发。

### 1.3 「就绪」判据不准
- 暖启动时 `prefixReady` 由 init() 里的文件检查（IsWinePrefixInitialized：reg 非空
  + 3 目录存在，wine_launch.cpp:48-55）在 native 启动前即置位，与引擎真实健康解耦。
- wineserver socket 等 5s、desktop root 等 15s、seed wineboot NCP 失败——三处全部
  只警告/记日志照样发 `wine-ready`（wine_launch.cpp:415/532/600）→「已就绪」不保证桌面出现。
- PC 窗口模式 explorer spawn 零判定（wine_launch.cpp:608-623）。

### 1.4 不存在「完全退出」判据
- 主 wineserver 未登记进程注册表、无监控；`stopAll` 的 KillAllProcesses 只杀注册表成员，
  **不杀 wineserver**（WineEnvService.ets:504 注释与实现不符），不 waitpid，无完成确认，
  ArkTS 立刻乐观直写「已停止全部会话」（WineEnvService.ets:510）。
- ProcMon 用 access(/proc/pid) 判活，不识别 zombie（wine_process.cpp:170）；
  zombie 感知实现 IsProcessAliveNotZombie（wine_launch.cpp:63）只用于 wineboot。
- winehua_keep.exe 桌面销毁后 Sleep(INFINITE) 永驻（thirdparty/wine/programs/winehua_keep/main.c:61），
  进程列表永不为空 → 「已就绪」永不出现。
- `exited` 分支读 300ms 节流刷新前的旧列表（WineEnvService.ets:254 vs ProcessService.ets:81），
  最后一个进程退出后状态卡「运行中」。
- root 销毁 ≠ 会话结束：desktopActive=false 时 wineserver/services/keep 全在跑。

### 1.5 启动成功判据不准
- `wine-running` 语义 = broker 受理 spawn（wine_exe.cpp:279-291），不是窗口出现；
  闪退程序误判成功；已有程序运行时任意 process-updated 都会误触发成功
  （AppLibraryView.ets:126-128）。

### 1.6 其他交互缺口
- doInit 无防重入；首启解压数分钟内按钮可重入（并发解压删 runtime 目录风险）。
- 重启 = 直接 doInit：桌面存活时再 spawn wineserver/explorer → 双桌面卡顿（已知 bug）。
- 重置 prefix 在会话存活时可用 → 运行中删 prefix。
- statusText 单槽混用：引擎状态与瞬时回执（已停止/已导出/已清空）互相覆盖。
- 运行中「结束」无确认；init overlay 非模态。

## 2. 目标设计

### 2.1 引擎状态机（native 权威，ArkTS 镜像）

```
               launchClient               LaunchPadMode 成功 + wineserver 存活复查
  idle ─────────────────────► starting ────────────────────────────► ready
   ▲                              │                                   │
   │                              │ 任一步骤致命失败                   │ wineserver 死亡
   │                              ▼                                   ▼
   │                           failed ◄───────────────────────────────┘
   │ state:stopped               │ (消息带阶段: wineserver/wineboot/graphics/desktop)
   │ (注册表进程全死 zombie 感知
   │  + wineserver 死 + wayland 停)
  stopping ◄──── stopAll ◄───────┘
```

- 「完全退出」唯一判据：注册表进程全死（zombie 感知等待）且 wineserver 死且
  Wayland server 停 → 发一次 `state:stopped`。root 窗口销毁只是「桌面关闭」事件。
- desktop root 15s 超时不再硬放行装死：发 `wine-ready-degraded`，UI 显示
  「桌面准备中…」，root 出现后补发 `desktop-ready`。

### 2.2 消息协议重构（test 设施不作兼容约束）

一条 tsfn 字符串通道，消息分两类（ArkTS 不再做冒号启发式切割）：

- `state:<name>` — 引擎持久状态迁移，仅状态机可写：
  `state:starting` / `state:ready` / `state:ready-degraded` / `state:stopping` /
  `state:stopped` / `state:failed:<stage>`
  （`state:failed:wineserver` 有两个触发点：启动流程存活复查失败，以及 ProcMon
  检测到运行中主 wineserver 非预期死亡；stopAll 等主动停止期间不上报）
- `evt:<name>` — 瞬时事件：
  `evt:proc-added:<pid>:<name>` / `evt:proc-exited:<pid>` / `evt:launch-accepted:<pid>` /
  `evt:launch-failed` / `evt:desktop-ready` / `evt:desktop-closed`

SmokeRunner / automation 同步适配新协议（可随时推翻，不构成设计约束）。

### 2.3 ArkTS 侧

- `WineEnvService`：`engineState`（idle/starting/ready/stopping/failed）只被
  `state:*` 迁移；`sessionAlive = desktopActive || runningCount > 0` 派生。
- statusText 拆两槽：`engineStatus`（状态机映射文案）+ 瞬时回执一律 toast。
- 「最后进程退出 → 已就绪」判断挪到 ProcessService 刷新完成回调，不读旧列表。
- 按钮判据矩阵（全部挂 engineState/sessionAlive/initBusy）：
  - 初始化/重启：`idle|ready|failed` 且非 busy；sessionAlive 时文案「重启 Wine 引擎」，
    动作 = stopAll → 等 engine-stopped → doInit（全程 busy）。
  - 停止全部：`sessionAlive` 时可用（不再用 prefixReady 当代理）。
  - 重置 prefix：动作编排 stop → reset → init，全程 busy（实现时放宽为
    "任何非忙状态可用"——停止编排已保证不踩活会话）。
  - 启动（应用库/文件页/长按菜单统一）：`engineState=='ready'`。
- 启动成功判据：spawn 后 3s 内未收到该 pid 的 evt:proc-exited；desktop 模式可进一步
  关联 toplevel 事件。闪退 → 失败对话框（含导出日志入口）。
- winehua_keep 加入 ProcessService 黑名单（基础设施不是用户程序）。
- init overlay 模态化。

## 3. 阶段划分

### 阶段 1：ArkTS 状态机 + 消息协议（纯 ets + 消息发端改名）
- native 仅改消息字符串为 state:/evt: 协议（发射点不变，语义不变）。
- ArkTS：engineState 拆分、失败分支接入、按钮判据矩阵、doInit 防重入、
  重启/重置编排（暂以轮询 desktopActive=false && 进程表空 为停止完成条件）、
  statusText 拆槽、启动守卫统一、winehua_keep 黑名单、「结束」加确认。
- SmokeRunner/automation 适配新协议。
- 验证：arm64 + x86_64 构建；Pad 回归：运行中按钮不误解锁、重启无双桌面、
  杀进程状态复位、失败 overlay 有重试。

### 阶段 2：native 会话终结信号（已实现）
- wineserver pid 登记 + ProcMon 监视；死亡发 evt + state:failed:wineserver
  （ProcMon 1Hz 检测；stopAll/KillAllProcesses 主动停止期间由
  gShutdownRequested 抑制上报）。
- KillAllProcesses 连 wineserver 一起杀（登记后自动覆盖）；zombie 感知等待全死后
  发一次 `state:stopped`（30s 封顶兜底）；ProcMon 判活改 IsProcessAliveNotZombie。
- 修静默失败：marker 失败发消息（阶段1已做）、wineboot 结果检查、state:ready 前
  复查 wineserver 存活。（实现注记：NCP 子进程由 appspawn 立即 reap，host 拿不到
  wineboot 退出码；可判定终点 = spawn 成功 + wineboot 退出后 wineserver 仍存活，
  暖启动播种失败由"仅记日志"改为 state:failed:wineboot。）
- ArkTS：重启/重置/停止改等 `state:stopped`（一次性 waiter，10s 超时硬放行为
  安全网）；wineserver 死亡 → failed 态 + 重试/重置入口（沿用 failInit）。
- 验证：stopAll 后 ps 确认 wineserver 死；kill -9 wineserver UI 立即失败态；
  重启拿到干净会话（新 wineserver pid）。

### 阶段 3：就绪/启动判定精细化
- desktop root 超时 → state:ready-degraded + evt:desktop-ready 后补。
- 启动成功 = 3s 存活（+ desktop 模式 toplevel 关联）。
- PC 窗口模式 explorer spawn 失败纳入判定；init overlay 模态化。
- 验证：闪退程序出失败提示；慢设备桌面延迟文案正确。

## 4. 施工纪律

- 构建唯一合法手段：`make NATIVE_ARCH=arm64-v8a` / `make NATIVE_ARCH=x86_64`
  （约 4 分钟；产物 entry/build/default/outputs/default/entry-default-signed.hap）。
- 提交只含 entry/src；entry/build-profile.json5 的 abiFilters 会被 Makefile 改写，
  提交前剔除；thirdparty/wine 有用户未提交改动，不碰。
- commit message 中文 conventional；不 push（用户明说才 push）。
- 设备：PC 模拟器 `hdc -s 192.168.1.3:8710`；Pad `hdc -t <connectkey>`（IP 会变，
  用 `hdc list targets` 确认）。部署回归需用户配合操作真机。
- 验证手段：`uitest uiInput click/dumpLayout`、`snapshot_display` 拉回 .temp 看图、
  hilog 采集分析。

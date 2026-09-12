# Proton ARM64 Parity —— 工作索引

> 建立日期：2026-09-11
> 分支：`feature/proton-arm64-parity`（工作树 `/home/liufeng/src/WineHua-proton-parity`，基线 `2728523`）
> 依据：`WineHua_Proton_ARM64_Review_and_Codex_Plan_2026-09-10_v2_OHOS`

本目录记录 Proton ARM64 对齐实验（P0—P2）的基线冻结、接口盘点与预检结论。
所有结论标注证据来源；未实测的一律写 `not_tested`，不以推断冒充实测。

# Proton ARM64 Parity —— 工作索引

> 建立日期：2026-09-11
> 分支：`feature/proton-arm64-parity`
> 工作树：`/home/liufeng/src/WineHua-proton-parity`（git worktree，基线 `2728523`）
> 依据：`WineHua_Proton_ARM64_Review_and_Codex_Plan_2026-09-10_v2_OHOS`

## 为什么单独开工作树

原工作树 `/home/liufeng/src/WineHua-arm64ec`（分支 `feature/arm64-heaven-port` @ `2728523`）
带有未提交的产品改动和脏子模块。本实验按要求另开分支与工作树，**不切换、不提交、不清理**原工作树，
从而保证对齐实验失败时可随时丢弃。

```bash
# 原工作树（保持不动）
cd /home/liufeng/src/WineHua-arm64ec     # feature/arm64-heaven-port

# 对齐实验工作树（本目录所属）
cd /home/liufeng/src/WineHua-proton-parity   # feature/proton-arm64-parity
```

Windows 侧入口：`F:\WineHua\proton-parity-worktree\`（指向该工作树的符号链接）。

## 文档地图

| 文件 | 阶段 | 内容 |
| --- | --- | --- |
| `architecture-overview.md` | 总览 | **当前架构与各层改动一览（从这里开始读）** |
| `proton-steam-gap-review.md` | 评审稿 | **从架构看距离"跑 Proton / 跑 Steam"的缺口（需求侧 vs 供给侧对照）** |
| `runtime-baseline.md` | P0 | 基线冻结：源码、脏改动、产物、工具链、测量上下文 |
| `artifact-sha256.txt` | P0 | 现有 HAP / FEX 产物 / 运行时包哈希 |
| `runtime-manifest.json` | P0 | 当前基线运行时的机器可读清单 |
| `proton-arm64-parity.lock.yaml` | P1 | Proton 参考锁定值与本地对应关系 |
| `wine-fex-interface-audit.md` | P1 | Wine/FEX 正式接口逐项盘点（已有 / 需回补 / 阻塞） |
| `patch-ledger.md` | P1 | 补丁分层台账（§8） |
| `aetherium-reference-adoption.md` | P1 | 第三方 OHOS 参考的采用 / 拒绝决定 |
| `ohos-preflight.md` | §12 | OHOS 预检矩阵逐项记录 |
| `ohos-capabilities.json` | §12 | 预检结果的机器可读汇总 |
| `fex-effective-config.json` | P2 预备 | 当前 FEX 生效配置（实测：无配置文件） |
| `p2-release-probe.md` | P2 | FEX Release 参数对齐探针（已实测，产物已构建） |
| `p2-unixlib-build.md` | P2 | UnixLib 回移与四产物构建（已实测，脚本已接入） |
| `p2-device-validation.md` | P2 | 真机验证：UnixLib 已加载、硬件 TSO/未对齐原子不支持（有真实返回值） |
| `p2-effective-config.md` | P2 | FEX 生效配置落地与 SHM 统计实测（配置已被真实读取） |
| `p3-p4-smoke-ab.md` | P3/P4 | smoke 编排 A/B：x86 后端对比、x64+ARM64X 链路验证与瓶颈信号 |
| `fex-build-parity.md` | P2 | FEX 构建参数对齐官方（Release/profiler/TUNE_CPU/RANGES_NATIVE + 缓存签名） |
| `p5-merge-decision.md` | P5 | 合入判定与回退策略（含"目标游戏性能"这条为何尚不满足） |
| `next-steps.md` | P2 | 下一步可执行动作与门禁 |
| `proton-wine-migration-status.md` | R0 | **Proton Wine 本体迁移状态：当前 Wine 不是 Valve 基线，含 R1 最小补丁清单** |
| `runtime-provenance.json` | S0 | **运行时来源清单（源码/产物/设备/配置，含设备实测哈希）** |
| `steam-client-manifest.json` | S0 | **设备上实际 Steam 客户端的清点与 PE Machine 实测** |
| `steam-test-scope.md` | S0 | **测试范围：用哪份运行时/客户端/prefix，测什么不测什么** |
| `steam-startup-trace.md` | S1 | **真实 Steam 启动逐事件追踪（进程/网络/CEF/窗口）** |
| `steam-process-tree.json` | S1 | **本次运行的进程树（NCP child ↔ Windows 可执行名映射）** |
| `steam-first-blocker.md` | S1 | **第一处可复现阻塞：登录窗口不上屏 + 客户端过旧** |

## 2026-09-12 阶段（S 线 / R0）

按《WineHua 下一阶段：Proton Wine 本体迁移与 Steam 登录闭环》把工作拆成两条线：

- **S 线（当前优先）**：真实 Windows Steam 启动 → 登录 → 游戏库 → 下载 → 启动游戏。
  本批完成 S0（清单与范围）与 S1（启动追踪 + 第一处阻塞）。
- **R 线**：先做 R0 定基线。结论是**当前 Wine 不是指定 Valve Wine 基线**，
  见 `proton-wine-migration-status.md`。

R0/S1 的关键实测结论（都是本轮真机取到的）：

1. 设备上的 Steam 是 **2023-07-10 冻结包**，`steam.cfg` 关了自更新，
   客户端自述 `Client version: 0` 并以 `LogonFailure License expired` 拒绝登录 ⇒ 不符合 S0 的客户端要求。
2. 用**默认参数、默认多进程 CEF**启动后：`steam.exe` 起了 5 个 `steamwebhelper.exe`，
   CEF 的 browser/gpu/utility/renderer 四类进程都出现，guest 内 IPv6 HTTP/UDP 连通性测试 SUCCESS。
3. 登录窗口真的被创建了（toplevel #5，`登录 Steam`，705x440，持续 commit），
   **但没有出现在画面上**，且每次 commit 都是 `geo=no`（客户端从未发 window_geometry）。
4. 当前安装的运行时 `bin/aarch64-unix/` 为空 —— **不含 P2 的 UnixLib 产物**。
   P2「UnixLib 已加载」的结论属于当时的探针包，不能自动继承。

### 2026-09-12 13:00 追加：官方客户端已安装并复现

用户按 S0 装了 Valve 官方客户端（`C:\Program Files (x86)\Steam`，build 1788652215，
`steam.exe` 实测为 **x86-64**）。三次冷启动三次复现同一条崩溃链：

```text
steamwebhelper + gpu/network/storage 子进程都起来
→ 启动后 18–41 s（视是否走完整更新检查）steam.exe 写 dumps/assert_steam.exe_*.dmp（随后 crash dump）
→ [ProcMon] steam.exe exit=1 → 全部 webhelper 跟着退出
```

assert 原文（从 dump 里读出）：

```text
Assert( Couldn't get string length ):...\src\vgui2\vgui_surfacelib\Win32Font.cpp:1129
```

**同一句话在 2023 旧包里也出现过，只是不致命**（`Win32Font.cpp (963)` 记一行日志）。
所以这是**我们 Wine 的文本度量缺陷**，新客户端把它变成致命断言。
详见 `steam-first-blocker.md` §0（含异常码 `0xC0000005`、下一步最小复现、以及
「不换 Valve 基线怎么收敛」的决策规则）。

同时暴露一个工具缺口：本轮传了 `WINEDEBUG=err+all,warn+all,+dwrite`（子进程已生效），
但 hilog 里**一条 Wine 的 err/warn 都没有** —— Wine 子进程 stderr 目前没有可读通道，
这个必须先补，否则后面都是盲调。

## 状态总览（2026-09-11）

- P0：已完成基线冻结与哈希记录；**设备侧基线未取**（本机无 `hdc`，真机数据仍为历史口述值）。
- P1：接口盘点完成第一轮，已定位一个结构性缺口——**当前锁定的 FEX 基线不含 UnixLib**。
- P2（大部分）：已用 Proton 官方参数集构建出 Release 版 `libarm64ecfex.dll` / `libwow64fex.dll`
  （`p2-release-probe.md`）；**UnixLib 已通过回移 6 个上游提交解决**，
  `build_fex.sh` 现在能一次产出四个产物并通过 aarch64 + 导出符号断言（`p2-unixlib-build.md`）。
- **真机验证已完成**：UnixLib 在设备上被真实加载；硬件 TSO / 未对齐原子确认为
  内核不支持（errno=22），非接口问题。见 `p2-device-validation.md`。
- **FEX 生效配置已落地并验证**：`FEX_Config.json`（Proton 参考值）随包部署，
  真机上 SHM 统计创建成功，证明配置被真实读取。见 `p2-effective-config.md`。
  剩余：SHM 统计内容读取、逐项生效值导出、随后进入 P3/P4 测量。
- **P3/P4 已有可复现结论**：x86(wowbox64/FEX)、原生 ARM64、真 AMD64+FEX+ARM64X
  四种配置帧时间都在 12.10–12.30 ms（≈82 fps），差异 <1%，
  两次独立运行偏差 ≤0.5%。**CPU 后端不是瓶颈，~12ms/帧在 graphics/present 共同路径**。
  见 `p3-p4-smoke-ab.md`。
- **P5 初步归因**：拆段计时显示 render 仅 0.09–0.17 ms，`Present` 占 11.66–11.95 ms
  （96–98%），且三种配置一致 ⇒ 瓶颈在宿主 present 链路，与 CPU 转译无关。
  换图形栈对照（DXVK/Venus vs WineD3D/virgl）两者 present 都是 10.7–11.7 ms
  ⇒ 这是**两条栈共有的宿主 present/上屏段**固定成本，不是 DXVK/Venus 特有。
- **P5 结论（打到宿主日志后）**：显示周期 **11.129 ms（90 Hz）**，presenter 按它 pacing。
  帧时间主体就是这个节拍。宿主 present 本身只花 2.97 ms（有 ~8 ms 余量）；
  D3D9/WineD3D 几乎正好卡在节拍（+0.04~0.19 ms），
  **DXVK/Venus 路径每帧多约 1.0 ms** 因而错过 90 Hz、落到 ~81 fps。
  CPU 后端差异（FEX vs wowbox64 0.14 ms）只是其中一小部分。
  方法学：该 cube 是节拍受限负载，只能测"超预算多少"，不能做后端绝对排名。
- 原工作树未做任何修改。

## 记录约定

- 状态取值：`not_tested` / `pass_existing` / `pass_new` / `unsupported_with_verified_fallback` /
  `not_applicable` / `blocked`。
- 空值表示**尚未取证**，不代表零错误或默认成功。
- 「脚本已改」「编译通过」「文件名对得上」都不算验收。

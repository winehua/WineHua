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
| `next-steps.md` | P2 | 下一步可执行动作与门禁 |

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

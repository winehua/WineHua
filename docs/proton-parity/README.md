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
| `next-steps.md` | P2 | 下一步可执行动作与门禁 |

## 状态总览（2026-09-11）

- P0：已完成基线冻结与哈希记录；**设备侧基线未取**（本机无 `hdc`，真机数据仍为历史口述值）。
- P1：接口盘点完成第一轮，已定位一个结构性缺口——**当前锁定的 FEX 基线不含 UnixLib**。
- P2：未开始构建。FEX 参考版本 `1cc4b93e`（FEX-2607）在本地对象库中可得，可离线构建。
- 原工作树未做任何修改。

## 记录约定

- 状态取值：`not_tested` / `pass_existing` / `pass_new` / `unsupported_with_verified_fallback` /
  `not_applicable` / `blocked`。
- 空值表示**尚未取证**，不代表零错误或默认成功。
- 「脚本已改」「编译通过」「文件名对得上」都不算验收。

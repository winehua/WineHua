# Proton Wine Core Migration（W0：Wine Delta Audit）

> 分支：`feature/proton-wine-ohos`（工作树 `/home/liufeng/src/WineHua-proton-ohos`，基线 `2728523`）
> Windows 侧入口：`F:\WineHua\proton-ohos-worktree\`
> 日期：2026-09-12
> 阶段：**只做审计，不改代码、不换基线**

## 这份目录是什么

按《WineHua：从 FEX / ARM64EC 基础完成转入 Proton Wine Core 迁移》的 W0 要求，
把「WineHua 自己在 Wine 上加了什么」和「Valve Proton Wine 里已有什么」摆清楚，
并给每一条改动一个明确的迁移决策。

## 交付物

| 文件 | 内容 |
| --- | --- |
| `wine-core-delta.md` | 两个基线的差异全貌：提交数、文件数、NEW/BOTH 分布、按模块的落点 |
| `wine-port-ledger.md` | **主台账**：22 组改动的分类、依赖、风险、迁移方式、验收、回退 |
| `proton-wine-ohos.lock.yaml` | 锁定值：目标 Core、当前基线、冻结分支、审计口径、明确不动的层 |
| `build-risk-map.md` | 换基线后"哪里会炸"的地图 + W1 的构建顺序与止损点 |
| `tools/wine-delta-audit.sh` | 可复现的审计脚本（本目录所有数字都由它产出） |

## 一句话结论

```text
WineHua 在 Wine 上的自有改动 = 61 个提交 / 101 个文件
                                ├─ 40 个文件是 Valve 树里完全不存在的新增（必须新增）
                                └─ 61 个文件是改动上游代码（11.0 → 11.10 已漂移，必须逐个重放）

Valve Proton Wine 已自带：ARM64EC 基础设施、unixlib 机制、wow64、dlls/winewayland.drv 基线
Valve Proton Wine 完全没有：OHOS 的全部平台层（broker / NCP / wineohos.drv / ohos_* 等）
```

## 方法（可复现）

```bash
# A = WineHua wine（本地完整克隆）
# B = ValveSoftware/wine@dc26e618（blobless，仅用于文件清单与按需取 blob）
git -C /tmp/valve-wine-probe ls-tree -r --name-only \
    dc26e61847081a1b5cb0733dc30feba6ee575482 > /tmp/valve_files.txt

docs/proton-wine-core-migration/tools/wine-delta-audit.sh \
    /home/liufeng/src/WineHua-arm64ec/thirdparty/wine \
    /tmp/valve_files.txt origin/master /tmp/w0
```

**刻意不做的事**：不输出全仓 diff 统计。两个前提版本相差一个上游开发周期
（11.10 vs 11.0），全仓 diff 只会有几十万行噪声，对"要迁哪些补丁"没有帮助。

## 完成条件（对照方案 §14）

| 方案要求回答的问题 | 本文档的回答位置 |
| --- | --- |
| 1. Proton Wine 可直接替代当前 Wine 的哪些部分？ | `wine-core-delta.md` §4 |
| 2. 必须移植的 OHOS patch 到底有哪些？ | `wine-port-ledger.md`（分类 = NEED_PORT / AUDIT_AND_PORT） |
| 3. 哪些现有 patch 应删除而不是迁移？ | `wine-port-ledger.md`（分类 = DROP_IF_REDUNDANT） |
| 4. 哪些 patch 与 Proton ARM64 / WoW64 / FEX 正式接口存在冲突风险？ | `wine-port-ledger.md` 的风险列 + `build-risk-map.md` §3 |
| 5. 最小可启动 Proton-Wine-OHOS candidate 需要哪些 patch？ | `build-risk-map.md` §4（M1/M2/M3 三档） |

## 下一步（W1 才做的事）

1. 在**这个工作树**里把 `thirdparty/wine` 的子模块初始化，并把 Valve Wine 加为参考 remote。
2. 按 `build-risk-map.md` §4 的 M1（最小可启动）先动手，而不是一次全迁。
3. 每迁一组就跑 `build-risk-map.md` §5 的 Gate（wineboot / cmd / 32-64 混跑 / FEX / GUI / TLS / IPC）。

## 边界

- 本阶段**没有**修改任何 Wine 源码，也没有换基线：全部是审计结论。
- 数字来自本机只读检查 + 官方仓库；Proton 侧只读了文件清单与少量按需 blob，
  **没有**做完整的按文件内容 diff。
- 未在本文档里下结论的地方，一律标 `待 W1 抽样核对`，不用推断充数。

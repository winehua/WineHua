# Pad 多窗口: 拖拽标题栏后画面层白屏 — 调查记录（已定位+已修复）

> 状态：已被另一条路线取代（2026-09-22 归档）。文中的"整组提升"修复只存在于未合入的 feature/pad-popup-white-screen 分支；master 上白屏问题由另一条路线解决（内嵌客户区合入主窗口帧，6e28f28）。

> 状态: **根因已定 + 修复验证通过** (2026-09-10)
> 现象: Pad 多窗口模式, graphics smoke（画面承载在 popup 子窗口），点/拖标题栏后画面白屏。
> 数据来源: 09-09 10:01 全量 hilog + 09-10 00:03/00:12/00:18 三轮回放（win id 打点 + 抓屏 + WMS z 序 dump）
> 修复所在: `feature/pad-popup-white-screen` 分支工作区（实验 A/B 已撤，最终修复见下）

## 现象

Pad 多窗口模式，graphics smoke 画面承载在 **popup 子窗口**（客户区 subsurface →
伪 toplevel → OHOS 子窗口），启动渲染正常；点/拖标题栏后画面全白。

## 根因：点击 focusable 子窗口 → WMS 自动置顶盖住画面 popup

**Final 证据链（00:18 会话，窗口 id 打点确认）**：

| win | 身份 | 帧内容 |
|---|---|---|
| 343 | 宿主主窗口（Index） | — |
| 345 | smoke 主窗（Fusion 子窗，物理父 343） | 94.7% 纯白（白壳+标题栏） |
| 346 | 画面 popup（子窗，物理父 343） | 全程彩色动画 |

- 点击标题栏 → move_grab_start（0.07s）→ WMS 焦点提升将主窗 345 自动置顶
- `WMS Layout dump`: **idx=0 = 主窗 345 (1936x1134 白壳), idx=1 = 画面 346 (1920x1080)** ← 被盖
- 画面 346 的帧内容全程正常（PIX-SAMPLE 白度恒 0）→ 白屏 = 主窗白壳盖住画面，非渲染问题
- 抓屏（snapshot_display）：可见"蓝标题栏 + 全白内容区"= 主窗 345 本体

**机制**：主窗（WineWindow）`focusable(true)` → 点击获焦 → WMS 按标准窗口行为自动置顶 →
主窗（白帧壳）压住后创建的画面 popup。popup 是 focusable(false) 不会自动抢回过。

## 已排除（前几轮怀疑全部证伪）

- **`CheckGetSubWindowAvoidAreaAvailable: rect mismatch`** — 创建瞬间的常规检查噪音
  （00:12 会话仅 27 条全在创建 0.06s 内；与白屏无时间重合），非触发条件
- **`MoveWindowToGlobal: Layout timeout`** — 补定位时 WMS 布局忙的超时提示，
  后续 move 仍成功（rect 正常），非白屏原因
- **渲染侧（guest/compositor/GL swap）** — 三轮回放全程帧内容正常
- **窗口位置/尺寸** — move 后 winRect 查询均正常 (816,48,1936x1134) / (824,94,1920x1080)

## 修复（已部署验证）

1. **点击置顶显式化**：`WineWindow.ets` onTouch Down/Up → `WineWindowManager.raiseWindowGroup(tl)`
   = Fusion 主窗 `raiseToAppTop()` 后**再** raise 其全部 popup（画面恒在主窗之上）
   - popup 通道语义 = "窗口组"：画面/menu popup 都从属父窗，随组提升（点 B 组提升盖 A，正常）
2. **拖动跟随**：windowRectChange 实时 reposition（去掉原 moveScrubbing 跳过），
   拖动中画面跟随主窗（残留: popup 独立子窗 IPC 跟随有 ≤1 帧滞后，已确认可接受；正解=subsurface 合成进主窗帧，见"后续"）
3. **保留 focusable(true)**：startMoving/系统拖拽需要焦点（曾试 focusable(false) 禁拖动，已回退）

## 后续方向（未实施）

画面合入主窗合成（多窗口模式复用 desktop 模式 subsurface 层合成）：wine 侧 subsurface
本就是"相对父窗口定位的内容"（客户区 = reconfigure_client；菜单 = reconfigure_subsurface
的 rect 差），合成进父窗帧后单窗口随动——z 置顶/跟随滞后/补定位全部方案性消失。
判定精确化不需要阈值（两类 subsurface 由 wine 建模语义区分），但有出窗裁剪问题需验证（菜单）。

## 遗留打点（保留）

- `PIX-SAMPLE`（egl_renderer，每 30 帧 1 条）— 帧 vs 显示分界的唯一诊断手段
- `[WINID]` subWinId/hostMainId + `dumpWindowLayout`（WMS z 序，3s 一次）— 窗口身份对照
- 已删：实验 A（reactivatePopups showWindow，验证无效）、VIRGL-PRESENT 周期打点、moveScrubbing 死代码

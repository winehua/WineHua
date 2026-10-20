# x11 多窗窗口装饰实施计划（路线：系统装饰）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, 单 agent 全程) 实施。

**Goal:** x11 多窗形态补齐窗口装饰——Pad 子窗自绘顶条（系统装饰实证不可用，以 Architecture 为准）、PC WWA 形态系统装饰；关闭出路盘点；known-issues §2.15 装饰行改写。

> **实施状态（2026-10-11 复核记账）**：T0/T1 全部收口；T2 代码落地（9334950），真机面 pending-hardware → #133；T3.1/T3.2/T3.4 收口；T3.3 资产清理转 #134（待用户叫停）。

**Architecture（2026-10-11 decorEnabled 实验后定案：双路线混合）:**
- **Pad subWindow：几何补偿式自绘顶条**（回退路线激活）。依据 = decorEnabled 三水实验（证据 `.temp/decor/`）：① 带内容 fusion 子窗（notepad 画面可见）decorEnabled:true 两轮零装饰 UI；② 裸窗对照连背景色都不合成；③ SDK 文档「装饰按钮只在自由窗口模式生效」——WineHua 全屏 stage 应用内进不了自由窗模式。系统装饰对 Pad 实证不可用。
- 自绘顶条设计：承载窗 h + STRIP_PX、XComponent 下移、三键驱动 M4b 动作链；STRIP_PX = 20 × getEffectiveScale()。判据单点 stripPxOf(tl)（仅 x11 fusion 子窗非 0；fusionUsesSubWindow 不可用——wayland Pad 也是子窗）。
- **PC WWA：OHOS 系统装饰**照旧（T2 执行时实测确认）。
- close 语义统一走 WM_DELETE_WINDOW 通道：sendToplevelClose x11 分支已在工作树（napi_init.cpp + x11_toplevel_close，C 侧已就位）。
- 用户待亲眼核验：可见版 A/B 测试窗（winehua.testDecor 1 + DecorTestPage）——verdict 前保留测试资产。

**Tech Stack:** ArkTS（FusionWindowManager 创建参数、WWA 装饰开关）。

**Spec:** 任务 #129；known-issues §2.15 装饰行。

## Global Constraints

- **禁止 wine 侧改动**（方向 ① 定义）
- incumbent 红线：wayland 路线行为不变（decorEnabled 分叉只在 x11 分支内）
- **WineWindow 页面双逻辑隔离**（用户约束）：装饰判据单点（stripPxOf 语义，必须能区分 x11 fusion 子窗与 wayland fusion 子窗——fusionUsesSubWindow 不够），wayland 路径渲染树/布局/交互与改动前逐字节等价，wayland fusion 对照轮验证
- 判定：真机手测清单 + 截图归档 .temp/decor-*；fusion 套件回归（Pad + PC 两形态）

## Review Focus

1. **顶条与 guest 帧对齐**：承载窗增高后 XComponent 下移、wine 帧顶不缺失、顶条区点击不落 guest（输入 y 换算有无 STRIP_PX 偏差）、菜单栏 y=0 可点
2. **三键与 M4b 动作链**：min/max/close 分别驱动既有 windowEvent/windowStatusChange 链；close = WM_DELETE（wine 收 WM_CLOSE，保存提示语义保留）
3. **隔离**：wayland fusion 子窗 stripPxOf=0、渲染树/布局/交互不变——清单核代码面 + 对照轮验行为
4. **拖动**：f43c764 手势面与顶条共存（顶条区=拖动区），阈值按有效 scale（1b75f80 先例）

---

### Task 0: 可见性测试窗收尾（用户阻塞项，最优先）

- [x] T0.1 `pages/DecorTestPage` 注册进 `entry/src/main/resources/base/profile/main_pages.json`（本仓库全部子窗页面均注册——未注册 → loadContent 失败 → .then 链断 → showWindow 未执行 = 当前「created 但不可见」根因假设；catch 分支已有 `[DECOR-TEST] A/B FAILED` 日志可证）
- [x] T0.2 冷启 `--ps winehua.testDecor 1 --ps winehua.desktopMode fusion` → 立刻 grep DECOR-TEST（缓冲几分钟即滚，落盘留证）→ 截图确认 A/B 可见带内容 → SendMessage 报告 main（转用户亲眼核验）
- [x] T0.3 verdict 前保留测试资产；若用户翻案（看到装饰）→ 回系统装饰路线

### Task 1: Pad 子窗自绘顶条（回退路线激活）

**Files:** `entry/src/main/ets/service/FusionWindowManager.ets`（stripPxOf/补偿/三键）、`entry/src/main/ets/pages/WineWindow.ets`（顶条 UI + 拖动）、`entry/src/main/ets/service/WineWindowManager.ets`（title 同步）、C 侧 close 通道已就位

- [x] T1.1 stripPxOf 恢复真实值（x11 fusion 子窗非 0）+ 承载窗高度补偿 + XComponent 下移
- [x] T1.2 三键（min/max/close）接线 M4b 动作链；close = x11_toplevel_close（C 侧已就位，实测生效）
- [x] T1.3 title 事件 → 顶条标题同步 + 顶条拖动（f43c764 手势保留，阈值按有效 scale）实测
- [x] T1.4 部署实测（双窗截图 .temp/decor-pad-*.jpeg：顶条/标题/三键/内容对齐/顶条区点击不落 guest）+ fusion 套件回归（Pad）
- [x] T1.5 wayland fusion 隔离验证（2026-10-11 复核：以代码面核验为准 ✓（R2），例外 e.title 已记档）+ Commit

### Task 2: PC WWA 系统装饰

**Files:** `entry/src/main/ets/entryability/WineWindowAbility.ets`（decor 开关 x11 形态判定内恢复、D52 定位适配）

- [x] T2.1 x11 形态内 decor visible + 标题按钮（代码落地 9334950；PC 真机截图 pending-hardware → #133）
- [x] T2.2 fusion 套件 PC 形态复跑（fusion-r20261011-pc-ability 在案；--extra-ps winehua.fusionHost ability）+ Commit

### Task 3: 关闭出路盘点 + known-issues 改写

- [x] T3.1 盘点：顶条×键 / Alt+F4 / Pad 任务卡片 / 应用自退——逐项实测归档（§2.15 关闭出路盘点在案）
- [x] T3.2 known-issues §2.15 装饰行改写 + §2.14 关联更新
- [ ] T3.3 测试资产清理（DecorTestPage + testDecor 钩子 + main_pages 条目）——转 #134（待用户叫停）
- [x] T3.4 Commit + push

## Self-Review

2026-10-11 复核更正：原句「路线裁决（用户确认系统装饰可用）取代原自绘顶条方案」与 Architecture/实现三方矛盾（decorEnabled 三水实验证伪 Pad 系统装饰，实现落自绘顶条）——以 Architecture 为准。隔离约束（用户裁定）升格为 Global Constraints。RF 四条均已实测（T2 真机面 pending-hardware → #133）。

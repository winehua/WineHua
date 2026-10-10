# M4d x11 Pad 承载（FusionWindowManager 接 x11 事件源）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** x11 fusion 多窗模式在 Pad 设备上的承载贯通（subWindow 路线）——承载/拖动/手势/多窗并发四项真机手测通过，x11 路线 Pad 多窗形态具备替代能力。

**Architecture:** x11 的 created 事件流与 wayland 共用 WineWindowManager 分发（M4a-T4 接线）；Pad/PC 承载判定 `fusionUsesSubWindow()`（WineWindowManager.ets:243）只看设备形态 + `winehua.fusionHost` 调试键，与显示路线无关——**分发层预计零改动**，M4d 增量 = 事件流通路验证 + subWindow 渲染回绑/输入验证 + Pad 真机手测 + 承载差异修复。

**Tech Stack:** ArkTS（FusionWindowManager/WineWindowManager 走查）、C（plugin_manager 渲染回绑核实）、Pad 真机。

**Spec:** docs/superpowers/specs/2026-10-07-display-route-x11-multimode-m4-design.md §4（Pad: createSubWindow×N）、§5 风险表（Pad subWindow 承载近似）、§6 M4d 行。

## Global Constraints

- spec §6：M4c 通过前不开 M4d（前置 = M4b/M4c 验收 PASS）。
- Pad subWindow 承载近似（hide/minimize 无系统能力）= **继承 incumbent 已知限制，不新增债**（§5 红线）——修复冲动对照 FusionWindowManager 现状，incumbent 没有的能力不做。
- Pad 设备清单与限制见 docs/assets/devices.md。
- 验收方式 = 真机手测清单（spec §6：本阶段无 smoke 判定要求），每项截图/日志归档 `.temp/m4d-*.jpeg|log`。

## Review Focus

1. **fusionUsesSubWindow 的设备判定分支**：x11 fusion 事件到达时 PC/2in1 走 ability、Pad 走 subWindow——若设备形态判定依赖 wayland 时代的窗口系统查询（自由窗口能力探测），x11 下行为一致性要实测（执行时核实判定函数的全部分支）。
2. **subWindow 的 surfaceId 回绑**：plugin_manager CreateRenderer 的 x11 分支按 toplevelId 查 entry 表（M4a-I5）——FusionWindowManager 的 subWindow surfaceId 到达时 toplevelId 是否一致传递（`wine_fusion_${tl}` 命名含 tl，页面侧 pending 机制同 WineWindow——走查 setPendingToplevel 的 FIFO 在 Pad 双窗并发下的顺序）。
3. **拖动语义分叉**：wayland Pad 拖动 = xdg_toplevel.move → OHOS 系统拖拽（FusionWindowManager.ets:251）；x11 无 xdg——x11 Pad 拖动 = 标题栏触摸 → ？

   设计定案：x11 侧标题栏拖动走 **ArkTS 手势面本地移动 subWindow**（request_configure 的 OHOS 位置回写已有 M4a 机制）， wine 侧不感知位置（managed 模式位置由合成器权威）——与 PC 承载同构。执行时核实 request_configure 的位置应答链在 Pad 布局下的正确性（坐标基准：X 全局 vs subWindow 局部）。
4. **多窗并发的 subWindow 上限**：FusionWindowManager 表驱动，Pad 系统 subWindow 数量上限实测（4-6 窗并发）——超限行为 = 排队还是拒绝，与 wayland incumbent 对齐。
5. **手势面缺失项**：WineWindow.ets 手势状态机（双击最大化/长按右键）在 FusionWindow 承载页是否齐（D15 的 wayland 教训——x11 Pad 同样要过手势清单）。

---

### Task 1: 事件流通路走查（零改动假设验证）

**Files:**
- 走查（不修改）: `entry/src/main/ets/service/WineWindowManager.ets`（created 分发 → fusionUsesSubWindow 分支 → FusionWindowManager.ensure）
- 走查: `entry/src/main/cpp/bridge/plugin_manager.cpp`（CreateRenderer x11 分支对 subWindow surfaceId 的处理）

- [ ] **Step 1: 分发链走查**——x11 created 事件 → `fusionUsesSubWindow()==true` → FusionWindowManager.ensure(tl,w,h) 的每一跳，列出依赖的 route 判定点（`displayRouteIsX11()` 应只出现在门禁，不出现在承载选择）
- [ ] **Step 2: 回绑链走查**——subWindow XComponent surfaceId → createRenderer(tl, surfaceId) → x11 分支 attach；toplevelId 传递一致性（Review Focus #2）
- [ ] **Step 3: 走查结论入 ledger**（零改动确认 or 缺口清单）

### Task 2: Pad 真机手测——承载与渲染

- [ ] **Step 1**: Pad 设备冷启 `--ps winehua.displayRoute x11 --ps winehua.desktopMode fusion --ps winehua.program 'C:\windows\notepad.exe'`——subWindow 出现且有内容（截图 `.temp/m4d-pad-carry.jpeg`）
- [ ] **Step 2**: 双窗并发（notepad + fusion_probe）——两 subWindow 独立呈现
- [ ] **Step 3**: 按现象修（渲染黑窗 = D50 同款指纹排查；回绑失败 = Review Focus #2）

### Task 3: Pad 真机手测——拖动/手势

- [ ] **Step 1**: 标题栏拖动 subWindow（Review Focus #3 定案实现，若需代码补 x11 分支——先测后补）
- [ ] **Step 2**: 手势清单（双击标题栏/长按右键——对照 WineWindow 手势面与 D15 清单）
- [ ] **Step 3**: Commit（若有修复）

### Task 4: 多窗并发 + 已知限制注记

- [ ] **Step 1**: 4-6 窗并发压测（subWindow 上限实测，对照 wayland incumbent 行为——Review Focus #4）
- [ ] **Step 2**: known-issues 注记：Pad 承载近似限制清单（hide/minimize 无系统能力——逐项对照 FusionWindowManager incumbent 现状）
- [ ] **Step 3**: Commit

### Task 5: 回归门禁 + 收口

- [ ] **Step 1**: displayroute 前缀组 + core（判据同前：11/13 + 3/4——Pad 设备跑 core 或 PC 设备代跑，按 devices.md 设备可用性定）
- [ ] **Step 2**: M4d 收口文档（三形态对账更新：x11 虚拟桌面 ✓ / PC 多窗 ✓ / Pad 多窗 ✓→退役条件评估输入）
- [ ] **Step 3: Commit**

---

## Self-Review 记录

1. **Spec 覆盖**：§6 M4d 行（承载/拖动/手势/多窗并发手测清单）= T2/T3/T4；§5 承载近似限制 = T4 Step2 注记 + Global Constraints 红线；§4 Pad createSubWindow×N = Task 1 走查。
2. **占位符扫描**：Review Focus #3 拖动语义给了设计定案（ArkTS 本地移动，与 PC 同构）+ 核实点（位置坐标基准）；#1/#2/#4/#5 均有走查锚点。
3. **前置链**：M4a 验收（等设备）→ M4b（cac2b17）→ M4c（e608855）→ M4d（本计划）。
4. **无 smoke 判定**：spec §6 明确 M4d = 真机手测清单，不新增套件——与「产品不为测试让步」一致。

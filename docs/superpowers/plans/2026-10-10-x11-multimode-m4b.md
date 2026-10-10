# M4b x11 多窗口状态面 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** x11 fusion 多窗模式的窗口状态面双向映射——minimize/fullscreen/activate/modal 四语义在 wine→映射层→ArkTS 承载层全链贯通，x11-fusion 套件扩展验收。

**Architecture:** 映射层（x11_toplevel.c）新增 4 个 xs 事件 listener，直译到 toplevel_event_bus **已有**枚举（Minimized/Fullscreen/Unfullscreen/Modal/Restored——wayland 路线已定义，桥与 TSFN 通道复用）；ArkTS 承载层消费新事件驱动承载窗动作（移后台/全屏/置前/归属跟随）。探针自驱动状态序列 + host 判定。

**Tech Stack:** C（x11_toplevel.c，wlroots xs 事件）、ArkTS（WineWindowManager/WineWindow）、mingw（fusion_probe 扩展）、python（判定器）。

**Spec:** docs/superpowers/specs/2026-10-07-display-route-x11-multimode-m4-design.md §3.1 状态面行、§5 风险表、§6 M4b 行、§7 判据。

## Global Constraints

- spec §6：每阶段独立提交；**M4a 通过前不开 M4b**（本计划执行前置条件：x11-fusion 套件 PASS）。
- 映射层纪律（M4a 已立）：只做映射不做策略；22 事件名走 ToplevelEventName 红线；listener 必须 wl_list_init 防未 add 被 remove 崩；napi 线程只入队。
- 设备端只跑不判；判定只读归档数据。
- 套件钉死档位：backend.d3d 与 backend.dxvk 成对声明；跑批 CLI 显式 `--desktop-mode fusion`。
- 改 `entry/src/main/cpp/` 只需 `make NATIVE_ARCH=arm64-v8a hap`；不碰 wine 源码。
- 回归门禁：每阶段跑 displayroute-win32-interactive 前缀组（spec §5「虚拟桌面回退」对策）。

## Review Focus

1. **listener 生命周期**：新 4 listener 若 destroy 先于 add 到达（D50 轮 notify_new_surface 同款竞态）——每个必须先 wl_list_init；验收手段 = 代码审查时逐个核对 init/add/remove 三点配对。
2. **request_activate 风暴**：wine 在焦点切换时可能连发 activate（explorer 桌面窗与程序窗交替）——ArkTS 侧置前需幂等（已在前的窗不重复 raise），否则承载窗管理器抖动。测试 = 双窗交替点击 10 次，无异常日志风暴。
3. **modal owner 已死**：set_parent 到达时 owner 可能已 destroyed（owner 表项 dead）——映射层只报「owner 在册」的 modal，owner 不在册降级为普通事件（spec §3.5「owner 在册」条件）。
4. **全屏尺寸回写环**：fullscreen 事件触发 ArkTS 承载窗全屏 → onSurfaceChanged resize 回写 → request_configure 应答（T9 教训：必须 wlr_xwayland_surface_configure 回应答）→ wine 再请求 fullscreen —— 断环判据 = 全屏切换一次成功后无 resize 事件流。
5. **探针状态序列的 guest 时序**：SW_MINIMIZE 后 wine 处理有延迟，探针每步状态写入前必须等 win32 状态确认（IsIconic/GetWindowPlacement 轮询），不能按固定 sleep 写终态。

---

### Task 0: explorer 桌面窗处置决策（M4a final review I2 强制项）

**Files:**
- Modify: `entry/src/main/cpp/display/x11_toplevel.c`（若决策 = 过滤）
- Modify: `docs/engineering/display-route-known-issues.md`（若决策 = 接受，记录语义边界）

**Interfaces:**
- Consumes: M4a 实测（D50 附带发现 + final review）：managed 模式 wine user32 `get_desktop_window` 自动 spawn explorer /desktop，其 desktop xs 走 created → startFusionSubWindow 被当普通承载窗（屏幕尺寸全屏窗），spec §2/§3.4「managed 不启 explorer desktop」只在 wine_launch 分支成立
- Produces: 三选一的**已实施处置**，M4b 后续任务以此为前提

- [x] **Step 1: 决策并实施（三选一，先在会话内陈述理由再动手）——选 a 接受**

  a. **接受**（最低成本）：explorer 桌面窗作为底层承载窗存在，M4b 的 activate/minimize 流在其上自然工作（D50 验证轮已见 notepad 正确压其上）；代价 = 每个 fusion 会话多一个无用全屏窗 + raise 顺序参与仲裁。落地 = known-issues 记录 + M4b 判定器容忍其存在（帧里有 explorer 桌面底色不算 FAIL）。
  b. **按 class 过滤**：映射层对 `xs->role`/window class = explorer 桌面特征（执行时实测抓取具体值）不 post created（同 skip-no-window 语义）；代价 = 需实测特征值且 wine 升级可能变。
  c. **抑制 spawn**：修 wine_launch/user32 路径让 managed 模式不自动 spawn（越出 M4 范围，碰 wine 源码需完整构建——除非 a/b 都不可行否则不选）。

- [x] **Step 2: 判定器适配**（选 a/b 时）——判定器容忍要求已写入 known-issues §2.12, M4b-T4 实帧验证

  `validate_fusion_window_*` 在 M4b 状态序列帧里对 explorer 桌面窗的象限外内容（桌面底色/图标）不误判；实帧验证。

- [ ] **Step 3: Commit**（决策记录 + 代码/文档落地，一行 message 注明三选一结果）

---

### Task 1: 映射层状态面 listener（minimize/fullscreen/activate/parent）

**Files:**
- Modify: `entry/src/main/cpp/display/x11_toplevel.c`（4 listener + 事件翻译）
- Modify: `entry/src/main/cpp/display/x11_toplevel_bridge.cpp`（4 个 post 桥：post_minimized/post_fullscreen/post_activated/post_modal）
- Modify: `entry/src/main/cpp/compositor/toplevel/toplevel_event_bus.h`（仅当缺 Activate 语义时评估复用——**执行时核实**：wayland 侧无独立 activate 事件，置前由 ArkTS 焦点模型驱动；x11 侧对齐 = request_activate → 复用 `Restored`？否——新增枚举违反「22 事件红线」。定案：request_activate 走**既有 TSFN 的置前通道**（WaylandServer::RaiseToplevel 的 x11 等价直调，不经事件枚举），理由：与 wayland 同构）

**Interfaces:**
- Consumes: `wlr_xwayland_surface.events.request_minimize`（`struct wlr_xwayland_minimize_event {struct wlr_xwayland_surface *surface; bool minimized;}`）、`events.request_fullscreen`、`events.request_activate`、`events.set_parent`（parent 变化，`surface->parent` 取新值）+ 状态位 `xs->modal`
- Produces:
  - `x11_toplevel_bridge_post_minimized(uint32_t id, bool minimized)` → bus `Minimized`/`Restored`
  - `x11_toplevel_bridge_post_fullscreen(uint32_t id, bool fs)` → bus `Fullscreen`/`Unfullscreen`
  - `x11_toplevel_bridge_post_activated(uint32_t id)` → RaiseToplevel 等价直调（不经枚举）
  - `x11_toplevel_bridge_post_modal(uint32_t id, uint32_t owner_id, bool modal)` → bus `Modal`（JSON 带 owner）

- [x] **Step 1: 四 listener 挂接（notify_new_surface 内）**

```c
/* entry 模式照抄 set_title/request_configure: 字段声明 + wl_list_init +
 * notify_new_surface 的 wl_signal_add + detach_listeners 的成对摘除 */
wl_list_init(&e->request_minimize.listener);   /* 其余 3 个同款 */
```

request_minimize 处理（`struct wlr_xwayland_minimize_event *ev`）：
`ev->minimized ? post_minimized(id,true) : post_minimized(id,false)`（false=Restored）。
request_fullscreen：读 `xs->fullscreen` 状态位（信号无 payload）post_fullscreen(id, xs->fullscreen)。
request_activate：查 entry → `x11_toplevel_bridge_post_activated(id)`（桥内做 RaiseToplevel 等价，见桥 Step）。
set_parent：`owner = entry_of_xs(xs->parent)`；`owner ? post_modal(id, owner->toplevelId, xs->modal) : 降级 log`（Review Focus #3）。

- [x] **Step 2: 桥实现（x11_toplevel_bridge.cpp）**

前三个照抄 post_title 模板（JSON 构造 + ToplevelEventType + Post）。
post_activated：**执行时核实** wayland 置前终点——`grep -n "RaiseToplevel" entry/src/main/cpp/compositor/`，x11 桥直调同一 ToplevelManager/WineWindowManager 终点；若终点在 ArkTS 侧（napi 回调），则发 `Restored` 同款 TSFN 通道带 `event=activate` 自定义字符串并同步扩 ToplevelEventName（红线修改点，commit message 记录依据）。

- [x] **Step 3: 构建 + 已有套件不回归**

Run: `make NATIVE_ARCH=arm64-v8a hap`
Expected: 构建绿（wl_list_init 配对齐全，无编译错）。

- [ ] **Step 4: Commit**

```bash
git add entry/src/main/cpp/display/ entry/src/main/cpp/compositor/
git commit -m "feat(display): M4b-T1 映射层状态面——minimize/fullscreen/activate/parent 四事件直译"
```

---

### Task 2: ArkTS 承载层动作

**Files:**
- Modify: `entry/src/main/ets/service/WineWindowManager.ets`（4 事件分发：minimized→承载窗移后台、restored→拉回、fullscreen→承载窗 setWindowLayoutFullScreen、modal→记录 owner 关系）
- Modify: `entry/src/main/ets/pages/WineWindow.ets`（全屏模式响应：layoutFullScreen 切换 + 全屏态下隐藏系统栏）

**Interfaces:**
- Consumes: Task 1 的 TSFN 事件（ArkTS setToplevelCallback 现有分发点扩 case）
- Produces: 承载窗状态动作幂等（已在目标态不重复调用系统 API——Review Focus #2/#4）

- [ ] **Step 1: 事件分发**

```ts
// WineWindowManager toplevel 回调新增 case（照抄 resize 分支形态）:
case 'minimized':   // 移后台: window.minimize()（API 12 窗口最小化）——执行时核实
                    // 该 API 名（window.Window#minimize）与 x11 承载窗可用性
case 'restored':    // 拉回前台
case 'fullscreen':  // this.setWindowFullScreen(true)
case 'unfullscreen':
case 'modal':       // 记录 modalOwner 关系表（跟随置顶用）
```

- [ ] **Step 2: 幂等守卫**

每个动作先查当前态（windowLayoutFullScreen 当前值/minimize 态），目标态==当前态直接 return（Review Focus #4 断环）。

- [ ] **Step 3: 构建 + Commit**

```bash
make NATIVE_ARCH=arm64-v8a hap   # ArkTS 构建绿
git add entry/src/main/ets/
git commit -m "feat(ets): M4b-T2 承载层状态动作——minimize/fullscreen/modal 分发 + 幂等守卫"
```

---

### Task 3: fusion_probe 状态序列（探针自驱动）

**Files:**
- Modify: `smoke/programs/win/fusion_probe.c`（`--probe-state` 模式：基础双窗后自动跑状态序列，每步 `IsIconic`/`GetWindowPlacement` 轮询确认后写 result JSON `stateSeq`）

**Interfaces:**
- Consumes: M4a 的双窗骨架（wnd_a/wnd_b）
- Produces: result JSON 扩 `"stateSeq": ["minimized_a","restored_a","fullscreen_a","unfullscreen_a","modal_b_owned_a"]` 每步实际达成态（Review Focus #5：状态确认驱动，非 sleep）

- [ ] **Step 1: 状态序列实现**

```c
/* 序列 (每步: 动作 → 轮询确认 ≤2s → 记录):
 * 1. ShowWindow(wnd_a, SW_MINIMIZE) → IsIconic(wnd_a)
 * 2. ShowWindow(wnd_a, SW_RESTORE)  → !IsIconic
 * 3. 全屏: SetWindowLongA 去边框 + SetWindowPos(0,0,screen) → GetWindowRect 覆盖全屏
 * 4. 还原原 rect
 * 5. CreateWindowExA owned 弹窗 (WS_POPUP, owner=wnd_a) → GWL_HWNDPARENT 断言
 * 每步结果写 result JSON (RUNNING 心跳实时更新 stateSeq) */
```

- [ ] **Step 2: mingw 构建绿**

Run: `python3 automation/smoke.py build --suite fusion`
Expected: fusion_probe.exe 重编译成功。

- [ ] **Step 3: Commit**

```bash
git add smoke/programs/win/fusion_probe.c
git commit -m "feat(smoke): M4b-T3 fusion_probe 状态序列——minimize/restore/fullscreen/owned 自驱动"
```

---

### Task 4: 判定器 + 套件扩展 + 验收跑批

**Files:**
- Modify: `automation/checks/frame.py`（fusion-state 判定器：stateSeq 完整性 + 全屏帧内容判定）
- Modify: `automation/checks/__init__.py`（注册 + result JSON 校验）
- Modify: `smoke/tests/fusion-probe/test.json`（checks 追加 `fusion-state`）
- Modify: `docs/engineering/display-route-known-issues.md`（M4b 状态行）

**Interfaces:**
- Consumes: Task 3 的 stateSeq JSON + Task 1/2 的全链
- Produces: x11-fusion 套件 M4b 判定 PASS

- [ ] **Step 1: 判定器（合成数据自测先行，同 M4a 方法）**

`validate_fusion_state(result_json)`：stateSeq 5 步全到位 + 顺序正确。
`validate_fusion_fullscreen_frame(png)`：全屏帧 = 红底覆盖（窗 A 全屏态截图）。

- [ ] **Step 2: 真机跑批**

```bash
hdc -t <DEV> shell "aa force-stop app.hackeris.winehua"
python3 automation/smoke.py run --job smoke/jobs/x11-fusion.json --desktop-mode fusion --device <DEV>
```
Expected: fusion-probe 全 checks PASS（M4a 的双窗判定不回归 + 新 fusion-state PASS）。

- [ ] **Step 3: 手工对照（自动化≠功能可用，原则 21）**

双窗下点击窗 B → 窗 B 置前；窗 A 最小化图标还原 → 内容完整；截图归档 `.temp/m4b-manual-*.jpeg`。

- [ ] **Step 4: Commit**

```bash
git add automation/ smoke/ docs/
git commit -m "feat(smoke+checks): M4b-T4 状态面判定 + x11-fusion 套件扩展"
```

---

### Task 5: 回归门禁 + 收口

- [ ] **Step 1: displayroute-win32-interactive 前缀组全量**

Run: 同 M4a T7 的跑法
Expected: 11/13 维持（keyboard 两项已知 XIM 缺口）。

- [ ] **Step 2: core 基线**

Run: `python3 automation/smoke.py run --suite core --device <DEV>`
Expected: 3/4（opengl-x86 = D49 已知）。

- [ ] **Step 3: known-issues M4b 段 + Pad 限制注记**

spec §5：Pad subWindow 承载近似（hide/minimize 无系统能力）= 继承 incumbent 限制，注记进验收说明（M4d 才做 Pad 承载，本阶段 Pad 形态仍走旧路）。

- [ ] **Step 4: Commit + 文档**

```bash
git add docs/
git commit -m "docs: M4b 收口——回归结果与已知限制"
```

---

## Self-Review 记录

1. **Spec 覆盖**：§6 M4b 行 = 状态面四语义（T1 映射 + T2 承载 + T3 探针 + T4 验收）；最小化还原/全屏切换/模态属主三判定全落 T4；§5 风险 #4（承载近似）落 T5 Step3；回归门禁落 T5。
2. **占位符扫描**：两个「执行时核实」点（activate 置前终点 / window minimize API 名）均为接口事实核对而非设计空白——步骤内给了核实方法（grep 锚点）与两分支的走法。
3. **类型一致性**：post_* 桥签名与 T1 Produces 块一致；stateSeq 字符串与 T4 判定器字面值一致。
4. **Review Focus**：#1→T1 Step1（wl_list_init 配对）；#2→T2 Step2（幂等）；#3→T1 Step1（owner 在册降级）；#4→T2 Step2（断环）；#5→T3 Step1（状态确认驱动）。

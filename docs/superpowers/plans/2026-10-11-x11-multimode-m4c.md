# M4c x11 弹出层（override_redirect → popup 子窗）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** x11 fusion 多窗模式下 wine 菜单/弹出层（override-redirect 窗）→ OHOS 独立子窗（PopupWindowManager 整链复用），越界菜单项可点可显示。

**Architecture:** 映射层 notify_new_surface 入口按 `xs->override_redirect` 分叉：OR 窗进独立 popup 表（不进 toplevel 表/bus created 流），map/configure/unmap 直译 popup_show/move/resize/hide（X 全局坐标直传）；渲染合流进 M4a 的 per-xs 渲染循环（popup entry 同 dirty/render_tick/present 机制，**定案：不合流「无 id 渲染目标」，原因：M4a 渲染机制已真机验证，popup 与 toplevel 渲染需求相同，分叉只在事件语义——单独渲染循环是重复代码）；ArkTS PopupWindowManager 现有管线零改动消费（事件 JSON 形态对齐 wayland）。

**Tech Stack:** C（x11_toplevel.c popup 分支）、ArkTS（PopupWindowManager 零改动/WinePopup 输入接线）、python（判定器）。

**Spec:** docs/superpowers/specs/2026-10-07-display-route-x11-multimode-m4-design.md §3.5、§6 M4c 行（验收 = 菜单用例：越界菜单项可点）。

## Global Constraints

- spec §6：M4b 通过前不开 M4c（本计划前置 = M4b 验收 PASS）。
- popup 判据无启发式只看 X 属性：`override_redirect == true`（§3.5 红线）。
- popup xs 不进映射层 toplevel 表、不发 created（§3.5）。
- 映射层纪律同 M4a（wl_list_init 配对 / napi 只入队 / 事件名红线——popup_* 四事件已在 ToplevelEventName）。
- 判定只读归档数据；跑批 `--desktop-mode fusion` 显式钉档。
- **模态/owned 链的宿主侧验证（M4b 移交，final review I1）必须走「owned 窗创建时
  建立」**：探测 CreateWindowExA 带 owner 的路径（创建即写 transient → manage →
  set_parent fire），不得用运行期 owner 变更（SetWindowLongPtr GWLP_HWNDPARENT）
  驱动。依据：wine 只在创建/管理路径写 `XSetTransientForHint`（set_style_hints，
  wine window.c:1162），运行期 owner 变更不经过任何重写路径 → xwm 的
  WM_TRANSIENT_FOR handler 不 fire → 宿主 set_parent 结构性不可达（M4b-T4 真机
  实测一致；known-issues §2.13 遗留观察）。

## Review Focus

1. **OR 属性读取时机**：xwm 对 OR 窗可能不发 associate（无 wl_surface）——popup 分支必须在 notify_new_surface 最早入口判 `xs->override_redirect`，不能依赖 surface 存在；渲染侧 OR 窗 buffer 到达可能晚于 show 事件（同 M4a created 补发兜底模式，帧钟收口）。
2. **popupId 空间碰撞**：wayland popupId 与 x11 toplevelId 共享 ArkTS 侧 setPendingToplevel/manager 表——x11 popupId 用独立基址（`X11_POPUP_ID_BASE = 1<<20`）避让两个既有 id 空间；验收 = 双路线交替跑各自 id 无串扰。
3. **wine 菜单的 grab 语义**：wine 菜单打开时 XGrabPointer（wine 自建事件捕获）——popup 显示期间合成器的 pointer 焦点若抢菜单 xs 之外的事件，菜单项收不到 motion；对策 = popup_show 时 FocusClient(popup xs)，popup_hide 后还焦点给 owner。
4. **popup_hide 时机**：wine 关菜单 = OR 窗 unmap，若 xwm 只发 destroy 不发 unmap，hide 判定要兼容两种终态（unmap OR destroy 都触发 popup_hide）。
5. **越界裁剪**：X 全局坐标可为负（菜单越出屏幕左/上边界）——OH 子窗定位需要钳位或允许负偏移的实测确认（PopupWindowManager 现有 offX/offY 语义执行时核实），判定用例含负坐标菜单。

---

### Task 1: 映射层 popup 识别与事件翻译

**Files:**
- Modify: `entry/src/main/cpp/display/x11_toplevel.c`（OR 分叉 + popup 表 + 4 事件翻译）
- Modify: `entry/src/main/cpp/display/x11_toplevel_bridge.cpp`（post_popup_show/move/resize/hide——复用 bus Popup* 枚举，JSON 形态对齐 wayland：`{popupId,x,y,w,h,argb}`）

**Interfaces:**
- Consumes: `xs->override_redirect`（xwayland.h:16）；OR xs 的 surface map/unmap 信号（`xs->surface->events.map/unmap`——**执行时核实** OR 窗 associate 行为，无 surface 时退 X 事件级别：xwm 的 map_request 对 OR 窗的行为）
- Produces:
  - popup entry 进 g_entries（`is_popup` 标记）参与渲染，**不**发 created
  - popup_show（map 时，`{popupId: X11_POPUP_ID_BASE+idx, x: xs->x, y: xs->y, w, h, argb:0}`）
  - popup_move/resize（request_configure 应答后，X 全局坐标）
  - popup_hide（unmap 与 destroy 双触发，幂等）

- [ ] **Step 1: OR 分叉 + popup 表**

notify_new_surface 入口：`if (xs->override_redirect) { popup_entry_init(e); return; }`（跳过 toplevel listener 挂接与 created 兜底）；popup listener：surface map/unmap + destroy + request_configure（move/resize 用）。

- [ ] **Step 2: 事件翻译 + 桥**

按 Interfaces 块翻译；hide 幂等（`e->popup_visible` 标记去重，Review Focus #4）。

- [ ] **Step 3: 构建**

Run: `make NATIVE_ARCH=arm64-v8a hap`
Expected: 绿。

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(display): M4c-T1 映射层 popup 识别——OR 分叉 + popup_show/move/resize/hide 直译"
```

---

### Task 2: ArkTS 接线（PopupWindowManager 消费 + 输入让渡）

**Files:**
- Modify: `entry/src/main/ets/service/WineWindowManager.ets`（toplevel 回调新增 popup_* case → PopupWindowManager.handlePopupEvent 现有入口；parentToplevel = owner toplevelId）
- Modify: `entry/src/main/cpp/display/display_input.c` 或 `x11_toplevel.c`（popup 打开期间 FocusClient(popup xs)，hide 后焦点还 owner——Review Focus #3）

**Interfaces:**
- Consumes: Task 1 事件；PopupWindowManager 现有 handlePopupEvent（PopupWindowManager.ets:105-114 case 已全）
- Produces: OH 子窗创建/定位/销毁零改动复用

- [ ] **Step 1: 分发接线**（x11 popup 事件按 wayland 同款 JSON 进现有 case）
- [ ] **Step 2: 焦点让渡**（popup_show → FocusClient(popup xs)；popup_hide → FocusClient(owner xs)）
- [ ] **Step 3: 构建 + Commit**

```bash
make NATIVE_ARCH=arm64-v8a hap
git commit -m "feat(ets+display): M4c-T2 popup ArkTS 接线 + seat 焦点让渡"
```

---

### Task 3: 渲染与呈现验证（notepad 菜单真机）

**Files:**
- Modify: `entry/src/main/cpp/display/x11_toplevel.c`（如 popup entry 需要渲染特判——预计零改动，合流设计）

**Interfaces:**
- Consumes: M4a render_tick（popup entry 已在表内）；ArkTS WinePopup surfaceId 回绑（createRenderer 的 x11 分支按 popupId 查 popup entry——**执行时核实** plugin_manager CreateRenderer 的 id 查表函数是否需 popup 分支）

- [ ] **Step 1: 真机场景验证**：fusion 冷启 notepad → uitest 点「格式(O)」→ 菜单子窗出现且有内容（截图归档 `.temp/m4c-popup-1.jpeg`）→ 点菜单项（越界场景：先点靠底部菜单）→ 菜单关闭无残留
- [ ] **Step 2: 按现象修**（渲染/定位/焦点三类问题各自的日志指纹：XTL popup render 缺失 = 渲染断；子窗位置错 = 坐标换算断；点击无响应 = 焦点让渡断）
- [ ] **Step 3: Commit**（若有修复）

---

### Task 4: 判定器 + 套件扩展

**Files:**
- Modify: `automation/checks/frame.py`（`validate_popup_menu`：popup 帧内容判定——菜单底色+条目高亮块；负坐标用例数据）
- Modify: `smoke/tests/fusion-probe/test.json`（或新增 fusion-popup 用例：探针/编排点菜单 + popup 判定）
- Modify: `smoke/suites/fusion.json`（追加用例）

- [ ] **Step 1: 判定器 + 合成数据自测**（同 M4a/M4b 方法）
- [ ] **Step 2: 真机跑批**（`--job x11-fusion.json --desktop-mode fusion`）Expected: 全 PASS
- [ ] **Step 3: Commit**

```bash
git commit -m "feat(smoke+checks): M4c-T4 popup 判定 + 套件扩展"
```

---

### Task 5: 回归门禁 + 收口

- [ ] **Step 1: displayroute 前缀组 + core**（判据同前：11/13 + 3/4）
- [ ] **Step 2: known-issues M4c 段**
- [ ] **Step 3: Commit + 文档**

---

## Self-Review 记录

1. **Spec 覆盖**：§3.5 四行全落（判据=T1 OR 分叉、事件面=T1 翻译+T2 接线、呈现=T1 合流定案+T3 验证、输入=T2 焦点让渡）；§6 M4c 验收（越界菜单项可点）= T3 Step1 + T4 判定。
2. **占位符扫描**：三个「执行时核实」（OR 窗 associate 行为 / createRenderer popup 查表 / offX 负偏移语义）均有 grep/实测锚点，非设计空白。
3. **类型一致性**：popup_show JSON 与 PopupWindowManager.ets:105-117 消费形态一致（popupId/x/y/w/h/argb）。
4. **Review Focus**：#1→T1 Step1；#2→T1 Step1（ID_BASE）；#3→T2 Step2；#4→T1 Step2；#5→T4 判定用例。

# D26 决策输入：x11 路线三形态对账——PC/Pad 多窗口差距盘点

> 状态：成稿（盘点完成，待用户拍板；决策记录另行落 docs/decisions/）
> 目的：给「x11 路线是否/如何补齐 PC 多窗口与 Pad 多窗口形态」提供决策输入。
> 最后核实：2026-10-07

## 0. 背景与定位

**退役判据**（用户 2026-10-06 明确）：自研窗口栈退役的前提是 x11 路线在全部三种形态上
都有可替代的产品方案——虚拟桌面、PC 多窗口、Pad 多窗口。单一形态可日常使用 ≠ 退役条件。

**方向校准**（用户 2026-10-06 拍板）：wlroots 直接服务 winewayland.drv 是后续
compositor 工作的基准方向；fusion/wayland 是长期产品线，「x11 收口 → M3 退役」只是
驱动 x11 搭建的目标设定，不是产品规划。mutter 候选已否决（GPL）。

**合流含义**：两条路线共享 wlroots 基座与 `entry/src/main/cpp/display/` 胶水
（ohos_output.c 呈现链、display_input.c 本就是 wlr_seat 输入面）。x11 路线补多窗口
所建的产品层（OHOS 多窗宿主、per-surface 呈现、窗口状态映射）与 fusion 换引擎是
**同一份资产的两个消费方**——本文按此视角盘点，方案取舍同时回答「x11 怎么补」与
「fusion 换引擎怎么复用」。

**验收基准**：wine bind 名单（winewayland.drv/wayland.c:93-246 的 17 个 global）+
winex11 侧的 WM 语义面；体验完整度按 wine 标准验收，不按 x11 自身。

## 1. 三形态与 incumbent（wayland 路线）现状

形态定义（docs/glossary.md、docs/decisions/0004）：

| 形态 | 呈现 | 窗口管理 | 默认形态 |
|---|---|---|---|
| 虚拟桌面 | 全部 Wine 窗口合成到一个全屏画面 | 合成器按坐标命中 | 平板/手机/开鸿 |
| PC 多窗口 | 每 Wine 窗口 = 独立 UIAbility 主窗口 | 系统窗口管理器 | PC / 2in1 |
| Pad 多窗口 | 每 Wine 窗口 = 主窗口下 subWindow | 系统子窗 + ArkTS 承载 | 平板可切换 |

### 1.1 原生数据流（incumbent，x11 侧对账的基准）

```
xdg get_toplevel → 分配 toplevelId (xdg_shell.cpp:340)
  → 首帧 commit 才发 created 事件 (wl_core.cpp:639-641; desktop 模式提前到建档时)
  → 事件总线 22 种事件 (toplevel/toplevel_event_bus.h:42-72)
  → NAPI TSFN 单通道 → ArkTS setToplevelCallback (WineWindowManager.ets:269)
  → ArkTS createRenderer(toplevelId, surfaceId) (WineWindow.ets:19)
  → 每 toplevel 一个 EglRenderer 绑 XComponent 的 NativeWindow
    (plugin_manager.cpp:54-71, map toplevelRenderers_)
  → 像素零拷贝按 surfaceKey=(pid<<32|surfaceId) 粒度
    (compositor_utils.cpp:4; egl_renderer.cpp:191/231)
```

### 1.2 ArkTS 承载两条路线（分叉只在界面层）

| | PC 多窗口 | Pad 多窗口 |
|---|---|---|
| 承载 | 每 toplevel `startAbility(WineWindowAbility)` 独立主窗口（WineWindowManager.ets:997-1001，want 带 toplevelId/title/w/h） | 主 stage 下 `createSubWindowWithOptions`（FusionWindowManager.ets:107-117，decorEnabled:false） |
| 回填 | registerAbility/Window/WindowStage 三步（WineWindowAbility.ets:38-77） | registerWindow + flushPendingEvents（WineWindowManager.ets:1051-1073） |
| 最小化/最大化 | 系统 `win.minimize()/maximize()` | subWindow 无系统能力：hide≈minimize 近似、全屏=resize 到屏尺寸（FusionWindowManager.ets:264-309） |
| 焦点/置顶 | 系统窗口系统 | 子窗点击 `raiseToAppTop` + 整组提升（FusionWindowManager.ets:260） |

原生侧 `DisplayPolicy desktop=false` 两路线完全相同；选择逻辑
`fusionUsesSubWindow()`（WineWindowManager.ets:221-229，tablet 默认 subWindow）。

### 1.3 窗口状态事件往返（双向）

- 原生→ArkTS：resize→`win.resize`（WineWindowManager.ets:826-836）、title→
  `setWindowTitle`、minimized/restored→`minimize()/showWindow()`、fullscreen→
  maximize(immersive)、modal→ModalWindowManager 建 isModal 子窗（:533-557）。
- ArkTS→原生：**窗口尺寸权威在 OHOS**——`onSurfaceChanged`→
  `notifyToplevelResize`→`XdgConfigureSend`（WineWindow.ets:39；Wine geo 回推被
  忽略，wl_core.cpp:643-646「OHOS 窗口管理器为权威」）；拖动 `win.startMoving()`；
  系统窗销毁→`sendToplevelClose→destroyToplevel`（WineWindowAbility.ets:187-215）。
- 输入：ArkTS 每窗事件自带目标 toplevelId 直接注入（sendPointerEvent/
  sendKeyEvent/sendScrollEvent），坐标=窗口局部经 letterbox 逆映射+钳制；
  合成器命中路径在多窗口模式整体关闭（`CompositorRoutesInput()=false`，
  display_policy.h:44-45）。

### 1.4 弹出层判据（无启发式，只看协议字段）

`IsInlineClientSurface(inputRegionEmpty, vpSrcW) = inputRegionEmpty && vpSrcW<=0`
（display_policy.h:84-93）：客户区子表面（空输入区+从不设 viewport 源）合进父窗帧、
输入穿透；其余（菜单、浮层）走伪 toplevel + 独立系统子窗 + 全局定位
（PopupWindowManager.ets:156-168）。判据出处是 winewayland.drv 协议行为
（wayland_surface.c:1184-1192/672-675），不变量见
docs/archive/SUBSURFACE_CLASSIFICATION_DESIGN.md。

### 1.5 模式位与 explorer 门禁

模式位唯一存于 `DisplayPolicy::desktop`（display_policy.h:34），ArkTS
`applyModePolicy()` 写入（WineWindowManager.ets:166-177）；**多窗口模式下
explorer desktop-shell 根本不 spawn**（wine_launch.cpp:587 门禁），root 识别被
`RootCompositing()` 短路（wl_core.cpp:718）——「桌面就绪」信号在多窗口路线由
首帧 created 事件替代。x11 侧现状例外：ArkTS `desktopRootId=0` + `openX11Canvas()`
（WineWindowManager.ets:202-213/1236-1241）。

### 1.6 incumbent 已知限制（x11 复用时会继承）

unset_minimized 协议缺失（合成器猜还原尺寸）、Z 序私有消息未加、Pad subWindow
承载近似（hide/minimize/标题 1300002）、2in1 主窗 ARGB 无法透视、无 owner 的
modal 被忽略、title JSON 不转义引号（toplevel_event_bus.h:179-181）、剪贴板/
读屏回灌跨路线硬缺口。逐条锚点见 win32-window-model-x11-wayland.md §结构性缺失。

## 2. x11 路线现状

### 2.1 链路与形态覆盖

```
winex11.drv → Xwayland (rootless, wlr_xwayland_create_with_server)
            → wlroots scene（全部 X client surface 合成进单一 scene graph）
            → 单一 headless output → 单 OH_NativeWindow 画布
            → DesktopWindow.ets（全屏画布页）
```

当前只实现了**虚拟桌面**形态。D2–D30 一轮修复后，交互面（点击/拖拽/滚轮/键盘/
菜单/缩放）在 smoke 13 前缀上 11/13（r20261007-072201；仅 keyboard 两项为已知
ime 缺口，见 §5）。

### 2.2 原生侧窗口语义现状（ohos_output.c / display_input.c）

| 语义 | 现状 | 证据 |
|---|---|---|
| X 顶层窗跟踪 | `g_clients` 链表（创建序，链尾最上）+ generation 身份 | ohos_output.c:110-129 |
| scene 挂载 | 每 xs 一个 `wlr_scene_surface`，等 associate | ohos_output.c:152-160 |
| Z 序 | 仅创建序（新创建压旧），无 raise/lower/activate | ohos_output.c:1103 |
| 命中/输入 | 合成器自裁：`topmost_at` 几何遍历 → wlr_seat 注入 | ohos_output.c:1157-1176 |
| 配置往返 | 仅 `request_configure` 直通 | ohos_output.c:1085-1090 |
| 最小化/全屏/焦点 | **无**——注释明示「withdrawn/最小化语义归 M2+ 窗口管理」 | ohos_output.c:1122 |
| guest Vulkan 帧 | 已 per-window（frame_node 挂 X 面之上） | ohos_output.c:113, 1189+ |

### 2.3 winex11 侧：多窗口形态的驱动前提已具备

winex11 的形态分叉点是 `is_virtual_desktop()`（winex11.drv/desktop.c:44，由
explorer `/desktop` 模式决定）：

- 虚拟桌面模式：忽略 WM 配置回传（window.c:1749），一切在 Wine 桌面窗口内合成；
  当前 x11 链跑的就是这个模式（explorer desktop-shell 由 ohos_output.c:205 识别）。
- **无虚拟桌面 = managed 模式**：每个 Win32 顶层窗直接映射 X 顶层窗
  （window.c:3187），走 ICCCM/EWMH 与 WM（= wlr_xwayland/xwm）交互——
  `_NET_WM_STATE`、WM_PROTOCOLS、configure 往返、瞬态窗 owner 提示。
  这是 wine 在桌面 Linux 的默认运行方式，**驱动侧零新增**。

结论：x11 多窗口形态的差距不在 wine/Xwayland 驱动层，全在**合成器 WM 语义 +
per-surface 呈现 + OHOS 窗口承载（产品层）**。

## 3. wlr_xwayland 已提供的 WM 语义面（0.20.2）

wlr_xwayland（xwm）本身实现了 X WM 协议侧（ ICCCM/EWMH 应答、CLIPBOARD 桥、
override-redirect 透传、窗口 hints 读取），把**管理决策**留给合成器：

| 需求（多窗口承载） | wlr_xwayland 提供 | 合成器侧待做 |
|---|---|---|
| 激活/焦点 | `wlr_xwayland_surface_activate`；`request_activate` 事件 | OHOS 窗口焦点 → activate 映射 |
| 几何配置 | `wlr_xwayland_surface_configure`；`request_configure`/`request_move`/`request_resize` 事件 | OHOS 窗口事件 → configure 回写 |
| 最小化 | `request_minimize` 事件 + `set_minimized` | OHOS 最小化 ↔ xs 状态映射 |
| 全屏 | `request_fullscreen` + `set_fullscreen` | 同上 |
| Z 序 | 无（WM 职责） | ArkTS/系统窗口序 → scene 节点序同步 |
| 弹出层 | override-redirect 窗口透传为 unmanaged xs | 命中与呈现归父窗还是独立子窗（对齐 §1 判据） |
| 装饰 | `_MOTIF_WM_HINTS` 读取 | Pad subWindow 无系统装饰 → Wine 自绘已是现状（winex11 client_side_graphics 默认） |

## 4. 差距矩阵与方案

### 4.1 差距矩阵（形态 × 层）

| 层 | 虚拟桌面 | PC 多窗口 | Pad 多窗口 |
|---|---|---|---|
| wine 驱动 | explorer `/desktop` + Wine 内自合成（**现成**） | managed 模式（不启 explorer），驱动侧**零新增**（§2.3） | 同左 |
| X/WM 语义 | 单画布 + `topmost_at` 命中（现成） | xwm 事件面激活：activate/minimize/fullscreen/configure 双向映射（§3 表右列，**全新**） | 同左 |
| 呈现 | 单 scene → 单画布（**现成**） | per-xs 渲染 → 独立 NativeWindow（方案 B，**全新**） | 同左 |
| 输入 | 合成器命中 + wlr_seat 注入（**现成**） | OHOS 窗口事件携带目标窗 → 映射到 wlr_seat 焦点/坐标（**全新**，但 display_input.c 本就是 wlr_seat 面） | 同左 + 触摸手势（D15 同款缺口） |
| ArkTS 承载 | DesktopWindow 画布页（**现成**） | WineWindowAbility/WineWindow 页**整链复用**，仅 created 事件源换成 x11 映射层 | FusionWindowManager **整链复用** |
| 模式门禁 | desktop-shell 识别 `evt:desktop-ready`（现成） | explorer 不 spawn（对齐 wine_launch.cpp:587 门禁）+ 就绪信号改首窗 created（**全新**） | 同左 |
| smoke 验收 | 13 前缀 11/13（ime 缺口除外） | C 型注入的 x11 多窗分支（按窗注入） | 同左 |

**结论**：三形态共享的底层（wine 驱动、wlr_seat 输入面、ArkTS 承载、事件总线、
smoke 设施）全部现成或高复用；全新工作集中在两块——**原生侧 per-xs 呈现 + 状态
映射层**（x11↔toplevel_event_bus 语义对齐）与**弹出层承载**。

### 4.2 per-surface 呈现方案对比

| 方案 | 做法 | 判定 |
|---|---|---|
| A：每窗一个 wlr_output | 每系统窗挂一个 headless output + scene_output | **否决**——output 是 wl_output global，X 侧镜像为 XRandR 显示器，N 窗 = N 屏会污染 wine 的显示器模型（EnumDisplayDevices/ChangeDisplaySettings 全部失真） |
| **B：per-xs 手动渲染** | scene 保留给虚拟桌面；多窗口模式对每个 xs 用 wlr renderer 把 surface texture 画进各自 buffer → 直推该窗 NativeWindow | **推荐**——X 侧单 output 不变（wine 显示器模型干净）；与 incumbent 的 per-toplevel EglRenderer 模型同构（plugin_manager map 结构直接对位）；已有先例：M2-T5 guest 帧就是绕开 scene 直挂 buffer |
| C：整画布 + ArkTS 裁剪 | 所有窗仍画进同一画布，每系统窗用 XComponent 视口裁剪 | **否决**——OHOS 窗口内容只能整窗呈现，视口裁剪要 ArkTS 侧逐窗换算坐标；Z 序/越界浮层/每窗独立缩放均无法表达 |

### 4.3 弹出层（override-redirect / transient）承载

x11 侧没有 xdg subsurface 可套 §1.4 判据；对应物是 X 窗口属性：

- `override_redirect == true`（菜单/tooltip，wine 自管）→ X 全局坐标定位。
- `transient` 有 owner（对话框类）→ 归属父窗，跟随其 Z 序与生命周期。

**推荐对齐 incumbent 的 popup 路线**（独立系统子窗 + 全局定位，
PopupWindowManager 事件面 popup_show/move/resize/hide 直接对位）：override-redirect
窗在 X 侧本就是独立顶层、全局定位，语义同构度最高；判据无启发式（只看
override_redirect / transient_for 两个 X 属性）。菜单越出父窗边界的场景只有
独立子窗能表达（incumbent 为同一理由放弃画布内合成）。

### 4.4 分阶段实施建议（拍板后进 spec）

| 阶段 | 内容 | 验收 |
|---|---|---|
| M4-0 | XTEST Unicode keysym 通道（§5 第 1 点，独立小项可先行） | keyboard 两项转 PASS |
| M4a | PC 多窗口最小闭环：per-xs 呈现 + created/resize/close 映射 + 按窗输入；单程序双窗 | smoke 新增 x11-fusion 套件，双窗内容/点击路由 PASS |
| M4b | 窗口状态面：minimize/fullscreen/activate/title/modal 双向映射 | 同上扩展 |
| M4c | 弹出层：override-redirect → 独立子窗 | 菜单用例（越界可点） |
| M4d | Pad 承载：FusionWindowManager 接 x11 事件源 + 手势（D15 同款） | Pad 真机手测 |

工作量重心：原生侧新增「x11 toplevel 映射层」（xs 状态 ↔ toplevel_event_bus
22 种事件语义，估 600-1000 行）+ ohos_output.c 多窗模式分支（per-xs 渲染循环，
估 400-600 行）；ArkTS 侧 WineWindowManager 增加 x11 created 事件源分支（其余
整链复用）。

## 5. keyboard / IME 缺口（归入本决策集）

现状两层：

1. **smoke 注入面**：`ime` 动作在 wayland 走合成器 text-input 提交
   （SmokeRunner.ets:565-572 → `testNapi.sendImeCommit`）；x11 分支无对应通道，
   keyboard 用例 2/13 FAIL（char-count 差 E5 一位）。**低成本修法**：XTEST 打
   Unicode keysym（0x01000000+UCS；fcitx5 对 Xwayland 无 XIM 时的同款 fallback，
   wine 侧 keysym→Unicode 折算现成）——smoke 面即可闭合，不等产品桥。
2. **产品面（真实 IME 打字）**：OHOS 输入法框架 → text-input-v3 → wlroots 后
   需要 X 侧通道把最终串送进 winex11。winex11 有完整 XIM 客户端（xim.c）但
   Xwayland 的 XIM server 生态支持差（win32-window-model 文档 §13）；候选：
   (a) XTEST fake keysym 直打（同 1，产品桥也可先走）；(b) 实现 XIM server
   （完整 preedit/候选语义，工作量大）；(c) D19 剪贴板桥承接（已建，仅粘贴语义）。
   决策点：先 (a) 后评 (b)，与 wayland 路线的 text-input 完整度对齐时再评。

## 6. 待用户拍板的决策点

1. **是否开工 x11 多窗口**（本盘点主体）：按 §4.4 分阶段（M4a→M4d），还是
   多窗口需求等 fusion 换引擎时一并解决（display/ 胶水届时直接服务
   winewayland，x11 侧不重复建 ArkTS 承载分支）？
   ——张力点：ArkTS 承载与事件总线是**两路线共享资产**，x11 侧建了 fusion
   换引擎直接复用；但 per-xs 呈现与状态映射层的验收要按 wine 标准走两遍。
2. **弹出层承载**：§4.3 推荐 override-redirect → 独立系统子窗（对齐 popup
   路线）；备选是先在父窗 buffer 内合成（实现更薄，但菜单越界会被裁剪）。
3. **ime 通道**：§5 第 1 点的 XTEST Unicode keysym 通道是否先行（不依赖 1 的
   拍板，独立小项，直接把 keyboard 两项转绿）。
4. **退役判据的时间表**：三形态对齐是 M3 退役的硬前提（记忆
   x11-retirement-three-mode-parity），但 wlroots-serve-winewayland 方向
   （记忆 wlroots-serves-winewayland-direction）下「退役」的紧迫度重估——
   若 fusion 换引擎先行，x11 三形态对账是否降级为「验证 wlroots 基座形态
   覆盖」的手段而非目的。

## 7. 相关已挂起项

- D15（手势层）：虚拟桌面形态内的触摸手势对齐；M4d Pad 承载复用同款结论。
- D23（guest GL 面）：虚拟桌面形态内的挂接问题，方向 (a) 子窗锚只服务虚拟桌面；
  多窗口形态下每窗 = 真 X 顶层，锚定表天然命中（记忆 x11-retirement-three-mode-parity）；
  per-xs 呈现（方案 B）下 guest 帧画进所属窗的 buffer。
- D27（inline 用例编排）：测试设施，与形态对账无关但验收面会用到。

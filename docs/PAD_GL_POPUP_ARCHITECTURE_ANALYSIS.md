# 多窗口模式（Pad 多窗口 + PC 融合）: GL 画面由独立 popup 子窗口承接 — 问题分析与解决方案

日期：2026-09-10
状态：**方案分析（未实施）**——含现状链路、关键事实、三方案对比、推荐路径与验证实验

**适用范围：Pad「多窗口模式」与 PC「融合模式」同源同病，方案对两平台同时生效。**
依据：C++ 侧共用同一分流——`DisplayPolicy::SubsurfaceAsLayer() = desktop`
（`entry/src/main/cpp/compositor/frame/display_policy.h:34-37`，注释即标
"PC 窗口模式"；`desktop=false` 的 PC 与 Pad 多窗口一样把 subsurface 转 popup
伪 toplevel），ArkTS 侧共用单例 `PopupWindowManager` / `pages/WinePopup`
同一套承载（popup 宿主 stage 按窗口来源取：PC = 该程序 UIAbility 的
WindowStage，Pad = Fusion 宿主 stage，同一入口
`WineWindowManager.ets:665-681`）。

## 1. 问题是什么

多窗口模式（Pad 多窗口 / PC 融合）下，一个带 GL 画面（游戏/D3D 程序）的
wine 窗口在 OHOS 上**占两个窗口**：

- **主窗**（Fusion subWindow，`wine_fusion_N`，加载 pages/WineWindow）：
  只显示"壳帧"——窗口背景 + Wine 自绘标题栏。白屏调查实测该窗 94.7% 纯白。
- **画面 popup**（子窗口 `wine_popup_N`，加载 pages/WinePopup）：
  客户区实际画面（GL/菜单/内容）在这里，独立 XComponent + 独立 EglRenderer。

这不是单一 bug，而是承载方式带来的一族问题：

| 问题 | 机制 |
|---|---|
| 白屏类（已修一例） | 两个独立 OHOS 窗口的 z 序由系统 WMS 管理：点击主窗获焦 → WMS 自动置顶主窗（白壳）→ 盖住画面 popup（focusable=false 抢不回）。2026-09-10 用 `raiseWindowGroup` 整组提升修掉一例，同类场景（多窗叠加/系统面板/拖拽中）仍可能再现 |
| 拖动跟随滞后 | 画面 popup 靠 `windowRectChange` 事件 + `repositionPopups`（全局坐标=父窗 rect + offset×scale）追赶主窗；popup 独立子窗口 IPC 有 ≤1 帧滞后，且需 2s 延时补定位兜底 |
| 资源开销 | 每窗口 2 个 OHOS 子窗口、2 个 XComponent、2 个 EGLContext/renderer、1 条跨进程 IPC 事件流（popup_show/move/resize/hide） |
| 全屏特例 | 全屏游戏由"popup 尺寸==父窗 Wine 尺寸"判定后拉伸铺满屏幕、主窗只做"最大化保证无系统 UI 露出"——两条窗口路径要各自维护 |
| 输入/命中 | 输入全在主窗（popup 不可聚焦），坐标常量/命中判定要把"画面在主窗里的位置"与"popup 全局位置"两套坐标对齐 |

**两平台差异（不影响问题同构性）**：

| | PC 融合模式 | Pad 多窗口 |
|---|---|---|
| 主窗形态 | 每窗口一个 UIAbility 主窗口 | 主界面 stage 下的 Fusion subWindow |
| popup 宿主 | 该程序 UIAbility 的 WindowStage | 主界面 stage（FusionWindowManager 提供） |
| 窗口拖动 | 系统自由窗口（系统装饰/标题栏） | 主窗触摸 → `startMoving`（wine xdg move） |
| 共同点 | popup 不可聚焦（`setWindowFocusable(false)`）、画面独立窗口、z 序交系统 WMS | 同左 |

即：平台差异只在 popup 的 stage 来源与窗口装饰方式；**白屏/跟随滞后/双窗开销/
全屏特例的表象与机制两平台一致**（白屏案例的 z 序机制在 PC 上同样成立：
点击 focusable 主窗 → WMS 置顶白壳 → 盖住不可聚焦的画面 popup）。

而 **desktop（虚拟桌面）模式没有这些**——它的客户区 subsurface 直接合成进
root 帧（单 renderer 单窗口），层序由 C++ 合成器掌控。

## 2. 现状链路（为什么画面在 popup 里）

三层根因，从下往上：

**① wine 侧天性：客户区不是主 surface，是一条 wl_subsurface。**
winewayland.drv 把窗口客户区内容建成父 toplevel 的 wl_subsurface 并 desync
（`thirdparty/wine/dlls/winewayland.drv/window.c:209-244`、
`wayland_surface.c:1207-1260` `wayland_client_surface_attach`）。GL 画面
（virgl readback 或 zero-copy）就提交在这条子表面上。**主 surface 帧里没有客户区像素**。

**② 宿主分流策略：多窗口模式不合成 subsurface，改为"伪 toplevel"。**
`entry/src/main/cpp/compositor/wl_core.cpp:743-774` `UpdateSubsurfaceOnCommit`：

- desktop 模式（`Policy().SubsurfaceAsLayer()` 为真）→ 存 layer，在
  `TakeToplevelFrame` 里合成进 root 帧；
- 多窗口模式 → **所有 subsurface 无差别**交 `PopupManager::UpdatePopupOnCommit`
  登记成"伪 toplevel"（从 toplevel id 取号器拿号、复用 ToplevelState 存帧、
  映射 surface 资源），向 ArkTS 发 `popup_show/resize/move` 事件
  （`compositor/toplevel/popup_manager.cpp:93-104,159-163`）。

原注释记了动机（wl_core.cpp:839-843）：**"不再 blit 进父 buffer（会被窗口
边缘裁剪）—— 参考 weston/wlroots: subsurface 可越出父 surface 边界，
compositor 不做父边界裁剪"**。即当时的选择是"宁可多一个窗口，也不要裁剪"。

**③ ArkTS 承载：每个伪 toplevel 建一个独立 OHOS 子窗口。**
`entry/src/main/ets/service/PopupWindowManager.ets`（类头注释 33-38）为每个
popup 建 `createSubWindowWithOptions(...decorEnabled:false, isModal:false)`
→ `setWindowFocusable(false)` → `loadContent('pages/WinePopup')`；页面的
XComponent `onSurfaceCreated → createRenderer(popupId, surfaceId)`
（`pages/WinePopup.ets:16-24`）挂独立 EglRenderer，渲染该子表面帧。
主窗同理渲染"壳"（`pages/WineWindow.ets:15-23` createRenderer(toplevelId)）。
`plugin_manager.h:58-59`：renderer 表按 toplevel 一个，popup 伪 toplevel 也是
一份，所以"一个 wine 窗口 = 2 个 renderer"。

**代价由系统 WMS 接管**：两窗 z 序、跟随、命中全部变成"两个独立系统窗口的
协同问题"——白屏文档把这条记为"后续正解=画面合入主窗合成"。

## 3. 关键事实（决定方案可行性）

以下均经代码核实（2026-09-10）：

**事实 1：zero-copy 不依赖 popup 承载方式。**
ZC 管线是 virgl 子进程直接把画面渲染进 OHOS SurfaceQueue（NativeWindow），
宿主 renderer 用 `OH_NativeImage` 绑成 external OES 纹理采样上屏
（`graphics/egl_renderer.cpp:204-231`、`graphics/graphics_broker.cpp:507-523`、
`graphics/virgl_child.cpp:206-231`），与 subsurface 是否登记 popup **正交**：
- `PopupManager` 代码零 ZC 引用；
- desktop 模式已证明 ZC 与"subsurface 合成进主帧"共存——ZC 层由
  `ShouldSkipCpu()=!visible||zcActive` 跳过 CPU 合成（`compositor/toplevel/desktop_compositor.h:84`），
  再由 root renderer 以 overlay 画（`egl_renderer.cpp:911-1003`）；
- PC 模式 ZC 几何取法只依赖 SurfaceData 父子链
  （`compositor/frame/zc_bridge.cpp:237-251`，条件 `rendererToplevelId == parentToplevel`）。

**事实 2：窗口内合成器已就绪，当前"恒空"只因 subsurface 被转走。**
`compositor/frame/frame_composer.cpp:46-94` `WindowFrameComposer`：基底 =
窗口 SHM 帧，遍历 `BuildWindowLayerListLocked`（`desktop_compositor.cpp:302-383`）
对每个非 ZC 的 Subsurface 层 `BlitWindowSubsurface`（CPU 1:1 blit，
`frame_pipeline.cpp:845-860`）。层列表构造里明确写着"**层序结构为窗口内内容
扩展预留；若未来窗口内 layer 化，按协议顺序 zIndex 递增**"，且已包含窗口内
ZC 层置顶逻辑（`desktop_compositor.cpp:338-381`，覆盖 toplevel 整窗与
subsurface 局部两种形态）。即：**"合入主窗"在 C++ 侧是让 subsurface 走回
已存在的代码路径，不是新写一套合成器。**

**事实 3：多窗口下合入的几何是自洽的。**
窗口 SHM 帧是 wine 逻辑尺寸，renderer 负责整帧 scale/letterbox 上屏
（`egl_renderer.cpp:738-781, 902-908`）。subsurface 的 `localX/localY/w/h`
是窗口局部的逻辑坐标（`desktop_compositor.cpp:318-336` 直接用）——blit 进
逻辑尺寸窗口帧后，随整帧统一缩放，**不需要逐层缩放**。desktop 模式能工作
也是同一套坐标语义。

**事实 4：现状下 ZC 画面在哪个 renderer 上，需要一次日志验证。**
`zc_bridge.cpp:237` PC 分支要求 `rendererToplevelId == parentToplevel`：
合入后主窗 renderer（=窗口 id）天然满足；而现状的 popup renderer
（=popupId）不满足。现状 ZC 是否仍有画面（还是静默回退到 SHM 回读路径），
决定了 ZC 相关的收益/风险盘子。列为验证实验 E1（见 §6）。

## 4. 方案对比

### 方案 A（推荐）：客户区合入主窗合成，只有"越界浮层"保留 popup

把 subsurface 分流从"按模式"改为"**按类别**"：

- **内嵌客户区**（wine 的 `reconfigure_client` 语义：客户区内容，几何恒在
  窗口内容矩形内）→ 走 layer 合成进窗口帧（desktop 同款
  `BlitWindowSubsurface`；ZC 层走 renderer overlay）。窗口 = 一个 OHOS 窗口、
  一个 renderer，拖动/缩放/置顶/全屏全部回到"单窗口"语义——白屏、跟随滞后、
  补定位**方案性消失**。
- **越界浮层**（wine 的 `reconfigure_subsurface` 矩形差语义：菜单/下拉/
  异型 tooltip，可能超出窗口边界）→ 保留 popup 伪 toplevel 通道。

两侧可由 wine 建模语义区分（白屏文档已论证"判定精确化不需要阈值"），
`DisplayPolicy` 是现成的策略单点（`compositor/frame/display_policy.h:14-16,38-57`）。

**改动面**（预估）：
- C++：`SubsurfaceAsLayer()` 换判据（或新增按类别的策略）+ `UpdateSubsurfaceOnCommit`
  分流条件；`WindowFrameComposer`/`BuildWindowLayerListLocked` 基本不用动；
  ZC 几何在 PC 分支的条件核对。**PC 侧的合成路径（WindowFrameComposer）
  本就在跑**（当前输出=窗口 SHM 帧，层列表恒空），改造 = 让内嵌 subsurface
  不再转 popup 并进层列表，改动面比 Pad 侧更小。
- ArkTS：`PopupWindowManager` 只处理浮层类 popup（内嵌类不再建窗）；
  `WineWindow` 主窗 XComponent 尺寸/输入不变；全屏简化（PC/Pad 都不再需要
  popup 全屏拉伸特例，主窗 fullscreen 承载客户区）。
- 兼容：desktop 模式不动；`winehua.fusionHost` 调试键可做开关灰度。

**风险**：① SHM（非 ZC）路径每帧多一次窗口内 CPU blit + 整窗上传（尺寸小，
量级参照：desktop 全屏 blit 曾 40-78ms/帧，是 640x480→1080p 全屏量级；
窗口内 1:1 blit 是 memcpy/alpha 级，需实测确认）；② ZC 全屏映射需验证
（现状 `zeroCopyFullscreen_` 仅 RootCompositing 成立，`egl_renderer.cpp:369`）；
③ 菜单类浮层判定要与 wine 语义对齐（做错=菜单被裁剪，回退容易）。

**验证成本**：中。改动集中、有 desktop 模式作参照实现、可分支灰度。

### 方案 B（权宜）：保留 popup，但让系统层级受控

不合并窗口，改为消解 z 序问题的根源：主窗放弃 `focusable(true)`（改由
其他方式支持 startMoving/拖拽）或改用 OHOS 提供的子窗口层级/zLevel 能力
（此前调研 `setWindowZLevel`）显式钉住"画面恒在主窗之上"。

- 优点：改动最小（纯 ArkTS），立即缓解白屏类问题。
- 缺点：跟随滞后、双 renderer/双窗口开销、全屏特例**全部保留**；z 序仍依赖
  "每次系统状态变化都要重新钉一遍"的脆性维护（焦点/分屏/系统面板都会扰动）。
- 结论：只能作为过渡，不是终态。

### 方案 C（更远）：全模式统一合成

把 desktop 的 root 合成与多窗口的窗口内合成统一为一套"按 toplevel 输出帧"
的模型（`COMPOSITOR_UNIFICATION` 方向），窗口内不再区分"壳帧/画面"。
本质是 A 的上层统一化，建议在 A 落地、语义稳定后再做抽象收敛。

## 5. 推荐路径

**A 为主、B 仅作灰度保底**。理由：

1. 合入的合成器、层列表、ZC overlay 全部已存在且被 desktop 模式验证；
   多窗口模式"恒空"只是分流判据的历史选择（当时理由是裁剪，而"内嵌类不会
   越界"这个前提在 wine 模型里成立）。
2. 收益是结构性的：白屏/跟随/全屏/资源四类问题一起消失，而不是逐个打补丁
   （raiseWindowGroup 已是一次补丁，同类还会有）。
3. 遗留的 popup 通道缩到"越界浮层"这一个明确职责，代码量和维护面都缩小。

**分阶段**：
1. 验证实验 E1-E3（见下），拿到 ZC 归属与合成原型数据；
2. C++ 分流改造 + `winehua.fusionHost` 灰度开关（Pad 上内嵌类走合成）；
3. 回归矩阵（**PC 融合 + Pad 多窗口双平台**）：GL 游戏（ZC 路径）/D3D12
   （vkd3d，无 ZC 值）/菜单弹出/拖动+缩放+全屏/多窗口叠加 z 序/desktop 模式回归；
4. ArkTS 收口（内嵌类不建窗、全屏特例删除）与文档更新。

## 6. 前提验证实验（实施前做，成本低）

| # | 实验 | 方法 | 判定 |
|---|---|---|---|
| E1 | 现状 ZC 画面归属 | Pad 多窗口跑 GL 程序，抓 `[VIRGL-ZC]`/`[MW-ZC]`/renderer 侧日志，对照 `createRenderer(popupId/toplevelId)` 归属 | ZC 由哪个 renderer 消费；主窗 renderer 是否也在跑 ZC overlay |
| E2 | 合成原型（不开 ZC） | 临时加调试开关让 `SubsurfaceAsLayer()` 恒 true（同时覆盖 PC 窗口模式与 Pad 多窗口），跑普通 SHM 窗口 | 客户区是否出现在主窗帧、位置/尺寸是否正确、拖动是否随动（**PC+Pad 各跑一遍**；PC 侧合成路径现成，可先跑） |
| E3 | ZC 场景验证 | E2 基础上跑 GL/D3D 程序 + 全屏 | ZC overlay 在主窗 renderer 的映射/全屏是否正确；帧率对比 popup 方案（**PC+Pad 各跑**） |

E2 是"1 行判据 + 观察"级别的最小实验，能最快否定/确认整个方案。

## 7. 未决项（实施时确认）

- 窗口内 SHM blit 的实测帧率影响（尤其 CPU 合成回退期）；
- ZC 全屏（游戏 exclusive）在主窗 renderer 的映射路径（`zeroCopyFullscreen_`
  的 RootCompositing 前提是否要放宽）；
- 菜单类浮层保留 popup 后的边界场景（菜单压在窗口外、菜单跨两个窗口）；
- `BlitWindowSubsurface` 对 ARGB（圆角/半透明层）的 alpha 语义已有
  （`compositor_blit.cpp:102-144`），需实例验证。

## 附：关键文件

| 文件 | 角色 |
|---|---|
| `entry/src/main/cpp/compositor/wl_core.cpp:743-843` | subsurface commit 分流 + 历史动机注释 |
| `entry/src/main/cpp/compositor/frame/display_policy.h:14-57` | desktop/managed 策略单点 |
| `entry/src/main/cpp/compositor/toplevel/popup_manager.cpp` | 伪 toplevel 登记与事件描述 |
| `entry/src/main/cpp/compositor/frame/frame_composer.cpp:46-94` | WindowFrameComposer（就绪、当前恒空） |
| `entry/src/main/cpp/compositor/toplevel/desktop_compositor.cpp:302-383` | 窗口层列表 + ZC 置顶（为内容层化预留） |
| `entry/src/main/cpp/graphics/egl_renderer.cpp:204-231, 911-1003` | ZC external texture / overlay 绘制 |
| `entry/src/main/ets/service/PopupWindowManager.ets` | popup 子窗口创建/定位/提升 |
| `entry/src/main/ets/pages/WinePopup.ets` / `WineWindow.ets` | 两页面的 XComponent/renderer 绑定 |
| `docs/PAD_POPUP_WHITE_SCREEN_DRAG_INVESTIGATION.md` | 白屏案例 + "后续方向" |
| `docs/COMPOSITOR_UNIFICATION.md:196` | "窗口内 subsurface 当前恒空"声明 |
| `thirdparty/wine/dlls/winewayland.drv/window.c:209-244` | wine 客户区=subsurface 的源头 |

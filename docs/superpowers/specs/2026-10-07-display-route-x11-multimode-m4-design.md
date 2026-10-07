# M4 设计：x11 路线多窗口形态（PC + Pad）

> 状态：设计提案（待用户 review；批准后进实施计划）
> 日期：2026-10-07
> 决策依据：用户 2026-10-07 拍板——开工 M4a、弹出层走独立系统子窗、维持三形态硬前提。
> 盘点依据：[2026-10-07-display-route-x11-three-mode-gap-analysis.md](2026-10-07-display-route-x11-three-mode-gap-analysis.md)（差距矩阵、方案对比、incumbent 机制全链证据，本文不重复引用行号时以该文 §1-§4 为准）。
> 验收纪律：验收判据先于实现确定（§7）；每阶段有独立可验证的通过判据。

## 1. 目标与范围

x11 路线（winex11 + Xwayland + wlroots）补齐 PC 多窗口与 Pad 多窗口两个产品形态，
使自研窗口栈退役的三形态硬前提可以逐项对账。

**范围内**：原生侧 per-xs 呈现与 WM 状态映射、ArkTS 承载接入（PC UIAbility /
Pad subWindow 整链复用）、弹出层（override-redirect）独立子窗、smoke 验收面。
**范围外**：XIM server（独立 spec）、手势层（D15 已实现，真机手测项）、
dxvk 全档位覆盖（另行按套件扩展）。

**冻结行为**：虚拟桌面形态（现有 13 前缀验收面）不得回退——每阶段提交后
跑 `displayroute-win32-interactive` 前缀组确认。

## 2. 目标架构

```
现状（虚拟桌面）:
  xs 顶层窗 → wlr_scene_surface（全部挂单 scene）→ 单 headless output
            → 单 OH_NativeWindow → DesktopWindow.ets 全屏画布

目标（多窗口, M4a 起）:
  xs 顶层窗 → x11 toplevel 映射层（新, display/x11_toplevel.c）
            → toplevel_event_bus（复用 wayland 路线 22 事件语义）
            → ArkTS 承载（WineWindowAbility / FusionWindowManager, 整链复用）
            → createRenderer(toplevelId, surfaceId) → NativeWindow 回绑
            → per-xs 渲染循环（wlr renderer 直画 surface texture → 各自 buffer）

  虚拟桌面路径保留不动（模式位分叉, §3.4）。
```

**关键裁决（已在盘点定案）**：
- per-surface 呈现走**手动渲染**（wlr renderer 把 surface texture 画进各窗
  buffer），不走每窗一个 wlr_output（X 侧多屏污染 wine 显示器模型）。
- Xwayland 维持 rootless 单 output；wine 侧走 managed 模式（不启 explorer
  desktop），驱动侧零新增。
- 弹出层 override-redirect → **独立系统子窗**（对齐 PopupWindowManager 全局定位）。

## 3. 组件设计

### 3.1 x11 toplevel 映射层（新文件 display/x11_toplevel.c/.h）

xs 生命周期事件 ↔ toplevel_event_bus 语义的翻译器。职责边界：**只做映射，
不做策略**（策略在 ArkTS 承载层，与 wayland 路线同构）。

| xs 事件/状态 | 产出事件 | 时机 |
|---|---|---|
| associate + 首帧 buffer 存在 | `created`（带 contentW/H） | 首帧对齐 wayland 的 created 语义（wl_core.cpp:639 同款延后） |
| xs->title 变化 | `title` | events.set_title |
| request_configure / OHOS resize 回写 | `resize` | 双向：xs 侧请求上送；OHOS 尺寸权威回写见 §3.3 |
| request_minimize / request_fullscreen / request_activate | `minimized` / `fullscreen` / `activate` | xwm 请求事件直译 |
| destroy / dissociate | `destroyed` | 清理映射记录 |
| parent 变化（transient） | `modal`（带 owner） | M4b：parent 非空且 owner 在册 |

**toplevelId 分配**：复用 wayland 路线的 id 空间与 TSFN 通道（两条路线显示上
互斥——DisplayRouteService 单选，会话内不会同时产生两类 id，无碰撞面）。
surfaceKey = `(clientPid << 32) | xs window_id`（对齐 wayland 的
`(pid<<32)|surfaceId` 位宽语义，x11 侧 window_id 天然唯一）。

**渲染循环**（映射层内，帧时钟复用 ohos_output 的 VSync 驱动）：
每 tick 遍历活跃 toplevel → `wlr_renderer_begin_buffer_pass`(该窗 buffer)
（wlroots 0.20 render pass 模型，pass.h:57）→ `wlr_render_pass_add_texture`
画 `wlr_surface_get_texture(xs->surface)`（wlr_compositor.h:363；texture 由
本渲染循环按 buffer 指纹经 `wlr_texture_from_buffer` 按需创建并缓存——
scene/cursor 同款用法，wlr_scene.c:1201）→ submit → NativeWindow 直推（复用
ohos_output 的路径 C Lock/Flush 纪律）。**源码验证（2026-10-07）**：texture
不随 commit 自动创建、由消费方在自己 renderer 上按需建是 wlroots 全树一致
模式，per-xs 独立渲染无 scene 依赖，方案成立。**帧跳过**：surface buffer
未变（提交序号 same）且窗口未 resize → 跳帧。guest Vulkan 帧（M2-T5 的
frame_node 机制）：多窗模式下改画进所属窗的 buffer（同 pass 内先 surface
texture 后 guest 帧叠加，几何用 D23 的子窗/顶层锚定查询）。

### 3.2 ohos_output.c 多窗模式分支

- `new_surface` 挂载点分叉：多窗模式不建 `wlr_scene_surface`（scene 空转），
  交给映射层；虚拟桌面模式维持现状。
- 命中/取数口（`topmost_at`/`client_xs`）：多窗模式不再用于输入命中（§3.3），
  但保留给 smoke 注入编排（C 型注入在 x11-fusion 套件的按窗注入改造用）。
- 模式来源：chain_start 入参扩一位（`multiwindow`），ArkTS 侧由
  DesktopModeService + DisplayRouteService 联合决定（route=x11 且 mode=fusion）。

### 3.3 输入路由（display_input.c 分支）

多窗模式关闭合成器命中路径（对齐 wayland 的 `CompositorRoutesInput()=false`）：

- **指针**：ArkTS 事件自带目标 toplevelId → 映射层查 xs → 坐标（OHOS 窗口
  局部）经 `xs->x/y` 反推 X 全局 → `wlr_seat_pointer_notify_enter/motion`。
  enter/activate 跟随目标窗（`wlr_xwayland_surface_activate`）。
- **键盘**：ArkTS onKeyEvent(tl) → seat 焦点先切目标窗（keyboard enter）→
  notify_key。焦点纪律沿用现有 FocusClient（activate 先行 + settle 防抖）。
- **滚轮**：同现有 wl_pointer 轴注入（#93 的 value120 语义不变），目标 =
  指针焦点窗。
- **触摸手势**：ArkTS 承载层手势状态机（DesktopWindow.ets 已有）不适用于
  系统子窗承载——PC/Pad 承载复用 WineWindow.ets 的手势面（wayland 多窗已
  走通），x11 侧只接它下发的事件，不重复实现。

### 3.4 模式门禁与生命周期

- **explorer 不 spawn**：x11 多窗模式下 wine_launch 的 desktop-shell 分支
  条件化（对齐 wayland 的 `IsDesktopMode()` 门禁——x11 侧新增等价查询口，
  从 display_route 控制面读，不复制模式位）。
- **就绪信号**：虚拟桌面用 `evt:desktop-ready`（desktop-shell 映射）；多窗
  模式改**首窗 created**（对齐 wayland 多窗语义）。
- **ArkTS**：`openX11Canvas()` 仅虚拟桌面分支调用；多窗分支走
  WineWindowManager 的 created 事件流（现有 x11 分支 `desktopRootId=0` 的
  特判改为「多窗时不注册画布 root」）。
- 会话重置：映射层状态（id 表/渲染循环）随 Wine 会话重置
  （对齐 ohos_output 的 `g_desktop_shell_mapped=0` 纪律）。

### 3.5 弹出层（M4c）

- 判据（无启发式，只看 X 属性）：`override_redirect == true` → popup 路
  线；transient 且 owner 在册 → 归属父窗（M4b 的 modal/owned 承载，跟随
  父窗生命周期）。
- popup 事件面：popup_show/move/resize/hide（PopupWindowManager 整链复用，
  全局定位 = X 全局坐标直传）。
- 呈现：popup xs 不进映射层的 toplevel 表，单独渲染循环小帧（或合流为
  「无 id 渲染目标」，M4c 设计时定）。
- 输入：popup 子窗的触摸/点击由 OHOS 子窗直送（对齐 wayland popup），合成
  器侧只需在 popup 打开期间把 seat 焦点让给 popup xs。

## 4. 数据流（M4a 最小闭环视角）

```
[guest] wine 程序起两窗 (smoke 程序自建)
  → Xwayland rootless → xs×2 (associate, buffer)
  → [映射层] created×2 (id=1,2; surfaceKey 按 window_id)
  → [TSFN] ArkTS setToplevelCallback
  → [WineWindowManager] PC: startAbility×2 / Pad: createSubWindow×2
  → [WineWindow] onSurfaceCreated → createRenderer(tl, surfaceId)
  → [plugin_manager] NativeWindow(surfaceId) → 映射层回绑
  → [渲染循环] 每 VSync: 画 xs texture → 推对应 NativeWindow
  → 屏幕两窗独立呈现
[输入] 触点窗 A → OHOS 焦点 → ArkTS sendPointerEvent(tl_A) → seat enter(A)+motion
  → Xwayland → wine 收 ButtonPress 于窗 A
```

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| 渲染首帧早于 NativeWindow 回绑 | 映射层持帧跳过未回绑窗（计 skip 计数，hilog 采样）；回绑后下一 tick 追上 |
| xs texture 读取生命周期（dissociate 竞态） | 渲染循环内 has_content 谓词 + destroy 监听摘除（复用 ohos_client_surface 纪律） |
| wayland 事件通道状态残留（route 切换会话） | 映射层与 wayland server 的 TSFN sink 为同一注册点，按会话重置对齐；smoke 套件显式钉 route |
| Pad subWindow 承载近似（hide/minimize 无系统能力） | 继承 incumbent 已知限制（FusionWindowManager 现状），不新增债；注记进验收说明 |
| 虚拟桌面回退 | 每阶段跑 displayroute-win32-interactive 前缀组（含 e2e/input 全集），FAIL 即停 |
| xwm 子窗死区复发（D23 补丁面） | guest 帧锚定继续走 D23 查询口；M4 不动 wlroots 补丁 |

## 6. 里程碑

| 阶段 | 内容 | 通过判据 |
|---|---|---|
| M4a | 映射层 + per-xs 呈现 + created/resize/destroyed + 按窗输入（PC 最小闭环） | smoke 新增 `x11-fusion` 套件：双窗程序（新 smoke 程序，各窗自绘标记块）双窗内容判定 PASS + 点窗 A 不落窗 B（rgba-quadrants 判定器按窗裁剪） |
| M4b | 窗口状态面：title/minimize/fullscreen/activate/modal 双向映射 | 同套件扩展：最小化还原后内容判定、全屏切换判定、模态属主置顶判定 |
| M4c | 弹出层：override-redirect → popup 独立子窗 | 菜单用例：越界菜单项可点（popup 全局定位判定） |
| M4d | Pad 承载（FusionWindowManager 接 x11 事件源） | Pad 真机手测清单（承载/拖动/手势/多窗并发） |

每阶段独立提交；M4a 通过前不开 M4b（前向依赖：b/c/d 全部消费 a 的映射层接口）。

## 7. 验收判据汇总（先于实现）

1. **M4a 判定器**：新 smoke 程序 `fusion_probe`（双窗，窗 A 纯色+窗 B 四象限
   标记），host 判定 per-window 帧内容（按窗 rgba-quadrants）；点击路由判定 =
   点击窗 A 后窗 A 侧计数 +1 且窗 B 不变。
2. **虚拟桌面零回退**：displayroute-win32-interactive 13 前缀维持 11/13
   （keyboard 两项为已知 ime 缺口，XIM 立项后转绿）。
3. **D22 判据纪律**：判定只读归档数据，设备端只跑不判；x11-fusion 套件
   显式声明 displayRoute=x11 + multiwindow 档位。
4. **产品不为测试让步**：fusion_probe 进 smoke/programs，产品路径不引用。

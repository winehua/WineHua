# GAP2 多 app 重叠输入错投 —— 治本 mini-plan（任务 #131）

档案: `docs/engineering/display-route-known-issues.md` §2.15。设备 192.168.1.5:33363。
BASE = 0ed9b81。证据目录 `.temp/gap2/`、SDD ledger
`.superpowers/sdd/2026-10-11-x11-gap2-overlap-input/progress.md`。

## 1. 复现（修复前实录，真机 2026-10-11 15:27-15:31）

编排: x11+fusion 冷启 notepad A（tl=13, X 窗 0xe00003）→ onNewWant 起 notepad B
（tl=17, X 窗 0x1200003）；两窗 created 同为 guest(4,30) 866x621（承载窗 rect 完全
重合 (8,60) 1732x1282）；WINEHUA_WINEDEBUG=+event 全量 trace（PID 段头归因）。

**数据点①（指针错投）**: uitest 点 B 承载窗 (400,300)。
- ArkTS/native: `TAP->LEFTCLICK tl=17` + `inject button evdev=0x110`（目标正确）。
- wine trace: `ButtonPress/ButtonRelease for hwnd/window 0x10060/e00003`（**A 的 X 窗**）。
- 事后: A 自激活（wine 点击激活）→ `_NET_ACTIVE_WINDOW` → 我方 activated → ArkTS
  raise A（hidumper: A ZOrd 104 > B 103）→ 之后键盘也随 A。
证据: `.temp/gap2/wine_trace_after_click1.log`（15:29:59 段）。

**数据点②（键盘错落，独立分离变量）**: 拖走 A（strip 拖动）→ 点 B 顶条（只
raiseWindowGroup，零 guest 注入）→ uitest keyEvent 2017。
- 目标: `KEY tl=17` + `keyboard enter xs=...(activated)`（FocusClient 执行）。
- wine trace: `warn:event:handle_wm_protocols Ignoring window 0x1008e/1200003
  WM_TAKE_FOCUS serial 165, event_time 0, foreground 0x10060` → `KeyPress for
  hwnd/window 0x10060/e00003`（**A**）。
证据: `.temp/gap2/wine_trace_after_key2.log`（15:31:27 段）。

## 2. 根因（机制级，源码实读 + 实测互证）

**指针（主因）**: 按窗注入只决定「局部坐标 + 目标 surface」，X 侧最终投递由
**X 服务端 hit-test（X 窗树/堆叠）** 决定：
- xwayland-input.c `dispatch_absolute_motion`: root 点 = `focus_window->toplevel->
  drawable` + local，`QueuePointerEvents(POINTER_ABSOLUTE|POINTER_SCREEN)` 进 DIX。
- 我方 root 点按构造必在目标 xs 的 X rect 内，**但** X 堆叠里目标不在顶时，
  hit-test 命中同 rect 的另一窗（x11 fusion 下两 app 的 X 窗都钉在创建位，
  rect 恒重叠）。
- X 堆叠为何不同步: wine 跨进程不 restack（win32 z-order 归 wineserver，X 侧
  由 WM 负责——xwm(wlroots) 把堆叠留给合成器 scene，本栈 fusion 不渲染 scene、
  无人 restack）；且 xwm `map_notify` 对新窗 `restack(BELOW)`（xwm.c:1602），
  后起的 B 反而垫底。实测互证: B 自激活（wine foreground）后 X 顶仍是 A。
- ⇒ **宿主→X 堆叠回写通道缺失**（D37 补丁只有 wine→宿主方向）。

**键盘（独立第二因）**: KeyPress 走 DIX input focus；xwm activate 对 wine
（ICCCM globally-active: WM_HINTS.input=False + WM_TAKE_FOCUS, winex11
event.c/x11drv_main.c `use_take_focus=TRUE` 默认）只发 WM_TAKE_FOCUS，且
`message_data.data32[1] = CurrentTime = 0`（xwm_focus_window）; wine 的接受规则
（event.c:738）: `hwnd != foreground && !window_should_take_focus(foreground, 0)`
→ 忽略；`window_should_take_focus`（window.c:2146）= `!focus_time ||
time > focus_time`，focus_time = 前台窗 WM_STATE 属性事件时间（非 0）
⇒ **time=0 的 WM_TAKE_FOCUS 对非前台目标恒被忽略** —— 合成器驱动焦点切换
结构性死路（实测数据点②的 wine WARN 原文）。
- 点击路径之所以能聚焦: 点击落对窗（修好指针后）→ wine 自置前台 →
  `_NET_ACTIVE_WINDOW` → xwm WM_TAKE_FOCUS 回声时 `hwnd == foreground` 通过 →
  `set_focus` → XSetInputFocus（wine 自己的接受路径）。
- wine 接受路径的等价效果（attention: `set_focus` 在有 EWMH 时**也只**
  XSetInputFocus，不动 win32 前台）见 event.c:635。

**几何回写评估（任务要求）**: host→wine configure 回写（§2.14 缺口）**不能**
修重叠区投递——重叠点仍由 X 堆叠裁决，两个 rect 都含该点。它改善的是
hover/hit 的坐标保真（非重叠区 + 拖动后视觉一致），属另一独立缺口。
⇒ 本任务机制 = 堆叠/焦点同步（非次优解，是重叠路由的必要条件）。

## 3. 修复设计（两处单点，全部落我方层：mapper + display_input）

**F1 指针/轴: 注入前 X 堆叠回写（click-to-raise 语义）**
- 点: `x11_toplevel_input_pointer_dispatch` / `..._axis_dispatch`（mm_resolve_local
  成功后、multimode 调用前）→ `wlr_xwayland_surface_restack(xs, NULL,
  XCB_STACK_MODE_ABOVE)`。
- 不变式: root 点按构造 ∈ 目标 X rect ∧ 目标在 managed 栈顶 ⇒ hit-test 必中目标。
- 语义: 「指针交互目标 = X 顶」= 桌面 click-to-raise 惯例；motion/axis 同款
  （OS 已按承载窗 z 序投递指针事件，restack 令 X 命中与 OS 命中一致）。
- 守卫: `!override_redirect`（wlroots 断言）+ `xs->surface`（withdrawn 时
  stack_link 已摘，restack 会破坏一致性）；popup 分支不碰。
- xwm_schedule_flush 在 socket 可写时同步 xcb_flush（实读 xwm.c:3097）——
  restack 先于后续 wayland 输入写入，Xwayland 侧通常先行执行。

**F2 键盘: X 输入焦点随注入目标同步**
- 点: `display_input.c` `FocusClient`（短路口**之前**，每次调用都同步 ——
  wine 自置前台会引起 X 焦点漂移，键注入前必须重断言）。
- 实现: `xcb_set_input_focus(xwm_conn, XCB_INPUT_FOCUS_POINTER_ROOT,
  xs->window_id, XCB_CURRENT_TIME)` + flush；xwm_conn 经既有补丁 API
  `wlr_xwayland_get_xwm_connection`（scripts/patches/wlroots-ohos-xwm-child-
  geometry.patch 已提供，零新补丁）；`wlr_xwayland` 句柄在
  `wl_ohos_input_xwayland_set_seat` 已有入参，同点暂存。
- 依据: 与 wine 自己接受 WM_TAKE_FOCUS 后的动作等价（set_focus →
  XSetInputFocus，event.c:635——有 EWMH 时同样不动 win32 前台）；
  被绕过的只是 `time=0` 的接受门（xwm 发不出真时间戳，上游只读）。
- 守卫: `x11_toplevel_active()`（multimode 门：虚拟桌面/wayland 零行为变化）+
  `!override_redirect` + `xs->surface`。
- wine 侧确认链: DIX 焦点 = 目标 → KeyPress 落目标 X 窗 → wineserver 按
  `msg->win` 线程路由（queue.c find_hardware_message_window）→ 目标应用内部
  焦点窗（编辑控件）。settle 40ms 已覆盖焦点先行的竞态（FOCUS_SETTLE_MS）。

**不做**: thirdparty（xserver/wlroots/wine）零改动；无新 NAPI；无调用点特判。

## 4. 验收判据（真机）

1. 重叠场景（A/B 两 notepad 或 notepad+wordpad）:
   - D1: 点 A 承载窗 → wine trace ButtonPress 落 A 的 X 窗（修复前落 B/反向同款）。
   - D2: 点 B 承载窗 → 落 B；**拖动后**（承载窗移位、X 钉位）再点仍正确。
   - D3: 点 B 后打字 → `keyboard enter` + KeyPress 落 B 的 X 窗 + 字符现于 B
     （截图）。
   - D4: 点 A 后打字 → 落 A（往返切换）。
   - D5: 纯键盘切换（点 B 顶条 → 打字）→ KeyPress 落 B（F2 覆盖）。
   - D6: 滚轮/双指滚动（axis）落目标窗。
2. 回归: fusion 3/3（基线）+ core 3/4（opengl-x86 = D49 已知）；前缀组不劣于基线。
3. wayland 对照轮（共享文件 display_input.c 改动）：wayland 冷启 + 点击/键注入
   行为与改动前一致（K2/F1 均 gated，列出清单）。

## 5. 风险与已核实点

- restack 与 wine 自 restack 的互搏: wine 只在激活/z-order 变化时 restack
  自我新状态，与「谁被点击谁在顶」一致，不互搏（实测复核）。
- 注入事件与 restack 的到达顺序: X 连接（xwm）与 wayland 连接（输入）不同
  通道；xcb_flush 同步发出先行；若观察到「首次点击仍错、后续对」即此序问题，
  备选：restack 后加 xcb_get_input_focus 往返同步（代价一次 round trip/事件）。
- restack 后 wine 客户端收 ConfigureNotify（stack 变化）: wine 忽略纯堆叠
  变化对自身 rect 的记账（D37 补丁处理反向同理），实测无副作用。
- K2 后 win32 前台与新 X 焦点分叉（A 前台 / X 焦点 B）: wine FocusOut 有 EWMH
  时不重置前台（event.c:895 门）；点击/激活链在下次真实点击时收敛（wine
  自置前台）；测试 D3/D4 实证。
- 被 restack 目标若已 withdrawn（surface NULL）→ 跳过（守卫）。

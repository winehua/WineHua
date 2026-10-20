# host→wine 几何回写 —— 治本 mini-plan（任务 #135）

档案: `docs/engineering/display-route-known-issues.md` §2.14 条「OHOS 拖拽 resize 在
x11 路线不回写 wine（M4a 模型缺口）」+ §2.15 表「边缘拖拽 resize」行。设备
192.168.1.5:33363。BASE = 2be1ef8。证据目录 `.temp/geom/`、SDD ledger
`.superpowers/sdd/2026-10-11-x11-geom-writeback/progress.md`。

## 1. 复现（修复前实录，真机 2026-10-11 17:20-17:24）

编排: `build/geom-r1-inline.json`（job: x11+fusion 冷启直启 fusion_probe 240s）。
试剂: 探针扩展 `geomA` = 窗 A 的 win32 事实（GetWindowRect + WM_SIZE/WM_MOVE 计数，
随 2s 心跳写 result JSON）。基线读数: geomA {100,100,640x480, sizes=0, moves=0}。

**症状① edge 拉伸不回写**: uitest drag 窗 A 右缘 (1466,700)→(1766,700)（+302 物理
= +151 逻辑，系统 enableDrag 拉伸成立 —— 截图窗 A 渲染拉伸变宽且盖住窗 B）。
- 探针 win32 事实原地不动: geomA 仍 {100,100,640x480, sizes=0, moves=0}
  （`.temp/geom/r1-result-01.json`）。wine 既无 WM_SIZE 也无几何变化。
- hilog 链（`.temp/geom/` 归档摘要）: `[Ctrl] onSurfaceChanged … surfaceWidth
  1516→1564`（拖拽全程 ~30 拍）→ 每拍 `notifyToplevelResize(755..782, 446,
  resize=no)` → `[NAPI] notifyToplevelResize` 75 条；`[MW] NotifyToplevelResize`
  **0 条** —— native 函数据 xdg resource 查询处静默早退（wayland_server.cpp:469
  `if (!tl) return;`，位于日志点之前）。**即：ArkTS 每拍都送到了，native 全部丢弃。**

**症状② 拖动承载窗后位置陈旧**: uitest drag 窗 A 顶条 (840,278)→(1040,478)
（+200,+200 物理 = +100 逻辑，startMoving 系统拖拽）。截图窗 A 整体位移；探针
geomA 仍 {x:100, y:100, moves:0}（`.temp/geom/r1-result-02.json`）。位置通道在
host→wine 方向**根本不存在**（全仓无位置类 NAPI；wine 的 X 窗永久钉在创建位）。

两项症状同根: **host→wine 几何回写半边缺失**（尺寸半边被 xdg 查询挡死、位置半边
无通道），与 §2.14 定档一致（方向吻合，不再另立假设）。

## 2. 根因（机制级）

- x11 fusion 的承载窗几何权威在 ArkTS（系统拉伸/系统拖拽都直接改承载窗），
  guest 几何权威在 wine 的 X 窗（xs->x/y/width/height）。两侧之间只有
  wine→host 单向（`handle_request_configure` → ResizeRenderer/承载窗），**反向
  通道结构性缺失**。
- `NotifyToplevelResize` 是唯一的 host→guest 尺寸 NAPI，但实现只面向 xdg
  （`FindToplevelResource` + `xdg_toplevel` configure）；x11 的 toplevel 没有 xdg
  resource → 早退。wayland 的 RESIZING/最小化哨兵语义（xdg 特有）在 x11 不适用。
- 修法（档案已定档）: x11 分支改用 X 协议原语 `wlr_xwayland_surface_configure`
  —— 即 M4a「wine→host」方向已用的同一函数（handle_request_configure 的应答），
  语义 = WM 主导 configure（xcb_configure_window + 尺寸不变时合成 ConfigureNotify）。

## 3. 修复设计（全部落我方层：x11_toplevel.c / display_input.c / wayland_server.cpp /
napi / ArkTS；thirdparty 与 wine 源码零改动）

**D1 尺寸半边（host→wine）**: `NotifyToplevelResize` 入口加 x11 分支
（gated: `x11_toplevel_active() && 目标 id 在 x11 登记表`——后者用新导出
`x11_toplevel_owns(id)` 反问，防路由旗标与具体 id 空间错配误劫 wayland toplevel）：
napi/JS 线程只入队（与 pointer/key/close 同纪律），loop 线程 dispatch:
- 查 entry（createdPosted 门 + 非 popup + 非 dead）；
- 目标 = (xs->x, xs->y, w, h)（尺寸半边保留 xs 当前位置）；
- **与 xs 现值全等 → 跳过**（无操作跳过 = 第一道防回声门：host 侧 resize() 回落、
  wine 采纳回声等场景下不发无意义 configure）；
- `xs->minimized` 时跳过（镜像 xdg 侧最小化门禁；x11 缓解面 = 图标化期间不动几何）;
- 执行 `wlr_xwayland_surface_configure`，并把本次写入记入 entry 的 last-writeback
  槽（供 echo 抑制）。

**D2 位置半边（新增通道）**: 新 NAPI `notifyToplevelMove(tl, x, y)`（guest 逻辑
坐标 = 承载窗全局原点 / scale，契约见 §5-C）。native 侧同队列入队，dispatch:
- 目标 = (x, y, xs->width, xs->height)（位置半边保留 xs 当前尺寸），其余门同 D1。

**D3 ArkTS 上报点（判据单点收敛）**:
- FWM subWindow 路径: `WineWindowManager.startFusionSubWindow` 的
  `subWin.on('windowRectChange')`（既有 popup 重排挂点）追加位置上报；
- WWA 路径: `WineWindowAbility` 的 `mainWindow.on('windowRectChange')` 同款追加；
- 上报值源 = `win.getWindowProperties().windowRect`（与 PopupWindowManager
  .positionEntry 同源，全链唯一坐标来源），gx = round(left/s), gy = round(top/s)；
- ArkTS 侧 `DisplayRouteService.isX11()` 门（wayland 连 NAPI 都不进——隔离红线
  加倍；native 侧 x11_toplevel_active 仍是权威门）。尺寸半边复用既有
  onSurfaceChanged → notifyToplevelResize（已含 strip 补偿：XComponent surface
  尺寸 = guest 尺寸，无需新计算）。

**D4 echo 抑制（handle_request_configure 补半）**: wine 收到 WM 主导 configure
后**不主动回发 ConfigureRequest**（源码实读见 §5-A），所以回环面只剩「app 主动
回推」与「wine 回声」两类，均不会震荡；仍按任务要求做抑制：请求 (x,y,w,h) 与
entry 的 last-writeback 槽在 ε=2px 内相等 → 仍发协议应答
（`wlr_xwayland_surface_configure`，T9 纪律不变），但**不上送 resize 事件**
（承载窗已在目标位，避免自回声驱动承载窗作无谓 resize）。ε 的实测依据: wine 的
win32⇄X 装饰折算在本栈无 WM 帧下是常数偏移（D52 实锤），请求值与回写值偏差超 2px
即 app 真实改几何，必须放行。

**D5 探针几何自证（测试资产）**: `smoke/programs/win/fusion_probe.c` 增 geomA
（已随 §1 复现轮落地, 作为本任务第 1 个 commit 独立提交）。

## 4. 验收判据（真机, 全部读归档/设备产物）

1. **resize 重排**: edge 拉伸（同 R1 编排同坐标）→ 探针 geomA.w/h 跟随（640→≈791
   形态 = X 782 + 装饰 8/34 折算）、sizes 计数增；截图内容为**重排**后 GDI 绘制
   （十字臂比例 = 新窗比例，非拉伸）。
2. **拖动跟随**: strip 拖动 +Δ → geomA.x/y 跟随（±2px 内，取整误差）。
3. **稳定性轮**: 连续 ≥4 次移动+resize 往返 → geomA 收敛于最终几何、无持续心跳
   增长（无 ping-pong；`handle_request_configure` 日志无自回声 resize 上送）。
4. **wine 侧log**: 一轮 `WINEHUA_WINEDEBUG=+x11drv` 中 `config`/`requesting
   config` 行与回写时序对应（wine 采纳 WM configure，无回写风暴）。
5. **回归**: fusion 3/3、core 3/4（opengl-x86 = D49 已知）；**wayland 对照轮**
   （共享文件 display_input.c / napi_init.cpp / wayland_server.cpp / ArkTS 改动）:
   wayland fusion 会话跑 probe → 回写标记日志 0 行、geomA 行为与修复前相同
   （全零 sizes/moves, 两窗不被动过）。

## 5. 风险与已核实点（Self-Review）

**A. 回环分析（wine 侧源码实读, thirdparty/wine 只读不改）**
- `X11DRV_ConfigureNotify`（winex11.drv/event.c:1096）→ `window_configure_notify`
  （window.c:1993）→ `handle_state_change`（window.c:1867）: serial 无匹配 pending
  时走「unexpected/mismatch」分支把 desired+pending+current 一并置为收到值 →
  `window_request_desired_state` → `window_set_config`（window.c:1467）EqualRect
  短路 → **不发 XReconfigureWMWindow**。⇒ wine 采纳 WM 主导 configure 不会回发
  ConfigureRequest。
- wine 发 ConfigureRequest 仅当 win32 侧主动改几何（X11DRV_SetWindowPos →
  window_set_config → XReconfigureWMWindow, window.c:1514）→ 走既有
  handle_request_configure（回写值吻合时由 D4 抑制、不吻合时 = app 真实意图，
  放行上送 → 收敛于 app 尺寸后自然静止）。
- host 侧: 回写只在承载窗几何事件驱动下发生；wine 采纳不改承载窗 ⇒ 无自发链。
  尺寸/位置半边上报拍频 = 承载窗事件拍频（与现网 popup 重排同级），队列 64 深 +
  no-op 跳过保证不积压。

**B. 坐标契约（含 strip）**: guest X 坐标 → 展示位置不变式 = `host = guest×s +
(0,strip)`（PopupWindowManager.positionEntry 佐证: 全局 = 父 windowRect + strip +
off×s），⇒ 承载窗原点/s 恒等于 X 窗口坐标（strip 在等式两侧抵消；创建摆放
FWM.ensure `pos = gx×scale` 同式）。位置上报 = windowRect 原点 / s 即满足契约；
strip 不进任何换算。

**C. 隔离红线（wayland 零变化）**: ① native 分支门 = `x11_toplevel_active() &&
x11_toplevel_owns(id)`，wayland 走原路径逐字节不变；② 新 NAPI 在 wayland 下
ArkTS 侧就不调用（isX11 门）；③ 对照轮实测（判据 §4.5）。共享文件改动清单:
`display_input.c`（队列两 item 类型 + drain 两支）、`wayland_server.cpp`
（NotifyToplevelResize 头部分支）、`napi_init.cpp`（新 NAPI 注册）、ArkTS
（windowRectChange 两处追加）。`x11_toplevel.c/h` 为 x11 专属文件。

**D. 最小化门禁**: xs->minimized 跳过回写（xdg 侧哨兵语义在 x11 不适用，但图标化
期间动几何无收益且可能扰动 wine restore 路径；镜像纪律、单点实现）。
恢复后的几何跟随由后续承载窗事件自然补齐（FWM.show → surface 事件链）。

**E. 已排除的次优方案**: 不在 ArkTS 里做「拖拽结束才回写」的节流（连续回写 =
原生 X WM 行为，wine GDI 应用逐拍 WM_SIZE 无压力；节流会引入拖拽结束瞬间跳变）；
不引入新事件类型（承载窗位置走独立 NAPI 而非扩 22 事件红线）。

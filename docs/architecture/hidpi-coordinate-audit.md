# HiDPI 坐标链审计（2026-11-08，进行中）

## 0. 背景

HiDPI 化改造（commit 待提交）把呈现模型从「wine 96 DPI 逻辑分辨率出帧 + 宿主 ×2 上采样」
切换为「wine 192 DPI 物理分辨率原生出帧 + 呈现 1:1」。改动点：

- `WineWindowManager.DESKTOP_PRESENT_SCALE` 2.0 → 1.0（ArkTS，呈现倍率单点）
- `WINE_CONTENT_SCALE = 2.0` → `WINEHUA_LOGPIXELS = 192` env → `win32u/sysparams.c`
  覆盖注册表 LogPixels（wine DPI 单点）

新坐标模型的核心事实：

| 量 | 旧模型 | 新模型 |
|---|---|---|
| wine px 与物理 px | 1:2（wine 逻辑 px，呈现 ×2） | **1:1** |
| wine DPI | 96 | **192** |
| wine UI 元素尺寸（菜单栏/字形/GetSystemMetrics） | 基准值 | **基准值 ×2**（wine px 数值上等于旧物理 px） |
| wine 看到的屏幕尺寸 | 物理宽高（不变） | 物理宽高（不变） |
| OHOS vp | px / 1.6125（不变） | 同左 |

真机已验证：文本渲染清晰（模糊根因 = 96 DPI 小字形 ×2 双线性放大）。
真机已暴露的失效：x11 弹窗位置偏上约一个菜单行；wayland 独立窗口尺寸减半、
菜单栏点击失效。定性：**各链路「wine px = 96 DPI 逻辑值」的隐含前提集体失效**，
需逐链审计而非散点修补。

## 1. 审计范围与方法

4 路并行代码梳理（x11 ArkTS / x11 C++ / wayland 全栈 / wine 侧 DPI 语义），
每个换算点记录：位置（文件:行）、公式、旧前提、新模型下是否成立、失效症状。
汇总后按「确认失效 / 前提恰好转正 / 需真机验证」三档归组。

（待填充：代理返回后的逐链清单）

## 1a. x11 C++ 侧审计结论（已返回）

**总判：无 ×2/÷2 残留，比例式换算全部自适应，1:1 下自洽。**弹窗上报值（父相对
wine px 差，x11_toplevel.c:443-444/486-503）正确——弹窗偏移实锤在 ArkTS 定位层
（PopupWindowManager，见 1b）。

自适应成立的换算单点：
- `display_input.c:1079-1080`（x11_toplevel mm_resolve_local）：
  `lx = px * xs->width / win_w`——ratio 随两端尺寸自适应，1:1 时精确无损
- `display_input.c:360-474` motion 注入：归一化 0..1 × canvas 物理 px（eglQuerySurface
  实时值）；letterbox 逆映射门按数学恒等判定（scale==1.0 && off==0 跳过），1:1 精确跳过
- `egl_renderer.cpp` letterbox/ComputeFrameDisplayRect：dst==src 恒等
- geom_writeback（x11_toplevel.c:1189-1237）：ArkTS 值（=windowRect，物理）直写 X 坐标，
  值域 3120×2080 远在 int16 域内

确认失效/风险点：

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| C1 | display_input.c:450-457 | 相对指针增量单位变为物理 px，dinput 相对模式灵敏度 ×2 | 需要旧手感则 ÷ WINE_CONTENT_SCALE，或重标 InputDeviceMapper 校准；先真机验证游戏再定 |
| C2 | wine_launch.cpp:560-561 | explorer /desktop fallback 1280×720：setOutputSize 晚到时桌面 1280×720@192DPI 被 ×2.44 上采样（UI 巨大且模糊）。实际时序中 OutputWidth 就绪是常态，fallback 极少走到；触发时有 WARN 日志 | 待办：读 display 尺寸需 native display API，拒绝/推迟启动改时序风险大于收益 |
| C3 | napi_init.cpp:1255-1275 | FindToplevelAt 纯 wayland 语义，x11 无分支误调返回垃圾 id（当前无 x11 调用方，潜伏） | 加 x11_toplevel_active() 早退门 |
| C4 | napi_init.cpp:180-181 / x11_toplevel.c:1015,1056 | 陈旧注释（旧模型描述） | 更新注释 |

（C1 待游戏真机验证后决定；C3 已修；C2 为低概率防御项，留待办不随批修）

## 1b. x11 ArkTS 侧审计结论（已返回）

**总判：wine↔宿主的全部 ×/÷ effectiveScale 换算（resize/limits/created 摆放/
reportCarrierMove/onSurfaceChanged/popup 尺寸/输入注入）在新模型下成立**；
唯一真实坐标失效族 = **"vp × scale" 单位混算**——`getWindowDecorHeight()` 返回 vp，
代码 ×scale 换算。旧模型旧设备 densityPixels 恰=2.0，`vp×scale(2.0)` 侥幸等于
vp→px；新模型 scale=1.0 ≠ vpr=1.6125，全部欠补偿。

结构性事实：`getEffectiveScale() ≡ DESKTOP_PRESENT_SCALE`（与设备密度无关），
它是 "wine px ↔ 物理 px" 换算单点，**不是** vp→px 换算——两者在新模型下首次
分离（旧模型数值巧合相等）。

确认失效点（按症状归组）：

| # | 位置 | 错误机理 | 症状 | 修法 |
|---|---|---|---|---|
| A1 | PopupWindowManager.ets:347 | decorPx = `getWindowDecorHeight()`(vp) × scale(1.0) = 40px，真值 40vp×1.6125≈64.5px；欠 24.5px，相对旧模型上移 40px | **x11 PC 弹出菜单偏上一菜单行（真机实锤）** | decorPx 改 vp→px（×display.densityPixels），勿 ×scale |
| A2 | ModalWindowManager.ets:87/90（heightCompPx）、415/418（computePosition）、351/447/467-469（frame 求和） | 同族 vp×scale 混算；frame 矮 24.5px | x11 PC 模态对话框偏上 + 底部内容裁切 ~24.5px | 统一 vp2px 口径 |
| A3 | FusionWindowManager.ets:85/106（STRIP_LOGICAL_PX=28）、WineWindow.ets:104/111（DRAG_STRIP=20） | 按 ×2 呈现标定的观感常数 ×1 后物理减半；顶条 56→28px，按钮压扁/热区减半；拖动判定区 20px < wine 新菜单栏 40px 盖不全 | 顶条观感/热区减半（x11 fusion 子窗、modal 顶条） | 常数改 vp 标定或 ×WINE_CONTENT_SCALE |
| A4 | WineWindowManager.ets:760-778、538-549（surface 校准） | contentVPH(vp) 与 lastResizeTargetH(px) 相减的单位错位；新模型过 <200 门污染 decor 字段（已无真实消费者） | 潜伏：日志噪音 + 多余重 resize | 退役该段或改物理口径 |

伴随观察（非 HiDPI 回归，顺手修）：WineWindowManager.ets:1620 读 desktopAbilityCtx
（应为 desktopWinCtx）→ PC 桌面内容 resize no-op；WWA 系统标题栏吃 frame ~64.5px。

成立无需改动的关键面：全部 ×/÷effectiveScale 换算（MW resize/limits/created
摆放/reportCarrierMove/onSurfaceChanged 回传/popup 尺寸）、x11 归一化输入、
fusion/popup 1:1 尺寸、Pad strip 几何、fullscreenPopup 判定。

## 1c. wayland 路线审计结论（已返回）

**根因（wine 侧一行语义）**：`winewayland.drv/window.c:183`
`conf->scale = NtUserGetSystemDpiForProcess(0) / 96.0`——LogPixels=192 后 scale=2.0。
wine 对合成器：发坐标/尺寸 ÷2（`coords_from_window`，wayland_surface.c:1083-1101）、
收 configure/指针 ×2（`coords_to_window`，window.c:609/pointer.c:143）。表现：
`wp_viewport.set_destination = win÷2`（真机 `dst=81x19` 来源）、subsurface position ÷2、
xdg window_geometry w/h ÷2（desktop 模式 x/y 却是物理——混合尺度）；shm buffer
仍是物理 px（唯一可信尺寸源）。

**旧服务 13 个失效点全是该语义的下游**：toplevel 帧被 window_geometry 裁成 1/4
象限（shm_frame_source.cpp:77-79）、整帧 ÷2 降采样（wl_core.cpp:600→CopyToplevelContent）、
created/resize 事件带半尺寸（→ 独立窗口减半）、popup vpDst clamp + off 逻辑当物理
（→ 盖菜单栏）、InlineClient 物理层 1:1 blit 进逻辑尺寸帧（frame_pipeline.cpp:857-885，
→ 菜单栏点击失效根源：注入逻辑坐标 wine ×2、可见内容物理 1:1，差 2×）、configure 发
物理被 ×2（→ 窗口 2× 超屏）、全屏漂移误判 → ReassertFullscreen 风暴、max_size 启发式
死亡、ZC 层几何半尺寸、desktop 模式 x/y 与 w/h 尺度分裂、min/max 限位减半。

**根治方案（采用）**：`__OHOS__` 下钉死 `conf->scale = 1.0`。wine 的 wayland 坐标
空间回归物理 px 1:1；DPI 192 只留在 win32u 内部驱动字体/UI 度量（WINEHUA_LOGPIXELS
本意）。一行改动使旧服务 13 个失效点恢复前提。备选（不采用）：旧服务按"呈现用 src、
注入/configure 用 dst"做 13 处双向改造——popup offset 与 window_geometry 混合尺度
无法在 compositor 侧单边修干净。

## 1d. wine 侧 DPI 语义结论（已返回，与 1c 交叉一致）

- 驱动拿到的窗口 rect 永远是 raw 物理像素（win32u window.c:2137 统一 virt→raw），
  与应用 DPI awareness 无关
- x11 侧完全无缩放：X 像素 = wine px = 物理 px；**x11 菜单弹窗是 override-redirect，
  X root 绝对坐标即最终物理位置，宿主不得再锚定/平移**（WineHua 加的
  `_NET_WM_FRAME_EXTENTS` client 锚定是嫌疑点）
- wayland 菜单弹窗不是 xdg_popup，是 owner 的 `wl_subsurface`，位置 logical 空间
  相对 owner surface 原点
- 量级指纹：SM_CYMENU@192DPI = 37px ≡ "偏上一行"观察值；弹窗 y=37 = 菜单栏底边，
  wine 侧自洽
- DPI-unaware 应用：buffer 本身 96-DPI 小图（viewport src=dst=logical），合成器拉 2 倍
  ——此类应用的内容模糊是固有行为，与本次改造无关
- 附带发现（非本批）：desktop 模式 window_geometry x/y 与 w/h 口径混用
  （wayland_surface.c:606-616，wine 侧既有问题）；WineHua min/max logical 上报口径

## 2. 坐标消费方总表

见 1a/1b/1c/1d 各节清单（审计底稿在代理输出，关键单点已入档）。
坐标模型判据基准：

- x11：X 像素 = wine px = 物理 px，全程 1:1，宿主唯一允许的换算 =
  getEffectiveScale(=1 恒等)；vp→px 一律 × display.densityPixels(1.6125)，
  **禁止用 ×effectiveScale 代替**
- wayland（修 W1 后）：surface-local = buffer = 物理 px，viewport 恒等；
  wine 坐标 == 物理 px；与 x11 同模型

## 3. 确认失效点与修复设计

| # | 修复 | 位置 | 说明 |
|---|---|---|---|
| W1 | wine wayland scale 钉死 1.0 | winewayland.drv/window.c:183（__OHOS__） | 根治 wayland 13 失效点 |
| A1 | popup decorPx vp→px | PopupWindowManager.ets:347 | ×densityPixels 替代 ×scale |
| A2 | modal 同族 vp→px | ModalWindowManager.ets:87/90/415/418/351/447/467-469 | 对话框偏上+底部裁切 |
| A3 | 顶条/拖动区常数 vp 标定 | FusionWindowManager.ets:85/106、WineWindow.ets:104/111 | 观感/热区回归修复 |
| C2 | desktop fallback 陷阱 | wine_launch.cpp:560-561 | 待办（低概率，触发有 WARN） |
| C3 | FindToplevelAt x11 门 | napi_init.cpp:1255-1275 | x11_toplevel_active() 早退 |
| C4 | 陈旧注释 | napi_init.cpp:180、x11_toplevel.c:1015/1056 | 文档漂移 |

验证矩阵（改完逐项真机）：x11 弹窗位置 / x11 模态对话框 / 顶条观感与拖动 /
wayland 独立窗口尺寸 / wayland 菜单栏点击 / wayland 弹窗位置 / 拖窗 / 游戏全屏
（含相对指针灵敏度 C1 观察项）/ 输入法弹层。

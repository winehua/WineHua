# DPI 尺寸协议体系审计（2026-11-09）

HiDPI 化（呈现 1:1 + LOGPIXELS 192）第二阶段审计。第一阶段
`hidpi-coordinate-audit.md` 覆盖窗口/桌面坐标换算；本篇覆盖系统性暴露的
应用内容层、图形栈与窗口接管时序。定位是问题清单与修复计划，逐项收口后
在本文件标记状态。

## 1. 目标模型（终态）

- wine 像素 = 物理像素，全模式 1:1 呈现；entry 侧一切缩放机制退化为恒等
- DPI 语义单点 = `WINEHUA_LOGPIXELS`（现 192 = 96 × WINE_CONTENT_SCALE），
  win32u `get_logpixels_override()` 单点驱动窗口度量与字形渲染
- 坐标系单点 = 物理屏幕（SM_CXSCREEN 类判据与呈现坐标一致）
- 宿主保留职责：标题栏（ArkUI 自绘，物理常数）、子窗管理、剪贴板桥

## 2. 尺寸/DPI 持有者实测值

| 层 | 持有者 | x11 融合 | x11 虚拟桌面 | wayland |
|---|---|---|---|---|
| 系统 | display / densityPixels | 3120×2080 / 1.61 | 同 | 同 |
| ArkTS | setOutputSize / setWineLogPixels | 3120×2080 / 192 | 同 | 同 |
| 合成 output | x11: display 链 outW/H；wayland: session outputW/H | 3120×2080 | **1560×1040 + viewport 2×** | 3120×2080 |
| wine 屏幕 | SM_CXSCREEN / monitor | 3120×2080（探针 win32 套件=virtual 跑出 1560×1040，属虚拟桌面值） | 1560×1040 | 未单独实测（output 已物理，推断一致） |
| wine 度量 | LOGPIXELS | 192 | 192 | 192 |
| 呈现 | 子窗/viewport | 1:1 | 2× 放大 | 1:1 |

结论：**融合与 wayland 已物理 1:1；虚拟桌面保留 viewport 2× 架构是仅存的
双标准**（已拍板统一 1:1，见 §5-D）。监视器 DPI 与 system_dpi 同源后，
wayland/x11 两条路线均为 192（§5-F）。2026-11-14 x11 虚拟桌面实测
（smoke 链）：X 桌面窗 3120×2080@0,0、输出帧 3120×2080、画布（XComponent）
3120×2020，present 按 fit 缩到 3030×2020（offX=45）；上表 x11 虚拟桌面
一列是 2026-11-09 的产品链值，待按当前链复测。

## 3. 窗口接管链（x11 融合）与竞态点

正常时序：
```
wine CreateWindow → X 窗口(初始尺寸) → CreatedAt 报宿主 → 宿主建子窗(同尺寸)
→ 宿主布局 resize 子窗 → windowSizeChange → notifyToplevelResize
→ WaylandServer::NotifyToplevelResize
  → x11 分叉门: x11_toplevel_active() && x11_toplevel_owns(id)
  → x11_toplevel_geometry_size → 队列 → geom_writeback
  → wlr_xwayland_surface_configure → X 窗口 resize → winex11 ConfigureNotify
  → window_configure_notify → WM_SIZE → 应用跟随
```

竞态点（实锤或高度可疑）：

| # | 竞态点 | 后果 | 状态 |
|---|---|---|---|
| R1 | `WaylandServer::NotifyToplevelResize` 的 x11 门含 `x11_toplevel_owns(id)`，**owns=false 时静默落入 wayland 路径丢弃**；move 半边 (`NotifyToplevelMove`) 无 owns 检查——门不对称 | geom-size 零下发，X 窗口/wine rect 停在初始尺寸；宿主子窗继续变 → 呈现缩放适配 → 图标/文字/内容观感缩小、resize 内容不变 | 断点实锤（geom-size 零条 vs geom-move 刷屏） |
| R2 | owns=false 的成因：x11 登记表条目生命周期与 wine 窗口生命周期错位（路线切换清理、表重建时序） | 间歇性：重启 wine 后表重建 → 正常；多开/切路线后 → 异常 | 待专项 |
| R3 | `geom_writeback` 对 `!createdPosted` 的窗口直接丢弃 resize，无补发 | 同 R1 形态 | 待专项 |
| R4 | 接管失败的窗口（无宿主子窗）走 wine 自画 NC 兜底 | explorer 出现 wine 标题栏装饰 | 实测形态 C |

用户可见形态 → 竞态映射：

| 形态 | 观察表现 | 对应 |
|---|---|---|
| A 接管正常 | 宿主标题栏 + 内容铺满 + 图标 48 | 无竞态命中 |
| B 接管但尺寸错位 | 宿主标题栏 + 内容缩左上(~81%) + 图标观感 ~24 + resize 内容不变 | R1/R2/R3 |
| C 未接管 | 无宿主标题栏，wine NC 兜底呈现 | R4 |

## 4. 应用内容层（wine builtin 程序 DPI 适配）

| 项 | 缺陷 | 状态 |
|---|---|---|
| notepad 默认编辑字体 | 上游 `main.c:199` 硬编码 `-12`（96 口径），同函数 iPointSize 路径有 DPI 换算先例；两路线字小 | **已修**（对齐同函数换算写法，96 DPI 等价） |
| explorer nav_toolbar | 走 comctl32 标准位图（W2 覆盖），图标观感小为呈现缩放（§3-B），非位图缺陷 | 随 §3-B |
| 无 manifest 进程的 DPI 声明 | 上游无声明进程留在 UNAWARE，窗口消息按 192→96 虚化；全屏游戏自设显示模式（如 800×600）后，点击落点只有一半并钳在左上 1/4（war3 实测） | **已修**：user32 `SYSPARAMS_Init` 对无 manifest 进程默认 `SetProcessDpiAwarenessContext(SYSTEM_AWARE)`，`WINEHUA_DPI_AWARE=0` 关闭；PE 模块走该分支依赖 configure.ac 给 PE 目标补 `-D__OHOS__`（mingw 不预定义，缺了就是死代码） |
| 显式 UNAWARE 应用呈现拉伸 | 应用 manifest 显式声明 UNAWARE 时，Windows 语义要求呈现侧拉伸回物理；内容按虚拟化尺寸渲染 | GDI：窗口 buffer 即物理尺寸，自动正确；GL：§5-C（已修）；Vulkan swapchain：走同一 viewport destination 通道（低概率，随回归观察） |
| 第三方程序自绘 | 程序自身责任（与 Windows 行为一致） | 不修 |

## 5. 修复计划（分批）

- **A. 已收口**：notepad 字体；hilog `%{public}` 脱敏；display_input.c 剪贴板
  EOF 泄漏（协议来源 send 后 close 宿主写端）
- **B. x11 接管链时序**（当前批）：owns 门对称化 + 失败发声（不得静默吞，
  也不得去掉 owns 的双隔离防误劫语义）；R2 条目生命周期专项；R3 补发队列
- **C. wayland GL 尺寸跟随**（已收口 2026-11-14）：DPI-unaware 窗口的
  GL 内容按可见尺寸（物理一半）渲染——Windows 语义要求呈现侧拉伸回
  物理客户区（DWM 行为）。GL 帧缓冲 = 应用可见尺寸（正确）；断点在
  `opengl_readback.c` SHM 提交把 client subsurface 的 wp_viewport
  destination 覆写为应用可见尺寸，踩掉 `wayland_surface_reconfigure_client`
  写入的物理 destination——合成层按 destination 记录 ZC 层几何 → 960×540
  帧以原尺寸贴进 1920×1080 槽 → 缩左上。修复 = 移除覆写，destination 单一
  所有权归 reconfigure 路径（物理口径，scale=1.0 恒等）。x11 路线
  frontbuffer 通道天然 tex→dst 拉伸所以无此症状。探针复现实测链：
  toplevel buffer 1936×1148（物理化）+ readback client=960×540（虚拟化）
  + Resize 事件 1936×1148
- **F. DPI 口径单一来源**（已收口 2026-11-14）：
  - system_dpi 由 `WINEHUA_LOGPIXELS=192` 单点驱动；无 manifest 进程默认
    system-aware（见 §4），游戏消息不再虚化。
  - 监视器 DPI 首会话写入 prefix 注册表 `Video\...\Sources\<name>\Dpi`，
    暖启动经 `update_display_cache_from_registry` 读回、不再调驱动回调。
    旧会话写入的 96 会一直生效，与 system_dpi=192 不同源。修复 =
    win32u `ohos_source_dpi()` 在注册表读取与 `add_source` 两处按
    LOGPIXELS 校正（显式 override 优先，与 WAYLAND_DISPLAY 路由无关）。
  - `AppendWineLogPixelsEnv` 同时注入 wineserver 与 wineboot：首 GUI
    进程之前的注册表写入与 DPI 决策也带 192，避免冷启动落 96。
  - PE 目标的 `__OHOS__` 宏由 configure.ac `EXTRACFLAGS` 补齐；此前 PE
    侧所有 `#ifdef __OHOS__` 分支都不参与编译。
- **D. x11 虚拟桌面统一 1:1**（已拍板）：output/root 1560×1040 → 物理，
  viewport 2× 退役；桌面窗口（随宿主窗口变化）作为 wine 屏幕的动态语义
  适配；smoke win32 套件（desktopMode=virtual）回归
- **E. DPI 参数化**（可选，后置）：现值硬编码 192（`96 × WINE_CONTENT_SCALE`），
  管道（setWineLogPixels → env → win32u 单点）已通；参数化 = ArkTS 源改
  可配置 + 覆盖协议（显式 > 推导 > 默认）

## 6. 验证手段备忘

- DPI 一致性门禁：`python3 automation/smoke.py run --job smoke/jobs/dpi-wayland.json`
  （x11 用 `dpi-x11.json`）。suite 级判定 `dpi-consistency` 读归档 wine stderr
  的 `DPIDIAG`/`MSGDIAG` 打点，断言全进程 `override=system_dpi=192`、本路线
  监视器 dpi=192、窗口消息 `raw==win` 且 `win_dpi=192`、至少一次建窗 aware；
  范围只取最后一次 wineserver 启动之后。打点由 `WINEHUA_DPI_DIAG=1` 开启
  （dpi-*.json job 已声明），平时不输出。矩阵一键跑：
  `scripts/verify-dpi-matrix.sh [设备地址]`（wayland/x11 × virtual/fusion）。
- x11 C 型注入坐标：action 是 guest 桌面（X 帧）坐标，而
  `injectDisplayRouteMotion` 收画布归一化坐标。换算必须过 present 的 fit
  正变换（`smokeMapPointX11`：桌面矩形 → 画布）。画布（XComponent surface）
  与输出尺寸可以不同——实测 3120×2020 vs 3120×2080，fit 缩到 3030×2020、
  offX=45；按帧尺寸直接归一化会把 letterbox 偏移带进落点（x 偏 40px）。
- 权威探针：`smoke.py run --suite win32 --tests screen-enum-x64,win-basic-x64`
  （testId 带架构后缀；win32 套件 desktopMode=virtual，数据属虚拟桌面模式）
- 窗口事件序列：hilog `XTL attach/geom-move/geom-size/render`（尺寸已
  `%{public}`）；`[Ctrl] onSurfaceChanged` / `[NAPI] notifyToplevelResize`
- wine 内部 rect 演化：`--ps winehua.env WINEHUA_WINEDEBUG=+winex11`
  （boot 链注入有效；runWineProgram 链对 notepad 实测未生效，待查）
- 呈现尺寸：`XTL render ... tex=WxH -> dst`（帧 vs 槽，1:1 即无缩放）

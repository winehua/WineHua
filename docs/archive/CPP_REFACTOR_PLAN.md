# entry/src/main/cpp 重构规划

> 状态：已完成（2026-09-22 整理归档）。合成器重构的复盘记录，重构已全部完成。

> 目标读者：要改动 compositor / 渲染 / 输入代码的人。
> 本文基于 `feature/split-wayland-server` 分支（wayland_server 已拆出 `compositor/` 模块）的现状制定，
> 事实清单均带 `文件:行` 引用，行号以该分支为准，漂移后按符号名查找。

## 1. 背景

这个项目是三套语义系统的交汇：Wayland 协议、Wine 的 Win32 窗口语义、鸿蒙 ArkUI 窗口系统。
历史 bug 的绝大多数不是代码质量问题，而是三方语义对齐出了问题（viewport 裁剪、全屏坐标
漂移、桌面 root 可见性回归等）。因此本规划的所有原则都指向同一件事：**把"为什么这么写"
固化进代码结构、注释和文档，让不靠记忆的人也能安全地改它。**

`split-wayland-server` 重构完成了第一步（模块拆分），但拆分只搬了代码，没有解决状态
双份、语义重复、特例散落的问题。本规划是后续步骤。

## 2. 原则（评价的尺子）

- **P1 以协议语义为骨架**：模块边界沿 Wayland 对象生命周期切；非显然行为注释引用协议条款。
- **P2 每份状态只有一个 owner**：状态只在一处可写，其他模块拿派生值。
- **P3 不变式写成注释和断言**：不靠口头默契。
- **P4 特例必须有名有姓有原因**：workaround 隔离成命名函数，注释写协议/硬件原因，不写 app 名。
- **P5 模式差异收敛成策略点**：PC/Pad/phone 差异集中在少数 policy 对象，主逻辑不感知模式。
- **P6 坐标变换收进纯函数**：无设备依赖，宿主机可单测。
- **P7 可观测性按纪律**：一模块一 log tag；每帧事件降采样；诊断插桩不混进重构提交。
- **P8 搬移与行为修改分开提交**：双架构编译 + 双模式回归是提交门槛。

## 3. 现状问题清单

### 3.1 状态双份 / 多份副本（违反 P2，本次重构已暴露过回归）

| 状态 | 副本位置 | 说明 |
|---|---|---|
| minimized | `SurfaceData::minimized`（surface_data.h:52）+ `ToplevelState::minimized`（toplevel_manager.h:37） | 两份，写入点不同：xdg_shell.cpp:274 写前者，wayland_server.cpp:1194 写后者，恢复路径两边都清 |
| fullscreen | `SurfaceData::fullscreen`（surface_data.h:55）+ `ToplevelState::fullscreen`（toplevel_manager.h:39） | 靠 xdg_shell.cpp:218/250 调 `SetToplevelFullscreen` 手工同步 |
| maximized | 只有 `SurfaceData::maximized`（surface_data.h:53） | `ToplevelManager::SetToplevelMaximized`（toplevel_manager.cpp:68）不记 maximized，却清 minimized/fullscreen |
| 窗口状态方法 | `WaylandServer::SetToplevelMinimized/Maximized/Fullscreen`（wayland_server.cpp:1191-1259）与 `ToplevelManager` 同名方法（toplevel_manager.cpp:62-86） | **两套非等价实现并存**，WaylandServer 未委托（例：ToplevelManager 版 Maximized 清 fullscreen，WaylandServer 版只改 x/y） |
| 像素 | `SurfaceData::pixels` → `ToplevelState::pixels` → `SubsurfaceLayer::pixels`，外加 deprecated 全局 `pixels_`（wayland_server.h:198-201） | 一帧 shm 最多 3-4 份拷贝 |
| 位置 | `ToplevelState::x/y` + `wineX/wineY`（toplevel_manager.h:32-33）；`SurfaceData::x/y/winW/winH`（surface_data.h:29，**无写入者的遗留字段**）；desktop 模式 `geoX/geoY` 当屏幕位置（wayland_server.cpp:573-574） | 同一概念三种表达 |
| popup 偏移 | `PopupRecord::offX/offY`（toplevel_manager.h:49）vs `SubsurfaceLayer::localX/localY`（desktop_compositor.h:40）vs 协议原始值 `SurfaceData::subsurfaceX/Y`（surface_data.h:37） | 两套存储一份概念 |
| viewport | `SurfaceData::vpSrc*/vpDst*` commit 时拷入 `SubsurfaceLayer::vpDstW/H`（wayland_server.cpp:871），zero-copy 再读一遍 | 拷贝链 |

### 3.2 坐标几何重复实现（违反 P6，全屏鼠标系列 bug 的温床）

- "保比例 + 黑边" 正向映射两份：`egl_renderer.cpp:700-717`（PC 模式）与
  `compositor/compositor_utils.cpp:20-29` `ComputeFullscreenTransform`（desktop 全屏）。
- 逆映射三处：`input_manager.cpp:141-143`（CoordTransform）、`input_manager.cpp:313-314`
  （desktop 二次变换）、`compositor/input_resolver.cpp:62-81`（全屏命中）。
- 缩放因子 2.0 三处表达：`WineWindowManager.ets:110-126`（动态计算，目标恒 2.0）、
  `WineWindow.ets:30-31`、`DesktopWindow.ets:23-25`（硬编码 `/2.0`）。
- 死代码：`EglRenderer::globalDisplayScale_`（egl_renderer.h:41-42）经 NAPI `setDisplayScale`
  （napi_init.cpp:369-378）写入，**全仓库无读取方**；`Seat::MapKeycode`（seat.cpp:235-303）
  无调用方（活表在 ArkTS `KeyMap.ets`）。
- 注释与实现不符：`move_grab.h:19` 写"相对位移"，实现按绝对坐标求差（move_grab.cpp:38-48）。

### 3.3 协议层未剥离（违反 P1）

- `wayland_server.cpp` 仍含 wl_compositor / wl_surface / wl_region / wl_subcompositor /
  wl_subsurface / wp_viewporter / wp_output 全部协议回调（h:127-178，cpp:28-1018）。
- `surface_commit`（wayland_server.cpp:521-1035，约 500 行）一个函数做完：shm 像素拷贝、
  window_geometry 裁剪、双缓冲、toplevel 建档、首帧事件、ARGB 掩码、popup 登记、
  desktop root 识别、subsurface layer、frame callback。
- 对照：`xdg_shell.cpp` 已是正确范式（协议回调薄、落点到模块）。

### 3.4 模式分支散落（违反 P5）

- 只有两个布尔开关：`WaylandServer::desktopMode_`（wayland_server.h:213，源头是 ArkTS
  `WineWindowManager.ets:98-104` 的 deviceType 判断）和 `g_isPhone`（ncp_dispatch.cpp:14）。
- `desktopMode_` 判断散布 ~20 处：wayland_server.cpp 约 14 处、xdg_shell.cpp:337、
  desktop_compositor.cpp:138/169/301、input_manager.cpp:118/301/351/418/461、
  egl_renderer.cpp:272/315/615/791、graphics_broker.cpp:675、wine_env.cpp:93、
  wine_launch.cpp:285/345。
- 可归为四类差异：①事件派发（created/argb/popup 事件发不发）②subsurface 处理
  （PC popup 伪 toplevel vs desktop layer）③渲染取帧（每窗一个 renderer vs root 合成）
  ④输入命中（OHOS 窗口系统路由 vs compositor 自命中）。
- phone 模式隔离良好（只在 ncp_dispatch/phone_adapter 传输层，compositor 不感知），
  这是正确范式，保持。

### 3.5 特例与硬编码（违反 P4）

| 位置 | 内容 |
|---|---|
| desktop_root_manager.cpp:81 | `appId.find("explorer")` 字符串匹配识别桌面 root；:82-83 "全屏" = ≥output×8/10；:87-100 按 title 空/非空决策 |
| wayland_server.cpp:826-837 | Wine 最小化坐标补偿：subsurface offset >16000 减 32000 |
| wayland_server.cpp:662-675 | 最小化自动恢复阈值 contentW>200 && contentH>50 |
| wayland_server.cpp:1053-1067 + :1182-1187 | 任务栏启发（h<100 且底部对齐）两处独立使用 |
| xdg_shell.cpp:99-127 | max_size→maximize 启发（Wine 不调 set_maximized） |
| wayland_server.cpp:748-759 | ARGB 掩码 alpha>=128 阈值 |
| wayland_server.h:92-93、:422-431 | 默认输出 1280x720、伪物理尺寸换算、refresh 固定 60000 |
| wayland_server.cpp:747 与 desktop_compositor.cpp:364 | FNV 哈希常数各写一遍 |

### 3.6 可观测性不统一（违反 P7）

- LOG_DOMAIN 全 0x0000；compositor/ 五个模块与 wayland_server.cpp 共用 `WL_Server` tag，
  xdg_shell=`WL_Xdg`、seat=`WL_Seat`、input_manager=`WL_Input`。
- 消息前缀混用：`[WL]` `[MW]` `[MW-SUBSURF]` `[MW-POPUP]` `[MW-GEO]` `[MW-MOVE]`
  `[MW-COMMIT]` `[MW-TAKE]` `[MW-Life]` `[GL-TAKE]` `[XDG]`。

### 3.7 协议能力缺口（不是重构问题，单列）

- seat 只宣告 POINTER|KEYBOARD（seat.cpp:121），**无 relative pointer、无 pointer
  constraints**（全 cpp grep 零命中）。全屏游戏鼠标依赖 Wine 内部 warp/clamp，
  这是全屏鼠标系列问题的协议级根因之一。属于 feature 立项，不混入重构。

## 4. 阶段规划

排序逻辑：先做低风险的清理和纯函数化（顺便建立测试安全网），再动状态所有权（高风险），
然后做协议层大搬移（纯移动、不改行为），模式策略化放最后（要双模式全回归）。P3/P7 贯穿始终。

### Phase 0 — 清理 quick wins（低风险，可直接做） ✅ 已完成

- [x] 删死代码：`Seat::MapKeycode`（seat.cpp:235-303）、`SurfaceData::x/y/winW/winH`
      （surface_data.h:29 无写入者字段）、`EglRenderer::globalDisplayScale_` 及写入链
      （napi_init.cpp `setDisplayScale` 保留为空操作或随 ArkTS 侧一起删）。
- [x] 修正 `move_grab.h:19` 注释（实现是绝对坐标求差，不是相对位移）。
- [x] 硬编码常量命名集中：默认输出尺寸/refresh/物理尺寸换算、FNV 常数（两份合一）、
      ARGB alpha 阈值、任务栏启发参数、最小化恢复阈值、-32000 补偿。
      放 `wine_constants.h` 或新建 `compositor/compositor_constants.h`。
- [x] 任务栏启发两处（wayland_server.cpp:1053-1067、:1182-1187）抽成一个判定函数。

验证：双架构编译通过即可，无需设备回归（无行为变化）。
（实际执行：常量集中在 `compositor/compositor_constants.h`，任务栏启发为
`compositor_utils.cpp` 的 `IsTaskbarLike`；双架构编译通过 + Pad 桌面模式实测正常。）

### Phase 1 — 坐标几何纯函数化 + 宿主机单测（P6，建安全网）

- [x] 新建 `compositor/geometry.{h,cpp}`：纯函数族 `FitRect`（保比例+居中正映射）与
      `UnmapPoint`（逆映射），统一 egl_renderer.cpp:700-717 与
      compositor_utils.cpp:20-29 两份实现；`ComputeFullscreenTransform` 迁移进来。
      （实际落地为 `FitRect`/`ComputeFitRect` + FitMap/FitUnmap/FitMapDisplay/
      FitUnmapDisplay/FitSizeDisplay 函数族；egl letterbox 由截断取整改为与合成
      一致的 lround，≤1px 差异）
- [x] 输入侧三处逆映射（input_manager.cpp:141-143、:313-314、input_resolver.cpp:62-81）
      全部改用该函数族，删除本地换算。（egl_renderer zero-copy 层/遮挡重绘的
      视口正映射也一并换成 FitMapDisplay/FitSizeDisplay）
- [x] 消掉 scale 2.0 三处表达：C++ 侧从单一来源（output 尺寸 vs 渲染尺寸推导）获取，
      ArkTS 两处硬编码 `/2.0` 改为同一常量来源（需 ArkTS 配合，单独提交）。
      （实际：两处硬编码改为 `WineWindowManager.getEffectiveScale()`，值仍为 2.0，
      行为不变；C++ 侧本就不消费 displayScale，见 Phase 0 删除的 globalDisplayScale_）
- [x] Makefile 增加 `test` target：用宿主 g++ 编译 geometry + 纯逻辑测试（不依赖 OHOS），
      纳入"Makefile 是唯一构建手段"的约束内。用例至少覆盖：正逆映射互逆、黑边区域判定、
      极端宽高比、零尺寸防御。（`host_tests/geometry_test.cpp`，41 项断言）
- [x] ~~顺手把 move_grab 的坐标还原（move_grab.cpp:38-39）也改为调用公共换算~~
      （不适用：该处只是"加窗口原点"的平移，不涉及 letterbox 适配，无对应公共函数）

验证：单测通过 + 双架构编译 + Pad/PC 各跑一次全屏游戏（鼠标映射是历史重灾区）。
（当前状态：单测 41/41 通过，双架构编译通过，Pad 桌面模式实测正常；
全屏游戏回归待人工操作验证）

### Phase 2 — 窗口状态所有权归一（P2，高风险，依赖 Phase 1 的安全网）

- [x] 先定边界并写进头注释：**SurfaceData = 协议侧请求状态**（客户端 raw 请求：
      window_geometry、viewport、subsurface offset、minimized/maximized/fullscreen 请求位
      及 preMax/preFs 恢复现场）；**ToplevelState = compositor 生效状态**（合成/命中/
      可见性判定只读它）。（surface_data.h "状态边界" 注释块）
- [x] minimized/fullscreen 双份归一：生效状态只存 ToplevelState，SurfaceData 两个
      副本字段删除；读取方（xdg_shell 状态机、surface_commit 自动恢复、
      SetToplevelRestored、NotifyToplevelResize）统一改为查询
      `IsToplevelMinimized/IsToplevelFullscreen`。maximized 确认本就单份
      （仅协议侧消费），保留在 SurfaceData 并注明边界。
- [x] ~~`WaylandServer::SetToplevel*` 委托 `ToplevelManager` 同名方法~~
      **实际结论**：ToplevelManager 的 SetToplevelMinimized/Maximized/Fullscreen/
      ForceToplevelRedraw 四个方法**零调用方且语义不等价**（无 Ensure 建档、
      Maximized 多清 minimized/fullscreen）——是"看起来官方"的死代码陷阱，
      直接删除；WaylandServer::SetToplevel* 保留为唯一实现（Ensure 建档 + dirty +
      configure 协议反应），ToplevelManager 只提供查询。
- [x] popup 偏移 / viewport / damage 的拷贝链（3.1 表）评估收敛：**结论保留拷贝**。
      这些都是小结构（几个 int），每次 commit 复制成本可忽略；改为引用 SurfaceData
      会引入 wl_surface 生命周期的耦合（layer/popup 的记录须独立于协议对象存活判断），
      收益不抵风险。
- [x] 像素副本瘦身：**实际消除两份全量拷贝**（toplevel 每次 commit 省 ~2 帧 memcpy）：
      ①`sd->pixels` 全量拷贝只保留给 subsurface（其消费方全部在 subsurface 分支：
      desktop layer/PC popup/ARGB 检测；toplevel 从未消费）；②deprecated 全局
      framebuffer 改为仅在 desktop 模式 root 未识别前维护（其唯一活消费场景是
      该窗口期渲染循环回退 TakeFrame；root 识别后渲染全走 TakeToplevelFrame）。
      st.pixels 的内容裁剪拷贝保留（window_geometry 需要裁剪，无法避免）。
- [x] deprecated 全局 framebuffer 核查结论：并非死代码（egl_renderer.cpp:625、
      graphics_broker.cpp:679 在 root 未识别窗口期消费），按上条改为按需维护，
      完全下线需先重构渲染循环的取帧回退链，超出本阶段范围。

验证：双架构编译 + 双模式全回归（窗口最小化/恢复/最大化/全屏/任务栏置顶/popup 菜单/
subsurface 场景逐一过）。每个小项一个提交。
（已完成项实测：双架构编译通过；Pad 桌面模式 uitest 自动化回归——最小化→窗口消失
任务栏按钮保留→点击还原 auto-restore 触发→像素级复原；启动序列与旧版逐条一致）

### Phase 3 — 协议层从 wayland_server.cpp 剥离（P1，纯搬移）

- [x] wl_compositor / wl_surface / wl_region / wl_subcompositor / wl_subsurface /
      wp_viewporter / wl_output 移到 `wl_core.cpp`（照 xdg_shell.cpp 范式：接口表 +
      static 回调薄壳，落点到 WaylandServer/模块）。
      （含 global 注册 `RegisterWlCoreGlobals`；wayland_server.cpp 1321→397 行，
      只剩 display 生命周期、deprecated TakeFrame、toplevel 策略、事件派发）
- [x] `surface_commit` 拆成命名私有函数，每段头注释写协议语义：
      HandleNullBufferCommit / BeginShmAccess / ComputeContentArea /
      MaintainDeprecatedGlobalFb / UpdateToplevelFrameOnCommit /
      CheckDesktopRootOnCommit / UpdateSubsurface(Layer)OnCommit /
      UpdatePopupOnCommit / FinishCommit（实际分段以提交时的函数职责为准，
      与本清单命名不同但覆盖相同）。
- [x] 完成后 wayland_server.cpp 只剩 display 生命周期、global 注册、toplevel 策略
      （RaiseToplevel 等）、事件派发；评估 toplevel 策略是否并入 ToplevelManager。
      **实际结论**：不并入——toplevel 策略含 configure 协议反应与事件派发，
      超出 ToplevelManager 的纯状态管理职责，留在 WaylandServer。
- [x] 关键协议行为补注释引用条款：viewport source/destination、subsurface 同步模式、
      window_geometry 与 buffer 的关系、popup 定位。（随拆分段头注释落地）

验证：双架构编译 + 双模式冒烟。**本阶段必须零行为变化**，发现要改行为就拆出去单独提交。

### Phase 4 — 模式差异策略化（P5，放最后，双模式全回归）

- [x] 引入 `DisplayPolicy`（或 FormFactor 枚举 + 策略表），把 3.4 的四类差异各收敛成
      一个策略查询点：事件派发策略 / subsurface 处理策略 / 渲染取帧策略 / 输入命中策略。
      （`compositor/display_policy.h`：OhosWindowPerToplevel / SubsurfaceAsLayer /
      RootCompositing / CompositorRoutesInput；模式上报类调用保留 IsDesktopMode）
- [x] 逐类替换 ~20 处 `desktopMode_` 判断为策略调用；每类一个提交，便于回归定位。
      （4.0 基建 f2c1692 → 4.1 渲染取帧 2fe1c89 → 4.2 事件派发 1e200b2 →
      4.3a subsurface b6167d9 → 4.3b 输入命中 217ff8d）
- [x] phone 模式维持 ncp_dispatch 隔离，补注释说明"compositor 不感知 phone"是设计决定。
      （写入 display_policy.h 头注释）

验证：每类替换后双架构编译 + 对应模式回归；全部完成后 Pad/PC 完整回归。
**实际**：每类 arm64 编译 + 最终 x86_64 门禁；Pad 完整人工回归通过
（桌面/窗口/最小化还原/菜单/全屏游戏）；PC 模拟器不在线，①④为 PC 路径，
仅编译门禁 + 代码审查，待模拟器恢复后补 PC 实机回归。

### Phase 5 — 特例治理（P4，可与 Phase 4 并行）

- [x] explorer 识别（desktop_root_manager.cpp:81）：评估能否用协议特征（首个接近全屏的
      toplevel）替代 appId 字符串匹配；若保留，抽命名函数 + 注释写清"为什么只能是
      explorer、识别失败时的兜底行为"。
      **结论**：保留 appId（xdg_toplevel.app_id 即协议身份机制），IsExplorerDesktopShell /
      IsNearFullOutputSize 命名函数化，兜底行为写入注释（2129d20）。
- [x] 各 workaround（-32000 补偿、200x50 阈值、max_size→maximize、任务栏启发）
      统一为命名函数，注释引用 Wine 行为来源（版本/机制），不散在流程里。
      （CompensateMinimizedSubsurfaceOffset / IsRestoreSizeCommit /
      ShouldInferMaximizeFromMaxSize / IsTaskbarLike[Phase 0 已做]，2794bef）
- [x] wl_output 上报参数（尺寸/refresh/物理尺寸）改为从真实 display 信息推导或显式
      常量 + 注释说明 Wine 侧消费方式。
      （尺寸本就是 ArkTS 真实 display；物理尺寸/refresh 为显式常量[Phase 0 已做]；
      本次补 winewayland 消费方式注释，2794bef）

### Phase 6 — 不变式与可观测性（P3/P7，贯穿所有阶段）

- [x] 各 owning class 头注释列不变式，首批：
  - ToplevelManager：z-order 唯一存放处；desktop root 永不参与可见性判定（本分支
    已踩过的坑）；fullscreen toplevel 锚定 (0,0)。
  - DesktopCompositor：zero-copy surface 与 CPU 合成互斥；root 帧是合成基底。
  - InputResolver：命中顺序（全屏 → subsurface 层 → toplevel → root 兜底）及
    swallow 语义。
- [x] 上述不变式加 debug 断言（`assert` 或 `__builtin_trap` 包装，release 编译掉）。
      **实际结论**：`compositor/debug_assert.h` MW_ASSERT（默认编译为空，
      -DWINEHUA_DEBUG_ASSERT 启用；不用标准 assert —— NDK 默认构建无 NDEBUG，
      设备上 live 误触发即 abort）。逐点审计后只有"全屏锚定 (0,0)"适合断言
      （SetToplevelFullscreen 维护守卫）；root 可见性等设计规则由代码契约
      强制（IsToplevelVisibleLocked 对 root 恒 false），落头注释不加断言。
- [x] 日志纪律成文并执行：一模块一 LOG_TAG（compositor 各模块是否从 `WL_Server`
      分出 `WL_Compositor` 等，在 Phase 3 搬移时一并定）；统一消息前缀规范；
      每帧级日志必须降采样；诊断插桩随用随删或单独 chore 提交。
      **实际结论**：刻意保持单一 `WL_Server` TAG（hilog 过滤粒度过粗，模块区分
      靠消息前缀），规范写入 ARCHITECTURE.md compositor 章节。
- [x] 每个难 bug 修完在 docs/ 留复盘（三方预期各是什么、错位在哪），并更新
      ARCHITECTURE.md 的 compositor 章节与 docs/README.md 索引。
      （ARCHITECTURE.md 补 compositor 模块结构 + 日志纪律；README 索引补
      CPP_REFACTOR_PLAN.md。注意：FULLSCREEN_COORDINATE_FIX.md 从未入库，
      全屏鼠标系列 bug 的复盘结论已散落于相关代码注释，未单独重建）

### 协议能力补全（单独 feature 立项，不属于本重构）

- [ ] relative pointer / pointer constraints 协议实现（全屏游戏鼠标的协议级解），
      立项前先在 docs/ 出协议分析（参照 FULLSCREEN_COORDINATE_FIX.md 的深度）。

## 5. 不在范围

- `thirdparty/wine` 及 wine 侧补丁（另有维护策略：patch 按可 upstream 的写法写）。
- `audio_*`（已与 compositor 完全解耦，结构良好）。
- `phone_adapter/`（传输层替换，隔离良好）。
- `include/` 下 wayland-scanner 生成文件。
- `wine_mmap_test`（debug 专用）。
- ArkTS 侧重构（仅 Phase 1 的 scale 归一需要 ArkTS 配合一处）。

## 6. 执行纪律（每个 Phase 都适用）

1. 搬移与行为修改分开提交；搬移提交必须零行为变化。
2. 提交门槛：`make NATIVE_ARCH=arm64-v8a` 和 `make NATIVE_ARCH=x86_64` 都编译通过；
   涉及行为的改动需 Pad（桌面模式）+ PC（窗口模式）双回归。
3. `entry/build-profile.json5` 会被构建脚本改脏，不带入提交。
4. 每个 Phase 完成后更新本文档勾选状态。
5. 发现新的"三方语义错位"类知识，先写进 docs/ 再改代码。

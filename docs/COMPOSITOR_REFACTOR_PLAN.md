# Compositor 重构方案

> 日期：2026-08-28
> 状态：方案待评审
> 范围：`entry/src/main/cpp/` 下 compositor 全栈（`compositor/` 子目录、`wl_core.cpp`、`wayland_server.*`、`xdg_shell.cpp`、`seat.cpp`、`input_manager.cpp`、`pointer_extras.*`、`egl_renderer.cpp`、`venus/virgl_surface_presenter.*`、`graphics_broker.cpp` 呈现相关部分）
> 关联文档：`COMPOSITOR_DECISION.md`（不替换自研 compositor 的决策）、`COMPOSITOR_UNIFICATION.md`（已完成的 Layer 统一抽象阶段 1-4）、`COMPOSITOR_REFACTOR_RESEARCH.md`（帧管线行为基线）、`PROCESS_REFACTOR_PLAN.md`（进程启动侧重构，已落地）

---

## 一、背景

`COMPOSITOR_UNIFICATION.md` 的阶段 1-4 解决了"画面来源多、层序规则散"的问题（CPU 侧 3 个来源已收口到 `CompositorLayer` 单一列表，渲染/输入共用全屏仲裁）。但在此之上，代码仍积累出四类结构性问题，日常维护成本持续升高：

1. **模块臃肿**：主干类/函数体量失控，单个函数承载十几个职责；
2. **封装泄漏**：本应收口在模块内的细节（构造规则、状态协议、几何换算）暴露在调用主干，调用方必须懂得被调方的内部机制才能正确使用；
3. **知识重复**：同一概念（显示尺寸、全屏几何、坐标补偿、层序政策）在多处各有一份实现，靠注释互相指认维持同步；
4. **语义不清**：命名不表意、一名字多义、状态权威分裂、死代码伪装成活路径。

近期实证：2026-08 连续三轮 bug（全屏黑屏、红警2 点击路由、war3 卡死）全部修在 `TakeToplevelFrame` 一个 794 行的函数内部；菜单被裁剪、异形窗口、全屏漏配等问题反复出现在"画面来源/层序政策"的散点上。结构不收敛，同类问题会持续再生。

本文档先给出四类问题的量化诊断（第二节），再给出目标模块划分（第三节）与分阶段实施方案（第四节）。

---

## 二、问题诊断

所有行号以 2026-08-28 master 代码为准。

### 2.1 模块臃肿

| 位置 | 体量 | 职责数 | 关键证据 |
|---|---|---|---|
| `DesktopCompositor::TakeToplevelFrame` | **单函数 794 行**（desktop_compositor.cpp:566-1359） | 14 项子职责 | 5 个嵌套 lambda、3 条提前返回路径；dirty 门控、全屏仲裁、直传判定、签名哈希、damage 计算、快照、黑边填充、ARGB 融合、fit 缩放、裁剪 blit、PC 分支、性能插桩全部内联 |
| `InputManager` | 1176 行 + 198 行头 | 16 项职责 | `SendPointerEvent` 单函数 337 行（input_manager.cpp:377-713）；队列、坐标变换、焦点状态机、按键修饰键、手势、协议拼接、注入、会话复位混在一类 |
| `EglRenderer` | 1047 行 | 8 项职责 | 37 个 `zeroCopy*` 成员字段（egl_renderer.h:71-119）；ZC 状态机一段 ~440 行（egl_renderer.cpp:97-536） |
| `wl_core.cpp` | 1173 行 | 协议职责仅 ~350 行 | 其余 ~800 行是 SHM 上传、图像处理（ARGB 掩码 :736-756）、窗口管理策略（:607-796）、popup 管理（:931-1075）、root 识别（:799-828）、性能诊断（:1111-1157） |
| `GraphicsBroker` | 1502 行、40 个成员字段 | 5 项职责 | virgl 宿主生命周期 / IPC 传输 / ZC 簿记 / env 装配 / guest 探测五合一 |
| `WaylandServer` | ~800 行 | 上帝 facade | **约 24 个纯一行转发方法**（wayland_server.h:51-341）；`GetInstance()` 全库 **186 处 / 24 文件**（input_manager 37、napi_init 33、wl_core 20、xdg_shell 20） |
| `VenusSurfaceQueueTarget::Present` | 单函数 ~450 行（venus_surface_presenter.cpp:297-746） | — | Impl 共 50+ 成员字段，其中 perf 统计字段 17 个 |
| `ToplevelManager` | 295 行头 | 12 类职责 | 5 张注册表 + z-order + fsPriority 取号器 + ID 分配 + **3 把互斥锁**，锁序靠注释约定 |

### 2.2 封装泄漏

**构造规则泄漏——调用方逐字段拼装被调方的内部结构：**
- `SubsurfaceLayer` 15 个字段全 public，`wl_core.cpp:877-918` 逐字段手填（isExternal 判定、坐标基选择、vpDst/damage 拷贝）；`UpsertSubsurfaceLayer` 返回旧 pixels 供调用方做双缓冲轮转（wl_core.cpp:920-921）——缓冲所有权流转本应是存储内部机制。
- `InputResolver::FindInputTargetAt` 返回半成品 `InputTarget`（originX/originY/scale/contentW/contentH/swallow 6 个裸字段），最终换算由 `input_manager.cpp:441-444` 手写 `(l-origin)/scale` + `ClampToContent` 补完——"桌面坐标→surface 局部"的知识被拦腰切成两段。

**跨层知识倒挂——compositor 懂得 renderer 的内部实现：**
- SHM 全屏直传（desktop_compositor.cpp:689-834，146 行）的正当性论证直接引用 renderer 内部行为：`uForceOpaque`、`GL_BLEND`、`glClear` 黑底、renderer 侧 `ComputeFitRect`。compositor 为跳过合成必须逐条证明 renderer 会画出逐像素一致的结果。

**帧交付契约缺失——返回值不带类型标签：**
- `TakeToplevelFrame` 返回裸 `(out, w, h)`：有时输出 1400x920 桌面合成帧，有时输出 800x600 游戏直传帧，**没有任何字段说明这是什么坐标空间的帧**，调用方只能按尺寸猜。已实证事故：红警2 直传时 renderer 的 letterbox 锚在游戏帧尺寸上，输入逆映射二次缩放导致主菜单永远点不中（input_manager.cpp:159-178 注释记录了修复绕行）。`frameArgb_`（egl_renderer.cpp:754）按 root 格式强制不透明，直传帧半透明内容丢失混合。

**状态协议无主——跨模块握手由执行者兼职编排：**
- ZC 三状态握手（compositor key → ready 文件 → renderer flag，含 fallback 两步时序）的编排者是 `EglRenderer`（egl_renderer.cpp:417-454），协议本体没有 owner；"某 key 现在什么状态"要拼 3 个地方才能回答。
- `ToplevelManager::Lock()` 把整个互斥锁交给调用方（toplevel_manager.h:162）；掩码生成算法整个在模块外（wl_core.cpp:736-756 做 FNV 哈希+阈值扫描），模块只提供 `MutableMask()` 裸引用出口。
- `desktopRootToplevelId_` 由 WaylandServer 持有，按引用注入 4 个组件共享（DesktopRootManager 写、DesktopCompositor/InputResolver 读）；`DesktopRootManager` 构造注入 7 个参数（1 个 tmgr + 5 个状态引用 + 1 回调），自己什么状态都不拥有。
- `WaylandServer::outputW_/outputH_` 是 public 字段（wayland_server.h:111-112）。
- `desktopRootFrameSerial_` 是非 atomic 的跨线程字段（wl 线程写/渲染线程读），正确性依赖调用点恰好持锁。
- 协议层反向依赖业务：`wl_core.cpp` 直接调 `PluginManager::MoveRendererToToplevel`、`InputManager::InjectPointerEnter`；`wayland_server.cpp` 的 `FireToplevelEvent` 直接 `napi_call_threadsafe_function`——NAPI 通道长在 compositor 核心里。
- 渲染器登记结构泄漏到输入模块：`input_manager.cpp:141-151` CoordTransform 需要 renderer 时按"rootId → GetAnyRenderer"三级 fallback 查找。

### 2.3 知识重复

按重复份数排序，每对重复都是一次"改 A 忘 B"的回归温床：

| 概念 | 份数 | 位置 |
|---|---|---|
| vpDst 显示尺寸公式 | **11 处、2 种语义变体** | `vpDstW>0 ? vpDstW : w`：desktop_compositor.cpp:127,287,389,401,430,556、wl_core.cpp:474；`vpDstW>0 ? min(vpDstW,w) : w`：input_resolver.cpp:129、desktop_compositor.cpp:672,962,1215。clamp 与否的差异无任何注释解释 |
| xdg configure 构造 | **8 份** | xdg_shell.cpp 6 份 + wayland_server.cpp 2 份，每份手工 wl_array_init/add/release |
| "某 surface 是 zero-copy" 状态 | **5 份拷贝** | compositor key 集合（desktop_compositor.h:222）/ broker attach 表（graphics_broker.h:150）/ ready marker 文件 / presenter kSurfaceAttached flag / renderer registered+published flag |
| 全屏占屏几何 | **4 份语义** | ① `ApplyFullscreen` 锚定 (0,0)（toplevel_manager.cpp:12-19）② fs-pick+fit（desktop_compositor.cpp:299-340）③ `GetZeroCopyOccluders:534` 直接整 root pushRect（不看 fs-pick）④ GPU 侧再 fit 一次（egl_renderer.cpp:910-926）。另有 `SurfaceLocalToDesktop` 自认的第二套全屏定义（input_resolver.cpp:231-235） |
| popup 偏移公式 | **4 份** | wl_core.cpp:206-209, 947-948、desktop_compositor.cpp:285-286, 452-453 |
| 矩形相交/裁剪数学 | **5 份手写** | desktop_compositor.cpp:1133-1139, 1186-1193, 1258-1266, 512-520, 936-948 |
| z-order 政策 | **4 处散点** | 存储（ToplevelManager）、任务栏 pin（wayland_server.cpp:182-190）、菜单恒置顶（desktop_compositor.cpp:196-221）、ZC 遮挡层序（:522-558，注释自认靠人工同步） |
| enter/leave 决策 | **3 份变体** | ACT_PRESS（input_manager.cpp:593-606）、ACT_MOVE（:690-706）、SendScrollEvent（:807-814）。**scroll 那份缺桌面→surface 局部转换、无 ClampToContent、无 FindInputTargetAt——目标非 root 或有 fit 缩放时坐标空间是错的，疑似实 bug** |
| 坐标逆映射 | **3 份** | geometry.h:55 `FitUnmapDisplayX`（权威，且 FitUnmapX/Y 死代码）↔ input_manager.cpp:188-189（调用，OK）↔ input_manager.cpp:441-442 手写（且载体是 int+float 的 InputTarget 而非 int+double 的 FitRect，精度规则分裂） |
| 最小化 -32000 补偿 | **2 份** | wl_core.cpp:860-866 用命名常量 vs desktop_compositor.cpp:417-419 用**裸魔数**（常量纪律被自己打破） |
| insideWin 坐标基启发式 | **2 份** | wl_core.cpp:895-912 vs desktop_compositor.cpp:421-429 |
| 层填充块 | **2 份** | BuildLayerListLocked 两个循环体（desktop_compositor.cpp:176-193 vs :202-221），18 行里 7 个字段赋值逐字相同 |
| 事件名 | **22 种 stringly-typed 字符串** | created/argb_created/destroyed/title/.../move_end 等散布 5 文件 ~30 处 `snprintf` 手拼 JSON，无 schema 收口 |
| presenter 工具函数 | **各 2-3 份** | `NowNs`/`PresentPerfSummaryEnabled` 逐字相同两份；`NormalizeFramePeriodNs`/`PacingPeriodNs` 同名不同策略两份；全屏 quad GLSL 3 份；`TraceFrameOrder` 同一 env 两种解析 |
| "desktop 模式渲染目标 = root" | **5 处** | egl_renderer.cpp:316-317,365,745、graphics_broker.cpp:988、input_manager.cpp:143-144 |

### 2.4 语义不清

**一名多义：**
- `SurfaceData::geoX/geoY` **三义**：toplevel 桌面模式=虚拟桌面坐标、toplevel PC 模式=内容偏移、subsurface=buffer 内内容偏移——靠 hasToplevel+模式在运行时分流（wl_core.cpp:553-573）。
- `SubsurfaceLayer::isExternal`（desktop_compositor.h:66）一个 bool 身兼两义：坐标基选择（Wine 虚拟屏幕系）+ Z 序恒置顶；名字还读作"外部 surface"，名实不符。
- `ZeroCopyLayerInfo::protocolOnly`（desktop_compositor.h:26）不表意，实为"无 shm commit、几何由协议状态回退合成"。
- `GraphicsBackendState::zeroCopyFramePath` 实为"virgl IPC 已配置"，与"帧在走零拷贝"无关。
- `egl_renderer.cpp:638 rendered` 实为"纹理曾上传过内容"。
- `lastGlobalPtrX/Y_` 一字段两坐标系：desktop 模式=桌面逻辑坐标，PC 模式=窗口局部+窗口位置（input_manager.h:101-106 注释自认）。

**状态权威分裂：**
- 窗口状态三元组拆在两个结构：maximized 在 `SurfaceData`，minimized/fullscreen 在 `ToplevelState`——查询路径不统一，`SetToplevelFullscreen` 时还要手工清 `sd->maximized`（xdg_shell.cpp:223-226）。
- popup 复用 `ToplevelState` 存储，但 `PopupRecord.w/h`（上报窗口尺寸）与 `ToplevelState.Width()/Height()`（内容像素尺寸）同名字段不同语义（wl_core.cpp:974-995 解耦后）。
- surface↔toplevel 映射 5 处独立存储靠手工同步（toplevelSurfaceMap_ / surfaceResources_ / SurfaceData::toplevelId / PopupRecord / InputManager 焦点指针），popup 两表已知可失步（wl_core.cpp:1012-1014 "不应发生"）。

**死代码伪装成活路径（清理清单）：**
- `WaylandServer::pixels_/TakeFrame` deprecated 全局帧缓冲：dirty_ 永不置位、恒返回 false，却仍被 graphics_broker.cpp:992、egl_renderer.cpp:757 当兜底路径调用。
- `GraphicsBroker::TakeFrameForToplevel`（graphics_broker.cpp:979-993）零调用方。
- `InputResolver::IsZcGameSurface` / `WaylandServer::IsSurfaceFromZcGame` 零调用方（已拆除的 nudge 补偿遗留）。
- `geometry.h:28-29 FitUnmapX/Y` 零调用方，调用方手写等价式。
- `WaylandServer::SetDesktopRootToplevelId` 无调用方，若被使用将绕过 DesktopRootManager 全部不变式。
- `PointerExtras::ConstraintFor`、`Seat::GetPointerResource`、`SurfaceData::dirty`（有写无读）、`DesktopRootManager` 的死参数 contentW/contentH 与死成员 outputW_/outputH_。
- `CoordTransform` 返回 `wl_fixed_t` 但无一调用方使用返回值。

**注释与文档漂移：**
- desktop_compositor.cpp:563 注释 "TakeToplevelFrame (~390 行)" vs 实际 794 行。
- wl_core.cpp:1168-1169 注释"relative 故意不注册" vs pointer_extras.cpp:37-38 确实注册了。
- pointer_extras.h:44-46 头注释"wine 建立 Lock 约束 = 游戏进入相对模式"已被 cpp:99-101/247-251 推翻。
- move_grab.h:19-20 文档"返回被结束的 toplevelId" vs `void` 签名。
- ARCHITECTURE_OVERVIEW.md §4 "wl_core.cpp 只管协议解析" 与现状不符（~800 行窗口管理策略混在其中）；模块索引漏 `compositor_blit`、`surface_data`、`geometry` 等成员。
- 触控板系数三处漂移（InputDeviceMapper.ets 注释 0.75 vs 代码 0.25 vs input_manager.cpp 注释抄 0.75）；DesktopWindow.ets:323 的 mouse move 未调 InputDeviceMapper 缩放而 WineWindow.ets 缩放了——两条 ArkTS 路径行为已分叉。

**常驻诊断桩（违反"插桩随用随删"纪律）：**
- `TakeBreakdownWindow`（desktop_compositor.cpp:567-602，static 常驻）、`[WL-T]` commit 统计（wl_core.cpp:1111-1157）、每帧 `[MW-TAKE]`/`[DBG-CPU]`/`[MW-SWAP]` INFO 日志（渲染循环每帧一条 OH_LOG_INFO）、`zeroCopyProtocolGeometryLogged_` 一次性日志去重集合作为类成员。

### 2.5 补丁层盘点（重构必须保护的资产）

代码中的游戏/设备适配补丁约 1/3 已命名收口、2/3 直接混在主干。**这些补丁是实测换来的资产，重构必须连同注释完整平移，不得顺手"简化"掉。**

已收口的好样板（重构时参照其写法）：`pointer_extras.*` 全文件（dinput 三协议栈，独立文件+专属 TAG）、`fsPriority` 全屏取号、`ShouldSkipFullscreenCascade`/`PickFullscreenLayerLocked`/`ComputeFullscreenFitLocked`（渲染/输入单一实现）、`CompensateMinimizedSubsurfaceOffset`、`IsRestoreSizeCommit`、`ClampToContent`、`ShouldInferMaximizeFromMaxSize`。

混在主干的重点补丁（`TakeToplevelFrame` 内 8 个）：

| 位置 | 补丁 | 针对问题 |
|---|---|---|
| desktop_compositor.cpp:689-834 | SHM 全屏直传 | war3 800x600 全屏 CPU 合成 70-85ms 钉死 ~10fps |
| :913-1011 | 局部合成 DamageRect | war3 每帧全桌面合成 ~74ms（13fps） |
| :878-1104 | BlitSource 快照+锁外 blit | 持锁合成堵 wl 事件循环，commit 平均被堵 27ms |
| :660-682 | fullscreenContentCovered 扫描 | 全屏内容被不透明 sub 覆盖时的黑边/缩放跳过 |
| :1112-1122 | ZC 游戏全屏整幅填黑 | ZC 游戏 SHM 内容是 explorer 桌面而非游戏画面 |
| :1078-1093 | ARGB opaque 判定融合进快照 | wl 线程全帧 alpha 扫描 ~7ms 堵事件循环 |
| :196-221 | 菜单恒置顶尾部层 | 菜单父窗口 z-order 低于置顶任务栏时被挡住 |
| input_manager.cpp:657-683 | 点击脉冲拉伸 | PAL2 按帧轮询 GetDeviceState，同刻 Press+Release 被吞 |
| input_manager.cpp:501-520 | rawDelta 优先+±512 钳制 | 光标钳在屏幕边缘后绝对差分恒 0，dinput FPS 视角卡死 |
| wl_core.cpp:766-795 | 全屏尺寸漂移重发 configure | war3 D3D 模式切换画面缩左上；含持锁自死锁修复 |
| wl_core.cpp:974-995 | popup 窗口/内容尺寸解耦 | war3 PC 模式 GL client surface 缩左上 |
| wayland_server.cpp:428-436 | NotifyToplevelResize 最小化门禁 | war3 还原后黑屏+点击再最小化 |

---

## 三、目标模块划分

原则：**数据跟着行为走，知识单点化，补丁隔离命名化**。不推翻现有类边界中合理的部分（`ToplevelState` 字段私有化、`compositor_blit`/`geometry` 纯函数、`DisplayPolicy`、`pointer_extras` 都是正面样板），新增/调整以下模块：

```
compositor/
  layer_stack.{h,cpp}        新增：subsurface 层容器 + 构造规则 + Z 序微调
                             （吸收 subsurfaceLayers_、Upsert/Remove/Reorder、
                              BuildLayerList/BuildWindowLayerList、层填充规则）
  fullscreen_policy.{h,cpp}  新增：全屏仲裁单点
                             （PickFullscreen/ComputeFullscreenFit/
                              ShouldSkipFullscreenCascade/SelectFullscreenContentSize，
                              渲染与输入继续共用，ZC 遮挡计算改为遍历 Layer 列表）
  frame_pipeline.{h,cpp}     新增：帧合成管线（拆 TakeToplevelFrame）
                             FramePlanner（锁内：层列表→全屏→直传判定→签名→damage→快照）
                             FrameBlitter（锁外：纯像素，不碰 tmgr_ 锁）
  zc_bridge.{h,cpp}          新增：ZC 层几何供给 + key 簿记
                             （GetZeroCopyLayerInfo/GetZeroCopyOccluders/
                              SetSurfaceZeroCopy/zeroCopySurfaceKeys_）
  zorder_policy.h            新增：层序政策单点（任务栏 pin、菜单恒置顶、
                             ZC 遮挡层序、fsPriority 消费收口为一个排序函数）
  geometry.h                 增补：DisplaySizeAfterViewport（两个命名变体区分
                             clamp/不 clamp）、ResolveSubsurfaceOrigin
  presented_frame.h          新增：帧交付契约（见下）
  toplevel_manager.*         调整：新增 RaiseToplevel/PinToTop/EnsureInZOrder 语义
                             方法；单字段 getter 换快照 struct；掩码算法内聚进来
  desktop_compositor.*       瘦身为上述模块的编排者 + PC/Desktop 策略分流
```

关键契约——**`PresentedFrame`（帧交付契约）**：

```cpp
struct PresentedFrame {
    enum class Kind { Composed, DirectPass } kind;  // 合成帧 / 直传帧
    enum class BaseSpace { Desktop, Window } space; // 坐标空间
    const uint32_t* pixels; int w, h;
    int contentW, contentH;   // 内容逻辑尺寸（输入逆映射用）
    bool opaque;              // 帧自身的不透明性（不再按 root 格式反查）
};
```

renderer 的 letterbox、`frameArgb_`、input 的 CoordTransform 全部改从该结构取几何——这是"画面来源统一抽象"缺的那一半，消灭直传帧按尺寸猜空间的红警2 类事故。

协议层与输入栈的目标划分（第五、六阶段实施）：

```
WlSurfaceProtocol（协议壳）   wl_core.cpp 瘦身：vtable + pending 状态机，
                             commit 产出不可变 CommittedSurface 快照
                             （role/frame/contentRect/screenPos/parentOffset 命名字段，
                              geoX/geoY 三义在此消亡）
PopupManager                 popup 登记/裁剪/事件（吸收 wl_core.cpp:931-1075，
                             popup 偏移公式单点化）
XdgConfigureBuilder          xdg configure 构造单点（消灭 8 份 wl_array 拷贝）
ToplevelEventBus             事件 enum 化 + JSON 构造单点，NAPI 通道移到订阅者侧
InputQueue/InputStateTracker/InputInjector/InputSpaceMapper
                             InputManager 拆四层：队列机制 / 焦点按键状态 /
                             唯一碰 wl_*_send_* 的注入器 / 坐标变换收口
DesktopSessionState          desktopRootToplevelId/output 尺寸等共享状态 POD，
                             消灭"引用成员指向宿主子字段"的隐式同步
```

---

## 四、分阶段实施方案

每阶段独立可构建、可验证、可提交；原则上**行为平价**（纯搬移/收口，不改变行为），任何行为变化必须单独标注并过设备回归。

### 阶段 0：死代码与诊断桩清理（零风险，纯删除）

- 删除：`WaylandServer::TakeFrame/pixels_` 及 graphics_broker.cpp:992、egl_renderer.cpp:757 两处死兜底；`GraphicsBroker::TakeFrameForToplevel`；`IsZcGameSurface`/`IsSurfaceFromZcGame`；`FitUnmapX/Y`；`SetDesktopRootToplevelId`；`ConstraintFor`；`Seat::GetPointerResource`；`SurfaceData::dirty`；DesktopRootManager 死参数/死成员；`CoordTransform` 无调用方的返回值。
- 诊断桩收口：`TakeBreakdownWindow`/`[WL-T]`/每帧 INFO 日志统一改为 `WINEHUA_FRAME_TRACE=1` 运行时开关（与现有 `TraceFrameOrder` 合并为一个门控点）；`zeroCopyProtocolGeometryLogged_` 改为函数级 static 去重。
- 同步修正过时注释与文档（~390 行注释、relative 注册注释、pointer_extras 头注释、move_grab 文档、ARCHITECTURE_OVERVIEW 模块清单）。
- **验证**：构建通过 + host_tests 通过 + 模拟器桌面链冒烟。

### 阶段 1：纯函数与语义收口（低风险，不改模块边界）

- `geometry.h` 增加 `DisplaySizeAfterViewport` 两个命名变体（区分 clamp/不 clamp），替换 11 处三元式——**变体选择逐处核对现有行为，不得统一为单一语义**（clamp 差异可能是有意的，统一即行为变化，需单独评估）。
- desktop_compositor.cpp:417-419 裸魔数改用 `kMinimizedCoord*` 常量；`CompensateMinimizedSubsurfaceOffset` 移为共享 helper。
- `BuildLayerListLocked` 两个循环体合并为一个带谓词的私有方法。
- `HasZeroCopyLayerForToplevelLocked`/`GetZeroCopyContentSizeLocked` 合并为 `FindZeroCopyLayerLocked`。
- `ToplevelManager` 新增 `RaiseToplevel(id)`/`PinToTop(id)`/`EnsureInZOrder(id)`，wayland_server.cpp:168-169,187-188、wl_core.cpp:760-761 三处改调用；5 个单字段 getter 换成一次加锁返回快照 struct。
- `XdgConfigureBuilder::Send()` 单点，替换 8 份 configure 构造。
- **验证**：构建 + host_tests（geometry_test.cpp 扩充 DisplaySize 用例）+ 模拟器桌面 + notepad 直启回归。

### 阶段 2：帧管线拆分 + 帧交付契约（核心阶段）

- `TakeToplevelFrame` 拆为 `frame_pipeline.{h,cpp}` 的 `FramePlanner`（锁内：BuildLayerList→全屏仲裁→直传判定→签名→damage→快照）+ `FrameBlitter`（锁外纯像素）；主函数只剩编排（<100 行）。blit lambda 变自由函数后纳入 host_tests。
- 引入 `PresentedFrame` 契约替代裸 `(out,w,h)`；egl_renderer 的 letterbox/frameArgb_、input_manager 的 CoordTransform 改从契约取几何；删除 input_manager.cpp:168-178 的绕路重算与 egl_renderer.cpp:750-755 的格式反查。
- PC/Desktop 两条合成路径按 DisplayPolicy 拆为两个策略实现（FrameComposer/WindowFrameComposer），不再挤在同一函数按 `id == desktopRootToplevelId_` 分流。
- SHM 直传改为能力协商：renderer 声明"能直接消费原始层帧"的查询接口，直传决策与逐像素等价论证移归 renderer 侧，compositor 不再引用 `uForceOpaque`/`GL_BLEND` 等 renderer 内部知识。
- **补丁平移**：§2.5 表中 TakeToplevelFrame 内 8 个补丁连同注释完整迁入对应命名阶段（直传补丁进 DirectPass 判定、局部合成进 Damage 计算、快照补丁进 Snapshot 阶段等）。
- **验证**：host_tests 新增 blit/damage 用例；**设备回归基准：war3（直传+局部合成+尺寸漂移）、PAL2（SHM+dinput）、RA2（全屏+点击路由）三游戏全过**；WL-T 埋点对比重构前后 commit 阻塞时长无回退。

### 阶段 3：ZC 与层序政策收口

- `zc_bridge.{h,cpp}`：ZC 层几何供给与 key 簿记从 DesktopCompositor 抽出；`GetZeroCopyOccluders` 改为遍历 Layer 列表（消除第 4 份全屏语义）；`ZeroCopyLayerInfo` 的 protocolOnly 布尔改显式 enum。
- `zorder_policy.h`：任务栏 pin、菜单恒置顶、ZC 遮挡层序、fsPriority 消费收口为一个可读的排序函数。
- `ZeroCopyStateCoordinator`：5 份 ZC 状态拷贝收敛为 1 权威（compositor 侧）+ 2 投影（ready marker、broker attach），提供 `Activate/BeginFallback/ConfirmFallback` 幂等方法；把 egl_renderer.cpp:417-454 的时序编排搬进协议 owner。
- 呈现侧顺手项：`PresentTarget` 统一接口（消灭 if/else 事实多态与 -2..-7 魔数返回值）；`presenter_common.h` 收编重复的 NowNs/NormalizeFramePeriodNs 等（不同钳制策略改显式命名常量）；全屏 quad GLSL 统一用 shader_utils。
- **验证**：ZC 游戏（DXVK/OpenGL 应用）遮挡、全屏、fallback 场景回归；菜单裁剪/置顶场景回归。

### 阶段 4：输入栈拆分

- `InputResolver` 裁决闭环：`FindInputTargetAt` 返回终态结论（surface + 可注入局部坐标 + action），逆映射与 ClampToContent 收进 resolver；`InputTarget` 砍为携带 `FitRect`（统一 double 精度）；**修复 SendScrollEvent 缺段问题（疑似实 bug，单独提交并标注行为变化）**；enter/leave 三份变体收敛为一个方法。
- `InputManager` 拆四层：`InputQueue`（队列+pipe+去重）、`InputStateTracker`（button/modifier/焦点，可宿主机单测）、`InputInjector`（唯一碰 `wl_*_send_*`）、薄编排管线。
- `InputSpaceMapper`：坐标变换收口（7 个坐标空间的映射单点化；`lastGlobalPtr` 双语义改显式类型）；renderer 查找 fallback 移入该模块，InputManager 不再认识 PluginManager；PointerExtras 与 InputManager 解环。
- 两处 `IsDesktopMode()` 真策略分支改用 `Policy()` 命名查询（input_manager.cpp:168/215）。
- **验证**：PAL2 点击/移动、war3 光标、RA2 全屏点击路由、桌面任务栏交互全回归；scroll 修复单独验证（有 fit 缩放的窗口内滚轮）。

### 阶段 5：协议层重构（牵动面最大，放最后）

- `wl_core.cpp` 拆协议壳：vtable + pending 状态机保留，commit 产出不可变 `CommittedSurface` 快照（role/contentRect/screenPos/parentOffset 命名字段，geoX/geoY 三义消亡）；SHM 拷贝/缩放抽为 `ShmFrameSource` 纯函数（可进 host_tests）。
- commit 业务段各归其主：ARGB 掩码 → `ToplevelManager` 内聚；位置同步/恢复/尺寸漂移 → ToplevelManager 语义方法层（消除"持锁不能调自己"的 lk.unlock hack）；popup 登记 → `PopupManager`（偏移公式 4 份收口）；首帧 focus → 会话焦点策略。
- 窗口状态权威统一：maximized 迁入 `ToplevelState`，与 minimized/fullscreen 同权威（行为敏感，需逐消费点核对）。
- `ToplevelEventBus`：22 种事件 enum 化 + JSON 构造单点；NAPI/wine_process 符号移出 compositor 核心。
- **验证**：全量设备回归（桌面链、PC 多窗口、popup 菜单、异形窗口、三游戏）。

### 阶段 6：facade 瘦身与共享状态收口

- `WaylandServer` 只留生命周期（Start/Stop/EventLoop/ResetSessionState）与回调注册；~24 个转发方法删除，调用方构造注入 `ToplevelManager*`/`DesktopCompositor*`/`InputResolver*`。
- `desktopRootToplevelId_`/`outputW_`/`outputH_` 等共享状态收进 `DesktopSessionState` POD，组件持同一对象；DesktopRootManager 真正拥有 root 状态。
- `desktopRootFrameSerial_` 改 `std::atomic<uint64_t>` 或并入 ToplevelState 的 frameSerial（核实是否与 BumpFrameSerial 为同一概念两份实现）。
- **验证**：全量回归。

### 阶段顺序的依赖关系

0 → 1 无依赖先行；2 是核心债务（794 行函数）独立可做；3 依赖 2 的层列表产出形态；4 依赖 3 的 FullscreenPolicy 接口（输入/渲染共用仲裁）；5 牵动面最大放最后；6 在 2-5 完成后转发方法自然收窄。

---

## 五、验证策略

- **构建**：Makefile 唯一手段（`make NATIVE_ARCH=x86_64` / `arm64-v8a`），每阶段双架构构建通过。
- **宿主单测**：host_tests（geometry_test、blit_scaled_test、env_*_test）随阶段扩充——凡抽成纯函数的逻辑（显示尺寸、伤害矩形、blit 裁剪、坐标映射）必须有对应用例。
- **设备回归基准**（每个行为敏感阶段必过）：
  - 桌面链冒烟：桌面启动、notepad 直启、任务栏交互、窗口最小化/还原；
  - **war3**：全屏直传、局部合成、尺寸漂移、最小化还原黑屏；
  - **PAL2**：SHM+dinput 相对模式、静止点击、光标滞后；
  - **RA2**：全屏点击路由、连带 fullscreen 窗口跳过；
  - 菜单/popup：窗口边缘菜单裁剪、菜单置顶；
  - ZC：OpenGL/DXVK 应用遮挡、全屏、窗口切换 z-order。
- **性能门禁**：WL-T（commit 阻塞）与 GL-TAKE（合成分段耗时）重构前后对比无回退——重构不得把锁外 blit 挪回锁内。

---

## 六、非目标与红线

1. **不替换 compositor 架构**：自研 vs libweston/wlroots 的决策已定（COMPOSITOR_DECISION.md），本方案是其后续——在自研基础上收敛结构。
2. **行为平价为默认**：重构阶段不做"顺手优化"；任何行为变化（如 SendScrollEvent 缺段修复、vpDst clamp 语义统一）必须单独提交、单独标注、单独回归。
3. **补丁逻辑是资产**：§2.5 清单中的补丁连同注释完整平移，禁止以"简化"为名删除游戏/设备适配逻辑。
4. **ArkTS 侧暂不纳入**：DesktopWindow/WineWindow 双份手势状态机、rawDelta 缩放分叉等问题已记录（§2.4），但 ArkTS 重构另行立项，本方案只动 native。
5. **协议行为不变**：Wayland 协议层对外语义（configure 时序、事件顺序、focus 语义）保持不变；CommittedSurface 快照是内部重构，不改变客户端可见行为。

---

## 七、风险与缓解

| 风险 | 缓解 |
|---|---|
| 阶段 2 拆帧管线时补丁语义丢失 | §2.5 清单逐项对账；三游戏回归基准；WL-T/GL-TAKE 性能对比 |
| vpDst clamp 两种变体实为有意区分 | 阶段 1 只抽命名函数不改语义；统一语义单独立项评估 |
| 全屏双定义（input_resolver.cpp:231-235 自认）统一引入行为变化 | 阶段 4 单独验证 warp/命中场景，按该注释里的诊断预案回滚 |
| 锁纪律（3 把锁+嵌套序）在拆分中破坏 | 拆分保持 tmgr 锁为唯一外层锁；新增模块不引入独立锁（COMPOSITOR_REFACTOR_RESEARCH.md 约束 1） |
| 阶段 5 窗口状态权威统一影响 xdg 行为 | 逐消费点核对 maximized 读写；xdg configure 状态位回归 |

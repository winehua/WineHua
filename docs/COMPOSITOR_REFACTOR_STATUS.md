# Compositor 重构进度与续作清单

本文档是 `COMPOSITOR_REFACTOR_PLAN.md`（方案与红线）的执行台账：记录已完成阶段、
当前中断现场的精确状态、剩余工作清单与续作执行要点。方案本身（目标模块划分、
阶段依赖、验证策略、红线）以 PLAN 文档为准，本文不重复。

工作分支：`feature/compositor-refactor`（从 master 切出，基点与 origin/master
同步）。所有提交目前只在本地分支，合回 master / push 需用户明确指示。

## 一、进度总览

| 阶段 | 状态 | 提交 |
| --- | --- | --- |
| 0 死代码与诊断桩清理 | 完成（门禁全过） | 3f23dc4 方案文档、b44f5d8 死代码、fe492aa 诊断门控、a2e1a55 注释文档 |
| 1 纯函数与语义收口 | 完成（门禁全过） | 5a2985a 显示尺寸、1bd342e 最小化补偿、c76b24e 循环合并、b1d1af6 ZC查找、1e1f611 Raise语义、2f3f41f xdg configure |
| 2A 帧管线结构拆分 | 完成（门禁全过） | 690fb68 抽段、5c554c5 迁 frame_pipeline、8de86bd blit_clip_test |
| 2B PresentedFrame 契约 + 直传能力协商 | **进行中（任务 1/2 完成，3 待做）** | 0d06926 消费侧接入、1902862 策略拆分 |
| 3 ZC 与层序政策收口 | **完成（代码层全落地，待 ZC 设备回归）** | f9a2d8a zc_bridge 抽离、cf7e9a1/a3156a6/20b4bdb presenter+ZC 状态机、328ed21/48b7333 zorder_policy 三散点、a2d142d 3A 精化、5ebded3 3B 层序精化 |
| 4A InputResolver 裁决闭环 | 完成（门禁全过） | 本分支 HEAD 提交（§二 4A 下标） |
| 4B 其余输入栈拆分（InputManager 拆层/ScrollEvent 缺段修复等） | 未开始 | — |
| 5 协议层重构 | 未开始 | — |
| 6 facade 瘦身与共享状态收口 | 未开始 | — |

已过门禁（每个完成阶段均满足）：`make test` host_tests 全绿；
`make NATIVE_ARCH=x86_64` 与 `make NATIVE_ARCH=arm64-v8a` 全量构建成功；
x86_64 模拟器桌面链（EntryAbility → DesktopAbility → 中文桌面+任务栏）与
notepad 直启冒烟通过。

## 二、2B 续作进展

中断现场（3461a1d）的**消费侧已接入**（任务 1），项目恢复可编译；任务 1、2
均已提交并过门禁（见 §一 表格、§三 各任务要点）。本节记录已完成任务的要点，
便于回溯；剩余任务 3 见 §三。

### 已完成

- `compositor/presented_frame.h`（新增）：契约结构 `PresentedFrame`——
  `kind(Composed|DirectPass)`、`baseSpace(Desktop|Window)`、`pixels/w/h`
  （buffer 及尺寸）、`contentW/contentH`（输入逆映射锚定的逻辑内容尺寸，
  与 buffer 尺寸解耦：直传帧 buffer 是游戏内容尺寸如 800x600，锚仍是桌面
  逻辑尺寸）、`opaque`（XRGB → 按不透明呈现）。头部注释记录了契约动机
  （红警2 直传点击事故：letterbox 锚错坐标空间导致输入二次缩放）。
- `compositor/frame_pipeline.{h,cpp}`：FramePlanner/FrameBlitter 全部签名
  从裸 `(out, w, h)` 切到 `PresentedFrame&`；三个出口按路径填好契约字段——
  - kFastPath（GateDesktopDirtyLocked 无子窗口快进）：Desktop 空间，
    buffer = 内容 = root 尺寸；
  - kDirectPass（TryShmFullscreenDirectLocked SHM 全屏直传）：Desktop 空间，
    buffer = 游戏内容尺寸，content = root 逻辑尺寸（红警2 修复的契约化）；
  - kCompose（PlanDesktopLocked 合成）：Desktop 空间，buffer = 内容 = root，
    `frame.pixels` 由编排者 blit 后补（注释已写明）。
- `compositor/desktop_compositor.{h,cpp}`：`TakeToplevelFrame` 签名切换，
  各出口补 `frame.pixels = out.data()`；PC 路径 `TakeWindowFrameLocked`
  填 Window 空间契约（buffer = 内容 = 窗口尺寸，opaque 取 ShmFormat）。
- `egl_renderer.h`：声明 `FitRect GetInputLetterbox() const` 替代
  `GetLetterbox()`，注释已写明语义（锚 contentW/H 到 surface 的保比例 fit；
  锚未就绪或 fit 失败退回显示 letterbox，与旧 CoordTransform fallback 一致）。
- `wayland_server.h`：转发签名切换；删除 `GetToplevelShmFormat`。
- `compositor/toplevel_manager.{h,cpp}`：删除 `GetToplevelShmFormat`。
- 任务 4（2A 遗留清理）已顺带完成：死变量 occlTl/occlType/occlZc 已删。

### 任务 1（消费侧接入，0d06926）：编译断点已解除

- `egl_renderer.h`：加 contentW_/contentH_ 缓存最近帧契约的逻辑内容尺寸（输入锚）。
- `egl_renderer.cpp`：实现 `GetInputLetterbox()`（contentW/H 对当前 surface 保比例
  fit，无帧/fit 失败退回显示 letterbox_）；RenderLoop 取帧改新签名
  `TakeToplevelFrame(id,out,frame)`，fw/fh 取 frame.w/h、frameArgb_ 取 !frame.opaque、
  缓存 contentW/H。
- `input_manager.cpp`：CoordTransform 改调 `GetInputLetterbox()`，删绕路重算
  （IsDesktopMode/GetToplevelW/ComputeFitRect/GetLetterbox）。
- 对账基准：桌面合成/快进/直传锚 root 逻辑尺寸，PC 窗口帧锚窗口内容尺寸 ——
  与旧实现逐点一致（红警2 直传点击修复即直传帧 buffer 尺寸与锚解耦）。

### 任务 2（显示策略拆分，1902862）：取帧路径 DisplayPolicy 多态化

- `display_policy.h`：新增 `FrameRouteFor(id, rootId)` 路由（DesktopRoot|Window），
  替换原 `RootCompositing() && id == desktopRootToplevelId_` 分流，编排者只问策略要帧。
- `frame_composer.{h,cpp}`（新增）：FrameComposer 抽象 + DesktopRootFrameComposer
  （原 TakeToplevelFrame desktop 分支：FramePlanner 锁内规划 + FrameBlitter 锁外合成）+
  WindowFrameComposer（原 TakeWindowFrameLocked：窗口 SHM 帧 + 窗口内 subsurface blit）。
- `desktop_compositor`：TakeToplevelFrame 瘦身为纯编排（按 FrameRouteFor 委托 composer），
  删除 TakeWindowFrameLocked；两 composer 经 friend 访问 tmgr_（合成状态仍由本类持有）。
- 行为平价：两 Compose 体逐行复刻原分支，锁边界/计时点/日志门控不变；desktop+非 root
  仍走 Window composer，与旧 `RootCompositing() && id==root` 逐点等价。

### 验证记录（模拟器冒烟，2026-08-29）

- 重建 x86_64 HAP 部署模拟器（1280x800，scale 2.25），卸载重装 + 清 prefix 首启 wineboot。
- **任务 2 桌面链冒烟 ✅**：DesktopAbility 出图 = 蓝色桌面 + 左下角中文「开始」任务栏
  （WL-STAT renderers=1，tl=4 桌面 root renderer 持续渲染 22s+ 无崩溃），
  `DesktopRootFrameComposer` 行为平价实证通过。
- **任务 1**：双架构编译 + host_tests 全绿 + 逻辑对账（`GetInputLetterbox` 桌面路径
  == 旧 CoordTransform 桌面分支，逐点等价；直传帧 contentW/H 锚 root 为红警2 契约化）。
  运行时输入逆映射：模拟器无 `input`/`uitest` 鼠标注入通道（均不可用），无法实证；
  且直传/游戏输入回归需 arm64 Pad 真机（war3/PAL2/RA2），属 2B 最终验收。
- 局限：模拟器无游戏，`WindowFrameComposer`（PC 模式）未在运行中触发（桌面模式取帧
  只走 root，走 `DesktopRootFrameComposer`）；其正确性由逐行复刻 + 双架构编译保证。

### 阶段 3A（zc_bridge 抽离，f9a2d8a，2026-08-29）

- `compositor/zc_bridge.{h,cpp}`（新增）：ZC 层几何供给与 key 簿记从
  DesktopCompositor 抽出。`ZeroCopyLayerInfo`/`ZeroCopyOccluderRect` 迁入
  zc_bridge.h（仍全局作用域，WaylandServer using 别名不变）；`protocolOnly`
  布尔改显式 `ZeroCopySource` 枚举（仅 once-log 信息位，无运行时读方，纯重标）。
  `ZcBridge` 为 DesktopCompositor 的 friend（同 FrameComposer 模式），持有
  ZC key 权威集合（`activeKeys_`，原 `zeroCopySurfaceKeys_`），DesktopCompositor
  经 `zc_` 委托（GetZeroCopyLayerInfo/GetOccluders/SetSurfaceZeroCopy/
  RemoveZeroCopyKeyLocked/HasZeroCopyLayer/GetZeroCopyContentSize）。
- 行为平价：六方法体逐字搬移（仅成员引用改 `comp_.xxx`、key 集合改 `activeKeys_`、
  `protocolOnly` 改 `source`）；`FindZeroCopyLayerForToplevelLocked` 保留在本类，
  改经 `zc_.IsActive` 判定——单一查找谓词不变。锁边界/读写线程域不变（tmgr 锁内）。
- 门禁：`make test` host_tests 全绿；x86_64 + arm64-v8a hap 构建通过。
- 设备验证（arm64 真机 192.168.1.8:33363，2026-08-29）：干净重装后首启正常；
  桌面正常渲染（MW-RNDR 1400x920→2800x1840 持续出帧），游戏窗口（如 toplevel #29
  800x600）经 SHM 合成正常出画面；VirGL ZC `pipeline ready tl=3 SURFACE_QUEUE`；
  全程无 compositor/ZC/CRASH/WL-ERR 错误；Wow64Install 干净（wow64 ok=747 failed=0）。
  **结论：3A 桌面/SHM 合成路径行为平价实证通过。** 局限：这组游戏走 CPU SHM 合成，
  未触发 GPU_ACTIVE（ZC GPU overlay 链）——该段为逐字搬移且其余路径已实证，非缺陷。
- **3A 剩余精化（暂缓）**：`GetZeroCopyOccluders` 改遍历 Layer 列表以消除第 4 份
  全屏语义——行为敏感，需 ZC 游戏遮挡回归，本次仅做原样搬移保平价。

### 阶段 3D（PresentTarget 统一 + presenter 收编，cf7e9a1 / a3156a6，2026-08-29）

- `presenter_common.h`（新增）：收编 virgl/venus 两 presenter 逐字重复的
  `NowNs`/`NowUs`/`PresentPerfSummaryEnabled` 与帧周期常量；`NormalizeFramePeriodNs`/
  `PacingPeriodNs` 同名不同策略拆为显式命名的 `NormalizeVirgl/VenusFramePeriodNs`、
  `Virgl/VenusPacingPeriodNs`，边界用命名常量（kMin/kVirglMax/kVenusMax/kDispatchLead/
  kReleaseFenceWatchdog）表达。
- `shader_utils.{h,cpp}`：收编 virgl presenter 内嵌全屏 quad GLSL 为
  `kPresentFullscreenQuadVS/FS`（`gl_VertexID` 大三角形 + 无 BGR swizzle/uForceOpaque，
  与 egl_renderer 的 `kFullscreenQuadFS` 语义不同，不可互换）；`virgl_child` 库加
  `shader_utils.cpp` 提供链接。
- `present_target.h`（新增）：`PresentTarget` 抽象接口（Attach/Detach/SetFramePeriod/
  IsVulkan/Present/PresentVenus/HasVulkanDevice/Prepare+FinishDeviceRelease）+ 返回码命名
  （kPresentNoTarget=-2/SourceInvisible=-3/GlSetupFailed=-4/MakeCurrentFailed=-5/
  BlitFailed=-6/FenceSyncFailed=-7/Invalid=-EINVAL/Throttled=1）——数值与旧实现逐点
  一致，消费者 virgl_child.cpp 的 `< -2 且 != -6` 日志门控语义保留。
- `SurfaceQueuePresenterManager`：Entry 改持单一 `unique_ptr<PresentTarget>`，
  Attach/Detach/SetFramePeriod/Present/PresentVenus/Prepare/Finish/Reset 全经接口调度，
  **消灭 `if (flags & kSurfaceVulkan)` 事实多态**；`RetireVenusTargetLocked` 泛化为
  `RetireTargetLocked`（按 IsVulkan 判定延迟释放）。两 target 分别实现接口，各自
  不支持的呈现路径为防御性死路径返回 kPresentInvalid。
- 行为平价：目标生成（vulkan 重造 venus / 非 vulkan 复用 virgl）、延迟释放（venus
  持 device 移入 retired）、返回码数值、日志门控、锁边界均与原实现逐段一致。
- 门禁：`make test` host_tests 全绿；x86_64 + arm64-v8a hap 构建通过。
- **3D 剩余（暂缓）**：veven child 的 `PresentVenus` 调度 wait 与 `retiredVenusTargets_`
  保持原样（未引入 PresentTarget 上的统一 deadline/pacing 表，属超范围）。

### 阶段 3B（zorder_policy 层序谓词收口，328ed21，2026-08-29 首步）

- `zorder_policy.h`（新增）：场景层序政策单一权威，首版两个谓词 —
  `ZOrderTopAnchored`（parent==root 或 isExternal 或 父不在 z-order → 恒置顶，
  原 BuildLayerListLocked 尾部置顶循环）与 `ZOrderNeedsParentPosCheck`（父==ZC
  窗口或==root 时不必查 z-order 位置，原 zc_bridge GetOccluders 遮挡防护条件）。
  注释记录收敛的散点（任务栏 pin 已在 ToplevelManager::PinToTop / 菜单恒置顶 /
  ZC 遮挡层序 / PickFullscreen fsPriority 消费）。
- `desktop_compositor.cpp` BuildLayerListLocked 尾部置顶循环、`zc_bridge.cpp`
  GetOccluders 遮挡防护条件改调谓词——逐字复现原 if，仅知识归属收拢。
- 行为平价：条件逐字等价，层序生成仍由 BuildLayerList 编排；消两处手写重复。
- 门禁：`make test` host_tests 全绿；x86_64 + arm64-v8a hap 构建通过。
- **3B 谓词收口完成（48b7333，2026-08-29）**：zorder_policy.h 第 3 个散点
  （PickFullscreen 的 fsPriority 取最大）收口为 `ZOrderFullscreenCandidateBeats`
  谓词，PickFullscreenLayerLocked 改调它——行为逐字等价（!best || cand>best →
  !bestValid || cand>best）。至此头部注释列的三个散点（菜单恒置顶 /
  ZC 遮挡防护 / fsPriority 取最大）全部收进 zorder_policy，消费方只调谓词。
- **3B 层序精化完成（5ebded3，2026-08-29）**：BuildLayerListLocked 由"嵌套
  循环隐式排布"显式化为"排序键排布" — `zorder_policy.h` 新增 `ZOrderGroup`
  （Root/InZOrder/TopAnchored）、`ZOrderSeq`（(group,laneSeq,itemSeq) 全序）、
  `ZOrderGroupFor`（组归属=!ZOrderTopAnchored→InZOrder，与旧 main/尾部循环
  互斥划分逐字对应）；BuildLayerListLocked 改为"待排项建键 → std::sort →
  按输出顺序分配 zIndex"。对账：5 场景 + 3000 组随机模糊对拍 0 不一致
  （laneSeq 用 emit 序号非原始下标——root 占位时原始下标会偏移 lane 破块序；
  父在列但 toplevels_ 缺失 → 丢弃，与旧行为逐字对应）。
- `PinToTop` 全屏例外收口为 `ZOrderPinSuppressed(raisedIsFullscreen)` 谓词
  （toplevel_manager.h）。
- `host_tests/zorder_test.cpp` 新增（40 checks）：ZOrderGroupFor 8 组合边界、
  12 项混排排序全表（组/lane/item 逐项断言）、ZOrderPinSuppressed 真/假；
  Makefile test 目标追加（与 blit_clip_test 同模式）。
- 行为平价：输出序列逐元素一致（顺序/type/zIndex 编号/字段值/sub 指针身份）。
- 门禁：host_tests 全绿（含 zorder_test）；x86_64 + arm64-v8a hap 构建通过。

### 阶段 3A 精化（GetOccluders 遍历层列表，a2d142d，2026-08-29）

- `ZcBridge::GetOccluders` 由"独立扫描 toplevelZOrder + subsurfaceLayers_"
  改为锁内构建 `BuildLayerListLocked` 层列表后单趟遍历 — **锚 = ZC 父窗口
  Toplevel 层的 zIndex**（父==root 时锚 = Root 层 zIndex=0）；判定条件
  `layer.zIndex > anchorZ` 且跳过 Root/zcActive/不可见层；Toplevel 用
  fullscreen?整屏:X/Y/W/H，Subsurface 用 x/y+DisplaySizeAfterViewport(
  vpDstW/H, w/h)（layer.w/h 非显示裁剪尺寸，勿用）。pushRect/前置返回/锁
  语境不变。
- 语义等价对账（Python 复刻新旧算法逐场景）：单 ZC 全屏+任务栏、同父菜单
  attach 早/晚、其它窗口、连带 fullscreen、多 ZC 并存、**ProtocolOnly ZC 层
  （D4 修复：不再因层缺失返 0）**、**ZC 父==root（D3b 修复：锚=0 全扫）**、
  ZC 层自身 isExternal（D5 顺带修复）、maxOut 截断 —— 全部相等；
- 已知语义修正（提交注明，需 ZC 游戏遮挡回归）：D1 置顶菜单挂低 z-order
  普通窗口、D2 层父不在 z-order 但可见 → 旧不遮挡新纳入（与
  BuildLayerListLocked"菜单恒置顶"层序一致，属统一数据源后的自然结果）。
- 门禁：host_tests 全绿；x86_64 + arm64-v8a hap 构建通过。

### 阶段 3C（ZC 状态机收敛到 ZcBridge，20b4bdb，2026-08-29）

- ZC 发布/回退时序编排收敛到协议 owner（ZcBridge）：EglRenderer 三幂等
  方法（PublishZeroCopyActive/UnpublishZeroCopyReady/ClearZeroCopyCompositorKey）
  与三状态位（zeroCopyReadyPublished_/zeroCopyFallbackPending_/
  zeroCopyFallbackShmSerial_）下沉，改为 per-key 状态机
  `ZcBridge::publishStates_`（unordered_map<key, ZcPublishState>，多
  renderer 多 ZC 绑定隔离），六个幂等动作：
  `Activate`（=发布：先 compositor key 后 ready marker）/ `BeginFallback`
  （=撤 ready，基线仅有效性时更新）/ `ConfirmFallback`（shmSerial>基线才
  撤 key，返回 bool 供日志门控）/ `CancelFallback`（GPU 恢复取消）/
  `Release`（释放复位序列）/ `BindSurface`（attach 复位）；三查询
  （IsReadyPublished/IsFallbackPending/GetFallbackShmSerial）供日志与守卫。
- EglRenderer 保留生命周期/观测状态（registered/failed/hasFrame 等），
  五个旧调用点（成功帧/连续失败≥8/确认路径/恢复取消/释放）逐一改写为
  WaylandServer 头内联委托（ActivateZcSurface/BeginFallbackZcSurface/… 9 个，
  与 SetSurfaceZeroCopy 委托同模式）。`zeroCopyReadyPublished_` 等不再由
  渲染线程持有——日志文本/门控/顺序逐字保留。
- 行为平价：触发时机/幂等语义/时序不可合并（先 key 后 ready；fallback
  两步）/线程域（渲染线程调用，tmgr 锁边界不变）/日志全部不变；
  GraphicsBroker::SetZeroCopySurfaceReady 进程单例直调（原方式）。
- 门禁：host_tests 全绿；x86_64 + arm64-v8a hap 构建通过。

### 阶段 3 收尾状态（2026-08-29）

- 阶段 3 代码层全部完成（3A/3B 谓词+层序精化/3C/3D）。**待设备回归**：
  ZC 游戏（DXVK/OpenGL）遮挡、全屏、fallback 场景回归（响应 D1/D2 语义
  修正与 3C 状态机迁移）；x86_64 模拟器桌面链/notepad 回归不受影响。
- 已知未做（记录在案）：2B 任务 3（DirectPassPolicy 能力位）、wineboot
  滞留排查（Task #49）、3D 剩余（venus child PresentVenus 调度 wait 保持
  原样，超范围）。

### 阶段 4A（InputResolver 裁决闭环，2026-08-29）

PLAN §四阶段4 与 §2.2 指出的封装泄漏：`FindInputTargetAt` 返回半成品
`InputTarget`（originX/Y int + scale float + contentW/H 裸字段），调用方
`SendPointerEvent` 必须手写逆映射与 `ClampToContent` 补完 ——
"桌面坐标→surface 局部"的知识被拦腰切成两段。4A 收内 + 精度统一，行为平价。

**改动文件：**

- `compositor/geometry.h`（+35）：新增 `ClampToContent`（自 input_manager.cpp
  file-static 收进本模块，函数体与注释逐字平移——§2.5 补丁资产完整保留）与
  纯函数 `ComputeLocalPoint`（桌面逻辑→surface 局部逆映射 +
  内容区钳制，与合成侧 `FitMapX`/`FitMapLayerRect` 正变换严格互逆，同一未取整
  scale；头部注释指明与 dst 取整变体 `FitUnmapDisplayX` 精度规则不同不得混用）。
- `compositor/input_resolver.h`：`InputTarget` 改终态——新增 `localX/localY`
  （可注入的 surface 局部坐标，resolver 锁内已算好 + 已钳制）；`originX/Y`
  int→double、`scale` float→double（与 FitRect 精度规则对齐：origin 是整数
  屏幕原点 double 无损提升、scale 是未取整 double，不再 float 截断）；
  `swallow`/`contentW/H`/`toplevelId`/`surface` 保留。`FindInputTargetAt`
  签名 int→double（见下"命中/逆映射双精度"）。
- `compositor/input_resolver.cpp`：`FindInputTargetAt` 内把 lround 收进函数的
  `x/y`（命中判定用），各命中分支（7 处）就位 origin/scale/content 后调用
  `finalize()`（=`ComputeLocalPoint`）填 `localX/localY`；`scale` 赋值去 float
  截断（`transform.scale` 直存 double）。**全部命中/全屏/root/黑边/swallow
  判定与产出分支逐字保留**。
- `wayland_server.h`：:146 转发签名 int→double（同步）。
- `input_manager.cpp`：删除 file-static `ClampToContent`（改用 geometry.h 同名
  函数，PC 分支 :457-458 调用点不变）；`SendPointerEvent` 桌面分支删手写
  `(logical-origin)/scale + ClampToContent`（:434-437），改注入
  `target.localX/localY`（wl_fixed 转换在注入时做）；TARGET 日志 origin 占位
  改 `%.1f`（double），字段与诊断语义不变。`lround` 调用点随之在
  input_manager 消失（移至 resolver 内）。
- `host_tests/geometry_test.cpp`（+67）：新增 3 组用例——11 ClampToContent
  8 例（content<=0 不钳 / 钳 [0,content-1] / content=1 退化）；12 ComputeLocalPoint
  恒等/整数 fit/黑边钳制/非整数 fit 与 FitMapX 逐点互逆；13 **4A 精度对账**
  特征化（1400x920→800x600 fit：float 截断确实有损；抽样 local 域新旧值
  最大偏差 < 800·2^-23 且 < wl_fixed 半格 1/512）。

**设计决策（精度统一）：** 不把 InputTarget 强套 `FitRect` —— FitRect 是
letterbox 显示几何（src/dst/off/scale 四元组），其 src/dst 只在 fit 映射
（全屏窗口）有定义；普通窗口命中（scale=1 恒等，content=0 不钳制）与 root
回退没有 fit 语义，强行构造退化 FitRect 需要为 content（src 尺寸）引入
新语义（0=不钳制既有契约会破裂）。因此保留显式 `double originX/Y` +
`double scale` + `int contentW/H` 字段——精度规则与 FitRect 一致（origin 为
FitRect offX/offY 无损 double 化、scale 为 FitRect scale 同源未取整 double），
换算数学收为 geometry.h 纯函数。此取舍理由已写入 input_resolver.h 注释。

**对账结论（旧 :437-441 手写式 vs 4A 收内，逐场景）：**

| 场景 | 旧 | 新 | 一致性 |
|---|---|---|---|
| 普通窗口/subsurface 命中（scale=1, content=0） | local=logical-origin，不钳 | ComputeLocalPoint 同公式同顺序，content=0 不钳 | origin 为原 int 无损转 double，**逐点相等** |
| 全屏内容区命中（fit origin/scale/srcW/H） | (logical-origin)/scale_float | 同公式，scale 为 double 全精度 | Δlocal ≤ \|local\|·2^-23（host_tests 13 量化），wl_fixed 转换绝大多数位一致；仅在 local 落入 1/256 格边界 ±ε 内时差 1 定点单位（0.0039px），方向为消除旧 float 截断误差（与合成侧未取整 scale 互逆更精确），无语义变化 |
| 全屏黑边 swallow（PRESS 吞 / MOVE/RELEASE 钳透传） | 调用方 clamp（同函数同 content 值） | resolver 内 clamp（同函数同 content 值 = transform.srcW/H） | **逐点相等** |
| 桌面合成/快进（root 恒等） | origin=0 scale=1, content=0 | 同（root 回退/root 自身命中） | **逐点相等** |
| 直传（红警2 修复点） | 锚=root 逻辑尺寸：contentW/H=transform.srcW/H（ZC→层几何、SHM→buffer），CoordTransform 锚 2B 契约 contentW/H=root 逻辑尺寸 | contentW/H 来源路径未动（ComputeFullscreenFitLocked 同一实现），逆映射在桌面逻辑系上做 | **逐点相等**（红警2 修复不回退） |
| PC 分支（不走 FindInputTargetAt） | file-static ClampToContent(wx, lb.srcW/H) | geometry.h 同名函数逐字相同 | **逐点相等**（纯平移） |
| 命中判定坐标 | 调用方 lround(logicalX) 传 int | resolver 内 lround(logicalX)（内部转换），napi `FindToplevelAt(int)` 内 int→double→lround 恒等 | **逐点相等** |

**门禁：** `make test` host_tests 全绿（geometry_test 71 checks / 0 failures；
blit_scaled 402 / blit_clip 38 / zorder 40 / env 21+88 / 全 0 failures）；
`make NATIVE_ARCH=arm64-v8a hap` 构建通过（HAP 374M 签名完成，构建后还原
build-profile.json5，工作树清洁）。**设备回归待做**（arm64 Pad 真机 / 模拟器
输入链路）：红警2 直传点击路由、PAL2 相对模式点击/移动、war3 光标、桌面任务栏
交互——属 4A 最终验收，同前几阶段由子代理归来后的冒烟+设备回归承担。

**遗留：** 4B（InputManager 拆 InputQueue/InputStateTracker/InputInjector/
InputSpaceMapper）、SendScrollEvent 缺段疑似实 bug（按 PLAN 单独提交标注行为
变化）、两处 `IsDesktopMode()` 改 `Policy()`；`InputTarget` 的 origin/scale/
contentW/H 字段保留作诊断（TARGET 日志断点），若后续无用可并入 4B 清理。

## 三、2B 剩余工作清单

1. **接上消费侧（解编译断点，完成任务 1）** — ✅ 已提交（0d06926）
   - 实现 `EglRenderer::GetInputLetterbox()`：按头文件注释语义，用最近一帧
     契约的 contentW/H 对当前 surface 做 ComputeFitRect；无帧/失败退回
     显示 letterbox。渲染器需缓存最近一帧的契约元数据（kind/baseSpace/
     contentW/H/opaque）。
   - RenderLoop 取帧改新签名；`frameArgb_` 改取 `frame.opaque`（对账旧逻辑
     `GetToplevelShmFormat == 0` → `!opaque`，产出侧 `opaque = ShmFormat != 0`
     已保证等价）。
   - input_manager.cpp CoordTransform 改调 GetInputLetterbox，删绕路重算；
     对账基准：桌面合成/快进/直传三路径的逆映射结果与旧实现逐点一致
     （直传路径正是红警2 修复点，旧逻辑 ~162-178 的注释保留了当时语义，
     新实现必须等价）。
2. **任务 2：PC/Desktop 显示策略拆分** — ✅ 已提交（1902862）。现状：`TakeToplevelFrame` 编排壳里
   靠 `policy_.RootCompositing() && id == desktopRootToplevelId_` 分流 PC
   （TakeWindowFrameLocked + BlitWindowSubsurface）与 Desktop 路径。把取帧
   路径选择下沉为 DisplayPolicy 多态，分流条件逐点等价，编排者只问策略要帧。
3. **任务 3：SHM 直传能力协商** — ✅ 已提交（61b1d84，2026-08-29）。
   `TryShmFullscreenDirectLocked` 的直传正确性前提（uForceOpaque/无
   GL_BLEND/fit 与 CPU 同源/XRGB 帧不透明）从注释假设收口为
   `compositor/direct_pass_policy.h` 能力位（DirectPassCapabilities 三枚举 +
   kDirectPassCapabilitiesAll + DirectPassPolicy 接口），EglRenderer 实现
   声明（恒全备，能力来源逐条注释），FramePlanner 锁内经
   PluginManager::GetRendererForToplevel 查询（渲染器不可查时不拦截=与旧
   行为一致）。决策结果逐点等价；pad 设备游戏回归正常。
4. **验证门禁**：每项 `make test`；全完后 `make NATIVE_ARCH=x86_64` 全量
   构建（构建后 `git checkout -- entry/build-profile.json5` 还原）；随后
   模拟器冒烟门禁 + 后台 arm64 构建门禁（同前几阶段）。
5. **性能对比（延期项）**：2A tip vs 2B tip 各临时翻 FrameTraceEnabled
   默认开构建测一次再还原，结果记录到本文档，不进提交。
6. **提交纪律**：每项独立提交，中文信息注明"行为平价（重构第 2B 步）"；
   3461a1d 是中断现场，续作完成后如需可用后续提交修复衔接，不必改写历史。

## 四、阶段 3-6 规划（摘要，详见 PLAN §四）

顺序不可重排（依赖关系见 PLAN "阶段顺序的依赖关系"）：

- **阶段 3 ZC 与层序政策收口**：**完成（见 §二，2026-08-29）**——3A（zc_bridge
  抽离 + 精化 GetOccluders 遍历层列表）、3B（zorder_policy 三散点 + 层序
  显式化 ZOrderSeq）、3C（ZC 状态机收敛到 ZcBridge）、3D（presenter 收编 +
  PresentTarget 统一）全落地；**待设备回归**：ZC 游戏遮挡/全屏/fallback 场景
  （3A 的 D1/D2 语义修正与此项绑定）。行为敏感点验证清单见 §二 3B/3A 小节。
- **阶段 4 输入栈拆分**：4A InputResolver 裁决闭环**完成**（见 §二 4A 下标，
  PLAN §2.2 封装泄漏收口 + InputTarget 精度 double 化 + ClampToContent 收
  geometry.h）。剩余 4B：修 SendScrollEvent 缺段疑似实 bug（单独提交并标注
  行为变化）；InputManager 拆 InputQueue/InputStateTracker/InputInjector/
  InputSpaceMapper；两处 IsDesktopMode 改 Policy()；InputTarget origin/scale/
  contentW/H 诊断字段去留。行为敏感：需 PAL2/war3/RA2 输入回归（arm64 Pad，
  需用户配合）。
- **阶段 5 协议层重构**：wl_core 拆协议壳 + CommittedSurface 快照；
  PopupManager；maximized 迁入 ToplevelState；ToplevelEventBus 事件 enum 化。
- **阶段 6 facade 瘦身**：WaylandServer 拆 24 个转发；DesktopSessionState
  POD；desktopRootFrameSerial_ 改 atomic。

## 五、续作执行要点

- **执行模式（已定型）**：每阶段委派一个 coder 子代理，brief 给足
  文件:行号证据、行为平价红线、补丁对账要求、提交纪律（每项一提交、
  中文信息注"行为平价"、不 push、不动 thirdparty/）；子代理回来后做
  模拟器冒烟门禁 + 后台 arm64 构建门禁，全过才进下一阶段。
- **构建纪律**：只能 `make NATIVE_ARCH=x86_64` 或 `arm64-v8a`（Makefile
  唯一手段，串行）；构建会改写 entry/build-profile.json5，构建后必须
  `git checkout --` 还原；构建产物是同一个 HAP 文件，arm64 构建会覆盖
  x86_64 产物，部署前 `unzip -l` 验架构；**构建运行期间绝不能动源码树**。
- **模拟器**：`hdc -s 192.168.1.3:8710 list targets` → `-t 127.0.0.1:5555`。
  冒烟：force-stop → EntryAbility → 等 ~20s → DesktopAbility → 等 ~22s →
  snapshot_display（.jpeg）拉回看图。成功基线：蓝色桌面 + 左下角中文
  "开始"任务栏；notepad 直启基线：中文"（未命名）- 记事本"窗口。
- **已知预存缺陷（勿误判为回归）**：
  1. 首启时序竞争——DesktopAbility 的 XComponent 若先于引擎就绪创建，
     renderer 永不创建（日志 `MoveRenderer old tl=0 NOT FOUND`、
     `[WL-STAT] renderers=0`）→ 黑屏；引擎就绪后重开 DesktopAbility 即恢复。
  2. arch 交叉构建致 wine-data.zip 漂移触发设备端 "upgrade pending"
     拒绝启动 wine——模拟器上卸载重装即可。
  3. nativespawn 的 wine 子进程逃逸 app 生命周期，残留 wineserver 占
     prefix 致启动失败——模拟器终极手段 `reboot`。
- **hilog 坑**：`hilog -r` 是清空 buffer 不是读取；读法 `hilog -x -t app`
  重定向后 file recv；`-D A00000` 参数不认，别用。

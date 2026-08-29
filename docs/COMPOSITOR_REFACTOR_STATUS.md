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
| 4B SendScrollEvent 缺段修复（行为变化例外） | 完成（门禁全过，见 §二 4B 下标） | 1c80f0d |
| 4C InputManager 拆层（4C1 坐标收口/解环/Policy 改名 → 4C2 拆 Queue/StateTracker/Injector + enter/leave 收敛 + host_tests） | 完成（门禁全过，见 §二 4C1/4C2 下标） | 4C1 3950980；4C2 0f7c6bf（StateTracker+测试）/ 479cccd（Queue+Injector+编排瘦身）/ 23fff1d（顺手项+台账） |
| **阶段 4（输入栈拆分）** | **代码层全部完成（4A/4B/4C1/4C2），待设备回归（清单见 §四 阶段 4 验证段）** | — |
| 5 协议层重构 | **代码层全部完成（5A1/5A2/5B1/5B2/5C/5D 落地，待设备回归）** | 5A1/5A2 前序记录（§二 5A1）；5B1 §二 5B1 下标；5B2 §二 5B2 下标；5C §二 5C 下标；5D §二 5D（本分支 HEAD 提交） |
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

### 阶段 4B 步（SendScrollEvent 缺段修复，1c80f0d，2026-08-29）

PLAN 红线明确的两个行为变化例外之一：SendScrollEvent 疑似实 bug，允许行为
变化、单独提交并逐一标注。修复内容是**生产侧（SendScrollEvent）**缺段对齐；
**注入端（InjectPointerAxis）不改**（诊断结论见下）。

**改动文件：**

- `entry/src/main/cpp/input_manager.cpp` 仅 `SendScrollEvent`（+109/-6）：
  可见性抑制、桌面/PC 双分支目标解析、ClampToContent、move-grab 丢弃、
  lastGlobalPtr 维护、enter/leave 升 surface 级（全部与 SendPointerEvent 对应
  分支或 SendKeyEvent 对齐，见对账表格）。

**行为变化清单**（每项: 场景 → 修复前 → 修复后 → 理由）：

1. **桌面模式目标解析（核心修复）** — 桌面模式指针悬于非 root 目标上滚轮：
   修复前 `CoordTransform(px,py,tl)` 无法按窗口 id 查到 renderer（桌面模式只有
   root 渲染器）→ `GetAnyRenderer` 兜底得到**桌面逻辑坐标**，却以窗口局部
   坐标语义进 enter → wine 光标被设置到偏移窗口原点/未经 fit 缩放的位置；
   axis 无位置属性（wl_pointer 协议不含坐标），实际落到 wine 光标当前所在
   窗口，可作用在错误窗口。修复后与 SendPointerEvent 桌面分支同构：root
   CoordTransform + FindInputTargetAt（4A 终态 `localX/localY`，逆映射+fit+
   钳制收内）→ enter 被命中 surface + 正确局部坐标，axis 在同批 flush 的
   enter 之后注入，落点正确。**理由：对齐 SendPointerEvent 桌面分支，消除
   PLAN §2.3 指出的"scroll 缺桌面→surface 局部转换"疑似实 bug**。
2. **PC 模式坐标钳制补齐 ClampToContent** — 全屏 letterbox 黑边/窗口边缘外
   的越界滚动坐标此前全文件唯一不钳制路径，直接进 enter 把 wine 光标放到
   黑边/屏幕外。修复后与 SendPointerEvent PC 分支同款钳制到 [0, content-1]。
3. **可见性抑制** — 最小化/不可见窗口上的滚动此前绕过 `toplevelVisible_`。
   修复后与 SendPointerEvent/SendKeyEvent 一致抑制（抽样日志 120:1）。
4. **enter/leave 升 surface 级** — 桌面模式滚动落在与父窗口同 toplevelId 的
   菜单 subsurface 上：修复前只比较 toplevelId，focused 为父窗口 surface 时
   不重新 enter，axis 继续喂父窗口；且 enter 用的是 `GetSurfaceForToplevel(tl)`
   = 父窗口 surface（菜单伸出父窗口边界，其越界局部坐标会被 winewayland
   motion clamp 夹回窗口内）。修复后与 ACT_MOVE 同纬：surface 级比较
   （`pointerFocusedSurface_ != targetSurf`）+ enter 命中层自己的 surface；
   非桌面/命中失败路径保持 toplevel 级（行为未变）。
5. **move-grab 期间滚动丢弃** — xdg_toplevel.move 拖拽进行中滚动此前照常
   注入（axis 落在拖拽窗口上）。修复后 `IsMoveGrabActive() &&
   GetMoveGrabToplevelId()==tl` 时丢弃（采样日志 SCROLL-DROP）。**理由：
   拖拽由 compositor 接管，不向 client 派输入事件（SendPointerEvent 同款
   语义）；axis 无绝对定位等价事件，丢弃比注入安全。**
6. **lastGlobalPtr 维护** — 滚动后立即发起拖拽时，grab 偏移基准此前用的是
   陈旧值（若滚动前无 MOVE）。修复后桌面分支存 root 逆映射后的桌面逻辑
   坐标、PC 分支存 wx+tlGeo（与 SendPointerEvent 同款），grab 基准新鲜。
7. **目标不可用回退路径语义变化** — FindInputTargetAt 返回 false（root
   surface 都不可用，仅异常时序）时走父窗口相对坐标（同 SendPointerEvent
   TARGET-FALLBACK），此前是桌面逻辑坐标直进 enter。

**保留（主动不变化）：**
- scroll 不产生 REL_MOTION → 不动相对增量基线 `lastLocal_`（与 ACT_MOVE 的
  enter 不以 enter 位置更新基线一致；PRESS 的基线更新是定位专用语义）；
  scroll NAPI 无 fromMouse 位，不引入 PRESS 的 skipEnter（其守卫对象是"冻结
  点的恒定坐标"，scroll enter 位置是真实指针位置，且 ACT_MOVE — 本修复的
  对齐基准 — 同样不 skip）。
- §2.5 输入相关补丁（rawDelta ±512 钳制、PAL2 点击脉冲拉伸、skipEnter 同
  surface 守卫、PRES-DEFER 延迟判定等）触发条件全部未动，本修复只改
  SendScrollEvent 单函数。

**注入端与 pointer 资源结论（诊断）：**
- pointer_resources 构成：`Seat` 的 vector 是每 client 一支 wl_pointer —
  wine 进程组里每个连到 compositor 的进程按 process_wayland 各开一条连接并
  `wl_seat_get_pointer`（winewayland.drv wayland.c:69/80），典型构成随会话
  有 wineboot/explorer/应用等多支；每支有独立 `focused_hwnd`（wayland_pointer.c
  pointer_handle_enter :195-224）。
- **axis 广播不构成 4-B 独有的 bug**：InjectPointerAxis（:999-1022）与
  InjectPointerMotion/InjectPointerButton 分发机制完全一致（同一
  GetAllPointerResources 循环、无 client 过滤、同样 frame 兜底）。wine 侧
  `pointer_handle_axis`（wl_pointer axis 本体）是空函数，实际滚动走
  axis_discrete → axis_value120（:337-368）且 `wayland_pointer_get_focused_hwnd()`
  判空返回 — 只有收到过 enter 的进程才消费 axis。注入端 enter 带 surface 的
  client 过滤（InjectPointerEnter :946-951），故广播最终只被正确进程消化。
  结论：**不改注入端**；`ev.tl` 不读 = 无害冗余，保留并注释为诊断字段。
- 由此路由正确性的全部杠杆在生产侧：**enter（+其坐标）必须先行且指向命中
  surface** — 本修复的 1/4 项即时此意。Wayland wl_pointer 无 axis+位置事件，
  这是协议设计使然，不要试图把坐标塞进 axis。

**门禁：** `make test` host_tests 全绿（geometry 71 / blit_scaled 402 /
blit_clip 38 / zorder 40 / env 21+88，全 0 failures，本提交未改测试代码 —
受影响数学已由 4A geometry_test 覆盖）；`make NATIVE_ARCH=arm64-v8a hap`
构建+签名通过（HAP 374M，构建后 `git checkout -- entry/build-profile.json5`
还原，工作树仅剩预存 thirdparty/ 改动）。

**遗留（设备回归请求）：** 桌面模式 fit 缩放窗口内滚轮（PLAN 阶段 4 验证点）、
菜单/任务栏弹出层滚动路由、PC 模式窗口滚动+黑边滚动、全屏游戏（war3/RA2/
PAL2）滚动、拖拽窗口中滚动（应无事件）。关注日志 tag `WL_Input` 的
SCROLL-TARGET/SCROLL-ENTER/SCROLL-DROP/FALLBACK 与 [PIPE] scroll 输入侧。

### 阶段 4C1（InputSpaceMapper 坐标收口 + PointerExtras 解环 + IsDesktopMode→Policy，2026-08-29）

PLAN §四阶段4 三件事的落地：InputSpaceMapper 模块抽离（§2.2 renderer 查找
泄漏收口 + §2.4 lastGlobalPtr 双语义显式化）、PointerExtras↔InputManager 双
向依赖单向化、input_manager.cpp 仅存的两处 IsDesktopMode 真策略分支改
Policy() 命名查询。全部结构收口，行为平价。

**模块边界设计（一句话版）：** InputSpaceMapper = 输入坐标空间映射单点
（renderer 查找 fallback + letterbox 逆映射 + lastGlobalPtr 显式语义），
InputManager 只依赖其公开接口且不再认识 PluginManager；PointerExtras 的
warp 位置同步改经注册表回调注入，不再 include input_manager.h。

**改动文件：**

- `compositor/input_space_mapper.{h,cpp}`（**新增**）：坐标变换收口。
  - `ResolveRendererFor(tl)`：renderer 查找 fallback 链（tl → RootCompositing
    下 root → GetAnyRenderer）自 PluginManager 取值 — PLAN §2.2"渲染器登记
    结构泄漏到输入模块"收口，InputManager 不再认识 PluginManager。
  - `CoordTransform(...)`：原 `InputManager::CoordTransform` 函数体逐字搬移
    （2B 契约化 GetInputLetterbox 锚点、FitUnmapDisplayX/Y 逆映射、120:1
    抽样 INFO 日志 — LOG_TAG 保持 `WL_Input`，诊断 grep 基线不回退）。
    `InputManager::CoordTransform` 保留公开委托（napi_init.cpp 调用者不变）。
  - `UpdateGlobalPtr/ResetGlobalPtr/GetGlobalPtrX/Y/GetGlobalPtr`：lastGlobalPtr
    双语义显式化 — `GlobalPtrState::Space{Desktop, Window}` 标签；X/Y 保持
    两个独立 `std::atomic<wl_fixed_t>`（写序先 X 后 Y、读方两次独立 load，
    与旧实现逐点一致，不合成单 atomic 结构体收紧既有竞态窗口）；space 标签
    纯语义诊断、无算法消费方。
- `input_manager.h`：删 lastGlobalPtrX_/Y_ 字段；`GetLastGlobalPointerX/Y`
  改委托 `InputSpaceMapper`（public 签名不变 — wayland_server.cpp:181
  StartMoveGrab 调用方不动）；include input_space_mapper.h。
- `input_manager.cpp`：删 `#include "plugin_manager.h"`；CoordTransform 改
  公开委托；`SendPointerEvent`/`SendScrollEvent` 4 处 `lastGlobalPtrX_.store`
  对改 `UpdateGlobalPtr(...)`（desktop 分支标 Desktop 空间、PC 分支标 Window
  空间，值与写序逐点一致）；`OnPointerWarp` 2 处 store 改 UpdateGlobalPtr
  （desktop 分支 Desktop 标签、PC 分支 Window 标签 — surface 局部原值，4C1
  只重标不修正该历史语义）；`ResetSessionState` 改 `ResetGlobalPtr()`。
- `pointer_extras.{h,cpp}`：新增 `SetPointerWarpSink(std::function<...>)` +
  私有 `warpSink_`；`:168`（Lock 约束销毁 hint 路径）与 `:206`（warp 请求，
  两处均在 Wayland 线程）改经 sink 转发；**删 `#include "input_manager.h"`**。
- `wl_core.cpp`：`RegisterWlCoreGlobals` 在 `PointerExtras::Register(display)`
  之后注入 `InputManager::OnPointerWarp` 转发 lambda（装配点）。
- `CMakeLists.txt`：源列表追加 `compositor/input_space_mapper.cpp`。

**解环方案（谁不再 include 谁、装配点）：** pointer_extras.cpp 不再
include input_manager.h；装配在 `wl_core.cpp RegisterWlCoreGlobals`
（Server Start 阶段、wl 事件循环启动前）一次性注入，之后回调只在 Wayland
线程读 → 无锁（与 wayland_server.h SetStateCallback 同模式，未新增锁）。
InputManager→PointerExtras 方向（`HasRelativePointer`/`SendRelativeMotion`）
保持 include — 单向依赖成立。

**IsDesktopMode→Policy 对账：** input_manager.cpp 仅存两处（OnPointerWarp
:199 策略分支 + :211 日志行），改为 `ws->Policy().CompositorRoutesInput()`
（同值谓词；"desktop 才做 surface 局部→桌面坐标换算"即输入自路由语境）。
日志文本 `WARP pos=(...) desktop=%d`（:211）保持逐字，desktop=0/1 位同值。
全文件已无其他 IsDesktopMode 调用（grep 佐证）。

**行为平价对账：** CoordTransform 函数体逐字搬移（含全部注释/日志/抽样
static 起点一致—所有调用方汇入同一新函数体）；OnPointerWarp 分支条件
同值、failure return 路径不变、store 值不变（新增第三字段 space 是无旧
观察方的诊断标签）；ResetSessionState 清零顺序不变（modifiers 后、相对
增量基准前）；4 处 UpdateGlobalPtr 值/写序逐点一致；ptr 扩展开关、rawDelta
±512、PAL2 脉冲拉伸、relSkipEnter 守卫、PRES-DEFER、4B scroll 修复逻辑
零触碰。全局 grep：`lastGlobalPtr` 仅剩 mapper 与委托处、`IsDesktopMode`
在 input_manager.cpp 仅剩注释、`InputManager::OnPointerWarp` 为唯一入口
（wl_core 装配 lambda 转发）。

**门禁：** `make test` host_tests 全绿（geometry 71 / blit_scaled 402 /
blit_clip 38 / zorder 40 / env 21+88，全 0 failures，本提交未改测试代码）；
`make NATIVE_ARCH=arm64-v8a hap` 构建+签名通过（HAP 374M，构建后
`git checkout -- entry/build-profile.json5` 还原，工作树仅剩预存
thirdparty/ 改动）。**设备回归待做**：同 4A/4B — arm64 Pad 真机
（往返/滚动/游戏输入）验证，本提交无任何运行时状态语义变化。

**4C2 预留说明：** InputSpaceMapper 只收坐标知识（协调器边界清晰）：
wl_*_send_* / PointerExtras 状态 / 队列与焦点追踪全部留在 InputManager。
4C2 拆 Queue/StateTracker/Injector 时的接口面 = `InputSpaceMapper` 公开
方法集（ResolveRendererFor/CoordTransform/UpdateGlobalPtr/ResetGlobalPtr/
GetGlobalPtrX/Y/GetGlobalPtr）+ 已公开的 Inject*/Enqueue/InputEvent — 无
隐藏依赖。已知 4C2 清理项（本步刻意未做）：`InputTarget` 的
origin/scale/contentW/H 诊断字段去留；enter/leave 三份变体收敛（
ACT_PRESS/ACT_MOVE/SCROLL-ENTER 同构见 §二 4B 记录）；`WaylandServer`
转发的 CoordTransform（napi_init.cpp FindToplevelAt 调用点）可一并改为
直呼 mapper（4C2 顺手项）。

### 阶段 4C2（InputManager 核心拆层，2026-08-29）

PLAN §四阶段4 最后一段：InputManager 拆四层 —
`InputQueue`（队列+pipe+去重）/ `InputStateTracker`（纯状态，可宿主机
单测）/ `InputInjector`（唯一碰 wl_*_send_*）/ `InputSpaceMapper`（4C1
产物，只消费不重建）；InputManager 留为薄编排门面。**对外公开接口
（InputManager 全部 public 方法与单例 GetInstance）保持不变** —
napi_init/wl_core/seat/wayland_server 等 30+ 调用点零改动（仅实现重排）。

**分层结构总览（每层职责一句话）：**

| 层 | 文件 | 职责 | wayland 依赖 |
|---|---|---|---|
| InputManager | input_manager.{h,cpp}（1222→~700 行） | 薄编排：NAPI 入口策略分流（坐标变换/目标解析/焦点判定/enter-leave 三语义变体）+ FlushQueue dispatch（去重批→注入器）+ 公开委托 | 有（签名用） |
| InputQueue | compositor/input_queue.{h,cpp} | 队列机制：queue+mutex、pipe 唤醒、wl_event_source 注册、Poll（锁内 swap + 去重）、FlushClients | 仅 .cpp（wayland-server-core.h 事件循环） |
| InputStateTracker | compositor/input_state_tracker.{h,cpp} | 纯状态：按钮位掩码/修饰键/pointer+keyboard 焦点/可见性抑制/serial/相对增量基线/最近按下时刻 | **零**（wl_resource 前向声明只存指针；host_tests 直连编译） |
| InputInjector | compositor/input_injector.{h,cpp} | 唯一 wl_*_send_* 注入：Enter/Motion/RelativeMotion/Button/Axis/Leave + Keyboard 全系列；注入前防御（surface 存活/资源空 DROP/client 过滤）；丢帧统计 | 有（协议头，注入层专属） |
| InputSpaceMapper | compositor/input_space_mapper.{h,cpp} | （4C1）坐标变换单点 + lastGlobalPtr 显式语义 | 不动 |

**上报值范围口径：** InputManager 不透明句柄（wl_resource*）只在前三层间
传递身份；tracker 只存指针做比较（`PointerFocusedSurfaceIs`），绝不 deref
或调 wl_resource_get_* — 这是宿主单测可行的关键。

**InputStateTracker 宿主单测（host_tests/input_state_test.cpp，TDD 先
红后绿，84 checks / 0 failures）：** 覆盖 11 组 —
1. IsModifierKey 转换（shift/ctrl/alt/super/caps/num 10 keycode 真 + 3 非
   修饰键假）；
2. UpdateModifiers depressed 位（左右 Ctrl 共享 bit2、非修饰键 no-op、
   latched/group 恒 0）；
3. Caps/Num toggle（press 翻转 locked、release 不翻、二按清除）；
4. 按钮位掩码（ButtonToBit/BitToButton 双向 + unknown 99→0、press 置位/
   unknown 忽略、release(0) 弹首个按下位/未按下指定键返回键码/空掩码返 0）；
5. pointer 焦点转移（无焦点→enter→leave→clear 全字段、PointerNeedsEnter
   组合语义、re-enter skip 判定 = PointerFocusedSurfaceIs 相同 surface）；
6. keyboard 焦点转移（SetKeyboardFocus/ClearKeyboardFocus 三元组清全）；
7. 可见性抑制边界（未登记放行、登记 true/false、多窗口隔离、ClearVisible）；
8. serial 递增（初值 1、单调 +1、跨复位不清零 — 协议串号唯一性合同保留）；
9. 相对增量基线（空→更新→覆盖→重置语义）；
10. 最近按下时刻（脉冲拉伸计时值）；
11. ResetSessionState 组合顺序特征化（复位后 serial 继续单调）。

**queue/注入器边界决策理由：** 去重归 **InputQueue::Poll** 而非 Injector —
去重只比对事件序列自身的 type/tl/surface/坐标（`PTR_MOTION 只留最后一
个坐标`、`PTR_BUTTON/KBD_KEY 同键同状态合并`、`PTR_ENTER 同 tl+surface
合并`、`PTR_AXIS 不去重（累积量不可丢）`、`PTR_LEAVE/KBD_* 不去重`），
不读任何 wayland 资源/客户机状态，与"锁内 swap 取批"同属队列批量语义；
Injector 保持"每事件一注入"的纯粹协议层（未来 dispatch 复用/测试不受
去重干扰）。dedup 日志（`[Input] dedup N→M`）随 Poll 平移，文本不变。
事件不再私嵌 InputEvent 类型，改 InputQueue::Event（wl_fixed_t x/y 以
等价 int32_t 承载，头文件不 include wayland）。

**Enter/Leave 三变体收敛方案：** 收敛点 = InputManager 编排层私有
`SubmitEnterLeave(tl, targetSurf, surf, x, y)`（放编排层而非 StateTracker
的理由：leave/enter 入队与 needLeave 判定依赖 GetSurfaceForToplevel/
Enqueue/日志环绕，且三调用点日志文本（PRESS-ENTER / MOVE-ENTER try+
enqueued OK / SCROLL-ENTER try+enqueued OK）与 surf 选定时机不同，无法
整体内聚到 tracker 而不改变日志顺序 — 红线禁变）。helper 只收敛"确认要
enter 后的共同动作"：needLeave 双判据（targetSurf 非空=surface 级：已有
聚焦 surface≠surf → leave；为空=toplevel 级：已有聚焦 tl≠tl → leave）→
PTR_LEAVE → PTR_ENTER 入队序。**needEnter 判定与 skipEnter 守卫不收敛**
（语义不同）：ACT_PRESS = `pressTargetSurf && !skipEnter`（每次点击强制
重定位 enter；skipEnter = fromMouse+相对模式+同一 surface 三重守卫，
跳过时也不发运动）；ACT_MOVE/SCROLL-ENTER = `NeedsPointerEnter() ||
focused 不同于目标`（先判再 enter，仅过渡时发）；三调用点逐字保留原
判定表达式与日志点。对账：PRESS 的基线更新（UpdateLastLocal 在 enter 后）
与 !skipEnter 才发 PTR_MOTION、MOVE/SCROLL 的 "try"/"enqueued OK" 日志
位置（与旧 if(surf) 块内）逐字一致。

**状态迁移对账（旧字段→新归属）：** pressedButtons_/ButtonToBit/
BitToButton/UpdateModifiers/IsModifierKey → tracker（含 `releaseBtn` 初值
=传参 button 的"未按下的指定键也入队 RELEASED"既有语义不修正）；pointer
焦点三原子（tl/surface/enterSerial）+ serial_ → tracker（NextSerial 取号
为旧 serial_++，InjectButton 的 enterSerial 复用语义不变）；keyboard 焦
点三元组 → tracker；visibleMutex_+toplevelVisible_ → tracker（同款 mutex
锁边界）；lastLocalX/Y/hasLastLocal → tracker（仅 NAPI 线程，无锁不变）；
lastPressMs_ → tracker（atomic 保留）；`display_` 随队列走（Initialize
注入的 WaylandServer 事件循环 display，FlushQueue 尾 wl_display_flush_
clients 委托 Queue::FlushClients）。ResetSessionState 清序逐字（pointer→
keyboard→buttons→modifiers→mapper ResetGlobalPtr→基线→press→可见性）。

**线程/锁边界：** tmgr 锁不新增接触；队列锁只在 InputQueue（Enqueue*
锁内 push+锁外 pipe、Poll 锁内 swap，与原逐字）；Injector 经构造注入的
tracker 指针（InputManager 持成员对象，只读共享 — 写方仍是原两个线程，
atomic 属性不变）；pipe 回调（Queue::OnPipeReadable）经事件循环启动前
注入的 flush 回调（lambda [this]{FlushQueue()}）回编排层，与 4C1 warpSink
同模式，无新锁。

**顺手项（4C1 遗留）：** `InputTarget` 的 contentW/contentH 确认为零消费
读点（全库仅 resolver 内 finalize 传给 ComputeLocalPoint；TARGET/
SCROLL-TARGET 日志只读 origin/scale/swallow/local）→ 删除字段，改口
resolver 内局部变量传纯函数（行为不变）；originX/originY/scale 因日志仍
输出而保留（注释更新：仅诊断不再参与换算）。4C1 预列的 `WaylandServer`
转发 CoordTransform→直呼 mapper 项**保留委托不直呼**（红线：公开接口不
变，napi_init.cpp 调用点零改动）。

**门禁：** `make test` host_tests 全绿（geometry 71 / blit_scaled 402 /
blit_clip 38 / zorder 40 / env 21+88 / **input_state 84**，全 0 failures）；
`make NATIVE_ARCH=arm64-v8a hap` 构建+签名通过（HAP 374M，libs/arm64-
v8a/，工作树仅剩预存 thirdparty/ 改动）。

**遗留（设备回归请求，阶段 4 统一汇总见 §四）：** 本提交无运行时时序/
语义变化（结构重排+纯搬移+日志文本不变），但输入栈是设备重度敏感区 —
arm64 Pad 真机回归清单：4A 直传点击路由（红警2 主菜单）+ 全屏黑边点击
（RA2）、4B fit 缩放窗口内滚轮 + 菜单/任务栏弹出层滚动（SCROLL-TARGET/
SCROLL-ENTER/SCROLL-DROP 日志）、4C 全局输入行为（PAL2 相对模式点击/
移动 + war3 光标 + 桌面任务栏交互 + 触摸 tap + PC 模式多窗口）。监视
tag：WL_Input（[Input] 全组日志顺序与基线一致）+ [PIPE/N] 无丢帧。模拟器
冒烟基线（蓝色桌面+「开始」任务栏、notepad 直启）待回归时复核。

### 阶段 5A1（ShmFrameSource 纯函数抽离，2026-08-29）

PLAN §四阶段5 第 1 条的前半：wl_core.cpp 的 SHM 拷贝/缩放/内容区计算
知识抽为纯函数模块 `compositor/shm_frame_source.{h,cpp}`，可进 host_tests。
这是阶段 5 的第一块（协议拆壳前先落地的纯函数部分）。

**抽离函数清单（原 wl_core.cpp 行号 → 新模块签名）：**

| 原函数 | 原行号 | 新签名（shm_frame_source.h） | 形式 |
|---|---|---|---|
| `CopyShmContentTight` | :457-466 (file-static) | `void CopyShmContentTight(const ShmCommitInfo& fi, std::vector<uint8_t>& dst)` | 纯函数，逐字搬移 |
| `CopyToplevelContent` | :472-499 (file-static) | `void CopyToplevelContent(int32_t vpDstW, int32_t vpDstH, ShmCommitInfo& fi, std::vector<uint8_t>& dst)` | 纯函数；`SurfaceData* sd` 值语义参数化为 `vpDstW/vpDstH`（函数体仅 `sd->vpDstW/H` → 参数，含 vpDst 缩放 clamp/不 clamp 与 BRGA 保通道注释逐字） |
| `CopyShmBufferTight` | :502-510 (file-static) | `void CopyShmBufferTight(const ShmCommitInfo& fi, std::vector<uint8_t>& dst)` | 纯函数，逐字搬移 |
| `ComputeContentArea` | :558-604 (WaylandServer 成员，私有) | 计算段 → `void ComputeContentAreaGeometry(ShmCommitInfo& fi, bool hasWindowGeometry, int32_t geoW, int32_t geoH, bool hasToplevel, int32_t geoX, int32_t geoY)`（新模块）；日志段 → 留在 `WaylandServer::ComputeContentArea` 包装 | 计算/日志按 hilog 边界切开（见下） |
| `BeginShmAccess` | :542-553 (WaylandServer 成员，私有) | **不抽离，留在 wl_core.cpp 原样** | wl 资源生命周期知识（wl_shm_buffer_get_*/begin_access 配对），不可纯化；host 环境无 wayland 头 |

**依赖迁移决策：**

- `ShmCommitInfo` 随迁：定义从 `surface_data.h:15-25` 移入
  `shm_frame_source.h`（纯值 POD + 不透明 `wl_shm_buffer*` 前置声明，
  本就不需要 wayland 完整头）；`surface_data.h` 改为 include 新头，
  头部注释注明归属变更。使 shm_frame_source 零 wayland/hilog 依赖，
  host g++ 直连编译。
- 日志处理（ComputeContentArea 切开的唯一理由）：两条 hilog
  `[MW-GEO]`（:593-596，位于 geometry if 块尾）与 `[MW-STRIDE]`
  （:600-603，函数尾）保留在 `wl_core.cpp` 的包装函数里，条件
  （`sd->hasWindowGeometry && geoW>0 && geoH>0`）、文本、占位符、
  顺序与旧实现**逐字一致**（对账脚本确认 identical）。纯函数只搬
  计算语义（值参数化 + clamp 防御 + 注释逐字），不做日志回调注入
  （%{public} 修饰符使回调宿主无法复现文本，注入即改动日志）。
- static 状态：三个 file-static free 函数升为模块全局函数（同名，
  调用点无需改名——`CopyShmBufferTight(fi, sd->pixels)` 未变），
  无其他文件静态依赖可随迁（`DisplaySizeAfterViewport` 来自
  geometry.h 纯函数头，新模块直接 include）。
- wayland_server.h 私有声明 `BeginShmAccess`/`ComputeContentArea`
  **未动**（签名不变），外部调用面零变化。

**wl_core.cpp 内改动：** include `shm_frame_source.h`；删除三个
file-static 函数定义（原 :456-510）；`ComputeContentArea` 计算段改
调纯函数 + 日志逐字保留；唯一调用点 `CopyToplevelContent` 改为传
`sd->vpDstW, sd->vpDstH`（:= 原行 617，位于 `UpdateToplevelFrameOnCommit`
的 `toplevelMgr_.Lock()` 锁内——锁域/调用顺序未变）；`surface_commit`
里 `CopyShmBufferTight` 调用点与 `BeginShmAccess` 调用形态原样。

**host_tests（shm_frame_source_test.cpp，39 checks / 0 failures）：**

- CopyShmContentTight：带 stride padding 的内容区紧凑拷贝（黄金值逐
  字节）、零内容尺寸、全内容 == CopyShmBufferTight 结果；
- CopyToplevelContent：vpDst=-1 → tight 路径（与 CopyShmContentTight
  逐字节一致）、vpDst 与内容同尺寸/0 → tight、（4x4→8x6）放大与
  （8x8→4x4）缩小的逐像素 vs 独立 double 路径参考、fi.contentW/H
  更新为逻辑尺寸；
- CopyShmBufferTight：padding 全 buffer 拷贝黄金值、零 buffer；
- ComputeContentAreaGeometry：无 geometry 全 buffer、toplevel 分支
  （off=0/screen=geo）、subsurface 分支（off=geo）、clamp（越界/负
  off/边界恰贴）、geoW/geoH 非正回退；
- 固定种子 fuzz（500 轮随机 buffer 尺寸/off/padding/内容区/逻辑尺寸）
  vs 独立参考 0 不一致。

**对账结论（逐函数与原体一致性）：** 三个拷贝函数与旧 file-static
函数体逐字一致（脚本 unified-diff，仅签名行 static 去除 /
`sd->vpDstW/H` → 参数）；`ComputeContentAreaGeometry` 与旧成员函数
计算段的语句序列逐语句等价（sd 字段 → 同值参数），`[MW-GEO]`/
`[MW-STRIDE]` 日志段脚本确认与旧文本 identical 且位置顺序不变
（计算 → MW-GEO → MW-STRIDE）；`BeginShmAccess` 未动。行为平价：
无算法/坐标/裁剪/缩放语义变化；锁域（tmgr 锁内调用点路径不变）、
日志门控、调用顺序逐字。

**门禁：** `make test` host_tests 全绿（geometry 71 / blit_scaled
402 / blit_clip 38 / zorder 40 / env 21+88 / input_state 84 /
**shm_frame_source 39**，全 0 failures）；`make NATIVE_ARCH=arm64-v8a
hap` 构建+签名通过（HAP 374M，build-profile.json5 已还原，工作树仅剩
本提交改动 + 预存 thirdparty/）。

**给 5A2 的接口说明（协议拆壳前置）：** `ShmFrameSource` 是 commit
路径「读像素」知识单点——后续把 `surface_commit` 语义段（角色分发/
更新上层状态）从 wl_core.cpp 挪走时，本模块的四个函数
（CopyShmContentTight / CopyToplevelContent / CopyShmBufferTight /
ComputeContentAreaGeometry）可直接复用，无需触碰 wl 资源知识
（BeginShmAccess/WaylandServer::ComputeContentArea 日志壳属协议层，
5A2 拆壳时归协议壳侧）；`ShmCommitInfo` 已是事实上的帧快照 POD
（值与 wl 无关），5A2 的 CommittedSurface 若携带像素裁切信息可直接
持有它或按字段抽取。注意红线：`CopyToplevelContent` 的
DisplaySizeAfterViewport（不 clamp 变体）语义与 clamp 变体不可互换
（PLAN §2.3 11 处重复中的两变体之一），5A2 若收口显示尺寸公式必须
按调用点历史行为选变体。

### 阶段 5B1（commit 业务段各归其主，2026-08-29）

PLAN §四阶段5 第 2 条（本段范围第 1/2/4 子段）：把 wl_core.cpp
`UpdateToplevelFrameOnCommit`/`FinishCommit` 的窗口管理业务段（ARGB
掩码生成 / 位置同步 / 恢复判定 / 全屏尺寸漂移重发 / 首帧 focus）收口
到 ToplevelManager 语义方法层与 WaylandServer 会话焦点策略，消除
"协议壳直接操作合成器内部状态、跨层知识倒挂"。全部行为平价（逐语句
等价搬移，无统一语义，无锁域变化）。

**业务段归属表（段 → 归属对象 → 语义方法签名，行号为搬移前 wl_core.cpp）：**

| 业务段（现状） | 归属对象 | 语义方法 | 时序等价论证 |
|---|---|---|---|
| 自动恢复最小化窗口 判定（:597-609，`IsRestoreSizeCommit` + `SetMinimized(false)` + `[MW] auto-restore` 日志） | ToplevelManager | `bool TryAutoRestoreLocked(uint32_t id, int32_t contentW, int32_t contentH)`（返回 justRestored） | 判定条件/SetMinimized 时机/日志文本/顺序逐字；求值位置不变（仍在首帧判定前）；justRestored 出参同值导出。补丁注释（"锁内不能调 SetToplevelRestored"）完整平移 |
| ARGB 窗口位置同步（:644-650，Wine 位置权威 → `SetPosition` + `argb_move`） | ToplevelManager（应用）+ wl_core（事件发出） | `bool SyncArgbPositionLocked(uint32_t id, int32_t screenX, int32_t screenY)`（返回位置是否变化） | X/Y 比较与 `SetPosition` 逐字搬入；模式/格式/首帧门禁守卫仍在调用点（相同短路顺序，求值位置一致）；`argb_move` 事件仍锁内发出，条件 = 方法返回 true 与原 `(X!=sx‖Y!=sy)` 逐字等价 |
| 桌面模式位置同步（:665-685，WineX/Y 快照比较 + justRestored/最小化阈值/geo 同步三分支） | ToplevelManager | `void SyncDesktopPositionLocked(uint32_t id, int32_t screenX, int32_t screenY, bool justRestored)` | `Policy().RootCompositing() && !outFirstCommit` 守卫留在调用点；三分支/比较/`SetPosition`/`SetWinePosition` 顺序/`[MW-MOVE] restore keep pos`/`wine geo sync` 日志逐字；位置与 `justRestored`（上方法返回值）传递同值 |
| ARGB 剪影掩码生成（:712-732，FNV-1a 哈希 + 128 阈值 0/1 剪影 + `mask_dirty`） | ToplevelManager（算法+状态）+ wl_core（事件发出） | `bool UpdateArgbMaskLocked(uint32_t id, const std::vector<uint8_t>& pixels, int32_t w, int32_t h)`（返回形状/尺寸更新发生） | FNV 常数/阈值/两段循环/更新条件 `(hash≠m.hash‖m.w≠w‖m.h≠h)`/bits resize+逐像素/`dirty=true` 逐字；事件保持调用方锁内发出（原同段同锁，输出条件逐字）；PLAN §2.2 掩码补丁注释（阈值 128 收半像素/哈希不变不重建/帧分辨率+effectiveScale 放大）完整平移 |
| 提交尺寸上报 + 全屏尺寸漂移重发（:752-761 判定 + `:766-770` resize 事件；war3 补丁 + 持锁自死锁修复） | ToplevelManager（判定）+ wl_core（锁外动作/锁内事件） | `enum class SizeCommitEffect {None, ResizeEvent, ReassertFullscreen}`；`SizeCommitEffect HandleCommittedSizeLocked(uint32_t id, uint32_t rootId, int32_t contentW, int32_t contentH, int32_t outputW, int32_t outputH)` | `CheckAndUpdateLastReportedSize` 收内（写点时机同）；`IsFullscreen && id≠root && (<output)` 判定逐字；`[MW] ... fullscreen size drift` 日志文本逐字（锁内，先于 unlock）；`ReassertFullscreen` → `lk.unlock()` + `NotifyToplevelResize` 同位置（unlock 后无 st 触碰，与原一致）；`ResizeEvent` → resize json/日志/事件锁内同位置（`sd->maximized` 读点不变） |
| 首帧 focus 预注入（FinishCommit :1049-1062，`firstFrame_` CAS → `active` 事件 + Pointer/Keyboard enter 预注入） | WaylandServer（会话焦点策略；非 ToplevelManager） | `void TryBeginSessionFirstFrame(uint32_t toplevelId, wl_resource* surfRes)`（private，实现 wayland_server.cpp） | CAS/`FireState("active")`/Seat 资源检查/两注入调用逐字，顺序不变；调用点仍在 release+callback 之后（FinishCommit 原位）；**首帧 focus 决策确不在 FireToplevelEvent created 处理**（wayland_server.cpp:205-231 无焦点注入），按实际从属收口（会话状态 firstFrame_ 所有者 = WaylandServer） |

**lk.unlock hack 处置结论：** 存在且如实保留其必要性 —— wl_core.cpp
原 :760 的 `lk.unlock()` + `NotifyToplevelResize` 是 war3 全屏尺寸漂移
补丁（PLAN §2.5 "wl_core.cpp:766-795"，含 2026-08-15 自死锁修复记录）。
消除方式：**判定**（IsFullscreen/root/output 比较）收进
`HandleCommittedSizeLocked`（补丁注释完整平移），wl_core 不再"懂得
为何条件成立"，只机械执行"语义方法返回 ReassertFullscreen → 解锁 →
重发"。`unlock` 本身无法移除 —— `NotifyToplevelResize` 内部
`IsToplevelFullscreen` 会再取非递归 `toplevelMutex_`，持锁调用自死锁
（这是补丁语义而非 hack），判定与动作用显式返回值（`SizeCommitEffect`）
分离，破坏性知识（"持锁不能调自己"）不出锁边界。

**首帧 focus 归属结论：** 决策实际在 `FinishCommit`（wl_core.cpp
:1049-1062，`firstFrame_` 会话级一次性字段 CAS），而非协议事件处理；
从属对象 = 会话状态所有者 WaylandServer（firstFrame_/FireState 属它，
注入经 InputManager 门面），收口为私有命名方法
`TryBeginSessionFirstFrame`，语义 = 会话首帧焦点策略。未触碰
InputStateTracker（4C2 产物）—— 该决策是跨模块编排（Seat 资源检查 +
门面注入），非 tracker 纯状态域。

**对账结论：** 六段逐语句等价（脚本抽取新旧全部日志 9 条多项集
比对一致 —— 文本/占位符/参数逐字，仅 `sd->toplevelId`→`id` 等
参数化改名）；补丁注释五处（恢复判定/ARGB 位置/桌面位置同步/ARGB
掩码/全屏漂移+自死锁）完整平移至语义方法定义处，调用点留引用性
短注释（原文勿删语义，均在目标文件）；锁域零变化（原锁内段仍在
锁内、原锁外重发仍在锁外、首帧段本无锁）；未统一任何阈值/判定
（掩码 128 阈值/FNV 常数/恢复尺寸阈值/最小化坐标阈值/漂移重发
条件逐字保留）。popup（UpdatePopupOnCommit）未触碰（留 5-B2
PopupManager）。

**门禁：** `make test` host_tests 全绿（geometry 71 / blit_scaled
402 / blit_clip 38 / zorder 40 / env 21+88 / input_state 84 /
shm_frame_source 39，全 0 failures，本提交未改测试代码）；`make
NATIVE_ARCH=arm64-v8a hap` 构建+签名通过（HAP 374M，构建后
build-profile.json5 已还原，工作树仅剩本提交改动 + 预存
thirdparty/）。**设备回归待做**（行为平价纯搬移，但 commit 路径
为设备重度敏感区）：位置跟随（桌面窗口 move grab 后拖动/3DMLauncher
边框残影复现）、ARGB 异型窗口（掩码 setWindowMask/shape 时钟）、
最小化自动恢复、war3 全屏尺寸漂移（`[MW] ... fullscreen size drift`
日志）、首帧 focus 注入（桌面激活/enter 注入日志与基线一致）。

**给 5-B2 的接口说明：** PopupManager 实现时可直接复用本段收口的
ToplevelManager 语义方法（`SyncArgbPositionLocked`/`SyncDesktopPositionLocked`
均按 id+ToplevelState 工作），`UpdatePopupOnCommit` 的父几何读点
（`parentSd->committed.contentRect.x/y`）与掩码语义方法（只认
pixels+w/h，不要求 ShmCommitInfo）解耦，PopupManager 迁移 popup
时不需要触碰这些方法。

### 阶段 5B2（PopupManager 拆出，2026-08-29）

PLAN §三 PopupManager + §四阶段5 第 2 条 popup 子段：把 popup 状态
管理（登记/裁剪/事件状态）从 wl_core.cpp/ToplevelManager 拆为独立
模块 `compositor/popup_manager.{h,cpp}`；popup 偏移公式 4 份收口
（PLAN §2.3）为 geometry.h 单一纯函数。全部行为平价（逐语句等价 +
通道等价论证，见对账结论）；**事件 fire 调用点保持现形态**（红线：
popup 事件仍在 wayland_server/wl_core 发出，PopupManager 只产出
事件描述）。

**接口形态（PopupManager，被 tmgr 锁守护，无独立锁）：**

| 方法 | 原出处 | 调用方 | 锁契约 |
|---|---|---|---|
| `PopupCommitEvent UpdatePopupOnCommit(sd, surfRes, parentSd, fi)` | WaylandServer::UpdatePopupOnCommit (wl_core.cpp) | wl_core.cpp UpdateSubsurfaceOnCommit else 分支: 状态段收内, 事件段(show/resize/move json+日志)按返回描述逐字恢复, fire 原位 | 方法自持 tmgr 锁段 (与迁移前同一锁边界: 无锁计算段→锁段→锁外事件描述) |
| `bool UpdatePopupPositionLocked(surfaceKey, x, y, parentContentX, parentContentY, PopupMoveEvent& out)` | subsurface_set_position popup_move 内联段 | wl_core.cpp subsurface_set_position (锁段内), 事件段按 out 锁外 fire 原位 | 调用方须已持 tmgr 锁 |
| `uint32_t RemovePopupBySurfaceKeyLocked(key, outPopupId)` | ToplevelManager 同名 | wl_core.cpp 4 处 (surface resource 析构 / subsurface_destroy / surface_destroy / HandleNullBufferCommit), 锁域不变 | 调用方须已持 tmgr 锁 |
| `void RemovePopupDataLocked(popupId)` | ToplevelManager 同名 | OnToplevelDestroyed 级联 (经下方收集) | 调用方须已持 tmgr 锁 |
| `std::vector<uint32_t> CollectPopupIdsForParentLocked(parentId)` | 原 `popups()` 遍历 (wayland_server.cpp OnToplevelDestroyed) | 同上 (收集→删除→dirty→unmap 顺序逐字, 锁外 popup_hide 原位) | 调用方须已持 tmgr 锁 |

**表迁移细节（谁调用 tmgr popup 方法 → 改向哪）：** `popups_` /
`popupBySurfaceKey_` + `PopupRecord` 类型 + `FindPopupBySurfaceKey /
FindPopup / RegisterPopup / RemovePopupDataLocked /
RemovePopupBySurfaceKeyLocked / popups()` 全部从 ToplevelManager 迁出
（tmgr 侧删除；无外部残留引用，grep 佐证）；PopupRecord 缩为
popupId/parentToplevel/surface/surfaceKey/offX/offY — 原 `w/h` 字段
随尺寸通道切换删除（见下）。popup 帧数据仍复用
`ToplevelManager::ToplevelState`（popupId 来自 tmgr 取号器），清理时
经 `EraseToplevelLocked`/`UnmapToplevelSurface` 对称清除（原
RemovePopupDataLocked 的 toplevels_/toplevelSurfaceMap_ 段，行为逐字）。
所有 tmgr popup 调用点改向 popupMgr_（WaylandServer 新成员，构造注入
`toplevelMgr_`+`outputW_/outputH_` 引用 — DesktopCompositor 同款注入）。

**popup 尺寸上报通道切换（唯一语义通道变更，等值论证）：**
旧 `sizeChanged = (rec->w != winW || rec->h != winH)`（rec->w/h 建档时
记录、每帧更新）；新改经 5B1 的 `HandleCommittedSizeLocked(popupId, 0,
winW, winH, outputW_, outputH_)` — popup 的 ToplevelState 去重通道
（`lastReportedW_/H`）语义等价：
- 判定值逐字 = winW/H（全屏父补丁后的窗口上报尺寸；popup 从不
  SetToplevelFullscreen → 漂移分支恒不触发；rootId=0 → id≠root 恒真）；
- `isNew` 首帧也调用（播种 lastReported，等价旧"建档时 rec->w/h =
  winW"），其 ResizeEvent 返回值被 isNew 吞掉（新 popup 只发 show，
  第二帧同尺寸不发 resize，逐帧一致）；
- 去重状态生命周期一致（随 popup 的 ToplevelState 在 RemovePopupData
  时复位，等价 rec 删除）；
- 随机对拍 5806 帧（3 surfaceKey 交错 commit/销毁/全屏补丁 winW 恒
  output 场景）事件序列 0 不一致。`rec->w/h` 随通道切换删除（零外部
  消费，grep 佐证）。

**偏移公式 4 份收口核对表（ComputePopupOffset(geometry.h)，
公式逐字 offX = subX − parentContentX/offY = subY − parentContentY）：**

| 调用点 | 旧 | 新 | 等值论证 |
|---|---|---|---|
| wl_core.cpp subsurface_set_position popup_move 段 | `rec->offX = x - parentContentX` （parentContent 读 parentSd->committed.contentRect） | `PopupManager::UpdatePopupPositionLocked` 内 ComputePopupOffset(x, y, parentContentX, parentContentY) | 同一读数/同一算术, 纯函数内联等价; 调用点读 parent 值不变 |
| wl_core.cpp UpdatePopupOnCommit（现 PopupManager::UpdatePopupOnCommit） | `offX = sd->subsurfaceX - parentSd->committed.contentRect.x` | 同函数体内 ComputePopupOffset(sd->subsurfaceX, …) | 同前 |
| desktop_compositor.cpp BuildLayerListLocked ZC subsurface 层 | `zcLayer.x = sd->subsurfaceX - parent->committed.contentRect.x` | ComputePopupOffset(...) 解构赋值 | 同前 (几何字段赋值顺序 x 先 y 后不变) |
| zc_bridge.cpp GetZeroCopyLayerInfo PC 分支 | `info.x = sd->subsurfaceX - parent->committed.contentRect.x` | ComputePopupOffset(...) 解构赋值 | 同前 |

全库 grep 确认无残留手写 `subsurfaceX - parent…` 表达式（仅注释提
公式）。host_tests geometry_test 新增第 14 组（6 checks: 零原点/正
偏移/负内容原点/负子偏移/与加法互逆恒等）。

**补丁资产平移（PLAN §2.5）：** war3 popup 窗口/内容尺寸解耦补丁注释
（"全屏主窗口的 GL client surface… 窗口按全屏输出尺寸上报, FrameData
仍按内容尺寸存"段落）完整平移至 PopupManager::UpdatePopupOnCommit
定义处；wp_viewport 裁剪注释 + P2 风险标注（父销毁后重登记竞态）逐字
平移；popup 裁剪逻辑（vpSrc/vpDst 源矩形 + 显示尺寸封顶 + crops
memcpy 紧凑排列 + 双缓冲轮换）零改动。

**对账结论：**
1. popup 相关日志/事件字符串（show/hide/resize/move 4 事件名 + 5 种
   JSON 模板 + 2 条 OH_LOG 文本）新旧多项集脚本比对完全一致（旧有
   集合无缺失项）；
2. 事件 fire 分支结构逐字：isNew → MapToplevelSurface → show json+
   日志+fire → return（不发 resize/move）；else → sizeChanged →
   resize → posChanged → move（顺序逐字）；
3. 移动路径：FindPopup(0) miss 防御、出参替换、fire 条件
   `move.popupId` 与旧 `movePopupId` 逐字；父 contentRect 读点在
   wl_core 原位（PopupManager 不涉 SurfaceData 父几何）；
4. 清理路径 4 处 + 级联：锁段边界/清理顺序（bySurfaceKey →
   data: key→toplevels→surfaceMap→popups）/锁外 fire 条件
   `removedPopup` 逐字；`toplevels_.erase`→`EraseToplevelLocked`、
   `toplevelSurfaceMap_.erase（锁内锁）`→`UnmapToplevelSurface`（公开
   路径），锁序（tmgr 锁→toplevelSurfaceMutex_）不变；
5. 级联收集返回顺序 = popups_ 遍历顺序（同一 unordered_map 同一
   遍历），与旧 toplevelMgr_.popups() 一致；
6. 尺寸通道切换：随机对拍 5806 帧 0 不一致（见上）。

**门禁：** `make test` host_tests 全绿（geometry 77 / blit_scaled
402 / blit_clip 38 / zorder 40 / env 21+88 / input_state 84 /
shm_frame_source 39，全 0 failures；本提交新增 geometry 第 14 组 6
checks）；`make NATIVE_ARCH=arm64-v8a hap` 构建+签名通过（HAP 374M，
构建后 build-profile.json5 已还原，工作树仅剩本提交改动 + 预存
thirdparty/）。**设备回归待做**（行为平价纯迁移，popup 为窗口模式
重敏感区）：PC 多窗口模式菜单弹出/边缘裁剪/菜单置顶、子菜单链移动、
popup 尺寸变化（resize 事件）、菜单关闭（popup_hide）、war3 D3D
模式切换（GL client surface 全屏上报补丁）、桌面 root 销毁时 popup
级联清理。

**给 5-C/5-D 的接口说明：** popup 侧无 ToplevelEventBus 事件名依赖
（popup_show/resize/move/hide 四事件仍经 FireToplevelEvent 原样
发出，5-D 事件 enum 化时这 4 个事件名应一并收编，json 字段见
UpdateSubsurfaceOnCommit 事件段）；popup 尺寸通道已并入
ToplevelState 的 lastReportedW_/H + HandleCommittedSizeLocked —
5-C maximized 迁移若调整 SizeCommitEffect 语义需复查 popup 消费
（当前只消费 ResizeEvent，popup 从不触发 ReassertFullscreen）。

### 阶段 5C（maximized 迁入 ToplevelState，2026-08-29）

PLAN §四阶段5 第 3 条 + §2.4 状态权威分裂修复的最后一块：窗口状态
三元组 maximized 的"生效状态"自 `SurfaceData::maximized` 迁入
`ToplevelState`，与 minimized/fullscreen 同权威（本步前 maximized
是三元组里唯一分裂在 SurfaceData 的字段，tl_set_fullscreen 还须
手工清它，PLAN §七风险表"状态权威统一影响 xdg 行为"焦点）。
preMaxW/H/preFsW/H 恢复尺寸**保留在 SurfaceData**（xdg 状态机尺寸
交接字段，PLAN 未列迁入，归 5-D/后续状态机收口，本步不越界）。

**接口形态（ToplevelState/ToplevelManager/WaylandServer）：**

- `ToplevelState`：私有 `bool maximized_ = false`；`IsMaximized()`
  /`SetMaximized(bool)`（裸 setter，与 SetMinimized 同款；注释记录
  5C 出处与"裸状态写无 dirty/日志 — dirty 由调用点随后的
  SetToplevelMaximized 锚定 / configure 路径负责"的等价论证）。
- `ToplevelManager::IsToplevelMaximized(id)`：加锁 find miss=false
  （与 IsToplevelMinimized/IsToplevelFullscreen 同款）；状态写不
  落 ToplevelManager（遵守"变更只经 WaylandServer::SetToplevel*"）。
- `WaylandServer::IsToplevelMaximized(id)` inline 转发（wayland_server.h
  状态查询区）；`WaylandServer::SetToplevelMaximizedState(id, on)`：
  Ensure 建档 + 裸状态赋值（cpp，无日志无 dirty — 对齐旧直接赋值）。
  已有 `SetToplevelMaximized(id)`（锚定+dirty 的几何反应函数）保持
  不动，头部注释更新为与状态位写互相指认（历史形态：两函数分离）。

**逐消费点核对表（全库 grep `->maximized`/`.maximized`/`Maximized`
业务代码命中 10 处 = 1 定义 + 6 读 + 4 写，全部落表）：**

| # | 读点（旧） | 新 | 等价论证 |
|---|---|---|---|
| R1 | wayland_server.cpp NotifyToplevelResize IN 日志 `(sd && sd->maximized)` | 函数内一次取局部 `const bool maximized = IsToplevelMaximized(toplevelId)`，三处消费（IN/状态位/出口日志）共用 | ① 全访问在 wl 线程，无并发写；② sd==null 旧=false ↔ State 未建档 miss=false 同值；③ State 已建档场景值与旧 sd 字段同步（写点全经 SetToplevelMaximizedState）；④ 锁域：独立加锁同 IsToplevelFullscreen（本函数入口 IsToplevelMinimized 已是该模式，无嵌套） |
| R2 | 同函数 MAXIMIZED 状态位 `if (sd && sd->maximized)` | `if (maximized)` | **xdg configure 状态位回归焦点（PLAN §七）**：json 状态序列、顺序、位值逐字；Wine 侧 WS_MAXIMIZE 同步语义不变；三读点合一局部变量的快照性更强（旧为同 sd 字段同刻值，等价） |
| R4 | xdg_shell.cpp fire_limits_event 日志 `sd->maximized` | `WaylandServer::GetInstance()->IsToplevelMaximized(sd->toplevelId)` | tl_set_max_size 推断路径：State 已由 SetToplevelMaximizedState(true) 置位 → yes 同旧；tl_set_min_size 路径从未 set → miss=false 同 sd 默认 false；toplevelId==0 已有函数首守卫 |
| R5 | xdg_shell.cpp ShouldInferMaximizeFromMaxSize `!sd->maximized` | `!ws->IsToplevelMaximized(sd->toplevelId)` | 谓词已持 ws 参数（参数化不变）；判定时机（max_size 写后、推断动作前）与短路顺序逐字（全屏在上、maximized 在下）；自身守卫在 SetToplevelMaximizedState 之前 → 未置位者 miss=false 与 sd 默认一致 |
| R6 | xdg_shell.cpp tl_set_maximized 转换守卫 `if (!sd->maximized)` | `if (!ws->IsToplevelMaximized(...))` | 守卫→preMax 快照→Set 置位→锚定顺序逐字；wl 线程内判定与写入之间无并发写，读改写原子等价 |
| R7 | wl_core.cpp resize 事件日志 `sd->maximized` | `st.IsMaximized()`（**已持锁的 st 引用**） | 该处在 UpdateToplevelFrameOnCommit 锁内（函数首 :587 Ensure 后 st 恒存在）— **不能调 IsToplevelMaximized（内部重新加锁→非递归 std::mutex 自死锁，同 5B1 ReassertFullscreen 约束）**；锁内直读 State 与旧 sd 字段同域同值（写点全经 Ensure 的 State 写入）；锁域不变 |

写点（4 处）：W1 tl_set_max_size 推断 → `ws->SetToplevelMaximizedState(id, true)`（插入点逐字：谓词通过后、锚定前）；W2 tl_set_maximized → 同上（R6 守卫内）；W3 tl_unset_maximized → `...SetToplevelMaximizedState(id, false)`（fullscreen 守卫后、preMax 读前，无 dirty 与旧等价）；**W4 tl_set_fullscreen 手工清（行为敏感焦点）** → `ws->SetToplevelMaximizedState(id, false)`：同一触发点（preFs 快照后、SetToplevelFullscreen 前）同一条件（!IsToplevelFullscreen 块内），注释全文平移+Bug 语义补记；迁移后清的是 ToplevelState（唯一权威）— 旧"hand 清 sd 而 State 无意"的双存失步由结构消除（无引入新失步：新存储只有 State 一处）。全库仅有此一处 set_fullscreen 清 maximized（grep 佐证；SetToplevelFullscreen 的 ApplyFullscreen 不清 maximized — 新旧一致）。

**行为敏感点处置（PLAN §七 两选项，已选严格）：** 发现两个候选差异，均确认旧行为模式在两端一致 → **选严格选项（保持逐字），候选上报用户**：
1. **State 已 Erase 后的读差异**：OnToplevelDestroyed（EraseToplevelLocked）后、xdg 资源销毁前的窄窗口，若 NotifyToplevelResize 再被调：旧读 sd->maximized 残留真值 → 新 miss→false。方向 = 与 minimized/fullscreen 同款"State 清即状态亡"权威语义（销毁后窗口带 MAXIMIZED 位本就是错误），修复方向而非回归。
2. **SetToplevelRestored 不清 maximized（预存嫌疑，非本步引入）**：最小化的最大化窗口经 SetToplevelRestored 还原，configure states 无 MAXIMIZED（Wine 清 WS_MAXIMIZE），但 compositor maximized 标志保持 true（旧 sd 字段同样不清）→ 后续 NotifyToplevelResize 会继续带 MAXIMIZED 位。新旧行为完全一致，属历史语义冲突（还原 notify 通道与状态位不同步），**严格选项不改**；扩展选项 = SetToplevelRestored 清 maximized（行为变化，需设备回归（最小化还原的最大化窗口）后方可考虑，留用户决定）。

**5-B2 popup 约束复查：** 通过。popup 尺寸通道经 `HandleCommittedSizeLocked` — 该函数体无 maximized 任何引用（点读确认，本步零改动）；popup 从不 set_maximized（xdg popup 无该协议）→ 迁入 State 的 maximized_ 对 popup 恒 false；NotifyToplevelResize 的 popupId 不可达（popup resize 走 FireToplevelEvent ResizeEvent，不调 NotifyToplevelResize）。5-C 对 popup 路径零影响。

**门禁：** `make test` host_tests 全绿（geometry 77 / blit_scaled 402 / blit_clip 38 / zorder 40 / env 21+88 / input_state 84 / shm_frame_source 39，全 0 failures，本提交未改测试代码 — ToplevelState 依赖 wayland 头不可进 host_tests，同 5B1）；`make NATIVE_ARCH=arm64-v8a hap` 构建+签名通过（HAP 374M，构建后 build-profile.json5 已还原，工作树仅剩本提交改动 + 预存 thirdparty/）。

**遗留（设备回归请求）：** 窗口最大化/还原（Wine 最大大小推断 + set_maximized 直发）、全屏切换（tl_set_fullscreen 手工清路径：全屏后 MAXIMIZED 位不误带）、最小化→还原（SetToplevelRestored 无 MAXIMIZED 状态位的预存语义）、popup 菜单；监视日志：`[XDG] tl_set_maximized/unset_maximized`、`[MW] NotifyToplevelResize ... max=yes/no`、`[MW] toplevel #N size changed ... max=`；war3 全屏尺寸漂移（ReassertFullscreen 路径 — maximized 不触达，回归确认）。

### 阶段 5D（ToplevelEventBus 事件 enum 化 + JSON 构造单点 + NAPI 通道移出 core，2026-08-29）

PLAN §三 ToplevelEventBus + §四阶段5 第 4 条 + §2.3（22 种 stringly-typed 事件名
散布 5 文件 ~30 处 snprintf）+ §2.2（FireToplevelEvent 直调
napi_call_threadsafe_function 点名的 NAPI 通道泄漏）。全部行为平价。

**事件清单表（22 种 → enum → JSON 模板 → caller → payload 来源）：**

| # | 事件名 | enum | JSON 模板（逐字） | caller（旧行号，无 payload 即默认 "{}"） | payload 来源 |
|---|---|---|---|---|---|
| 1 | created | Created | `{"w":%d,"h":%d}` | wl_core.cpp surface_commit 首帧 XRGB 分支 | fi.contentW/H |
| 2 | created（桌面） | Created | `{"w":640,"h":480}` | xdg_shell.cpp xs_get_toplevel（硬编码常量） | 无 |
| 3 | argb_created | ArgbCreated | `{"x":%d,"y":%d,"w":%d,"h":%d}` | wl_core.cpp surface_commit 首帧 ARGB 分支 | fi.screenX/Y + contentW/H |
| 4 | destroyed | Destroyed | `{}` | wayland_server.cpp DestroyAllToplevels / wl_core.cpp (surface resource 析构 :134, surface_destroy :390) / xdg_shell.cpp (xs_destroy, xs_resource_destroy) — 5 处 | — |
| 5 | popup_hide | PopupHide | `{"popupId":%u}` | wayland_server.cpp 级联 / wl_core.cpp 4 处（析构/subsurface_destroy/surface_destroy/NULL buffer）— 5 处 | removedPopup / pid |
| 6 | popup_move | PopupMove | `{"popupId":%u,"x":%d,"y":%d}` | wl_core.cpp subsurface_set_position / UpdateSubsurfaceOnCommit | PopupMoveEvent.popupId/offX/offY |
| 7 | popup_show | PopupShow | `{"popupId":%u,"x":%d,"y":%d,"w":%d,"h":%d,"argb":%d}` | wl_core.cpp UpdateSubsurfaceOnCommit isNew 分支 | ev.popupId/offX/offY/winW/winH + shmFormat==0?1:0 |
| 8 | popup_resize | PopupResize | `{"popupId":%u,"w":%d,"h":%d}` | wl_core.cpp UpdateSubsurfaceOnCommit | ev.popupId/winW/winH |
| 9 | argb_move | ArgbMove | `{"x":%d,"y":%d}` | wl_core.cpp 首帧后 ARGB 位置同步（SyncArgbPositionLocked 返回 true） | fi.screenX/Y |
| 10 | argb | Argb | `{"argb":%d}` | wl_core.cpp shm format 变化/首帧（OhosWindowPerToplevel） | shmFormat==0?1:0 |
| 11 | mask_dirty | MaskDirty | `{}` | wl_core.cpp UpdateArgbMaskLocked 返回 true | — |
| 12 | resize | Resize | `{"w":%d,"h":%d}` | wl_core.cpp HandleCommittedSizeLocked ResizeEvent | fi.contentW/H |
| 13 | desktop_root | DesktopRoot | `{}` | wl_core.cpp CheckDesktopRootOnCommit + DesktopRootManager::PromotePending（经构造注入 lambda · 2 条路径） | — |
| 14 | surface | Surface | `{"w":%d,"h":%d}` | plugin_manager.cpp ResizeRenderer | w/h |
| 15 | title | Title | `{"title":"%s"}`（不转义，既有行为） | xdg_shell.cpp tl_set_title | sd->title.c_str() |
| 16 | limits | Limits | `{"minW":%d,"minH":%d,"maxW":%d,"maxH":%d}` | xdg_shell.cpp fire_limits_event（set_min/max_size 共用） | sd->min/maxWidth/Height |
| 17 | maximized | Maximized | `{}` | xdg_shell.cpp tl_set_maximized | — |
| 18 | unmaximized | Unmaximized | `{}` | xdg_shell.cpp tl_unset_maximized | — |
| 19 | fullscreen | Fullscreen | `{}` | xdg_shell.cpp tl_set_fullscreen | — |
| 20 | unfullscreen | Unfullscreen | `{}` | xdg_shell.cpp tl_unset_fullscreen | — |
| 21 | minimized | Minimized | `{}` | xdg_shell.cpp tl_set_minimized | — |
| 22 | move_start | MoveStart | `{}` | wayland_server.cpp StartMoveGrab（OhosWindowPerToplevel） | — |
| 23 | move_end | MoveEnd | `{}` | wayland_server.cpp EndMoveGrab | — |

（"created" 有两种模板 — PC 首帧内容尺寸 vs 桌面 get_toplevel 硬编码 640x480，
为既有分化，bus 保留为 JsonCreated / JsonCreatedDefault 两个构造器，未统一。）

**模块形态（新增 compositor/toplevel_event_bus.{h,cpp}）：**

- `ToplevelEventType` 全局 enum class（22 种，命名与旧字符串一一对应）；
  `ToplevelEventName()` 头内联 switch 映射旧字符串（日志/sink 消费，逐字）。
  enum 名不与 napi_init.cpp 的 `struct ToplevelEvent`（全局）冲突。
- `ToplevelEventBus`：`Post(id, evt, json = "{}")`（投递单点，语义 = 旧
  FireToplevelEvent：① 抑制门禁 created/argb_created → `[MW] suppress` 日志
  ② `[MW] FireToplevel id=... event=... data=...` 日志 ③ EventSink 派发 —
  文本/条件/短路顺序逐字；EventSink 签名 = 旧 ToplevelCb
  (id, eventName, jsonData)，**napi_init.cpp SetToplevelCallback 装配点零改动**）；
  `SetSuppressed`（wine_launch SetToplevelEventSuppressed 转发，签名不变）；
  Json* 静态构造器（12 个 14 模板，snprintf 模板逐字，含 created 双模板）。
- 零依赖约定：EventName/Json* 头内联（无 wayland/hilog），bus 投递实现在
  .cpp（hilog）。host_tests/toplevel_event_test.cpp（62 checks）直连头编译。
- 事件 fire 调用点保持原位（30 处），只改 fire 方式：字符串事件名 +
  调用点 snprintf → `ToplevelEventType::Xxx` + `ToplevelEventBus::JsonXxx(...)`；
  WaylandServer::FireToplevelEvent → `PostToplevelEvent`（公开转发，内部
  bus.Post + desktop_root 会话侧旁路）。桌面 root 的 PromotePending 间接
  路径（DesktopRootManager 构造注入 lambda）同步收编。

**NAPI 移出方案（谁注入/何时）：**

- 移出前：FireToplevelEvent 只有 desktop_root 分支直调
  `napi_call_threadsafe_function(gStateTsfn, "evt:desktop-ready")`
  （其余已回调化 toplevelCb_）；gStateTsfn 经 wine_process.h extern 引入 —
  即 PLAN §2.2 点名的唯一泄漏点。
- 移出后：desktop_root 旁路在 WaylandServer::PostToplevelEvent（bus.Post 之后，
  时序与旧逐字）：`MarkDesktopShellProcesses()`（wine_process 编排，会话层
  职责）+ `FireState("evt:desktop-ready")` — 经 stateCb_ 通道，由
  napi_init.cpp SetStateCallback 注册的转发到达同一 WLState TSFN（消息
  "evt:desktop-ready"/blocking 投递/判空 `if (gStateTsfn)` 全部等价 —
  判空从 bus 侧移到 napi 侧 lambda，语义同值）。
- 结论：`napi_call_threadsafe_function`/`gStateTsfn` 符号在
  wayland_server/wl_core/xdg_shell/plugin_manager/bus 全部零引用（grep 佐证，
  仅注释溯源提及）；ToplevelCallback/StateCallback 两装配点（napi_init.cpp）
  零改动；wine_process.h 仍被 wayland_server.cpp include（MarkDesktopShellProcesses
  声明，属会话层对进程模块的编排，非 NAPI 符号依赖）。

**旁路收编结论：** `FireState("active")` 与 `"evt:desktop-ready"` 属**引擎状态
通道**（stateCb_ → ArkTS setStateCallback），而非 toplevel 事件通道 —
active 由 TryBeginSessionFirstFrame（5B1 产物，原位不动）发出，desktop-ready
经上面方案收编。二者均不再构成 compositor 核心 NAPI 依赖；无 toplevelId
承载的语义（状态迁移/补票）留在会话状态通道是对的（事件总线按
(toplevelId, eventName, json) 三维建模，旁路不满足）。

**对账结论（脚本多项集比对，0 差异）：**

1. 事件名多项集：旧 22 种（grep FireToplevelEvent 字面量 + fireEvent_
   间接路径）== 新 ToplevelEventName 映射 22 种，无缺无多；
2. JSON 模板多项集：旧 11 种（9 snprintf + `{"w":640,"h":480}` 字面量 +
   "{}" 等价类）== 新 11 种，0 差异（键名/值/顺序逐字）。
   注意：旧模板提取受 `%{public}`（hilog 占位符含 `{`）与注释引号干扰，
   对账脚本按"以 '{' 开头的字符串字面量"过滤 — 黄金值同步固化进
   host_tests/toplevel_event_test.cpp。
3. 调用计数逐项一致（22 事件名 × 每个的 fire/Post 次数；desktop_root
   2 条路径：wl_core 直接 + PromotePending 间接）。
4. 日志逐字：`[MW] FireToplevel`/`[MW] suppress`（bus.cpp 逐字搬移）、
   `[MW] argb_created`/`[XDG] fire_limits ... %s`（json.text 参数化传
   c_str()，占位符与值不变）、fire 调用点前后的业务日志零改动。

**门禁：** `make test` host_tests 全绿（geometry 77 / blit_scaled 402 /
blit_clip 38 / zorder 40 / env 21+88 / input_state 84 / shm_frame_source 39 /
**toplevel_event 62**，全 0 failures）；`make NATIVE_ARCH=arm64-v8a hap`
构建+签名通过（HAP 374M，构建后 build-profile.json5 已还原，工作树仅剩
本提交改动 + 预存 thirdparty/）。

**给阶段 6 的接口说明：** bus 已是独立模块（零 napi/wine_process 依赖），
WaylandServer 的 `PostToplevelEvent` 仅剩"转发 + desktop_root 会话旁路"一层；
阶段 6 若删转发方法，bus 可直接作为成员注入各调用方（wl_core/xdg_shell/
plugin_manager 当前经 WaylandServer 门面调用 — GetInstance 调用点不归本段）。
`ToplevelEventType` 全局名与 napi_init.cpp 的 `struct ToplevelEvent` 无冲突
（已命名规避）。

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
- **阶段 4 输入栈拆分：代码层全部完成，待设备回归** — 4A InputResolver
  裁决闭环（§二 4A）、4B SendScrollEvent 缺段修复（§二 4B，行为变化例外）、
  4C1 InputSpaceMapper 坐标收口 + PointerExtras 解环 + Policy 改名（§二
  4C1）、4C2 InputManager 拆 InputQueue/InputStateTracker/InputInjector +
  enter/leave 三变体收敛 + host_tests（§二 4C2）全部落地。
  **阶段 4 设备回归点汇总（arm64 Pad 真机或模拟器，按 4A/4B/4C 溯源码）**：
  1. 4A：红警2 直传点击路由（主菜单可点中）、全屏黑边点击/拖动（RA2 点击
     + 黑边透传）、PAL2 相对模式点击/移动、war3 光标、桌面任务栏交互；
  2. 4B：fit 缩放窗口内滚轮（桌面模式命中非 root 目标）、菜单/任务栏弹出
     层滚动路由、PC 模式窗口滚动+黑边滚动、全屏游戏滚动、拖拽窗口中滚动
     （应无事件）、最小化窗口滚动（应被抑制）— 关注 SCROLL-TARGET/
     SCROLL-ENTER/SCROLL-DROP/SCROLL-FALLBACK 日志；
  3. 4C：全输入链路冒烟 — WL_Input 日志组（PRES/PRE-ENTER/TARGET/SCROLL/
     REL/Inject*，文本与基线逐字对比）、输入无丢帧（Input-DROP 汇总）、
     拖拽窗口（move grab 期间 motion 注入总线）、热重启会话状态复位
     （快捷键残留无卡键）。
  模拟器冒烟（x86_64）与 arm64 Pad 回归均待续作承担。
- **阶段 5 协议层重构：代码层全部完成，待设备回归** — 5A1（ShmFrameSource
  纯函数）+ 5A2（CommittedSurface 快照，前序记录）+ 5B1（commit 业务段
  各归其主）+ 5B2（PopupManager 拆出）+ 5C（maximized 迁入 ToplevelState）+
  **5D（ToplevelEventBus 事件 enum 化 + JSON 构造单点 + NAPI 通道移出 core）**
  全部落地（各阶段台账见 §二）。**阶段 5 设备回归点汇总**（arm64 Pad 真机
  或模拟器）：桌面链冒烟（桌面启动/notepad 直启/任务栏交互/窗口最小化
  还原）+ 事件通道全量抽查（[MW] FireToplevel 日志事件名逐字、ArkTS
  收到的事件名/JSON 字段与基线一致 — WineWindowManager/PopupWindowManager
  无未识别事件告警）+ PC 多窗口/菜单/popup（5B2 清单）+ 窗口状态
  （5C 清单：最大化推断/全屏转换/最小化还原）+ 三游戏（war3 直传+局部
  合成+尺寸漂移、PAL2 SHM+dinput、RA2 全屏点击路由）+ ZC 遮挡/全屏/
  fallback（阶段 3 清单）。
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

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
| 3 ZC 与层序政策收口 | **进行中（3A/3D 完成，3B/3C 待做）** | f9a2d8a zc_bridge 抽离、cf7e9a1 presenter_common+GLSL、a3156a6 PresentTarget 统一 |
| 4 输入栈拆分 | 未开始 | — |
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
3. **任务 3：SHM 直传能力协商**。现状：`TryShmFullscreenDirectLocked`
   （frame_pipeline.cpp:248 起）决策引用渲染器 GL 层细节（uForceOpaque/
   GL_BLEND 等）。定义 compositor 侧 DirectPassPolicy 能力位接口，由
   egl_renderer 实现注入；决策结果逐点等价，只换知识归属。
4. **验证门禁**：每项 `make test`；全完后 `make NATIVE_ARCH=x86_64` 全量
   构建（构建后 `git checkout -- entry/build-profile.json5` 还原）；随后
   模拟器冒烟门禁 + 后台 arm64 构建门禁（同前几阶段）。
5. **性能对比（延期项）**：2A tip vs 2B tip 各临时翻 FrameTraceEnabled
   默认开构建测一次再还原，结果记录到本文档，不进提交。
6. **提交纪律**：每项独立提交，中文信息注明"行为平价（重构第 2B 步）"；
   3461a1d 是中断现场，续作完成后如需可用后续提交修复衔接，不必改写历史。

## 四、阶段 3-6 规划（摘要，详见 PLAN §四）

顺序不可重排（依赖关系见 PLAN "阶段顺序的依赖关系"）：

- **阶段 3 ZC 与层序政策收口**：3A（zc_bridge 抽离）、3D（presenter 收编 +
  PresentTarget 统一）已完成见 §二；剩余：3A 精化（GetZeroCopyOccluders 改遍历
  Layer 列表，消除第 4 份全屏语义）、3B zorder_policy 层序单点、
  3C ZeroCopyStateCoordinator 5 份状态收敛。行为敏感：需 ZC 游戏遮挡/全屏回归。
- **阶段 4 输入栈拆分**：InputResolver 裁决闭环；修 SendScrollEvent 缺段
  疑似实 bug（单独提交并标注行为变化）；InputManager 拆 InputQueue/
  InputStateTracker/InputInjector/InputSpaceMapper；两处 IsDesktopMode 改
  Policy()。行为敏感：需 PAL2/war3/RA2 输入回归（arm64 Pad，需用户配合）。
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

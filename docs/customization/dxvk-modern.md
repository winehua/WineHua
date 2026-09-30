# dxvk-modern（DXVK 2.6.2）定制

> 适用场景：改 modern 档的 DXVK、排查 modern 档下渲染问题、评估 DXVK 升级。
> 基线：上游 tag `v2.6.2`（HEAD `v2.6.2-3-gbf22f73f`）。定制共 3 个提交 / 43 文件 / +2365 / -100。
> 最后核实：2026-09-25（逐 commit 对照 diff）
> 相关文档：[dxvk-legacy.md](dxvk-legacy.md)（同一套兜底在 1.10.3 的版本，含逐文件合并清单）、[README.md](README.md)（档位背景见 [../architecture/graphics-matrix.md](../architecture/graphics-matrix.md)）

三个定制提交：

| 提交 | 日期 | 内容 | 规模 |
|---|---|---|---|
| `bcdf34d7` | 2026-08-01 | WineHua DXVK 2.6 compatibility layer（主体） | 41 文件 +2267/-99 |
| `977a3d78` | 2026-08-07 | vkd3d 2.6 swapchain factory 兼容 | 2 文件 +28 |
| `bf22f73f` | 2026-08-27 | mapped flush 区间合并收口 | 4 文件 +93/-24 |

主体是**把 legacy fork（1.10.3）的 Venus 兜底移植到 2.6 代码基**，并按 2.x 的架构变化调整实现位置。总开关从环境变量改为**设备自动识别**：`DxvkAdapter::isWineHuaVenus()`（`src/dxvk/dxvk_adapter.h`）按 `vk12.driverID == VK_DRIVER_ID_MESA_VENUS` 判定——在 Venus 设备上自动生效，没有 env 可整体关闭。

## 定制点总览

| 定制点 | 问题 | 思路 | 来源 |
|---|---|---|---|
| Venus 映射内存 flush/invalidate | shadow 映射下 CPU 写不发布宿主看不到 | Map/Unmap 链路补显式 flush，按 atom 对齐 | 移植 |
| flush 批处理合并 | 逐次 flush 在 Venus 上开销大 | cmdlist 录制期入队、提交前排序合并、每次 ≤256 range | 移植+演进 |
| dualSrcBlend 两遍模拟 | Venus 无 `dualSrcBlend` | 严格条件下拆两遍 draw，管线实例键带变体维度 | 移植 |
| BC 纹理回退 | Venus 无 `textureCompressionBC` | 格式表重映射 + 上传时 CPU 解压（复用 wine 的 bcdec.h） | 移植 |
| custom border color 模拟 | 无 `VK_EXT_custom_border_color` | cb15 UBO 传 border 色，采样后按权重混回 | 移植 |
| event query 改信号 | Venus 上 VkEvent 语义不可靠 | 改用提交完成 Fence + 单调值比较 | 移植 |
| 设备创建能力策略 | 上游硬请求桌面特性，Venus 上建设备失败 | 仅对 5 组特性按真实能力请求 | 演进（替代 1.10.3 的 env 放行） |
| vkd3d swapchain factory | vkd3d-proton 2.6 只认 DXVK 已删除的 legacy 接口 | 把上游删掉的历史回退加回来 | 恢复上游历史行为 |
| 上传/绑定 A/B 诊断 | 2.x 新路径（CopyBuffer2、动态 stride）排障需要切回老 API | 每条路径一个开关，默认全关 | 全新 |

环境变量全集（13 个，默认全关）：功能开关 `DXVK_WINEHUA_DUAL_SRC_MODE`（two-pass/secondary-replace/primary-replace）、`DXVK_WINEHUA_BATCH_MAPPED_FLUSH`、`WINEHUA_DXVK_DISABLE_CUSTOM_BORDER_EMULATION`；诊断开关 `DXVK_WINEHUA_BATCH_MAPPED_FLUSH_STATS`、`_TRACE_API`、`_TRACE_MAPPED`、`_TRACE_GEOMETRY`、`_TRACE_VIEWPORT`、`_TRACE_DEVICE_RESTART`、`_INITIAL_BUFFER_COPY`、`_STATIC_VERTEX_STRIDE`、`_HOST_VISIBLE_GEOMETRY`、`_READBACK_DEVICE_IDLE`。开关声明集中在 `src/util/util_winehua_api_trace.h`（对应 1.10.3 的 `dxvk_winehua_trace.h`；其中夹带功能开关 DUAL_SRC_MODE，不能当纯诊断文件处理）。

## 变更明细

### Venus 映射内存 flush/invalidate 协议（≈10 文件，最大一块）

**问题**：WineHua 的 Venus 传输对 host-coherent 内存持有 Guest/Host 两份映射，普通 CPU store 不会出现在 Host 侧。CPU 写必须显式 `vkFlushMappedMemoryRanges` 发布，CPU 读前必须显式 invalidate。

**思路**：
- `DxvkBuffer::flushMappedSlice(storage, slice, commandList)` / `invalidateMappedSlice`：校验 slice 归属（buffer handle 与 offset 不匹配即返回 `VK_ERROR_MEMORY_MAP_FAILED`，防跨资源错 flush），换算 memory 偏移并按 `nonCoherentAtomSize` 对齐（起始向下、结束向上）后单次执行。
- `DxvkImage::invalidateMappedRange(offset, length)`：只有 invalidate，没有 image flush。
- 门控 `m_forceMappedFlush = adapter()->isWineHuaVenus()`，设备自动。
- D3D11 接入点：Unmap（非 READ）→ `EmitCs flushMappedBuffer`（mapType 用 `D3D11_MAP(~0u)` 复位防重复 flush）；Map READ/READ_WRITE 前 invalidate（失败返回 `E_FAIL`）；UpdateSubresource 与初始化上传：非批处理模式下同步 flush（失败 throw），批处理模式下并入 cmdlist（失败仅日志）。

**与 1.10.3 的差异**：门控从 env 改为 driverID 自动；flush 落点从 `DxvkBufferHandle` 列表改为 2.3+ 分配器重写后的 `DxvkResourceAllocation`；1.10.3 的 precise-shadow 双语义与 FIFO buffer slices **未移植**——2.6 固定为「flush=发布、invalidate=可见性」的 Vulkan 原语义。

### flush 批处理（`dxvk_cmdlist.cpp/.h`、`dxvk_winehua_mapped_range.h` 新增）

**问题**：Venus 上逐次 flush 是重操作，应合并摊薄。

**思路**：录制期 `queueWineHuaMappedFlush` 入队（持 `Rc<DxvkResourceAllocation>` 防资源释放）；`submit()` 在组装 2.6 的 `DxvkCommandSubmission` 之前执行发射期处理：按 (memory, offset) 排序 → 相邻/重叠合并 → 每次调用最多 256 个 range。`bf22f73f` 的收口：合并判据抽成 `winehuaMergeMappedRange` 纯函数（入队期与发射期共用一套）；入队时先与队尾做 O(1) 合并（动态缓冲更新按分配序到达，避免无谓的排序规模）；合并判据同时比对 allocation 指针与 Vulkan memory 句柄——注释明示「memory 句柄可能被复用，单独作 lifetime 键不充分」；queued 统计改为每命令列表字段并在 `reset()` 清零。

**不变式**：flush 必须发生在提交描述组装之前；`reset()` 必须清空队列与计数；`VK_WHOLE_SIZE` 传播为合并后整段。验证：`DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1` + `_STATS=1`，`WineHuaModernMappedFlushPerf` 的 `calls` 远小于 `queued_ranges` 即合并生效。

### dualSrcBlend 两遍模拟（`dxvk_context.cpp` drawGeneric、`dxvk_graphics.cpp/.h`、`dxvk_shader.cpp/.h`）

**问题**：Venus 不暴露 `dualSrcBlend`，D3D11 的 `SRC1_COLOR` 混合（pre-multiplied alpha 的典型路径）不可用。

**思路**：`drawGeneric` 严格条件判定（单颜色 RT、fs 有输出位 1、无 logicOp/深度/模板/alpha-to-coverage、blend 因子恰好 `ONE/SRC1_COLOR` + `ONE/SRC1_ALPHA` + `ADD`）命中后派生只差 `omBlend[0]` 的两份状态，同一 render pass 内先后绑两条管线 draw。次要遍先于主要遍、两遍 writeMask 限定原值防 alpha 双加。`WineHuaDualSrcVariant` 进入管线实例键，但 secondary 变体**不建 base pipeline、不派发 worker 编译、不写 state cache**——三条 2.6 新增的加速路径都用 `variant == None` 守卫，防缓存污染。`dxvk_shader` 的 decoration 扫描按 `OpVariable Output` 变量收集 location0/1/index 三张表配对出偏移，`fsSecondaryOutput` 时 swap o0/o1 的 location decoration。任一管线创建失败 → 打一次日志后静默回退单遍直出。

**与 1.10.3 的差异**：模式从 5 种收窄到 3 种；实现点从 7 个 draw 入口收敛到单一 `drawGeneric`；新增实例键维度与加速路径绕行。

### BC 纹理回退（`d3d11_bc.cpp/.h` 新增、`dxgi_format.cpp`、`d3d11_texture.cpp`、上传链）

**问题**：Venus 无 `textureCompressionBC`，BC 贴图建不了资源。

**思路**：格式层把 21 个 DXGI BC 格式重映射（BC1/2/3/7→RGBA8、BC4→R8、BC5→RG8、BC6H→RGBA16F），unorm/srgb 拆成**单成员 family**避免 mutable image；数据层上传时 `DecodeD3D11BcImage` 整块 CPU 解压——BC1/2/3/7/6H 走 wine 的 `bcdec.h`（相对路径 `../../../wine/dlls/d3dx9_36/bcdec.h`，**与 1.10.3 相同的子模块布局耦合**），BC4/5 SNORM 自实现端点插值（-128→-127 钳制）。约束：map 非 NONE / RT|DS|UAV / shared 的 BC 模拟资源创建时直接 `throw`（注释：解码后的是像素，不是 D3D 可见的块布局，只接受 device-local sampled）。

**与 1.10.3 的差异**：门控从 env 改为设备自动；2.6 格式表新增 aspect 字段，`RemapColorFormat` 顺带补 `AspectColor`。

### custom border color 模拟（`dxbc_compiler`、`d3d11_sampler`、`d3d11_context`）

**问题**：Venus 无 `VK_EXT_custom_border_color`，任意 border 色采样失真。

**思路**：与 1.10.3 完全同构（整个 fork 里重合度最高的一块）：`dclSampler` 惰性声明 cb15（slot 15、32 向量）；SampleL 且 lod==0、float、非数组/Cube/无偏移时按 `OpImageQuerySize` + 坐标在界内外算 point/linear 权重混回 border 色；D3D11 侧每 stage 一个 UBO，SetSamplers 变更即重建内容并绑定；32 字节 `D3D11SamplerEmulationData`（borderColor + mode + U/V/W 掩码），四分派 standard/native/emulated/unsupported（comparison、各向异性、min≠mag 标 unsupported 只告警）。槽位配对约束同 1.10.3：DXBC 侧 `SamplerEmulationBindingSlotId=15` 与 D3D11 侧 `computeConstantBufferBinding(stage, DxbcConstBufBindingCount-1)` 必须一致。

### event query 改提交完成信号（`d3d11_query.cpp/.h`）

**问题**：Venus 上 VkEvent 语义不可靠（远端传输无 feedback 同步），同步 `vkGetEventStatus` 往返也有问题。

**思路**：`D3D11_QUERY_EVENT` 改 `sync::Fence` + 单调 `m_completionValue`：End 时 `ctx->signal`，GetData 比较信号值。与 1.10.3 一致。

### 设备创建能力策略（`d3d11_device.cpp` GetDeviceFeatures）

**问题**：上游对一批桌面特性无条件 `= VK_TRUE` 请求，Venus 如实上报移动栈能力时缺项 → `vkCreateDevice` 直接失败。

**思路**：`isWineHuaVenus()` 时仅对 5 组特性改按 `supported` 请求：`dualSrcBlend`、`multiViewport`、`textureCompressionBC`、`transformFeedback`、`geometryStreams`；命中打 `WineHua: applying Venus capability policy`。缺失项交给各使用点的既有回退（双源→两遍模拟、BC→解压回退）。

**与 1.10.3 的差异（演进）**：1.10.3 是 `WINEHUA_DXVK_RELAXED_FEATURES=1` 的 22 项告警放行；2.6 收敛为「5 组按真实能力请求」，不开 env、不放行——坚守 1.10.3 清单里的红线「绝不请求驱动不支持的设备特性」。**已知边界**：`geometryShader`、`imageCubeArray`、`independentBlend` 等仍无条件请求，Host 缺这些时设备仍建不起来。

### vkd3d 2.6 swapchain factory（`977a3d78`，`dxgi_factory.cpp`、`dxgi_interfaces.h`）

**问题**：**版本错配**。上游 DXVK 在 2023 年初（`9010f11a`）删除了 legacy 接口 `IWineDXGISwapChainFactory`（因为 vkd3d-proton 2.8 起改用 `IDXGIVkSwapChainFactory`）；而本项目 vkd3d-proton 停在 **2.6**，只实现 legacy 接口。D3D12 程序经 vkd3d 查 swapchain factory 必然失败 → `DXGI_ERROR_UNSUPPORTED` → 黑屏。

**思路**：把上游删掉的历史回退加回来——`QueryInterface(IDXGIVkSwapChainFactory)` 失败时退查 `IWineDXGISwapChainFactory`（GUID `53cb4ff0-c25a-4164-a891-0e83db0a7aac`）调其 `CreateSwapChainForHwnd`。接口注释明确「Used by vkd3d-proton versions before 2.8」。**隐含约束**：未来 vkd3d 升到 2.8+ 这条补丁成为死代码（新接口先命中），可整体删除。

### 2.6 新增 A/B 诊断通道（1.10.3 没有）

2.x 把初始化上传改到 `vkCmdCopyBuffer2` + SdmaBuffer、顶点绑定改成动态 stride——都是 Venus 排障时需要能切回老 API 对比的点。四个开关（默认全关，排查工具不是修复）：`DXVK_WINEHUA_INITIAL_BUFFER_COPY`（legacy/exec/exec2 三种上传组合）、`DXVK_WINEHUA_STATIC_VERTEX_STRIDE`（静态 stride + 老 bind）、`DXVK_WINEHUA_HOST_VISIBLE_GEOMETRY`（IMMUTABLE 几何缓冲强制 host-visible）、`DXVK_WINEHUA_READBACK_DEVICE_IDLE`（MAP_READ 前 waitForIdle）。另有编译期总闸 `DXVK_WINEHUA_ENABLE_API_TRACE`（默认 0）把函数级 trace 整体编译成空语句。

## 与 1.10.3 fork 的差距（评估 modern 档风险时必读）

以下 1.10.3 定制**没有**移植到 2.6：

| 未移植项 | 1.10.3 里的作用 | 风险 |
|---|---|---|
| combined sampler 模式 + bool spec 冻结 | Venus 分离描述符采样返回 0 | **最高**。2.x 描述符方案已不同、冻结逻辑理论上可删，但 2.6 fork 未验证过「分离描述符已正常」 |
| cube array Dref 模拟 / Dref 坐标补齐 | Maleoon 执行原生指令挂 host ring；最小 vec3 Dref 返回全 1 | **高**。Host GPU 缺陷与 DXVK 版本无关，含 CubeArray 阴影的内容在 modern 档可能复现 |
| RGBA8 SNORM RT 模拟 | Maleoon sampled SNORM 不能做 RT | 高，同属 Host 能力缺口 |
| host query reset 走 `vkCmdResetQueryPool` | Venus 无 `VK_EXT_host_query_reset` | 中，2.6 未验证 |
| pipelineStatistics/XFB 查询兜底 | Venus 缺这两类查询 | 中 |
| FIFO buffer slices、precise-shadow | 切片复用与读回正确性 | 中低，2.6 分配器语义已变，可能不再需要 |
| RT 转储/帧边界诊断框架（约 17 文件） | Heaven 调参遗留 | 低，2.6 的 geometry/mapped trace 覆盖了部分 |

2.6 专属模块（`dxvk_descriptor.cpp`、`dxvk_presenter.cpp`、timeline semaphore、`src/wsi/`）**零改动**。modern 档已通过设备验证的覆盖面见图形矩阵文档的档位白名单。

## 维护注意

- 本 fork 沿用 1.10.3 清单的红线：**2.x 迁移不得原地替换 legacy fork**；两版并行走 `.gitmodules` 的 `dxvk-legacy-1.10.3` / `dxvk-modern-2.6` 双分支。
- 所有开关默认关闭，关闭状态行为应与上游 v2.6.2 一致；唯一设备自动且不可关的是 flush 链路（driverID 判定），可调的只有批处理开/关。
- 升级 DXVK 2.7+ 前先看 [../architecture/graphics-matrix.md](../architecture/graphics-matrix.md) 的已知限制（需要 descriptor indexing 与 buffer device address，当前 Venus 都没暴露）。

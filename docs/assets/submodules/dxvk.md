# dxvk 补丁清单

> 基线：v1.10.3 tag（2022-08-02，winehua 独有 33 commit，`v1.10.3..HEAD` 实测 66 文件 / +6147 / -169）
> 生成：2026-08-01，统计更新：2026-09-22（下方逐文件的数字为生成时的快照）
> 说明：下行合并方向；未来 2.x 迁移时本清单是现有改动的语义来源

## 变更总览

- **新增 6 文件**（+922 行），按用途：
  - 诊断/开关集中声明：`src/dxvk/dxvk_winehua_trace.h`（+442，纯 header，全部 env 门控）
  - BC1-BC7 CPU 解码器：`src/d3d11/d3d11_bc.cpp`（+203）/ `d3d11_bc.h`（复用 Wine 自带 bcdec.h）
  - RGBA8_SNORM→RGBA16F CPU 转换：`src/d3d11/d3d11_format_convert.cpp` / `.h`
  - fork 文档：`WINEHUA_FORK.md`
- **修改 60 文件**（+5225 / -169），按功能域：
  - Venus 映射内存 / flush 语义兼容：14 文件（dxvk_buffer / dxvk_image / dxvk_cmdlist / dxvk_context 及 D3D11 map-unmap-上传链路）
  - 特性检测放宽与缺失能力兜底：4 文件（d3d11_device / d3d11_query / dxvk_cmdlist.h / dxvk_context.cpp 双源分支）
  - BC 解压落地：5 文件（dxgi_format + d3d11 上传/初始化链路）
  - RGBA8 SNORM RT 模拟：6 文件（d3d11_texture / view_rtv / view_srv / format_convert / context / texture.h）
  - DXBC/SPIR-V 编译兼容：12 文件（dxbc_compiler / options / analysis / decoder / dxvk_shader / dxvk_graphics / dxvk_compute / dxvk_sampler / d3d11_sampler）
  - 诊断/追踪（Heaven 调参遗留）：17 文件（context / cmdlist / presenter / gpu_query / barrier / image / device / context_imm / initializer / query / swapchain / dxgi_swapchain 等）
  - 构建与工具链：5 文件（meson.build / d3d10_interfaces.h / d3d9_include.h / config.h / util_bit.h）
- 说明：核心改动高度交织（如 combined-sampler 模式同时改 dxbc_compiler 声明、dxvk_shader bool 冻结、dxvk_context 描述符配对、dxbc_options 开关）；下文按文件给出语义来源，供 2.x 重写时逐条对照。

## 变更明细

### src/dxvk/dxvk_winehua_trace.h（新增，整文件）
- **为什么存在**：约 25 个 `DXVK_WINEHUA_*` / `WINEHUA_DXVK_*` env 开关的单一声明点：双源混合 5 种模式、sampled/render-pass/query/flow 四类 trace、RT 转储参数（frame/pass/draw/fs 匹配、字节上限、路径）、mapped-flush 批处理、precise-shadow、FIFO 切片、Heaven pass2 depth、force-sampled-GENERAL。宏在**调用点先查开关再构造字符串**，禁用时热路径零开销；多数 trace 带 atomic 计数抑制（如 render-pass 1024 条后停）。
- **依赖的上游行为**：仅依赖 `Logger::info/err` 与 `str::format`，无其他耦合。
- **不变式**：所有开关默认关闭；产品渲染路径不得依赖任何开关值；新增开关必须在此头登记而不是散落各处。
- **验证方法**：全关状态下与上游基线对比（commit `52322854`/`c6657078` 等曾以此对照 Heaven 帧一致）；单个开关 1/0 A/B 验证行为差异。

### src/dxvk/dxvk_context.cpp（+1834，fork 最大改动）
#### flushMappedBuffer / flushMappedImage（`DxvkContext` 新入口）
- **为什么存在**：WineHua 的 Venus vtest 桥接使用**独立的 Host Vulkan 映射**，CPU 写必须显式 `vkFlushMappedMemoryRanges` 发布，否则旧 fence 刷新影子映射时读不到新数据；D3D11 层（map/unmap、UpdateSubresource、初始化上传、HUD/gamma 缓冲）统一经 `EmitCs` 在此执行 flush，可走 cmdlist 批处理（见 dxvk_cmdlist）。
- **依赖的上游行为**：`DxvkBufferSliceHandle` / `DxvkImage` 的 memory 归属（m_buffer / m_buffers 多 backing）、`nonCoherentAtomSize`。
- **不变式**：flush 必须在相关 GPU 读取命令**之前**进入同一命令列表；offset/length 按 atom 对齐归整。
- **验证方法**：`DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1` 开启/关闭画面差异；`DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1` + `_STATS=1` 统计 range 合并率。

#### draw 入口双源混合模拟（`emulateDualSource`，drawIndexed 等 7 个入口）
- **为什么存在**：Venus 无 `dualSrcBlend` 核心特性，Heaven 的 src1 混合（ONE/SRC1_COLOR 模式）在设备上不可用。条件严格收窄（单颜色 RT、无 logicOp/深度/模板、blend 因子恰好 ONE/SRC1_COLOR + ONE/SRC1_ALPHA、fs 有输出位 1）才拆两遍 draw。
- **依赖的上游行为**：`DxvkGraphicsPipeline::getPipelineHandle(state, renderPass)` 按状态实例化管线；`DxvkGraphicsPipelineInstance` 状态匹配；dxvk_shader 的 `fsSecondaryOutput` location 重映射。
- **不变式**：次要遍先于主要遍、同一 render pass 内、无中间 barrier；两遍 writeMask 限定 RGB 防 alpha 双加；派生的两套状态只有 `omBlend[0]` 与 `ds.depthWrite` 不同；secondary 变体**不进状态缓存**；模拟失败（getPipelineHandle 返回空）静默回退单遍直出。
- **验证方法**：`DXVK_WINEHUA_DUAL_SRC_MODE=two-pass|secondary-replace|secondary-multiply|primary-replace|primary-add` 五种模式 Heaven 画面/性能 A/B（commit `910f9130`）。

#### 诊断框架（winehuaFrameBoundary / winehuaTraceDraw / winehuaCaptureTargetDraw / winehuaCaptureRenderPass / winehuaWrite*Dumps / 描述符 trace）
- **为什么存在**：定位 Heaven 在 Venus/Maleoon 上的采样异常（相机 UBO 复用、描述符身份、RT 内容）——全部 opt-in，按 frame 精确捕获一次：pass 计数、draw 参数/管线/描述符/UBO 内容哈希、push constants 字、render-pass 附件状态 JSON、`copyImageToBuffer` 到 host-visible 转储缓冲再 `waitForResource` 落盘（jsonl + bin）。
- **依赖的上游行为**：renderPassBind/UnbindFramebuffer 的 begin/end、`updateDescriptorSetWithTemplate` 的写点、`m_execBarriers`、`spillRenderPass`、`copyImageToBuffer`。
- **不变式**：所有钩子以 `unlikely(winehuaDrawTraceEnabled()...)` 前置，禁用时零字符串构造；捕获在 render pass 结束后才做 copy；转储缓冲用独立 host-visible 内存不污染正常资源。
- **验证方法**：`WINEHUA_DXVK_TRACE_DRAWS=1` + `WINEHUA_DXVK_DUMP_RT=1` 产出帧文件与离线 replay 对比（commit `92a6ebb8`~`52322854` 全链路）。

#### updateDescriptorSetWithTemplate 的 splitD3d11Binding 配对
- **为什么存在**：combined-sampler 兼容模式下 DXBC 在 t# 槽声明 combined 描述符，但 D3D11 API 仍把 sampler 存在 s# 槽；此处把 (s#, t#) 配对（`binding.slot - (DxbcResourceBindingIndex - DxbcSamplerBindingIndex)`），Texture2D.Load（无 s# 操作数）用 DXVK dummy sampler 补位（OpImageFetch 忽略 sampler）。
- **依赖的上游行为**：`DxbcStageBindingCount` 槽位布局（stage × 16 槽）、`m_common->dummyResources()`。
- **不变式**：快速路径（同槽双对象）不变；split 时按 pair 做 `trackResource`（sampler + imageView 都要 track，防 lifetime 逃逸）。
- **验证方法**：`WINEHUA_DXVK_COMBINED_SAMPLER=1` 下采样非零；`WineHuaSampled` trace 比对 written/current 身份。

### src/dxbc/dxbc_compiler.cpp（+507）
#### useCombinedImageSampler 模式（dclResource / emitLoadCombinedImage / ld / ImageQuery*）
- **为什么存在**：目标 Venus 栈上**分离 sampled-image 描述符路径采样返回零**（代码注释原文）。兼容模式不声明 image-only 变量（`varId=0`），首次采样时惰性生成 `OpTypeSampledImage` 变量 + `specConstBool(true)`（`decorateSpecId(bindingId)`），描述符类型改为 COMBINED_IMAGE_SAMPLER；`ld/ld2ms`、`resinfo/resquerylevels/resquery samples` 经 `OpImage` 提取 image 分量。
- **依赖的上游行为**：`m_textures`/`m_samplers` 表、`computeSrvBinding` 槽位算法、`getResourceType` 的 image type。
- **不变式**：开启时绝不再 `opLoad(texture.varId)`（旧路径会生成悬空 id 0 → Venus 设备/ring 挂起）；bindingId 必须与 D3D11 侧 t# 槽一致（dxvk_context 的 split 配对依赖它）；配套 bool 特化常量必须冻结（见 dxvk_shader）。
- **验证方法**：`WINEHUA_DXVK_COMBINED_SAMPLER=1` 采样结果非零；关闭时行为与上游一致（默认关闭）。

#### emulateCubeArrayDref（dclResource + emitCubeArrayTo2DArrayCoord + Sample/Gather 坐标 + resinfo）
- **为什么存在**：Maleoon 执行原生 CubeArray shadow/Dref 指令会**挂起 Host Venus ring**。声明期把 affected 资源降为 2D-array 类型，shader 内把 (direction, cubeIndex) 转成 (uv, cubeIndex*6+face)——face 判定按 x/y/z major 轴实现，layer 计算保留 Vulkan `floor(layer+0.5)` 规则；`resinfo` 的 depth 分量除 6 还原。
- **依赖的上游行为**：`dxbc_analysis` 预扫描的 `accessCubeArrayDref` 标记（SampleC/Clz、Gather4C/PoC 操作数 1/2）；D3D11 侧绑定同资源 2D-array 视图。
- **不变式**：转换仅作用于被 analysis 标记的资源；坐标加载放宽为 4 分量；仍走原生 `OpImageSampleDref` 保留 compare-before-filter 语义。
- **验证方法**：Maleoon 设备跑含 CubeArray 阴影内容不挂 ring；`WINEHUA_DXVK_EMULATE_CUBE_ARRAY_DREF=0/1` 与 `WINEHUA_DXVK_QUIRKS=maleoon-cube-array-dref` A/B。

#### padCubeDrefCoordinates（SampleC/SampleD 的 Dref 坐标）
- **为什么存在**：Maleoon 片段编译器对最小 vec3 形式 Cube `OpImageSampleDref*` 返回**全 1 结果**，vec4(direction, dref) 正确。
- **依赖的上游行为**：SPIR-V 允许 Dref 坐标超宽（合法 vec4）。
- **不变式**：仅 PixelShader + 非数组 Cube + Dref 生效；CubeArray 走另一条契约不受影响。
- **验证方法**：`WINEHUA_DXVK_PAD_CUBE_DREF_COORD=0/1`、`WINEHUA_DXVK_QUIRKS=maleoon-cube-dref`。

#### emulateCustomBorderColor（emitCustomBorderColorCorrection / cb15 声明）
- **为什么存在**：Venus 无 `VK_EXT_custom_border_color`（`customBorderColorWithoutFormat=0`）。SampleL 后用 `OpImageQuerySize` + 坐标在 [0,size) 内/外加权，把 border 色按 point/linear 权重混入结果；border 色与 mode/U/V/W 掩码经 cb15（`SamplerEmulationBindingSlotId=15`）UBO 传入。
- **依赖的上游行为**：dclSampler 处的 cb 惰性声明（32 向量）、`emitQueryTextureSize`、SampleL 结果值。
- **不变式**：仅 float、非数组、非 Cube、无偏移（u/v/w=0）采样生效；mode 0 表示不修正；cb15 在 D3D11 侧由 `D3D11DeviceContext::UpdateSamplerEmulationBuffer` 维护（见下）。
- **验证方法**：`WINEHUA_DXVK_DISABLE_CUSTOM_BORDER_EMULATION=1` 前后 border 颜色差异；`WineHua: custom-border path=` 日志分派。

### src/dxvk/dxvk_cmdlist.cpp（+209）
- **为什么存在**：逐次 `vkFlushMappedMemoryRanges` 在 Venus 上开销大（commit `afc9c2ac`）。`queueWineHuaMappedFlush` 在录制期入队（持 `Rc<DxvkResource>` 防释放），`submit()` 前置 `flushWineHuaMappedFlushes`：按 (memory, offset) 排序、相邻/重叠区间合并、每次调用最多 256 区间；`_STATS` 模式输出 calls/bytes/whole-range 统计。另有录制期诊断：`winehuaRecordingId`（全局递增）、每 cmdlist 的帧清单、present copy 的源/目标 image 身份（submit 时打印）。
- **依赖的上游行为**：`DxvkCommandList::submit` 的提交描述组装时机、`beginRecording/reset` 生命周期。
- **不变式**：flush 必须发生在提交描述入队**之前**且不改变命令内容；reset 时清空 flush 队列；exec buffer 句柄仅作日志身份，不得影响录制/提交行为（dxvk_cmdlist.h 注释约束）。
- **验证方法**：`DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1` + `_STATS=1`（calls 远小于 queued_ranges 即合并生效）；`WineHuaDxvkSubmit` 日志核对帧归属。

### src/dxvk/dxvk_shader.cpp（+207）
#### freezeBoolSpecConstants / dxvkWineHuaFreezeBoolSpec
- **为什么存在**：Venus/Maleoon 对 bool 特化常量（1.10.3 用其表达绑定掩码，SpecId=0..bindingCount-1）处理错误；在 SPIR-V 二进制上把 `OpSpecConstantTrue/False` 重写为 `OpConstantTrue/False`（值取自绑定掩码）、删掉 `SpecId` decoration。
- **依赖的上游行为**：1.10.3 的 `DxvkShaderModule::createShaderModule` 阶段（此时绑定掩码已知）、`SpirvCodeBuffer` 指令遍历。
- **不变式**：冻结必须发生在 vkCreateShaderModule 之前；仅 binding 类 specId（< bindingCount）用掩码值，其他 bool spec（固定功能遗留）保留 SPIR-V 默认值；冻结后绑定掩码变化 = 新管线实例（管线变体数上升是预期代价）。
- **验证方法**：Venus/Maleoon 默认自动开启（deviceName 含 `Venus`/`Maleoon`）；`DXVK_WINEHUA_FREEZE_BOOL_SPEC=0` 复现采样错误、`WINEHUA_DXVK_QUIRKS=venus-bool-spec` 为新适配器强开。

#### fsSecondaryOutput 双源输出重映射 + o0 追踪
- **为什么存在**：1.10.3 只有 dualSrcBlend 的 index/location 交换；两遍模拟需要把 o1 输出交换 location 0/1（`fsSecondaryOutput` 分支 `swap(code[m_o0LocOffset], code[m_o1LocOffset])`）。为此把 location0/location1/index 的 decoration offset 收集改为按 `OpVariable Output` 变量配对（m_o0LocOffset 新增）。
- **依赖的上游行为**：DecorationLocation/Index 扫描循环、dualSrcBlend 的既有 remap。
- **不变式**：仅 fs 生效；dualSrcBlend 与 fsSecondaryOutput 互斥分支。
- **验证方法**：双源模式 A/B 时 `WineHuaDualSrcEmulation` 日志 + 画面正确。

#### SPIR-V 转储（DXVK_WINEHUA_DUMP_REMAPPED_SPIRV + DXVK_SHADER_DUMP_PATH）
- **为什么存在**：普通 dump 在 bool 冻结/remap 之前发生，replay 用的是不同二进制；此处按 FNV-1a 哈希命名落盘**最终喂给 vkCreateShaderModule 的模块**（m_winehuaVariantId）。
- **不变式**：仅 env 开启时写文件；不改变管线创建结果。
- **验证方法**：对比 dump 与 replay 用模块哈希一致（commit `53b12ec0` 的 exact-replay 前提）。

### src/d3d11/d3d11_bc.cpp（新增 +203）/ d3d11_bc.h（新增）
- **为什么存在**：移动 Vulkan（Venus）无 `textureCompressionBC`。`DecodeD3D11BcImage` 在上传时把 BC1-BC7 块解压为未压缩像素：BC1/2/3/7 走 Wine 已 vendored 的 `bcdec.h`（`WINE_UNUSED` 强制启用 BC6H/BC7 例程，避免第二份解码器拷贝）；BC4/5 SNORM 自实现端点插值（含 -128→-127 钳制）；BC6H 用 half 输出补 alpha=1.0。
- **依赖的上游行为**：`../../../wine/dlls/d3dx9_36/bcdec.h` 的相对路径（**子模块布局耦合**，2.x 重写时注意）；调用方 `D3D11CpuImage` 结构。
- **不变式**：源 rowPitch/slicePitch 校验（≥块行宽，缺省按最小）；输出尺寸溢出检查；整次上传是 CPU 路径（无 readback、无 per-sample shader 替换）。
- **验证方法**：`WINEHUA_DXVK_BC_EMULATION=0/1` 纹理正确性对比（解压 vs 原生）；BC6H/7 游戏贴图目测。

### src/d3d11/d3d11_format_convert.cpp（新增 +103）/ .h（新增）
- **为什么存在**：RGBA8 SNORM RT 模拟（见 d3d11_texture）需要把 D3D 可见的 R8G8B8A8_SNORM 字节布局转成 R16G16B16A16_SFLOAT（域保持：-128→-1、127→1、其余 n/127），`Float32ToFloat16` 手写转换（含 round-to-nearest、次正规、溢出到 inf）。
- **依赖的上游行为**：调用方（d3d11_context / d3d11_initializer）的上传循环与 pitch 约定。
- **不变式**：源/目的 pitch 溢出检查；转换在 CPU 完成，GPU 侧永远只见目标格式。
- **验证方法**：`DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=1` 时负值 RT 内容正确（Heaven 场景 A/B）。

### src/d3d11/d3d11_device.cpp
- **为什么存在**：Venus 缺桌面特性（dualSrcBlend、multiViewport、transformFeedback、textureCompressionBC、hostQueryReset、shaderDrawParameters 等）。`WINEHUA_DXVK_RELAXED_FEATURES=1` 时 `checkFeatureSupport` 失败仅逐项告警放行（22 项 WINEHUA_LOG_MISSING）；`textureCompressionBC` 在 BC 模拟路径下按 supported 值请求（**绝不请求驱动不支持的设备特性**）；transformFeedback 缺失时流输出调用仍不可用（D3D11 层已有 XFB 门控）。
- **依赖的上游行为**：`DxvkDeviceFeatures` 枚举、`GetFormatSupportFlags`。
- **不变式**：relaxed 模式只在 env=1 时生效（默认严格）；缺失特性以 warning 记录以便诊断。
- **验证方法**：Venus 设备默认不能建设备 → 设 `WINEHUA_DXVK_RELAXED_FEATURES=1` 可建；日志逐项核对缺失项。

### src/d3d11/d3d11_query.cpp / .h
- **为什么存在**：Venus 缺 `pipelineStatisticsQuery` 与 transform feedback 查询 → 按特性门控（缺失返回零统计 + 一次告警）；**D3D11 event query 从 VkEvent（DxvkGpuEvent）改为 `sync::Fence` 提交完成信号**（`m_completionSignal` + 单调 `m_completionValue`，GetData 比较 value），规避 Venus 上 VkEvent 语义不可靠。
- **依赖的上游行为**：上游的 `signalGpuEvent` / `m_event[0]->test()` 事件模型、CS 提交链。
- **不变式**：`m_query[0]` 可能为空，begin/end 需判空；事件状态机（Pending→Signaled）对外接口不变。
- **验证方法**：`DXVK_WINEHUA_TRACE_QUERY=1` 看 `backend=submit-completion`；缺失特性设备上查询返回 0 且不崩溃。

### src/dxvk/dxvk_cmdlist.h（resetQueryPool 分支）
- **为什么存在**：Venus 无 `VK_EXT_host_query_reset` → `DXVK_WINEHUA_COMMAND_QUERY_RESET=1` 改走 `vkCmdResetQueryPool`（入 InitBuffer），与 host 重置二选一。
- **不变式**：两条路径互斥且每次 reset 只走一条；cmd 路径需标记 InitBuffer 已用。
- **验证方法**：两种模式查询结果一致（`WineHuaQuery: reset mode=...` 日志）。

### src/dxvk/dxvk_buffer.cpp / .h
- **为什么存在**：flush/invalidate 三件套（见 dxvk_context 条目）实现在此：`flushMappedSlice`（发布 CPU 写）、`invalidateMappedSlice`（Host→Guest 可见性）、`beginMappedSliceWrite`——legacy 桥接语义下它是**带外"CPU 写开始"标记**（内部做 invalidate），precise-shadow 模式（`DXVK_WINEHUA_PRECISE_SHADOW=1`）下恢复 Vulkan 语义（begin 为 no-op，invalidate 只表示可见性）。`m_fifoSlices`（`DXVK_WINEHUA_FIFO_BUFFER_SLICES=1`）：host-visible UBO 的空闲切片回收从 LIFO 改 FIFO（allocSlice 前 reverse），避免立刻复用手头刚释放且未 flush 的切片。
- **依赖的上游行为**：多 backing buffer（`m_buffers` 重命名列表）、`allocSlice/freeSlice` 纪律。
- **不变式**：所有映射操作按 nonCoherentAtomSize 对齐；flush 可走 cmdlist 批处理（`winehuaBatchMappedFlush()` 时）。
- **验证方法**：`DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED` 与 `_PRECISE_SHADOW` 组合 A/B；FIFO 开关对 WRITE_DISCARD 帧率的影响。

### src/dxvk/dxvk_image.cpp / .h
- **为什么存在**：纹理直接映射（map mode DIRECT）的 flush/invalidate 经 `syncMappedRange`（atom 对齐、长度钳制到 memory 长度）；`D3D11DeviceContext::Map/Unmap` 纹理路径调用（见 context_imm）。另有 image-create trace（R8G8B8A8_UNORM 小图与 6 种深度格式，`DXVK_WINEHUA_TRACE_SAMPLED` 门控）。
- **不变式**：`flushMappedRange`/`invalidateMappedRange` 仅在存在 memory 时有效；`offset >= length` 返回 `VK_ERROR_MEMORY_MAP_FAILED`。
- **验证方法**：precise-shadow 下纹理读回正确；`WineHuaSampled: image-create` 日志核对布局。

### src/d3d11/d3d11_context_imm.cpp
- **为什么存在**：Map/Unmap 的完整映射协议：Map 记录 `SetMapType` + `beginMappedSliceWrite`（buffer 与纹理两条路径）；Unmap 时非 READ 模式 `EmitCs flushMappedBuffer`；precise-shadow 下 READ/READ_WRITE 的 Map 前 `invalidateMappedSlice/invalidateMappedRange`（失败返回 E_FAIL）；WRITE_DISCARD 后 `GetMappedBufferSlice` 绑定（见 d3d11_buffer.h）。另有 GetData/Flush/ExecuteCommandList/UpdateSubresource/隐式 flush 的 flow trace（含空 flush 抑制）。
- **依赖的上游行为**：上游 Map/Unmap 的 mapMode 分派（DIRECT/STAGING）、`GetMappedSlice`。
- **不变式**：flush 与 invalidate 必须成对围绕 CPU 写区间（precise 模式）；mapType 以 `D3D11_MAP(~0u)` 复位防重复 flush。
- **验证方法**：`DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1` 全链路画面一致；`WineHuaSampled: dynamic-mapped-flush` 日志。

### src/d3d11/d3d11_context.cpp
- **为什么存在**：三大块。(1) **Sampler 模拟 UBO**：`m_samplerEmulationBuffers[6]`（每 stage 一个 host-visible+device-local buffer，cb15），SetSamplers 变更时 `UpdateSamplerEmulationBuffer` 重建内容并 EmitCs `flushMappedBuffer + invalidateBuffer + bindResourceBuffer`；UnbindConstantBuffers 循环改为 `DxbcConstBufBindingCount`（含 slot 15）。(2) **UpdateSubresource 转换**：BC 模拟走 `DecodeD3D11BcImage`、SNORM 模拟走 `ConvertD3D11Rgba8SnormToRgba16Float`，经 staging 上传；原生/模拟 SNORM 混合 CopyImage 拒绝（texel 尺寸不同，防内存损坏）。(3) Map 时 buffer slice 用 `GetMappedBufferSlice`（WRITE_DISCARD rename 后避免物理偏移二次叠加）。
- **依赖的上游行为**：`computeConstantBufferBinding(stage, 15)` 槽位约定（DXBC 侧 `SamplerEmulationBindingSlotId=15`）、`AllocStagingBuffer`、`UpdateImage`。
- **不变式**：cb15 只在 `m_samplerEmulationEnabled` 时声明/绑定；转换失败必须 `return`（不半上传）；mixed-copy 拒绝只在两纹理模拟状态不一致时触发。
- **验证方法**：`WINEHUA_DXVK_DISABLE_CUSTOM_BORDER_EMULATION` A/B；BC/SNORM 纹理上传后采样正确。

### src/d3d11/d3d11_buffer.h
- **为什么存在**：`GetMapType/SetMapType`（Unmap 判定是否需要 flush）与 `GetMappedBufferSlice`——返回逻辑子区间绑定（物理 rename 偏移由 CS 侧 `invalidateBuffer` 生效，注释明确"再加一次物理偏移会双重叠加"）。
- **不变式**：`DxvkBufferSlice(m_buffer, offset, length)` 直接用逻辑 offset。
- **验证方法**：WRITE_DISCARD 高频场景画面/偏移正确（配合 `WineHuaSampled: dynamic-cb-invalidate` 日志）。

### src/d3d11/d3d11_initializer.cpp
- **为什么存在**：初始化上传同样需要 BC 解压/SNORM 转换（`DecodeD3D11BcImage` / `ConvertD3D11Rgba8SnormToRgba16Float`，mip 循环内按 subresource 转换后替换 uploadData/pitch）；`winehuaFlushDynamicMapped` 时对 staging/init buffer 显式 flush；flow trace。
- **不变式**：转换后 `m_transferMemory` 按转换后字节计；packImageData 始终消费转换后的数据指针。
- **验证方法**：冷启动 Heaven 贴图正确；`d3d11-init-*` flow 日志。

### src/d3d11/d3d11_context_def.cpp
- **为什么存在**：CopyResource 拷贝路径（deferred context）同样加目标 `flushMappedBuffer`。
- **验证方法**：deferred 路径拷贝后内容正确。

### src/d3d11/d3d11_shader.cpp
- **为什么存在**：着色器 uniform buffer（`m_buffer`）初始 memcpy 后 `flushMappedSlice`。
- **验证方法**：shader uniform 生效（如 Heaven 后期参数）。

### src/dxvk/dxvk_swapchain_blitter.cpp / src/dxvk/hud/dxvk_hud_renderer.cpp
- **为什么存在**：gamma LUT 缓冲与 HUD 顶点缓冲的 CPU 写后 flush（`winehuaFlushDynamicMapped` 门控）。
- **验证方法**：HUD 正常显示、gamma 生效。

### src/d3d11/d3d11_texture.cpp / .h
- **为什么存在**：(1) **RGBA8 SNORM RT 模拟**：Maleoon 暴露 sampled R8G8B8A8_SNORM 但不支持 color attachment → `DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=1/on/auto` 时，资源本地替换为 R16G16B16A16_SFLOAT（去掉 MUTABLE、清 viewFormats、tiling optimal→linear 回退）；`GetViewFormat` 重定向 view 格式、`IsRgba8SnormRtEmulated` 供上传/拷贝判定。(2) **BC 模拟约束**：map 模式非 NONE、RT/DS/UAV 绑定、shared 语义的 BC 纹理创建时 `throw`（只支持 device-local sampled）。(3) `winehuaForceSampledGeneral`（GENERAL vs SHADER_READ_ONLY_OPTIMAL 的 A/B）。(4) format trace（image-query + SNORM 候选格式探测）。
- **依赖的上游行为**：`D3D11CommonTexture::Create` 的 CheckImageSupport 判定点、view 创建时 `LookupFormat`。
- **不变式**：模拟是资源本地决策，格式能力查询（CheckFormatSupport）保持真实；模拟资源绝不进共享/交换链路径。
- **验证方法**：`DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=1` 下 RT 内容正确（负值保留）；BC 异常绑定路径在日志中抛错而非损坏内存。

### src/d3d11/d3d11_view_rtv.cpp / d3d11_view_srv.cpp
- **为什么存在**：RTV/SRV 创建时 `formatInfo.Format = texture->GetViewFormat(pDesc->Format)`（SNORM 模拟资源的 view 指向 16F 物理格式）。
- **不变式**：仅模拟纹理重定向；普通纹理走原 LookupFormat。
- **验证方法**：模拟 RT 渲染 + 采样链完整工作。

### src/dxgi/dxgi_format.cpp
- **为什么存在**：无 `textureCompressionBC` 时（`WINEHUA_DXVK_BC_EMULATION != "0"`）把 15 个 DXGI BC 格式映射到未压缩 VkFormat（BC1-3/7→RGBA8、BC4→R8、BC5→RG8、BC6H→RGBA16F）并覆写 `m_dxgiFamilies`；**具体格式用单元素 family**（如 BC1_UNORM→仅 R8G8B8A8_UNORM）避免 mutable image——Venus 的 mutable-image 能力查询开销/不可用（commit `bea4b847`），typeless 保留 unorm/snorm 或 sRGB 双成员 family。
- **依赖的上游行为**：`RemapColorFormat` 机制、`m_dxgiFamilies` 在 view 创建时驱动格式族。
- **不变式**：BC 模拟与 device 的 BC feature 请求互斥（d3d11_device 侧保证）；SRGB 映射保持 SRGB 目标格式。
- **验证方法**：`WineHua: BC1-BC7 upload-time decompression enabled` 日志；BC 纹理目测与 format trace。

### src/dxbc/dxbc_options.cpp / .h
- **为什么存在**：四个编译选项的解析点：`useCombinedImageSampler`（纯 env）、`emulateCustomBorderColor`（无扩展且未禁用）、`padCubeDrefCoordinates` 与 `emulateCubeArrayDref`（**env 覆盖 > WINEHUA_DXVK_QUIRKS > adapter 名默认**：Maleoon 自动开启两条 quirk）。启动时打印三条路径日志。
- **不变式**：选项是 per-adapter 快照（设备创建后固定）；默认值对非 Maleoon/Venus 设备全部为 false。
- **验证方法**：启动日志 `WineHua: Cube Dref coordinate path=...` 等三行核对。

### src/dxbc/dxbc_analysis.cpp / .h
- **为什么存在**：声明期需要知道 SRV 是否被 Dref 使用（CubeArray 模拟是声明级类型决策）：扫描 SampleC/SampleClz（src[1]）与 Gather4C/Gather4PoC（src[1]/src[2]），写入 `srvInfos[reg].accessCubeArrayDref`。
- **不变式**：仅 Resource 操作数标记；128 槽上限。
- **验证方法**：`WineHua: tN CubeArray Dref uses shader-side 2D-array emulation` 日志与 shader 实际指令一致。

### src/dxbc/dxbc_decoder.h / dxbc_compiler.h
- **为什么存在**：`DxbcShaderResource`/`DxbcSampler` 增加 `bindingId`、`emulateCubeArrayDref` 字段（combined 惰性声明需要记住槽位）；compiler 头登记三个新 emit 方法声明。
- **不变式**：字段默认值保持上游行为（bindingId=0 仅 combined 模式消费）。
- **验证方法**：随各功能一并回归。

### src/dxvk/dxvk_graphics.cpp / .h
- **为什么存在**：(1) 管线实例新增 `secondaryOutput` 维度：`findInstance/createInstance/getPipelineHandle(state, rp, secondaryOutput)`，实例匹配条件含 `m_secondaryOutput`，secondary 变体不写状态缓存（防 2.x 缓存污染）。(2) `createShaderModule` 传入 `info.freezeBoolSpec`（绑定掩码冻结）与 `info.fsSecondaryOutput`。(3) `WINEHUA_DXVK_FORCE_HEAVEN_PASS2_DEPTH_ALWAYS=1` quirk：匹配 Heaven pass2 特征（R16G16B16A16_SFLOAT 单色 + D24S8 深度）时强制 compare ALWAYS。(4) 管线创建 flow trace + `WineHuaPipelineVariant`（SPIR-V dump 变体 id 关联）。
- **依赖的上游行为**：`DxvkGraphicsPipelineInstance` 状态向量、`writePipelineStateToCache` 时机。
- **不变式**：secondary 与 primary 共用同一 `DxvkGraphicsPipeline` 对象，只差状态参数；`!secondaryOutput` 才缓存。
- **验证方法**：双源模式 A/B；Heaven depth quirk 开关对比。

### src/dxvk/dxvk_compute.cpp
- **为什么存在**：compute 管线同样接 `freezeBoolSpec`（binding 掩码冻结）+ flow trace。
- **不变式**：与 graphics 同策略。
- **验证方法**：compute shader（如后期处理）在 Venus 上正常。

### src/dxvk/dxvk_shader.h
- **为什么存在**：`DxvkShaderModuleCreateInfo` 增加 `fsSecondaryOutput/freezeBoolSpec/boolSpecMask/boolSpecCount`；`DxvkShaderModule` 增加 `m_o0LocOffset` 与 `winehuaVariantId()`；声明 `dxvkWineHuaFreezeBoolSpec`。
- **验证方法**：随 graphics/compute 回归。

### src/dxvk/dxvk_sampler.cpp / .h
- **为什么存在**：采样器创建时保存 `m_info`（去 pNext 以便对比）+ `m_customBorderColor`，提供 `info()/customBorderColor()/compareToDepth()/compareOp()` 访问器供描述符 trace 与边界判定；比较采样器创建打日志。
- **依赖的上游行为**：`DxvkSamplerCreateInfo` 的 borderColor/customBorderColor 传递。
- **不变式**：`m_info.pNext = nullptr` 后才保存（防悬挂链）。
- **验证方法**：`WineHua: comparison-sampler host=...` 日志与实参一致。

### src/d3d11/d3d11_sampler.cpp / .h
- **为什么存在**：custom border 三分派：标准色（0/0/0/0、0/0/0/1、1/1/1/1）→ 原生路径；设备有 `customBorderColorWithoutFormat` → native；否则生成 `D3D11SamplerEmulationData`（32 字节：borderColor[4] + mode + U/V/W 掩码，mode 1=point/2=linear），comparison、各向异性、min≠mag 三种组合标记 unsupported 并告警（shader 模拟不做这三类）。
- **不变式**：`static_assert(sizeof == 32)`；非 BORDER 寻址采样器不产生模拟数据。
- **验证方法**：`WineHua: custom-border path=emulated|native|unsupported` 日志 + 边缘像素 A/B。

### src/dxvk/dxvk_gpu_query.cpp
- **为什么存在**：query begin/end/result 的 `WineHuaQuery` trace（pool/id/type/vkResult）。
- **验证方法**：`DXVK_WINEHUA_TRACE_QUERY=1`。

### src/dxvk/dxvk_barrier.cpp / dxvk_image.cpp（trace 部分）
- **为什么存在**：sampled 图像（R8G8B8A8_UNORM 小图 + 6 种深度格式）barrier 与创建 trace——Heaven 采样异常调查的依赖（布局/阶段/访问位全程可见）。
- **不变式**：仅 `DXVK_WINEHUA_TRACE_SAMPLED=1` 生效。
- **验证方法**：对比 trace 中布局与预期。

### src/d3d11/d3d11_swapchain.cpp / .h
- **为什么存在**：Present 前记录 backbuffer→presenter copy 的身份（`winehuaTracePresentCopy`）；`SubmitPresent` 增加 `NextFrameId` 参数，最后一遍 present 后在 CS 流插入 `winehuaFrameBoundary(nextFrameId)`——frame 计数为 capture 路径提供明确边界（capture 帧号以 D3D11 Present 为准而非 Vulkan submit）。
- **不变式**：frameId 只在 capture 路径消费；SyncInterval>1 时仅最后一遍携带边界。
- **验证方法**：`WINEHUA_DXVK_DUMP_RT=1` 的 frame 编号与 Present 次数对齐。

### src/dxgi/dxgi_swapchain.cpp / src/vulkan/vulkan_presenter.cpp / .h
- **为什么存在**：present 链路诊断：`WineHuaPresentImage`（acquire/present/image-map 三事件，swapchain/image 身份 + 序列号 `m_winehuaPresentSequence`）、present flow trace。用于核对 DXVK 层 vs 鸿蒙侧 present 的 image 归属。
- **不变式**：trace 仅日志，不改变 acquire/present 时序。
- **验证方法**：`WINEHUA_DXVK_TRACE_PRESENT_IMAGE=1` 与 `WineHuaPresentCopy` 日志交叉核对（commit `df55b90e`/`2de8230b`）。

### src/d3d11/d3d11_cmdlist.cpp
- **为什么存在**：deferred command list 发射时 flow trace（chunks/queries/resources/submitted 计数）。
- **验证方法**：`DXVK_WINEHUA_TRACE_FLOW=1`。

### src/d3d10/d3d10_interfaces.h
- **为什么存在**：MinGW 的 d3d10effect.h 已声明 d3d10 UUID，重复声明编译失败 → `#elif !defined(__MINGW32__)`（commit `3a505bfd`）。
- **验证方法**：MinGW 工具链编译通过。

### src/d3d9/d3d9_include.h
- **为什么存在**：现代 MinGW 的 d3d9types.h 自带 `D3DDEVINFO_RESOURCEMANAGER`，旧回退定义改为仅非 MinGW（`!defined(_MSC_VER) && !defined(__MINGW32__)`）（commit `8c508c35`）。
- **验证方法**：MinGW 编译通过。

### src/util/config/config.h / src/util/util_bit.h
- **为什么存在**：显式 `#include <cstdint>`（commit `e991d184`，工具链头文件依赖差异）。
- **验证方法**：编译通过。

### src/d3d11/meson.build
- **为什么存在**：登记 `d3d11_bc.cpp`、`d3d11_format_convert.cpp` 两个新源文件。
- **验证方法**：`make dxvk` 构建产物含新符号。

### WINEHUA_FORK.md（新增）
- **为什么存在**：fork 边界文档：Legacy（dxvk-legacy-1.10.3）与未来 Modern profile 分离、构建入口、全部 opt-in 开关与 Maleoon quirk 的政策说明、指向 `docs/decisions/0002-d3d-backend-profiles.md` 的升级就绪记录。
- **不变式**：2.x 迁移不得原地替换本 fork（文档第 44-58 行明令）。
- **验证方法**：对照本清单逐项可追溯。

## 附注

- 上游行为依赖风险最高的三点（2.x 重写时必须先确认上游是否仍保留）：(1) 1.10.3 的绑定掩码 bool spec 常量机制（2.x 已改用其他描述符方案，冻结逻辑可整体删除）；(2) 1.10.3 的 `updateDescriptorSetWithTemplate` 描述符模板路径（split 配对依赖其槽位布局）；(3) `wine/dlls/d3dx9_36/bcdec.h` 相对包含路径（子模块布局耦合）。
- 诊断类改动（约占 diff 一半）在 2.x 中应整体按"弃用-重写"处理，语义来源只有环境变量名与日志格式，不复用实现。
- 未标注 [待确认] 的条目均经代码核实；commit 数 28（任务背景中的 65 与实际 `v1.10.3..HEAD` 不符，以实测为准）。

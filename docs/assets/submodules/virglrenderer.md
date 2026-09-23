# virglrenderer 补丁清单

> 基线：8cb58e47（2026-06-10，upstream/main merge-base）
> 生成：2026-08-01，统计更新：2026-09-22
> 说明：本清单服务未来合并上游 main 时确认每个 hunk 的意图与不变式
> 范围：8cb58e47..HEAD，53 files（修改 52 / 新增 1），+8942/-955，39 个 commit

## 变更总览

按用途分类（全部变更均服务于鸿蒙 OHOS 平台上的 Maleoon GPU 驱动 + OHNativeWindow 合成器）：

| 类别 | 文件 | 性质 |
|------|------|------|
| OHOS shadow 内存兼容路径（核心） | vkr_device_memory.c/h、vkr_buffer.c/h、vkr_queue.c/h、vkr_context.c/h、vkr_descriptor_set.c/h、vkr_command_buffer.c、vkr_ring.c/h、vkr_transport.c | 功能 + 诊断 |
| WineHua present 桥（vtest→Venus→平台合成器） | vkr_renderer.c/h、vkr_device.c/h、vkr_instance.c、vkr_physical_device.c/h、vkr_image.c/h、virglrenderer.c/h、vtest/vtest_renderer.c、vtest/vtest_server.c、vtest/vtest_protocol.h、vtest/vtest.h、vtest/meson.build、**vtest/winehua_vtest_server.c（新增）** | 功能 |
| Venus 诊断埋点（Heaven UBO 问题定位） | vkr_pipeline.c/h、vkr_render_pass.c、vkr_command_buffer.c、vkr_descriptor_set.c | 诊断（默认关闭） |
| VirGL/GLES 宿主侧适配 | vrend_winsys.c、vrend_winsys_egl.c、vrend_formats.c、vrend_renderer.c/h、vrend_decode.c | 功能 |
| 同进程 render server 适配 | server/render_context.c、src/proxy/proxy_context.c | 功能 |
| 构建系统 | meson.build、meson_options.txt | 功能 + 大量空白噪音 |

**合并注意**：meson.build / meson_options.txt / vrend_winsys.c 的 diff 大部分是 CRLF 换行转换造成的假改动（`git diff -w` 后真实改动分别只有 15/14/17 行）。合并上游时应以 `-w` 视图为准，避免被噪音淹没。

## 变更明细

### 一、OHOS shadow 内存兼容路径（核心，合并冲突敏感区）

背景：鸿蒙 Maleoon Vulkan 驱动不支持 Linux dma-buf 外部内存 fd 导出（沙箱内也无法用 guest 映射的 dma-buf 直接做 host 端 Vulkan 内存），因此 host-visible 内存分配改为：host 侧创建一个匿名文件 shadow（`os_create_anonymous_file` + mmap）导出给 guest（VIRGL_RESOURCE_FD_SHM），guest 写 shadow 映射；host 侧同时 `vkMapMemory` 真实 VkDeviceMemory，在 queue submit / flush / fence 信号时机做 shadow↔host 的 memcpy 与缓存域转换。

#### vkr_device_memory.c: vkr_device_memory_export_blob（OHOS shadow 分配路径）
- **为什么存在**：Maleoon 无外部内存 fd 导出，Host-visible blob 无法按上游路径 `virgl_gbm_bo_create`/`export_blob` 走 memfd/dma-buf。OHOS 分支改为：拒绝 CROSS_DEVICE，创建匿名 shadow fd + mmap（对齐页大小），`vkMapMemory` 真实内存（仅 mappable 时），非 coherent 先 Invalidate，随后把 host 初始内容 memcpy 进 shadow，最后以 `VIRGL_RESOURCE_FD_SHM` + 导出的 fd 返回 guest。
- **依赖的上游行为**：`vkr_context_create_resource_from_device_memory` 中 upstream 对非 DMABUF 的 blob 只存 fd 不导入；OHOS 分支在 vkr_context.c 里新增了对 `VIRGL_RESOURCE_FD_SHM` 的直接 `vkr_context_import_resource_from_shm` 导入路径（guest 的 res 由共享内存直接映射，无需 memfd seals）。
- **不变式**：guest 拿到的 shadow 映射必须与 host_map 初始内容一致（否则首帧前 guest 读到垃圾）；所有 shadow 资源必须 mappable 且 host-visible，否则分配失败。丢失会导致贴图花屏/黑屏（首帧数据丢失）。
- **验证方法**：DXVK 游戏首帧渲染；`WINEHUA_RESOURCE_TRACE=1` 看 blob 创建/导出路径。

#### vkr_device_memory.c: vkr_dispatch_vkFlushMappedMemoryRanges / vkr_dispatch_vkInvalidateMappedMemoryRanges
- **为什么存在**：上游这两个 dispatch 是 NULL（guest 不做显式 flush 也有 virtio 语义保证），但 OHOS shadow 路径下 guest 的 flush/invalidate 是唯一同步信号，必须接管。flush = 把 shadow 脏区间拷到 host_map 并记录精确脏范围；invalidate 分两种模式：`VKR_WINEHUA_SHADOW_FROM_HOST=precise` 做 host→shadow 区间拷回，否则保持"写开始标记"的旧 DXVK 兼容语义（`shadow_guest_write_depth`）。
- **依赖的上游行为**：`vkr_context_init_device_memory_dispatch` 原本将两个 dispatch 置 NULL。
- **不变式**：flush 必须把脏数据送到 host 内存，invalidate 必须把 GPU 写回结果拷回 shadow；`shadow_generation_mutex`（serialize 模式）保证 flush 与 submit 不同步时 shadow/host 拷贝不竞争。丢失 → 动态 UBO/顶点数据错乱、渲染闪烁。
- **验证方法**：Heaven/游戏动态 uniform 场景；`VKR_WINEHUA_SHADOW_TRACE=1` 的 OHOS shadow remote flush/invalidate 日志。

#### vkr_device_memory.c: vkr_device_memory_flush_shadow_range / invalidate_shadow_range（精确脏区间）
- **为什么存在**：逐 64 字节 memcpy + 逐次 FlushMappedMemoryRanges 在 Maleoon 上开销不可接受（注释原文），因此：flush 只做 memcpy + 记录脏区间（合并/去重，上限 4096 条，溢出降级为整段），真正的一次 cache-domain 转换推迟到 submit 时批量做。另有 `shadow_upload_snapshot`（`VKR_WINEHUA_GPU_UPLOAD_INLINE=1` 时）推迟 host 拷贝，让 GPU upload 命令直接以 snapshot 为源。
- **不变式**：脏区间必须精确覆盖（多 buffer 别名同一内存时合并）；溢出必须降级为保守整段拷贝而不是丢数据。丢失 → 部分动态数据不更新（"贴图一半是旧的"）。
- **验证方法**：VKR_WINEHUA_SHADOW_TRACE 下比较 shadowFnv/hostFnv（`submit-input` 日志）是否相等。

#### vkr_device_memory.c: vkr_device_memory_prepare_shadow_upload / submit_shadow_upload（GPU upload 路径）
- **为什么存在**：Maleoon 上 mapped 写对 shader 读的可见性不可靠（连 coherent 堆都要显式 flush 才能保证，见 sync_shadow 注释），而每次 submit 做整内存 flush 太贵。`VKR_WINEHUA_GPU_UPLOAD=1` 时改为：把脏区间录成 `CmdUpdateBuffer`（64KB 分块、4 字节对齐）到每 queue 的私有 transient command buffer，先于 guest submit 提交（独立 submit + fence，或 inline 进同一次 QueueSubmit，用 timeline semaphore retire，slot 环形 8 个），让数据经 GPU transfer 落到 host-visible 内存。覆盖性证明（`shadow_gpu_upload_full_coverage`）：每个脏字节必须有 TRANSFER_DST buffer 覆盖（bound_buffers 列表 + 排序合并区间），否则回退 mapped 拷贝路径。
- **依赖的上游行为**：buffer 创建时 OHOS 分支强制追加 `VK_BUFFER_USAGE_TRANSFER_DST_BIT`（vkr_buffer.c）；vkr_buffer.c 在 BindBufferMemory 时维护 `bound_memory`/`bound_buffers` 拓扑（上游无此追踪）。
- **不变式**：upload 命令与 guest submit 必须严格按序（inline 模式同一次 QueueSubmit 保证，独立模式靠 queue 顺序 + fence/timeline wait）；upload 失败必须回退 mapped 路径再放行 guest submit（代码中 disable_coverage + retry sync 保证）。丢失 → GPU 读到旧数据，Heaven 类 benchmark 花屏。
- **验证方法**：Heaven 跑分对比 `VKR_WINEHUA_GPU_UPLOAD=1/0` 正确性 + WineHuaPerf upload_bytes/updates 统计。

#### vkr_device_memory.c: vkr_device_memory_sync_shadows_to_host / sync_shadows_from_host
- **为什么存在**：submit 前把所有脏 shadow 一次性同步到 host（批量 `FlushMappedMemoryRanges`，一次最多 256 个 range，跨分配合并，溢出回退逐条）；GPU upload 已覆盖的分配跳过（skipped 统计）；fence 信号成功后反向 Invalidate + 拷回 shadow（GPU 写回结果）。`VKR_WINEHUA_SHADOW_TO_HOST=explicit` 提供一次性全量初始化模式。
- **不变式**：submit 与 from_host 同步不能互相覆盖同一区间（shadow_generation_mutex）；fence 信号后的 from_host 必须在队列空闲后执行。丢失 → 渲染结果与 guest 预期不一致。
- **验证方法**：submit-input 日志 hash 对比；WineHuaPerf 的 shadow_copies/bytes。

#### vkr_device_memory.c: WineHuaUboHost 系列（UBO identity 诊断）
- **为什么存在**：定位 Maleoon 上 Heaven 的 UBO（binding 3/4，48/1536 字节）内容与 host 端不一致的问题：对 flush/upload-range/update/watched-update/descriptor 五个阶段打 FNV64 内容 hash 日志，并在 CmdBindDescriptorSets 时注册"watch"（vkr_buffer.h 中每 buffer 最多 256 个 winehua_ubo_watches），hash 变化才记录。纯诊断，默认关闭（WINEHUA_VKR_TRACE_UBO_IDENTITY）。
- **依赖的上游行为**：vkr_descriptor_set.h 中 set 上的 winehua_ubo_bindings 状态、vkr_buffer.h 的 watch 数组（均为 OHOS 条件编译）。
- **不变式**：无（不改变 Vulkan 行为）。删除仅失去诊断能力。
- **验证方法**：Heaven 跑分 + 环境变量开启后比对日志 hash。

#### vkr_queue.c: vkr_dispatch_vkQueueSubmit / vkr_dispatch_vkQueueSubmit2（shadow 同步集成）
- **为什么存在**：guest submit 前必须完成 shadow→host 同步；私有 upload 命令先于 guest submit 落队列；`shadow_upload_mutex` + `shadow_generation_mutex` 保护跨 worker 竞争；`vkr_ohos_wait_deferred_shadow_host_copy` 在需要时 QueueWaitIdle（VKR_WINEHUA_GPU_UPLOAD_SERIALIZE 或 deferred copy 回退）。
- **依赖的上游行为**：上游 vkr_queue.c 仅做 `vn_replace_*_args_handle` + `vk_mutex` 保护下的 QueueSubmit。
- **不变式**：upload 失败必须回退并重同步再执行 guest submit；shadow_upload_mutex 与 vk_mutex 的加锁顺序（先 shadow_upload 后 vk_mutex）必须保持，否则与 present 路径死锁。
- **验证方法**：游戏持续渲染无卡死；WineHuaPerf 日志。

#### vkr_queue.c: vkr_ohos_get_fence_status / vkr_dispatch_vkGetFenceStatus / vkr_dispatch_vkWaitForFences
- **为什么存在**：Maleoon 的 `GetFenceStatus` 会瞬态返回 `VK_ERROR_OUT_OF_HOST_MEMORY`（即使 submit 成功），一次错误在 Venus 中会毒化 ring 导致致命。重试 ≤4 次后仍失败则用 `WaitForFences(timeout=0)` 等价查询（VK_TIMEOUT→VK_NOT_READY）。fence 成功后触发 sync_shadows_from_host。
- **依赖的上游行为**：上游直接转发 GetFenceStatus。
- **不变式**：只重试 OOM 一个错误码，DEVICE_LOST 必须立即透传；from_host 同步只在 fence 成功时执行。
- **验证方法**：长时间游戏运行（Heaven 1h+）无 ring 致命错误；日志无 "fence status transient OOM" 以外的异常。

#### vkr_queue.c: vkr_ohos_perf_*（性能计数器，必覆盖项）
- **为什么存在**：shadow 路径把大量开销搬进 submit 路径，需要可观测性定位瓶颈。全进程级 atomic 聚合计数：submit 次数、shadow scanned/copies/bytes、prepare 六相位耗时（wait/reset/dirty_scan/buffer_record/uncovered_scan/end，total/max）、sync/lock/upload/driver/total/gap 耗时、upload 统计（buffers/UBO/SSBO/ranges/updates/bytes/skipped）、fence status/wait 统计。三种输出：`VKR_WINEHUA_PERF_SUMMARY=1` 全量、`VKR_WINEHUA_PERF_SAMPLE_INTERVAL=N` 采样（不污染生产路径）、`WineHuaFrameTimeline`（present 时刻按帧汇总，由 present 路径在 vk_mutex 下 arm）。
- **依赖的上游行为**：vkr_queue.h 中 winehua_* 字段、`vkr_winehua_queue_frame_timeline_present`（vkr_renderer.c 的 present 调用）。
- **不变式**：纯统计不改变行为；采样模式不得在生产 submit 路径加时钟（注释明确）。
- **验证方法**：Heaven 跑分 + VKR_WINEHUA_PERF_SUMMARY=1 观察 prepare_us 占比。

#### vkr_queue.c: vkr_dispatch_vkQueueBindSparse
- **为什么存在**：sparse 绑定同样需要 submit 前的 shadow 同步与 deferred wait。
- **不变式**：与 QueueSubmit 相同的同步顺序。

#### vkr_queue.h: vkr_shadow_upload_slot / vkr_winehua_* 字段
- 结构体扩展（全部 OHOS 条件编译）：8 个 upload slot 环形、timeline semaphore、perf 采样状态。合并时注意保持 vkr_queue 结构布局。

#### vkr_buffer.c: vkr_winehua_set_buffer_memory / vkr_dispatch_vkBindBufferMemory(2)
- **为什么存在**：GPU upload 覆盖性计算需要知道"哪些 buffer 绑在哪块内存的哪个偏移"（guest 侧 BindBufferMemory 的 offset 在 host 侧只通过 handle 替换后进驱动，不记录）。vkr_buffer.c 维护 bound_memory/offset + memory→bound_buffers 双向链表，绑定变化时失效 coverage 缓存。
- **依赖的上游行为**：上游 vkr_buffer.c 无任何绑定追踪。
- **不变式**：binding 变化必须失效 `shadow_coverage_valid`（vkr_device_memory_invalidate_shadow_coverage），否则上传区间按旧拓扑切割会漏数据；DestroyBuffer 必须先从列表摘除（release 路径同样处理）。
- **验证方法**：VKR_WINEHUA_BOUND_BUFFER_LIST=1 场景 + 动态 buffer 复用游戏。

#### vkr_context.c: vkr_context_create_resource_from_device_memory / vkr_context_destroy_resource / shadow 基础设施
- **为什么存在**：FD_SHM blob 直接导入为 guest 资源；destroy 时若 ring/encoder 仍引用资源需置 fatal（上游已有），OHOS 增加冲突计数诊断；context 增加 `shadow_generation_mutex` + `shadow_dirty_memories` 全局脏链表（脏列表模式避免全对象表扫描）。
- **不变式**：脏链表与 object_table 必须在 object_mutex 下操作；destroy 时的 encoder_busy 检查不能放宽（否则资源释放与命令流竞争）。

#### vkr_descriptor_set.c: vkr_dispatch_vkUpdateDescriptorSets（+ set 上 UBO 映射追踪）
- **为什么存在**：诊断 UBO identity 问题的配套：更新描述符时记录 set 上 binding 3/4 的 buffer/offset/range/array_element 映射（winehua_ubo_bindings），供 CmdBindDescriptorSets 时注册 watch；对 guest 对象与 host handle 分别打日志（WineHuaSampled）；`VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE=1` 时在 UpdateDescriptorSets 前 QueueWaitIdle 所有 queue（每 submit 代只等一次）——诊断用。
- **不变式**：描述符追踪不得改变 Vulkan 语义；watch 溢出必须静默降级。

#### vkr_command_buffer.c: vkr_dispatch_vkCmdBindDescriptorSets / CmdPipelineBarrier 等（命令录制诊断）
- **为什么存在**：录制阶段日志（WineHuaCapture/WineHuaUboHost bound-descriptor/WineHuaFrameAssoc source-transition）：bind pipeline/index/vertex/draw/copy/buffer-image/barrier/begin-render-pass 的 guest/host 对象对账，用于把"帧 → 命令 → 源图像 → present"串起来（Heaven 花屏问题定位）。全部默认关闭，有独立限额（512/50000/4096 条）。
- **不变式**：CmdBindDescriptorSets 从 VKR_CMD_CALL 宏改成显式调 proc_table（为在 replace 前后取 guest 对象），行为等价，但合并时需注意与上游新命令的同步；其余纯日志。

#### vkr_ring.c / vkr_transport.c: vkr_ring_thread / vkr_ring_notify
- **为什么存在**：ring 提交计数 + head/tail 诊断（VKR_WINEHUA_SHADOW_TRACE 控制），用于排查 guest 命令流停顿。
- **不变式**：无行为改变。

### 二、WineHua present 桥（Venus→平台合成器）

#### vkr_renderer.c（OHOS 后端初始化，必覆盖项）
- **为什么存在**：鸿蒙没有传统 WSI 呈现链路（guest 无真实 swapchain），Wine 的 DXVK 把渲染目标图像直接交给 vtest server，由 host 侧"present"命令把图像推到 OHNativeWindow。为此 vkr_renderer 增加：
  - `context_mutex` + 64 项 `vkr_context_cache`（present 命令在另一 worker 线程到达，O(n) 链表查找不可接受）；
  - `vkr_renderer_set_winehua_present_callback` / `vkr_renderer_set_winehua_device_release_callback`：注册平台合成器回调（由鸿蒙 app 侧 dlopen 注入）；
  - `vkr_renderer_winehua_present`：按 ctx/queue/image 的 **Venus 对象 ID** 查对象表，校验 image 元数据（尺寸/格式/usage 需含 TRANSFER_SRC/2D/单采样），在 `queue->vk_mutex` 下调用回调执行 `vkQueuePresentKHR`，保证与 QueueSubmit 严格串行；锁获取用 trylock + 1ms×200 次有界重试（直接 EAGAIN 会丢帧导致白屏 swapchain，硬上限防死锁）；回调内提供 `release_queue` 钩子（合成器阻塞期间释放 vk_mutex，让其他 submit 继续）；
  - present 返回 `next_present_deadline_ns` 给 guest 做帧节奏。
- **依赖的上游行为**：vkr_queue.h/vkr_renderer.h 新增回调类型与接口、vkr_device.c 的 device release 两阶段钩子（PREPARE 停新 present、AFTER_WAIT 在 DeviceWaitIdle 后销毁 WSI 对象）、vkr_image.c 保留 image 元数据（device/format/extent/usage/image_type/mip/layers/samples）。
- **不变式**：present 与 QueueSubmit 共享 vk_mutex 是正确性的核心（否则在命令执行中切走图像）；对象 ID 查找失败必须返回 EAGAIN 让 guest 重试而非毒化设备；device 销毁顺序（先停 present → DeviceWaitIdle → 毁 WSI → 毁 device）不能颠倒。
- **验证方法**：任意游戏窗口呈现；窗口拖动/缩放连续 present；销毁窗口无崩溃（release_device 两阶段日志）。

#### vkr_instance.c / vkr_device.c
- **为什么存在**：host 端实例/设备内部启用 `VK_KHR_surface` + `VK_OHOS_surface`（实例）与 `VK_KHR_swapchain`（设备）供私有 presenter 使用；**绝不向 guest 暴露**（不修改 capset 广告）。
- **不变式**：扩展追加只发生在 host 创建路径（ext_count 计算同步 +1），guest 看到的 enabledExtensionNames 不变。

#### virglrenderer.c/h
- 导出三个 `VIRGL_EXPORT` 包装：`virgl_renderer_set_winehua_vk_present_callback`、`virgl_renderer_set_winehua_vk_device_release_callback`、`virgl_renderer_winehua_vk_present`（从 vtest 共享库调用）；另新增 `virgl_renderer_context_finish`（VirGL 上下文 glFinish，供 VTEST_SYNC_GL_FINISH 同步模式）。

#### vtest/vtest_protocol.h + vtest/vtest_server.c + vtest/vtest_renderer.c
- **为什么存在**：新增两个私有协议命令（命令号 0x57485052 / 0x57485650，避开上游 1-38 区间）：`VCMD_WINEHUA_PRESENT`（VirGL 纹理路径：资源 handle→tex_id 交给回调）与 `VCMD_WINEHUA_VK_PRESENT`（Venus 路径：queue/image 对象 ID 直通 vkr_renderer_winehua_present），回复携带 status + present deadline。dispatch 表外单独处理这两个命令。
- **不变式**：命令号与格式（VCMD_WINEHUA_*_SIZE 等）是 guest 侧 Wine 补丁与 host 的 ABI 契约，合并上游时不能改号；回复必须回写 serial 用于对账。
- **验证方法**：winehua_vk_present_trace 日志；游戏帧率稳定性。

#### vtest_renderer.c: vtest_create_implicit_fence / vtest_resource_busy_wait（每 client fence 作用域）
- **为什么存在**（commit bbe88210/cc959273）：上游隐式 fence 计数是全局的（"TODO this is bad when there are multiple clients"），OHOS 多 client（每窗口一个 vtest 连接）下 A client 的 busy-wait 会等 B client 的 fence，且 EGL 原生 fence 在 OHOS 上可无限 EGL_TIMEOUT_EXPIRED，造成跨上下文死锁。改为每 client 记录 `implicit_fence_submitted`，busy-wait 只比较自己的区间；`WINEHUA_VIRGL_SYNC_MODE=egl-main` 时禁用 EGL fence（VIRGL_DISABLE_EGL_FENCE）改用 glFinish 路径；`VTEST_SYNC_GL_FINISH=1` 时 busy-wait 前 virgl_renderer_context_finish。
- **不变式**：fence 比较改用有符号差值（`(int32_t)(submitted-completed) > 0`），防止回绕/回归误判；新 client 的 baseline 取全局 completed。
- **验证方法**：多窗口同时运行（explorer + 游戏）；窗口频繁开关。

#### vtest/winehua_vtest_server.c（新增文件）
- 把 vtest 主程序封装成可 dlopen 的共享库 `libwinehua_vtest_server.so`，暴露 `winehua_vtest_main` 与三个 setter（present 回调、Venus present 回调、device release 回调），供鸿蒙 app 以 native child 方式启动并注入合成器回调。

#### vtest_renderer.c 其余：诊断设施
- `WINEHUA_VIRGL_LOG_PATH` 统一诊断日志（fence submit/complete 对账、submit 失败命令流 dump（最多 64 条命令）、blob 创建/导出轨迹、busy-wait 时长）、`WINEHUA_VTEST_PRESENT_PERF_SUMMARY`、`WINEHUA_RESOURCE_TRACE`、`WINEHUA_VK_PRESENT_TRACE`。纯诊断。

### 三、Venus 诊断埋点（Heaven UBO 花屏问题）

#### vkr_pipeline.c: vkr_freeze_bool_spec_constants
- **为什么存在**：Maleoon 的 Vulkan 编译器错误处理 DXVK 的绑定存在性 `OpSpecConstantTrue`（经向量 select 时），`WINEHUA_VKR_FREEZE_BOOL_SPEC=1` 时把 SPIR-V 中只被 OpSpecConstantTrue/False 声明的 bool 常量烘焙成普通 OpConstantTrue/False（opcode 41/42），非 bool spec 常量不动。
- **不变式**：仅重写 OpSpecConstantTrue/False 声明点（opcode 48/49）且该 ID 只以 bool 形式被 OpSpecConstantOp(71) 引用的情况；bound 上限 65536 防越界；重写失败必须原样传原模块（fail 路径返回 false）。丢失 → 依赖此 workaround 的应用渲染错误（Heaven 场景黑/错）。
- **验证方法**：Heaven 全场景对比开关前后；`WINEHUA_VKR_TRACE_PIPELINE=1` 看 frozen 计数。

#### vkr_pipeline.c: shader 模块身份记录 + vkr_pipeline.h: winehua_* 字段
- shader module 对象记录 code 的 FNV32 hash / 尺寸 / 首尾字，管线创建时连同 specialization 参数一起打 WineHuaPipeline/WineHuaShader 日志，用于对账 host 编译管线与 guest 期望。纯诊断。

#### vkr_render_pass.c: vkr_winehua_log_render_pass_create / framebuffer 日志
- 纯诊断（WINEHUA_VKR_TRACE_CAPTURE），记录 pass 附件/subpass/颜色深度引用与 framebuffer 附件（view→image 元数据）。注意 CreateFramebuffer 的 trace 分支在 enabled 时才分配 attachment 数组，正常路径零开销。

#### vkr_image.c / vkr_image.h: image 元数据保留
- **功能用途**：present 校验需要 image 的 device/format/extent/usage/image_type/mip/layers/samples；ImageView 回指 image。这是 present 桥的正确性依赖（见上），非纯诊断。
- **不变式**：元数据在 create 时快照，不受后续 host 驱动影响。

#### vkr_physical_device.c/h
- `physical_dev->instance` 回指（present 路径需要 instance handle）；`winehua_shadow_gpu_upload_quirk` 标志当前恒 false——**GPU upload 只允许显式环境变量开启**（"explicit, qualification-gated opt-in"），未来硬件验证通过后可在此改默认。
- **不变式**：quirk 默认 false 是正确性保险，勿在上游合入时顺手置 true。

### 四、VirGL/GLES 宿主侧适配

#### vrend_formats.c: vrend_override_formats（Z32 深度格式仿真）
- **为什么存在**（commit 2963cae0）：上游 GLES 用 `add_formats` 注册 Z32 但"fake support with Z24 and hope nobody notices"；OHOS GLES 宿主上 Z32_UNORM/Z32_FLOAT_S8X24_UINT 不能做 framebuffer attachment（FBO 不完整）。改为 `override_formats`：按格式查已有表项回填 bindings/flags，注册为 DEPTH_COMPONENT24/DEPTH24_STENCIL8 仿真；新增 Z32_FLOAT_S8X24_UINT→DEPTH24_STENCIL8。
- **依赖的上游行为**：`vrend_get_format_table_entry` 已存在。
- **不变式**：override 的格式必须仍能被 wine 侧当成 Z32 用（深度精度损失可接受，bindings 能力必须够）；FBO incomplete 时 vrend_renderer.c 会 dump 全部 attachment 明细（新增诊断）。
- **验证方法**：需要 Z32 深度的游戏/场景（如特定画质档）可正常渲染；FBO 失败日志完整。

#### vrend_renderer.c: vrend_renderer_init（VIRGL_DISABLE_EGL_FENCE）
- **为什么存在**：OHOS EGL 的 fence 路径（eglClientWaitSyncKHR）可无限超时，允许环境变量禁用 EGL fence 改用 GL 路径（配合 vtest 的 egl-main 模式）。
- **不变式**：无（默认启用 EGL fence 不变）。

#### vrend_winsys.c / vrend_winsys_egl.c
- **为什么存在**：OHOS 嵌入式合成器提供 EGL 但无 GBM/libdrm。`HAVE_EPOXY_EGL_H` 且无 ENABLE_GBM 时，用 `EGL_DEFAULT_DISPLAY` 直接初始化 surfaceless/default EGL；`vrend_winsys_cleanup` 的守卫从 ENABLE_GBM 改为 HAVE_EPOXY_EGL_H。winsys_egl 增加 `VIRGL_DISABLE_NATIVE_FENCE_FD`（绕过 native fence fd 轮询）与 `WINEHUA_VIRGL_LOG_PATH` fence-wait 诊断。
- **不变式**：EGL 初始化失败仍必须返回 -1；清理路径对 gbm 判空。

#### vrend_decode.c: vrend_renderer_context_finish
- 提供 `glFinish` 于指定 VirGL 上下文（供 VTEST_SYNC_GL_FINISH）。**注意**：上游已有同名 API 时需改名。

### 五、同进程 render server 适配

#### server/render_context.c + src/proxy/proxy_context.c（ENABLE_SAME_PROCESS_RENDER_SERVER + ENABLE_RENDER_SERVER_WORKER_THREAD 时）
- **为什么存在**：鸿蒙沙箱里 render server 以同进程线程模式跑。SCM_RIGHTS 在线程间共享 fd 编号——发送成功后 server 再 close 会与接收方竞争（F_ADD_SEALS 成功而 fstat EBADF）；且沙箱对 sealed anon 文件的 fstat 直接拒绝。因此：发送成功则不再 close（所有权移交 proxy），proxy 的 fstat 大小校验在 thread 模式下跳过（fd 来自可信进程，mmap 才是权威校验）。
- **不变式**：非 OHOS 或非 thread 模式走原路径（close 与 fstat 校验不变）；mmap 失败仍拒绝。
- **验证方法**：HAP 内 run_in_thread 模式 blob 创建稳定不 EBADF。

### 六、构建系统

- meson_options.txt：新增 `external-egl-without-gbm`（EGL 无 GBM 构建，见 vrend_winsys）与 `vtest`（默认 true，允许关闭 vtest 构建）；其余为 CRLF 噪音。
- meson.build：vtest subdir 受 `vtest` option 控制；EGL/GBM 检测逻辑按 option 分支（gbm_dep 不存在时 have_egl = option）；`ENABLE_GBM`/`ENABLE_GBM_ALLOCATION` 的 conf_data 设置随之调整。CRLF 噪音建议以 -w 合并。
- vtest/meson.build：新增 winehua_vtest_server 共享库目标。

## 合并上游建议（要点）

1. **CRLF 噪音**：meson.build / meson_options.txt / vrend_winsys.c 用 `git diff -w` 提取真实改动，三文件真实功能改动仅 15/14/17 行。
2. **OHOS 条件编译是安全网**：绝大部分功能代码在 `#ifdef __OHOS__` 内，非 OHOS 构建行为与上游一致（除 vrend_formats.c 的 GLES 格式 override 与 fence 诊断外），合入上游时可按平台剥离。
3. **环境变量清单**（合并时逐一确认去留）：VKR_WINEHUA_GPU_UPLOAD / GPU_UPLOAD_INLINE / GPU_UPLOAD_WAIT / GPU_UPLOAD_SERIALIZE、VKR_WINEHUA_SHADOW_TRACE / SHADOW_TO_HOST / SHADOW_FROM_HOST / SHADOW_MSYNC / SHADOW_DIRTY_LIST / SHADOW_MERGE_RANGES / SHADOW_COVER_UPLOAD / SHADOW_SUBMIT_UNMAP_LARGE / SHADOW_GENERATION_SERIALIZE、VKR_WINEHUA_BOUND_BUFFER_LIST / BATCH_FLUSH / COVERAGE_SORT / PERF_SUMMARY / PERF_SAMPLE_INTERVAL / FRAME_TIMELINE_INTERVAL / DESCRIPTOR_UPDATE_SERIALIZE、WINEHUA_VKR_TRACE_UBO_IDENTITY / TRACE_CAPTURE / TRACE_SAMPLED / TRACE_PIPELINE / TRACE_PRESENT_IMAGE / PRESENT_STAGE_TRACE、WINEHUA_VKR_FREEZE_BOOL_SPEC、WINEHUA_VIRGL_LOG_PATH / VIRGL_SYNC_MODE / RESOURCE_TRACE / VTEST_PRESENT_PERF_SUMMARY / VK_PRESENT_TRACE、VIRGL_DISABLE_EGL_FENCE / DISABLE_NATIVE_FENCE_FD、VTEST_SYNC_GL_FINISH。
4. **已知噪音**：vkr_device_memory.c 中一处上游注释被意外截断（"mapped buffe"，少一个 'r'），是误删，合入时恢复。

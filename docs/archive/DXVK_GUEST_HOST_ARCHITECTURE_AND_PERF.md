# WineHua DXVK Guest -> Host 架构与性能调查备忘录

> 状态：已被取代（2026-09-22 整理归档）。2026-07-21 的性能调查，结论已被后续调查超越。

> Updated: 2026-07-21
>
> Scope: Legacy DXVK 1.10.3, Wine Vulkan, Mesa Venus over vtest,
> virglrenderer Venus, HarmonyOS Maleoon Vulkan, and BrokerPresent.
>
> Purpose: provide a self-contained technical handoff for performance review.
> This memo separates verified facts from hypotheses and proposed experiments.

## 2026-07-21 当前性能与稳定性结论

`shadow-precise` 已把全量 Host-to-Guest refresh 从正常帧路径移除，DX11
cube 的 Host present 稳定在约 83.6 FPS；每帧 Guest-to-Host 通常只发布约
384 B 的动态顶点/常量数据，显示路径仍只有一次 GPU copy。

此前的随机画面消失不是 SurfaceQueue 回退，而是 Venus ring command stream
解码失步。Guest 在 Box64 下以 x86 release store 发布 tail；这种指令通常依赖
x86 TSO，不能充分约束此前由原生 AArch64 `memcpy` 完成的共享内存 payload
写入。加入可控的 sequentially-consistent publish fence 后，两轮实机测试分别
通过 45/45 和 60/60 帧序采样，0 duplicate、0 regression、0 CS error，后者
累计超过 5400 帧且 present failures=0。最终普通默认启动继续运行到截图帧号
21,989（83.7 FPS、`regress 0`），Host present serial 超过 22,069，仍无 CS
error。

当前产品候选档位为：

```text
DXVK Legacy
  + precise mapped-memory shadow contract
  + BOX64_DYNAREC_WEAKBARRIER=0
  + VN_WINEHUA_STRONG_RING_BARRIER=1
  + Venus BrokerPresent
```

`shadow-none` 的约 31 FPS 与向回转属于错误诊断档位；`full` 的约 5 FPS 是
保守同步基线；`no_async_queue_submit` 不能消除 decoder fatal。后续 P0 从
“找 30 FPS 瓶颈”转为完成 60 分钟长稳、真实游戏和 x86/WoW64 回归，然后再
决定是否把强屏障无条件固化为 OHOS/Box64 quirk。

## 2026-07-20 Shadow A/B 最新结论

实机同 HAP、reuse prefix、x64 DXVK Legacy cube 的有效 A/B：

* full/shadow-trace：约 4.9-5.1 FPS；Host->Guest 每次复制 128-192 MiB，约 41-65 ms。
* shadow-none：31.4 FPS，但用户观察到向回转/抖动，属于正确性失败，只能作为性能上限。
* shadow-to-host-explicit：5.4 FPS。Guest->Host 只在首轮复制 64 MiB（9.4 ms），后续 submit 通常为 0 B/3-5 us，真实 dirty range 为 640 B 或约 90 KiB。

因此 Guest->Host 显式 dirty range 已验证可行，但只贡献约 0.3-0.5 FPS；当前压倒性瓶颈是 Host->Guest 全量 refresh。Host fence status 本身通常只有 0-2 us。

VirGL 可复用的是“资源 ID + 明确 transfer box/dirty range”的原则：呈现资源保持在 GPU，CPU 只处理明确的 upload/readback range。不能直接复用它的 backing；当前 Harmony/Maleoon 没有为 Venus Host-visible allocation 提供可用的 dma-buf/opaque-fd 共享。

下一步必须同时满足正确性和性能：

1. 用 DXVK 显式 invalidate/map-read 路径发布 Host->Guest 精确 range。
2. renderer 只在对应 fence 完成后复制 GPU 实际写过的 allocation/range。
3. 对 cube 增加 frame/angle watermark，并串联 Guest present serial、Host accepted serial、NativeImage timestamp，定位向回转是渲染状态回退还是旧帧呈现。
4. generic Vulkan 保留 full fallback；shadow-none 和 explicit 档位仍是诊断模式，不改变产品默认。


## 0. 中文共享摘要

### 0.1 当前结论

目前 DXVK Legacy 1.10.3 已经能够在正常产品启动路径创建 D3D11 设备，
并显示旋转立方体。此前正常桌面模式只有白色客户区的问题已经定位并解决：

```text
DXVK 对应的 Wayland subsurface 没有 wl_shm commit
  -> 桌面 renderer 没有发现这个 Vulkan surface
  -> BrokerPresent 的 NativeImage target 没有创建
  -> Vulkan present 持续返回 -EAGAIN
```

现在通过只提交几何信息、不提交像素 SHM 的方式完成 surface 注册。因此正常
Vulkan 显示链路仍然是纯 GPU 路径：

```text
DXVK 渲染出的 Host VkImage
  -> NCP 内一次 vkCmdBlitImage / vkCmdCopyImage
  -> OHNativeWindow / SurfaceQueue swapchain
  -> App 侧 OHNativeImage external-OES texture
  -> XComponent compositor
```

正常显示过程中没有整帧 `glReadPixels`、GPU framebuffer 到 CPU readback、
CPU 像素转换、Wayland SHM framebuffer 搬运或 `glTexSubImage2D` 回传。

当前性能约 **4.92 FPS**。Host present 回调平均只有约 **5.65 ms**，因此
低帧率不是最后一次 GPU blit 或 SurfaceQueue 导致的。主要时间消耗位于
Guest Venus/vtest 的 mapped-memory shadow 同步和 fence 查询往返。

### 0.2 DXVK Guest -> Host 完整架构

```text
Windows x64/x86 D3D11 游戏
  -> WineHua 受管 DXVK 1.10.3 dxgi.dll / d3d11.dll
  -> Wine Vulkan PE/Unix thunk
  -> Box64 中的 x86_64 Vulkan Loader
  -> x86_64 Mesa Venus ICD
  -> Venus protocol object ID + command stream
  -> vtest Unix socket / SCM_RIGHTS fd
  -> AArch64 NCP 中的 virglrenderer vkr
  -> Guest object ID 映射为真实 Host VkDevice/VkQueue/VkImage 等对象
  -> Maleoon 910 Harmony Vulkan driver
  -> Venus BrokerPresent
  -> SurfaceQueue / NativeImage / App compositor
```

其中有三个需要分开理解的数据通道：

1. **Vulkan 命令通道**：Guest Venus 编码 Vulkan 调用，vtest 将 command
   stream 交给 Host virglrenderer，vkr 解码后调用真实 Host Vulkan。
2. **mapped buffer 通道**：Guest CPU 写 staging、constant、upload、query
   等 Host-visible allocation。当前设备使用 SHM shadow 与 Host
   `vkMapMemory` 双映射，需要按 dirty range 做 CPU memcpy。
3. **显示 buffer 通道**：Host VkImage 直接在 Host GPU 上复制到
   SurfaceQueue swapchain。这里只传资源 ID、对象所有权和同步，不传像素。

所谓“去掉 GPU 回读”主要针对第 3 条，目前已经做到。第 2 条仍有 CPU copy，
但它不是整帧显示 readback，而是 Host-visible Vulkan allocation 的兼容桥。

### 0.3 为什么当前需要 shadow memory

上游 virglrenderer/Venus 的理想模式是：

```text
Host VkDeviceMemory
  -> 导出 dma-buf 或 opaque fd
  -> vtest 用 SCM_RIGHTS 把 fd 交给 Guest
  -> Guest 与 Host 映射同一块 backing storage
```

在当前 Maleoon/Harmony 运行时，Host-visible allocation 没有得到可供这条
链路使用的 dma-buf/opaque-fd export。因此 virglrenderer 的 OHOS fallback
实际是：

```text
Guest mmap -> anonymous SHM shadow
                     |
                     | dirty range memcpy
                     v
Host vkMapMemory pointer -> Host VkDeviceMemory
```

Guest 写内存后，如果只调用 vtest 本地 `bo_flush`，Host mapping 看不到变化。
当前通过下面两个开关补齐语义：

```text
DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1
VN_WINEHUA_REMOTE_MEMORY_SYNC=1
```

DXVK 在 staging upload、纹理初始化、BC 解压上传、constant/uniform、gamma、
HUD 等 CPU 写入后发布精确 range。Guest Venus 把
`vkFlushMappedMemoryRanges` 发给 Host，Host 只复制该范围并在下一次
queue submit 做 Vulkan cache flush。

这项优化已经把原来部分 submit 遍历多个 64 MiB allocation、单次约
120 ms 的整块 memcpy，降到当前每个 submit 通常约 4-9 ms。cube 帧率从
约 4.42 FPS 提升到约 4.92 FPS，但还没有解决主要同步等待。

### 0.4 当前最可疑的 fence 路径

DXVK Legacy 的 command-list finish thread 会执行：

```text
DxvkCommandList::synchronize
  -> vkWaitForFences(timeout = 1 second)
```

当前产品设置了：

```text
VN_PERF=no_fence_feedback
```

因此 Guest Venus 并没有一次调用 Host `vkWaitForFences`，而是循环：

```text
vn_WaitForFences
  -> vn_GetFenceStatus
  -> vn_call_vkGetFenceStatus
  -> vtest/virglrenderer/Host Vulkan 同步往返
  -> vn_relax
  -> 再次查询
```

设备日志中 fence status 之间规律性出现约 40-65 ms 的间隔，且一帧通常有
约 5 次 queue submit。这与当前约 200 ms/frame 的结果一致，是 P0 嫌疑。

不能直接恢复上游 fence feedback。Host GPU/driver 更新的是 Host mapping，
Guest 读取的是 SHM shadow；两者不是同一 backing。之前打开 fence feedback
的 A/B 结果是 0 FPS/静态白屏，因为 Guest 永远读到旧的 unsignaled slot。

Host virglrenderer 已经实现真正的 `vkr_dispatch_vkWaitForFences`，成功后也会
执行 Host -> Guest shadow refresh。因此下一项实验应当是：

```text
VN_WINEHUA_DIRECT_FENCE_WAIT=1

仅对 VN_SYNC_TYPE_DEVICE_ONLY：
Guest vkWaitForFences
  -> 一次同步 Venus vkWaitForFences
  -> Host vkWaitForFences
  -> 成功后同步必要 shadow
  -> 一次 reply

imported sync-fd 继续走原路径。
```

需要重点评审是否存在 renderer worker 自锁风险：Host wait 所在的线程不能
阻塞尚未被 Host 执行、但负责 signal 该 fence 的 earlier queue submit。
当前启用了 thread render server，并且协议有顺序保证，但仍必须用有限超时、
watchdog 和 A/B 验证，不能直接无界等待。

### 0.5 当前问题优先级

**P0：fence status 轮询跨层往返。**

优先试验 device-only direct Host fence wait。对比 FPS、每帧 status 次数、
wait 时间和是否出现死锁/白屏。

**P1：Host -> Guest shadow refresh 太粗。**

每次 fence 成功后，当前会扫描 context 全部 object，并对所有可映射 shadow
allocation 做 invalidate 和全 allocation memcpy。它可能复制大量 GPU 根本
没有写过的数据。目前没有 from-host bytes/us 统计，这是关键测量缺口。

**P2：每次 queue submit 都扫描全部 device memory。**

Guest -> Host 已经是 range dirty，但 submit 仍锁 object table 并遍历全部
memory object。应改成 dirty-allocation list，只处理实际发布过 range 的对象。

**P3：private present 同步且持有 Host queue mutex。**

present 会做 Guest ring roundtrip、同步 vtest request，并在 acquire、GPU copy、
queue present、source release wait 期间持有 vkr queue mutex。目前实测仅约
5.65 ms，不是 P0，但会串行化后续 submit。

**P4：DXVK Legacy 每帧约 5 次 Host submit。**

每次 submit 都放大协议和 shadow 固定成本。先解决 P0-P2，再标记每个 submit
用途，判断哪些可批处理，避免过早修改 DXVK 调度语义。

**P5：BrokerPresent 仍有一次 GPU copy。**

理论上可让 Venus 直接渲染到 acquire 的 SurfaceQueue NativeBuffer，从而连
这一次 copy 也去掉。但需要官方 NativeBuffer Vulkan import、格式/modifier、
queue ownership、acquire/release fence、跨 NCP lifetime 全部成立。目前它
不是 5 FPS 根因，应后移。

**P6：诊断日志未充分限流。**

当前长时间 cube 运行后 Wine stderr 和 virgl Host log 各增长到约 60 MiB。
关闭 Wine debug 后帧率基本不变，说明日志不是主根因，但产品不能保留逐
submit、逐 fence、逐 present stage 输出。

### 0.6 可以参考的现有方案

1. **virtio-gpu resource blob / Venus HOST3D blob**：用于 Guest/Host 共享
   一块外部内存 backing，是 mapped buffer 零复制的最直接参考。
2. **Android gralloc/minigbm/AHardwareBuffer**：传 buffer ownership 与
   acquire/release fence，而不是传 framebuffer 像素。WineHua 的 Harmony
   对应物就是现有 `OHNativeWindow -> SurfaceQueue -> OHNativeImage`。
3. **Venus fence feedback / renderer sync object**：用 Guest 可见的小型
   status slot 或事件替代反复 `GetFenceStatus`。当前要解决的是该 slot 如何
   真正落在 Guest/Host 同一 backing，或由 renderer 直接通知 Guest。
4. **直接渲染 Native queue buffer**：可消除最后一次 GPU copy，但它需要
   完整 external-memory 和 explicit-fence 协议，不是传一个裸指针或 fd 即可。

之前真实设备 probe 没有证明公共 raw dma-buf 能安全跨 NCP 使用。当前已验证
的生产边界是 NativeWindow/SurfaceQueue IPC。可以继续探测 Maleoon 是否有
可用的 OHOS NativeBuffer Vulkan external-memory 扩展，但不能为此拆掉现有
可工作的 BrokerPresent。

### 0.7 建议其他工程师重点回答的问题

1. Mesa Venus thread render server 下，同步 renderer `vkWaitForFences` 是否会
   阻塞负责提交 signal 操作的线程？正确的 ordering barrier 应放在哪里？
2. 能否让 renderer fence completion 直接写 Guest SHM feedback slot，或通过
   eventfd/futex/vtest sync object 通知 Guest，避免 status polling？
3. virglrenderer 能否根据 submit 的 command/resource 引用，确定哪些 mapped
   allocation 可能被 GPU 写入，从而精确做 Host -> Guest refresh？
4. dirty allocation 是否可以维护无全表扫描的队列，并安全处理 allocation
   destroy、重复 range 合并和多 queue submit？
5. Maleoon 是否支持将 `OH_NativeBuffer` 正式 import 为 Vulkan image/memory？
   对应扩展、格式、modifier、fence 和跨 NCP 权限分别是什么？
6. private present 能否通过 semaphore/timeline 返回 source release，而不是
   持有 vkr queue mutex 同步等 copy fence？
7. DXVK 1.10.3 cube 的约 5 次 submit 分别是什么，哪些可安全 batch？

### 0.8 下一轮实验顺序

1. 先给 shadow 两个方向增加 bytes/copies/us/object-scan 统计。
2. 实现 `VN_WINEHUA_DIRECT_FENCE_WAIT=1`，只覆盖 device-only fence。
3. clean 重建 x86_64 Guest Mesa，再完整构建 ARM64 HAP。
4. 做 baseline/direct-wait A/B：FPS、fence calls/frame、shadow bytes/frame、
   Host present time、旋转正确性、超时与死锁。
5. 通过后跑 Wine Vulkan x64/x86、官方 D3D11 smoke x64/x86 和 visual gate。
6. 再实现 dirty-allocation list，减少每 submit 全表扫描。
7. 限流所有逐帧日志并重复 A/B。
8. 最后再考虑 present 异步同步或直接渲染 Native queue buffer。

以下英文部分保留更细的逐层实现、源码入口和风险说明，方便直接对照代码。

## 1. Executive summary

The normal product path can now create a D3D11 device and display the rotating
DXVK cube. The former all-white window was a presentation lifecycle issue, not
a D3D11 or shader failure:

```text
DXVK Wayland subsurface had no wl_shm commit
  -> desktop renderer did not discover the Vulkan surface
  -> BrokerPresent NativeImage target was never created
  -> Vulkan present returned -EAGAIN
```

A protocol-only geometry commit now registers the surface without introducing
a CPU pixel path. The displayed Vulkan frame still follows:

```text
Host VkImage
  -> one Host GPU blit/copy
  -> OHNativeWindow / SurfaceQueue swapchain
  -> OHNativeImage external-OES texture
  -> App compositor / XComponent
```

Normal display has no full-frame `vkCmdCopyImageToBuffer`, `glReadPixels`, CPU
pixel conversion, SHM framebuffer copy, or CPU texture upload. The current
performance problem is not a display readback problem.

The latest measured cube rate is about **4.92 FPS**. Host presentation takes
only about **5.65 ms/frame**. Most of the remaining approximately 200 ms frame
time is in the Guest Venus / vtest synchronization path, especially repeated
fence-status round trips and mapped-memory shadow synchronization.

## 2. Process and architecture topology

```text
Harmony main application process (AArch64)
  EntryAbility / WineWindowAbility
  Wayland server
  XComponent renderer
  OHNativeImage SurfaceQueue consumer
             ^
             | NativeWindow lifecycle IPC + NativeImage frame consumption
             |
Harmony native child process, NCP (AArch64)
  vtest server
  virglrenderer
    vrend                 (OpenGL/VirGL path)
    vkr / Venus renderer  (Vulkan/DXVK path)
  Harmony Host Vulkan driver, Maleoon 910
  VenusSurfaceQueueTarget / VirglSurfacePresenter
             ^
             | vtest Unix socket + SCM_RIGHTS file descriptors
             |
Box64 guest Linux process (x86_64 userspace)
  Windows game PE (x64 or WoW64 x86)
    -> managed DXVK 1.10.3 dxgi.dll / d3d11.dll
    -> Wine Vulkan Unix backend
    -> x86_64 Vulkan loader
    -> x86_64 Mesa Venus ICD
```

Host code is AArch64. Wine's Unix Vulkan loader and Guest Mesa Venus remain
x86_64 even for a 32-bit Windows application. WoW64 handles the PE-side thunk;
there is no separate i386 Linux Vulkan stack.

## 3. Command path: DX11 call to Host Vulkan

```text
Windows D3D11 API
  -> DXVK records Vulkan command buffers and queue submissions
  -> Wine Vulkan forwards Vulkan ABI calls to the x86_64 loader
  -> Guest Venus assigns protocol object IDs and serializes Vulkan commands
  -> vtest transports command streams and resource file descriptors
  -> virglrenderer vkr maps Guest object IDs to real Host Vk handles
  -> Maleoon Host Vulkan executes vkQueueSubmit
```

Important details:

1. Guest Vulkan handles are not Host handles. Venus assigns protocol object
   IDs for devices, queues, memory, images, views, samplers, descriptor sets,
   fences, and other objects.
2. The renderer command path and private present command use the same vtest
   connection but different ordering mechanisms. Vulkan work is published by
   the Venus ring/submit path. Present is a synchronous private vtest request.
3. Before private present, `vn_winehua_present` performs a renderer round trip
   so the Host object table has observed the queue and image. It retries a
   bounded `-EAGAIN` publication race.
4. virglrenderer looks up the protocol queue/image IDs and validates image
   size, format, usage, mip, layer, sample count, and owning device before
   exposing only the real Host handles to the NCP presenter callback.
5. The Host queue mutex is shared with `QueueSubmit`, `QueueSubmit2`, sparse
   binding, and private present. This gives correctness but can serialize
   command submission with platform presentation.

Primary implementation points:

```text
Guest Mesa:
  src/virtio/vulkan/vn_renderer_vtest.c
    vn_winehua_present
    vtest_vcmd_winehua_vk_present

Host vtest / virglrenderer:
  vtest/vtest_renderer.c
    vtest_winehua_vk_present
  src/venus/vkr_renderer.c
    vkr_renderer_winehua_present

NCP presenter:
  entry/src/main/cpp/venus_surface_presenter.cpp
    VenusSurfaceQueueTarget::Impl::Present
```

## 4. Mapped memory path and OHOS shadow memory

### 4.1 Upstream fast path

The normal virglrenderer Venus design prefers a HOST3D resource blob backed by
exportable Host Vulkan memory:

```text
Host VkDeviceMemory
  -> dma-buf or opaque-fd export
  -> vtest sends fd with SCM_RIGHTS
  -> Guest mmap sees the same backing allocation
```

When this is available, CPU writes through the Guest mapping address the same
storage imported or mapped by Host Vulkan. Cache flush/invalidate operations
remain necessary, but there is no second full allocation to copy.

### 4.2 Actual Maleoon/OHOS path

For the tested Host-visible allocations, virglrenderer cannot obtain a usable
exportable dma-buf or opaque-fd backing. The current OHOS fallback therefore
uses two mappings:

```text
Guest x86_64 mmap
  -> anonymous SHM shadow fd
                    | memcpy dirty Guest ranges to Host
                    v
Host AArch64 vkMapMemory pointer
  -> Host VkDeviceMemory
```

At blob creation:

1. Host Vulkan allocates `VkDeviceMemory`.
2. virglrenderer creates an anonymous SHM file of the allocation size.
3. virglrenderer maps both SHM and Host `VkDeviceMemory`.
4. vtest sends the SHM fd to Guest Venus.
5. Guest `vkMapMemory` returns the Guest mapping of the SHM shadow.

This is not a framebuffer readback. It is a compatibility bridge for
Host-visible Vulkan allocations such as staging, constant, upload, query, and
feedback buffers.

### 4.3 Guest -> Host dirty-range publication

Local vtest `bo_flush` is a no-op because the Guest only maps the shadow file.
WineHua enables:

```text
VN_WINEHUA_REMOTE_MEMORY_SYNC=1
DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1
```

DXVK now calls `flushMappedSlice` after CPU writes in the critical paths:

- D3D11 buffer and image staging upload.
- BC compatibility texture upload.
- Host-visible buffer and texture initialization.
- Deferred buffer writes.
- Shader uniform data.
- Swapchain gamma data.
- HUD text and graph data.

Guest Venus turns `vkFlushMappedMemoryRanges` into a synchronous Venus call.
The Host copies only the published range from SHM shadow to Host mapping and
unions dirty ranges for the allocation. The following queue submit performs
one Host `vkFlushMappedMemoryRanges` for the union.

This changed the observed behavior from repeated full 64 MiB copies, sometimes
about 120 ms in one submit, to approximately 4-9 ms shadow preparation per
submit in the current cube workload.

### 4.4 Host -> Guest synchronization

On a successful Host fence status/wait, virglrenderer currently calls:

```text
vkr_device_memory_sync_shadows_from_host(context)
```

That function scans the whole context object table. For every mapped shadow
allocation not protected by a Guest-write scope, it invalidates the Host
mapping and copies the allocation from Host mapping back to Guest SHM.

This direction does not yet have a corresponding precise Host-dirty range
protocol. It is needed for legitimate readback/query data, but it may copy
allocations that the GPU did not write. The current logs do not quantify these
Host -> Guest bytes, so this remains a high-value measurement gap.

Primary implementation points:

```text
Guest Mesa:
  src/virtio/vulkan/vn_device_memory.c
    vn_FlushMappedMemoryRanges
    vn_InvalidateMappedMemoryRanges

Host virglrenderer:
  src/venus/vkr_device_memory.c
    vkr_device_memory_flush_shadow_range
    vkr_device_memory_sync_shadow
    vkr_device_memory_sync_shadows_to_host
    vkr_device_memory_sync_shadows_from_host
  src/venus/vkr_queue.c
    vkr_dispatch_vkQueueSubmit
    vkr_dispatch_vkGetFenceStatus
    vkr_dispatch_vkWaitForFences
```

## 5. Fence path and the leading bottleneck

DXVK Legacy retires command lists on a finish thread with:

```text
DxvkCommandList::synchronize
  -> vkWaitForFences(timeout = 1 second)
```

Because `VN_PERF=no_fence_feedback` is enabled, Guest Venus implements a
device-only fence wait as a loop:

```text
vn_WaitForFences
  -> vn_GetFenceStatus
  -> synchronous vn_call_vkGetFenceStatus
  -> vtest/renderer round trip
  -> vn_relax
  -> repeat until signaled or timeout
```

Fence feedback is deliberately disabled. In the normal Venus feedback design,
the renderer/GPU updates a feedback slot that Guest reads without a renderer
round trip. On the OHOS shadow path, Host writes update the Host mapping but
not the Guest SHM shadow. The Guest therefore sees a stale unsignaled value.
The measured fence-feedback A/B produced a static white frame for this reason.

The Host already implements a real renderer-side `vkWaitForFences` dispatch.
After success it also synchronizes Host shadow data back to Guest. This gives a
short-term optimization path without requiring coherent cross-process Vulkan
memory:

```text
Guest device-only vkWaitForFences
  -> one synchronous Venus vkWaitForFences call
  -> one Host vkWaitForFences
  -> one shadow refresh after success
  -> one reply
```

Imported sync-fd payloads must retain the existing local `sync_wait` behavior.
Multiple-fence wait-any/wait-all and finite timeout semantics must be preserved.

### Query feedback uses the same shadow-memory rule

Venus normally injects a GPU copy after `vkCmdEndQuery` and stores the query
result plus availability in an internal mapped feedback buffer. Its
`vkGetQueryPoolResults` fast path reads that Guest mapping without a renderer
round trip. On WineHua, the GPU copy updates the Host Vulkan mapping while the
Guest SHM availability word remains stale.

The verified compatibility policy is therefore:

```text
VN_PERF=no_fence_feedback,no_query_feedback
```

DXVK still records `vkCmdResetQueryPool`, `vkCmdBeginQuery`, and
`vkCmdEndQuery`. Only result retrieval changes to the synchronous renderer
call. On the physical Pad, the same precise D3D11 occlusion query returned
42,488 samples for x86 and x64; the prior feedback path remained
`VK_NOT_READY` for two seconds. Do not replace this with unconditional
feedback-buffer readback: that adds Host-to-Guest copies to every query poll
and needs explicit range alignment and lifetime proof.

## 6. Present path: no full-frame CPU readback

The DXVK swapchain invokes the private Guest entry `vn_winehua_present` with:

```text
queue object ID
image object ID
width / height / format / layout
client pid / Wayland surface id
presentation serial
```

The Host resolves those IDs to the real `VkQueue` and `VkImage`. The NCP then:

1. Waits for reuse of one bounded per-frame presenter slot.
2. Acquires an OHNativeWindow swapchain image.
3. Transitions the Guest-rendered Host image to transfer source.
4. Performs one `vkCmdBlitImage` or `vkCmdCopyImage` into the SurfaceQueue
   swapchain image.
5. Restores the source layout.
6. Submits and calls `vkQueuePresentKHR`.
7. Waits for the copy fence before releasing source-image ownership.
8. Returns a next-present deadline to Guest for bounded pacing.

The App consumes the queued buffer as `OHNativeImage` / external-OES and
composes it into the XComponent. Pixel data never travels through the vtest
socket or Wayland SHM in the normal Vulkan path.

Current Host presentation averages from the cube:

| Stage | Average |
| --- | ---: |
| Acquire | 1.20 ms |
| Submit | 1.61 ms |
| Queue present | 2.42 ms |
| Source release fence | 0.04 ms |
| Total Host present callback | 5.65 ms |

This layer cannot explain an approximately 200 ms frame interval by itself.

## 7. OpenGL VirGL comparison

The stable OpenGL/VirGL path uses the same architectural principle but a
different renderer object type:

```text
Guest virpipe resource handle
  -> private VTEST present-resource command
  -> Host vrend resolves the GL texture
  -> one Host GL GPU blit to SurfaceQueue
  -> OHNativeImage / App compositor
```

The verified OpenGL smoke runs near the 90 Hz display rate with stable
`upload_bytes=0`. This is strong evidence that SurfaceQueue, NativeImage, the
App compositor, and the single GPU blit are not inherently limited to 5 FPS.

The important difference is Venus mapped-memory and fence synchronization,
not the final native display buffer transport.

## 8. Current measured results

| Configuration | Result |
| --- | ---: |
| Original visible DXVK cube | about 4.42 FPS |
| Product logging reduced | about 4.42 FPS |
| Remote-memory sync disabled | about 3.78 FPS |
| Dynamic mapped flush disabled | about 3-4 FPS, still rotating |
| Fence feedback enabled | 0 FPS / static white frame |
| Dirty-range DXVK publication enabled | about 4.92 FPS |

Additional observations:

- The cube typically generates about five Host `vkQueueSubmit` calls per
  displayed frame.
- Current shadow preparation is usually about 4-9 ms per submit.
- Fence-status calls recur with roughly 40-65 ms gaps in the current trace.
- Host present remains near 5-6 ms.
- Disabling `WINEDEBUG` and rate-limiting some Host logs did not materially
  change FPS, so logging was not the original root cause.
- The current diagnostic build still has excessively verbose Mesa/vkr present
  traces. One long cube run grew both Wine stderr and virgl Host logs to about
  60 MiB. These logs must be rate-limited before release even though they are
  not the main 5 FPS cause.

## 9. Problem list, ranked

### P0: Guest fence polling performs repeated renderer round trips

Evidence is strong. The current code sends synchronous `vkGetFenceStatus`
queries until the Host fence signals. Each query crosses Box64/Guest Mesa,
vtest socket, the NCP dispatcher, virglrenderer object lookup, and the Host
driver, then returns through the same layers.

Proposed first experiment:

```text
VN_WINEHUA_DIRECT_FENCE_WAIT=1
```

For `VN_SYNC_TYPE_DEVICE_ONLY`, make Guest `vn_WaitForFences` issue the existing
synchronous Venus `vkWaitForFences` protocol call once. Keep imported sync-fd
and other payload types on their existing paths. Measure FPS and call counts.

Risk to review: a blocking Host wait must not occupy the only renderer worker
needed to publish an earlier submit. The current thread-render-server setup and
an ordering round trip should prevent that, but the experiment must include a
bounded timeout and deadlock watchdog.

### P1: Host -> Guest shadow refresh scans and copies too broadly

Every successful fence status/wait can scan all context objects and copy whole
mapped allocations. The actual byte count and time are not currently logged.

Add counters before changing policy:

```text
shadowScanObjects
shadowToHostBytes / shadowToHostCopies / shadowToHostUs
shadowFromHostBytes / shadowFromHostCopies / shadowFromHostUs
shadowFlushCalls / shadowInvalidateCalls
```

Then maintain an allocation dirty list and Host-written/readback ranges rather
than walking the full object table. Do not skip required query/readback data.

### P2: Guest -> Host still scans all allocations on every submit

The dirty-range protocol removed large copies, but
`vkr_device_memory_sync_shadows_to_host` still locks the context object table
and visits every device-memory object for every queue submit. Five submits per
frame amplify this fixed cost.

Possible improvement: enqueue only allocations with a published dirty range.
The queue-submit path should drain that bounded dirty-allocation list, merge
ranges, perform the Host cache flush, and clear the list.

### P3: Private present is synchronous and holds the Host queue mutex

The private present path performs a Guest renderer round trip, a synchronous
vtest request, and a Host queue-locked callback. The callback includes acquire,
copy, present, and source-release fence wait. It costs only about 5.65 ms now,
but it serializes later work on the same queue.

Longer-term options include explicit source-ready/release synchronization and
a separate compatible present queue. This should be considered only after P0
and shadow byte accounting.

### P4: Too many DXVK queue submissions for a small frame

The cube produces roughly five Host queue submits per frame. Some are expected
from DXVK init, upload, graphics, and swapchain work, but every submit triggers
shadow processing and protocol overhead. After P0-P2, identify submit purpose
and determine whether Legacy DXVK can batch any of them without changing
correctness.

### P5: One remaining GPU copy in BrokerPresent

The display path performs one GPU image copy/blit. Removing it requires
rendering directly into an acquired SurfaceQueue/NativeWindow buffer or
importing that buffer into the Venus image allocation with explicit acquire and
release ownership.

This can improve bandwidth and latency, but it does not explain the current
5 FPS because the measured copy/present callback is about 5.65 ms and the
OpenGL path using the same design reaches about 90 FPS.

### P6: Diagnostic logging remains unbounded

Per-submit, per-fence-status, descriptor, and per-present stage messages should
be first-N plus every-N summaries. Product mode should not set verbose
`VN_DEBUG=vtest` unless a diagnostic run explicitly requests it.

## 10. Which upstream designs are useful references

### 10.1 Virtio-gpu resource blobs / Venus HOST3D blobs

This is the closest reference for mapped-buffer transfer. A resource blob
allows Guest and Host to refer to one backing allocation and pass its fd rather
than copying bytes through the command socket. The current code already uses
this abstraction, but falls back to an OHOS SHM shadow when Host Vulkan memory
cannot be exported.

Useful question: can Harmony expose a Host-visible allocation through an
official external-memory handle that vtest can send to the Guest process?

### 10.2 Android gralloc/minigbm/AHardwareBuffer model

Android virgl/gfxstream commonly transfers buffer ownership and fences rather
than framebuffer pixels:

```text
gralloc buffer / dma-buf
  + acquire fence
  -> producer renders
  + release fence
  -> compositor consumes
```

The supported Harmony analogue in this project is:

```text
OHNativeWindow / SurfaceQueue producer
  -> OHNativeImage consumer
  -> App compositor
```

WineHua already uses this analogue for normal OpenGL and Vulkan presentation.
Previous device probing did not establish a supported public cross-NCP raw
dma-buf path. Do not replace the working SurfaceQueue ownership boundary with
an assumed raw fd transport.

### 10.3 Venus fence feedback / renderer sync objects

Upstream fence feedback avoids repeated renderer status calls by publishing a
small status slot. It is the right conceptual model, but the slot must be
backed by storage coherent between Host writes and Guest reads. The current
OHOS two-allocation shadow breaks that assumption.

Potential alternatives for review:

- Have renderer fence completion update the Guest SHM status slot directly.
- Use a vtest sync object/eventfd/futex-style notification for fence completion.
- Export a native sync fence if Host driver and NCP boundary support it.
- Use timeline values in a coherent control-page allocation, not in a Host-only
  mapped allocation.

### 10.4 Direct rendering into the native queue buffer

This is the reference for eliminating the remaining GPU blit, but it needs all
of the following, not only a buffer pointer:

- Official NativeBuffer acquisition/import API.
- A Vulkan external-memory import supported by Maleoon.
- Image format/modifier compatibility.
- Queue-family and layout ownership transitions.
- Acquire/release fence transport.
- Bounded buffer lifetime across NCP and App compositor.
- Resize, detach, background, and destruction acknowledgements.

Treat this as a later optimization after the current synchronization path is
healthy.

## 11. Questions for external review

1. Is a synchronous Guest `vkWaitForFences` -> renderer
   `vkWaitForFences` call safe with Mesa Venus thread render server, or can it
   block the worker that must submit the fence-signaling queue operation?
2. What is the cleanest Venus/vtest mechanism to publish fence completion into
   Guest-visible SHM without copying all Host-visible allocations?
3. Can the current renderer know which mapped allocations may have been
   GPU-written by a submission, so Host -> Guest invalidation can be limited to
   exact buffers/ranges?
4. Can Guest -> Host dirty allocations be tracked in a lock-light list instead
   of scanning the vkr object table at every submit?
5. Does Maleoon expose a usable OHOS external-memory extension for importing an
   `OH_NativeBuffer` into Vulkan, even though generic dma-buf/opaque-fd export
   did not work for current Host-visible allocations?
6. Can private present return source ownership using a semaphore/timeline value
   rather than holding the vkr queue mutex through a synchronous release wait?
7. Which of the approximately five DXVK submits per cube frame are structurally
   required, and which can be batched in Legacy 1.10.3?

## 12. Recommended experiment order

1. Add bounded counters/timing for both shadow-copy directions and fence calls.
2. Add `VN_WINEHUA_DIRECT_FENCE_WAIT=1` for device-only fences only.
3. Rebuild Guest Mesa clean, then build/deploy the full ARM64 HAP.
4. Compare baseline/direct-wait cube FPS, fence calls/frame, shadow bytes/frame,
   Host present time, rotation correctness, and watchdog results.
5. If direct wait improves FPS, keep it behind capability/quirk policy and run
   x64/x86 Vulkan plus D3D11 smoke.
6. Replace full object-table shadow scans with dirty-allocation lists.
7. Rate-limit diagnostic logs and repeat the same A/B.
8. Only then investigate separate present synchronization or direct native
   queue-buffer rendering.

Required correctness gates for every performance change:

```text
visible rotating cube
x64 and x86 official D3D11 smoke
Texture2D.Load and Sample coverage
compute/UAV/query coverage
CPU full-frame readback/upload = 0
per-frame vkDeviceWaitIdle = 0
bounded pending frames and SurfaceQueue backlog
WineD3D/VirGL regression remains stable
```

The dedicated DXVK wall-clock gate is:

```powershell
Invoke-WineHuaAutomation.ps1 -Suite dxvk-long -Prefix reuse `
  -PerfProfile shadow-precise-strong-ring -LongSeconds 3600 `
  -SkipBuild -DeviceId <device-id> -TimeoutMinutes 65
```

Use `-LongSeconds 60` only to validate the automation infrastructure. It does
not satisfy the release long-run gate. The Host coverage check rejects a result
whose recorded wall-clock duration is shorter than the requested value.

## 13. Source and runtime state

```text
Repository: /home/maple/Work/WineHua-build
Branch: feature/render-element-completeness
Main baseline before the long-run update: 6c15447

Mesa:         b2ecdc82d68, clean
virglrenderer:b141b55650d, clean
Wine:         4af0d72b67d, clean
DXVK fork:    0cbbfa7d4c3, with two untracked `.orig` backups only
```

The DXVK `.orig` backup files are not source and must not be committed. The
latest validated HAP is 397,785,474 bytes with SHA-256
`455e666edbaa7f0118f60cb6784041c2c96eb14aeef69c4e26441b9c3d2b3a1a`.
Its 60-second `dxvk-long` infrastructure run passed with 4,484 frames and no
fallback. The full 3,600-second gate remains pending.

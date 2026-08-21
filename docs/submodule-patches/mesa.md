# mesa 补丁清单

> 基线：OpenHarmony-v6.0-Beta1（merge-base e5d8c3f2，2025-06-13）
> 生成：2026-08-01
> 说明：mesa 上游是 OpenHarmony 官方分支，清单服务未来跟随 OH 发布升级时确认每个 hunk 的意图与不变式

## 变更总览

- 修改文件：23（diff 范围内无新增文件）
- 分组：
  - 构建系统 2（meson.build ×2）
  - GL/virgl 呈现桥 6（st_manager.c、vulkan.sym、virgl_vtest_socket/winsys.c/winsys.h、vtest_protocol.h）
  - Venus 呈现桥 6（vn_renderer.h、vn_renderer_vtest.c、vn_device.c、vn_instance.c、vn_queue.c/h）
  - Venus ring 同步 2（vn_ring.c/h）
  - Venus 内存桥 2（vn_device_memory.c/h）
  - 诊断打点 5（vn_command_buffer.c、vn_descriptor_set.c、vn_image.c、vn_pipeline.c、vn_query_pool.c）
- 共性：**全部变更均为运行时环境变量 opt-in（VN_WINEHUA_*/WINEHUA_*/DXVK_WINEHUA_*），无编译期 `__OHOS__` 分支**。默认关闭时行为与上游一致，这是合并冲突时最容易误删、也最容易被上游接受的部分。

## 变更明细

### meson.build: libdrm 依赖
- **为什么存在**：鸿蒙无 DRM/KMS，但 virtio-vulkan（Venus）构建需要 libdrm 依赖项；原条件 `system_has_kms_drm` 在 OH 上不成立。
- **依赖的上游行为**：`with_virtio_vk` 定义、libdrm pkg-config 探测。
- **不变式**：OH 构建必须同时拿到 libdrm 的 dep 与 `src/virtio` 子目录。丢失 → 构建缺依赖失败。
- **验证方法**：`make NATIVE_ARCH=arm64-v8a` 编译期验证。

### src/meson.build: virtio 子目录
- **为什么存在**：同 meson.build——OH 上 `system_has_kms_drm` 为假，需靠 `with_virtio_vk` 打开 `src/virtio`（venus、vtest 协议）。
- **不变式**：`with_virtio_vk` 必须联动 virtio 目录编译；注意上游已废弃此 hunk 的注释版（`#if with_gallium_virgl or with_virtio_vk`），未来合入时以本文件为准。
- **验证方法**：编译 + `libEGL/icd` 产物含 vtest winsys。

### src/mesa/state_tracker/st_manager.c: st_manager_flush_frontbuffer
- **为什么存在**：Wine GL 双缓冲应用在 OH 上无宿主窗口，frontbuffer flush 是唯一 present 信号；上游 `doubleBufferMode` 下提前 return，使 virgl vtest 的 present 拦截点收不到调用。`WINEHUA_VTEST_PRESENT` 设置时强制走完整 flush 路径。
- **依赖的上游行为**：`Visual.doubleBufferMode`/`stfb` 判定、flush 到 winsys `flush_frontbuffer`。
- **不变式**：环境变量未设时行为与上游逐字节一致（GL 桌面回归依赖此开关的精确性）；设置时不得跳过 winsys 回调。
- **验证方法**：Wine 内 GL 应用（explorer 桌面绘制）呈现在宿主可见；`WINEHUA_VTEST_PRESENT` 未设时回归上游行为。

### src/virtio/vtest/vtest_protocol.h: VCMD_WINEHUA_PRESENT / VCMD_WINEHUA_VK_PRESENT
- **为什么存在**：两条私有 vtest 命令（0x57485052 GL 路径、0x57485650 Venus 路径），位于上游命令号（≤24）之外。GL 版带 res_handle/drawable/surface_id，VK 版带 Venus object id（queue_id/image_id，**绝不用 host Vulkan handle**）；回复统一为 status + 64 位 display deadline + serial 回显。
- **依赖的上游行为**：`VTEST_HDR_SIZE` 头布局、`virgl_block_write/read` 阻塞语义。
- **不变式**：**与 virglrenderer fork（thirdparty/virglrenderer 同源修改）必须成对演进**——命令号、字段序、VERSION 号、reply 布局任一漂移即 -EPROTO。协议版本号递增策略必须保持。
- **验证方法**：任一路 present 链路（DXVK 游戏 / Wine GL 窗口）。

### src/gallium/winsys/virgl/vtest/virgl_vtest_socket.c: virgl_vtest_send_winehua_present
- **为什么存在**：GL 路径 present 客户端。经 `winehua_present_surface_page`（TMPDIR 下宿主编写的 shm 页）拿 surface_id 关联宿主窗口；按宿主回传 deadline 做**帧节奏控制**（pacer 表 16 槽/surface_id，deadline 上限提前 50ms），`status==1` 时重试最多 8 次。
- **依赖的上游行为**：`virgl_block_write/read`、socket 序列化、`VTEST_HDR_SIZE`。
- **不变式**：vtest socket 消息必须与协议头同步（与 virglrenderer fork 联动）；pacer 状态在 socket 重连/资源释放时必须清理；-EAGAIN 后重试不吞协议错误。
- **验证方法**：Wine GL 窗口拖动/全屏游戏，宿主帧率接近显示刷新率且无"撕裂后重试风暴"。

### src/gallium/winsys/virgl/vtest/virgl_vtest_winsys.c: virgl_vtest_flush_frontbuffer
- **为什么存在**：`WINEHUA_VTEST_PRESENT` 开启时 flush_frontbuffer 先发私有 present（走专用通道，宿主直接呈现），随后跳过原 transfer_get/upload 路径；未设时完全保持上游行为。附 120 帧节流的 `WINEHUA_VTEST_FRONTBUFFER_LOG` 诊断日志。
- **依赖的上游行为**：dt 存在性判定、`pipe_to_virgl_format`、原有 upload 流程。
- **不变式**：present 路径下不能遗漏资源引用（res_handle 在宿主侧必须已创建）；`!present_mode` 分支不可被重构吞掉。
- **验证方法**：Wine GL 应用画面呈现 + 日志确认 `present_ret==0`。

### src/gallium/winsys/virgl/vtest/virgl_vtest_winsys.h
- **为什么存在**：pacer 表（`WINEHUA_VTEST_MAX_PRESENT_PACERS 16`）与等待统计字段嵌入 `struct virgl_vtest_winsys`；新函数声明。
- **不变式**：pacer 数组为热路径存储，合并时保持字段布局与初始化（`winehua_get_present_pacer` 依赖全零初始态）。
- **验证方法**：编译 + 双窗口呈现。

### src/virtio/vulkan/vn_ring.c: struct vn_ring 热路径 + perf 埋点
- **为什么存在**：**合并冲突最敏感区**。`struct vn_ring` 嵌入：4 个行为开关 + `winehua_perf_log[512]` + 约 40 个 `atomic_uint_fast64_t` 计数 + **3×512 数组 `perf_reply_count/total_us/max_us`（VN_RING_PERF_COMMAND_TYPE_COUNT=512，按 VkCommandTypeEXT 分类 reply 耗时）**。`VN_WINEHUA_PERF_SUMMARY=1` 时在 submit/mutex/seqno/space/roundtrip/notify/reply 各热路径计时，`vn_ring_perf_maybe_log` 每 60 万次 submit 输出 `WineHuaGuestPerf`（Heaven 单帧数千次提交，节流避免诊断变瓶颈）。
- **为什么鸿蒙需要**：vtest socket 传输下 ring 提交完全同步，帧时间"去哪了"只能靠 guest 侧计数归因；DXVK 的 present 路径不做 fence wait，须从 submit 侧自证等待分布。
- **依赖的上游行为**：`vn_ring_shared`（guest/host 共享字）、`vn_ring_submit_internal` 的 IDLE 位通知合并、`vn_ring_wait_seqno/space` 的 relax 循环。
- **不变式**：perf 开关默认关 → 只多一次 `if` 分支读，不得改变任何同步语义；512 数组随 `VkCommandTypeEXT` 枚举增长须保持足够大；**perf 与通知逻辑不能互相引入新的锁序**（`vn_ring_lock` 只是 mtx 外计时）。
- **验证方法**：`VN_WINEHUA_PERF_SUMMARY=1` 跑 Heaven/游戏，日志有 submit/seqno/space/roundtrip 分布；默认关闭时帧率与上游基线无差异。

### src/virtio/vulkan/vn_ring.c: vn_ring_store_tail / force_notify
- **为什么存在**：两处同步修复。①`VN_WINEHUA_STRONG_RING_BARRIER=1` 在 tail 发布前加 seq_cst fence——Box64 下 ring payload 可能被 **native ARM64 memcpy** 拷贝，x86 TSO 假设不成立，此开关用于证明 tail 发布是否会超过共享内存写。②`VN_WINEHUA_ALWAYS_NOTIFY_RING`/`VN_WINEHUA_REMOTE_MEMORY_SYNC` 强制通知：WineHua 的 Guest/Host 分离 shadow 映射下，宿主侧写入的 IDLE 状态字 guest 读不到，通知不能依赖 IDLE 位，否则对象创建在 private present 前不可见。
- **依赖的上游行为**：release-acquire 配对（host acquire 读 tail）、1ms 通知合并窗口。
- **不变式**：强制通知只改变"何时发 notify"，不改变 seqno 计数与提交顺序；strong barrier 保持 opt-in（x86 默认路径不能退化）。
- **验证方法**：DXVK 游戏启动/切场景不挂起；`VN_WINEHUA_REMOTE_MEMORY_SYNC=1` 时启动场景回归。

### src/virtio/vulkan/vn_ring.h
- **为什么存在**：`vn_ring_perf_stats`（28 字段）、`VN_RING_PERF_TOP_REPLY_COUNT 8`、`enum vn_ring_perf_rpc` 与 4 个导出 API。
- **不变式**：`vn_ring_get_perf_stats` 宏加载与结构体字段必须同步（跨文件对不上会读到脏字段，不崩溃但日志错乱）。
- **验证方法**：perf 摘要日志字段齐全。

### src/virtio/vulkan/vn_queue.c: vn_WaitForFences（重点）
- **为什么存在**：**fence wait 三模式**。OHOS vtest 内存桥下 Host GPU 写 guest 共享内存的 fence feedback 不可用（guest/host 分离 shadow 映射），而 DXVK 每帧 waitForFences 若退化为跨 socket 轮询 GetFenceStatus 则帧率灾难。①`VN_WINEHUA_EVENT_FENCE_WAIT=1`：每 fence 惰性建 renderer timeline sync（`winehua_event_sync`，monotonic 递增复用），提交等待 ring seqno 的 marker batch，一次 `vn_renderer_wait`（3s watchdog 防丢事件死锁）后零超时确认 `vkWaitForFences`；②`VN_WINEHUA_DIRECT_FENCE_WAIT=1`：纯 device-only fence 直接一次 host `vkWaitForFences`；③默认：上游轮询路径，仅加 status_call 计数。
- **依赖的上游行为**：`vn_sync_payload_external`（ring_idx/ring_seqno）、`vn_update_sync_result`、`OS_TIMEOUT_INFINITE` 绝对超时语义。
- **不变式**：watchdog 超时后必须回落原轮询路径（VK_TIMEOUT→VK_NOT_READY 转移）——丢事件绝不能把无限 wait 变死锁；marker 信号后零超时确认不可省（防 device-lost 被吞成假成功）；`submission_valid` 标志保证 Reset 后不误用旧 marker；`winehua_event_mutex` 保护跨线程 host 访问。
- **验证方法**：DXVK 游戏长时间运行无挂起/无 DEVICE_LOST；`VN_WINEHUA_EVENT_FENCE_WAIT=1` 下每帧 wait 日志显示 event 模式命中。

### src/virtio/vulkan/vn_queue.c: vn_queue_submit / vn_CreateFence 等
- **为什么存在**：`external_payload` 安装处加 mutex + `submission_valid=true` 发布；fence 创建/销毁/重置维护 event sync 生命周期。诊断：`WINEHUA_DXVK_TRACE_CAMERA=1` 打印每次 submit 的 guest cmd 句柄↔cmdId 关联。
- **不变式**：`vn_GetFenceStatus` 的 perf 计时要包裹真实 RPC 而非本地分支（`VN_RING_PERF_RPC_FENCE_STATUS`）。
- **验证方法**：相机类场景（采样器/descriptor 关联）定位时开 trace；常规游戏回归。

### src/virtio/vulkan/vn_renderer_vtest.c: vn_winehua_present（重点，导出入口）
- **为什么存在**：**WineHua 专属呈现桥**。DXVK present 时经 `vkGetDeviceProcAddr("vn_winehua_present")` 进入（见 vn_device.c），带 Venus object id 走 vtest socket 发 `VCMD_WINEHUA_VK_PRESENT`，宿主端 virglrenderer fork 按 queue_id/image_id 在 host Venus 上下文呈现并回传 display deadline。内部：①**节奏**——上一帧 deadline 在渲染完成后 sleep（≤100ms），与显示周期重叠渲染；②**ring drain**——present 前 `vn_ring_roundtrip + vn_ring_wait_all`，因 Venus roundtrip 只插跨传输 marker 不保证 ring worker 消费完，需 drain 防止 present 拿到 host queue mutex 先于生产者 QueueSubmit（对象未发布竞态 -EAGAIN 重试 ≤8 次 + 指数退避）；③**-EAGAIN 绝不上抛**（vulkan thunk 把负结果映射为 DEVICE_LOST 毒死 x86 进程）；④perf 汇总 `WineHuaGuestFramePerf`（120 帧节流，含 top-8 reply 耗时排序）。
- **依赖的上游行为**：`vn_ring_roundtrip`/`vn_ring_wait_all` 语义、`sock_mutex` 串行化、`vn_object_id` 稳定性。
- **不变式**：drain 是正确性要求不是性能优化——合并后必须保留"present 前 ring 全量 drain"；deadline 写回 `queue->winehua_next_present_deadline_ns`（队列操作外部同步，可安全消费）；重试退避不得无限循环。
- **验证方法**：DXVK 游戏（Heaven/3DMark）持续呈现无 DEVICE_LOST；`VN_WINEHUA_PRESENT_TRACE=1` 看 drain/reply 时序。

### src/virtio/vulkan/vn_renderer_vtest.c: vtest_winehua_present / defer_shmem_unref
- **为什么存在**：`ops.winehua_present` 钩子实现（sock_mutex 下收发、serial 校验）。`VN_WINEHUA_DEFER_SHMEM_UNREF=1`：诊断模式——guest munmap 后延迟宿主资源释放，避开 3s shmem cache 过期与活跃 ring/stream 竞态，随 vtest 连接销毁回收（**明确非生产策略**）。
- **不变式**：defer 模式只在显式设置时生效；munmap 仍必须立即执行。
- **验证方法**：`VN_WINEHUA_PERF_SUMMARY=1` 长跑游戏无 shmem 竞态崩溃。

### src/virtio/vulkan/vn_renderer.h / vn_device.c / vn_instance.c / vulkan.sym
- **为什么存在**：呈现桥基础设施。`vn_renderer_winehua_present` 结构 + ops 钩子（无实现返回 -ENOSYS，**保持 virglrenderer 后端可独立工作**）；`vn_winehua_present` 以 `visibility("default")` 导出并挂到 GetDevice/GetInstanceProcAddr——避免 DXVK 在 Vulkan 调用活跃时二次 dlopen 本库；vulkan.sym 导出。
- **不变式**：`GetInstanceProcAddr(NULL)` 路径也要能解析该符号；-ENOSYS 回退语义必须保留（virgl 后端/非 vtest 环境不崩）。
- **验证方法**：编译 + 链接检查导出符号。

### src/virtio/vulkan/vn_device_memory.c/h、vn_device.c/h、vn_queue.c: remote/persistent map 同步
- **为什么存在**：OHOS vtest 用 shadow 文件映射而非直接映射 host VkDeviceMemory，`vn_renderer_bo_flush` 只对本地映射有效。`VN_WINEHUA_REMOTE_MEMORY_SYNC=1` 时：①新增 `map_offset` 记录（上游只有 map_end）；②Unmap 时对 HOST_COHERENT 内存自动 flush 全映射区间（shadow 下 coherent 写不进 host dirty 列表）；③Flush/Invalidate 额外走 Venus 协议 `vn_call_vk*` 让宿主更新映射。VKD3D 的 upload heap 会长期保持映射，常量/实例数据没有 Unmap 边界；`VN_WINEHUA_PERSISTENT_MAP_SYNC=1` 因而在 device 上追踪当前映射的 coherent allocations，并在 QueueSubmit/QueueSubmit2 进入宿主前通过同一协议发布映射范围。
- **依赖的上游行为**：`vn_FlushMappedMemoryRanges` 的 bo flush 分支、`VK_WHOLE_SIZE` 语义。
- **不变式**：`map_offset/map_end` 与上游 `map_end` 的初始化/清零位置一致（Unmap 清零）；mapped list 的加入、移除和提交遍历由 device mutex 保护；persistent 开关必须保持默认关闭且仅由 VKD3D profile 注入，DXVK 路径不能承担逐提交 flush；任一开关关闭时上游同步语义不变。
- **验证方法**：`VN_WINEHUA_REMOTE_MEMORY_SYNC=1` 跑 DXVK 游戏；`REMOTE_MEMORY_SYNC=1 + PERSISTENT_MAP_SYNC=1` 跑 vkd3d-proton `triangle.exe` 与持续映射常量/实例缓冲的 `gears.exe`，确认齿轮连续旋转、Host 日志有 submit 前 flush 且无 device loss/全局 wait；关闭 persistent 开关回归 DXVK 默认路径。

### 诊断打点组（vn_command_buffer.c / vn_descriptor_set.c / vn_image.c / vn_pipeline.c / vn_query_pool.c）
- **为什么存在**：guest 对象身份追踪。①`WINEHUA_DXVK_TRACE_CAMERA=1`：CmdBindDescriptorSets 打印 guest cmd/set 句柄↔Venus id（10 万条上限节流）；②`DXVK_WINEHUA_TRACE_SAMPLED=1`：UpdateDescriptorSets（含 template 展开）打印 image 类 descriptor 写入 + CreateImage/ImageView 打印 id 映射——用于 DXVK 采样器/相机场景定位"采样错图"类问题；③`WINEHUA_VKR_TRACE_PIPELINE=1`：CreateGraphicsPipelines 打印创建参数与结果；④vn_query_pool.c：GetQueryPoolResults 计时进 ring perf（`VN_RING_PERF_RPC_QUERY_RESULTS`）。
- **依赖的上游行为**：各自 entrypoint 的正常流程，打点全部在调用前/后插入。
- **不变式**：全部 stderr/`vn_log` 输出必须受环境变量门控；`vn_ring_perf_record_rpc` 的 `VK_NOT_READY` 计数语义（fence/query 未就绪统计）保持。
- **验证方法**：相机类 DXVK 应用开 trace 看 id 关联；`VN_WINEHUA_PERF_SUMMARY=1` 下 query/fence 分布进帧摘要。

## 合并冲突敏感度分级

- **高（跨文件协议/结构体布局）**：vtest_protocol.h、vn_ring.c struct vn_ring、vn_queue.h struct vn_fence、vn_renderer.h ops、virgl_vtest_winsys.h——与 virglrenderer fork 的协议联动、结构体内存布局，冲突必须人工评审。
- **中（行为开关）**：vn_ring.c 同步修复（force_notify/strong barrier）、vn_queue.c WaitForFences 三模式、vn_renderer_vtest.c present 主流程、vn_device_memory.c、st_manager.c。
- **低（纯诊断）**：五个打点文件 + perf 计数，可整块跳过或延迟合入，不影响功能。

## 跟进 OH 新版本时的检查要点

1. 协议头两文件与 virglrenderer fork 同步 diff，确认 VERSION/字段序未漂移；
2. `struct vn_ring` 若上游改动字段（如新增共享状态），perf 数组与开关字段的嵌入位置需复核；
3. 上游若引入新 `VkCommandTypeEXT` 枚举值，确认 < 512 槽约束仍成立；
4. 上游 vtest 命令号若增长到 0x57 区域（不可能，远小于），需重新选址。

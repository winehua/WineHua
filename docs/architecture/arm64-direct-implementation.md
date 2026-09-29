# ARM64 Direct 渐进实施方案（feature/proton-wine-ohos）

> **2026-09-29 晚，手机路线决定**：Mate 80 重新在线后，反向共享图像 P0 已实现并实测。Guest 的可导出 Vulkan 图像分配实际进入 `vkAllocateMemory → OH_NativeBuffer_Alloc → HDI/SAMGR`，仍超时、0 帧，未解决手机 Direct。按用户最新意见，**手机继续原 Venus 路线，暂停 Direct 上屏研究**；实测 c56 包的 Venus + DXVK 2.6.2 冷启动立方体 **871 帧、呈现与视觉检查通过**。平板 Direct 进度不变。详见 [本轮证据与边界](mate80-direct-reverse-export-20260929.md)。以下“最新包”均需结合设备与时间阅读。

> **2026-09-29 最新装机与验证**：signed HAP `4b92988e…a287f`。第一版 d371 遗漏 caller 的外层递归 USER 锁，诊断短测 12/12 不足以证明修复；第二版同时修正 caller/callee，18 项宿主检查及原版/第一版死锁负对照通过。关闭采样器的正式 AMD64/FEX 75 秒负载 **12/12 冷启动通过**（Venus、Direct 各 6）；另一次 Direct 运行中 resize **878 帧、resize 后 818 帧通过**。本组 Direct 两进程 CPU/accepted present 中位数低约 4.85%，仍未完成真实游戏/整体收益验收，保持 opt-in。参见 [深审与根因](direct-startup-lock-audit-20260929.md)、[最终实机报告](direct-startup-fix-validation-20260929.md)。手机继续搁置，Steam/FEX/default 未调整。以下各旧包记录是阶段历史。

> **2026-09-29 生产 presenter 最新包**：signed HAP `00b507b6…3c5e6`。fusion 已独立为 `DirectVulkanPresenter`，产品调用点不再使用诊断 probe。fusion 1063 帧、运行中 resize 1057 帧及生产输出重建、虚拟桌面单窗口 586 帧通过；同 App 单窗口通过后的双窗口 scene 11 阶段/触摸 2/2 通过。未经预热启动 scene 的两次 120000 ms 超时仍待定位，不能称冷启动矩阵已通过；[实现、失败记录与证据](direct-vulkan-presenter-20260929.md)。手机继续搁置。

> 2026-09-26，基于主仓库 `7fc70bfc`、`thirdparty/wine-valve` `8b5bee4` 和本机 OpenHarmony SDK 头文件核对。`WineHua_ARM64_Direct_渐进式架构重构指导.md` 是目标与阶段建议；此文把它落到当前分支的实际进程、图形和构建接口。设备能力和性能结论必须以新 HAP 的实测为准。

> **2026-09-29 最新平板进展**：按用户要求搁置手机验证，已接入真正的 **HAP Vulkan 桌面 compositor**。平板 Direct 双进程 DXVK 窗口、遮挡/透明、最小化/恢复、resize、全屏退出与实际触摸坐标已通过实机 fixture 验收；游戏图像沿共享 NativeBuffer 直接 GPU 导入，关闭诊断回读。仍是显式 opt-in，尚未完成真实游戏、性能 A/B 和完整异常/窗口矩阵；详见 [本轮实现与验收](direct-vulkan-desktop-20260929.md)。历史记录中的“下一步手机 P0”已由本次工作顺序取代。

> **公共核心最新包复核**：signed HAP `64cbd659…3021f`，payload `smoke-v2-c87c4aa03bfc`；桌面已从 probe/friend 迁到独立 `DirectVulkanContext`。双槽输出 resize 8/8、Wine/DXVK 桌面 11/11 阶段、真触摸 2/2、fusion 707 帧均通过；完整证据和采集时序修复见 [公共核心记录](direct-vulkan-context-20260929.md)。前一包 `65203256…6414c` 的 Create NCP + Venus/EGL 对照另存，不算此次新包回归。之前 Start NCP 的 timeout、安装冷启动边界和整个重构剩余项仍单列。

> **手机历史结论（2026-09-28～29）**：Mate 80 的早期 fork server、真实 Wine x64/x86 Direct 离屏和 DXVK 2.6.2 功能资格检查已通过；Direct 上屏与跨进程 NativeBuffer 注册仍未通过。通用 OPAQUE_FD 能力检查未通过，OHOS NativeBuffer 目标格式支持 import/export，但不能代替真实共享图像验证。以下手机记录保留；恢复手机工作时先解决注册/系统服务上下文，不调整 Steam/FEX。

> 后续设计：[Direct 渲染与共享图像呈现设计](direct-render-shared-image-design.md)，明确本地 Vulkan、共享图像与控制 IPC 的边界，以及手机 P0～P4 的实现顺序。

## 当前基础与边界

- `Makefile` 默认 `WINE_SRC=thirdparty/wine-valve`，已有 `WINE_ARCH=aarch64` 的 Wine/FEX/ARM64X DXVK 构建分支。32 位 Wow64 在 ARM64 Wine 下默认 FEX，box64 是显式回退。
- 现有 D3D11/Vulkan 生产路径仍是 Wine 私有 swapchain → Guest Mesa Venus → vtest → virglrenderer → Host Vulkan。`thirdparty/wine-valve/dlls/win32u/vulkan.c` 的私有 present 不能直接视为 Direct WSI。
- `graphics/egl_renderer.cpp` 创建 `OH_NativeImage` 和 producer `OHNativeWindow`；`graphics/graphics_broker.cpp` 用 `OH_NativeWindow_WriteToParcel` 将它传给 `virgl_child`，子进程用 `ReadFromParcel` 接收。这证明跨进程 producer 交接在本项目中已有可复用方式，但当前消费端是 GLES external OES，不是计划中的 Vulkan compositor。
- `graphics/venus_surface_presenter.cpp` 已在 VirGL 子进程中对 producer window 调用 `vkCreateSurfaceOHOS`、创建 swapchain 并 present。D2 可参考其中的原生 OHOS WSI 建链与清理顺序，但要保持 probe target 独立，不能把 Venus 的资源和私有 present 接到 Direct 上。
- Wine 游戏进程经 `proc/broker.cpp` 调用 `OH_Ability_StartNativeChildProcess("libwine_child.so:Main", ...)`。该 API 传 entry 参数和 fd，不返回 IPC remote proxy；VirGL 使用的 `OH_Ability_CreateNativeChildProcess` 则会返回 IPC proxy。Direct WSI 接入 Wine 前，必须解决这个交接差异。
- 现有 `smoke/suites/` 是 Windows/Wine 用例；NCP 原生探针需要独立入口，不能把 guest Vulkan smoke 的通过当作 D0 通过。

## 必须先固定的接口选择

1. **Direct 不复用 Venus 的私有 present。** NCP 中的 ARM64 Vulkan loader、物理设备、queue 和 OHOS WSI 必须独立于 Guest Mesa 与 vtest。D0 在独立 NCP 动态加载系统 `libvulkan.so`，同时记录实际库路径、设备名和扩展列表，防止误连包内 guest loader。
2. **跨进程传 producer window 用 IPC parcel，不能传指针或仅传 surfaceId。** SDK `external_window.h` 明确说本进程创建的 surface 不能靠 `OH_NativeWindow_CreateNativeWindowFromSurfaceId` 跨进程取得。现有 `WriteToParcel/ReadFromParcel` 是可验证起点；D1 要在独立探针 NCP 中复测所有权和销毁顺序。
3. **Vulkan consumer 要新建队列，不混用现有 GLES NativeImage。** SDK 规定 `OH_ConsumerSurface_Create` + `OH_NativeImage_AcquireNativeWindowBuffer/ReleaseNativeWindowBuffer`，不能和 `OH_NativeImage_UpdateSurfaceImage` 同时使用。D2 先在独立诊断 XComponent 上完成，避免让 EGL 和 Vulkan 同时驱动产品 XComponent。
4. **Buffer 身份不是窗口身份。** 用 `(clientPid, toplevelId, generation)` 识别 Direct surface；resize、销毁重建后递增 generation。`OHNativeWindowBuffer`/`OH_NativeBuffer` 的 Vulkan import 按 buffer 身份缓存，generation 变化时清除，不在每帧导入/释放。
5. **fence fd 所有权逐项记录。** acquire 返回的 fd 由调用方最终关闭；flush/release 成功后的 fd 由系统接管。先查询设备对 `VK_OHOS_external_memory`、`VK_KHR_external_semaphore_fd` 和 `SYNC_FD` 的实际支持，再选择 GPU wait/signal 实现；不把头文件有声明当作设备支持。

## Gate 与代码落点

| Gate | 代码落点 | 只新增/改变什么 | 通过证据 |
| --- | --- | --- | --- |
| L0 旧路径基线 | 现有 `automation/smoke.py`、`smoke/suites/`，另存设备证据 | 固定 HAP/设备/档位，跑 `core`、`wine-vulkan-present`、DXVK、Steam 及 Heaven/PAL4 手动场景；分别记录 ARM64 Wine + Venus 与仍需维护的 x86_64 旧路径 | 进程、画面、FPS/帧时、CPU、拷贝量和崩溃数据可复核；已知失败单列，不要求先修好 |
| D0 NCP Vulkan 离屏 | 新增 `entry/src/main/cpp/direct/ncp_vulkan_probe.cpp` 为独立 `libdirect_vulkan_probe.so`；`entry` 增加测试触发与结果收集；CMake 单独 target | NCP `dlopen` 系统 Vulkan → instance/device/queue → 16×16 image clear → copy 到 staging buffer → fence wait → 校验像素；不访问 Wine/窗口 | 至少 20 次启动均通过，loader/设备确认、像素正确、退出与 RSS/fd 无持续增长；失败有阶段、`VkResult` 和 pid |
| D1 BufferQueue 交接 | 独立 `direct_surface_probe_launcher.cpp` 与 `direct_surface_probe_child.cpp` | App 创建 `OH_ConsumerSurface_Create`、设置尺寸/usage、取得 producer window，经 IPC parcel 交给探针 NCP；子进程 request/flush，App acquire/release；测试双 buffer 循环、resize、异常退出 | 跨进程反复创建/销毁无泄漏、无旧尺寸帧、无队列耗尽；此 Gate 用可识别图案即可，不要求 Vulkan |
| D2 Vulkan producer/consumer | 新增 `direct/direct_buffer_compositor.{h,cpp}`，先挂独立诊断渲染目标 | NCP 用 `VK_OHOS_surface` 向 D1 producer 渲染；App `AcquireNativeWindowBuffer` → `OH_NativeBuffer` → Vulkan import/cache → 合成到 XComponent；传递并回收 acquire/release fence | 连续 60 FPS、无 CPU readback/memcpy/upload、无逐帧 import 或 `vkDeviceWaitIdle`、resize/recreate 后无撕裂死锁，内存/fd 稳定 |
| D2.5 Wine NCP 控制通道 | `proc/broker.cpp`、`proc/wine_child.cpp` 的 **Direct 专用**启动变体；旧 `StartNativeChildProcess` 路径保留 | 验证如何让每个 Wine NCP 取得 IPC proxy 和 producer window。优先试 `CreateNativeChildProcess` + IPC parcel 下发原有 argv/env/fd，再启动 Wine 主函数；子进程须经 IPC 回报 pid，原有 ProcessBroker 语义不变 | launcher → game 多进程、fd/环境继承、wineserver、退出回调与进程登记均与旧路径等价；失败可退回旧启动方式 |
| D3 Wine Direct WSI | `thirdparty/wine-valve/dlls/win32u/vulkan.c` / `dlls/winevulkan/`，App 侧 Direct surface token 服务 | 将 surface/swapchain/present 的 Venus 与 OHOS 实现收在明确的 backend 接口后；进程启动时选择 `WINEHUA_VULKAN_BACKEND=venus/direct`，Direct 使用 D2 producer window 和系统 Vulkan | Wine 原生 Vulkan smoke 通过创建、acquire、present、resize、device-lost；同一 HAP 的 Venus 路径仍通过 |
| D4 DXVK | 现有 ARM64X DXVK 构建与打包入口 | 先接 DXVK cube，再接 D3D11 游戏；只在 D3 后处理扩展映射、format、memory、swapchain 兼容 | Legacy/Direct A/B 的画面与功能一致，性能改善可量化；设备能力不够时明确回退 |
| D5 多窗口 | compositor 的窗口树与新的 Direct layer 管理 | 由 Wayland 提供位置、z-order、popup、input；Vulkan compositor 对每个已就绪 buffer 合成，先单窗口再 popup/多窗口 | Pad Desktop 的 resize、focus、遮挡、透明和多进程窗口通过回归 |

OpenGL/Zink、Audio Direct、Gamepad Shared State 是后续独立 Gate；不要随 D0–D4 一起切换。

## D0 已实现契约与设备结论（2026-09-26）

- `winehua.mode=direct-probe`（`direct-probe-create` 是同义入口）使用 `OH_Ability_CreateNativeChildProcess`。子进程在独立 NCP 运行离屏 Vulkan 探针；父进程通过 IPC proxy 取回带版本号的固定结果，再发送 finish 请求允许子进程退出。NAPI 用异步 work 等待，ArkUI 线程不阻塞。结果写入应用 cache 的 `direct-vulkan-probe.json`，并用 `DIRECT-D0` 输出状态、PID、父进程 fd/RSS 采样。
- `winehua.mode=direct-probe-start` 保留 `OH_Ability_StartNativeChildProcess` 命名 fd 路径作为对照。同一 signed HAP 与设备上，Start NCP 虽加载 `/system/lib64/libvulkan.so`，仅枚举 8 个实例扩展，API 1.0–1.3 的 `vkCreateInstance` 都返回 `VK_ERROR_INCOMPATIBLE_DRIVER (-9)`；Create NCP 枚举 13 个扩展，创建 Maleoon 910 设备成功并通过像素校验。原因尚未确定，不能据此声称所有 Start NCP 都没有 GPU 权限；Direct 后续 Gate 必须使用已验证的 Create 进程类型并核对权限/环境。
- 结果包含 `gate`, `status`, `stage`, `launchMode`, `pid`, `loaderPath`, `deviceName`, `apiVersion`, `vkResult`, `pixelCheck`, `elapsedMs` 和父进程 fd/RSS。失败时保留阶段与错误码。设备是 USB MatePad Mini `5KPBB25818203996`；D0 完整验收时的签名 HAP SHA-256 为 `4824f57fc59759704892e2d393e69f3080b8e9aa9a9d8670bee402ecc81cb996`。
- D0 验收 HAP 的 Create 路径连续 20 次得到 20/20 `PASS`，子进程均报告 `/system/lib64/libvulkan.so`、Maleoon 910、`pixel=1`，PID 均不同且测试后无探针子进程残留。父进程 fd 20 次均为 42；RSS 从 95360 KiB 到 95448 KiB，范围很小，没有持续线性增长。逐次父/子进程日志在 [D0 父进程](evidence/direct-d0-final-4824-parent.log)与 [D0 子进程](evidence/direct-d0-final-4824-child.log)。前一 HAP 另有 20/20 `PASS`；更早一次首轮启动的 RSS 波动被设备内存回收影响，因此以此连续采样为准。
- 同一最终 HAP 的 Start 路径连续 3 次仍在 `vkCreateInstance=-9` 失败。对照探针发现该 API 返回后父进程的命名 socket fd 仍然打开；未关闭时每启动一次 fd 增加 1。D0 探针现在核对 fd 身份后关闭本地副本，再跑 3 次 fd 稳定为 42。生产 `proc/broker.cpp` 中“所有 fd 的所有权已转移”注释与此设备观察不符；应另做 Wine/Steam 进程创建回归并审计，不能直接把 D0 的实验性处理当成生产路径已修复。
- D0 HAP 没有注入 `WINEHUA_VULKAN_BACKEND=direct`，也没有改 Wine、DXVK、Mesa、VirGL 或现有 XComponent。D1 继续独立探针，不接产品呈现链。

## D1 独立 BufferQueue 探针（2026-09-26）

- `winehua.mode=direct-surface-probe` 在主进程创建 64×64 `OH_ConsumerSurface`，把 producer `OHNativeWindow` 写入 IPC parcel。`libdirect_surface_probe.so` 是另一个 `CreateNativeChildProcess` NCP，读取 window、request buffer、CPU 填充逐帧可识别 RGBA 图案并 flush；主进程只用 `OH_NativeImage_AcquireNativeWindowBuffer/ReleaseNativeWindowBuffer` 消费，按 stride 校验首、中、末像素。前三帧为 64×64，后三帧改为 96×48，并逐帧校验尺寸。
- 每组三帧先连续提交两帧，再消费两帧，核对同时在队列中的两个 buffer sequence 不同；第三帧验证释放后可复用。设备上第一组为 `2927558660,2927558661`，resize 后第二组为 `2927558662,2927558663`。初版子进程在每次 flush 后销毁 producer window，虽收到 frame-available 通知，消费者仍持续得到 `NATIVE_ERROR_NO_BUFFER (40601000)`；把 producer window 保持到整轮结束后六帧全部通过。最终实现的 finish IPC 先让子进程释放 producer window，再向父进程确认；父进程收到确认后才销毁 ConsumerSurface。这是 D2 surface 生命周期的明确约束。
- `winehua.mode=direct-surface-abort-probe` 在双 buffer 往返成功后调用 `OH_Ability_KillChildProcess`，随后清理 ConsumerSurface。设备上该模式返回 `PASS stage=aborted_clean frames=2`，紧接着的常规六帧模式仍 `PASS`，没有探针子进程残留。
- 当前 D1 签名 HAP SHA-256 是 `2422a6c285049406109c36681d9177ab150da120ef345158c2c32f26e58931f5`。此包交替执行 20 次常规模式与 5 次异常退出模式，25/25 `PASS`、25 个不同子进程 PID；子进程日志合计 130 帧，与 `20×6+5×2` 一致。父进程 fd 全程为 42，RSS 从 95636 KiB 到 95692 KiB，未见持续增长，也无探针子进程残留。逐次日志在 [D1 父进程](evidence/direct-d1-final-2422-parent.log)与 [D1 子进程](evidence/direct-d1-final-2422-child.log)。同包 D0 Create 入口再测为 `PASS`。
- D1 只证明 CPU 图案的跨进程 BufferQueue 交接和生命周期；resize 由尺寸与逐帧图案校验，尚未接入产品窗口的 generation 身份。Vulkan WSI、NativeBuffer GPU import、fence GPU 同步或零拷贝合成仍属 D2 Gate。Wine、DXVK、Mesa、VirGL 以及产品 XComponent 均未接入 D1 探针。

## D2 设备能力预检（2026-09-26）

- D0 Create 探针的协议升为 v3，在原有离屏像素校验之外，枚举所选物理设备和实例的扩展，查询 `SYNC_FD` external semaphore 的 import/export 特性，以及 RGBA8 `OHOS_NATIVE_BUFFER` sampled image 的 import 特性。探针优先创建设备支持的较高 Vulkan API 版本（1.3→1.0），因为 1.0 实例不暴露核心 1.1 的 external properties 查询。查询本身不启用扩展，也不代替实际 `OH_NativeBuffer` import、GPU fence 或 WSI 测试。
- JSON 的 `nativeCapabilities` 位含义：bit 0 `VK_KHR_surface`、1 `VK_OHOS_surface`、2 `VK_KHR_swapchain`、3 `VK_OHOS_external_memory`、4 `VK_KHR_external_semaphore_fd`、5 `VK_KHR_external_memory_fd`、6 `VK_EXT_queue_family_foreign`、7 `SYNC_FD` exportable、8 `SYNC_FD` importable、9 RGBA8 OHOS NativeBuffer image importable；`deviceExtensionCount` 是所选物理设备报告的扩展总数。任一位缺失都要结合后续实际路径判定，不能把头文件声明或单个位直接当作 D2 成败。
- USB MatePad Mini `5KPBB25818203996` 上，签名 HAP `2f7344c4ae93dbc1e000064aa7ed002bfeeb836f4ef49b4c3d3e25aaafb270a9` 的 Create NCP 结果为 `PASS`、Maleoon 910、`/system/lib64/libvulkan.so`、像素校验通过。设备扩展数 68，`nativeCapabilities=1023 (0x3ff)`，上述 10 位全有；父进程 fd 为 42。原始 JSON 在 [D2 能力结果](evidence/direct-d2-capability-2f73.json)。此结果证明可开展 WSI、import 和 `SYNC_FD` 实验，但未证明实际 BufferQueue 图像可导入、跨进程 fence 无死锁、或合成性能达标。
- 首次 `aa start` 曾因设备锁屏返回 `10106102`，手动解锁后同包运行成功。ArkTS `hilog.info` 的十六进制格式在设备上把后续字段错位显示，已改为十进制；上述数值以原始 JSON 和 NCP 日志为准。
- 修正日志格式后重构建并覆盖安装的能力预检 HAP SHA-256 为 `3fcf359caff3d4adc729765453f71ca50a182faadc5407e7ebfb38cfe2fd5ed4`。同包 D0 Create 再次 `PASS`、`nativeCapabilities=1023`、扩展数 68、fd 42（[最终 JSON](evidence/direct-d2-capability-3fcf.json)）；D1 常规六帧回归也 `PASS`、fd 42。

## D2 WSI producer 独立探针（2026-09-26）

- `winehua.mode=direct-gpu-surface-probe` 使用独立 `libdirect_gpu_surface_probe.so`，沿用 D1 的 ConsumerSurface、IPC parcel 和 finish 所有权顺序。子进程创建系统 Vulkan instance/device/OHOS surface/swapchain，对 swapchain 图像用 GPU clear 提交六帧；父进程逐帧 Acquire、等待 acquire fence、CPU map 校验 RGBA 图案，然后 Release。前三帧 64×64，后三帧 96×48；resize 时子进程重建 swapchain。此模式逐帧提交和消费，尚未测并发队列深度。
- 签名 HAP SHA-256：`3006c674d4a93b06ce1f28613a7a83c1765f414dc31f1c462e13928c5e3e0250`。MatePad Mini 连续 101 次启动，101/101 `PASS`、606/606 帧正确、101 个不同子进程 PID、无残留子进程；父进程 fd 全程 42。RSS 首次 91216 KiB、预热后最高 96136 KiB、末次 93508 KiB，没有随轮次持续增加。[父进程逐次日志](evidence/direct-d2-wsi-101-parents.log)、[子进程逐帧日志](evidence/direct-d2-wsi-101-children.log)、[首轮结果](evidence/direct-d2-wsi-first.json)、[末轮结果](evidence/direct-d2-wsi-101-final.json)。同包 D0 Create 与 D1 常规六帧回归均 `PASS`、fd 42。
- 此探针证明 Create NCP 的系统 Vulkan WSI 能向跨进程 BufferQueue 写入可辨像素并处理 resize。父进程仍用 CPU map 检查，子进程每帧等待提交 fence；这不是 GPU import、零拷贝合成、GPU acquire/release fence 或 60 FPS 性能验收。下一节单独验证实际 NativeBuffer 的 image/memory/view 导入和缓存。

## D2 NativeBuffer import/cache 独立探针（2026-09-26）

- `winehua.mode=direct-gpu-import-probe` 在上节的 Vulkan WSI producer 基础上，每组各提交四帧。App 消费端先等待 acquire fence，再按 NativeBuffer sequence 缓存 Vulkan image、memory 和 image view；同一 generation 重访 buffer 时复用这些对象，resize 前清空旧 generation 的缓存，最后释放所有 Vulkan 对象和 NativeBuffer 引用。探针用 `vkGetNativeBufferPropertiesOHOS` 确认真实 buffer 的格式和内存类型，通过 `VkImportNativeBufferInfoOHOS` 分配/绑定内存，并创建 RGBA8 sampled image view。每组的第四帧应复用已有 buffer，因此合计应是六次导入、两次缓存命中。
- 签名 HAP SHA-256：`6bfeaa4ab362e28d262ee1dcf28a125609b50a2e3e110cda66334b40a92055a2`。MatePad Mini 连续 21 次 `PASS`、168/168 GPU producer 帧成功；每轮八帧 CPU 像素校验通过、六次 image/memory/view 导入和两次缓存命中。第一次安装后父进程 fd 为 50，第二轮起 20 次均为 42；RSS 在预热后约 102–103 MiB 范围波动，未呈持续线性增长。无探针子进程残留。同包 D0、D1、D2 WSI 入口复测均 `PASS`、fd 42。[父进程逐次日志](evidence/direct-d2-import-parents.log)、[子进程逐帧日志](evidence/direct-d2-import-children.log)、[末轮 JSON](evidence/direct-d2-import-final.json)。
- 仅修正异常设备枚举时错误码后重构建的最终 HAP SHA-256 为 `39db6d118117293084c75eb6699022c7a3bcb9c5dfa9ba8579f792715e91d404`，已覆盖安装；导入探针再跑两轮均 `PASS`，第二轮 fd 42（[最终 JSON](evidence/direct-d2-import-39db.json)）。
- 这证明实际跨进程 BufferQueue 的 NativeBuffer 可在 App 进程创建并缓存 Vulkan sampled image view；该版本**尚未通过 GPU 命令从该 image 取样或合成**。像素判断仍靠 CPU map，acquire fence 用 CPU poll，子进程每帧等待 submit fence。GPU 取样见下节。

## D2 NativeBuffer GPU 采样独立探针（2026-09-26）

- 新增 `winehua.mode=direct-gpu-sample-probe`，保留原 import 模式作回归基线。App 对每帧导入的 sampled image 提交 Vulkan compute shader：用 `VK_QUEUE_FAMILY_FOREIGN_EXT` → App 队列的 ownership/layout barrier 取得图像，`texelFetch` 读取九个位置并写入 host-visible storage buffer，再转换回 `PRESENT_SRC_KHR` 并交还 foreign queue family。等待提交 fence 后，CPU 只读取这 36 字节的 GPU 输出并核对逐帧 RGBA 图案；原 CPU map 检查保留作交叉校验。着色器源码为 `direct/direct_sample.comp`，嵌入头可在构建容器里运行 `python3 scripts/generate_direct_sample_shader.py` 重新生成。
- 签名 HAP SHA-256：`e84d2a3bd527f605223bf9ee198d026fc2db83d99760b6cb11ee2fdf96ec60d2`。MatePad Mini 首轮与后续连续 20 轮全部 `PASS`；连续运行的 20 个子进程 PID 各不相同，共 160/160 帧 GPU 取样与 CPU 交叉校验通过。每轮 6 次导入、2 次缓存命中、8 次 GPU 取样；父进程 fd 始终为 42，RSS 在 114560–115536 KiB 间。结果见 [20 轮 JSON](evidence/direct-d2-sample-runs.ndjson)。同包 D0 Create、D1、D2 WSI 和原 D2 import 均 `PASS`，fd 为 42（[回归 JSON](evidence/direct-d2-sample-regression.ndjson)）。
- 本探针证明跨进程 BufferQueue 图像内容可被 App 侧 Vulkan shader 实际读取，也验证了本设备上这组 foreign queue family / layout barrier 能正常运行。该模式仍在读取前用 CPU `poll` 等 acquire fence，提交后用 CPU 等 Vulkan fence，再用 `ReleaseNativeWindowBuffer(..., -1)` 释放；也仍有 36 字节诊断读回。GPU semaphore fence 交接见下节。

## D2 SYNC_FD acquire/release 独立探针（2026-09-26）

- 新增 `winehua.mode=direct-gpu-fence-probe`，沿用 GPU 采样与 NativeBuffer 缓存，但 Acquire 返回的 fence fd 通过 `vkImportSemaphoreFdKHR` 临时导入 Vulkan semaphore，提交时让 GPU 等待该 semaphore；提交后通过 `vkGetSemaphoreFdKHR` 导出 release `SYNC_FD`，直接交给 `OH_NativeImage_ReleaseNativeWindowBuffer`。成功导入的 acquire fd 由 Vulkan 接管，成功 Release 的 fd 由 BufferQueue 接管。App 在 Release **之后**才等 Vulkan submit fence 并读取 36 字节诊断结果；此模式不再 CPU `poll` acquire fd，也不再 CPU map NativeBuffer。NDK `libvulkan` 不直接导出这两个扩展函数，因此用 `vkGetDeviceProcAddr` 解析。
- 最终签名 HAP SHA-256：`6091311a05e529ffb0736cd6a65b4a33f236cb6e4d493e0e8b17ace6403df951`。MatePad Mini 在此包上连续 30/30 轮 `PASS`，240/240 帧 GPU 像素校验通过，30 个子进程 PID 各不相同。每帧都实际导入 acquire fd、导出 release semaphore 并向 BufferQueue 提交一个 release fd，合计 **240/240 次导入、240/240 次导出、240/240 个 release fd**；每轮 6 次 NativeBuffer 导入、2 次缓存命中。父进程 fd 始终为 42，RSS 为 115548–116848 KiB。[连续运行结果](evidence/direct-d2-fence-final-runs.ndjson)。同包 D0 Create、D1、D2 WSI、D2 import、D2 sample 回归均 `PASS`、fd 42（[回归结果](evidence/direct-d2-fence-final-regression.ndjson)）。
- 这验证了本设备上的 acquire/release `SYNC_FD` 实际交接和 GPU 图像读取。探针仍逐帧等提交 fence 才读取小型诊断缓冲区，也由子进程逐帧等待生产者提交；尚未证明多帧同时在途、独立 XComponent 合成或无逐帧 CPU 等待的 60 FPS 性能。下一步应将导入图像采样到独立 XComponent 的 Vulkan render target，并让帧槽持有命令缓冲区、输出 buffer 与同步对象直到 GPU 完成，再测连续 present 和 resize。

## D2 独立 XComponent Vulkan 输出探针（2026-09-27）

- `winehua.mode=direct-gpu-output-probe` 在诊断页创建独立 XComponent，App 从其 surfaceId 建立 OHOS Vulkan surface 和三图像 swapchain，连续 GPU clear 并 present 八帧。每帧颜色可辨；第八帧的中心像素应为 `(224,90,165)`。最后一次 `vkQueuePresentKHR` 后保留 swapchain 100 ms，给系统合成器显示 FIFO 末帧的时间；提交 fence 与 `vkDeviceWaitIdle` 本身都不是上屏完成信号。此等待仅供诊断，不是目标帧循环策略。
- 当前签名 HAP SHA-256 为 `605b76374fd8208624351b926ae284fc30b50ef1224c07c4957031f61763aa02`。MatePad Mini 上输出为 `760×570`、`VK_FORMAT_R8G8B8A8_UNORM (37)`、三图像 swapchain。最终 [屏幕截图](evidence/direct-d2-output-final-screen.png) 的中心像素实测为 `(224,90,165)`。通过重新加载诊断页连续创建 70 个不同 surfaceId（sequence 3–72），70/70 次均报告 `PASS`、每次 present 八帧（[逐次结果](evidence/direct-d2-output-recreate-runs.ndjson)）。
- 70 次结果中的父进程 fd 为 44–60，RSS 为 113196–146852 KiB。fd 在 sequence 29、43、59 分别回落至 44、45、45，RSS 也曾回落；末次为 fd 58、RSS 143104 KiB。这表明 ArkUI 页面与 Vulkan 资源有延迟回收，但有限样本尚不能证明长期 RSS 稳定或完全无泄漏。最终包又复跑 D0、D1、D2 WSI、import、sample、fence 六项，全部 `PASS`，其中 fence 模式仍完成每帧 acquire/release `SYNC_FD` 交接（[回归结果](evidence/direct-d2-output-final-regression.ndjson)）。
- 此独立输出探针只证明 XComponent 可由 App 的系统 Vulkan swapchain 正确显示 GPU 输出。NativeBuffer import/sample/fence 模式与此模式分别创建 `VkDevice`，因此这些独立结果本身不能证明输入图像已显示。下一节把输入导入、GPU fence 和输出 swapchain 放到同一 `VkDevice`，用 shader 直接采样呈现；CPU readback/upload 不能代替该验证。

## D2 同设备 GPU 合成探针（2026-09-27）

- 新增 `winehua.mode=direct-gpu-composite-probe`。它沿用跨进程 Create NCP Vulkan producer、ConsumerSurface、按 NativeBuffer sequence 缓存导入图像和 `SYNC_FD` acquire/release。App 侧在**同一 Vulkan instance/device/queue** 上创建独立 XComponent swapchain：一个提交中先用 compute shader 取九点作诊断，再用 fragment shader 直接采样导入的 image view，绘制全屏三角形到 swapchain，随后 present。画面本体没有 CPU map、readback、memcpy 或 upload；只有 GPU fence 完成后的 36 字节九点诊断读回。输入每轮从 64×64 resize 到 96×48，输出当前为固定尺寸，按全屏拉伸显示。
- 签名 HAP SHA-256：`cb6a29a1be6a9ee5de9594481e0baaa0a5b848f3fe7165fdc284b3dfe513dd53`。MatePad Mini 上连续重载诊断页 51 次，51/51 `PASS`、51 个不同 NCP PID 和 XComponent surfaceId，共 408 帧 GPU 取样、408 次 acquire fence 导入、408 次 release fence 导出、408 次 Vulkan present（[逐轮结果](evidence/direct-d2-composite-runs.ndjson)）。每轮六次 NativeBuffer 导入、两次缓存命中。最后一帧的[设备截图](evidence/direct-d2-composite-final-screen.jpeg)为预期的紫红色，JPEG 中心像素 `(225,90,166)`；源图案目标值为 `(224,90,165)`。
- 51 轮父进程 fd 为 44–59，RSS 为 114376–161228 KiB，末次分别为 52 和 148488 KiB；fd 在第 28 轮回落至 44、第 44 轮回落至 45，RSS 也曾回落。这只说明观察到延迟回收，尚未证明长期内存稳定。同一 HAP 又复跑 D0、D1、WSI、import、sample、fence、独立输出和合成八个入口，全部 `PASS`（[回归结果](evidence/direct-d2-composite-regression.ndjson)）。
- 该探针已证明本设备上输入 NativeBuffer 可经 App GPU shader 显示到独立 XComponent。它仍逐帧等待提交 fence 以读取诊断缓冲区，NCP producer 也逐帧等待；此模式没有改变输出 surface 尺寸，resize 见下节。多个帧槽同时在途、60 FPS 稳定性和产品 Wayland/XComponent 接入尚未验证。生产合成器需要把 frame slot、image generation 与 surface 生命周期显式管理后再替换旧呈现链。

## D2 XComponent 输出 resize 探针（2026-09-27）

- 新增 `winehua.mode=direct-gpu-composite-resize-probe`，沿用同设备 GPU 合成。前四帧以 `320×240 vp` 的 XComponent 呈现；worker 随后向诊断页请求改为 `400×160 vp`，只接受**同一 surfaceId** 的 `onSurfaceChanged` 回调。App 等该尺寸回调后在 resize 边界等待设备空闲、销毁旧输出 swapchain 的 framebuffer/view/sync 资源并重建；输入 BufferQueue 同时按既有流程从 64×64 切换为 96×48。后四帧使用新 swapchain，仍以 shader 直接采样 NativeBuffer，继续交接 `SYNC_FD`。`vkDeviceWaitIdle` 只在此诊断 resize 边界使用，当前帧循环仍逐帧等 fence。
- 签名 HAP SHA-256：`b67d8edf00e601f2faca4e322f96c97c3b9e4723bc1c3abdb4dc1db930313ce1`。MatePad Mini 连续重载页面 30 次，30/30 `PASS`、30 个不同 NCP PID 与 XComponent surfaceId；每轮输出从物理 `760×570` 变为 `950×380`，恰好重建一次 swapchain，八帧 GPU 采样、fence 导入/导出和 present 均通过。合计 240 帧、30 次重建（[逐轮结果](evidence/direct-d2-resize-runs.ndjson)）。最后一轮的[屏幕截图](evidence/direct-d2-resize-final-screen.jpeg)显示新宽高比的末帧；JPEG 中心像素 `(225,90,166)`，与源图案目标 `(224,90,165)` 仅有压缩误差。
- 30 轮父进程 fd 范围 44–58，RSS 范围 114828–158916 KiB；fd 在第 12、13、28 轮回落至 44，RSS 也回落。此样本仍不足以证明长期稳定。最终包复跑 D0、D1、WSI、import、sample、fence、独立输出、合成和合成 resize 九个入口，全部 `PASS`（[回归结果](evidence/direct-d2-resize-regression.ndjson)）。后续要让多个 frame slot 同时在途，去掉每帧 CPU fence 等待，再测稳定 60 FPS 与更长时间的资源占用。

## D2 连续提交吞吐基线（2026-09-27）

- 新增 `winehua.mode=direct-gpu-composite-throughput-probe`，在现有 NCP Vulkan WSI → 跨进程 NativeBuffer → App Vulkan shader → 独立 XComponent 的链路上连续提交 600 帧。每帧仍先完成 producer IPC，再 acquire/submit/release/present，最后等待 App 提交 fence 并读取 36 字节诊断结果；没有 CPU 图像拷贝或逐帧导入，但生产端和消费端都仍**逐帧串行等待**。JSON 的 `fps` 是 `framesPassed / elapsedMs` 算出的**提交吞吐率**，不是 RenderService 已显示帧率；`throughputAtLeast60` 只判断这个提交率。
- 已安装的签名 HAP SHA-256 为 `0be8175533bdac3876b0d71ead9f32abd55908834caac15609cd729a96b2ccb5`。MatePad Mini `5KPBB25818203996` 上六次连续重载均 `PASS`，合计 3600/3600 帧、3600 次 shader 采样、acquire fence 导入、release semaphore 导出与 Vulkan present。每轮导入 3 个 NativeBuffer，复用 597 次；六轮的提交率为 88.83–91.05 次/秒，帧时 p95 为 13.23–13.76 ms，父进程 fd 为 44–47，RSS 为 124276–130092 KiB（[逐轮 JSON](evidence/direct-d2-throughput-runs.ndjson)）。同包 D0、D1、WSI、import、sample、fence、独立输出、合成和 resize 九个入口全部 `PASS`（[回归 JSON](evidence/direct-d2-throughput-regression.ndjson)）。
- `releaseExportCount` 每轮为 600，实际非负的 `releaseFdCount` 为 555–567；导出成功不等于每次都返回非负 fd，`-1` 被原样交给 BufferQueue。后续应结合设备实现核对已完成 `SYNC_FD` 返回 `-1` 的语义。当时以应用窗口名查询 RenderService `fps` 没有得到可归属的采样；下节改用 XComponent surface 名取得记录。此包只作为串行基线。

## D2 双帧槽与 RenderService 帧记录（2026-09-27）

- 新增 `direct-gpu-producer-pipeline-probe` 与 `direct-gpu-dual-slot-probe` 两个独立入口，保留上一节的串行入口做同包 A/B。NCP 生产者使用三套命令缓冲区、acquire semaphore 和提交 fence，只在槽复用时等待；host 先发送两帧并确认 swapchain 图像索引不同，此后消费一帧再补一帧。双槽入口还给 App 每槽单独配置 descriptor、诊断 readback buffer、命令缓冲区、提交 fence、acquire/release semaphore 和输出 acquire semaphore。App 提交两帧后才等待并校验最早的槽；`bufferedFramesPeak=2` 和 `consumerSlotsPeak=2` 记录这一实际路径。输入 NativeBuffer 仍按 sequence 缓存，显示图像依然由 Vulkan shader 直接采样，没有 CPU 图像拷贝或逐帧 import。
- 签名 HAP SHA-256：`21f4173424d1a5ba30922e9fa136ade8d9c2112784477251d1561f0d4d92dd0f`。MatePad Mini `5KPBB25818203996` 连续重载双槽入口 **38/38 `PASS`**，38 个不同 NCP PID，合计 22800/22800 帧、22800 次 GPU 采样、acquire fence 导入、release semaphore 导出和 Vulkan present。每轮 3 次 NativeBuffer 导入、597 次复用；提交率 88.49–89.08 次/秒，端到端帧时 p95 为 40.16–43.90 ms（[38 轮 JSON](evidence/direct-d2-dual-slot-runs.ndjson)）。同包 D0、D1、WSI、import、sample、fence、独立输出、合成、resize、串行吞吐和生产端预提交共 11 个入口均 `PASS`（[回归 JSON](evidence/direct-d2-dual-slot-regression.ndjson)）；其中串行与生产端预提交分别为 88.94 和 88.77 次提交/秒。双槽消除了每帧立即 CPU 等 App GPU fence 的依赖，但在当前 90 Hz FIFO swapchain 上没有提高提交吞吐，队列等待使端到端延迟高于串行基线。
- 38 轮父进程 fd 范围 44–58，RSS 为 132068–163144 KiB；页面反复重载时两者先上升，在第 16、31 轮又分别回到 fd 44、RSS 约 129–130 MiB，未见此样本中的单调泄漏。每轮 `releaseExportCount=600`，非负 `releaseFdCount=529–565`，仍需单独核对导出 `-1` 的设备语义。末轮以实际 surface 名 `direct_output_xcSurface` 调用 RenderService `fps`，得到连续 **384 条记录 / 4.258 秒**，首列记录间隔对应约 **89.95 次/秒**、p95 **13.11 ms**、最大 **17.91 ms**（[原始记录](evidence/direct-d2-dual-slot-rs-fps.txt)）；[末帧截图](evidence/direct-d2-dual-slot-final-screen.png)中央像素为 `(224,90,165)`，与第 599 帧目标一致。这证明独立 XComponent 持续向 RenderService 交付帧；该接口的时间戳不能直接当作物理屏扫描完成时间。
- 这仍是**诊断 XComponent**，没有把 Wine/Wayland 窗口或 DXVK 接到 Direct；也没有验证游戏负载或持续数分钟的物理显示帧率。双槽与 resize 同时发生的边界见下节。设备截图时电量约 6%；当前已接电但充电较慢，长时测试须注意电量余量。

## D2 双帧槽 resize/generation 边界（2026-09-27）

- 新增 `winehua.mode=direct-gpu-dual-slot-resize-probe`，把两帧预提交与双槽回收逻辑收为 `RunPipelinedFrames`，供 600 帧吞吐模式和 resize 模式共用。前四帧输入 64×64、输出 760×570；**完成两个 App GPU 槽的 fence 等待和像素校验后**，才要求诊断页把同一 XComponent surface 从 320×240 vp 改为 400×160 vp。之后在边界等待设备空闲、重建输出 swapchain、把 ConsumerSurface 默认尺寸改为 96×48、清除上一输入 generation 的导入缓存，再以双槽提交后四帧。每组都先排入两帧并核对不同 swapchain 图像，避免只测到“配置了两个槽但仍逐帧串行”。
- 签名 HAP SHA-256：`5643c00aae49eff326b385a328c847628efc5bad305cba8e6be0f40dfc058308`。MatePad Mini 连续重载 **20/20 `PASS`**、20 个不同 NCP PID，共 160/160 帧；每轮 `bufferedFramesPeak=2`、`consumerSlotsPeak=2`，6 次输入 NativeBuffer 导入、2 次缓存复用、8 次 GPU 采样/fence 导入/fence 导出/present，输出恰好重建一次并变为 950×380（[逐轮 JSON](evidence/direct-d2-dual-slot-resize-runs.ndjson)）。父进程 fd 为 44–54、RSS 为 129052–147312 KiB，第 11 轮 fd 回落到 44。最终包复测既有 12 个入口全部 `PASS`（[回归 JSON](evidence/direct-d2-dual-slot-resize-regression.ndjson)）。[末帧截图](evidence/direct-d2-dual-slot-resize-final-screen.png)中央像素为精确的 `(224,90,165)`，且画面宽高比已变。
- 该结果验证了独立探针在双槽回收后切换输入 generation 和输出 swapchain 的顺序。真实 Wine/Wayland 窗口的 surface 身份、突发 resize、设备丢失以及物理屏长期显示节奏仍需后续阶段验证。

## D2.5 Wine NCP 参数与 fd 控制通道（2026-09-27）

- `libwine_child.so` 增加 Create 型 NCP 的 `NativeChildProcess_OnConnect` / `NativeChildProcess_MainProc` 入口，保留原有 `Main(NativeChildProcess_Args)` 入口和 broker 的 `StartNativeChildProcess` 默认路径。新入口只接受一个带版本号的 bootstrap IPC 请求：完整 `entryParams`、0–16 个具名 fd。子进程通过 `OH_IPCParcel_ReadFileDescriptor` 取得自己的 fd，在回包中报告真实 PID，再把这些参数交给现有 `Main`。父进程的 fd 在 `WriteFileDescriptor` / `SendRequest` 后仍归父进程管理，不能沿用旧 broker 注释中的“所有权已转移”假设。
- 独立 `winehua.mode=direct-wine-ipc-probe` 直接创建**真实 `libwine_child.so`**，向其传入诊断参数及 `probe_input`、`probe_output` 两个命名 fd；子进程的 `MainProc` 读取预置 token 并通过第二个 fd 回报 PID、状态和 fd 数量。此探针不启动 Wine 游戏。签名 HAP SHA-256 为 `2bbac02ce24a65cb97bd6e73ddc56dbb227e164a6f78f4458be7e37464462919`。MatePad Mini 上先跑单次通过，随后每轮 `aa force-stop` 后重新启动，**10/10 次通过且 10 个 PID 各异**；每轮 `launchCode=callbackCode=ipcCode=0`、`fdCount=2`、`pid=replyPid`、`parentFdsOpen=1`（[逐轮结果](evidence/direct-d25-wine-ipc-runs.ndjson)）。只对已在前台的 Ability 连续调用 `aa start` 会读到同一份旧结果，不算重复验证。
- 后续给 Create 型 proxy 注册 `OHIPCDeathRecipient`，要求子进程写完结果并退出后父进程收到死亡通知。新签名 HAP SHA-256 为 `22b4083135e3218d7f19e226d869ca06c4f2e094d788fdd29f640cc3d0de0b56`；MatePad Mini 单次及随后 **10/10 次独立启动**均 `PASS`，10 个不同 PID、每轮 `deathReceived=1`（[逐轮结果](evidence/direct-d25-death-runs.ndjson)）。这给 Create 型 NCP 提供了可用的死亡信号，但死亡回调不携带退出码或 signal，broker 的进程登记、异常分类与 proxy 清理仍未接入。
- 用设备 SDK 的 `llvm-readelf --dyn-syms` 核对打包前 ARM64 库同时导出 `Main`、`NativeChildProcess_OnConnect` 和 `NativeChildProcess_MainProc`；两版 `assembleHap` 均成功。此阶段证明参数、双 fd、PID 和 proxy 死亡通知的 Create 型 bootstrap；broker 接入与后续验证见下节。平台文档指出 `RegisterNativeChildProcessExitCallback` 只覆盖 Start 型 NCP；Direct 仍须在 D3 按实际 Wine surface 发送 producer window，不能把启动探针等同于产品图形接入。

## D2.5 broker 可选 Create 路由（2026-09-27）

- `proc/broker.cpp` 按每次 SPAWN 的序列化环境 token `__env=WINEHUA_DIRECT_NCP=1` 选择 Create 型 IPC；缺省和 `=0` 继续使用 Start 型 NCP。Create 变体在 `wine_child_ipc_launcher.cpp` 中保留 proxy，收到死亡通知后更新现有进程登记，来源记为 `ipc-death`。死亡通知没有退出码或 signal，因此此来源的 `reason=-1`，不能据此判断进程是正常退出还是崩溃。手机 fork 模式不启用此路由。
- broker 对接收的 SCM_RIGHTS fd 和音频 bootstrap fd 在 NCP 调用前记录设备/inode，调用后仅在原 fd 身份未变时关闭父进程副本；之前把 fd 所有权误认为已转移的注释已修正。Create 路由先验证参数长度、fd 数量、名称和有效性，拒绝明显无效的请求而不创建 NCP。子进程在 PID 回包写入 parcel 后才唤醒 Wine 主函数，避免短命子进程先退出。bootstrap/回包失败会释放父进程 proxy、parcel 和本地 fd；已创建但未收到有效 bootstrap 的子进程由 15 秒等待超时退出。若请求已被子进程接收但回包在传输中失败，仍存在无法从父进程确认是否已开始执行的边界，尚需两阶段确认或可取消启动协议。
- broker 双命名 fd 短探针在此前候选包上 **10/10 次独立启动 `PASS`**，PID 各不相同，均收到 `ipc-death` 并在注册表完成退出登记（[逐轮结果](evidence/direct-d25-broker-ipc-runs.ndjson)）。最终修正包 SHA-256 为 `943d1b3782d305667c354c688056c471502d0b0e98d197bc465f5c22eacfeb59`，已安装到 MatePad Mini；同包 broker 探针再次 `PASS`，随后真实 x64 Wine DNS 进程 PID 38280 经 `create-ipc` 启动、测试通过并由 proxy 死亡通知收口（[设备日志](evidence/direct-d25-broker-ipc-final.log)）。
- 旧 Start 路径 `core` 首轮为 **3/4**：x86 OpenGL、x64/x86 DNS 通过，x64 OpenGL 在结果写入前以 `exit=1` 退出；仅重跑 x64 OpenGL 随即通过，且固定帧校验通过。这个单次失败不能归因于 fd 回收，也不能称旧路径整套稳定通过。用 `WINEHUA_DIRECT_NCP=1` 对真实 Wine 进程跑 `core` 为 **4/4**，x64/x86 OpenGL 固定帧与 DNS 均通过；最终包的 x64 DNS 单项复验也通过（[回归摘要](evidence/direct-d25-core-regression.json)）。自动化脚本的日志超时处理原先把字符串当字节串解码，会在设备 `hilog` 超时后异常退出；已兼容两种返回类型。一次从诊断页直接 `aa start` smoke 只进入现有 Ability 的 `onNewWant`，没有构成有效回归；正式测试前已 `force-stop` 并重新启动。
- 此结果证明可选 Create 路由能启动一个真实 Wine 测试进程，并保留旧 Start 路径。会话的 wineserver、其他服务进程未逐个验证为 Create，Steam/CEF 多进程与游戏、异常退出分类、长期 proxy/fd 稳定性仍是 D2.5 完整切换前的门槛；当前不改默认路由，也不接产品 Direct WSI。

## D2.5 会话级 Create 路由与最终包复测（2026-09-27）

- Wine 会话新增显式 `winehua.direct_ncp_session=1` Want：broker 对该会话的 Wine 子进程默认选择 Create 型 NCP；没有该键仍用 Start。单次请求的 `__env=WINEHUA_DIRECT_NCP=0/1` 覆盖会话默认。此 Want 在启动 Wine 会话时生效，测试前须冷启动应用；已有会话的 `onNewWant` 不会切换正在运行的 broker。手机 fork 模式将会话 Create 默认值强制归零，避免整个手机会话误入不支持的路由。`automation/smoke.py` 可用 `--direct-ncp-session` 做冷启动对照，`tools/steam-rwx/launch_want.py` 也可传同名选项。smoke 推送验证按 manifest 清单核对文件；host 判定和 `check` 重判会按 `inline` / `tests` 选测范围收敛，避免把未运行的原套件用例误报为缺失。
- 在手机保护修改之前的候选包，单进程 opt-in 的 `platform-process`、`steam-arch-compare` 均 **2/2 PASS**，默认 Start 对照亦各 **2/2 PASS**；会话 Create 的 `platform-process`、`steam-arch-compare` 为 **2/2 PASS**，`core` 为 **4/4 PASS**。完整 `steam-contract` 无论 Create 还是 Start 均 **1/2**：i386 用例分别在写结果前以 `ipc-death` 退出、在 `terminate-code` 阶段以 `exit=11` 退出，因此不能把失败归咎于 Create。`steam-contract-isolate` 两路径各 **3/3 PASS**。这些记录留在本机 `F:\WineHua\.temp\smoke-d26-logs` 对应 run ID 归档中。
- 最终签名 HAP SHA-256 `e2de0bd6e4b5e3644e9e3b6a5af2f2bee9dba3c731bac1a9212e0784dbac9621` 已装至 MatePad Mini。最终包的会话 Create `platform-process` **2/2**，wineserver PID 57818 由 `create-ipc` 启动；默认 Start 同套 **2/2**，wineserver PID 58700 由 `start` 启动；会话 Create 加单进程 `WINEHUA_DIRECT_NCP=0` 的 x64 DNS **1/1**，探针 PID 59845 由 `start` 启动并以 0 退出。最终包的 i386 契约在跳过 `TerminateThread` 后，Start 与 Create 各 **1/1 PASS**；这把未解决的完整契约故障收窄到该路径附近，但未证明 Wine 线程终止实现的具体根因。[最终包结构化证据](evidence/direct-d25-session-route-e2de.json)、[设备路由日志](evidence/direct-d25-session-route-e2de.log)。
- 本轮尝试真实 Steam/CEF 回归时发现当前 prefix 的 `Program Files (x86)` 下没有 Steam，`steam.exe` 启动仅报 `failed to open`。本机 Valve Corp. 有效签名的 `SteamSetup.exe`（SHA-256 `7d3654531c32d941b8cae81c4137fc542172bfa9635f169cb392f245a0a12bcb`）复制到 `C:\smoke` 后以 `/S` 运行，FEX 与显式 box64 两次均以 `exit=11` 结束，未写出 `steam.exe`；安装会话已停止。因此真实 Steam/CEF 多进程、长期 fd/proxy 稳定性与完整 `TerminateThread` 契约仍是 D2.5 未通过的门槛，不据此打开 Direct 默认路由。

## D3 第一阶段：Wine Vulkan 离屏 Direct（2026-09-27）

- D3 仍为**显式单进程 opt-in**：测试进程带 `WINEHUA_DIRECT_NCP=1` 和 `WINEHUA_VULKAN_BACKEND=direct`。`wine_child.cpp` 清除 guest Venus ICD 覆盖；Wine `win32u/vulkan.c` 选择 `/system/lib64/libvulkan.so`；`winewayland.drv/vulkan.c` 在 Direct 模式暂不公布 Win32 surface，也拒绝把旧私有 Venus surface tag 交给系统 loader。默认会话和默认 Wine Vulkan 仍走原有 Venus/VirGL 路径。Direct 模式的 `get_opengl_gpus` 跳过隐式 VirGL/EGL 枚举，避免离屏 Vulkan 进程在 `vkCreateInstance` 前因独立 VirGL 服务故障于 `eglInitialize` 中止；该模式不承诺 OpenGL。
- 全新 prefix 首次启动暴露了既有启动门禁问题：wineboot 已写 `wineboot-init-ok`，但持久 wineserver 尚未把 `system.reg`、`user.reg` 落盘，App 因此在注册表条件上等待超时。ARM64 Wine PE 的 `wineboot` 现在于初始化完成、发出 boot event 前对 HKLM/HKCU 调用 `RegFlushKey`。实机复测出现 `system.reg`、`user.reg` 和 `userdef.reg`，wineboot 用 66 秒完成，App 进入 `ready`。会话级 Create 路由在**全新** prefix 上尚未用最终包单独复测；Create 的死亡通知仍不携带退出码。
- 最终候选 signed HAP SHA-256：`e5de3ab0b4f769a7a3b0f464e708b84be2651bc344fe4246a35285ce98570763`；`scripts/w1-verify-candidate.sh` 的 binary mapping 与 runtime closure 均 `PASS`。MatePad Mini `5KPBB25818203996` 上，同包、同为单进程 Create NCP 的 Wine Vulkan 离屏 A/B 如下。四项均 `PASS`，所有图像/采样检查为真，`fallbackDetected=false`，`presentFrames=0`：

  | PE | Direct 系统 Vulkan | 原 Venus 路径 |
  | --- | --- | --- |
  | x64 | Maleoon 910，loader API 1.3.275，[结果](evidence/direct-d3-offscreen-direct-x64.json) | Virtio-GPU Venus (Maleoon 910)，loader API 1.3.290，[结果](evidence/direct-d3-offscreen-venus-x64.json) |
  | x86 | Maleoon 910，loader API 1.3.275，[结果](evidence/direct-d3-offscreen-direct-x86.json) | Virtio-GPU Venus (Maleoon 910)，loader API 1.3.290，[结果](evidence/direct-d3-offscreen-venus-x86.json) |

- 单进程 Create NCP 的非 Vulkan x64 DNS 对照也 `PASS`（本机归档 `F:\WineHua\.temp\direct-d3-runs\core-d3-create-dns-20260927`）。修正 EGL 枚举前的 Direct 测试在 `win32u` 的 OpenGL GPU 枚举中进入 `eglInitialize → libgallium` 并 `SIGABRT`；修正后通过。
- 最终包又测了**缺省 Start + Venus**，x86 离屏 `PASS`，x64 在进入 `__wine_main` 后 90 秒没有写出结果，[套件摘要](evidence/direct-d3-venus-start-final-summary.json)。同包 Create + Venus 的 x64/x86 则均通过；不能把 Start x64 超时归因于 Direct loader，也不能称缺省生产路径的 Vulkan 回归已全绿。Start 与 Create 的 x64 差异仍须单独定位；D3 Direct 继续只在 Create NCP 显式启用。
- **D3 WSI 未完成**：离屏 smoke 没有 Win32 surface、swapchain、acquire、present 或 resize，也没有把产品窗口接到 D2 的 BufferQueue 和 Vulkan compositor。下一步需要把 App 的 producer window 通过 D2.5 的 IPC 交给目标 Wine NCP，建立 `(clientPid, toplevelId, generation)` surface 身份与生命周期，再实现 Direct 的 Win32 surface/swapchain/present 和 resize/device-lost 回退。D4 DXVK Direct 尚未开始。

### D3 第二阶段：真实 Wine NCP 的 surface 控制面（2026-09-27）

- App 已把独立 `OH_ConsumerSurface` 的 producer window 通过 IPC parcel 交给真实 Create 型 `libwine_child.so`。身份为 `(clientPid, toplevelId, wlSurfaceId, generation)`；attach/query/detach、旧代拒绝、resize 换代和 NCP 死亡回调已有独立探针。首版协议冷启动 **10/10 PASS**（[逐轮结果](evidence/direct-d3-wine-surface-ipc-runs.ndjson)）；增加 Direct 进程路由断言后，SHA-256 `87a6f1d2…` 候选又冷启动 **10/10 PASS**、10 个 PID 各异、每轮 `directRoute=1`（[逐轮结果](evidence/direct-d3-wine-surface-ipc-final-runs.ndjson)）。
- Wayland toplevel 创建、resize、销毁已接入后台 surface 控制器；它只为显式 Direct Create NCP 建立队列。Wine `winewayland.drv` 的显式 Direct 分支已尝试从同进程 `libwine_child.so` 取得 producer，并调用系统 Vulkan 的 `vkCreateSurfaceOHOS`。`win32u` 将 Win32 surface 扩展映射到实例扩展 `VK_OHOS_surface`；曾误在设备扩展列表中查找该实例扩展，现已改为在实例扩展枚举时记录。
- Wine 源码已用 `scripts/w1-m4-build-merged.sh` 真正编译成功（ARM64 `win32u.so`、`winewayland.so`），随后 `scripts/w1-m2-assemble.sh` 和 `scripts/w1-m3-build-hap.sh` 成功，候选映射与 runtime closure 均 `PASS`。未消费帧的候选 SHA-256 `87a6f1d24a9d16ffbc7d1d9ad7ed40ec7170e8af2b62659f528ef3ba71fae78b` 在 MatePad Mini 的温启动 Win32 Vulkan 用例中成功创建 OHOS surface，提交 3 帧后第 4 次 acquire 返回 `VK_ERROR_SURFACE_LOST_KHR`（[结果](evidence/direct-d3-wsi-preconsumer-x64.json)）；App 日志同时确认真实 toplevel 的 producer 已 attach。
- 当前 App 控制器已增加 NativeBuffer GPU 导入、双帧槽和 acquire/release `SYNC_FD` 传递，按帧消费真实 Wine 的 ConsumerSurface；此路径关闭诊断像素读回，**仍未把帧绘制到 XComponent**。新 signed HAP SHA-256 `9cc8019a165af525cf9aafb59157ccef86c76aa3a614e0c7f61d09e92d4cf4b5` 已构建、校验并装机。同包、同前缀的 Direct 离屏 x64 [对照](evidence/direct-d3-wsi-consumer-offscreen-x64.json) 为 `PASS`；温启动 Win32 WSI [30 帧结果](evidence/direct-d3-wsi-consumer-x64.json) 和 [150 帧结果](evidence/direct-d3-wsi-consumer-150-x64.json) 均 `PASS`。150 帧耗时 2147 ms（约 70 帧/秒的 producer 完成速率），`cpuReadBytes=cpuUploadBytes=0`、`perFrameDeviceWaitIdle=0`；App 消费者日志累计报告第 60/120 帧，真实帧尺寸 632×446。测试程序结果里的 `private BrokerPresent` 文案是旧诊断文本；设备名 Maleoon 910、Direct NCP 日志和系统 loader 版本共同确定这次走的是 Direct OHOS WSI。
- **边界仍在**：当前 30 帧证明 producer→BufferQueue→GPU consumer 与 fence 回收，不证明画面上屏、60 FPS、多窗口混合合成或 resize。旧 producer 在 resize 后的窗口/`VkSurfaceKHR` 生命周期、跨进程窗口呈现和 device-lost 回退均未实测。缺省 Venus 路由未切换。新包冷启动第一次 present 曾在 PE 写 `STARTED` 前超时，随后的温启动离屏与 present 均通过；需区分 prefix/Wine 初始化和 WSI 后再判稳定性。下一 Gate 是把真实帧接到产品 XComponent Vulkan compositor，并验证 resize、异常退出和持续资源占用。
- 设备解锁后，以同一已安装 HAP 对 D3 surface IPC 再做 **10 次强停后冷启动**：10/10 `PASS`，10 个不同的 NCP PID；每轮 `directRoute=1`，首次 attach/query、旧代拒绝、resize 换代、detach 与死亡通知均通过（[逐轮 JSON](evidence/direct-d3-wine-surface-ipc-unlock-runs.ndjson)）。这验证的是控制面冷启动，不等同于 Wine WSI 的冷启动 present 或产品上屏。
- 第二阶段的产品显示接入点核对：非桌面窗口由 `WineWindow.ets` 为每个 toplevel 创建独立 XComponent，当时 `PluginManager::CreateRenderer` 无条件给它创建 EGL renderer；桌面模式仅有 root toplevel 的 EGL XComponent，真实 Direct Wine 窗口是其他 toplevel。`DirectBufferImportProbe` 已有同设备 GPU sample→Vulkan swapchain present 能力，可复用于独占的 Direct XComponent；第三阶段按此接入独立窗口，桌面叠层仍未完成。

### D3 第三阶段：独立窗口产品 XComponent 上屏（2026-09-27）

- 显式 Direct Create NCP 的非桌面 toplevel 现在由 `createRenderer` 绑定到 App 的 Vulkan consumer；同一 XComponent 不再创建 EGL renderer。`resizeRenderer`/`destroyRenderer` 和窗口关闭走同一个输出所有者，工作线程在解绑前等待旧 GPU 提交和 swapchain 释放。桌面模式仍保持原 EGL root，未创建 Direct 叠层；缺省 Venus/Start 路由不变。`automation/smoke.py --desktop-mode fusion|virtual` 增加仅本次冷启动生效的模式覆盖和归档字段。
- 首次 fusion x64 实测在第 10 帧得到 `VK_ERROR_SURFACE_LOST_KHR`（[失败 JSON](evidence/direct-d3-fusion-pre-inplace-x64.json)）。设备日志显示原因为 Wine 已持有 generation 1 的 producer，随后 xdg 尺寸回报让 App 建 generation 2 并销毁旧 ConsumerSurface。现把**同一 `(clientPid, toplevelId, wlSurfaceId)` 的普通 resize**改为原队列 `SetDefaultSize` + IPC 尺寸元数据更新，不换 producer/generation；身份变化或销毁仍按原代际规则处理。修正后的 IPC 探针仍 `PASS`，包括旧代拒绝和死亡通知（[最终 IPC JSON](evidence/direct-d3-fusion-final-ipc.json)）。
- MatePad Mini 的 `fusion` 独立窗口模式，Win32 x64 Direct WSI 实测 **150/150 PASS**，随后 **900/900 PASS**，长跑 **1800/1800 PASS、22933 ms**（[长跑 JSON](evidence/direct-d3-fusion-output-x64.json)）。App 侧日志持续报告同一 XComponent 上 `presents=1728`（其余前导帧在 XComponent 绑定前消费），并记录原位 resize（[设备日志](evidence/direct-d3-fusion-output-device.log)）；运行中的[设备截图](evidence/direct-d3-fusion-live2.png)显示 Wine Vulkan 测试窗口的黄色帧。生产者报告 `cpuReadBytes=cpuUploadBytes=0`、`perFrameDeviceWaitIdle=0`；不能仅凭 1800 帧/22933 ms 推断显示器实际刷新率。
- 产品路径进一步在 `verifyPixels=false` 时跳过诊断 storage/readback buffer、compute shader/pipeline 和相关 descriptor；只保留 fragment 取样与 Vulkan 输出。最终 signed HAP SHA-256 `6e0ee5ccc9a075ba3ea07c592e8344a8e7debc9a17dc457cdb87861f4b8ac822` 已构建、候选校验并装机。该包在 `fusion` 模式再次完成 **150/150 PASS**、App 输出 `presents=114`（[最终 JSON](evidence/direct-d3-fusion-no-readback-x64.json)）。
- 同一最终包对保留诊断校验的 D2 路径补测：fence 模式 8/8 帧、8 次 acquire/release `SYNC_FD`、像素检查均 `PASS`（[结果](evidence/direct-d3-fusion-final-d2-fence.json)）；同设备合成探针 8/8 帧、8 次 Vulkan output present、GPU 像素检查 `PASS`（[结果](evidence/direct-d3-fusion-final-d2-composite.json)）。
- 同一最终包、同一 `fusion` 会话的 Venus 对照完成 **150/150 PASS**，设备报告 `Virtio-GPU Venus (Maleoon 910)`、loader 1.3.290（[结果](evidence/direct-d3-fusion-venus-control-x64.json)）；随后再次切回 Direct，也 **150/150 PASS**，报告 Maleoon 910、系统 loader 1.3.275（[结果](evidence/direct-d3-fusion-direct-after-venus-x64.json)）。该结果证明本包在这一 x64 用例上保留了双后端，并不覆盖 Steam/游戏或全套默认生产路径。
- 虚拟桌面最终包的冷/温对照各在 Win32 测试写结果前超时，设备进程表同时留有上次强停后的 Wine NCP，且首次冷启动测试期间 App/DesktopAbility 曾处于后台；这两次不构成 WSI 失败判据，也**不能算虚拟桌面回归通过**。桌面模式的 Direct 多窗口叠层、运行中窗口尺寸变化/旧 swapchain 处理、异常退出和资源长期稳定性仍待验证；本阶段只证明独立窗口产品上屏。

### D3 第四阶段：运行中 resize 与旧 swapchain（2026-09-27）

- `winehua_vulkan_smoke` 新增显式 `WINEHUA_VULKAN_RESIZE=1` 路径：在 150 帧中点用 `SetWindowPos` 把同一 Win32 窗口从 640×480 改为 800×600，重新查询 surface capabilities，等待队列空闲，以旧 swapchain 作为 `oldSwapchain` 创建新链，重取图像并清除旧布局状态，然后继续 acquire/submit/present。结果 JSON 记录 `resizeRequested`、`resizeCompleted`、`swapchainRebuilds` 和前后 client extent。新增独立 `wine-vulkan-resize` 套件及宿主判定器，要求帧数、尺寸变化、重建次数和无 fallback 同时成立；旧 `wine-vulkan-present` 套件保持默认行为。
- MatePad Mini 同一已装 D3 HAP 上，**Direct 150/150 PASS**，client extent `632×446 → 792×566`、重建 1 次（[原始结果](evidence/direct-d3-resize-x64.json)）；**Venus 150/150 PASS**、同样尺寸与重建（[对照](evidence/direct-d3-resize-venus-x64.json)）；再切回 **Direct 1800/1800 PASS**、23,404 ms、无 fallback（[长跑](evidence/direct-d3-resize-after-venus-x64.json)）。新独立 gate 实跑 **150/150、设备端与宿主均 PASS**（[结果](evidence/direct-d3-resize-gate-x64.json)、[宿主判定](evidence/direct-d3-resize-gate-host.json)）。
- App 侧 `DirectWineSurface` 证据显示同一个 `top=6`、`gen=4`、`output=7224134993359` 在 resize 前消费 632×446 帧，收到 800×600 的原位 resize 后消费 792×566 帧，输出 `presents` 在新输出链上持续增长到 831；[筛选后的设备日志](evidence/direct-d3-resize-output-device.log)。这证明 producer→consumer→Vulkan 输出在运行中 resize 后继续工作。运行中截图落在 App 游戏库页，没有可用画面证据，因此本项只以帧/输出日志为判据；不宣称虚拟桌面、多窗口或 device-lost 已通过。
- 这轮只改了探针和自动化判定器，未修改已安装 HAP 的产品代码。Windows 宿主运行 `automation/smoke.py` 时需要 `PYTHONUTF8=1`，否则默认 GBK 解码会在设备测试已启动后中断归档；首次 Direct 结果是直接从设备文件沙箱取回，后续归档正常。

### D4 第一阶段：ARM64X DXVK 1.10.3 Direct（2026-09-27）

- 新增显式 `dxvk-direct-cube` 套件：ARM64X `d3d11.dll`/`dxgi.dll`、Create NCP、`WINEHUA_VULKAN_BACKEND=direct`，在 `fusion` 独立窗口模式运行。MatePad Mini 上立方体 **695 帧、设备端和视觉判定均 PASS**（[PE 结果](evidence/direct-d4-cube-x64.json)、[宿主判定](evidence/direct-d4-cube-host.json)、[运行中截图](evidence/direct-d4-cube-live.jpeg)）。App 记录同一个 Direct toplevel 的 960×640 GPU 帧持续消费并输出到 XComponent，至少到 660 帧（[设备日志](evidence/direct-d4-cube-output-device.log)）。同 HAP、同 ARM64X DXVK DLL、只把 Vulkan 路由覆盖为 Venus 的对照 **648 帧、视觉判定 PASS**（[PE 结果](evidence/direct-d4-cube-venus-x64.json)、[宿主判定](evidence/direct-d4-cube-venus-host.json)）。
- `dxvk` 原有 D3D11 资源覆盖探针在显式 Direct 下 **60/60 present PASS，suite coverage PASS**；包括 RGBA8 Load/POINT/LINEAR、descriptor、subresource 和纹理采样等矩阵，`cpuReadBytes=cpuUploadBytes=0`（[PE 结果](evidence/direct-d4-d3d11-x64.json)、[宿主判定](evidence/direct-d4-d3d11-host.json)）。App 的 Direct surface 日志显示 632×446 帧被消费并输出 60 次（[设备日志](evidence/direct-d4-d3d11-output-device.log)）。Venus 对照在同包同探针也 **60/60、coverage PASS**（[PE 结果](evidence/direct-d4-d3d11-venus-x64.json)、[宿主判定](evidence/direct-d4-d3d11-venus-host.json)）。探针原先把 `vulkanDevice` 写死为 `via winevulkan/Venus`，已改成中性的 `via winevulkan`；实际 Direct 路由由显式环境、surface attach/consumer/output 和 cube 截图共同确认。两轮的总用时含启动与资源检查，不能据此宣称性能收益。
- DXVK 2.6.2 的 Vulkan transport 资格探针在 Direct Maleoon 910 和 Venus Virtio-GPU 上均返回设备端 `UNSUPPORTED`：两侧底层 Vulkan API 都是 1.2.309，缺 Vulkan 1.3、robustness2、dynamic rendering、synchronization2、maintenance4，故 `eligibility.transport=FAIL`、`bringup=BLOCKED`（[Direct](evidence/direct-d4-dxvk26-capability-x64.json)、[Venus](evidence/direct-d4-dxvk26-capability-venus-x64.json)）。此探针的宿主 PASS 只表示正确识别了不支持状态，不表示 2.6.2 已可运行；不在 Direct 上强启现代 DXVK。
- D4 当前结论限定为 **ARM64X DXVK 1.10.3 的单窗口立方体和 D3D11 smoke A/B**。真实 D3D11 游戏、帧时/CPU/拷贝量的同场景测量、异常退出回退，以及 D3 的 device-lost 和 D5 虚拟桌面多窗口仍未验收；默认路由保持 Venus。

### D4 Mate 80 无线实机：DXVK 2.6.2 的已验证范围（2026-09-27）

- 测试目标是无线 HDC `192.168.180.76:44559` 的 **Mate 80 / VYG-AL00 / API 26**，不是 USB `5KPBB25818203996` 的 MatePad Mini。已安装并由 `bm dump` 核验 `app.hackeris.winehua`；签名 HAP SHA-256 为 `6e0ee5ccc9a075ba3ea07c592e8344a8e7debc9a17dc457cdb87861f4b8ac822`。初始验证只推送 smoke 载荷，后续复测重装同哈希的 HAP；没有重新构建产品 HAP。
- 手机的现有 **Venus/fork** 路径上，项目定制的 ARM64X **DXVK 2.6.2** D3D11 立方体完成 **876 帧、设备端 PASS、视觉校验 PASS**（[PE 结果](evidence/mate80-d4-modern-cube-venus-x64.json)、[实机截图](evidence/mate80-d4-modern-cube-live.jpeg)）。因此该手机能运行 2.6.2 的这条 D3D11 呈现路径；这不代表 Direct 路由已通过。
- 同手机同 HAP 的 2.6.2 完整 D3D11 资源覆盖探针在首帧前 **FAIL**（[结果](evidence/mate80-d4-modern-baseline-venus-x64.json)、[宿主判定](evidence/mate80-d4-modern-host.json)）。错误文案写作“initialization or required feature contract failed”，但数据中的 `featureProbeGpuCopies=70` 证明 D3D11 device/swapchain 创建和多项探针已经执行；源码中 `run_feature_probes()` 返回后，`run_heaven_resource_probes()` 的资源矩阵失败使 `create_device()` 早退。RGBA8 采样/更新读回均为零、Heaven 资源矩阵全未通过，`presentFrames=0`。**DXVK 1.10.3** 同探针在此手机为 **60/60 帧、覆盖 PASS**（[对照](evidence/mate80-d4-legacy-venus-x64.json)），故需继续定位现代 DXVK 与这组资源/同步操作的差异，不能把失败泛称为设备不支持 2.6.2。
- Wine Vulkan Venus 离屏对照枚举到 `Virtio-GPU Venus (Maleoon 920)`、设备 Vulkan **1.3.269**；buffer copy 和 image clear 通过，但 `venus_storage_write.spv` 的读回保持 `0xdeadbeef`，探针 FAIL（[结果](evidence/mate80-d4-vulkan-venus-offscreen-x64.json)）。这为存储图像/读回路径提供独立线索，尚未证明它与上述 DXVK 2.6.2 资源矩阵同根因。DXVK 2.6.2 资格探针在写出能力结果前退出（[摘要](evidence/mate80-d4-dxvk26-probe-summary.json)），该次退出同样不能作为“不支持”的结论。
- 对 Direct 请求了显式 `WINEHUA_VULKAN_BACKEND=direct`、`--direct-ncp-session` 和 `fusion` 独立窗口：Wine Vulkan 离屏探针 **90 秒未写结果**（[摘要](evidence/mate80-d4-direct-create-offscreen-summary.json)），2.6.2 立方体 **60 秒未写结果**（[摘要](evidence/mate80-d4-modern-direct-create-summary.json)）。随后核对发现手机模式的 `SetBrokerDirectNcpSessionDefault()` 会强制关闭会话级 Create NCP，设备 broker 对测试 PID 记录的实际启动方式是 `launch mode=start`。因此这两次是 **phone fork/Start 路径的超时**，没有运行 Pad 上的 Create NCP Direct surface 控制面，不能拿来判定 Direct WSI 或手机 Vulkan 驱动失败。Mate 80 的 Direct WSI/DXVK 2.6.2 仍未验收；要在手机上验收，先需建立 phone fork 到 Direct NativeWindow 的受控传递与生命周期，再按离屏→WSI→DXVK 顺序复测。手机默认 Venus 路由保持原状。
- 同日按用户要求在无线 Mate 80 上重装上述同 SHA-256 的签名 HAP，并用新载荷复测。**Venus + DXVK 2.6.2** 的原版 ARM64X D3D11 立方体再次 **911 帧 PASS**，实际加载 `modern-2.6/arm64x/d3d11.dll` 和 `dxgi.dll`；[设备结果](evidence/mate80-d4-reinstall-modern-cube-x64.json)、[实机截图](evidence/mate80-d4-reinstall-modern-cube-live.jpeg)。同配置仅切换为 DXVK 1.10.3 的对照 **893 帧 PASS**（[设备结果](evidence/mate80-d4-reinstall-legacy-control-x64.json)）。这两轮仅选立方体，`dxvk-modern-baseline` 套件级 coverage 因未选资源矩阵用例而显示 FAIL，不能把该 coverage 状态误读成这两个立方体失败。
- 新增的运行中 resize 立方体探针在手机上即使未设置 `WINEHUA_D3D11_RESIZE`，手工替换同一个 `C:\smoke\x64` 测试程序后仍伴随宿主退出；Hiview fault 记录为 `SIGABRT`，栈落在 `virgl_renderer_cleanup → libepoxy → abort`。整套载荷重新推送后的首轮也出现相同宿主崩溃，而不再推送的旧探针复测通过，因此目前只能把 resize 探针和首轮推送/启动状态列为触发条件，不能由清理栈反推 DXVK 根因（[诊断摘要](evidence/mate80-d4-reinstall-diagnostics.json)）。已把 resize 探针独立为 `d3d11-resize-cube` 用例，常规 DXVK 回归继续用原版立方体；两者均编译通过。完整资源矩阵复测在写出设备结果前又触发同一宿主清理栈，维持上条已记录的资源覆盖未通过结论。
- 重装后的显式 Direct 2.6.2 立方体复测仍无设备结果；broker 对该测试 PID 再次记录 `launch mode=start`，未进入 Create NCP Direct surface 路径。这次结果仍不能作为手机 Direct 2.6.2 兼容或不兼容的判据。

### Mate 80 Direct 启动链路暂停点（2026-09-27）

- 新增仅诊断用的 `direct-probe-system-create`，绕过手机 fork 拦截层，直接调用系统 `libchild_process.so` 的 Create。Mate 80 返回 `NCP_ERR_MULTI_PROCESS_DISABLED (16010004)`，在创建子进程前被拒绝（[原始结果](evidence/mate80-direct-system-create-20260927.json)）。`bm dump` 显示本应用 `allowMultiProcess: false`；抽查系统设置、相机和两个其他应用也为 `false`。本轮没有找到可由应用声明启用的依据，不把它写成已证实的手机硬件不支持。
- 显式开放手机上的 `direct-probe-start` 作为独立诊断。phone fork `Start` 返回成功和 PID，但两次均在 15 秒内没有写出 Vulkan 结果；最近一次为 `stage=result_timeout`（[原始结果](evidence/mate80-direct-phone-fork-start-20260927.json)）。子进程仍存活，新增的 `Main` 入口和 Vulkan `dlopen` 阶段日志均未出现，故尚无法判定手机系统 Vulkan 能力。当前应先定位 fork 子进程在加载诊断库或进入入口函数前的阻塞，再测离屏 Vulkan。
- 当前安装在无线 Mate 80 的诊断 HAP SHA-256 为 `cce3905fc248e14c70a3380c8ed6ddfb837b8a6aac4c81408b8cb2c05a58c7dd`，签名构建通过。它只新增诊断入口/日志，默认 Wine 路由仍是原有 Venus/fork。安装后的 Venus + 2.6.2 立方体尝试从仍停留在 DirectProbe 页的会话发起，SmokeHook 接收了请求但测试没有执行；该次已中止，**不能算新包回归**。暂停前已 force-stop App。上文的 911 帧 PASS 是此前同手机、此前 HAP 的有效结果；恢复工作时先冷启动做当前包的 Venus 基线，再继续 Direct。
- 恢复调试时为 phone fork `Start` 探针增加独立阶段 fd：子进程用单字节报告进入 `Main`、系统 Vulkan `dlopen`、扩展查询与 `vkCreateInstance` 的前后阶段；父进程同时轮询阶段 fd 和原结果 fd，短包不会被误判为 PASS。签名 HAP SHA-256 `eafd89fcde678d78eeeb747b62b6f30753fca5022f3e754a3d67dc55efc56941` 已构建并校验，但**尚未装机**。Mate 80 无线 HDC 在此时显示 `Offline`，旧 IP:端口的 TCP 可连接但 HDC 握手失败；这属于设备连接状态，不能计为渲染测试结果。装机恢复后先冷启动重测 Venus + 2.6.2，再运行 `direct-probe-start` 读取精确阻塞阶段。

## Mate 80 根因修复与手机上屏方案（2026-09-28）

### 已解决：早期 fork、trace fd 污染与会话重启

1. 晚期 App fork 的子进程在系统 GPU 初始化附近阻塞。将单线程 fork server 放在 `EntryAbility.onCreate`、ArkUI 页面创建之前，再在该 server 中通过系统 musl 的 `_Fork()` 派生子进程；不能在 server 中再次普通 `fork()`，否则会执行继承的 atfork 回调。原生 D0 **10/10 PASS**，不同 child PID、exit 0、全部回收（[逐轮结果](evidence/mate80-direct-fork-server-runs-20260928.ndjson)）。此特殊 `_Fork` 路径仅用于这个尚未初始化 GPU 的单线程 server。
2. Wine 随后遇到 `Protocol error: partial recvmsg 7 for fd`。报错位置是 `server/request.c:receive_fd()`；抓到的七字节为 `63735f7465735f`（`cs_tes_`），没有 SCM_RIGHTS（[原始日志](evidence/mate80-direct-wine-fd-bytes-20260928.log)）。server 继承 fd 4、9 指向 `/sys/kernel/debug/tracing/trace_marker`（[fd 证据](evidence/mate80-direct-fork-inherited-fds-20260928.log)）。原先全部关闭，系统 tracing 库仍可能按缓存编号写入，被 Wine 复用的编号便会污染协议通道。**仅保留 tracing fd 的单变量 A/B** 使真实 Wine Direct 离屏由失败变为通过，有无 Vulkan warmup 都通过（[有 warmup](evidence/mate80-direct-wine-keep-trace-warmup-20260928.json)、[无 warmup](evidence/mate80-direct-wine-keep-trace-no-warmup-20260928.json)）。尚未取得具体写入库的调用栈；结论依据为实际错误字节、继承 fd 和 A/B。
3. `phone_adapter/phone_process.cpp:DirectForkServerMain()` 现按 `/proc/self/fd` 的实际路径保留 `/sys/kernel/debug/tracing/trace_marker`、`/sys/kernel/tracing/trace_marker`，并保留控制/退出/握手 fd。没有硬编码 fd 4、9。临时 warmup 和 wineserver FD-PROBE 插桩已移除，没有放宽 wineserver 协议校验。
4. 运行库刷新和实际 UI“重启 Wine”会调用 `KillAllProcesses()`；原逻辑同时杀死了早期 server，导致后续 `launch mode=phone-fork-server ret=801 childPid=-1`。`proc/wine_process.cpp` 两轮 descendants 清理均跳过 `Phone_GetDirectForkServerPid()`，仍清理 Wine 子进程。实际 UI 重启时 App PID `58080`、server PID `58903` 保持，wineserver `58972 → 60243`；同 App 重启后 Direct x64 离屏再次通过（[结果](evidence/mate80-direct-after-engine-restart-20260928.json)）。server 生命周期仍跟随 App，未重置用户 prefix 来模拟验证。

以上产品修复包 SHA-256：`30b38d23319440d219dce9cae417205f8d45d983b7f7b63bb9a2ed49d59af209`。真实 Wine Direct [x64](evidence/mate80-direct-trace-lifecycle-final-x64-20260928.json)、[x86](evidence/mate80-direct-trace-lifecycle-final-x86-20260928.json) **2/2 PASS**：系统 loader、Maleoon 920、Vulkan 1.3.309，buffer copy、image clear、storage write/read、sampled fetch 和组合/分离 sampler 均通过，无 fallback。专用 [DXVK 2.6.2 资格检查](evidence/mate80-direct-dxvk26-final-20260928.json) 为真实设备端 **PASS**，包括 Vulkan 1.3、robustness2、dynamic rendering、synchronization2、maintenance4、device create 与 timeline 往返；这不是 DXVK Direct 上屏通过。

同包默认 **Venus + ARM64X DXVK 2.6.2 cube 885 帧 PASS**，设备结果和宿主单项视觉判定均通过（[设备结果](evidence/mate80-trace-fix-venus26-final-20260928.json)、[宿主结果](evidence/mate80-trace-fix-venus26-host-20260928.json)、[截图](evidence/mate80-trace-fix-venus26-live-20260928.jpeg)）。仅选了 cube，套件级 coverage 因未选资源矩阵而 FAIL；不能把它误记为 cube 失败，也不能把 cube 通过当作完整资源矩阵回归。

### 未解决：手机 Direct 上屏缺少 producer

真实 Wine Direct WSI 已明确到达 surface 创建，但记录 `WineHua Direct producer unavailable owner_wl=3 pid=59267`，`presentFrames=0`、`VK_ERROR_SURFACE_LOST_KHR (-1000000000)`（[设备结果](evidence/mate80-direct-trace-wsi-20260928.json)，对应日志未归档）。这是该手机历史截点的首帧阻塞点，不能用离屏 PASS 替代。

代码链为 `direct_wine_surface_controller.cpp` 的 `WineIpcChildUsesDirectVulkan(pid)` 登记 → `wine_child_ipc_launcher.cpp` 的真实 `OHIPCRemoteProxy` 和 `OH_NativeWindow_WriteToParcel` → 子进程 producer registry → `winewayland.drv/vulkan.c:winehua_direct_surface_create()`。平板 Create NCP 具有该通道；手机早期 fork server 只建立了 argv/fd/退出通道，没有 Binder proxy 和 producer 登记，surface 获取等待后失败。仅修改登记布尔值、传 surfaceId 或父进程指针，都不能补齐资源交接。

本机官方离线 `@ohos.app.ability.childProcessManager` 文档明确 SELF_FORK 不能用 Binder 与其他进程通信，而 APP_SPAWN_FORK 可以；相关 ArkTS 接口本身只在 Tablet/PC 正常调用。手机系统 Create 实测返回 `16010004`。官方 `OH_NativeWindow_CreateNativeWindowFromSurfaceId` 文档又明确只允许获取本进程创建的 surface。因此不能把手机 dummy proxy 当真 proxy 使用，也不能将平板的生产者窗口原样搬过来。

### 共享图像方案的实测能力与取舍

为避免仅凭扩展名实施新 WSI，D0 结果协议升级为 version 4，记录每组 external image 的 handle、格式、tiling、usage、VkResult、features 和 compatible handles，并单独记录 external buffer 查询。最终能力诊断包 SHA-256：`b360f4a02f01d7984c7e202e1180f0387297de9a1e0ccc6c617bdf5bf3f7dfb4`，已构建、签名、候选校验并装机。原生 D0 PASS，child PID `6500`、server PID `6394`、child exit 0 且已回收。**此 PASS 只属于 D0**；[完整能力结果](evidence/mate80-direct-external-memory-audit-20260928.json) 应逐项读取：

| 方案/查询 | Mate 80 结果 | 含义 |
| --- | --- | --- |
| OPAQUE_FD 图像：RGBA8 optimal，render+sample+transfer / color+sample / sample；RGBA8 linear transfer；BGRA8 optimal 两种 usage | 六项均 `VK_ERROR_FORMAT_NOT_SUPPORTED (-11)` | 此次所测配置均不支持；不能采用通用 fd 图像方案 |
| OPAQUE_FD buffer：transfer src+dst | 查询执行，features=0 | 不能以共享 buffer 加 GPU 拷贝绕过上述限制 |
| DMA_BUF 图像 | 未公布 `VK_EXT_external_memory_dma_buf`，未查询；记录的 -7 为探针跳过标记 | 没有可依赖的 DMA_BUF Vulkan 导入路径 |
| OHOS NativeBuffer：RGBA8 optimal，color+sample+transfer / sample | 两项 `VK_SUCCESS`，features=7，compatible handles=`0xa000` | 支持 import/export，要求 dedicated allocation；仍需实际分配、跨进程导入验证 |
| SYNC_FD semaphore | 支持 import/export | 具备 GPU fence 交接的能力前提，手机新桥尚未实测往返 |

初始只查一组 OPAQUE_FD 的候选 `d00f359a…` 已装机测得 mask=1023（[原始结果](evidence/mate80-direct-fd-capability-initial-20260928.json)）。最终矩阵排除了单一格式或过宽 usage 导致误判的情况，但不泛化为驱动在所有场景均不支持 external memory。

最终 `b360f4a0…` 包的真实 Wine Direct 离屏复测 **x64/x86 2/2 PASS**（[x64](evidence/mate80-memory-audit-final-x64-20260928.json)、[x86](evidence/mate80-memory-audit-final-x86-20260928.json)、[宿主](evidence/mate80-memory-audit-final-host-20260928.json)）。首次冷启动在 wineboot 阶段等待 180 秒失败，测试未执行；强停 App 后原配置、原 prefix 重试即通过。保留[首轮超时日志](evidence/mate80-memory-audit-wineboot-timeout-20260928.log)，不将重试通过描述为冷启动稳定性已验收。

同包温启动默认 Venus + 2.6.2 [878 帧设备结果](evidence/mate80-memory-audit-venus26-final-20260928.json) PASS；[自动视觉判定](evidence/mate80-memory-audit-venus26-host-20260928.json)也写了 PASS，**但人工逐张检查四张截图均是控制页，没有立方体，因此撤销该轮视觉通过结论**（[原图](evidence/mate80-memory-audit-venus26-live-20260928.jpeg)）。`d3d11-cube-color-depth-v1` 仅检查全图 RGB/暗色分布，控制页彩色按钮能触发假阳性；后续必须同时确认测试窗口/目标区域，不能只看这个自动 PASS。此轮只证明 PE present 完成，前述 `30b38…` 包的 885 帧截图经人工核对确实包含立方体。

随后同一最终包、同一 prefix 冷启动 Venus + 2.6.2，**891 帧 PASS，人工复核截图确有立方体**（[设备结果](evidence/mate80-memory-audit-venus26-cold-20260928.json)、[宿主结果](evidence/mate80-memory-audit-venus26-cold-host-20260928.json)、[实际画面](evidence/mate80-memory-audit-venus26-cold-live-20260928.jpeg)）。仍只选 cube，套件 coverage FAIL 的原因仍是未运行资源矩阵。结束时 App 已由 smoke 强停，没有 WineDirectFork/wineserver 残留，临时熄屏设置已通过 `power-shell timeout -r` 恢复。

### 推荐的手机实现路径及下一个验收点

采用**手机专用 NativeBuffer 图像槽交接**，保持 Wine/DXVK 在自己的进程直接执行系统 Vulkan：

```text
App 分配 NativeBuffer 槽并管理窗口/代际
  ↕ Unix socket：缓冲区 fd（SCM_RIGHTS）、有界元数据、slot/generation、SYNC_FD
Wine 子进程导入 NativeBuffer → 系统 Vulkan 渲染
  → 帧完成 fence → App Vulkan consumer/compositor → XComponent
```

这条候选路径不传 Vulkan 绘制命令；跨进程仍需要窗口控制、图像身份和同步。它与已完成的平板 BufferQueue/Binder 路径并行，不能直接复用当前 `VkSurfaceKHR` 构造而省略手机 swapchain/acquire/present 实现。

实现前必须先做一个隔离探针：App 分配 64×64 NativeBuffer，传完整 buffer handle/附加 fd/元数据至早期 fork 子进程，子进程按 dedicated allocation 导入、GPU 写已知图案并导出 SYNC_FD，App GPU 等待/采样验证，交还 release fence，再复用第二帧。后续才扩到双槽、resize 换代、异常退出和真实 Wine WSI。App 的诊断 GPU 取样读回只用于证明像素，正式路径关闭读回。

2026-09-29 更新：**P0 交接代码已实现，但跨进程 NativeBuffer 注册门槛未通过；手机 Wine/DXVK Direct 上屏仍未完成。** 公开 NativeBuffer C API 提供 `WriteToParcel/ReadFromParcel`，并未直接提供 Unix socket 导入/导出接口。OpenHarmony 上游代码显示其序列化对象是 SurfaceBuffer 的 handle/字段，区别于包含远程 producer 的 NativeWindow；但这只支持选定调查方向，不证明 Mate 80 的版本兼容性。本轮设备限定的实验 codec 已验证本地序列化布局、附加 fd backing identity 和重新编码；实际 fork child 的注册却触发 HDI/SAMGR 服务获取等待，尚未进入 Vulkan 图像导入。不得直接拷贝 parcel 字节中的 fd 整数、虚拟地址或指针，也不得把 NativeWindow parcel 当作普通 buffer parcel。保留手机现有路由，单列未满足的系统接口，不将探针代码或 0 帧计数写成 Direct 成功。

上游核对入口（仅作设计参考，未将其代码加入产品）：[native_buffer.cpp](https://github.com/openharmony/graphic_graphic_surface/blob/master/surface/src/native_buffer.cpp)、[buffer_utils.cpp](https://github.com/openharmony/graphic_graphic_surface/blob/master/surface/src/buffer_utils.cpp)、[surface_buffer_impl.cpp](https://github.com/openharmony/graphic_graphic_surface/blob/master/surface/src/surface_buffer_impl.cpp)。`master` 不代表 Mate 80 固件对应版本。

本轮没有切换默认 Vulkan 路由，没有调整 Steam/FEX，也没有提交或批量恢复工作树的现有改动。完整 smoke 载荷保持 `smoke-v2-2e5e6843ea9d`。

## 手机 P0 共享 NativeBuffer 实现与最终实测（2026-09-29）

目标仍是 Wine 进程执行系统 Vulkan、App GPU 采样成品图像。新增 opt-in `winehua.mode=direct-phone-shared-buffer-probe`，只运行隔离的 P0，不启动 Steam、不改变 FEX/默认图形路由，也没有提交现有工作树。

- `direct_shared_buffer_probe.cpp`：App 分配 64×64 RGBA8 单槽、本地 codec 往返、启动 child、GPU sampler 和 SYNC_FD 归还；15 秒 packet deadline、3 秒退出观察、失败清理与结果 JSON。系统 Start 用 `dlopen/dlsym` 直接解析真实实现，并独立记录系统退出回调；不伪报系统 child 的 waitpid/reaping。
- `direct_shared_buffer_child.cpp`：系统 Vulkan device-first 初始化、dedicated NativeBuffer import、GENERAL/EXTERNAL ownership、四帧 GPU clear、render/release fence 往返与最终 drain。NativeBuffer RAII 的声明顺序保证 Vulkan image/memory/device 先析构。上述是代码契约；实际未执行到图像导入。
- `native_buffer_socket.{h,cpp}`：wire v4、SOCK_SEQPACKET/SCM_RIGHTS、CLOEXEC、截断/长度/fd 数量校验、RAII；最多 16 fd 和 256 ints。Linux 真实 socket 测试通过，包括 backing alias、400 个非法包、无 fd 泄漏和超时。
- `native_buffer_parcel.cpp`：使用公开 WriteToParcel/ReadFromParcel，避免 SDK `BufferHandle` 尾部与 Mate 80 ABI 不一致；只接受简单布局或设备实测的 `reserveFds=1/reserveInts=65`。第 13/14 项作为私有字段对清零并要求系统重建；其含义未公开，不能由本地校验断言跨进程安全。其他字段严格匹配，两个 fd 的 `st_dev/st_ino/st_size` 相同。该适配器仍不能作为通用稳定协议。
- `direct_native_read_watchdog.cpp`：child 内用官方 `libohhidebug.so` 的 FP unwinder 采集有限等待栈。普通 fork/保留映射的实验分支仅对 P0 SO 和参数生效；其他 Wine/探针继续原路由。

最终 HAP SHA-256 **`04e91fb20aa6834e2e68c0b71a74a9180f581ea24e5cfca34c9a0d69b25d5a79`**，构建、签名、候选校验并装机完成。完整 smoke 保持 `smoke-v2-2e5e6843ea9d`、33 个测试 EXE。最终同包三组结果：

| 启动方式 | 结果 |
| --- | --- |
| 真实系统 Start | 本地 NativeBuffer 往返通过；`launchCode=16010004`、PID=-1，未创建子进程（[JSON](evidence/mate80-phone-shared-p0-system-start-v4-20260929.json)） |
| 早期 server 的 `_Fork()`，device-first | 系统 Vulkan/Maleoon 920 device 创建成功；ReadFromParcel 的 HDI/SAMGR 获取超时，0 帧（[JSON 与栈](evidence/mate80-phone-shared-p0-device-first-v4-20260929.json)） |
| 早期 server 的普通 `fork()`，保留映射/高编号 fd | 同样创建 device 后卡在同一服务获取栈，0 帧（[JSON 与栈](evidence/mate80-phone-shared-p0-standard-fork-v4-20260929.json)） |

栈为 `OH_NativeBuffer_ReadFromParcel → SurfaceBufferImpl::SetBufferHandle → IDisplayBuffer::Get → IAllocator::Get → HDI ServiceManager::Get → SAMGR GetSystemAbilityWrapper/Recompute → usleep`。它证明本地 parcel 重建仍需要接收进程访问系统服务，与官方 SELF_FORK 的 Binder 限制相符；没有明确 Binder 拒绝码，不能将推断写成已取得拒绝日志。两次 fork 的 SIGKILL=9 均由探针超时清理发送，server 已回收 child，不能算原生崩溃。保留/清理低映射、普通 fork/_Fork、Vulkan 初始化顺序都未解决注册等待。

同包原生 D0 **PASS**，系统 Vulkan 1.3.309、Maleoon 920、离屏像素正确、exit 0 且已回收（[结果](evidence/mate80-phone-shared-p0-final-d0-20260929.json)）。本轮未重跑真实 Wine/DXVK 上屏；此前手机 2.6.2 的 Venus 和 Direct 离屏结论仍按各自历史包记录，不扩展为最终包的新回归。

当前停在 **P0 FAIL/0 帧**。下一步先核实能访问 SAMGR/HDI 的受系统管理进程入口，或官方无需此注册链的 NativeBuffer 导入接口。通过共享图像和 SYNC_FD 的真实往返后，才推进双槽/resize/异常生命周期（P1）、Wine 手机 WSI（P2）和同手机 1.10.3/2.6.2 Direct 对照（P3）。完整设计、最终包身份和实验边界见 [共享图像设计 §8](direct-render-shared-image-design.md#8-p0-实现与实验边界2026-09-29)。

结束前 App 已 force-stop，设备进程列表无本应用/fork server/P0 child/wineserver 残留；临时熄屏设置已恢复。`git diff --check` 通过，工作树未提交。

### DXVK Native 手机成功路径的代码核对（2026-09-29）

已阅读 PomeloTechLabs 的 OHOS Native 1.10.3（`fb2cfafb…`）和 2.6.2（`7b924f77…`）。两版都在进程内登记 XComponent NativeWindow，由同进程 DXVK WSI 创建 OHOS surface/系统 swapchain 并直接 present，没有我们 P0 的跨进程 NativeBuffer 重建。用户反馈的手机运行成功与此路径相符，但仓库没有手机 HAP 启动代码，不能由库源码断言调用应用整体无其他进程。

手机当前受限的是所测系统 NCP 入口及自行 fork 后的图形服务/资源交接，不应概括成手机不支持 Vulkan Direct 上屏。下一设计决策应围绕渲染进程的窗口归属与系统服务上下文；同进程 Native 方案并不证明现有 fork Wine 能直接使用 App 的窗口 ID。完整固定版本源码依据与差异见 [设计记录 §9](direct-render-shared-image-design.md#9-pomelotechlabs-dxvk-native-代码对照2026-09-29)。

### 手机恢复后的候选：反向 Vulkan NativeBuffer 导出（尚未实测，已搁置）

现有自定义 fd/元数据 transport 已通；新增优先实验是 Guest 通过 dedicated Vulkan image/memory 和公开 `vkGetMemoryNativeBufferOHOS` 导出，再由正常 App 接收/注册并 GPU 采样。NDK 已确认该 API，手机 RGBA8 OHOS external memory 的 exportable 位也已查询；驱动 allocation/export 内是否仍访问 HDI/SAMGR 尚未知。此方案只改变分配与注册方向，保留 Guest GPU 渲染、App 贴图；不提前声称可行或零拷贝通过。先做 64×64 单槽四帧、实际 SYNC_FD 往返、资源清理探针，细节见 [设计记录 §10](direct-render-shared-image-design.md#10-自定义图像交接的新候选guest-vulkan-导出2026-09-29)。

用户明确排除 CPU 图像回读/共享像素/上传呈现：此次优化只推进 Guest 本地执行 Vulkan、GPU 图像共享和 fence 交接。若该路径不通，继续现有 Venus；不新增低性能回读 fallback。探针的小型像素校验仅用于验收，正式帧路径与性能 A/B 关闭诊断读回。

## 平板 D5 当前落点（2026-09-29）

当前平板装机为 [性能基准候选](direct-performance-preparation-20260929.md) `b741c40f…9f0d`，包含 viewport 采样与 Native 性能计数，已构建/签名/安装；完整 host payload 为 `smoke-v2-474c8f475ecd`。真实 AMD64 的短工具检查与第一组 fixed60 ABBA 已运行，后续第五轮启动后超时；初组未显示两进程 CPU 节省，见 [初步数据](direct-performance-results-20260929.md)。此前 `c64d3c85…7ea62` 的初次显示回归被锁屏阻止。性能方案与旧包结果分别记录，未宣称显著性能提升或整体验收完成。

新增 `DirectVulkanDesktopCompositor` 已独占虚拟桌面 XComponent 的 Vulkan 输出。游戏用系统 Vulkan 渲染，App 缓存导入共享 NativeBuffer，按 Wayland scene 合成，并用 acquire/release SYNC_FD 交接 GPU 所有权；GDI/ARGB UI 上传单独计数。输出方向、client-only 窗口退出全屏状态同步已修复，最终包的双窗口、透明遮挡、客户区 resize、全屏切换、实际触摸与 fusion 方向均有有效证据，见 [实现与验收](direct-vulkan-desktop-20260929.md)。

公共 Vulkan device/import/output 核心已拆出为 `DirectVulkanContext`，桌面不再依赖 probe 私有状态；后续 fusion 也已独立为 `DirectVulkanPresenter`，生产 fusion 输出 resize 通过，见 [最新记录](direct-vulkan-presenter-20260929.md)。显示 smoke 已补齐产品 ensureDesktop 就绪等待，[三轮未经预热 scene 启动](direct-desktop-startup-20260929.md) 的宿主/设备与输入通过。性能尚未量化，下一门槛是 [同设备 Direct/Venus A/B](direct-performance-gate-20260929.md)。下一阶段仍在平板上：补完整窗口与异常生命周期、生产桌面输出 resize/旋转和长期资源稳定性，再做真实游戏与性能 A/B。手机候选留档，暂不运行；Steam/FEX 不随本轮切换。

## 能力事实与剩余边界

- 目标设备的 Create 类型 NCP 能加载系统 Vulkan，并暴露 Maleoon 910 queue/device；D0 已实测。Start 类型 NCP 的 `vkCreateInstance=-9` 原因未查明。
- ConsumerSurface producer 能在独立 NCP 通过 IPC parcel 稳定使用，双 buffer 与 CPU fence/resize/异常退出已实测；D1 已回答此范围。
- 平板 `OH_NativeBuffer` 的 Vulkan 导入、GPU fence 交接和共享图像合成已由 D2 及最终 D5 fixture 实测。手机的跨进程注册仍未通过。设备结论分别记录，不能由 SDK 符号存在或平板通过外推手机支持。
- Wine NCP 的 Create 型 IPC bootstrap 已在真实 `libwine_child.so` 验证 argv 串、两个命名 fd、PID 和 proxy 死亡通知；broker 会话级可选路由已接入进程登记。最终包已核对 wineserver 的 Create/Start A/B、跨架构多进程 `platform-process` 2/2、单进程回退；此前 `core` 可选路径 4/4 通过。仍需验证真实 Steam/CEF、完整 i386 `TerminateThread` 契约、异常退出分类与长期资源稳定性，才能回答完整的 D2.5 默认切换问题。

这些 Gate 的任一项失败，保留当前 Venus/VirGL 路径，记录设备与失败阶段；不提前在 Wine 主路径上堆条件分支。Direct 的默认启用要等 D3/D4 真实游戏 A/B 和回退验证完成。

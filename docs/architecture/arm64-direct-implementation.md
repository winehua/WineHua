# ARM64 Direct 渐进实施方案（feature/proton-wine-ohos）

> 2026-09-26，基于主仓库 `7fc70bfc`、`thirdparty/wine-valve` `8b5bee4` 和本机 OpenHarmony SDK 头文件核对。`WineHua_ARM64_Direct_渐进式架构重构指导.md` 是目标与阶段建议；此文把它落到当前分支的实际进程、图形和构建接口。设备能力和性能结论必须以新 HAP 的实测为准。

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
- 这证明实际跨进程 BufferQueue 的 NativeBuffer 可在 App 进程创建并缓存 Vulkan sampled image view；**尚未通过 GPU 命令从该 image 取样或合成**。现有像素判断仍靠 CPU map，acquire fence 用 CPU poll，子进程每帧等待 submit fence。下一步需在独立诊断 XComponent 上完成 GPU 采样/合成与 fence 的 GPU import/export，确认 queue-family/layout 所有权、release fence 交接，以及无逐帧 CPU wait/import 的 60 FPS 运行。

## 关键的未知事实

- 目标设备的 Create 类型 NCP 能加载系统 Vulkan，并暴露 Maleoon 910 queue/device；D0 已实测。Start 类型 NCP 的 `vkCreateInstance=-9` 原因未查明。
- ConsumerSurface producer 能在独立 NCP 通过 IPC parcel 稳定使用，双 buffer 与 CPU fence/resize/异常退出已实测；D1 已回答此范围。
- 设备实际是否支持 `OH_NativeBuffer` 的 Vulkan 导入、GPU fence 交接和零拷贝合成；D2 回答。`VK_OHOS_external_memory` 等符号存在于 SDK 只说明可以编译。
- 每个 Wine NCP 如何获得 OHIPC proxy 而不破坏目前 `StartNativeChildProcess` 的 argv、命名 fd 和 CreateProcess 语义；D2.5 回答。不能把现有 VirGL 子进程的 IPC 能力直接推断到 Wine 子进程。

这些 Gate 的任一项失败，保留当前 Venus/VirGL 路径，记录设备与失败阶段；不提前在 Wine 主路径上堆条件分支。Direct 的默认启用要等 D3/D4 真实游戏 A/B 和回退验证完成。

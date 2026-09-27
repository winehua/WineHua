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

## 关键的未知事实

- 目标设备的 Create 类型 NCP 能加载系统 Vulkan，并暴露 Maleoon 910 queue/device；D0 已实测。Start 类型 NCP 的 `vkCreateInstance=-9` 原因未查明。
- ConsumerSurface producer 能在独立 NCP 通过 IPC parcel 稳定使用，双 buffer 与 CPU fence/resize/异常退出已实测；D1 已回答此范围。
- 设备实际是否支持 `OH_NativeBuffer` 的 Vulkan 导入、GPU fence 交接和零拷贝合成；D2 回答。`VK_OHOS_external_memory` 等符号存在于 SDK 只说明可以编译。
- Wine NCP 的 Create 型 IPC bootstrap 已在真实 `libwine_child.so` 验证 argv 串、两个命名 fd、PID 和 proxy 死亡通知；broker 会话级可选路由已接入进程登记。最终包已核对 wineserver 的 Create/Start A/B、跨架构多进程 `platform-process` 2/2、单进程回退；此前 `core` 可选路径 4/4 通过。仍需验证真实 Steam/CEF、完整 i386 `TerminateThread` 契约、异常退出分类与长期资源稳定性，才能回答完整的 D2.5 默认切换问题。

这些 Gate 的任一项失败，保留当前 Venus/VirGL 路径，记录设备与失败阶段；不提前在 Wine 主路径上堆条件分支。Direct 的默认启用要等 D3/D4 真实游戏 A/B 和回退验证完成。

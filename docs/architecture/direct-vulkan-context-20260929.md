# Direct Vulkan 公共资源核心（2026-09-29）

本页保留 `64cbd659…3021f` 公共核心阶段的包与证据。后续已完成 [生产单图 presenter 拆分](direct-vulkan-presenter-20260929.md)，其包为 `00b507b6…3c5e6`；后续 [启动 gate 修复](direct-desktop-startup-20260929.md) 的 `f5dbf252…df23` 完成三轮未经预热启动。各阶段结果分别记录。

本轮继续平板 Direct 生产接口重构，手机验证保持搁置。已有共享 GPU 图像路径不变：游戏进程调用系统 Vulkan，HAP 采样共享 NativeBuffer 并合成窗口；不新增游戏图像 CPU 回读、像素搬运或重新上传。

## 资源归属

当前装机另已更新到 [性能基准候选](direct-performance-preparation-20260929.md) `b741c40f…9f0d`，包含 viewport 采样与 Native 性能计数；其回归状态单列，不继承旧包 PASS。此前 `c64d3c85…7ea62` 的首次回归被锁屏阻止。

原桌面借用 `DirectBufferImportProbe` 并通过 `friend` 访问其私有状态，还重复实现了 NativeBuffer 导入。现已新增 `direct/direct_vulkan_context.{h,cpp}`：

| 所有者 | 管理的资源与状态 |
| --- | --- |
| `DirectVulkanContext` | Vulkan instance/device/queue、command pool、不可复制的提交资源、OHOS output surface/swapchain、输出 render pass/framebuffer、acquire/render semaphore，以及统一 NativeBuffer 图像导入 |
| `DirectVulkanImage` | 输入 VkImage/memory/view 与 NativeBuffer 引用；RAII 释放；调用方必须持有到最后一次 GPU 使用完成，context/device 必须更晚销毁 |
| `VulkanDesktopRenderer` | scene pipeline、sampler/descriptor、窗口层序、双槽 in-flight 状态、当前/退休图像和 UI staging；只通过 context 公开接口操作 Vulkan 资源 |
| `DirectBufferImportProbe` | 独立的单图 sample pipeline 与诊断 readback/像素核对；底层资源改用同一 context；每个在途槽持有被读取图像，避免清缓存提前释放 |

桌面不再包含 probe 头文件，也没有 `friend` 依赖。公共核心不包含诊断 shader、readback buffer、像素 map 或 upload 接口。桌面不再为借用核心而创建一套未使用的 probe 单图 graphics pipeline。SHM 桌面、标题栏和 GDI/ARGB UI 仍有独立 staging 上传，不能说整个 HAP 没有 CPU 上传。

context 与调用方均由单一渲染线程操作。初始化顺序为 `Initialize → InitializeSubmissions → InitializeOutput`，pipeline/descriptor 属于具体 consumer。output recreate 在资源边界 drain；普通帧只在提交槽复用时等待 VkFence，仍无 device/queue WaitIdle。重建或销毁前停止提交，释放输入图像/descriptor/pipeline，再由 context 销毁公共输出、提交资源及 device；不能让图像 RAII 对象活过其 device。

## 构建身份与验收

- signed HAP：`64cbd659505ba1c1c3d70c83cc9d7380a48b41abe1149206e1489f636783021f`，已安装到 USB MatePad Mini `5KPBB25818203996`。
- 完整 smoke payload：`smoke-v2-c87c4aa03bfc`，34 EXE；Wine/runtime 未在本轮改动。
- 主 HEAD `b53ca22fd1a7f8a96c2392f5931f51cd4436d70d`、Wine HEAD `4d3ec031c42a232e15a860274f743f756e047e8d`，均 dirty。源码文件 hash 与包身份见 [summary.json](evidence/direct-core-pad-20260929/summary.json)，证据 hash 见 [manifest.json](evidence/direct-core-pad-20260929/manifest.json)。
- HAP 构建/签名/candidate 验证、Python 语法编译和 `git diff --check` 通过；本次未重新跑无改动的 geometry/SHM 等宿主测试，不宣称完整 host suite 通过。

| 项目 | 结果与范围 | 证据 |
| --- | --- | --- |
| 双槽 NativeBuffer 导入、GPU 取样与输出 resize | PASS，8/8 帧；producer/consumer 槽峰值均 2；import 6、reuse 2；acquire import/release export 均 8；非负 release fd 7；输出 760×570→950×380，recreate 1 | [JSON](evidence/direct-core-pad-20260929/dual-slot-resize.json)、[对应 PID 53708 的日志](evidence/direct-core-pad-20260929/dual-slot-resize.log)。这是诊断 XComponent 的重建，不能代替生产桌面输出 resize/旋转验收 |
| 真实 Wine/DXVK Vulkan 虚拟桌面 scene | PASS，11 个阶段截图；App PID 55410 的 Vulkan renderer 已确认；A 2979 帧/5 次 rebuild、B 2713 帧/3 次 rebuild | [主结果](evidence/direct-core-pad-20260929/scene/device-results/direct-desktop-scene-x64.json)、[GPU 日志](evidence/direct-core-pad-20260929/scene/vulkan.log)、[全屏](evidence/direct-core-pad-20260929/scene/frames/scene-fullscreen.jpeg)、[恢复 700×450 客户区](evidence/direct-core-pad-20260929/scene/frames/scene-windowed.jpeg) |
| 真实触摸 | 2/2 PASS；全屏 `(640,400)` / 1280×800；窗口 `(350,225)` / 700×450 | [input-checks.json](evidence/direct-core-pad-20260929/scene/input-checks.json)，记录收到的设备 tick、client 尺寸与注入前阶段 |
| fusion 独立窗口，共用 context 的单图路径 | PASS，707 帧；marker 在左上，方向正确 | [结果](evidence/direct-core-pad-20260929/fusion/device-results/dxvk-direct-cube-x64.json)、[截图](evidence/direct-core-pad-20260929/fusion/live.jpeg) |

scene 最后一次定期日志样本是 presents 2880、directDraws 4956、acquireWaits 4550、releaseFences 4531、producerRetired 18、releaseErrors 0、imports 44、reuses 4506、UI upload 339046496 bytes。游戏图像 CPU read/upload 的零值为审查过的路径断言，不是硬件范围字节测量；这些功能样本不能作为性能 A/B 收益。

## 触摸采集时序修复

首次 `pad-core-vulkan-scene-20260929` 的 fixture 与画面通过，但窗口模式触摸只读到之前的全屏事件，输入验收为 1/2；[失败证据](evidence/direct-core-pad-20260929/input-timing-failure/input-checks.json)保留。fixture 的 windowed 阶段只停留 3 秒，原工具在 1.2 秒 settle 后先 screenCap/file recv 再点击，注入可能落到关闭窗口之后。

工具现在先核对当前阶段、点击并等待新 `WM_LBUTTONUP`，再截图；必须同时满足新设备 tick、准确 client 尺寸及坐标误差不超过 3 像素。阶段已变化或没有新事件仍报 FAIL，没有重复点击掩盖失败。复测 `pad-core-vulkan-input-order-20260929` 两次输入在观察阶段后约 1.94/2.08 秒完成，11 阶段与输入均通过。该修复只改宿主自动化工具，复测使用同一 HAP；没有改游戏或放宽标准。

重现沿用 [D5 命令](direct-vulkan-desktop-20260929.md#重现最终桌面验收)，换新的 run-id/归档路径。诊断入口是 `winehua.mode=direct-gpu-dual-slot-resize-probe`；JSON 在应用 `data/storage/el2/base/haps/entry/cache/`，需与本次 hilog 的 App/child PID 对照，不能直接采信旧 cache 文件。

## 剩余范围

本页截点的 fusion 单图 consumer 仍使用关闭诊断的 probe wrapper；后续独立 presenter 和生产 fusion 输出 resize 已完成，见上述记录。生产桌面输出 resize/旋转、popup/subsurface/viewport、异常 child 退出/device-lost、长期 RSS/fd 和真实游戏/性能 A/B 仍待覆盖；后续旧包的未经预热 scene 超时保留；启动 gate 修复后三轮进程/输入通过，两个完整画面矩阵通过，见上述启动记录。新包未重复 Venus 对照；上一包的 Venus PASS 仍是上一包证据，不能标成此次同包回归。

整体 64 位架构优化仍包含 OpenGL/WineD3D→Zink、Audio Direct、XInput shared state、能力选择与默认切换验证。这些没有因公共 Vulkan 核心拆出而完成。Steam/FEX 不随本轮切换，手机候选暂不执行。

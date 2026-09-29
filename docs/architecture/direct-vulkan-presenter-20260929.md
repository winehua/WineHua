# 平板 Direct Vulkan 生产 presenter（2026-09-29）

fusion 单图消费已从 `DirectBufferImportProbe(verifyPixels=false)` 拆为独立的 `DirectVulkanPresenter`，复用 [公共 Vulkan 核心](direct-vulkan-context-20260929.md)。当前生产桌面与 fusion 均不依赖诊断 probe；probe 仅供原生诊断入口使用。本轮按用户排序继续平板直通，手机验证搁置。

## 实际路径与归属

```text
x64 游戏 → FEX / ARM64 Wine → ARM64X DXVK → Guest 系统 Vulkan
  → OHOS swapchain / 共享 NativeBuffer + acquire SYNC_FD
  → HAP Vulkan 导入 / GPU 采样 → XComponent 输出 swapchain
  → release SYNC_FD 返回 BufferQueue
```

这里不转发每条 Vulkan 调用，也不回读、共享像素或重新上传游戏图像。窗口控制、producer parcel 和 GPU fence 交接仍有 IPC；不能称所有协议通信都被消除。SHM 桌面、GDI 和 ARGB UI 的 staging 上传仍单独存在。

| 所有者 | 当前职责 |
| --- | --- |
| `DirectVulkanContext` | instance/device/queue、提交资源、NativeBuffer 导入、OHOS 输出资源；无 readback/map/upload 接口 |
| `DirectVulkanPresenter` | 生产单图 sampler/descriptor/pipeline、双提交槽、图像缓存、fence 交接及输出重建；无诊断 shader/compute/readback 资源 |
| `DirectWineSurfaceController` | producer 队列和窗口生命周期；在 worker 调用 `Present` 并将 release fd 返回队列 |
| `DirectVulkanDesktopCompositor` | 多窗口 scene、层序/透明/全屏、共享图像和独立 SHM UI 上传，沿用公共核心 |
| `DirectBufferImportProbe` | 诊断取样/小型读回与像素核对；未删除诊断 API，产品调用点已移除 |

源码入口为 [presenter](../../entry/src/main/cpp/direct/direct_vulkan_presenter.cpp)、[controller](../../entry/src/main/cpp/direct/direct_wine_surface_controller.cpp) 和 [context](../../entry/src/main/cpp/direct/direct_vulkan_context.cpp)。

## 生命周期与同步改动

presenter 内部复用双槽，先等该槽上次 VkFence，再更新 descriptor/command buffer。每个在途槽持有输入图像，输入尺寸变化清理缓存时不会提前销毁 GPU 正在使用的资源。同尺寸 swapchain 反复重建的闲置导入缓存也限定为 16 项；在途引用另由提交槽保留。

相同输出 surface 的尺寸 revision 只使 output 失效，保留 device 和图像缓存；下一帧排空旧提交并重建输出。acquire 返回 `OUT_OF_DATE` 时最多重建并重试一次；present 返回 `OUT_OF_DATE/SUBOPTIMAL` 时保留已导出的 release fence，安排下一帧重建。普通帧没有 device/queue WaitIdle；重建、解绑、失败清理和销毁可 drain。绑定/解绑不同 surface 仍销毁 presenter，排空后才确认输出 revision，允许 ArkUI 销毁旧 XComponent。

acquire fd 的副本交给 Vulkan，原 fd 保留到 `vkQueueSubmit` 成功，避免 import 成功但 submit 失败时丢失 producer 同步。失败后 controller 先销毁/drain presenter；未消耗的 acquire fd 可直接作为 release fence 返回队列，不再用有超时的 CPU poll 后无条件返还 buffer。成功 export 返回 `-1` 仍是合法的已完成 fence，不能要求每帧都有非负 fd。

公共核心现在分别记录 device、submission 和 output 的完整就绪状态，部分初始化留下的 handle 不再等于成功。失败尝试不继续复用，调用方销毁重建；本轮未进行内存分配失败或 device-lost 故障注入。

## presenter 阶段构建身份与实机结果

- 本页 presenter 阶段 signed HAP SHA-256：`00b507b659659c3a5202568b20d09d26ffc0826405c0a59bbb63aa4ea483c5e6`。
- USB MatePad Mini，Maleoon 910。完整 payload `smoke-v2-c87c4aa03bfc`，34 EXE。Wine/runtime、Steam/FEX 和默认后端未在本轮修改。
- 主 HEAD `b53ca22fd1a7f8a96c2392f5931f51cd4436d70d`、Wine HEAD `4d3ec031c42a232e15a860274f743f756e047e8d`，均 dirty；保留既有改动，无批量提交或子模块更新。
- 详细结果及源码 hash 在 [summary.json](evidence/direct-presenter-pad-20260929/summary.json)，81 个归档文件的 hash 在 [manifest.json](evidence/direct-presenter-pad-20260929/manifest.json)。HAP 构建/签名/candidate 检查通过。

| 项目 | 00b507 包结果 | 证据 |
| --- | --- | --- |
| fusion 独立 Direct 上屏 | 设备/宿主 PASS，1,063 帧，angleRegressions=0，marker 左上且方向正确 | [结果](evidence/direct-presenter-pad-20260929/fusion/device-results/dxvk-direct-cube-x64.json)、[画面](evidence/direct-presenter-pad-20260929/fusion/frames/dxvk-direct-cube-x64-2.jpeg) |
| fusion 运行中 resize | PASS，1,057 帧；客户区 960×640→800×600，重建 1 次，之后 997 帧 | [结果](evidence/direct-presenter-pad-20260929/resize/device-results/dxvk-direct-resize-x64.json)、[画面](evidence/direct-presenter-pad-20260929/resize/frames/dxvk-direct-resize-x64-2.jpeg) |
| 生产 fusion 输出重建 | 同一 presenter 的输出 1936×1348→1616×1268，outputRecreates=1；device/import cache 保留 | [App PID 1058 日志](evidence/direct-presenter-pad-20260929/logs/DirectVkPresenter-fusion-resize.log) |
| 虚拟桌面单窗口 | 冷 App 启动，设备/宿主 PASS，586 帧 | [结果](evidence/direct-presenter-pad-20260929/virtual-cube/device-results/dxvk-direct-cube-x64.json) |
| 虚拟桌面双窗口 scene | **同一 App 在单窗口通过后** PASS，11 阶段截图；A 2,967 帧/5 rebuild，B 2,717 帧/3 rebuild | [主结果](evidence/direct-presenter-pad-20260929/scene/device-results/direct-desktop-scene-x64.json)、[重叠](evidence/direct-presenter-pad-20260929/scene/frames/scene-overlap.jpeg)、[透明 UI](evidence/direct-presenter-pad-20260929/scene/frames/scene-alpha-cover.jpeg) |
| 真实触摸 | 2/2 PASS，全屏 `(640,400)`/1280×800，窗口 `(350,225)`/700×450 | [输入记录](evidence/direct-presenter-pad-20260929/scene/input-checks.json)、[全屏](evidence/direct-presenter-pad-20260929/scene/frames/scene-fullscreen.jpeg)、[窗口](evidence/direct-presenter-pad-20260929/scene/frames/scene-windowed.jpeg) |

fusion 日志的 1,020 次提交样本：completed 1,018、imports 4、reuses 1,016、acquire imports/release exports 各 1,020。resize 样本为 imports 8、reuses 1,012、cache 4、outputRecreates 1。它们是定期样本，不是游戏的精确最终累计，也不构成性能 A/B。

App PID 9631 的桌面末次样本包括单窗口及 scene：presents 3480、draws 5559、acquireWaits 5153、releaseFences 5133、producerRetired 19、releaseErrors 0、imports 48、reuses 5105、UI upload 429606016 bytes。[日志](evidence/direct-presenter-pad-20260929/logs/DirectVkDesktop-scene-final.log)中的游戏 CPU read/upload 为代码路径断言，不是硬件流量测量。

## 失败与验收边界

首次 fusion 的 Windows 宿主 runner 用 GBK 读取 UTF-8 fixture JSON，启动后报 `UnicodeDecodeError`。设备完成 PASS，但没有宿主终态/截图；[原始结果与错误](evidence/direct-presenter-pad-20260929/host-encoding-failure/host-runner-failure.json)保留。随后使用 `python -X utf8`，新 run-id 完成宿主判定和截图；未改变 HAP 来处理此问题。

双窗口 fixture 两次未经预热直接启动均在 120000 ms 超时，未生成首个阶段文件：[同 App 重启 Wine 后的失败](evidence/direct-presenter-pad-20260929/scene-warm-timeout/host-summary.json)、[冷 App 失败](evidence/direct-presenter-pad-20260929/scene-cold-timeout/host-summary.json)。两次 Vulkan 桌面输出已创建，但没有游戏 scene；冷启动有 explorer 退出/early-fault 的 [有限 stderr](evidence/direct-presenter-pad-20260929/logs/scene-cold-stderr-tail.log)，其与挂起的因果关系尚未证明。不能把该故障仅归于热重启，也不能因后续预热 scene PASS 声称冷启动矩阵通过。

App 的正常“重启 Wine”按钮已确认发出 `state:stopped` 并清理旧 NCP；外部 sandbox 的 SIGTERM 被权限拒绝，没有终止子进程。随后 `aa force-stop` 后所测 UID 的进程列表为空，冷测试未复用旧 App 进程。过程见 [停止记录](evidence/direct-presenter-pad-20260929/logs/session-stop.log)和 [状态日志](evidence/direct-presenter-pad-20260929/logs/wine-scene-start.log)。

RSS 单次样本 178000 KiB、high water 200636 KiB；fd 枚举被拒绝，不能把命令输出的 0 当 fd 数。没有长期资源稳定性结论。本轮未重跑 Venus 对照、完整宿主测试、手机或真实游戏。设备临时熄屏设置 [已恢复](evidence/direct-presenter-pad-20260929/logs/screen-timeout-restored.log)。

## 复测与后续

所有宿主 Python 命令使用 `python -X utf8`。fusion 使用 `smoke.py run --suite dxvk-direct-cube --desktop-mode fusion --direct-ncp-session --seconds 12`，resize 使用 `--suite dxvk-direct-resize`，其余设备/prefix/run-id/archive 参数见各归档 `job.json`/`artifact.json`。

本页旧包只验证了 cube 预热后的 scene。后续 [显示启动 gate 修复](direct-desktop-startup-20260929.md) 在 `f5dbf252…df23` 包上完成三轮未经预热 scene 启动，宿主/设备和输入均通过；第一、三轮画面完整，第二轮 windowed 漏拍明确保留。复测使用 D5 命令与新 run-id，无需先跑 cube。保留 `--probe-input` 和 `WINEHUA_D3D_INPUT_TRACE=1`。

补齐启动 gate 后，接下来补 popup/subsurface/viewport、生产桌面输出 resize/旋转、child/device-lost/fence 错误注入、长期 RSS/fd 与真实游戏/同设备性能 A/B。性能收益尚未量化，下一验收门槛见 [同设备 Direct/Venus 性能方案](direct-performance-gate-20260929.md)。整体计划中的 Zink、Audio Direct、XInput 和能力/默认选择仍未完成；此次完成的是 Vulkan 生产 consumer 的独立实现及上述范围验收。

# Mate 80 Direct 反向导出验证与手机路线决定

日期：2026-09-29 晚。设备：无线 Mate 80（VYG-AL00），系统自报 `OpenHarmony-7.0.0.105`；本次没有操作 USB 平板。

## 当前决定

用户在获得失败阶段后明确表示：如果实现较困难，手机继续原来的策略。因此手机保留 **Venus + 现有显示路径**，Direct 上屏不启用；平板 Direct 的既有实现、验收与 opt-in 边界继续保留。没有修改 Steam、FEX、引擎默认值或子模块版本，也没有引入 CPU 图像回读/上传的呈现回退。

本轮没有解决手机 Direct。新增反向 P0 仅用于保留可复现证据，不接入正常游戏启动。后续若恢复手机 Direct，应先验证可用的系统服务/进程上下文，再处理图像与 WSI，不能重复更换 DXVK 或传递参数。

## 本轮真正验证了什么

此前 App 分配 NativeBuffer 后，fork child 在 `OH_NativeBuffer_ReadFromParcel` 的注册阶段等待 HDI/SAMGR，尚未进入 Vulkan import。本轮实现设计文档 §10 的反向实验：

```text
Guest：系统 Vulkan → 可导出 VkImage → dedicated memory
  → vkGetMemoryNativeBufferOHOS → metadata + SCM_RIGHTS
App：NativeBuffer decode/import → GPU sample
双方：render-done / release SYNC_FD
```

仍为 64×64、单槽四帧的独立探针；allocation/export、接收/导入、实际非负 fence fd 与已完成的 `-1` fence 分别计数。只有完成四帧才有资格讨论图像共享成功；本次实际为 **0 帧**。

入口：`winehua.mode=direct-phone-shared-buffer-probe` 加 `winehua.shared-buffer-guest-export=1`。未指定参数仍运行原 App→child P0。wire 版本升至 5，父/子同包更新；Linux socket 测试通过（fd 别名、CLOEXEC、400 个非法包、无 fd 泄漏）。

## 实机结果

已安装、实测的 HAP：

- SHA-256：`c56eb3a8012157ccf52a7590a43fec5c0e84982c0278c51b6aa66110ae45da04`。
- 大小：350,926,386 字节。
- 相对平板启动锁修复基线 `4b92988e…a287f`，仅变更 `libentry.so`、`libdirect_shared_buffer_probe.so` 和 ArkTS 诊断入口；Wine/DXVK 载荷不变。
- parent PID=35008，fork server=35313，child=35331。
- 系统 loader=`/system/lib64/libvulkan.so`，GPU=`Maleoon 920`，device 创建成功。
- 最后阶段=`writer_export_allocate`，15 秒超时。父探针发送 SIGKILL 并观察回收；不是 GPU/系统主动崩溃。JSON 中 `vkResult=0` 是尚无失败返回值，不表示 allocation 成功。

HiDebug 的实际等待栈：

```text
vkAllocateMemory
  /vendor/lib64/passthrough/libmaleoon_v200.so
  OH_NativeBuffer_Alloc
  SurfaceBufferImpl::Alloc
  IDisplayBuffer::Get
  IAllocator::Get
  HDI::ServiceManager::Get
  SystemAbilityManagerProxy::Recompute / GetSystemAbilityWrapper
  usleep
```

**结论：反向导出无法绕过当前服务获取门槛。** 驱动在可共享图像的 Vulkan allocation 内部仍调用 NativeBuffer 分配，随后进入与旧接收路径相同的 HDI/SAMGR 等待。尚未调用 `vkGetMemoryNativeBufferOHOS`，也未发生跨进程 GPU 图像或 fence 交接。不能把此结果说成手机没有 Vulkan，或简单归为某个 export API 不支持。

首版探针把普通图像的非零 allocation size 当成前置条件，在 `writer_export_memory_type` 自行拒绝。这不是驱动拒绝。第二版记录到 `size=0, memoryTypeBits=3, memoryTypeCount=4`，保留驱动提供的 size 与类型约束后进入真实 `vkAllocateMemory`，才得到上述系统等待栈。两轮结果分别保留，不能混算。

## 根因边界与未采用的实验

当前证据定位到 fork child 的系统图形服务上下文。OpenHarmony 上游 BinderConnector 源码存在缓存 driver fd 的单例，而当前早期 fork server 会清理继承 fd，因此失效的继承 IPC 状态是一个合理假设；**未获得设备 Binder errno 或重连对照，尚不能把它写成已证明的唯一根因，也没有明确的权限拒绝日志。** 上游源码不是手机闭源固件的逐字节依据。

曾编译一个内部 BinderConnector 符号审计/重连实验，但用户决定保留手机旧路线后已移除，未装机、未调用，也未接入 Wine。最终源码不包含这个内部接口方案。尝试由宿主 shell 执行系统 musl loader 返回 Permission denied，同样不能推断 App 上下文的 exec 结果；没有继续做重启进程/内部 ABI 绕路。

能继续研究的方向是系统支持的子进程入口或完整的新进程服务初始化；这些需要单独验证，并非现有窗口代码的小修。此前系统 NCP 返回 `16010004` 的记录仍有效。手机暂不承担这部分架构复杂度。

## 证据与复现边界

- [反向 P0 的实际结果及等待栈](evidence/mate80-direct-reverse-p0-20260929/reverse-v2/result.json)
- [首版探针前置检查结果](evidence/mate80-direct-reverse-p0-20260929/reverse-v1/result.json)
- [child 的系统日志](evidence/mate80-direct-reverse-p0-20260929/reverse-v2/child-hilog.txt)
- [装机包身份](evidence/mate80-direct-reverse-p0-20260929/candidate-v2.json)
- [归档哈希清单](evidence/mate80-direct-reverse-p0-20260929/manifest.json)

原始宿主目录：`F:/WineHua/.temp/mate80-direct-reverse-p0-20260929/`。最终整理仅改善诊断阶段计数/排版；signed HAP `1e85b09941fe653a342af9af926f7687bfc43023f4b65b10ef36311c98fab551` 编译和 runtime closure 检查通过，**未装机**，设备保持上述实测 c56 包。详见 [最终构建身份](evidence/mate80-direct-reverse-p0-20260929/final-build-only.json)。

## 手机原路线回归

在 c56 实测包冷启动，使用当前完整 `build/smoke-performance-payload`，运行 8 秒 x64 D3D11 立方体：

- Venus + DXVK 2.6.2 / ARM64X DLL；fusion + EGL。
- 显式覆盖 `WINEHUA_VULKAN_BACKEND=venus`、`WINEHUA_DIRECT_NCP=0`、`WINEHUA_PHONE_DIRECT_FORK=0`。沿用 `dxvk-direct-cube` fixture 的名称不代表这次走 Direct，实际参数见 [job.json](evidence/mate80-direct-reverse-p0-20260929/venus26-regression/job.json)。
- **871 帧**，renderSequence=871，angleRegressions=0，初始化/Present HRESULT 均为 0。
- 设备结果和宿主结果/视觉检查均 **PASS (1/1)**；采集四张截图，其中一张已人工核对彩色立方体。不是完整 API 矩阵、真实游戏或性能对比结论。
- [设备结果](evidence/mate80-direct-reverse-p0-20260929/venus26-regression/device-results/dxvk-direct-cube-x64.json)、[宿主判定](evidence/mate80-direct-reverse-p0-20260929/venus26-regression/host-summary.json)、[画面](evidence/mate80-direct-reverse-p0-20260929/venus26-regression/frames/dxvk-direct-cube-x64.jpeg)。

第一次回归 Want 发到仍在诊断页的 App（PID=35008），没有建立 Wine 会话/测试结果，已取消宿主等待并另起明确冷启动，记录为 [NOT_RUN](evidence/mate80-direct-reverse-p0-20260929/venus26-first-launch-cancelled.json)，不算渲染失败或成功。冷启动 runId=`mate80-retain-venus26-cold-20260929` 才是上述有效结果。

测试结束自动 force-stop App，UID=20020471 的进程残留为零。本轮未修改系统熄屏超时。手机正常启动继续原方案；Direct 后续研究暂停。

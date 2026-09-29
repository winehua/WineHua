# 平板 Direct Vulkan 桌面合成（2026-09-29）

本轮按用户要求搁置手机共享图像验证，在 USB MatePad Mini `5KPBB25818203996` 上推进 D5。Steam、FEX 和默认图形后端没有随本轮切换。

后续已完成 [公共 Vulkan 资源核心拆分及同包复测](direct-vulkan-context-20260929.md)和 [生产 presenter 独立实现](direct-vulkan-presenter-20260929.md)，该阶段 HAP 为 `00b507b6…3c5e6`。后续 [启动 gate 修复与三轮冷 App 结果](direct-desktop-startup-20260929.md) 使用 `f5dbf252…df23`；本页 `65203256…6414c` 和公共核心页 `64cbd659…3021f` 的结果分别保留，不混作最新包证据；00b507 包未经预热直接启动 scene 的两次超时也单列保留。

## 实际路径

当前安装的后续 [性能基准候选](direct-performance-preparation-20260929.md) 为 `b741c40f…9f0d`，包含 viewport 采样与 Native 性能计数；构建/签名通过，回归与性能结果单列。此前 `c64d3c85…7ea62` 的首次回归被锁屏阻止，尚不计入显示验收。

```text
x64 游戏 → FEX / ARM64 Wine → ARM64X DXVK → 系统 Vulkan
  → Wine Create NCP 的 OHOS swapchain → 共享 NativeBuffer / BufferQueue
  → HAP Vulkan 导入缓存 + acquire fence GPU wait
  → 按 Wayland 窗口树合成 → XComponent Vulkan swapchain
  ← release SYNC_FD + FOREIGN ownership barrier
```

Direct 路径不传 Venus/vtest Vulkan 命令。游戏 NativeBuffer 不做 CPU map、readback 或 upload；游戏关闭前保留当前帧用于窗口移动/遮挡重绘，换帧与销毁时才归还。标题栏、桌面、GDI/ARGB UI 仍来自 SHM，独立纹理上传计入 `uiUploadBytes`，不能与游戏图像合并称为“整个桌面没有上传”。HAP 最终合成已使用 Vulkan，早期 EGL bridge 只保留作为对照。

## 代码落点和运行边界

- `direct/direct_vulkan_desktop_compositor.cpp`：独占输出窗口，两个提交槽，NativeBuffer 按 sequence 缓存导入，GPU semaphore 等待/释放，提交 fence 保持资源存活。正常帧不调用 device/queue WaitIdle；输出重建、退出和异常清理允许 drain。
- `DesktopCompositor::SnapshotGpuDesktopScene()`：同一窗口锁下取得几何、层序、可见性和 SHM serial；Direct 客户区按 `(pid, wlSurfaceId, toplevelId)` 找父窗口及唯一 viewport subsurface，避免重复画 SHM 客户区。GDI 和 premultiplied ARGB 仍按层序覆盖 Direct。
- 渲染和输入共用 `ComputeFullscreenFitLocked()`；Direct 客户区尺寸登记只在当前 snapshot 有效，退出时清理。
- `winehua.desktop_renderer=vulkan` 显式启用虚拟桌面 Vulkan 合成；默认仍为 EGL。fusion 独立窗口沿用已有 Direct consumer。Venus 对照必须选择 EGL，尚未把旧 Venus ZC consumer 接入新的 Vulkan scene renderer。
- 桌面和 fusion 产品输出使用设备支持的 `IDENTITY` preTransform，修复首版整桌面旋转 90°与独立窗口方向错误；能力不支持时明确失败，没有软件旋转/回读 fallback。诊断探针默认变换策略保留，产品调用显式选择 identity。
- 静态窗口树且没有新 Direct buffer、退休 buffer 或输出 resize 时停止重复 present。
- `41210000` 是 `NATIVE_ERROR_BUFFER_NOT_IN_CACHE`，单列 `producerRetired`；其它返回值计入 `releaseErrors`。系统 Release 调用接管 fence fd，调用后不再次 close。

初版 Vulkan renderer 通过 `friend` 复用了 `DirectBufferImportProbe` 的 device/import/output 核心并关闭诊断读回。后续已拆出独立 `DirectVulkanContext`，桌面改用公开资源接口，NativeBuffer 导入统一；fusion 也迁到独立 `DirectVulkanPresenter`。详见上述记录；完整生产生命周期仍待覆盖。

## 全屏退出修复

双窗口 Win32 fixture 返回 PASS 不代表画面正确。初次 identity 构建中，退出全屏后客户区已经恢复 700×450，实机截图却仍铺满，Wayland 日志只有 `tl_set_fullscreen`。延迟截图和补齐 fixture 的 `SWP_FRAMECHANGED` 均没有消除问题。

根因是 `WAYLAND_WindowPosChanged()` 的 client-only 保留父窗口分支更新了 `data->rects/is_fullscreen`，却没有像 GDI 分支一样更新 `wayland_surface->window`。随后比较 xdg 状态一直用旧 fullscreen 配置。修复在同一 `win_data_mutex` 下调用 `wayland_win_data_get_config()`，再更新 Wayland 状态；没有增加游戏图像 SHM 数据。补丁为 `patches/wine/0007-client-only-window-state.patch`，构建入口幂等应用。

## 实机验收与限制

最终包已安装到 USB MatePad Mini（Maleoon 910）。本轮 D5 的 DXVK 1.10.3 多窗口 fixture 已通过，Direct 成品图像经共享 NativeBuffer 进入 HAP Vulkan compositor，实际输出和触摸均有独立证据。仍是 opt-in 功能，不能把这一结果视为整个 ARM64 重构计划或所有游戏验收完成。

构建身份：

- signed HAP SHA-256：`65203256f2c52de7cb88de4c355bb846630e39c18581efca6c9495caf946414c`。
- 完整 34 EXE payload：`smoke-v2-c87c4aa03bfc`。
- 主仓库 HEAD：`b53ca22fd1a7f8a96c2392f5931f51cd4436d70d`；Wine HEAD：`4d3ec031c42a232e15a860274f743f756e047e8d`；两者均有未提交改动，HEAD 不能单独重现该包。未批量提交、更新子模块或清理工作区。
- 原始 Windows runner 的 `artifact.json` 中 `wineCommit` 为空；[summary.json](evidence/direct-d5-pad-20260929/summary.json) 补充了 WSL git 核对的 HEAD/dirty 状态。各证据文件 SHA-256 和对应 HAP 见 [manifest.json](evidence/direct-d5-pad-20260929/manifest.json)。

最终包证据位于 `evidence/direct-d5-pad-20260929/final-65203256/`。该目录外原有 scene/resize 文件属于上一包 `1d75701f98a09a44da62b7e88e531667bcf4b2287e48d1e3b62a31ce34ab7626`，不标成最终包测试。

| 验收项 | 最终包结果 | 证据与范围 |
| --- | --- | --- |
| Vulkan 虚拟桌面，双进程 DXVK 窗口 | PASS，12 次窗口操作，11/11 阶段截图 | [fixture](evidence/direct-d5-pad-20260929/final-65203256/scene/device-results/direct-desktop-scene-x64.json)、[App Vulkan 启用与计数](evidence/direct-d5-pad-20260929/final-65203256/scene/vulkan-start.log)；当前 App PID `40911`，2560×1600 输出，双槽，identity |
| 重叠/移动、GDI/ARGB 遮挡、最小化/恢复、关闭 B | 画面通过 | [双窗口](evidence/direct-d5-pad-20260929/final-65203256/scene/frames/scene-overlap.jpeg)、[GDI](evidence/direct-d5-pad-20260929/final-65203256/scene/frames/scene-gdi-cover.jpeg)、[透明/50%/不透明三段](evidence/direct-d5-pad-20260929/final-65203256/scene/frames/scene-alpha-cover.jpeg)、[恢复](evidence/direct-d5-pad-20260929/final-65203256/scene/frames/scene-restore.jpeg)；最小化后客户图像消失，仍有 Wine 的最小化 caption |
| 客户区 resize、进入/退出全屏 | PASS | A 共 2967 帧，5 次 swapchain rebuild，resize 后 2907 帧；B 共 2710 帧，3 次 rebuild，resize 后 2650 帧；均无 angle regression。[全屏](evidence/direct-d5-pad-20260929/final-65203256/scene/frames/scene-fullscreen.jpeg)返回[700×450 客户区](evidence/direct-d5-pad-20260929/final-65203256/scene/frames/scene-windowed.jpeg)，[xdg 日志](evidence/direct-d5-pad-20260929/final-65203256/scene/fullscreen.log)包含 `tl_unset_fullscreen → configure(708,484)` |
| 真实触摸 | 2/2 PASS | [input-checks.json](evidence/direct-d5-pad-20260929/final-65203256/scene/input-checks.json)：全屏收到 `(640,400)` / client 1280×800；窗口模式收到 `(350,225)` / client 700×450 |
| 同一 Vulkan 桌面会话再次运行 resize | 功能 PASS | [593 帧，960×640→800×600，resize 后 533 帧，rebuild 1](evidence/direct-d5-pad-20260929/final-65203256/resize/device-results/dxvk-direct-resize-x64.json)，[GPU 日志](evidence/direct-d5-pad-20260929/final-65203256/resize/vulkan-resize.log)继续呈现。该短测试自动截图显示前台管理页，不能计为画面验收；视觉 resize 由前项 scene 提供 |
| fusion 独立 Direct 窗口 | PASS，707 帧，方向正确 | [结果](evidence/direct-d5-pad-20260929/final-65203256/fusion/device-results/dxvk-direct-cube-x64.json)、[截图](evidence/direct-d5-pad-20260929/final-65203256/fusion/live.jpeg)中 marker 在左上；修复前的方向错误截图仅作历史对照 |
| Create NCP + Venus/EGL 对照 | PASS，604 帧 | [运行参数](evidence/direct-d5-pad-20260929/final-65203256/venus-create/job.json)、[结果](evidence/direct-d5-pad-20260929/final-65203256/venus-create/device-results/dxvk-direct-cube-x64.json)、[截图](evidence/direct-d5-pad-20260929/final-65203256/venus-create/live.jpeg)。不覆盖此前 Start NCP 的 180000 ms timeout；[失败原始判定](evidence/direct-d5-pad-20260929/previous-1d75701f-venus-start-timeout/host-summary.json)保留 |

scene 末次定期 GPU 日志样本（非精确退出累计）为：presents 2880、directDraws 4968、acquireWaits 4559、releaseFences 4539、producerRetired 18、releaseErrors 0、imports 44、reuses 4515。UI 上传独立计为 341524256 字节。日志里的 `gameCpuReadBytes=0 / gameCpuUploadBytes=0` 是已审查代码路径的断言，非系统范围的字节测量；NativeBuffer 只经 Vulkan import/sample，像素读回关闭，`vkMapMemory/memcpy` 仅用于 SHM UI 上传。不能据这些样本或 cube FPS 宣称真实游戏性能收益。

fixture JSON 只验证 Win32 操作与两个子进程的结果，实际画面必须与阶段截图、App GPU/fence 日志一起判断。`pad-final-vulkan-scene-20260929` 曾在现有 PID `40095` 上请求 renderer 切换；进程沿用 EGL，虽然 fixture/输入通过，也不计入 Vulkan 验收。该次参数和结果保留在 `final-65203256/reused-egl-excluded/`。有效验收使用重启后的 `pad-final-vulkan-cold-scene-20260929`，并核对当前 PID 的 `Vulkan desktop enabled`；这里的 cold 指 App 重启且复用 Wine prefix，不代表重新安装冷启动。

触控检查使用 `WINEHUA_D3D_INPUT_TRACE=1` 的 cube sidecar，记录真正收到的 `WM_LBUTTONUP`；宿主通过 `uitest uiInput click` 发送实际触摸。截图工具的 `--probe-input` 只适用于本轮 1280×800 逻辑桌面、2560×1600 平板输出，不能直接套到其它尺寸设备。默认 fixture 不依赖用户点击。

## 重现最终桌面验收

在 `F:\WineHua\proton-ohos-worktree` 运行。切换 fusion/virtual 或 EGL/Vulkan 之前需重启测试 App 会话；已运行会话的 Want override 不能证明实际 renderer 已改变。对仍有 Wine 子进程的会话应先走 App 正常停止流程，避免直接 force-stop 留下失去登记的 NCP；本轮 fixture 的游戏子进程已结束。先启动截图观察器，再在另一终端启动唯一 smoke runner：

```powershell
hdc -t 5KPBB25818203996 shell aa force-stop app.hackeris.winehua
python automation/capture_direct_desktop_scene.py `
  --run-id pad-final-vulkan-cold-scene-20260929 --device 5KPBB25818203996 `
  --archive F:/WineHua/.temp/direct-pad-20260929/scene-vulkan-final-cold `
  --renderer vulkan --settle-seconds 1.2 --probe-input --timeout 300
```

```powershell
python automation/smoke.py run --suite direct-desktop-scene --prefix reuse `
  --device 5KPBB25818203996 --run-id pad-final-vulkan-cold-scene-20260929 `
  --archive-root F:/WineHua/.temp/direct-pad-20260929 `
  --desktop-mode virtual --desktop-renderer vulkan --direct-ncp-session `
  --env WINEHUA_D3D_INPUT_TRACE=1 --timeout-minutes 4 `
  --keep-app --poll-seconds 2 --skip-push
```

重跑时换新的 run-id/归档路径，避免旧结果被当成当前结果；设备须已有对应 payload，否则先 push。套件自带 `WINEHUA_DIRECT_NCP=1` 和 `WINEHUA_VULKAN_BACKEND=direct`。使用 `pidof` 与有界 `hilog -T DirectVkDesktop` 核对当前 PID 的 renderer 和帧计数。同一 Vulkan 会话继续跑 `--suite dxvk-direct-resize`；Venus 对照需先重启、改选 `--desktop-renderer egl`，并加 `--env WINEHUA_DIRECT_NCP=1 --env WINEHUA_VULKAN_BACKEND=venus`。

## 检查状态与下一阶段

Wine、完整 smoke payload、最终 HAP 构建/签名/candidate 校验已通过；Python 语法编译和主树/Wine `git diff --check` 通过。此前 5 个相关 compositor 测试共 605 checks 通过。完整 `make test` 仍被既有 `toplevel_event_test` 对已删除 `ArgbMove/MaskDirty/JsonArgbCreated/JsonArgbMove` 的引用阻止，不能报告全 suite 通过。设备临时 10 分钟熄屏设置已恢复。

后续平板 Direct 工作按此顺序继续：公共 context、独立 fusion presenter 和 fusion 输出 resize 已完成；smoke 显示启动就绪等待已补齐，继续扩展 popup/subsurface/viewport、生产桌面输出 resize/旋转、异常 child 退出/device-lost 和长期 RSS/fd 的覆盖，最后做真实游戏与同设备同场景性能 A/B，再决定默认切换。scene 的客户区 resize 和 fusion 的输出 resize 均不能替代生产桌面输出窗口 resize 验收。

性能收益仍待 [同设备 Direct/Venus A/B](direct-performance-gate-20260929.md) 量化。安装后运行库更新与 wineboot 冷启动曾延迟/超时，不能将复用 prefix 的成功当作安装首启稳定。手机 P0 仍未通过，遵用户要求搁置；后续 Zink、Audio Direct 和 XInput 也未完成。

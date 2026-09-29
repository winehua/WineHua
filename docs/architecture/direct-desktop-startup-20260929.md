# Direct 桌面启动就绪与冷 App 验证（2026-09-29）

显示 smoke 用例现在与产品启动程序一样，在 spawn 前调用 `WineEnvService.ensureDesktop()`，等待 desktop surface ready。未就绪会返回 `desktop-startup`，不继续启动游戏。没有固定延迟、cube 预热或自动重试。

`automation/smoke.py` 为有固定画面检查且非 offscreen 的用例生成 `requiresDesktop=true`；suite 可显式覆盖。双窗口 scene 没有自动画面配置，因此在 suite 中显式声明。`SmokeTypes` 与 runner 接入这一字段。

本阶段装机 signed HAP SHA-256 为 `f5dbf2521faa102a36427d32dfdc790baf09357c6b9ecf2fa7f06db454e1df23`，payload 为 `smoke-v2-df9a09211262`。34 个 EXE 与前包字节完全一致，只刷新 suite/manifest。HAP 中 Wine/runtime manifest 未变；没有切换 Steam/FEX、更新 Wine 或重置 prefix。前包 presenter/fusion 的结果仍只属于 [00b507 包记录](direct-vulkan-presenter-20260929.md)。当前平板已更新为 [性能基准候选](direct-performance-preparation-20260929.md) `b741c40f…9f0d`，本页三轮证据继续只属于 f5dbf252 阶段。

三轮均先 force-stop 并确认 App UID 的进程列表为空，然后直接运行双窗口 scene，复用既有 prefix；没有 cube 预热和 stall dump。每轮只有一个 smoke runner 和一个截图观察器。

| 轮次 / App PID | surface 等待 | A / B 帧数 | A / B swapchain rebuild | 设备与宿主 / 输入 |
| --- | --- | --- | --- | --- |
| 1 / 23129 | 335 ms | 3020 / 2723 | 5 / 3 | PASS / 2 次 PASS |
| 2 / 24007 | 151 ms | 2856 / 2636 | 5 / 3 | PASS / 2 次 PASS |
| 3 / 24745 | 358 ms | 2968 / 2699 | 5 / 3 | PASS / 2 次 PASS |

三轮开始均记录 `active=true surfaceReady=false`，就绪后才 spawn。六个 child 的 `angleRegressions=0`，resize/present HRESULT 为成功。各自 App PID 日志确认实际启用 Vulkan desktop，双槽并以共享 GPU 图像合成。

后续 PE header 核对确认本轮 cube/scene 为 ARM64 `0xaa64`，路径与 JSON 的 `x64` 是 64 位槽标签，不能作为真实 AMD64/FEX 覆盖。实际 AMD64 基准已另行 [显式构建](direct-performance-preparation-20260929.md)。

33 张阶段截图已通过 contact sheet 核查。第一、三轮的 11 阶段画面完整；第二轮 `windowed` 截图已落到窗口关闭后的桌面，不能用它验证窗口恢复画面。该轮新鲜 tick、700×450 extent 与 `(350,225)` 触摸仍通过。观察器的返回码只证明生成了文件与输入检查，不能代替画面核查。

原始结果、截图、PID 过滤日志、payload 身份和熄屏恢复记录共 87 文件，见 [summary.json](evidence/direct-startup-pad-20260929/summary.json) 与 [manifest.json](evidence/direct-startup-pad-20260929/manifest.json)。旧包两次 120 秒超时 [继续保留](evidence/direct-presenter-pad-20260929/scene-cold-timeout/host-summary.json)。旧包开启 stall dump 后未重现失败，未取得挂起时的 Wine 等待栈；因此结论限于补齐显示启动 gate 及三轮未经预热启动通过，不能声称已经找到 Wine 内部的具体挂起函数。

复测可直接运行 [D5 的 scene 命令](direct-vulkan-desktop-20260929.md#重现最终桌面验收)，使用新的 run-id、`python -X utf8`、`--skip-push`（设备须已有匹配 payload），无需先跑 cube。捕获时间短的阶段应核对 phase 与截图时间。

本包未重跑 fusion、Venus、完整宿主 suite、手机或真实游戏；复用 prefix 的 App 冷启动不代表新安装首启稳定。后续仍需 viewport/subsurface/popup、生产桌面 output resize/旋转、异常退出与 fence/device-lost、长期资源稳定性和性能 A/B。Zink、Audio Direct、XInput 与能力/default 选择仍在整体计划内。

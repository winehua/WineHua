# 完整锁修复的实机验证（2026-09-29）

第二版在 USB MatePad Mini 上完成关闭采样器的 **12/12 冷启动完整负载**，Venus、Direct 各 6 轮。未重现此前 Explorer 启动挂起。这与完整 caller/callee 的确定性反例共同验证了本次修复；不等于所有窗口/游戏或长期稳定性验收已完成。

## 根因与修复依据

实机捕获的环是窗口线程持 USER 等 surface、刷新线程持 surface/Wayland 窗口锁等 USER，游戏因桌面消息无法回复而等待。第一版只释放内层递归 USER，仍保留外层锁，故 12 次诊断短测通过不能证明它修好。

第二版把 region 更新移到 caller 的外层 USER 完全释放之后，并持独立 surface 引用保护生命周期。原版/第一版在真实函数宿主测试中分别留下 USER 深度 2/1 并稳定死锁，第二版深度为 0；18 项定向检查及两个负对照通过。详见 [完整锁审查](direct-startup-lock-audit-20260929.md)。本轮未重跑已有 `toplevel_event_test` 编译失败的全量 `make test`，不报告全量通过。

## 同包测量条件

- signed HAP：`4b92988e0a5a569ab26d313a605ffbc41012be369a1147efa3ed7b758f7a287f`，350923640 bytes；相对第一版仅 `win32u.so` 改变，另外 124 个 native/rawfile 条目字节相同。
- 载荷 `smoke-v2-474c8f475ecd`，真实 AMD64/FEX，100 draws、小 scissor、目标 60 FPS；每轮 75 秒，前 15 秒预热、后约 60 秒计量。
- 顺序 `(Venus, Direct, Direct, Venus) × 3`。每轮 force-stop 后确认 App UID 无剩余进程；保持相同 prefix/载荷。未启用游戏或桌面栈采样器，未在计量中截图。
- 12 轮均通过独立 raw frame CSV / JSON / HAP 计数核对，桌面结束时均在前台，记录中未见 DesktopAbility 后台事件。
- 前后环境记录的 Battery 为 33000（服务原始温度单位），system_h 为 35842–36555。显示结束快照相同；启动前 launcher 的一个 RefreshRate 字段为 120、应用内为 90，同时 activeModes/另一个字段为 60，不能把这些 dump 数值当成实际物理扫描帧率。

## 全部 12 个样本

| 轮次 | 后端 | accepted/s | P99 ms | 两进程 CPU ms/accepted |
| --- | --- | ---: | ---: | ---: |
| 1 | venus | 59.035 | 22.835 | 13.544 |
| 2 | direct | 59.521 | 20.359 | 12.817 |
| 3 | direct | 59.553 | 19.774 | 12.979 |
| 4 | venus | 59.034 | 22.376 | 13.713 |
| 5 | venus | 59.142 | 21.661 | 13.725 |
| 6 | direct | 59.534 | 19.888 | 13.056 |
| 7 | direct | 59.965 | 20.133 | 12.851 |
| 8 | venus | 59.440 | 21.849 | 13.671 |
| 9 | venus | 58.915 | 22.108 | 13.424 |
| 10 | direct | 59.580 | 20.179 | 12.915 |
| 11 | direct | 59.506 | 19.723 | 12.979 |
| 12 | venus | 59.318 | 21.875 | 13.464 |


中位数：

| 指标 | Venus | Direct |
| --- | ---: | ---: |
| HAP accepted present/s | 59.089 | 59.544 |
| 游戏帧 P95 ms | 18.612 | 18.252 |
| 游戏帧 P99 ms | 21.992 | 20.010 |
| 游戏 CPU ms/游戏帧 | 5.394 | 6.961 |
| HAP CPU ms/游戏帧（估算） | 8.013 | 5.877 |
| 两进程 CPU ms/accepted present（估算） | 13.607 | 12.947 |

本组 Direct 两进程 CPU 成本中位数变化 **-4.85%**；三个 ABBA block 的平均成本变化分别为 -5.36%、-5.43%、-3.70%。这是该固定负载内的有限 CPU 改善，未达到原定约 15% CPU 成本下降的扩大默认覆盖门槛，也没有证明真实游戏提速。Direct 继续为可选路径。

CPU 仅包含游戏进程与 HAP，按各自观察时长归一，再除以 HAP accepted present/s；不包括 wineserver、系统服务或 GPU 能耗。accepted present 不等于物理扫描 FPS。旧包曾观测到 Direct CPU 成本高约 4.8%，其样本和这组不能混算，也不能把跨包/跨时段差异全部归因于这次锁修复。

本轮没有改变 Vulkan 渲染/共享 NativeBuffer 数据路径，没有加入游戏画面 CPU 回读。接下来性能验收仍需 commands/fill、匹配输出调度条件、真实游戏固定片段与长测。

## 运行中缩放回归

同一 HAP 再次冷启动 `dxvk-direct-resize`，明确使用 Direct/Vulkan 桌面、`--no-start-retry`、相同 prefix/载荷。结果为 878 帧，960×640→800×600，`ResizeBuffers` HRESULT=0，重建 swapchain 1 次，resize 后继续呈现 818 帧，角度回退 0。设备结果和宿主判定均 PASS，4 张实机截图通过立方体判定；人工检查首张可见正确的窗口边框、彩色立方体和桌面背景。

这是 ARM64X legacy DXVK 的生命周期回归，不与 AMD64/FEX 性能样本混算。测试结束时设备为 AWAKE，系统熄屏设置恢复 600000 ms。没有重启全局 HDC。

[resize 结果](evidence/direct-startup-wait-pad-20260929/runs/direct-surface-complete-20260929/resize/dxvk-direct-resize-surface-lock-v2-resize-20260929/device-results/dxvk-direct-resize-x64.json) · [实机截图](evidence/direct-startup-wait-pad-20260929/runs/direct-surface-complete-20260929/resize/dxvk-direct-resize-surface-lock-v2-resize-20260929/frames/dxvk-direct-resize-x64.jpeg)

## 可核对产物

- [完整测量与区间](evidence/direct-startup-wait-pad-20260929/runs/direct-surface-complete-20260929/perf-fixed60/review.json)
- [每轮结果和环境记录](evidence/direct-startup-wait-pad-20260929/runs/direct-surface-complete-20260929/perf-fixed60/outcomes.json)
- [测试计划及包身份](evidence/direct-startup-wait-pad-20260929/runs/direct-surface-complete-20260929/perf-fixed60/plan.json)
- [证据 manifest](evidence/direct-startup-wait-pad-20260929/manifest.json)（包含 raw CSV、版本源码与第一版正式失败）

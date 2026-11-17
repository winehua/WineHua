# WineHua Mini 本地功能验证（2026-11-17）

分支 `feature/vpp-runtime`，基于 WineHua `master@40b74771`。独立安装 `app.hackeris.winehua` 1.0.13（1000013），旧柚 Pro 的数据未改动。未推送、未合主分支、未上架。

## 本轮结果

| 项目 | 结果 |
| --- | --- |
| WineHua 原 UI 启动、Wine 初始化、桌面及重入 | 通过；首次初始化完成标记及桌面画面已观察 |
| Win32 窗口创建、几何往返、标题、销毁 | x86、原生 ARM64、真正 AMD64/FEX 均通过 |
| X11 键盘配对和中文 IME commit | 通过；第一次任务配置漏了 inject，修正配置后通过，失败记录保留 |
| Wine 内部剪贴板往返 | 通过；不是系统双向剪贴板验收 |
| X11 / Wayland 的 DNS、OpenGL 固定帧 | DNS 两种程序架构通过；OpenGL 两种架构画面检查通过 |
| X11 / Wayland 的 D3D9 VirGL、DXVK9、Zink，以及 AMD64/FEX DXVK D3D11 | 两条显示路线各 4/4，通过 API 结果和固定帧像素检查；不是独立 GPU 后端身份或游戏性能验收 |
| 最终包显示统计回归 | 两条路线分别复测 x86 OpenGL：设备结果及像素检查通过，displayFps 恢复，路线原始采样文件已归档 |
| 当前主机严格 presented-route 判据 | **未通过**：锁定 Proton 内的旧探针未输出 expectedRoute/presentedRoute；未降低门禁或改写 PASS |
| 系统 → Wine 剪贴板 | **未验收**：现有两份合法 WineHua debug profile 均没有 READ_PASTEBOARD ACL |

渲染矩阵与输入使用 `a451b83` 的精确包；最终包 `47c1582` 只增加显示统计兼容修复，两条路线均针对它单独回归。所有 suite 原始 verdict、用例结果、固定帧及包身份保存在任务 `mini/runs/`。不以 8 秒简单场景的 FPS 推断 Heaven、PAL4 或性能提升。

## 发现并修复

1. `a451b83`：移植后把 smoke NAPI 注册一起链入 libentry，导致产品导出被覆盖，onCreate 的 setOpenTargetCallback 调用闪退。恢复独立 libsmoke_napi；最终包检查同时验证所有 ArkTS 原生导入及唯一模块注册。旧 CMake 负对照被拒绝，Mini 已正常启动。
2. `47c1582`：VPP 按窗口统计拒绝了 X11 合成器的 output id=0，并丢掉上游 route/surface_key 协议。恢复独立的上游五字段采样，保留按窗口的带时间戳 HUD 文件。主机测试确认两种格式互不覆盖、陈旧样本和非法数据被拒绝，真机显示帧数已恢复。

## 测试包与限制

完整权限包安装被系统以 `9568289 / READ_PASTEBOARD` 拒绝。为验证其余运行时，临时测试包只省略该权限声明；ArkTS、运行时和产品身份保持原样，源码中的生产 manifest 已恢复。它能覆盖启动、图形、输入和缺失权限时继续启动的行为，不能用于正式发布，也不能宣称系统剪贴板通过。完整验证需由 WineHua 对应 AGC 项目提供获准该权限的合法 profile。

当前包与上一份已测回退包仅保留在 WSL 目标工作树的 `workspace_temp/runtime-check/`，不是正式上架产物：

- 当前：`winehua-mini-runtime-only-signed.hap`，源码 `47c1582`，SHA-256 `d54f9ceaec0125747f13808ba2321a934ef6b912d767599f7d075e0ab520c773`。
- 回退：`winehua-mini-runtime-only-previous-signed.hap`，源码 `a451b83`，SHA-256 `400e3ea2f139d5f1700c8024a98fe9ab4def1a8313a0f4d04e96e8a3a4a95037`。

包名、版本、完整 SHA、各轮包身份和验收边界详见相邻 JSON。APP debug 标记为 false（release 优化构建），签名使用合法 debug profile；未改正式身份或切换为正式签名。

未覆盖真实游戏、Heaven 性能对照、媒体、多实体手柄、系统 OPEN、M80、Edge/PC。测试结束已停止 WineHua，恢复系统熄屏超时，临时日志开关随测试进程退出。

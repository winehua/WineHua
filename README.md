# WineHua

WineHua 是一个让 Windows x86_64 程序运行在 HarmonyOS / OpenHarmony 上的实验工程。当前主线是 `Wine + 内嵌 Wayland compositor`，并按目标架构决定是否需要 `Box64`。

![运行效果](docs/images/run_wine_example.jpg)

## 支持平台

| 平台 | Wine | Box64 | 进程模型 | 打包方式 |
| --- | --- | --- | --- | --- |
| arm64 PC emulator | x86_64 (musl) | box64 可执行文件 | `execve` | HNP |
| arm64 Pad emulator | x86_64 (musl) | `box64.so` | NCP appspawn | rawfile zip |
| x86_64 PC emulator | x86_64 (musl) | 不需要 | `execve` | HNP |
| x86_64 Pad emulator | x86_64 (musl) `.so` | 不需要 | NCP appspawn | rawfile zip |

## 当前状态

- 默认构建入口是 `scripts/rebuild_harmony.ps1`，默认宿主 backend 是 `MSYS2`
- `WSL` 仍保留为 fallback 和单独构建环境
- 当前最强验证目标是 `x86_64 + HarmonyOS 2in1 / pc_all emulator`
- 已验证主链路包括 `wineserver`、`wineboot --init`、`notepad.exe`
- 图形 Step 1 已跑通：`winehua_graphics_smoke.exe` 能通过 `VirGL/vtest + guest Mesa virpipe` 正确渲染 OpenGL 画面
- 当前最终上屏仍是 `wl_shm + cpu_copy + gl_upload`，还不是零拷贝显示链路

## 常用命令

首次检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap_msys2.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Mode doctor -Arch x86_64
```

完整构建：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Mode full -Arch x86_64 -Target 127.0.0.1:5555
```

日常增量：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Mode incremental -Arch x86_64 -Target 127.0.0.1:5555
```

仅重装当前包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Mode deploy -Target 127.0.0.1:5555
```

## 图形相关

- GL / VirGL Step 1 设计与改动说明见 [docs/OPENGL_VIRGL_DESIGN.md](docs/OPENGL_VIRGL_DESIGN.md)
- guest 3D receiver bundle 说明见 [prebuilt/guest_gfx/README.md](prebuilt/guest_gfx/README.md)
- 图形相关 fork / submodule 管理约定见 [docs/SOURCE_MANAGEMENT.md](docs/SOURCE_MANAGEMENT.md)
- 当前图形方案强调复用 Wine 现有 `WGL/EGL + winewayland.drv` 路径，不要求 Windows 应用做适配

## 文档入口

- [docs/ONBOARDING.md](docs/ONBOARDING.md): 新机器或第一次接手时先看
- [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md): 当前推荐构建、打包、安装、日志流程
- [docs/CURRENT_STATUS.md](docs/CURRENT_STATUS.md): 当前能力、限制和验证目标
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): 总体架构
- [docs/AUDIO_ARCHITECTURE.md](docs/AUDIO_ARCHITECTURE.md): 音频方案
- [docs/OPENGL_VIRGL_DESIGN.md](docs/OPENGL_VIRGL_DESIGN.md): OpenGL / VirGL Step 1 设计资料
- [docs/README.md](docs/README.md): 文档索引

## 目录概览

```text
WineHua/
├── entry/src/main/cpp/          # Native C++: NAPI, Wayland compositor, renderer, broker
├── entry/src/main/ets/          # ArkTS UI, window model, ability entry
├── scripts/                     # 构建、打包、部署脚本
├── prebuilt/guest_gfx/          # guest Mesa / virpipe 运行时 bundle
├── docs/                        # 设计、构建、状态文档
└── thirdparty/                  # wine / box64 / virglrenderer / mesa-ohos / libdrm-ohos / wayland 等子工程
```

## 产物路径

- Signed HAP: `entry/build/default/outputs/default/entry-default-signed.hap`
- HNP:
  - `entry/hnp/x86_64/winehua.hnp`
  - `entry/hnp/arm64-v8a/winehua.hnp`

## 交流

WeChat 二维码：

<img src="docs/images/wechat_qrcode.png" width="300">

# WineHua

在 HarmonyOS / OpenHarmony 设备上运行 Windows x86_64 程序的实验性工程。当前主线是 `Wine + 内嵌 Wayland compositor`，并按目标架构决定是否需要 `Box64`。

![运行效果](docs/images/run_wine_example.jpg)

## 支持平台

| 平台 | Wine | Box64 | 进程模型 | 打包方式 |
| --- | --- | --- | --- | --- |
| arm64 PC (emulator) | x86_64 (musl) | box64 可执行文件 | `execve` | HNP |
| arm64 Pad (emulator) | x86_64 (musl) | `box64.so` | NCP appspawn | rawfile zip |
| x86_64 PC emulator | x86_64 (musl) | 不需要 | `execve` | HNP |
| x86_64 Pad emulator | x86_64 (musl) `.so` | 不需要 | NCP appspawn | rawfile zip |

## 当前构建体系

- 默认宿主已经切到 `MSYS2`
- `WSL` 仍保留为 fallback 和回归路径
- DevEco / OpenHarmony SDK、`hvigorw`、`hnpcli`、`hdc` 继续复用 Windows 侧安装
- Windows 统一外层入口是 `scripts/rebuild_harmony.ps1`
- 内层单 shell 编排入口是 `scripts/rebuild_harmony.sh`
- DevEco 直接点击构建也已接入仓库内的 SDK overlay / Java / Node 环境补齐

构建体系说明见 [docs/MSYS2_BUILD_SYSTEM.md](docs/MSYS2_BUILD_SYSTEM.md)。

## 推荐命令

首次准备:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap_msys2.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Backend msys2 -Mode doctor -Arch x86_64
```

完整构建:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Backend msys2 -Mode full -Arch x86_64 -Target 127.0.0.1:5555
```

日常增量:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Backend msys2 -Mode incremental -Arch x86_64 -Target 127.0.0.1:5555
```

WSL fallback:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 -Backend wsl -Mode doctor -Arch x86_64
```

## 当前主线目标

- `x86_64 + HarmonyOS 2in1 / pc_all` 模拟器
- HNP + 已签名 HAP 产物
- `wineserver`、`wineboot --init`、`notepad.exe` 主链验证

## 产物路径

- Signed HAP: `entry/build/default/outputs/default/entry-default-signed.hap`
- HNP:
  - `entry/hnp/x86_64/winehua.hnp`
  - `entry/hnp/arm64-v8a/winehua.hnp`

## 文档入口

- [docs/ONBOARDING.md](docs/ONBOARDING.md): 第一次接手仓库先看这里
- [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md): 当前推荐构建、打包、安装、日志流程
- [docs/MSYS2_BUILD_SYSTEM.md](docs/MSYS2_BUILD_SYSTEM.md): MSYS2 构建体系、脚本分层、SDK overlay、DevEco 直构建
- [docs/CURRENT_STATUS.md](docs/CURRENT_STATUS.md): 当前能力、限制和验收状态
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): 代码主链路和运行时结构
- [docs/README.md](docs/README.md): 文档索引

## 目录结构

```text
WineHua/
├─ entry/src/main/cpp/          # Native C++: NAPI, Wayland compositor, renderer, input
├─ entry/src/main/ets/          # ArkTS UI, window model, ability entry
├─ scripts/
│  ├─ env.sh                    # 共享环境解析 + host adapter
│  ├─ build_*.sh                # deps / wine / box64 / native 等底层步骤
│  ├─ assemble.sh               # HNP / rawfile 运行时布局
│  ├─ package.sh                # HNP / HAP 打包、签名、校验
│  ├─ rebuild_harmony.sh        # 内层单 shell 编排入口
│  ├─ rebuild_harmony.ps1       # Windows 宿主统一入口
│  ├─ bootstrap_msys2.ps1       # MSYS2 初次安装和补包
│  ├─ inject_hnp_into_hap.ps1   # unsigned HAP 注入 HNP
│  ├─ ensure_hnp_signed_hap.ps1 # signed HAP 校验 / 必要时重签
│  └─ deveco_hvigor_env.js      # DevEco 直构建时补 SDK overlay / Java / Node 环境
├─ docs/                        # 构建、状态、架构和研究文档
└─ thirdparty/                  # git submodule: wine / box64 / freetype / wayland / xkb 等
```

## 交流

WeChat 二维码:

<img src="docs/images/wechat_qrcode.png" width="300">

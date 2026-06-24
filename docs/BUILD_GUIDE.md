# WineHua 构建指南

> 更新日期: 2026-06-24

这份文档只讲当前主线、可复现、推荐维护的构建路径。

## 结论先说

当前推荐入口有两层:

- Windows 宿主统一入口: `scripts/rebuild_harmony.ps1`
- 内层单 shell 编排入口: `scripts/rebuild_harmony.sh`

默认 backend 已切到 `MSYS2`，`WSL` 仍保留为 fallback。

体系说明见 [MSYS2_BUILD_SYSTEM.md](MSYS2_BUILD_SYSTEM.md)。

## 首次准备

### 1. 初始化 submodule

```bash
git submodule update --init --recursive
```

说明:

- `thirdparty/freetype/subprojects/dlg` 当前仍可能是 warning，不是主线阻塞项

### 2. 准备 MSYS2

在 Windows 宿主运行:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap_msys2.ps1
```

这个脚本会:

- 如缺少 `C:\msys64`，自动下载并解压 MSYS2 base
- 按官方要求执行两次 `pacman --noconfirm -Syuu`
- 安装仓库所需的 `git`、`make`、`pkgconf`、`python`、`perl`、`zip`、`unzip`、`libtool`、`autoconf-wrapper`、`automake-wrapper`、`patch`、`diffutils`、`cmake`、`meson`、`ninja`

### 3. 跑 doctor

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend msys2 `
  -Mode doctor `
  -Arch x86_64
```

`doctor` 应至少能解析出:

- `HOST_SHELL`
- `OHOS_SDK`
- `HVIGORW`
- `NODE_BIN`
- `JAVA_BIN`
- `HNPCLI`
- `HDC`
- `CLANG`

## 日常命令

### Windows 宿主默认路径

完整重建:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend msys2 `
  -Mode full `
  -Arch x86_64 `
  -Target 127.0.0.1:5555
```

日常增量:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend msys2 `
  -Mode incremental `
  -Arch x86_64 `
  -Target 127.0.0.1:5555
```

只重做 Wine:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend msys2 `
  -Mode wine `
  -Arch x86_64 `
  -Target 127.0.0.1:5555
```

只重打包:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend msys2 `
  -Mode package `
  -Arch x86_64 `
  -Target 127.0.0.1:5555
```

只重装当前 HAP:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Mode deploy `
  -Target 127.0.0.1:5555
```

只看日志:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Mode logs `
  -Target 127.0.0.1:5555
```

### WSL fallback

如需回退旧链路验证:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend wsl `
  -Mode doctor `
  -Arch x86_64
```

或在 WSL 内只做构建:

```bash
bash scripts/rebuild_harmony.sh doctor x86_64
bash scripts/rebuild_harmony.sh full x86_64
bash scripts/rebuild_harmony.sh incremental x86_64
bash scripts/rebuild_harmony.sh wine x86_64
bash scripts/rebuild_harmony.sh package x86_64
```

把 `x86_64` 换成 `arm64` 或 `all` 即可。

## DevEco 直接构建

现在 DevEco 直接运行:

```text
node.exe hvigorw.js --mode module ... assembleHap
```

也会自动补齐:

- `DEVECO_SDK_HOME`
- `OHOS_BASE_SDK_HOME`
- `JAVA_HOME`
- `NODE_HOME`
- `PATH`

并将 SDK 解析指向仓库内的 `out/sdk-links/harmonyos-sdk-root` overlay。

对应实现:

- 根 `hvigorfile.ts`
- `scripts/deveco_hvigor_env.js`
- `entry/hvigorfile.ts`

所以现在有两条可用入口:

- 推荐维护入口: `scripts/rebuild_harmony.ps1`
- IDE 直接构建入口: DevEco 的 `assembleHap`

## `Mode` 怎么选

- `full`
  - `thirdparty/` 变了
  - `scripts/env.sh` 变了
  - `scripts/build_*.sh` 变了
  - Wine / Box64 / sysroot-ext 相关依赖变了
  - 换机器、换 SDK、切 backend 后第一次完整构建

- `incremental`
  - `entry/src/main/cpp`
  - ArkTS 页面、窗口管理
  - `assemble.sh`
  - `package.sh`
  - HNP / HAP 打包与签名逻辑

- `wine`
  - 只改了 `thirdparty/wine/`
  - 希望复用现有 `deps/native` 产物，只重做 `wine -> hnp -> hap`

- `package`
  - 只改 HNP / HAP 组装
  - 只改 ABI 过滤
  - 只改签名逻辑

- `deploy`
  - 当前 HAP 已存在，只需要重装

- `logs`
  - 当前应用已装好，只需要抓 `hilog`

- `doctor`
  - 优先确认工具链、submodule、SDK 路径和 `hdc` 目标是否健康

## 关键规则

### 1. 不要拆成多个 shell

无论是 `MSYS2` 还是 `WSL`，都不要把 `deps / wine / native / hnp / hap` 拆成多个独立 shell。

### 2. 多目标时显式传 `-Target`

PC 模拟器推荐直接写 `127.0.0.1:5555`。

### 3. `package` / `incremental` 可能自动补前置产物

如前置产物缺失，脚本会自动补 `deps`、`wine`、`box64`、`native`。

如果明确要 fail-fast:

- Windows 宿主入口加 `-NoAutoHeal`
- 内层 shell 入口加 `--no-auto-heal`

## 产物路径

- Signed HAP:
  - `entry/build/default/outputs/default/entry-default-signed.hap`

- HNP:
  - `entry/hnp/x86_64/winehua.hnp`
  - `entry/hnp/arm64-v8a/winehua.hnp`

## 验收顺序

- `doctor` 能跑通
- 目标架构的 `winehua.hnp` 存在
- `entry-default-signed.hap` 存在
- `hdc install -r` 成功
- `aa start -b app.hackeris.winehua -a EntryAbility` 成功
- `x86_64` 主链中 `notepad.exe` 能启动

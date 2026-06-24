# WineHua Onboarding

> 更新日期: 2026-06-24

这份文档给第一次接手 `WineHua` 的同学用。目标不是解释全部历史，而是让你在 10 分钟内知道:

- 现在默认的构建宿主是什么
- 新机器第一天该跑哪些命令
- 改了什么代码该用什么模式重建
- 出问题时先看哪里

## 先记住这三件事

### 1. 统一入口优先

不要一上来就手工拼 `build.sh`。优先使用:

- Windows 宿主: `scripts/rebuild_harmony.ps1`
- 内层编排: `scripts/rebuild_harmony.sh`

### 2. 默认 backend 已切到 `MSYS2`

当前主推荐路径是:

- Windows 宿主安装好的 DevEco / OpenHarmony SDK
- `scripts/bootstrap_msys2.ps1` 准备 MSYS2
- `scripts/rebuild_harmony.ps1 -Backend msys2 ...` 驱动整条构建链

`WSL` 还保留着，但主要作为 fallback 和回归验证路径。

### 3. 构建尽量保持在同一个 shell

不要把 `deps / wine / native / hnp / hap` 拆成多个独立 shell 调用。

## 第一天建议顺序

### 第 1 步: 初始化 submodule

```bash
git submodule update --init --recursive
```

### 第 2 步: 准备 MSYS2

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\bootstrap_msys2.ps1
```

### 第 3 步: 跑 doctor

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend msys2 `
  -Mode doctor `
  -Arch x86_64
```

你应看到:

- `HOST_SHELL=msys2`
- `OHOS_SDK`
- `HVIGORW`
- `NODE_BIN`
- `JAVA_BIN`
- `HNPCLI`
- `HDC`
- 一个可见的 `hdc` target，比如 `127.0.0.1:5555`

### 第 4 步: 做一次完整重建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend msys2 `
  -Mode full `
  -Arch x86_64 `
  -Target 127.0.0.1:5555
```

### 第 5 步: 确认运行主链

重点看三件事:

- HAP 是否安装成功
- `aa start` 是否成功
- 日志里 `wineserver`、`wineboot --init`、`notepad.exe` 是否正常

## 改动类型和推荐模式

### 用 `incremental`

适用于:

- `entry/src/main/cpp`
- ArkTS 页面和窗口模型
- `assemble.sh`
- `package.sh`
- HNP / HAP 组装或签名逻辑

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\rebuild_harmony.ps1 `
  -Backend msys2 `
  -Mode incremental `
  -Arch x86_64 `
  -Target 127.0.0.1:5555
```

### 用 `full`

适用于:

- `thirdparty/`
- Wine fork
- Box64
- `scripts/env.sh`
- `scripts/build_*.sh`
- 换机器、换 SDK、切换 backend 后首次构建

### 用 `package`

适用于:

- 只改 HNP / HAP 组装
- 只改 ABI 过滤
- 只改签名逻辑

### DevEco 直接构建也可用

现在 DevEco 直接点 `assembleHap` 也会自动补:

- SDK overlay
- `DEVECO_SDK_HOME`
- `OHOS_BASE_SDK_HOME`
- Java / Node 环境

对应实现:

- 根 `hvigorfile.ts`
- `scripts/deveco_hvigor_env.js`
- `entry/hvigorfile.ts`

## 关键脚本该怎么看

- `scripts/env.sh`
  - 共享环境解析
  - host adapter
  - SDK overlay
  - Windows `.exe` wrapper

- `scripts/rebuild_harmony.ps1`
  - Windows 宿主入口
  - backend 选择
  - build / install / launch / logs

- `scripts/rebuild_harmony.sh`
  - 内层单 shell 编排
  - `full / incremental / wine / package / doctor`

- `scripts/assemble.sh`
  - HNP 运行时布局
  - 增量布局 stamp

- `scripts/package.sh`
  - HNP 打包
  - hvigor 打 HAP
  - signed HAP HNP 校验

- `scripts/inject_hnp_into_hap.ps1`
  - unsigned HAP 注入 HNP

- `scripts/ensure_hnp_signed_hap.ps1`
  - signed HAP 校验
  - 必要时自动重签

## 常见排障顺序

### `doctor` 先失败

先看:

- `scripts/bootstrap_msys2.ps1` 是否完整跑完
- DevEco Studio 路径是否变化
- `MSYS2` 里 `git / bash / python / cmake / ninja / meson / zip` 是否齐
- 模拟器是否已启动
- `hdc list targets` 是否能看到目标

### HAP 能打出来但安装/运行异常

先看:

- `entry-default-signed.hap` 是否是最新产物
- HAP 内是否保留 `hnp/<arch>/winehua.hnp`
- `bm install -p` 是否能成功
- 日志里 `wineboot` 是否再次被交互安装器阻塞

## 交接前至少确认

- 统一入口脚本还能跑 `doctor`
- 至少一个目标架构能产出 signed HAP
- 当前主线目标能安装并拉起
- 文档里的命令没有漂移
- 若改了流程，记得同步:
  - `docs/BUILD_GUIDE.md`
  - `docs/MSYS2_BUILD_SYSTEM.md`
  - `docs/CURRENT_STATUS.md`
  - `.codex/skills/winehua-harmony-build`

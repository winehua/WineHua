# WineHua MSYS2 构建体系

> 更新日期: 2026-06-24

这份文档专门解释当前仓库的构建体系，不只讲“怎么跑命令”，而是讲:

- 为什么默认宿主要切到 `MSYS2`
- PowerShell / MSYS2 / DevEco / SDK overlay 分别负责什么
- 各脚本的职责边界
- 增量、多核、HNP 保留、DevEco 直构建是怎么接上的

## 1. 设计目标

迁移到 `MSYS2` 不是为了替换 Windows 侧 DevEco SDK，而是为了替换原先 `WSL` 里的 shell / POSIX 工具层。

当前目标是:

- 默认宿主用 `MSYS2`
- `WSL` 保留为 fallback
- DevEco / OpenHarmony SDK、`hvigorw`、`hnpcli`、`hdc` 继续复用 Windows 安装
- 把路径转换、SDK overlay、构建入口、HNP/HAP 组装统一到仓库脚本里

## 2. 分层结构

### 外层控制

- `scripts/rebuild_harmony.ps1`
  - Windows 宿主统一入口
  - 负责 `auto | msys2 | wsl` backend 选择
  - 负责 `doctor / full / incremental / wine / package / deploy / logs`
  - 负责 `hdc install`、`aa start`、日志抓取

### 内层单 shell 编排

- `scripts/rebuild_harmony.sh`
  - 唯一的内层编排入口
  - 保证 `deps -> wine -> native -> hnp -> hap` 在同一个 shell 内完成
  - 负责 auto-heal / fail-fast 切换

### 环境与宿主适配

- `scripts/env.sh`
  - 共享环境解析
  - host shell 探测: `msys2 | wsl | posix`
  - SDK overlay 生成
  - Windows `.exe` 包装
  - 并发变量导出: `JOBS`、`MAKEFLAGS`、`CMAKE_BUILD_PARALLEL_LEVEL`、`MESON_NUM_THREADS`

- `scripts/wsl_exe_wrapper.sh`
  - 宿主无关的 Windows 工具 wrapper
  - `wsl` 下用 `wslpath`
  - `msys2` 下用 `cygpath`
  - 显式处理 `MSYS2_ARG_CONV_EXCL` / `MSYS2_ENV_CONV_EXCL`

### 底层构建步骤

- `build.sh`
  - 单步入口
  - 会分发到 `scripts/build_*.sh`

- `scripts/build_*.sh`
  - `deps / wine / box64 / native / xkb / wayland / ...`

### 运行时布局与打包

- `scripts/assemble.sh`
  - 组装 HNP 运行时布局
  - 维护 assemble stamp，避免每次重组整棵 staging

- `scripts/package.sh`
  - 打 HNP
  - 调 hvigor 打 HAP
  - 校验 signed HAP 是否仍保留 HNP

- `scripts/inject_hnp_into_hap.ps1`
  - 向 unsigned HAP 注入 `hnp/<arch>/winehua.hnp`

- `scripts/ensure_hnp_signed_hap.ps1`
  - 校验 signed HAP 是否保留 HNP
  - 必要时自动重签

### DevEco IDE 直构建

- 根 `hvigorfile.ts`
  - 在 IDE 原生命令进入 hvigor 之前补环境

- `scripts/deveco_hvigor_env.js`
  - 自动设置 `DEVECO_SDK_HOME`
  - 自动设置 `OHOS_BASE_SDK_HOME`
  - 自动设置 `JAVA_HOME`
  - 自动设置 `NODE_HOME`
  - 自动补 `PATH`
  - 自动构建并复用 `out/sdk-links/harmonyos-sdk-root`

- `entry/hvigorfile.ts`
  - 为 hvigor 原生 `PackageHap / SignHap` 流程插入 HNP 处理任务
  - 保证 DevEco 直构建产出的 signed HAP 也保留 HNP

## 3. 为什么还需要 SDK overlay

Windows 默认 SDK 安装路径通常带空格，例如:

```text
C:\Program Files\Huawei\DevEco Studio\...
```

仓库当前的工具链组合里，以下环节对空格路径较敏感:

- `bash`
- `autotools`
- `meson`
- `cmake`
- `ninja`
- Windows `.exe` 通过 POSIX shell 调用
- DevEco / hvigor 组件探测

因此 `scripts/env.sh` 会在仓库内生成:

```text
out/sdk-links/harmonyos-sdk-root/<SDK版本>/
```

作用:

- 提供无空格、稳定、可复用的 SDK 根目录
- 保留 `openharmony` / `hms` 的统一布局
- 让 MSYS2、WSL 和 DevEco 直构建最终都指向同一套可用路径

## 4. backend 选择逻辑

`scripts/rebuild_harmony.ps1` 支持:

- `-Backend msys2`
  - 默认路径

- `-Backend wsl`
  - fallback / 回归验证

- `-Backend auto`
  - 优先 `msys2`
  - 若本机没有可用 `MSYS2`，再回退 `wsl`

`msys2` 分支固定设置:

- `MSYSTEM=MSYS`
- `CHERE_INVOKING=1`
- `MSYS=winsymlinks:nativestrict`

目的:

- 保持单一 MSYS2 shell 语义
- 避免路径和符号链接行为漂移

## 5. DevEco 直构建为什么之前会失败

IDE 原生命令大致是:

```text
node.exe hvigorw.js --mode module ... assembleHap
```

它有两个典型早期问题:

### 1. `SDK component missing`

原因不是 SDK 真没装，而是 hvigor 在很早阶段依赖:

- `DEVECO_SDK_HOME`
- `OHOS_BASE_SDK_HOME`

并按自己的组件定义文件查找结构。仓库当前真正验证通过的是 overlay 结构，而不是直接裸用 DevEco 默认目录。

### 2. `spawn java ENOENT`

IDE 直跑时，Java 路径不一定总是能被 hvigor 可靠继承。

现在这两个问题都由根 `hvigorfile.ts` + `scripts/deveco_hvigor_env.js` 统一补齐了。

## 6. HNP 为什么需要专门保留逻辑

单纯把 `entry/hnp/<arch>/winehua.hnp` 放在工程里，不代表 signed HAP 一定会最终保留下来。

实际问题是:

- hvigor 在 `PackageHap` 和 `SignHap` 阶段可能重新组装产物
- 如果只在外层脚本后处理，DevEco 直接安装时仍可能拿到丢 HNP 的包

现在的处理链路是:

1. `PackageHap` 后注入 HNP 到 unsigned HAP
2. `SignHap` 后校验 signed HAP
3. 若 signed HAP 丢了 HNP，则自动重签修复

这样:

- `scripts/rebuild_harmony.ps1` 产出的 signed HAP 正确
- DevEco 直接点击构建 / 安装拿到的 signed HAP 也正确

## 7. 增量和多核是怎么落地的

### 多核

`scripts/env.sh` 会导出:

- `JOBS`
- `MAKEFLAGS=-jN`
- `CMAKE_BUILD_PARALLEL_LEVEL=N`
- `MESON_NUM_THREADS=N`

`entry/hvigorfile.ts` 还会给 hvigor 触发的 Ninja 命令补 `-j N`。

### 增量

当前增量不是只靠 hvigor:

- `assemble.sh` 用 stamp 跳过未变化的 HNP 布局重组
- `package.sh` 会复用已是最新的 `winehua.hnp`
- hvigor 使用 `--parallel --incremental --daemon`
- `rebuild_harmony.sh` 的 `incremental` / `package` 模式只重走必要阶段

## 8. 脚本覆盖范围检查

当前构建链路里关键场景都有对应脚本:

- 新机器准备: `scripts/bootstrap_msys2.ps1`
- Windows 宿主统一构建: `scripts/rebuild_harmony.ps1`
- 单 shell 内层编排: `scripts/rebuild_harmony.sh`
- 环境解析与宿主适配: `scripts/env.sh`
- Windows 工具调用包装: `scripts/wsl_exe_wrapper.sh`
- HNP 运行时布局: `scripts/assemble.sh`
- HNP / HAP 打包签名: `scripts/package.sh`
- DevEco 直构建环境补齐: `scripts/deveco_hvigor_env.js`
- hvigor 内 HNP 保留: `entry/hvigorfile.ts`

补充说明:

- `build.sh` 仍保留为低层单步调试入口，但不再是推荐维护入口
- `scripts/build_all.sh` 属于历史辅助脚本，不纳入当前主线验收链路

如果后续再改流程，优先同步:

- 入口脚本
- 这份体系文档
- `docs/BUILD_GUIDE.md`
- `docs/ONBOARDING.md`

## 9. 推荐排障顺序

### 构建入口层

先确认你走的是哪条入口:

- `scripts/rebuild_harmony.ps1`
- `scripts/rebuild_harmony.sh`
- DevEco `assembleHap`

### 环境层

先看:

- `doctor` 是否通过
- `MSYS2` 包是否齐
- DevEco SDK 路径是否变化
- `out/sdk-links` 是否生成

### 打包层

先看:

- `entry/hnp/<arch>/winehua.hnp` 是否存在
- signed HAP 内是否有 `hnp/<arch>/winehua.hnp`
- `bm install -p` 是否能成功

### 运行层

先看:

- `wineserver`
- `wineboot --init`
- `notepad.exe`
- 是否再次被 Mono / Gecko 交互安装阻塞

## 10. 维护原则

- 不要绕开统一入口直接拆多段 shell
- 不要让不同入口走出两套互相不一致的 HNP / HAP 逻辑
- 不要把仓库路径写死到某个盘符
- 优先让 `MSYS2`、`WSL fallback`、`DevEco direct build` 三条路径共享同一套产物和规则

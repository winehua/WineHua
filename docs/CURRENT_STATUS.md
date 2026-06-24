# WineHua 当前状态
> 更新日期: 2026-06-24

## 已验证主线

### 构建链路

- `MSYS2` 已成为默认 backend，并由 `scripts/rebuild_harmony.ps1` 统一调度
- `WSL` fallback 仍保留，可继续用于回归验证
- Windows 侧 DevEco / OpenHarmony SDK、`hvigorw`、`hnpcli`、`hdc` 继续复用
- `scripts/rebuild_harmony.sh` 仍是唯一的内层单 shell 编排入口
- `x86_64` HNP 与 signed HAP 仍是主验收产物

### 打包与 IDE 链路

- `entry/hvigorfile.ts` 已接入 HNP 注入与 signed HAP 校验修复
- DevEco 直接执行 `assembleHap` 时，会通过根 `hvigorfile.ts` + `scripts/deveco_hvigor_env.js` 自动补齐 SDK overlay、Java、Node 环境
- 当前主线已验证 DevEco 直构建产出的 signed HAP 能保留 `hnp/<arch>/winehua.hnp`

### 运行链路

- HarmonyOS `2in1 / pc_all` 模拟器仍是最强验证目标
- HAP 进程内嵌 Wayland compositor 仍可启动
- `wineserver` 可启动
- `wineboot --init` 不再被 Mono 首启阻塞
- `notepad.exe` 仍是主线 smoke target

### 宿主适配与效率

- `env.sh` 已拆成共享环境解析 + host adapter
- Windows `.exe` wrapper 已兼容 `wslpath` 和 `cygpath`
- `out/sdk-links` 的无空格 SDK overlay 继续作为 MSYS2 / WSL / DevEco 共用层
- 并行变量 `JOBS`、`MAKEFLAGS`、`CMAKE_BUILD_PARALLEL_LEVEL`、`MESON_NUM_THREADS` 已统一导出
- `assemble.sh` / `package.sh` 已接入增量复用，避免每次都从头重组 HNP

## 当前默认假设

### 1. 默认 backend 是 `MSYS2`

当前默认路径是:

- Windows 侧 DevEco / OpenHarmony SDK
- `scripts/bootstrap_msys2.ps1`
- `scripts/rebuild_harmony.ps1 -Backend msys2 ...`

如果 `MSYS2` 缺包、升级失败或需要回归对比，再切 `-Backend wsl`。

### 2. 当前最强验证目标仍是 `x86_64`

当前优先目标仍是:

- `x86_64` Wine 用户态
- `x86_64` native libs
- HarmonyOS `2in1` PC 模拟器

`arm64` 仍走同一套脚本和打包逻辑，但运行侧验证强度还不如 `x86_64`。

### 3. Mono / Gecko 首启仍默认关闭

当前仍默认设置:

```text
WINEDLLOVERRIDES=mscoree,mshtml=
```

影响:

- `.NET` / `mshtml` 依赖程序不是当前默认成功目标

### 4. HNP 运行时布局仍以 `bin/` 为中心

当前运行时仍以这些路径为准:

- `opt/winehua/bin/ntdll.so`
- `opt/winehua/bin/x86_64-unix/*`
- `opt/winehua/bin/x86_64-windows/*`
- `opt/winehua/lib/<abi>/*`
- `opt/winehua/share/X11/xkb/*`

## 已知限制

### 1. `MSYS2` 仍处于默认迁移收口期

- 默认 backend 虽已切换，但 `full / incremental / package` 仍需持续做回归
- `WSL` 不会立刻删除，要保留到 `MSYS2` 主线长期稳定

### 2. Wayland 可选协议仍未补齐

当前仍缺:

- pointer constraints
- relative pointer
- text input manager v3
- data device manager
- toplevel icon manager

这会直接影响:

- 剪贴板
- 相对鼠标移动
- 指针锁定
- 输入法集成
- 部分窗口外观能力

### 3. 音频链路还需要继续 soak

当前已经确认“能出声”，但还没把下面这些验证做完:

- 多进程同时播放的长期稳定性
- `MP3 / WAV / 视频音轨` 等多格式验证
- `underrun / overflow / queued_frames` 指标核对
- 非音频 Wine 进程是否会被 broker 初始化拖慢

### 4. XKB 数据仍以 vendored 构建为主

- 当前推荐路径是 `thirdparty/xkeyboard-config` 一致产出 `share/X11/xkb`
- `scripts/build_xkbconfig.sh` 里仍保留 host XKB 数据应急 fallback 逻辑，后续若继续收严可再改成显式开关

## 当前优先验收

- `scripts/bootstrap_msys2.ps1` 能在干净机器完成安装和补包
- `rebuild_harmony.ps1 -Backend msys2 -Mode doctor -Arch x86_64` 通过
- `rebuild_harmony.ps1 -Backend wsl -Mode doctor -Arch x86_64` 通过
- DevEco 直接 `assembleHap` 能产出保留 HNP 的 signed HAP
- `entry/build/default/outputs/default/entry-default-signed.hap` 存在
- `entry/hnp/x86_64/winehua.hnp` 存在
- `bm install -p` 可安装当前 signed HAP
- `x86_64` 主线上 `notepad.exe` 能启动

## TODO

### Merge Before PR

- [ ] 复跑并留档 `MSYS2` backend 下的 `full x86_64`
- [ ] 持续回归 `MSYS2` backend 下的 `incremental x86_64`
- [ ] 持续回归 `MSYS2` backend 下的 `package x86_64`
- [ ] 路径回归: `C:\Program Files\Huawei\DevEco Studio`、response file、JSON config、`clang.exe / hnpcli.exe / hdc.exe`
- [ ] 音频 soak test 与多格式验证

### Deferred

- [ ] capture / microphone
- [ ] exclusive mode
- [ ] multichannel output
- [ ] 更广的 PCM format negotiation
- [ ] 继续减少 event callback 路径上的轮询

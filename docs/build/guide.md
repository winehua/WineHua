# Wine for HarmonyOS — 构建指南

> 最后核实: 2026-09-24

## 环境

构建环境搭建详见 [env.md](env.md)。

快速检查：

```bash
# 必备工具
make cmake ninja meson bison flex autoconf libtoolize gcc-mingw-w64-x86-64 i686-w64-mingw32-gcc java
glslangValidator               # guest Vulkan 栈构建依赖 (build_ohos_guest_vulkan.sh)
# OHOS SDK: /apps/harmony/sdk/default/openharmony/
# wayland-scanner: /usr/local/bin/wayland-scanner (需预装)
```

---

## 平台

三种构建方案（`NATIVE_ARCH` = 设备架构，`WINE_ARCH` = Wine/模拟层架构）：

| 方案 | Makefile 命令 | `NATIVE_ARCH` | `WINE_ARCH` | Wine | 转译 |
|------|-------------|---------------|-------------|------|------|
| ① x86_64 原生 | `make NATIVE_ARCH=x86_64` | `x86_64` | `x86_64` | Wine .so → libs/ | 无 |
| ② box64+wine | `make NATIVE_ARCH=arm64-v8a WINE_ARCH=x86_64` | `arm64-v8a` | `x86_64` | x86_64 ELF → rawfile zip | box64.so (NCP dlopen) |
| ③ arm64 原生 | `make NATIVE_ARCH=arm64-v8a` | `arm64-v8a` | `aarch64` | Wine aarch64 .so → libs/ | FEX (`libarm64ecfex.dll`/`libwow64fex.dll`) + Box64 wow64 (`wowbox64.dll`, HODLL 默认) |

默认值：`NATIVE_ARCH=x86_64`（注意 `scripts/env.sh` 直接运行脚本时默认 `arm64-v8a`）；`WINE_ARCH` 默认由 `NATIVE_ARCH` 推导（arm64→aarch64 方案③，x86_64→x86_64 方案①）。

> `NATIVE_ARCH=all` 双架构 HAP 已移除：单一 `WINE_ARCH` 无法同时满足方案②/③ 的 arm64 assemble。需要多个 HAP 时分别执行各方案的完整构建。

> Wine 和 wineserver 通过 NCP（`OH_Ability_StartNativeChildProcess`）创建子进程。方案② 下 Box64 编译为 box64.so 由 NCP 子进程 dlopen 加载。

---

## 新 clone / 他人编译（与 CI 一致的流程）

### 两步

```bash
# 1) 取主仓库 + 全部 submodule（含构建实际使用的 Wine 源码 thirdparty/wine-valve）
git clone https://github.com/winehua/WineHua.git && cd WineHua
git submodule update --init --recursive

# 2) 构建（arm64 原生：FEX + wow64 box64）
make NATIVE_ARCH=arm64-v8a WINE_ARCH=aarch64
```

- `thirdparty/wine-valve`（2026-09-22 起）是正式 submodule：`winehua/wine.git` 的
  `ohos-port-steam-win64` 分支，`WINE_SRC`（`scripts/env.sh` / `Makefile`）指向它。
- 历史遗留：早期它不是 submodule，而是 `thirdparty/wine` 的 git worktree；
  `scripts/prepare-wine-valve.sh` 保留给那些旧 clone（目录已存在时直接返回，不会破坏 submodule 布局）。
- 分支名可用 `WINE_VALVE_REF=<branch>` 覆盖（仅旧流程需要），CI 与 submodule 都以 `ohos-port-steam-win64` 为准。
- 私有 submodule（`wine` / `box64` / `mesa-ohos`）需要访问权限；CI 用
  `url."https://x-access-token:<TOKEN>@github.com/".insteadOf "git@github.com:"` 改成 HTTPS 取。
- 签名用仓库私有 `sign.py`（口令在本地配置里，不进仓库）；公开/CI 产物是 **unsigned HAP**。

### 构建输入各自来自哪里（改代码前必看）

| 输入 | 来源 | 说明 |
|------|------|------|
| Wine (`WINE_SRC`) | submodule `thirdparty/wine-valve` → `winehua/wine.git` 的 **`ohos-port-steam-win64` 分支** | **Wine 侧改动必须推到这条分支**，再更新主仓库 gitlink |
| box64（含 `wowbox64.dll`） | 主仓库 gitlink pin，submodule `thirdparty/box64` | 按 SHA 取（当前 `e970ee6f2`，在分支 `ohos-wow64-smc-fs` 上） |
| FEX（`libarm64ecfex.dll` / `libwow64fex.dll`） | `thirdparty/fex` submodule + `scripts/patches/fex-*.patch` | 由 `make fex` / `scripts/build_fex.sh` 应用补丁后构建 |
| dxvk | submodule `thirdparty/dxvk` → fork 分支 **`feature/arm64-legacy`** | 含 WineHua present 观测(`DXVK_WINEHUA_PERF_DIAGNOSIS` / `WineHuaPresentImage` 时间线) |
| mesa / virglrenderer / dxvk-modern / libepoxy / vkd3d-proton | 各自 fork 的 gitlink pin | 与上游 fork 分支对应，见 [SUBMODULE_MAINTAINABILITY.md](SUBMODULE_MAINTAINABILITY.md) |
| guest gfx / guest vulkan / host vulkan | `make deps`（源码在 submodule 内） | |

> **最常踩的坑**：改了 Wine 源码但只提交在本地分支（历史上本地 `ohos-port` 与远端 `ohos-port`
> 历史无共同祖先，直接 push 会被拒）。正确做法是在 `ohos-port-steam-win64` 上提交并快进推送，
> 然后 `git add thirdparty/wine-valve` 更新主仓库 gitlink——否则别人 clone 到的还是旧内容。

### WSL 下拉取/推送

- 全局 `git config http.proxy = http://127.0.0.1:8080` 在 WSL 内不生效（代理在 Windows 侧），
  用网关地址：`git -c http.proxy=http://172.17.80.1:8080 <cmd>`。
- 直连 GitHub 取 ref 可以，但大 pack 上传会超时；推送统一走上面的网关代理。
- 本机若配置了 `url.https://github.com/.insteadOf git@github.com:`，SSH 地址会被改写成 HTTPS。

### 本地专属问题（不影响别人 clone）

- 本地某些 submodule 未初始化（`wayland` / `freetype` / `libdrm` / `gmp` / `gnutls` / `fex` 等）：
  依赖走预编译 `build/` 缓存，别人按上面第 1 步初始化即可。
- `thirdparty/dxvk` 曾经因为 `.git/modules/thirdparty/dxvk` 丢失而报
  `fatal: not a git repository`（2026-09-22 已重新初始化修复）。若再遇到：备份该目录 →
  删掉目录 → `git submodule update --init thirdparty/dxvk`。
- `thirdparty/wine-proton`、`build/` 下若干目录是历史遗留产物，不参与当前构建。

---

## Makefile 构建

### 完整构建

```bash
# 默认: x86_64 (方案①)
make

# arm64 原生 wine (方案③)
make NATIVE_ARCH=arm64-v8a

# box64+wine (方案②)
make NATIVE_ARCH=arm64-v8a WINE_ARCH=x86_64
```

### 单阶段构建

```bash
make deps                          # 交叉编译依赖 → build/sysroot-ext/（含 guest_gfx + guest_vulkan）
make wine                          # Wine + wineserver
make fex                           # FEX 转译 DLL (方案③, WINE_ARCH=aarch64)
make box64                         # Box64 box64.so (方案②, NATIVE_ARCH=arm64-v8a + WINE_ARCH=x86_64)
make box64-wow64                   # Box64 wowbox64.dll (方案③, 32 位应用 HODLL 引擎)
make native                        # 各架构原生 compositor 依赖
make dxvk                          # DXVK Legacy fork (x64 + x86 DLL)
make dxvk-modern                   # DXVK 2.6.2 fork
make vkd3d-proton                  # VKD3D-Proton (D3D12)
make host-vulkan                   # Host Vulkan exact replay 诊断模块
make assemble                      # 组装布局 (wine-data.zip)
make hap                           # HAP 打包 + 签名
```

> `assemble` 的依赖不少：`deps`、`wine`、`native`、`host-vulkan`、两套 DXVK（`dxvk` / `dxvk-modern`）和 `vkd3d-proton` 的产物、以及 smoke 载荷（`smoke-payload`）——缺任何一个都会在这里失败。改了对应源码就重跑那一阶段，再 `make hap`。

### 增量构建

增量判断基于 `build/.stamps/` 下的 stamp 文件 + `find -newer` 源码变更检测，日常命令见 `.claude/rules/build-and-log.md`：

```bash
# 只改 ArkTS / entry/src/main/cpp/ → 只需重打 HAP
make NATIVE_ARCH=arm64-v8a hap

# 改了 Wine C 源码 (thirdparty/wine/) → 必须完整构建 (assemble 需重新打包 wine-data.zip)
make NATIVE_ARCH=arm64-v8a

# 改了 native compositor 依赖 (thirdparty/virglrenderer 等)
make NATIVE_ARCH=arm64-v8a native && make NATIVE_ARCH=arm64-v8a hap

# 完全清理
make clean
```

### Stamp 文件

```
build/.stamps/
├── deps
├── dxvk-legacy                    # DXVK 1.10.3
├── dxvk-modern-2.6                # DXVK 2.6.2
├── vkd3d-proton-limited-500k      # VKD3D-Proton
├── wine-arm64-v8a
├── wine-x86_64
├── box64-arm64-v8a
├── arm64-v8a/
│   ├── native
│   ├── host-vulkan
│   └── assemble
└── x86_64/
    ├── native
    ├── host-vulkan
    └── assemble
```

删除对应 stamp 文件即可强制重跑该阶段。

---

## 环境变量

构建时关键变量（由 `Makefile` / `scripts/env.sh` 设置）：

| 变量 | 默认 | 说明 |
|------|------|------|
| `NATIVE_ARCH` | `x86_64` (Makefile) | `arm64-v8a` 或 `x86_64` 或 `all` |
| `GUEST_ARCH` | `x86_64` | guest 侧架构 |
| `OHOS_SDK` | `/apps/harmony/sdk/default/openharmony` | HarmonyOS SDK 路径 |
| `BUILD_GUEST_GFX` | `1` (Makefile) / `0` (脚本直跑) | 构建 guest Mesa (VirGL) |
| `BUILD_GUEST_VULKAN` | `1` (Makefile) | 构建 guest Vulkan 栈 (Loader + Venus ICD) |
| `BUILD_WINE_MONO` | `0` | 设为 `1` 构建实验性的 Wine Mono (.NET 运行时) 包 |
| `TARGET_SDK_VERSION` | `6.1.0(23)` | HAP SDK 版本 |

运行时变量（注入 Wine 子进程，见 `graphics_broker.cpp` / `wine_env.cpp` / `wine_child.cpp`）：

| 变量 | 作用 |
|------|------|
| `BOX64_LD_LIBRARY_PATH` | Box64 搜索 x86_64 .so 的路径 |
| `BOX64_DYNAREC_SAFEFLAGS` | Box64 兼容默认 `1`（steam_api64 等自修改代码必需） |
| `GALLIUM_DRIVER` | Mesa Gallium 驱动 (`virpipe` 启用 VirGL) |
| `LIBGL_DRIVERS_PATH` | Mesa DRI 驱动路径 |
| `VTEST_SOCKET_NAME` | VirGL socket 路径 |
| `WINEDEBUG` | Wine 调试频道 (`-all` 关闭) |
| `XKB_CONFIG_ROOT` | XKB 键盘布局数据路径 |
| `WINEHUA_VULKAN_PRESENT` | 选择 DXVK/Venus Vulkan present 路径 |
| `WINEHUA_VTEST_PRESENT_PERF_SUMMARY` | 默认 `0`，关闭周期性 present 日志 |
| `DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT` | 默认 `auto`，RGBA8 SNORM 兼容（Tomb Raider） |
| `WINEHUA_ZERO_COPY_READY_DIR` | VirGL zero-copy 握手目录 |
| `WINEHUA_RESOURCE_TRACE` | `1` 时启用资源身份跟踪 (capture profile) |

---

## 产物说明

### libs/（arm64-v8a，原生 .so）

```
entry/libs/arm64-v8a/
├── box64.so                       # Box64, in-process dlopen
├── libwineserver.so               # wineserver NCP 入口
├── libwinehua_vtest_server.so     # VirGL vtest server 入口
├── libvirglrenderer.so.1, libepoxy.so.0   # compositor 依赖
├── libwayland-{client,server,egl}.so.0    # wayland 库
├── libfreetype.so.6, libxkbcommon.so.0, libxkbregistry.so.0, libxml2.so.2
├── libffi.so.8
└── virgl_test_server              # VirGL host server (独立二进制)
```

### resources/rawfile/wine-data.zip（运行时解压）

```
wine-data.zip
├── bin/
│   ├── wine, wineserver, *.exe    # x86_64 ELF / exe stubs
│   ├── x86_64-windows/            # 64-bit PE DLL
│   ├── x86_64-unix/               # Unix .so（含 winevulkan）
│   ├── i386-windows/              # 32-bit PE DLL (WoW64)
│   ├── guest_gfx/                 # guest Mesa (VirGL) 库
│   ├── guest_vulkan/              # guest Vulkan Loader + Venus ICD (manifest.json)
│   └── host_vulkan/               # Host Vulkan exact replay (manifest.json)
├── dxvk/
│   ├── legacy/{x64,x86}/          # DXVK 1.10.3（d3d10 链 + d3d11 + dxgi）
│   ├── modern-2.6/{x64,x86}/      # DXVK 2.6.2（只有 d3d11 + dxgi，2.x 没有 d3d10 链）
│   └── manifest.json
├── vkd3d/
│   ├── limited-500k/              # VKD3D-Proton（D3D12）
│   └── manifest.json
├── smoke/
│   ├── x64/, x86/                 # winehua_*_smoke.exe 等受管测试
│   ├── assets/                    # SPIR-V 采样 shader
│   ├── suites.json                # 套件定义（跑哪些用例、声明什么档位）
│   └── manifest.json
├── audio/winehua-gm.sf2           # MIDI SoundFont
└── share/
    ├── wine/ (nls/, fonts/, wine.inf, mono/)
    └── X11/xkb/
```

rawfile 侧另有 `wine-runtime-manifest.json`（payloadSha256），CI 用它校验 HAP 内嵌运行时与 submodule commit 一致。

---

## 相关文档

- [env.md](env.md) — 从零搭建构建环境
- [CURRENT_STATUS.md](../archive/CURRENT_STATUS.md) — 当前功能状态
- [wine-internals.md](../architecture/wine-internals.md) — 项目架构概览
- [.claude/rules/build-and-log.md](../../.claude/rules/build-and-log.md) — 构建命令、stamp 机制与调试日志
- [.claude/rules/submodule-workflow.md](../../.claude/rules/submodule-workflow.md) — Submodule 管理方案

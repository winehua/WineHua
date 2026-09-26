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
# wayland-scanner: 无需预装，构建时自动编译到 build/host-tools/bin/
```

---

## 平台

| 平台 | Makefile 命令 | `NATIVE_ARCH` | Wine | Box64 | 打包 |
|------|-------------|---------------|------|-------|------|
| arm64 | `make NATIVE_ARCH=arm64-v8a` | `arm64-v8a` | x86_64 ELF → rawfile zip | box64.so (dlopen) | rawfile zip |
| x86_64 | `make NATIVE_ARCH=x86_64` | `x86_64` | Wine .so → libs/ | 无 | rawfile zip |
| 双架构 HAP | `make NATIVE_ARCH=all` | 双架构 | 同上 | arm64 含 Box64 | rawfile zip |

默认值：`NATIVE_ARCH=x86_64`（注意 `scripts/env.sh` 直接运行脚本时默认 `arm64-v8a`）。

> Wine 和 wineserver 通过 NCP（`OH_Ability_StartNativeChildProcess`）创建子进程。arm64 下 Box64 编译为 box64.so 由 NCP 子进程 dlopen 加载。

---

## Makefile 构建

### 完整构建

```bash
# 默认: x86_64
make

# arm64（当前调试目标）
make NATIVE_ARCH=arm64-v8a

# 双架构 HAP
make NATIVE_ARCH=all
```

### 单阶段构建

```bash
make deps                          # 交叉编译依赖 → build/sysroot-ext/（含 guest_gfx + guest_vulkan）
make wine                          # Wine + wineserver
make box64                         # Box64 ARM64 翻译器 (仅 arm64)
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
| `BUILD_WINE_MONO` | `1` | 设为 `0` 跳过 Wine Mono（.NET 运行时），缩小产物 |
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

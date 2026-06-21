# Wine for HarmonyOS — 构建指南

> 最后更新: 2026-06-22

---

## 环境

- WSL2 Ubuntu 26.04
- OHOS SDK (`/apps/harmony`)
- 设备: ARM64 / x86_64 HarmonyOS Pad/PC

---

## 平台速查

| 平台 | 构建命令 | Wine | Box64 | 打包 |
|------|---------|------|-------|------|
| arm64 PC | `bash build.sh quick <ip>` | x86_64 (musl) | box64 可执行文件 | HNP |
| x86_64 PC | `bash build.sh quick <ip> x86_64` | x86_64 (musl) | passthrough | HNP |
| arm64 Pad | `bash build.sh pad arm64` | x86_64 (musl) | box64.so | rawfile zip |
| x86_64 Pad | `bash build.sh pad x86_64` | x86_64 (musl) .so | 无 | rawfile zip |

PC 有 execve，Wine/Box64 编为可执行文件，HNP 打包。  
Pad 仅 fork、无 execve，Wine 文件放入 rawfile zip 运行时解压，Box64 编为 .so 由 NCP 子进程 dlopen。

---

## 子命令

### PC 命令 (有 execve, HNP 打包)

| 命令 | 说明 |
|------|------|
| `bash build.sh deps` | 构建交叉编译依赖 |
| `bash build.sh wine` | 构建 Wine |
| `bash build.sh box64` | 构建 Box64 (仅 arm64 需要) |
| `bash build.sh hap` | 编译 ArkTS + Native → .hap → 签名 |
| `bash build.sh deploy <ip>` | 推送 HAP + 安装 |
| `bash build.sh quick <ip>` | deps → wine → box64 → assemble → hnp → hap → deploy |

### Pad 命令 (fork-only, rawfile zip)

| 命令 | 说明 |
|------|------|
| `bash build.sh pad <arch>` | 完整构建 Pad HAP (arm64 / x86_64) |
| `bash build.sh pad-hap <arch>` | 仅 HAP (只改 ArkTS/native 时，跳过 Wine 重编译) |
| `bash build.sh pad-deploy <ip>` | 推送并安装 |

---

## 增量构建

### PC

```bash
# 只改 ArkTS
bash build.sh quick <device_ip>

# 改了 Wine C 源码 → 需要 make 重编 .so, 然后 quick
# (build.sh wine 会完整重编, 可手动增量 make 加速)
```

### Pad

```bash
# 只改 ArkTS / napi_init.cpp
bash build.sh pad-hap arm64

# 改了 Wine C 源码 → 必须先 build.sh pad arm64 完整构建

# 改了 Box64 C 源码
bash build.sh box64        # 重编 box64.so
bash build.sh pad-hap arm64 # 重新打包 HAP
```

---

## 产物说明

### PC (HNP 布局)

```
out/staging/opt/winehua/
├── bin/
│   ├── wine, wineserver, box64
│   ├── ntdll.so
│   ├── *.exe                  (PE stubs)
│   ├── x86_64-windows/        (PE DLL)
│   └── x86_64-unix/           (Unix .so)
├── lib/x86_64/
│   └── libc.so
└── share/
    ├── wine/ (nls/, fonts/, wine.inf)
    └── X11/xkb/
```

### Pad (rawfile zip + libs/)

```
entry/
├── libs/arm64-v8a/            (ARM64 原生 .so)
│   ├── box64.so               (Box64 .so, dlopen 加载)
│   ├── libwine_child.so       (NCP 子进程入口)
│   └── libentry.so            (NAPI 桥接)
├── libs/x86_64/               (x86_64 原生 .so, x86_64 Pad)
│   ├── ntdll.so, kernelbase.so, ...
│   └── libwineserver.so
└── resources/rawfile/
    └── wine-data.zip           (运行时解压)
        ├── bin/
        │   ├── wine, wineserver (x86_64 ELF)
        │   ├── ntdll.so
        │   ├── *.exe
        │   ├── x86_64-windows/
        │   └── x86_64-unix/
        └── share/
            ├── wine/
            └── X11/xkb/
```

arm64 Pad 下 Wine x86_64 .so 全部在 zip 内由 Box64 加载，libs/ 只放 ARM64 原生 .so。
x86_64 Pad 下 Wine .so 直接放 libs/ 由系统 linker 加载。

---

## 环境变量

构建时关键变量（由 `build.sh` 自动设置）：

| 变量 | PC 默认 | Pad 默认 | 说明 |
|------|---------|---------|------|
| `DEVICE_TYPE` | (空) | `pad` | 触发 assemble.sh 走 Pad 分支 |
| `NATIVE_ARCH` | `arm64-v8a` / `x86_64` | 同左 | 原生 .so 架构 |
| `WINE_SRC` | `thirdparty/wine` | 同左 | Wine 源码路径 |

运行时关键变量（由 `wine_child.cpp` / `napi_init.cpp` 设置）：

| 变量 | 作用 |
|------|------|
| `LIBBOX64_SO` | Box64 cmake flag，编 .so 时设为 ON |
| `USE_LIBBOX64` | 运行时标记，通知 Wine process.c 调整 entryParams |
| `BOX64_LD_LIBRARY_PATH` | Box64 搜索 x86_64 .so 的路径 |
| `BOX64_LOG` | Box64 日志级别 (0=关闭, 生产环境) |
| `WINEDEBUG` | Wine 调试频道 (-all=关闭) |
| `XKB_CONFIG_ROOT` | XKB 键盘布局数据路径 |

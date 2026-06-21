# WineHua — Wine on HarmonyOS

在 HarmonyOS (OpenHarmony) 设备上运行 Windows x86_64 程序，通过 Box64 指令翻译，嵌入 Wayland compositor 渲染窗口。

![运行效果](docs/images/run_wine_example.jpg)

## 支持平台

| 平台 | Wine | Box64 | 进程模型 | 打包方式 |
|------|------|-------|---------|---------|
| arm64 PC (emulator) | x86_64 (musl) | box64 可执行文件 | execve | HNP |
| arm64 Pad (emulator) | x86_64 (musl) | box64.so (dlopen) | NCP appspawn | rawfile zip |
| x86_64 PC emulator | x86_64 (musl) | - | execve | HNP |
| x86_64 Pad emulator | x86_64 (musl) .so | - | NCP appspawn | rawfile zip |

PC 设备有 execve 和 HNP 包管理；Pad 设备仅 fork、无 execve，通过 OHOS NativeChildProcess (appspawn) 启动子进程，产物放在 rawfile zip 中运行时解压。

## 架构

```
Windows PE (x86_64)
    ↓ Box64 (x86_64 → ARM64 指令翻译)
Wine (x86_64, musl libc)
    ↓ winewayland.drv
嵌入式 Wayland compositor (ARM64 原生, HAP 进程内)
    ├── WaylandServer (wl_compositor, xdg_shell, wl_seat)
    ├── InputManager  (鼠标/键盘事件注入)
    └── EglRenderer  (EGL/GLES → XComponent 上屏)
```

arm64 Pad 下 Box64 编译为 **共享库 (box64.so)**，由 NCP 子进程 dlopen 加载，通过 `box64_hmos_main()` 在同一进程内模拟执行 x86_64 Wine ELF。编译宏 `LIBBOX64_SO` 控制此模式，运行时通过 `USE_LIBBOX64` 环境变量通知 Wine 侧适配 entryParams 格式。

## 当前状态

| 功能 | 状态 |
|------|------|
| ARM64 Pad Box64 .so 方案 | ✅ |
| x86_64 Pad Wine .so 方案 | ✅ |
| Explorer 桌面 | ✅ |
| GUI 程序 (notepad 等) | ✅ |
| NAPI 沙箱运行 | ✅ |
| 多窗口 | ✅ |
| 鼠标 / 键盘输入 | ✅ |
| 音频 | ❌ |

## 构建

依赖 [Command Line Tools](https://developer.huawei.com/consumer/cn/download/) 命令行工具 (`hvigorw`)，通过 `TOOL_HOME` 环境变量定位。

```bash
# PC 构建 (execve + HNP)
bash build.sh quick <device_ip>          # arm64 PC (默认)
bash build.sh quick <device_ip> x86_64   # x86_64 PC

# Pad 构建 (fork-only, rawfile zip)
bash build.sh pad arm64                  # arm64 Pad
bash build.sh pad x86_64                 # x86_64 Pad

# Pad 增量构建 (只改 ArkTS/native 时)
bash build.sh pad-hap arm64
```

## 目录结构

```
WineHua/
├── entry/src/main/
│   ├── cpp/                   # Native C++: Wayland compositor, EGL, Input
│   │   ├── napi_init.cpp      # NAPI 桥接 + wine/wineserver/broker 进程管理
│   │   ├── wine_child.cpp     # NCP 子进程入口 (Main/WineserverMain)
│   │   ├── wayland_server.cpp # 嵌入式 Wayland compositor
│   │   ├── egl_renderer.cpp   # EGL/GLES 渲染器
│   │   ├── input_manager.cpp  # 鼠标/键盘事件注入
│   │   ├── plugin_manager.cpp # surfaceId 驱动 XComponent 管理
│   │   ├── xdg_shell.cpp      # xdg-shell 协议实现
│   │   └── seat.cpp           # wl_seat + keycode 映射
│   ├── ets/pages/
│   │   ├── Index.ets          # 主界面 (侧边栏 + 桌面)
│   │   ├── WineWindow.ets     # Wine 子窗口
│   │   └── DesktopWindow.ets  # 桌面窗口
│   └── ets/model/
│       └── WineWindowManager.ets  # 窗口生命周期管理
├── build.sh                   # 统一构建入口
├── scripts/
│   ├── env.sh                 # 环境变量 (NATIVE_ARCH, DEVICE_TYPE, 工具链)
│   ├── build_deps.sh          # 交叉编译依赖
│   ├── build_wine.sh          # Wine 构建
│   ├── build_box64.sh         # Box64 构建 (可执行文件 / .so)
│   ├── assemble.sh            # PC: HNP 布局 / Pad: rawfile zip + libs/
│   └── package.sh             # HNP/HAP 打包
├── docs/                      # 详细文档
│   ├── CURRENT_STATUS.md      # 当前状态 & 修复清单
│   ├── ARCHITECTURE.md        # 架构详解
│   └── BUILD_GUIDE.md         # 构建指南
└── thirdparty/                # git submodule
    ├── wine/                  # winehua/wine (fork)
    ├── box64/                 # winehua/box64 (fork)
    ├── freetype/
    ├── wayland/
    ├── libxkbcommon/
    └── xkeyboard-config/
```

## 关键适配

- **Box64**: x86_64 → ARM64 指令翻译 (Dynarec)，Pad 下编译为 .so (LIBBOX64_SO) 由 NCP 子进程 dlopen 加载
- **Wayland compositor**: HAP 进程内嵌入式 compositor，不依赖外部 Wayland 服务
- **NCP appspawn**: Pad 通过 `OH_Ability_StartNativeChildProcess` 创建子进程，绕过无 execve 限制
- **surfaceId 架构**: 通过 `XComponentController` 回调获取 surfaceId，解决多窗口冲突
- **noexec 文件系统**: 可执行段用匿名 mmap + pread 替代文件映射
- **dosdevices symlink**: OHOS 沙箱禁止 symlink()，fallback 到 drive_c
- **XKB 键盘**: xkeyboard-config 打包到 rawfile/HNP，通过 `XKB_CONFIG_ROOT` 指向

## 日志

所有 Wine 输出（stdout + stderr）重定向到 hilog + 文件：

```bash
# Pad 设备
hdc -t <device_ip> hilog | grep -E '\[wine\]|WineChild|WL_NAPI|WL_Input'
```

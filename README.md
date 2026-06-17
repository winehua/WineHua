# HonWine — Wine on HarmonyOS

在 HarmonyOS (OpenHarmony) 设备上运行 Windows x86_64 程序，通过 Box64 指令翻译 + 嵌入式 Wayland compositor。

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

每个 Wine toplevel 窗口对应一个独立的 HarmonyOS 子窗口，通过 `XComponentController` + `surfaceId` 驱动渲染。

## 当前状态

| 功能 | 状态 |
|------|------|
| 控制台程序 (cmd.exe) | ✅ |
| GUI 程序 (notepad.exe) | ✅ |
| NAPI 沙箱运行 | ✅ |
| 多窗口 | ✅ |
| 鼠标输入 | ✅ |
| 键盘输入 | ✅ |
| 音频 | ❌ |

## 构建

依赖 [DevEco Studio](https://developer.huawei.com/consumer/cn/deveco-studio/) 命令行工具 (`hvigorw`)，通过 `TOOL_HOME` 环境变量定位。

```bash
# 一键构建 + 部署
bash build.sh quick <device_ip>

# 仅构建 HAP
bash build.sh hap

# 部署到设备
bash build.sh deploy <device_ip>
```

## 目录结构

```
HonWine/
├── entry/src/main/
│   ├── cpp/                   # Native (C++): Wayland compositor, EGL, Input
│   │   ├── wayland_server.cpp # 嵌入式 Wayland compositor
│   │   ├── egl_renderer.cpp   # EGL/GLES 渲染器
│   │   ├── input_manager.cpp  # 鼠标/键盘事件注入
│   │   ├── plugin_manager.cpp # surfaceId 驱动 XComponent 管理
│   │   ├── xdg_shell.cpp      # xdg-shell 协议实现
│   │   └── seat.cpp           # wl_seat + keycode 映射
│   ├── ets/pages/
│   │   ├── Index.ets          # 主界面
│   │   └── WineWindow.ets     # Wine 子窗口
│   └── ets/entryability/
│       └── WineWindowAbility.ets  # 子窗口 UIAbility
├── build.sh                   # 统一构建入口
├── docs/                      # 详细文档
│   ├── CURRENT_STATUS.md      # 当前状态 & 修复清单
│   ├── ARCHITECTURE.md        # 架构详解
│   └── BUILD_GUIDE.md         # 构建指南
└── thirdparty/wine/           # Wine 源码 (git submodule)
```

## 关键适配

- **Box64**: x86_64 → ARM64 指令翻译，Dynarec 模式
- **Wayland compositor**: HAP 进程内嵌入式 compositor，不依赖外部 Wayland 服务
- **surfaceId 架构**: 不用 `libraryname`，通过 `XComponentController` 回调获取 surfaceId，彻底解决多窗口冲突
- **noexec 文件系统**: 可执行段用匿名 mmap + pread 替代文件映射
- **dosdevices symlink**: OHOS 沙箱禁止 symlink()，四条代码路径全覆盖 fallback
- **XKB 键盘**: xkeyboard-config 打包到 HNP，通过 `XKB_CONFIG_ROOT` 指向

## 日志

所有 Wine 输出（stdout + stderr）重定向到 hilog：

```bash
hdc -t <device_ip> hilog | grep '\[wine\]'
```

# Wine for HarmonyOS — 当前状态

> 更新: 2026-06-22
> 状态: ✅ ARM64 Pad Box64 .so | ✅ Explorer 桌面 | ✅ 输入 | ✅ 多窗口

---

## 里程碑

### 1. ARM64 Pad Box64 .so 方案 ✅

Box64 编译为共享库 (box64.so)，由 NCP 子进程 (`wine_child.so`) dlopen 加载，通过 `box64_hmos_main()` 在同一进程内模拟执行 x86_64 Wine ELF。编译宏 `LIBBOX64_SO` 控制，运行时环境变量 `USE_LIBBOX64` 标记。

### 2. x86_64 Pad Wine .so 方案 ✅

x86_64 原生 Wine .so 由系统 linker 直接加载，无 Box64。

### 3. Wine prefix 初始化 ✅

`wineboot --init` 完整生成 drive_c 目录结构（windows/system32, users, Program Files 等）和 registry。

### 4. Explorer 桌面 ✅

`explorer /desktop=shell,<w>x<h>` 桌面窗口通过 Wayland compositor 渲染上屏。

### 5. 控制台 / GUI 程序 ✅

cmd.exe, notepad.exe 等正常运行。

### 6. 输入框架 ✅

鼠标/键盘事件 ArkTS → NAPI → InputManager → Wayland → Wine 完整链路。  
XKB 键盘布局数据通过 `XKB_CONFIG_ROOT` 环境变量指向。

### 7. NAPI 沙箱 ✅

app.hackeris.winehua HAP 在 NAPI 沙箱中运行，dosdevices symlink 不可用全覆盖修复。

---

## 当前架构

```
NCP 子进程 (appspawn)
  wine_child.so: Main()
    ├─ arm64 Pad: dlopen("box64.so") → box64_hmos_main() → Wine x86_64 ELF
    └─ x86_64 Pad: dlopen("ntdll.so") → __wine_main()
          ↓
    Wine + Box64 (arm64) / Wine native (x86_64)
          ↓ winewayland.drv
    嵌入式 Wayland compositor (ARM64 原生, HAP 内)
          ↓ EGL/GLES
    XComponent 上屏
```

---

## 已修复问题

| # | 问题 | 修复方案 |
|---|------|---------|
| 1 | TEB 分配崩溃 | `anon_mmap_alloc()` fallback |
| 2 | Noexec 文件系统 PROT_EXEC | 匿名 mmap + pread |
| 3 | prctl(0x6a6974) SIGSEGV | Box64 my_prctl 清零 r10/r8 |
| 4 | PR_SET_NAME fallthrough SIGSEGV | Box64 my_prctl 直接 return 0 |
| 5 | Box64 fork 子进程 RWX 失败 | NCP appspawn + .so dlopen 替代 fork |
| 6 | wineserver ARM64 架构不匹配 | ARM64 Pad 下编 x86_64 PIE，Box64 加载 |
| 7 | entryParams `\|wine\|` 多余 | `USE_LIBBOX64` 环境变量按需跳过 |
| 8 | Box64 DEBUG 日志 I/O 过慢 | BOX64_LOG=0, WINEDEBUG=-all |
| 9 | dosdevices symlink 不可用 | 四条代码路径 fallback |
| 10 | XKB 键盘数据缺失 | xkeyboard-config 打包 + XKB_CONFIG_ROOT |
| 11 | 鼠标 action 常量错误 | 对齐 ArkTS MouseAction |
| 12 | dxg_surface use-after-free | wl_resource destroy 回调 + pending auto-destroy |
| 13 | 多窗口 XComponent exports 冲突 | surfaceId + XComponentController |
| 14 | Terminal 终端页面 | 已删除 (功能不完整) |

---

## 已知问题

| 问题 | 影响 | 说明 |
|------|------|------|
| libfreetype.so.6 未找到 | 位图字体回退 | Box64 搜索路径问题，非致命 |
| libxkbcommon/xkbregistry/xml2 未找到 | 部分输入功能受限 | 同上，需调查 BOX64_LD_LIBRARY_PATH |
| xkbcommon include path 错误 | XKB 加载失败日志 | `/usr/share/X11/xkb` 应为 rawfile 解压路径 |
| services.exe 无法启动 | 非阻塞 | 错误 267 (目录无效) |
| dnsapi / nsiproxy 编译错误 | 跳过 | mingw 交叉编译 musl 不兼容 |
| 音频 | 无 | 未实现 |

---

## Pad 产物布局

```
entry/
├── libs/arm64-v8a/             (ARM64 原生 .so, Pad arm64)
│   ├── box64.so                (Box64 .so)
│   ├── libwine_child.so        (NCP 入口)
│   ├── libentry.so             (NAPI)
│   └── libwineserver.so        (仅 x86_64 Pad)
└── resources/rawfile/
    └── wine-data.zip
        ├── bin/{wine, wineserver, ntdll.so, *.exe,
        │       x86_64-windows/, x86_64-unix/}
        └── share/{wine/, X11/xkb/}
```

---

## 构建命令

```bash
# PC (execve + HNP)
bash build.sh quick <ip>          # arm64
bash build.sh quick <ip> x86_64   # x86_64

# Pad (fork-only + rawfile)
bash build.sh pad arm64
bash build.sh pad x86_64
```

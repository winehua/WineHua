# Wine for HarmonyOS — 当前状态

> 更新: 2026-06-18
> 状态: ✅ 键盘输入 | ✅ Wayland 渲染上屏 | ✅ NAPI 沙箱 | ✅ 多窗口

---

## 里程碑

### 1. 控制台程序 ✅

`cmd.exe /c echo hello` 正常输出。

### 2. GUI 程序 ✅

notepad.exe 窗口通过 Wayland 协议 → 嵌入式 compositor → EGL/GLES → 鸿蒙 XComponent 上屏。

### 3. NAPI 沙箱 ✅

app.hackeris.honwine HAP 在 NAPI 沙箱中启动 Wine，dosdevices symlink 不可用已全覆盖修复。

### 4. 多窗口 (multiton) ✅

每个 Wine toplevel 对应一个独立 HarmonyOS TYPE_MAIN 窗口，任务栏独立条目、独立生命周期。

### 5. 输入框架 ✅

#### 鼠标 ✅
- ArkTS XComponent `.onMouse` → NAPI → InputManager → Wayland → Wine
- action 常量对齐 ArkTS MouseAction (Press=1, Release=2, Move=3)
- 坐标转换、多点触控映射为鼠标左键

#### 键盘 ✅
- ArkTS XComponent `.onKeyEvent` → NAPI → InputManager → Wayland → Wine
- OHOS keycode → evdev 映射 (120+ 键)
- XKB keymap 正确打包到 HNP (`XKB_CONFIG_ROOT` 指向 `/opt/honwine/share/X11/xkb`)
- 首键自动 keyboard enter

---

## 架构概览

```
Wine (x86_64, Box64)
  └── winewayland.drv → Wayland socket
        └── 嵌入式 compositor (HAP ARM64)
              ├── wl_compositor, wl_shm, xdg_wm_base
              ├── wl_subcompositor, wp_viewporter
              ├── wl_seat (pointer + keyboard)
              └── EGL Renderer → XComponent
```

---

## 核心修复清单

### dosdevices symlink 不可用 🔑

OHOS NAPI 沙箱禁止 `symlink()`。四条代码路径全覆盖：
- 前向映射 (NT→Unix): `nt_to_unix_file_name_no_root`
- 反向映射 (Unix→NT): `unix_to_nt_file_name`
- `\??\unix\` 快速通道: `get_nt_and_unix_names`
- Unix 路径 null 终止修复

### noexec 文件系统

HNP 安装目录挂载为 noexec，PE DLL 代码段无法加 PROT_EXEC。修复：可执行段用匿名 mmap + pread 替代文件支持映射。

### XKB 键盘数据

Wine 键盘驱动初始化需要 `/usr/share/X11/xkb`。xkeyboard-config 通过 git submodule + meson 构建，打包到 HNP，运行时通过 `XKB_CONFIG_ROOT` 环境变量指向。

---

## 已修复问题

| # | 问题 | 修复方案 |
|---|------|---------|
| 1 | TEB 分配崩溃 | `anon_mmap_alloc()` fallback |
| 2 | 路径计算错误 | `--prefix=/opt/honwine --libdir='${prefix}'` |
| 3 | Noexec 文件系统 PROT_EXEC | 匿名 mmap + pread |
| 4 | PE DLL 路径 | `bin/x86_64-windows/` |
| 5 | `MAP_FIXED_NOREPLACE` ENOSYS | `#undef` 走 fallback |
| 6 | Null Driver 窗口创建 | `nulldrv_CreateWindow` |
| 7 | ntdll socket 路径 | `-D__ANDROID__` → `~/.wine/` |
| 8 | dosdevices symlink | 四条代码路径 fallback |
| 9 | 鼠标 action 常量 | 对齐 ArkTS MouseAction |
| 10 | 键盘 enter 时机 | 首键自动 enter |
| 11 | Wine 键盘 init 失败 | XKB 数据打包 + XKB_CONFIG_ROOT |

---

## 已知问题

| 问题 | 状态 | 说明 |
|------|------|------|
| dnsapi / nsiproxy musl 不兼容 | 编译时跳过 | 不影响基础功能 |
| services.exe 无法启动 | 非阻塞 | 不影响 notepad |
| FreeType dlopen 失败 | Box64 需 ARM64 原生 .so | 位图字体可用 |
| Box64 fork 子进程 RWX 失败 | EINVAL | wineserver 用 NAPI 原生 fork 绕过 |

---

## HNP 布局

```
out/staging/opt/honwine/
├── bin/
│   ├── wine, wineserver, box64
│   ├── ntdll.so               # wine loader 同目录加载
│   ├── *.exe                  # PE stubs (109)
│   ├── x86_64-windows/        # PE DLL (619)
│   └── x86_64-unix/           # Unix .so (30)
├── lib/x86_64/
│   └── libc.so                # musl 运行时
└── share/
    ├── wine/
    │   ├── nls/               # NLS 文件
    │   ├── fonts/             # 内置 13 个 .ttf
    │   └── wine.inf
    └── X11/xkb/               # XKB 键盘布局数据
```

---

## 构建方式

```bash
# 一键构建+部署
bash build.sh quick <device_ip>

# 仅构建 HAP
bash build.sh hap
```

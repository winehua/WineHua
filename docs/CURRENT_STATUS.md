# Wine for HarmonyOS — 当前状态

> 更新: 2026-06-14
> 状态: ✅ cmd.exe 通过 | ✅ notepad.exe GUI | ✅ NAPI 沙箱 | ✅ Wayland 渲染上屏

---

## 里程碑

### 1. 控制台程序 ✅

`WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/cmd.exe /c echo hello`

输出 `hello`

### 2. GUI 程序 (headless BMP) ✅

`WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/notepad.exe`

窗口正确渲染至 offscreen BMP，文字正常显示。

### 3. NAPI 沙箱中运行 ✅

app.hackeris.honwine HAP 在 NAPI 沙箱中启动 Wine，notepad.exe 成功输出窗口位图。

dosdevices 符号链接不可用（OHOS 不允许 symlink），通过 `__OHOS__` 编译标志实现全覆盖 fallback。

### 4. Wayland 渲染到鸿蒙原生 XComponent ✅

Wine 窗口内容通过 Wayland 协议 → 嵌入式 compositor → EGL/GLES → 鸿蒙 XComponent 上屏。

**架构**: Wine(winewayland.drv) → Wayland socket → 嵌入式 compositor (HAP ARM64) → EGL Renderer → XComponent

**关键修复**:
- PE 侧 `winewayland.drv` 构建 + 打包
- `wl_subcompositor` + `wp_viewporter` + `wl_output` compositor 协议实现
- OHOS 驱动加载绕过 PnP（`WAYLAND_DISPLAY` 直接触发 `NtUserLoadDriver`）
- `LD_LIBRARY_PATH` 传入 envp（winewayland.so 的 ELF 依赖链需要动态链接器查找）

---

## 核心修复: dosdevices symlink 不可用 🔑

OHOS NAPI 沙箱禁止 `symlink()` 系统调用 (EACCES)。Wine 依赖 `dosdevices/c:` 和 `dosdevices/z:` 符号链接将 NT 路径 `\??\C:\...` 映射到 Unix 路径。修复覆盖了四条代码路径：

### 1. 前向映射 (NT→Unix) — `nt_to_unix_file_name_no_root`

**文件**: `dlls/ntdll/unix/file.c`

驱动器字母前缀 (`C:`→`Z:`) 通过 `lstat()` 检测 symlink 是否存在。若不存在，直接用硬编码映射：
- `C:` → `$WINEPREFIX/drive_c`
- `Z:` → `/`

### 2. 反向映射 (Unix→NT) — `unix_to_nt_file_name`

**文件**: `dlls/ntdll/unix/file.c`

`find_drive_nt_root()` 在 nb_drives=0 时返回 `STATUS_OBJECT_PATH_NOT_FOUND`。修复后允许此状态通过，fallback 到 `\??\unix\` 路径。**关键**: fallback 后 `status` 设为 `STATUS_SUCCESS`，确保 `add_path_var()` 能成功设置 `WINEDLLDIR0`。

### 3. `\??\unix\` 快速通道 — `get_nt_and_unix_names`

**文件**: `dlls/ntdll/unix/file.c`

`get_nt_and_unix_names()` 对 `\??\unix\` 路径有快速通道：直接提取 Unix 路径，然后调用 `find_drive_nt_root()` 尝试映射回 DOS 驱动。修复：`find_drive_nt_root()` 失败时不阻止 `NtCreateFile` 调用 `open_unix_file()` —— 状态改为 `STATUS_SUCCESS`。

### 4. Unix 路径 null 终止

**文件**: `dlls/ntdll/unix/file.c`

`ntdll_wcstoumbs()` 转换 WCHAR→multibyte 后没有显式 null 终止。`malloc` 不保证零初始化，导致 `strlen()` 读到随机垃圾字符。修复：`unix_name[lenA] = 0;`

### 其他支持修复

**文件**: `dlls/ntdll/unix/server.c`

`setup_config_dir()`: symlink 失败时 `fd_cwd` fallback 到 `open("drive_c", O_RDONLY)`。

### 编译标志

所有修复用 `-D__OHOS__` 隔离，与上游 `__ANDROID__` 路径互不干扰。

---

## 已修复的问题 (8/8)

### 1. TEB 分配崩溃
**文件**: `dlls/ntdll/unix/virtual.c`
Box64 39 位地址空间下 `MEM_TOP_DOWN` 行为异常，fallback 到 `anon_mmap_alloc()` 直接 mmap。

### 2. 路径计算错误
**configure**: `--prefix=/opt/winebox --libdir='${prefix}'`

### 3. Noexec 文件系统
**文件**: `dlls/ntdll/unix/virtual.c`
可执行 section 用匿名 mmap + pread，`mprotect_exec()` 用 `prctl` 绕过。

### 4. PE DLL 路径
PE DLL 放入 `bin/x86_64-windows/`

### 5. MAP_FIXED_NOREPLACE ENOSYS
Box64 wrapped mmap 不支持，`#undef MAP_FIXED_NOREPLACE` 走 fallback。

### 6. Null Driver 窗口创建
**文件**: `dlls/win32u/driver.c`
`null_user_driver.pCreateWindow = nulldrv_CreateWindow`

### 7. ntdll `__ANDROID__` 路径
**文件**: `dlls/ntdll/unix/server.c`
编译时加 `-D__ANDROID__`，socket 路径走 WINEPREFIX。

### 8. dosdevices symlink 不可用 (NAPI 沙箱) 🔑
**文件**: `dlls/ntdll/unix/file.c`, `dlls/ntdll/unix/server.c`
四条代码路径覆盖前向/反向映射，`\??\unix\` 快速通道，null 终止修复。

---

## 字体渲染

- **位图字体** ✅ — Wine 内置 13 个 .ttf 字体已打包
- **FreeType 矢量字体** ⚠️ — Box64 dlopen 需要 ARM64 原生 .so

---

## 已知问题

### dnsapi / nsiproxy: musl 不兼容 (编译时跳过)

| 文件 | 问题 |
|------|------|
| `dlls/dnsapi/libresolv.c` | musl 没有 `_res` 全局变量 |
| `dlls/nsiproxy.sys/tcp.c`, `udp.c` | musl 没有 `sys/socketvar.h` |

当前 `make -k` 跳过，不影响基础功能。

### services.exe 无法启动

wineboot 执行 wine.inf 时 `start_services_process error 267`，不影响 notepad 运行。

### FreeType dlopen 失败

`[BOX64] Error initializing native libfreetype.so.6` — Box64 需要 ARM64 原生 .so。

---

## 完整源码修改清单

| 文件 | 修改 | 原因 |
|------|------|------|
| `dlls/ntdll/unix/virtual.c` | `#undef MAP_FIXED_NOREPLACE` | Box64 mmap 不支持 |
| `dlls/ntdll/unix/virtual.c` | `virtual_alloc_first_teb()` 移除 MEM_TOP_DOWN | Box64 39位地址空间 |
| `dlls/ntdll/unix/virtual.c` | `virtual_alloc_thread_data()` fallback | Box64 map_view 失败 |
| `dlls/ntdll/unix/virtual.c` | `mprotect_exec()` prctl 绕过 | noexec 文件系统 |
| `dlls/ntdll/unix/virtual.c` | `map_image_into_view()` 匿名 mmap+pread | noexec 文件系统 |
| `dlls/win32u/driver.c` | `nodrv` → `nulldrv_CreateWindow` | 允许无头窗口创建 |
| `dlls/win32u/dce.c` | BMP dump | offscreen 渲染验证 |
| `dlls/ntdll/unix/server.c` | `setup_config_dir()`: fd_cwd fallback + `__ANDROID__` socket 路径 | OHOS: symlink 不可用 + socket 路径 |
| `dlls/ntdll/unix/file.c` | `nt_to_unix_file_name_no_root()`: OHOS 前向映射 fallback | OHOS: C:/Z: 无 symlink 时直接映射 |
| `dlls/ntdll/unix/file.c` | `get_dos_device()`: OHOS 驱动器 fallback | OHOS: 无 symlink 时硬编码映射 |
| `dlls/ntdll/unix/file.c` | `unix_to_nt_file_name()`: `\??\unix\` fallback + STATUS_SUCCESS | OHOS: 反向映射兜底 + WINEDLLDIR0 设置 |
| `dlls/ntdll/unix/file.c` | `get_nt_and_unix_names()`: `\??\unix\` 快速通道不因 find_drive_nt_root 失败阻塞 | OHOS: DLL 文件从 Wine 安装目录加载 |
| `dlls/ntdll/unix/file.c` | `get_nt_and_unix_names()`: `ntdll_wcstoumbs` 后 null 终止 | 修复随机垃圾字符导致文件找不到 |
| `dlls/ntdll/unix/env.c` | `add_path_var()`: 诊断日志 (已注释) | 诊断环境变量设置 |
| `dlls/ntdll/loader.c` | PE 侧 DLL 搜索诊断日志 (已注释) | 诊断 PE 侧 DLL 搜索 |
| `build-ohos/` configure | `--prefix=/opt/winebox --libdir='${prefix}'` | 路径计算 |
| `build-ohos/` 编译 | `-D__ANDROID__ -D__OHOS__` | OHOS 平台编译标志 |
| `build-ohos/` configure | 启用 FreeType (`SONAME_LIBFREETYPE`) | 字体渲染 |
| `napi_init.cpp` | 环境变量 (HOME, PATH, WINEDEBUG 等) | 对齐终端环境 |
| `server/main.c` | `#define PACKAGE_VERSION` via config.h | OHOS 交叉编译配置补全 |

---

## 构建 configure 参数

```bash
--host=x86_64-linux-ohos
--prefix=/opt/winebox
--libdir='${prefix}'
--with-wine-tools=../build-native
--with-mingw=gcc
--disable-tests
--without-x --without-alsa --without-opengl --without-vulkan
```

FreeType 交叉编译: `cmake -DOHOS_ARCH=x86_64 -DBUILD_SHARED_LIBS=ON`，SONAME=`libfreetype.so.6`

## HNP 布局

```
out/staging/opt/winebox/
├── bin/
│   ├── wine                  # musl loader
│   ├── wineserver            # x86_64 musl
│   ├── box64                 # ARM64 native
│   ├── ntdll.so              # ⚠️ 必须在 bin/，wine loader 同目录加载
│   ├── *.exe                 # PE stubs (109个)
│   ├── libfreetype.so        # FreeType 矢量字体引擎
│   ├── libfreetype.so.6      #   (soname, Box64 dlopen 暂不可用)
│   ├── libz.so               # FreeType 依赖
│   ├── x86_64-windows/       # PE DLL (615个)
│   └── x86_64-unix/          # Unix .so (30个) — load_builtin_unixlib 拼接路径
├── lib/x86_64/
│   └── libc.so               # musl 运行时
└── share/wine/
    ├── nls/                  # NLS 文件
    ├── fonts/                # 内置 13 个 .ttf 字体
    └── wine.inf              # 前缀初始化脚本
```

| 路径 | 加载方式 |
|------|---------|
| `ntdll.so` | `bin/` — wine loader `dlopen("ntdll.so")` |
| `kernel32.dll` 等 | `bin/x86_64-windows/` — WINEDLLDIR0→`\??\unix\` 路径→`open_unix_file()` |
| `ws2_32.so` 等 | `bin/x86_64-unix/` — `load_builtin_unixlib` 拼接 `dll_dir + "/x86_64-unix/" + name` |

---

## 构建方式

```bash
# 完整构建 (Wine + Box64 + 打包 + 部署)
bash scripts/build_all.sh full

# 分步骤
bash scripts/build_all.sh wine    # 只构建 Wine
bash scripts/build_all.sh box64   # 只构建 Box64
bash scripts/assemble.sh          # 组装 HNP 布局
bash scripts/package.sh hnp       # 打包 HNP
bash scripts/package.sh hap       # 打包 HAP + 签名
bash scripts/package.sh deploy    # 推送到设备并安装
```

## 设备测试命令

```bash
cd /data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox
rm -rf /data/local/tmp/.wine
WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/cmd.exe /c echo hello 2>&1
```

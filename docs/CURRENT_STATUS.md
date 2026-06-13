# Wine for HarmonyOS — 当前状态

> 更新: 2026-06-13
> 状态: ✅ `cmd.exe /c echo hello` 通过 | ✅ notepad.exe GUI 渲染通过

---

## 里程碑

### 1. 控制台程序 ✅

`WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/cmd.exe /c echo hello`

输出 `hello`

### 2. GUI 程序 (headless) ✅

`WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/notepad.exe`

窗口正确渲染至 offscreen BMP，文字正常显示（不再是大黑块）。

BMP 输出路径: `/storage/Users/currentUser/workspace/wine/wine_surface_*.bmp`

---

## 已修复的问题 (7/7)

### 1. TEB 分配崩溃

**文件**: `dlls/ntdll/unix/virtual.c`

**症状**: Box64 下 `wine cmd.exe` SIGSEGV，crash 在 `virtual_alloc_first_teb + 0x4ea`

**根因**: `NtAllocateVirtualMemory(MEM_TOP_DOWN)` 在 Box64 39 位地址空间下行为异常；`map_view()` 返回 `STATUS_NO_MEMORY`，导致 `thread_data` 为 NULL 写入崩溃。

**修复**:
1. `virtual_alloc_first_teb()` — 移除 `MEM_TOP_DOWN` 标志
2. `virtual_alloc_thread_data()` — fallback 到 `anon_mmap_alloc()` 直接 mmap

**结果**: ✅

---

### 2. 路径计算错误

**根因**: `--prefix=/usr/local --libdir=/usr/local/lib` 导致运行时 `build_relative_path()` 多一层 `..`

**修复**: 重新 configure: `--prefix=/opt/winebox --libdir='${prefix}'`

**结果**: ✅ 路径正确: `/opt/winebox/bin`, `/opt/winebox/share/wine`

---

### 3. Noexec 文件系统

**文件**: `dlls/ntdll/unix/virtual.c`

**症状**: `map_image_into_view failed to set ... protection on ... section .text, noexec filesystem?`

**修复**: 
- 可执行 section 用匿名 mmap + pread（不走文件 mmap）
- `mprotect_exec()` 用 `prctl(0x6a6974)` 绕过 noexec

**结果**: ✅

**深入分析**: [NOEXEC_MMAP_ANALYSIS.md](./NOEXEC_MMAP_ANALYSIS.md)

---

### 4. PE DLL 路径

**症状**: `wine: failed to load ntdll.dll error c0000135`

**修复**: PE DLL 放入 `bin/x86_64-windows/`

**结果**: ✅

---

### 5. MAP_FIXED_NOREPLACE ENOSYS

**文件**: `dlls/ntdll/unix/virtual.c`

**症状**: `try_map_free_area mmap() error Function not implemented` — Box64 wrapped mmap 不支持 `MAP_FIXED_NOREPLACE`

**修复**: `anon_mmap_tryfixed()` 开头 `#undef MAP_FIXED_NOREPLACE`，强制走 `MAP_FIXED|MAP_EXCL` fallback

**结果**: ✅

---

### 6. Null Driver 窗口创建

**文件**: `dlls/win32u/driver.c`

**症状**: 无 X11/Wayland 时 GUI 程序无法创建窗口

**修复**: `null_user_driver.pCreateWindow = nulldrv_CreateWindow`（原为 `nodrv_CreateWindow`），允许在无显示驱动时创建窗口。Wine 自动 fallback 到 offscreen DIB 位图。

**结果**: ✅ notepad.exe 窗口正常创建

---

### 7. ntdll `__ANDROID__` 路径

**文件**: `dlls/ntdll/unix/server.c`

**症状**: wineserver socket 目录错误访问 `/tmp/.wine-<UID>/`（OHOS 无 /tmp 权限）

**根因**: `ntdll.so` 编译时缺少 `-D__ANDROID__`，走了 `/tmp` fallback 路径而非 WINEPREFIX 下的 `.wineserver/`

**修复**: `WINE_CFLAGS` 加入 `-D__ANDROID__`，全 Wine .so 生效

**结果**: ✅ socket 路径正确: `$WINEPREFIX/.wineserver/server-...`

---

## 字体渲染

### 状态: 部分可用

- **位图字体** ✅ — Wine 内置 13 个 .ttf 字体已打包，文字正常渲染
- **FreeType 矢量字体** ⚠️ — 已交叉编译并打包 `.so`，但 Box64 dlopen 机制需要 ARM64 原生版本，当前未生效

后续方向: 编译 ARM64 原生 FreeType，或静态链接进 win32u.so。

---

## 已知问题

### ⚠️ kernel32.dll 加载失败: symlink 权限不足 (EACCES)

**状态**: 根因已确认，修复方案待实施

**症状**: NAPI 启动 Wine 时 `could not load kernel32.dll, status c0000135`

**根因链**:
1. Wine 首次启动时 `setup_config_dir()` (`dlls/ntdll/unix/server.c:1353`) 调用 `symlink()` 创建 `dosdevices/c:` 和 `dosdevices/z:`
2. NAPI 沙箱中 `symlink()` 返回 `errno=13 (EACCES)` — 应用没有创建符号链接的权限
3. `dosdevices/` 目录为空 → `get_drives_info()` 返回 `nb_drives=0`
4. 所有 DOS 驱动映射 (`\??\C:\`, `\??\Z:\`) 无法解析
5. `search_dll_file` 和 `find_builtin_without_file` 都失败 → kernel32 加载失败

**日志证据**:
```
get_drives_info: nb_drives=0                                          # 无任何驱动
[symlink-test] symlink() FAILED: errno=13 (EACCES)                    # NAPI 沙箱禁止 symlink
```

**终端对比**: 终端运行在同一 WINEPREFIX，`dosdevices/c:` 和 `z:` 已由之前运行创建，`nb_drives=2`，正常工作。

**修复方向**:
- A) HNP 打包时预创建 dosdevices 目录结构（符号链接在打包时创建，运行时直接使用）
- B) Wine 源码中 `symlink()` 失败时 fallback 到普通文件格式的 dosdevice
- C) 全面使用 `\??\unix\` 路径绕过 dosdevices 依赖

**相关文件**: `dlls/ntdll/unix/server.c:1349-1355`, `dlls/ntdll/unix/file.c:2322-2365`

### dnsapi / nsiproxy: musl 不兼容 (编译时跳过)

| 文件 | 问题 |
|------|------|
| `dlls/dnsapi/libresolv.c` | musl 没有 `_res` 全局变量 |
| `dlls/nsiproxy.sys/tcp.c`, `udp.c` | musl 没有 `sys/socketvar.h` |

当前 `make -k` 跳过，不影响基础功能。后续需要源码修复。

### FreeType dlopen 失败

`[BOX64] Error initializing native libfreetype.so.6` — Box64 需要 ARM64 原生 .so，当前打包的是 x86_64 版本。

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
| `dlls/win32u/dce.c` | BMP dump (first 5 flushes) | offscreen 渲染验证 |
| `build-ohos/` configure | `--prefix=/opt/winebox --libdir='${prefix}'` | 路径计算 |
| `build-ohos/` 编译 | `-D__ANDROID__` | /tmp → WINEPREFIX socket |
| `build-ohos/` configure | 启用 FreeType (`SONAME_LIBFREETYPE`) | 字体渲染 |
| `dlls/ntdll/unix/file.c` | `get_nt_and_unix_names()`: `\??\unix\` 路径 `find_drive_nt_root` 失败时 `status=SUCCESS` | OHOS: 无 dosdevices 时 `\??\unix\` 兜底 |
| `dlls/ntdll/unix/file.c` | `unix_to_nt_file_name()`: 允许 `STATUS_OBJECT_PATH_NOT_FOUND` 走 `\??\unix\` fallback | OHOS: 空 WINEPREFIX fallback |
| `dlls/ntdll/unix/file.c` | `get_drives_info()`: 增加 ERR 日志 | 诊断 dosdevices 扫描 |
| `dlls/ntdll/unix/file.c` | `get_dos_device()`: 增加 ERR 日志 | 诊断 DOS 设备解析 |
| `dlls/ntdll/unix/file.c` | `nt_to_unix_file_name_no_root()`: 增加 ERR 日志 | 诊断 NT→Unix 路径转换 |
| `dlls/ntdll/unix/file.c` | `unix_to_nt_file_name()`: fallback 后 `status=STATUS_SUCCESS` | OHOS: 修复 WINEDLLDIR 等环境变量未设置 |
| `dlls/ntdll/unix/server.c` | `setup_config_dir()`: symlink 失败时打印 errno | 诊断 NAPI 沙箱 symlink 权限 |
| `dlls/ntdll/unix/env.c` | `add_path_var()`: 增加 ERR 日志 | 诊断环境变量设置 |
| `dlls/ntdll/loader.c` | 全路径 ERR 日志: find_dll_file / search_dll_file / open_dll_file / open_known_dll | 诊断 PE 侧 DLL 搜索 |
| `dlls/ntdll/unix/loader.c` | `find_builtin_dll()`: 增加 ERR 日志 | 诊断 builtin DLL 搜索 |
| `box64/src/main.c` | BOX64-ENV dump: CWD/exe/argv/env | 诊断两边的环境差异 |
| `napi_init.cpp` | 环境变量: HOME=/storage/Users/currentUser, 添加 PATH, TMPDIR | 对齐终端环境 |
| `napi_init.cpp` | symlink 权限测试: 两个测试用例 + errno 输出 | 确认 NAPI 沙箱 symlink 权限 |
| `server/main.c` | `#define PACKAGE_VERSION` via config.h | OHOS 交叉编译配置补全 |
| `build-ohos/include/config.h` | `PACKAGE_VERSION="10.0"`, `PACKAGE_STRING="Wine 10.0"` | OHOS 交叉编译配置补全 |

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
# --without-freetype 已移除，改用 FreeType
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
│   └── x86_64-unix/          # Unix .so (21个) — load_builtin_unixlib 拼接路径
├── lib/x86_64/
│   └── libc.so               # musl 运行时
└── share/wine/
    ├── nls/                  # NLS 文件
    └── fonts/                # 内置 13 个 .ttf 字体
```

**重要**: `ntdll.so` 必须在 `bin/`，其他 .so 在 `x86_64-unix/`。

| 文件 | 位置 | 加载方式 |
|------|------|---------|
| `ntdll.so` | `bin/` | wine loader `dlopen("ntdll.so")` (loader/main.c:161) |
| `ws2_32.so` 等 | `bin/x86_64-unix/` | `load_builtin_unixlib` 拼接 `dll_dir + "/x86_64-unix/" + name` |
| `libfreetype.so` | `bin/x86_64-unix/` | win32u.so 内 `dlopen("libfreetype.so.6")` (当前 Box64 限制不可用) |

---

## 构建方式

```bash
# 完整构建 (Wine + Box64 + 打包 + 部署)
./build.sh all

# 分步骤
./build.sh wine         # 只构建 Wine
./build.sh box64        # 只构建 Box64
./build.sh assemble     # 组装 HNP 布局
./build.sh hnp          # 打包 HNP
./build.sh hap          # 打包 HAP + 签名
./build.sh deploy       # 推送到设备并安装
./build.sh quick        # assemble → hnp → hap → deploy
```

---

## 设备测试命令

### 控制台

```bash
cd /data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox
rm -rf /data/local/tmp/.wine
WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/cmd.exe /c echo hello 2>&1
```

### GUI (notepad)

```bash
cd /data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox
killall wineserver 2>/dev/null; killall box64 2>/dev/null
sleep 1; rm -rf /data/local/tmp/.wine /tmp/.wine-*
rm -f /storage/Users/currentUser/workspace/wine/wine_surface_*.bmp

export WINEPREFIX=/data/local/tmp/.wine
export BOX64_LD_LIBRARY_PATH=./bin:./lib/x86_64
./bin/box64 ./bin/wine ./bin/notepad.exe 2>&1

# 查看渲染结果
ls -la /storage/Users/currentUser/workspace/wine/wine_surface_*.bmp
# 用 hdc file recv 拉到电脑查看
```

## 当前产物 hash

| 文件 | SHA1 | 说明 |
|------|------|------|
| ntdll.so | `6fc39dc79f34a34bf9daf77266c3f6c3dcac0b7b` | 含 MAP_FIXED + noexec + TEB + __ANDROID__ 修复 |
| win32u.so | `03a172c4089deb7dc48c75ffd059065dcc3b9787` | null driver + BMP dump + FreeType soname |
| wineserver | `a005d5f6f052371289038f92f7a41a0f363ebd18` | epoll_pwait2 stub + BINDIR/DATADIR |
| box64 | `050b9936a2639090c09a365c60d18b2e0982d620` | OHOS musl 兼容补丁 |

# Wine for HarmonyOS — 当前状态

> 更新: 2026-06-12
> 状态: ✅ `cmd.exe /c echo hello` 通过

---

## 里程碑

`WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/cmd.exe /c echo hello`

输出 `hello` ✅

---

## 已修复的问题 (5/5)

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

## 已知问题

### dnsapi / nsiproxy: musl 不兼容 (编译时跳过)

| 文件 | 问题 |
|------|------|
| `dlls/dnsapi/libresolv.c` | musl 没有 `_res` 全局变量 |
| `dlls/nsiproxy.sys/tcp.c`, `udp.c` | musl 没有 `sys/socketvar.h` |

当前 `make -k` 跳过，不影响基础功能。后续需要源码修复。

---

## 完整源码修改清单

| 文件 | 修改 | 原因 |
|------|------|------|
| `dlls/ntdll/unix/virtual.c` | `#undef MAP_FIXED_NOREPLACE` | Box64 mmap 不支持 |
| `dlls/ntdll/unix/virtual.c` | `virtual_alloc_first_teb()` 移除 MEM_TOP_DOWN | Box64 39位地址空间 |
| `dlls/ntdll/unix/virtual.c` | `virtual_alloc_thread_data()` fallback | Box64 map_view 失败 |
| `dlls/ntdll/unix/virtual.c` | `mprotect_exec()` prctl 绕过 | noexec 文件系统 |
| `dlls/ntdll/unix/virtual.c` | `map_image_into_view()` 匿名 mmap+pread | noexec 文件系统 |
| `build-ohos/` configure | `--prefix=/opt/winebox --libdir='${prefix}'` | 路径计算 |
| `build-ohos/` 编译 | `-D__ANDROID__` | /tmp → ~/.wine socket |
| `server/musl_compat.c` | `epoll_pwait2` stub | musl 不提供 |

---

## 构建 configure 参数

```bash
--host=x86_64-linux-ohos
--prefix=/opt/winebox
--libdir='${prefix}'
--with-wine-tools=../build-native
--with-mingw=gcc
--disable-tests
--without-x --without-freetype --without-alsa --without-opengl --without-vulkan
```

## HNP 布局

```
out/hnp_combined/opt/winebox/
├── bin/
│   ├── wine                  # musl loader
│   ├── wineserver            # x86_64 musl
│   ├── box64                 # ARM64 native
│   ├── ntdll.so              # ⚠️ 必须在 bin/，wine loader 同目录加载
│   ├── *.exe                 # PE stubs (109个)
│   ├── x86_64-windows/       # PE DLL (615个)
│   └── x86_64-unix/          # Unix .so (21个) — load_builtin_unixlib 拼接路径
├── lib/x86_64/
│   └── libc.so               # musl 运行时
└── share/wine/nls/           # NLS 文件
```

**重要**: `ntdll.so` 必须在 `bin/`，其他 .so 在 `x86_64-unix/`。

| 文件 | 位置 | 加载方式 |
|------|------|---------|
| `ntdll.so` | `bin/` | wine loader `dlopen("ntdll.so")` (loader/main.c:161) |
| `ws2_32.so` 等 | `bin/x86_64-unix/` | `load_builtin_unixlib` 拼接 `dll_dir + "/x86_64-unix/" + name` |

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
```

---

## 设备测试命令

```bash
cd /data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox
rm -rf /data/local/tmp/.wine
WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/cmd.exe /c echo hello 2>&1
```

## 当前产物 hash

| 文件 | SHA1 | 说明 |
|------|------|------|
| ntdll.so | `6fc39dc79f34a34bf9daf77266c3f6c3dcac0b7b` | 含 MAP_FIXED + noexec + TEB 修复 |
| wineserver | `a005d5f6f052371289038f92f7a41a0f363ebd18` | epoll_pwait2 stub + BINDIR/DATADIR |
| box64 | `050b9936a2639090c09a365c60d18b2e0982d620` | OHOS musl 兼容补丁 |

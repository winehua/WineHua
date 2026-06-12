# Wine for HarmonyOS — 当前状态

> 更新: 2026-06-12
> 状态: 开发中，已修复 4 个问题，1 个待测试

---

## 已修复的问题

### 1. TEB 分配崩溃 (`virtual_alloc_first_teb`)

**文件**: `thirdparty/wine/dlls/ntdll/unix/virtual.c`

**症状**: Box64 下 `wine cmd.exe` SIGSEGV，crash 在 `virtual_alloc_first_teb + 0x4ea`

**根因**: `NtAllocateVirtualMemory(MEM_TOP_DOWN)` 在 Box64 39 位地址空间下行为异常；`virtual_alloc_thread_data()` 内 `map_view()` 返回 `STATUS_NO_MEMORY`(0xc0000017)，导致 `thread_data` 为 NULL，后续 `thread_data->teb = teb` 写入 NULL 崩溃。

**修复**:
1. `virtual_alloc_first_teb()` — 移除 `MEM_TOP_DOWN` 标志
2. `virtual_alloc_thread_data()` — `map_view()` 失败时 fallback 到 `anon_mmap_alloc()` 直接 mmap 匿名内存
3. 添加 13 条 `ERR("OHOS-DBG: ...")` 诊断日志

**结果**: ✅ TEB 分配成功，`thread_data=0x60390000`

---

### 2. 路径计算错误（wineserver/NLS 找不到）

**文件**: configure 参数

**症状**:
- wineserver: 查找 `bin/../../bin/wineserver`（多一层 `../`）
- NLS: 查找 `bin/../../share/wine/nls/`（多一层 `../`）

**根因**: `--prefix=/usr/local --libdir=/usr/local/lib`，运行时 `build_relative_path()` 从 `LIBDIR/wine` 算到 `BINDIR`/`DATADIR` 多了一个 `..`

**修复**: 重新 configure:
```bash
--prefix=/opt/winebox --libdir='${prefix}'
```
这样 `from = /opt/winebox/wine`，到 `dest = /opt/winebox/bin` 只差 1 层 → `../bin` ✓

**结果**: ✅ 路径 baked-in 正确：`/opt/winebox/bin`, `/opt/winebox/share/wine`

---

### 3. Noexec 文件系统

**文件**: `thirdparty/wine/dlls/ntdll/unix/virtual.c`

**症状**: HNP 安装目录挂载为 noexec，PE DLL 加载时 mmap/mprotect PROT_EXEC 失败：
```
map_image_into_view failed to set ... protection on ... section .text, noexec filesystem?
```

**根因**: 文件系统不支持可执行映射。文件 mmap 的页面即使 prctl 也加不上 PROT_EXEC。

**修复**: 在 `map_image_into_view()` 中，对带 `IMAGE_SCN_MEM_EXECUTE` 的 section，不调用文件 mmap，改为：
1. 匿名 mmap（`MAP_ANON | MAP_PRIVATE`，无 PROT_EXEC）
2. `pread()` 读取文件内容到匿名内存
3. 后续 `set_vprot()` 通过 `mprotect_exec()` 加 PROT_EXEC（匿名页面 + prctl 可加 exec）

同时在 `mprotect_exec()` 中添加 prctl 绕过：
```c
if (unix_prot & PROT_EXEC) {
    prctl(0x6a6974, 0, 0);  // 临时允许 exec
    ret = mprotect(base, size, unix_prot);
    prctl(0x6a6974, 0, 1);
    return ret;
}
```

**结果**: ⏳ "noexec filesystem" 错误消失，待完整测试确认

**深入分析**: 参见 [NOEXEC_MMAP_ANALYSIS.md](./NOEXEC_MMAP_ANALYSIS.md) — mmap + PROT_EXEC + fd 的完整源码级分析

---

### 4. PE DLL 路径不匹配

**文件**: HNP 打包布局

**症状**: `wine: failed to load ntdll.dll error c0000135`

**根因**: Wine 查找 PE DLL 时会拼接 `pe_dir`（x86_64 架构下为 `/x86_64-windows`）：
```
find_builtin_dll: dll_dir + "/x86_64-windows/" + "ntdll.dll"
```
但 HNP 布局把所有 .dll 放在 `bin/` 根目录，不在子目录。

**修复**: HNP 布局中创建 `bin/x86_64-windows/`，615 个 PE DLL 移入其中。

**位置**: `out/hnp_combined/opt/winebox/bin/x86_64-windows/`

**结果**: ⏳ 待测试

---

## 完整的源码修改清单

| 文件 | 修改 | 原因 |
|------|------|------|
| `dlls/ntdll/unix/virtual.c` | `virtual_alloc_first_teb()` 移除 MEM_TOP_DOWN | Box64 39位地址空间 |
| `dlls/ntdll/unix/virtual.c` | `virtual_alloc_thread_data()` map_view 失败 fallback anon_mmap_alloc | Box64 map_view 返回 STATUS_NO_MEMORY |
| `dlls/ntdll/unix/virtual.c` | 添加 `#include <sys/prctl.h>` | prctl |
| `dlls/ntdll/unix/virtual.c` | `mprotect_exec()` 所有 PROT_EXEC 调用加 prctl 绕过 | noexec 文件系统 |
| `dlls/ntdll/unix/virtual.c` | `map_image_into_view()` 可执行 section 用匿名 mmap+pread | noexec 文件系统 |
| `dlls/ntdll/unix/virtual.c` | 添加 OHOS-DBG 诊断日志 | 调试 TEB 分配 |
| `build-ohos/` configure | `--prefix=/opt/winebox --libdir='${prefix}'` | 路径计算 |
| `build-ohos/` 编译 | `-D__ANDROID__` | /tmp → ~/.wine socket |
| `musl_compat.c` | `epoll_pwait2` stub | musl 不提供 |
| `out/hnp_combined/` | PE DLL 移到 `bin/x86_64-windows/` | Wine 查找路径 |

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

## 编译标志

| 标志 | 用途 |
|------|------|
| `-D__MUSL__` | musl libc |
| `-D_GNU_SOURCE` | GNU 扩展 |
| `-DWINE_UNIX_LIB` | msvcrt 头冲突 |
| `-D_NTSYSTEM_` | ntuser.h 修复 |
| `-D__WINESRC__` | Wine 源码 |
| `-DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO` | Wine 标准 |
| `-D__ANDROID__` | OHOS /tmp 只读兼容 |
| `-fPIC` | .so 位置无关代码 |

## HNP 布局

```
out/hnp_combined/opt/winebox/
├── bin/
│   ├── wine                  # musl loader
│   ├── wineserver            # x86_64 musl
│   ├── box64                 # ARM64 native
│   ├── ntdll.so              # ⚠️ 必须在 bin/，wine loader 硬编码同目录加载
│   ├── *.exe                 # PE stubs (114个)
│   ├── x86_64-windows/       # PE DLL (615个)
│   │   ├── ntdll.dll
│   │   ├── kernel32.dll
│   │   └── ...
│   └── x86_64-unix/          # Unix .so (22个) — load_builtin_unixlib 拼接路径
│       ├── avicap32.so
│       ├── crypt32.so
│       ├── ws2_32.so
│       └── ...
├── lib/x86_64/
│   └── libc.so               # musl 运行时
└── share/wine/nls/           # NLS 文件
```

**重要**: `ntdll.so` 必须留在 `bin/`，不能移入 `x86_64-unix/`。

| 文件 | 位置 | 加载方式 |
|------|------|---------|
| `ntdll.so` | `bin/` | wine loader `dlopen("ntdll.so")` (loader/main.c:161) |
| `ws2_32.so` 等 | `bin/x86_64-unix/` | `load_builtin_unixlib` 拼接 `dll_dir + "/x86_64-unix/" + name` |

## 部署流程

```bash
# 1. 复制产物
cp build-ohos/loader/wine        out/hnp_combined/opt/winebox/bin/
cp build-ohos/server/wineserver  out/hnp_combined/opt/winebox/bin/
cp build-ohos/dlls/**/*.so       out/hnp_combined/opt/winebox/bin/

# 2. 打包 HNP
hnpcli pack -i out/hnp_combined -o out -n winebox -v 0.1.0

# 3. 打包 HAP
cp out/winebox.hnp HonWine/entry/hnp/arm64-v8a/honwine.hnp
hvigorw assembleHap
cd entry && zip -r ../entry/build/default/outputs/default/entry-default-unsigned.hap hnp
python3 sign.py <unsigned> <signed>

# 4. 推送安装
hdc tconn 192.168.1.4:38879
hdc file send <signed.hap> /data/local/tmp/
hdc shell bm install -p /data/local/tmp/<hap> -r
```

## 设备测试命令

```bash
cd /data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox
rm -rf /data/local/tmp/.wine
WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/cmd.exe /c echo hello 2>&1
```

## 当前产物 hash

| 文件 | SHA1 | 说明 |
|------|------|------|
| ntdll.so | `e41e954fbd81b81978504bfd69600d083a90c4f3` | 含所有修复 + 匿名mmap |
| wineserver | `54ff6f2a727ecaf639cb16663ed48a4f3b8b8a22` | epoll_pwait2 stub |
| loader/wine | `63cd7e62cfdc146ae128a6d0e77e5ac0ede292a7` | musl |

## 待测试

- [ ] `wine cmd.exe /c echo hello` 是否完整跑通
- [ ] wineserver 是否被 Box64 正确识别为 x86_64
- [ ] 后续 PE DLL 加载是否全部正常

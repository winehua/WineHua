# Wine for HarmonyOS — 编译验证记录

> **日期**: 2026-06-10 ~ 2026-06-11
> **环境**: WSL2 Ubuntu 26.04 x86_64, OHOS SDK 15.0.4 / Clang 15.0.4 / musl 1.2.5
> **目标**: Wine on HarmonyOS x86_64 (Phase 1)

---

## 1. 环境配置

### OHOS SDK
```
路径:     /apps/harmony/sdk/default/openharmony
工具链:   native/llvm/bin/clang (Clang 15.0.4)
Sysroot:  native/sysroot
目标:     x86_64-linux-ohos

关键文件:
  sysroot/usr/lib/x86_64-linux-ohos/libc.so     ← musl libc (ELF x86-64)
  sysroot/usr/lib/x86_64-linux-ohos/Scrt1.o     ← CRT
  sysroot/usr/include/x86_64-linux-ohos/asm/prctl.h  ← arch_prctl (ARCH_SET_GS/FS)
  sysroot/usr/include/sys/epoll.h, inotify.h      ← Linux API
```

### Wine 源码
```
路径:     .temp/wine/ (github.com/wine-mirror/wine, depth=1)
Native:   .temp/wine/build-native/ (x86_64-linux-gnu, gcc)
OHOS:     .temp/wine/build-ohos/   (x86_64-linux-ohos, clang)
```

---

## 2. 编译步骤

### Step 1: Native Wine 构建 (生成工具链)

```bash
cd .temp/wine/build-native
../configure --enable-win64 --disable-tests --without-x --without-freetype \
    --without-alsa --without-opengl --without-vulkan
make -j$(nproc)
# 结果: ✅ Wine build complete.
```

### Step 2: OHOS 交叉编译 configure

```bash
cd .temp/wine/build-ohos
CC="gcc" \
../configure \
    --host=x86_64-linux-ohos \
    --with-wine-tools=../build-native \
    --with-mingw=gcc \
    --disable-tests \
    --without-x --without-freetype --without-alsa --without-opengl --without-vulkan
# 结果: ✅ configure: Finished. Do 'make' to compile Wine.
```

**注意**: 用系统 `gcc` 处理 PE 交叉编译 (支持 `-mabi=ms`)，OHOS `clang` 处理 Unix 编译。

### Step 3: 编译 wineserver (OHOS clang)

```bash
CLANG=/apps/harmony/sdk/default/openharmony/native/llvm/bin/clang
SYSROOT=/apps/harmony/sdk/default/openharmony/native/sysroot

make CC="$CLANG --target=x86_64-linux-ohos --sysroot=$SYSROOT -D__MUSL__ -D_GNU_SOURCE" \
     CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -DWINE_UNIX_LIB" \
     LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=x86_64-linux-ohos" \
     server/wineserver

# 结果: ✅ 全部 44 个 server/*.c 编译通过，0 错误
```

### Step 4: 手动链接 (绕过 Makefile)

Makefile 链接阶段需要额外处理 musl 缺失符号:

```bash
# 创建 musl_compat.c (epoll_pwait2 stub)
$CLANG -c server/musl_compat.c -o server/musl_compat.o

# 手动链接
$CLANG --target=x86_64-linux-ohos --sysroot=$SYSROOT \
    -fuse-ld=lld -o server/wineserver server/*.o server/musl_compat.o -lm

# 结果: ✅ wineserver (3.4MB, ELF x86-64, musl)
```

---

## 3. 编译错误和修复

### 错误 1: `fatal error: 'config.h' file not found`
- **原因**: 首次编译缺少 configure 生成的头文件
- **修复**: 先运行 `./configure`

### 错误 2: `error: You cannot use config.h with msvcrt`
- **原因**: server 代码（Unix 侧）不应包含 msvcrt 头文件
- **修复**: 添加 `-DWINE_UNIX_LIB` 编译标志，移除 `-I.../msvcrt`

### 错误 3: `fatal error: 'linux/ntsync.h' file not found`
- **原因**: OHOS sysroot 不提供 ntsync 驱动头文件，但 native config.h 定义了 `HAVE_LINUX_NTSYNC_H=1`
- **修复**: `#undef HAVE_LINUX_NTSYNC_H` (ntsync 为可选优化，不影响功能)

### 错误 4: `fatal error: 'netipx/ipx.h' file not found`
- **原因**: IPX 协议头文件不在 OHOS sysroot
- **修复**: `#undef HAVE_NETIPX_IPX_H` (IPX 协议已废弃)

### 错误 5: `undefined symbol: epoll_pwait2`
- **原因**: musl 不提供 `epoll_pwait2` (glibc 2.6+)
- **修复**: 创建 `musl_compat.c` 弱符号 stub，返回 `ENOSYS`。Wine 代码在 `epoll_pwait2` 失败时会自动 fallback 到 `epoll_wait`。

### 错误 6: `fatal error: 'unixlib.h' file not found` (ntdll/unix)
- **原因**: 缺少 `-I$WINE/include/wine` 头文件路径
- **修复**: 添加 `-I$(SRCDIR)/include/wine` 编译标志

### 错误 7: `field has incomplete type 'enum NONCLIENT_BUTTON_TYPE'` (ntdll/unix)
- **原因**: `ntuser.h` → `winuser.h` 的 include 顺序问题，`NONCLIENT_BUTTON_TYPE` 在 `ntuser.h` 中引用但定义在 `winuser.h` 中
- **修复**: 添加 `-D_NTSYSTEM_` 和 `-D__WINESRC__` 编译标志（与 native build 保持一致）

### 错误 8: 使用 Makefile 的正确方式
- **关键**: 需要用 `make CFLAGS="..."` 传递完整的编译标志，包括 `-D_NTSYSTEM_`, `-D__WINESRC__`, `-DFAR=`, `-fPIC` 等
- **成功命令**:
```bash
make CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
     CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -DWINE_UNIX_LIB \
     -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
     -fPIC -fasynchronous-unwind-tables" \
     LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET" \
     dlls/ntdll/ntdll.so
```

### 警告 (非阻塞)
- `-Wlogical-op`, `-Wno-packed-not-aligned`, `-Wshift-overflow=2` — clang 不识别这些 gcc 特有 warning flags。仅警告，不影响编译。后续可用 `-Wno-unknown-warning-option` 消除。

---

## 4. 产物验证

```bash
$ file out/wineserver
out/wineserver: ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV),
  dynamically linked, interpreter /lib/ld-musl-x86_64.so.1,
  with debug_info, not stripped

$ llvm-readelf -d out/wineserver | grep NEEDED
  0x0000000000000001 (NEEDED)  Shared library: [libc.so]

$ llvm-readelf -h out/wineserver | grep -E 'Class|Machine|OS/ABI'
  Class:     ELF64
  OS/ABI:    UNIX - System V
  Machine:   Advanced Micro Devices X86-64
```

| 验证项 | 预期 | 实际 |
|--------|------|------|
| 架构 | x86-64 | ✅ x86-64 |
| 动态链接器 | ld-musl-x86_64.so.1 | ✅ |
| libc | libc.so (musl) | ✅ |
| 大小 | 合理的调试构建 | ✅ 3.4MB |

---

## 5. 下一步计划

### 短期 (Phase 1)
1. **编译 ntdll.so** — Wine 核心 Unix 层的 .so 文件
2. **编译其他 Unix .so** — kernel32, user32 等
3. **在 OHOS x86_64 真机/模拟器上运行测试** — `wineserver --version`
4. **整合 PE DLLs** — 从 native build 提取 PE DLLs + OHOS build 的 Unix .so
5. **HNP 打包** — 创建 HarmonyOS Native Package

### 中期 (Phase 1 收尾)
6. **完善 patch 脚本** — 针对 vkd3d, dll_soinit 的实际修改
7. **搭建 CI/CD** — 自动化交叉编译流程
8. **性能测试** — OHOS 上的 Wine 性能基准测试

### 长期 (Phase 2)
9. **ARM64 移植** — 引入 Hangover (Box64 + FEX) 进行交叉架构模拟
10. **图形栈** — 适配 OHOS 原生图形系统或 Wayland

---

## 6. 已知待解决问题

| # | 问题 | 优先级 | 状态 |
|---|------|--------|------|
| 1 | `pthread_setname_np` 签名检测 (configure) | P1 | 🟡 待处理 |
| 2 | `program_invocation_name` (vkd3d) | P1 | 🟡 patch 已写，待实际编译验证 |
| 3 | `dladdr1/dlinfo` fallback (dll_soinit.c) | P1 | 🟡 patch 已写，待实际编译验证 |
| 4 | `libc.so.6` hardcoded (signal_x86_64.c) | P2 | 🟡 patch 已写 |
| 5 | clang WARN_FLAGS 清理 | P3 | 🟢 低优先级 |
| 6 | PE DLLs 交叉编译 (mingw) | P2 | 🟡 可用 native build 产物替代 |
| 7 | 真机运行验证 | P0 | 🔴 需要 OHOS x86_64 环境 |

---

## 附录: 完整编译命令参考

```bash
# 环境变量
export OHOS_SDK=/apps/harmony/sdk/default/openharmony
export CLANG=$OHOS_SDK/native/llvm/bin/clang
export SYSROOT=$OHOS_SDK/native/sysroot
export TARGET=x86_64-linux-ohos
export WINE_SRC=/src/ohos/wineohos/.temp/wine

# Native 构建
cd $WINE_SRC/build-native
$WINE_SRC/configure --enable-win64 --disable-tests --without-x
make -j$(nproc)

# OHOS configure
cd $WINE_SRC/build-ohos
CC="gcc" $WINE_SRC/configure --host=$TARGET \
    --with-wine-tools=$WINE_SRC/build-native --with-mingw=gcc \
    --disable-tests --without-x --without-freetype

# OHOS 编译 (单个组件)
make CC="$CLANG --target=$TARGET --sysroot=$SYSROOT -D__MUSL__" \
     LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET" \
     server/wineserver
```

---

## 7. PE DLLs 整合 (2026-06-11)

从 native build 复制 PE DLLs 到 OHOS 输出:

| 组件 | 来源 | 数量 |
|------|------|------|
| PE DLLs (.dll) | `build-native/dlls/` | 615 |
| PE Executables (.exe) | `build-native/programs/` | 109 |
| Unix .so | `build-ohos/dlls/` | 22 |
| wineserver | `build-ohos/server/` | 1 |

**输出目录结构**:
```
out/wine/
├── bin/wineserver          # OHOS 编译
├── bin/*.exe               # PE (native build, 可复用)
├── lib/wine/ntdll.so       # OHOS 编译
├── lib/wine/*.so           # OHOS 编译 (22 个)
└── lib/wine/*.dll          # PE (native build, 615 个)
```

## 8. clang 警告清理

移除 Makefile 中的 gcc 特有 warning flags:
- `-Wlogical-op` → 删除
- `-Wno-packed-not-aligned` → 删除
- `-Wshift-overflow=2` → `-Wshift-overflow`

脚本: `scripts/fix_warnings.sh`

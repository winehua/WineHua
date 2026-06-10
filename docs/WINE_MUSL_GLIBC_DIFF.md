# Wine glibc → musl 移植兼容性分析 (Phase 1: x86_64)

> **目标**: 将 Wine 构建在 musl libc (HarmonyOS) 上，运行 x86_64 Windows 程序于 x86_64 HarmonyOS。
> **核心挑战**: 仅 libc 兼容性，无 CPU 架构模拟。
> **分析方法**: 逐文件审查 Wine 源码中的 glibc 特定依赖，参考 Box64 OHOS 移植 的 20 个 patch 作为对照。

---

## 1. 总体评估

| 维度 | 评估 | 说明 |
|------|------|------|
| glibc 耦合度 | 🟢 **低** | Wine 远低于 Box64 |
| 预计 patch 数量 | **10-15** | 远少于 Box64 OHOS 移植 的 20 个 |
| 最大风险 | 🟡 `arch_prctl` 和信号处理 | x86_64 段寄存器管理 |
| 预计工作量 | **1-2 个月** (1 人) | 大部分是构建系统适配 |

**关键发现**: Wine 的 ntdll/unix 层**不使用**以下 glibc 私有 API：
- ❌ `RTLD_NEXT` / `RTLD_DEEPBIND`
- ❌ `__libc_malloc` / `__libc_free` / `__libc_calloc` / `__libc_realloc`
- ❌ `dlinfo`
- ❌ `error_at_line` / `error_message_count`
- ❌ `obstack_*` / `fts_*`
- ❌ `program_invocation_name`
- ❌ `backtrace` / `backtrace_symbols` (非测试代码)

---

## 2. 逐项 glibc 依赖分析

### 2.1 编译期问题 (Headers / Types / Macros)

#### ✅ 低风险 — musl 已有等价实现

| API / Header | Wine 使用位置 | musl 状态 | 处理方式 |
|-------------|--------------|----------|---------|
| `<dlfcn.h>` | `dlls/ntdll/unix/loader.c` | ✅ 完全支持 | 无需处理 |
| `dlopen` / `dlsym` / `dlclose` / `dladdr` | `loader.c` 多处 | ✅ POSIX | 无需处理 |
| `<pthread.h>` | `thread.c`, `signal_*.c` | ✅ 完全支持 | 无需处理 |
| `<signal.h>` / `sigaction` / `sigset_t` | `signal_*.c` | ✅ POSIX | 无需处理 |
| `<sys/mman.h>` / `mmap` / `mprotect` | `virtual.c` | ✅ POSIX | 无需处理 |
| `<sys/ucontext.h>` / `ucontext_t` | `signal_*.c` | ✅ musl 提供 | 无需处理 |
| `<link.h>` / `dl_iterate_phdr` | `signal_x86_64.c`, `loader.c` | ✅ musl 提供 | 无需处理 |
| `<elf.h>` | 多处 | ✅ musl 提供 | 无需处理 |
| `<sys/epoll.h>` / `epoll_*` | `server/fd.c` | ✅ musl 提供 | 无需处理 |
| `<sys/inotify.h>` / `inotify_*` | `server/change.c` | ✅ musl 提供 | 无需处理 |
| `<sys/prctl.h>` / `prctl` | `signal_x86_64.c`, `env.c`, `server.c` | ✅ musl 提供 | 无需处理 |
| `<sys/auxv.h>` / `getauxval` | `signal_x86_64.c`, `loader.c` | ✅ musl 提供 | 无需处理 |
| `futex` via `syscall(__NR_futex, ...)` | `sync.c` | ✅ 内核 syscall | 无需处理 |
| `gettid` via `syscall(__NR_gettid)` | `server.c` | ✅ 内核 syscall | 无需处理 |
| `environ` | `env.c`, `loader.c` | ✅ musl 提供 | 无需处理 |
| `sched_getcpu` | `thread.c` | ✅ musl 提供 | 无需处理 |

#### 🟡 中等风险 — 需要适配

| API | Wine 位置 | 问题 | 处理方案 |
|-----|----------|------|---------|
| `pthread_setname_np` (1-arg) | `dlls/ntdll/unix/thread.c:2094` | glibc 是 2-arg (`pthread_t, name`)，musl 是 1-arg (`name`)。Wine 使用 1-arg 形式，glibc 上由 `_GNU_SOURCE` 宏展开；musl 原生就是 1-arg | configure 检测签名，用 `HAVE_PTHREAD_SETNAME_NP_1` 保护 |
| `pthread_setname_np` (vkd3d) | `libs/vkd3d/libs/vkd3d-common/debug.c:541-544` | 已有 `HAVE_PTHREAD_SETNAME_NP_1` / `HAVE_PTHREAD_SETNAME_NP_2` 守卫 | configure 需要正确检测 |
| `pthread_attr_setstack` | `thread.c:1321` | ✅ POSIX，musl 支持 | 无需处理 |
| `pthread_attr_setguardsize` | `thread.c:1322` | ✅ POSIX，musl 支持 | 无需处理 |
| `sysconf(_SC_CLK_TCK)` | `thread.c:1951, 2242` | ✅ POSIX，musl 支持 | 无需处理 |

#### 🔴 高风险 — x86_64 特有, 需要关注

| API | Wine 位置 | 问题 | 处理方案 |
|-----|----------|------|---------|
| `#include <asm/prctl.h>` | `signal_x86_64.c:102` | 内核头文件，路径可能不同 | 检查 HarmonyOS kernel headers 路径 |
| `arch_prctl(ARCH_SET_FS, ...)` | `signal_x86_64.c:792, 2863` | x86_64 FS 段基址设置 | 只有 x86_64 才需要，内核必须支持 |
| `arch_prctl(ARCH_SET_GS, ...)` | `signal_x86_64.c:2258, 2860` | x86_64 GS 段基址设置 | Windows TEB 通过 GS 访问 |
| `arch_prctl(ARCH_GET_GS, ...)` | `signal_x86_64.c:2197` | 读取 GS 基址 | 同上 |
| `prctl(PR_SET_SYSCALL_USER_DISPATCH, ...)` | `signal_x86_64.c:2611, 2866` | Linux 5.11+ 特性，用于 syscall 拦截 | 如果 OH 内核不支持，需要 fallback |
| `PR_SET_PTRACER` (0x59616d61) | `server.c:1668` | 允许 ptrace，Linux 4.3+ | 检查 OH 内核 |

### 2.2 链接期问题 (Runtime Symbols)

#### ✅ 确认不需要的符号（Wine 不使用）

以下 glibc 私有符号在 Wine 源码中**未找到引用**：

```
__libc_malloc      __libc_free     __libc_calloc
__libc_realloc     __libc_memalign dlinfo
qsort_r            glob64          globfree64
error_at_line      error_message_count
obstack_vprintf    obstack_printf
__ctype_b_loc      __ctype_tolower_loc  __ctype_toupper_loc
pthread_*_affinity_np (除了 setname)
scandirat          scandirat64
program_invocation_name
```

> **结论**: 不需要 `musl_compat.c` 类似的弱符号桩文件（Box64 OHOS 移植 的 Patch 12）。

#### 🟡 可能缺失的库

Wine 构建时通常链接：
- `-lpthread` — musl 内置，不需要单独链接
- `-ldl` — musl 内置
- `-lm` — musl 提供
- `-lrt` — musl 内置（不单独存在）

**处理**: configure 检测时跳过不存在的库，或者在 CMakeLists / Makefile 中将 `-lrt` 移除。

### 2.3 musl 特有的 64 位宏问题

#### 🟡 stat64 = stat 宏冲突

musl 在启用 `_GNU_SOURCE` 或默认情况下定义：
```c
#define stat64  stat
#define fstat64 fstat
#define lstat64 lstat
#define open64  open
#define mmap64  mmap
```

这会导致 Wine 内部使用 `stat64()` / `open64()` 等符号时，如果代码中有同名变量或结构体字段叫 `stat64`，会产生冲突。

**Wine 影响范围**: 
- `dlls/ntdll/unix/file.c` — 文件操作，使用了 `fstatat` 等
- 需要全局搜索 `open64` / `fopen64` / `mmap64` 作为符号名的代码

**处理方案**（参考 Box64 OHOS 移植 Patch 09）:
```c
// 在 wrapper 函数表 include 之前
#undef stat64
#undef fstat64
#undef lstat64
// ... include wrappercallback.h ...
// 在 include 之后
#define stat64  stat
#define fstat64 fstat
```

### 2.4 构建系统适配

#### configure.ac 需要的修改

| 检测项 | 问题 | 处理 |
|--------|------|------|
| `_GNU_SOURCE` | configure.ac 已经定义 (line 1286) | 保持，musl 也支持 |
| `HAVE_SCHED_GETCPU` | 已定义 `#undef` | configure 能正常检测到 |
| `HAVE_SYS_EPOLL_H` | 已定义 `#undef` | musl 提供 |
| `HAVE_SYS_INOTIFY_H` | 已定义 `#undef` | musl 提供 |
| `HAVE_SYS_UIO_H` | 用于 `process_vm_readv` | musl 提供 |
| `HAVE_PTHREAD_SETNAME_NP_1` / `_2` | 需要新增检测 | 写 autoconf 测试 |
| `HAVE_SYS_SYSCALL_H` | 已定义 | musl 提供 |
| `HAVE_SYS_UCONTEXT_H` | 已定义 | musl 提供 |
| `libinotify` | configure 检测 `-linotify` | musl 上 inotify 在 libc 中，不需要单独链接 |

---

## 3. 与 Box64 OHOS 移植 的 Patch 对照

Box64 移植需要 20 个 patch。Wine 需要多少？

| Box64 Patch | 问题 | Wine 是否需要 | 说明 |
|------------|------|-------------|------|
| Patch 01 `mallopt` | `M_ARENA_*` 缺失 | ❌ 不需要 | Wine 不使用 mallopt |
| Patch 02 `pthread NP init` | `PTHREAD_*_NP` 宏 | ⚠️ 可能需要 | 如果 Wine 某处用了这些宏 |
| Patch 03 `fts.h` | musl 缺失 fts | ❌ 不需要 | Wine 不使用 fts |
| Patch 04 `__sigset_t` | 需要 struct 标签 | ⚠️ 待确认 | 检查 signal_*.c |
| Patch 05 `pthread_cleanup` | 声明冲突 | ❌ 不需要 | Wine 不直接使用内部函数 |
| Patch 06 `obstack.h` | musl 缺失 | ❌ 不需要 | Wine 不使用 obstack |
| Patch 07 `error.h` | musl 缺失 | ❌ 不需要 | Wine 不使用 GNU error() |
| Patch 08 `RTLD_DL_*` | 常量缺失 | ❌ 不需要 | Wine 不使用 dlinfo |
| Patch 09 `wrappedlibc` | *64 宏冲突 | ✅ **可能需要** | Wine 有 open64/stat64 使用 |
| Patch 10 `ctype/CLONE` | `__ctype_*_loc` + `_GNU_SOURCE` | ❌ 不需要 | Wine 直接包含 `<ctype.h>` |
| Patch 11 `config.h` | autotools 缺失 | ❌ 不需要 | Wine 有自己的 config.h |
| Patch 12 `musl_compat.c` | glibc 运行时符号 | ❌ 不需要 | Wine 不使用那些符号 |
| Patch 13 `mallochook` | box64 内存池 | ❌ 不适用 | Wine 没有类似的机制 |
| Patch 15 `__curbrk` | glibc 私有 sbrk 指针 | ❌ 不需要 | |
| Patch 16 `RTLD_NEXT→DEFAULT` | musl 主程序 dlsym | ❌ 不需要 | Wine ntdll 不使用 RTLD_NEXT |
| Patch 17 `skip box64lib dlopen` | dlopen 自我递归 | ❌ 不适用 | Wine 的 dlopen 使用正常 |
| Patch 18 `skip pthread dlsym` | dlsym glibc 私有符号 | ❌ 不需要 | |
| Patch 19 `PRE_INIT dlopen` | wrappedlibc 初始化 | ❌ 不适用 | |
| Patch 20 `libdl rename` | dlopen/dlsym 强符号冲突 | ❌ 不需要 | Wine 不 export dl 符号 |

**总计**: Wine 可能只需要 **Patch 09 的 *64 宏冲突处理** 和少量新增适配。

---

## 4. 关键源码文件清单

### 最需要关注的 Wine 文件

| 文件 | 行数 | 关注点 | 风险等级 |
|------|------|--------|---------|
| `dlls/ntdll/unix/signal_x86_64.c` | ~3100 | `arch_prctl`, `asm/prctl.h`, `PR_SET_SYSCALL_USER_DISPATCH` | 🔴 高 |
| `dlls/ntdll/unix/thread.c` | ~2900 | `pthread_setname_np`, `pthread_attr_setstack` | 🟡 中 |
| `dlls/ntdll/unix/virtual.c` | ~1000 | `MAP_NORESERVE`, `MAP_FIXED_NOREPLACE` fallback | 🟢 低 |
| `dlls/ntdll/unix/loader.c` | ~2200 | `dlopen`/`dlsym`/`dladdr`, `environ` | 🟢 低 |
| `dlls/ntdll/unix/env.c` | ~2100 | `environ`, `prctl(PR_SET_NAME)` | 🟢 低 |
| `dlls/ntdll/unix/sync.c` | ~3700 | `futex` syscall, `<linux/futex.h>` | 🟢 低 |
| `dlls/ntdll/unix/server.c` | ~1700 | `prctl(PR_SET_PTRACER)`, `syscall(__NR_gettid)` | 🟡 中 |
| `server/fd.c` | ~700 | `epoll_*` | 🟢 低 |
| `server/change.c` | ~1000 | `inotify_*` | 🟢 低 |
| `libs/vkd3d/libs/vkd3d-common/debug.c` | ~550 | `pthread_setname_np` 签名检测 | 🟡 中 |

---

## 5. 预期的 Patch 列表 (Phase 1)

按照优先级排列：

### P0 — 阻塞编译
1. **`asm/prctl.h` 路径检测** — `signal_x86_64.c` 包含此头文件，需确认 HarmonyOS kernel headers 位置
2. **`pthread_setname_np` 签名检测** — configure.ac 新增检测逻辑
3. **`HAVE_SYS_PRCTL_H`** — 确认 musl 提供，用于 `signal_x86_64.c`

### P0.5 — 新发现的编译阻塞（从 agent 分析确认）
4. **`program_invocation_name` 缺失** — `libs/vkd3d/libs/vkd3d/utils.c:886`，musl 不提供此 glibc 全局变量。需 `#ifdef __GLIBC__` 保护或从 `/proc/self/exe` 获取。
5. **`dladdr1` / `dlinfo` 双缺失** — `libs/winecrt0/dll_soinit.c:99-104`，fallback 链不可用。需添加 `_DYNAMIC` + `AT_PHDR` fallback。
6. **`libc.so.6` 硬编码名称** — `signal_x86_64.c:2759`，musl 使用 `libc.so`。需同时匹配 `"libc.so"`。

### P1 — 编译警告/错误修复
7. **`MAP_NORESERVE` fallback** — 已处理，但需验证
8. **`-lrt` 移除** — musl 不需要，configure 需适配
9. **`environ` 类型** — musl 提供 `char **environ`，确认无冲突
10. **`configure.ac` 添加 `linux-ohos*` host case** — 需要在 autoconf 中识别 HarmonyOS 目标

### P2 — 链接期
11. **`-lpthread` → 不需要** — musl 内置 pthread，但保留也不报错
12. **`-ldl` → 不需要** — 同上

### P3 — 运行时验证
9. **`PR_SET_SYSCALL_USER_DISPATCH`** — 如果 HarmonyOS 内核不支持，需 fallback
10. **`PR_SET_PTRACER`** — 同上
11. **`arch_prctl` 功能验证** — 实际测试 FS/GS 段寄存器 setup
12. **`futex` 正确性** — 验证 `__NR_futex` 在 HarmonyOS 上可用
13. **`inotify` 功能** — 验证 wineserver 的文件变更通知
14. **`epoll` 功能** — 验证 wineserver 的 I/O 多路复用

---

## 6. HarmonyOS 特定注意事项

### 6.1 动态链接器路径

HarmonyOS 使用 `ld-musl-aarch64.so.1` (ARM64) 或 `ld-musl-x86_64.so.1` (x86_64)。
Wine 的 .so 文件安装位置需要注册到 linker namespace。

### 6.2 库搜索路径

HarmonyOS 的库搜索路径（从 ld-musl namespace 配置）：
```
/system/lib64
/vendor/lib64
...
```

Wine 默认期望 `/usr/lib/wine/` 路径，需要适配。

### 6.3 HNP 打包

如果要在真机上运行，需要将 Wine 的 .so 文件和 wineserver 打包为 HNP (HarmonyOS Native Package) 格式。参考 Box64 OHOS 移植 的 HNP 打包脚本。

### 6.4 图形栈

Phase 1 可以先跑 **headless（无头）模式**：
- 只验证控制台 Windows 程序 (`wineconsole`)
- 不涉及 X11/Wayland 驱动
- 使用 Wine 的 `null` 或 `ttydrv` 图形驱动

---

## 7. 下一步行动

1. **获取 HarmonyOS x86_64 SDK** — 确认 OHOS 支持 x86_64 目标
2. **尝试交叉编译 Wine** — 用 OHOS NDK clang 编译 wine，收集第一批编译错误
3. **分析信号处理代码** — 仔细审查 `signal_x86_64.c` 中的 x86_64 特定代码
4. **检查 harmonyos kernel headers** — 确认 `<asm/prctl.h>` 和 `arch_prctl` 系统调用号
5. **编写 configure 适配** — 添加 musl 特性检测

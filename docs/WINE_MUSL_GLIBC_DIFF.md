# Wine glibc → musl 移植兼容性分析

> 目标: 将 Wine 构建在 musl libc (HarmonyOS) 上
> 核心挑战: libc 兼容性，无 CPU 架构模拟

---

## 1. 总体评估

| 维度 | 评估 | 说明 |
|------|------|------|
| glibc 耦合度 | 🟢 **低** | Wine 远低于 Box64 |
| 预计 patch 数量 | **10-15** | 构建系统适配为主 |
| 最大风险 | 🟡 `arch_prctl` 和信号处理 | x86_64 段寄存器管理 |

**关键发现**: Wine 的 ntdll/unix 层**不使用**以下 glibc 私有 API：
- `RTLD_NEXT` / `RTLD_DEEPBIND`
- `__libc_malloc` / `__libc_free` / `__libc_calloc` / `__libc_realloc`
- `dlinfo` / `error_at_line` / `obstack_*` / `fts_*`
- `program_invocation_name` (仅 vkd3d 使用)
- `backtrace` / `backtrace_symbols` (非测试代码)

---

## 2. 逐项 glibc 依赖分析

### 2.1 编译期 — musl 已有等价实现 ✅

| API / Header | Wine 使用位置 | musl 状态 |
|-------------|--------------|----------|
| `<dlfcn.h>` / `dlopen` / `dlsym` | `dlls/ntdll/unix/loader.c` | ✅ |
| `<pthread.h>` | `thread.c`, `signal_*.c` | ✅ |
| `<sys/mman.h>` / `mmap` / `mprotect` | `virtual.c` | ✅ |
| `<sys/ucontext.h>` / `ucontext_t` | `signal_*.c` | ✅ |
| `<link.h>` / `dl_iterate_phdr` | `signal_x86_64.c`, `loader.c` | ✅ |
| `<sys/epoll.h>` / `epoll_*` | `server/fd.c` | ✅ |
| `<sys/inotify.h>` / `inotify_*` | `server/change.c` | ✅ |
| `<sys/prctl.h>` / `prctl` | `signal_x86_64.c`, `server.c` | ✅ |
| `<elf.h>` | 多处 | ✅ |
| `futex` via `syscall(__NR_futex)` | `sync.c` | ✅ 内核 syscall |
| `sched_getcpu` | `thread.c` | ✅ |

### 2.2 需要适配 🟡

| API | Wine 位置 | 问题 | 处理 |
|-----|----------|------|------|
| `pthread_setname_np` (1-arg) | `thread.c:2094` | glibc 2-arg, musl 1-arg | configure 检测签名 |
| `pthread_setname_np` (vkd3d) | `libs/vkd3d/` | 已有 `HAVE_PTHREAD_SETNAME_NP_1/_2` 守卫 | configure 检测 |
| `arch_prctl` | `signal_x86_64.c` | x86_64 段寄存器管理 | ✅ OHOS 内核支持 |
| `epoll_pwait2` | `server/fd.c` | musl 未提供 | stub 返回 ENOSYS, 自动 fallback |

### 2.3 musl 特有 64 位宏问题

musl 定义 `stat64 = stat`, `open64 = open` 等宏，可能与其他代码冲突。Wine 需要在相关文件 `#undef` 这些宏。

---

## 3. 关键源码文件

| 文件 | 行数 | 关注点 | 风险 |
|------|------|--------|------|
| `signal_x86_64.c` | ~3100 | `arch_prctl`, `PR_SET_SYSCALL_USER_DISPATCH` | 🔴 |
| `thread.c` | ~2900 | `pthread_setname_np` | 🟡 |
| `virtual.c` | ~5300 | `MAP_NORESERVE`, `MAP_FIXED_NOREPLACE`, noexec | 🟡 |
| `loader.c` | ~2200 | `dlopen`/`dlsym`/`dladdr` | 🟢 |
| `server.c` | ~1700 | `prctl(PR_SET_PTRACER)`, socket 路径 | 🟡 |
| `fd.c` | ~700 | `epoll_*` | 🟢 |
| `change.c` | ~1000 | `inotify_*` | 🟢 |
| `sync.c` | ~3700 | `futex` syscall | 🟢 |

---

## 4. HarmonyOS 特定注意事项

- **动态链接器**: `ld-musl-aarch64.so.1` (ARM64) / `ld-musl-x86_64.so.1` (x86_64)
- **库搜索路径**: 通过 `LD_LIBRARY_PATH` 和 linker namespace 配置
- **HNP 打包**: Wine .so 文件和 wineserver 打包为 HNP 格式

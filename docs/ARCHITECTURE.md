# Wine on HarmonyOS — 架构设计文档

## 1. Wine 内部架构概述

```
┌──────────────────────────────────────────────────────────┐
│              Windows PE 可执行文件 (.exe)                  │
│              x86_64 指令，PE 格式                          │
├──────────────────────────────────────────────────────────┤
│                    ntdll.dll (PE 侧)                      │
│   ┌──────────────────────────────────────────────────┐  │
│   │  loader.c   virtual.c   thread.c   heap.c         │  │
│   │  (PE 加载器，Windows 语义)                          │  │
│   └──────────────┬───────────────────────────────────┘  │
│                  │ __wine_syscall_dispatcher              │
│                  │ (汇编 trampoline: PE→Unix 上下文切换)    │
│                  ▼                                        │
├──────────────────────────────────────────────────────────┤
│              ntdll/unix/ (Unix 侧，原生 ELF .so)           │
│   ┌──────────────────────────────────────────────────┐  │
│   │  signal_x86_64.c   thread.c   virtual.c           │  │
│   │  server.c          loader.c   sync.c              │  │
│   │  (POSIX/Linux 系统调用，实现 NT 语义)               │  │
│   └──────────┬───────────────────────────────────────┘  │
│              │ Unix Domain Socket (sendmsg/recvmsg)       │
│              ▼                                            │
│         ┌──────────┐                                      │
│         │wineserver│  (独立进程，事件驱动的 I/O 循环)      │
│         └──────────┘                                      │
└──────────────────────────────────────────────────────────┘
```

### 关键层次

| 层 | 文件位置 | 编译目标 | 功能 |
|---|---------|---------|------|
| PE DLL | `dlls/ntdll/` (不含 `unix/`) | PE (x86_64-w64-mingw32) | Windows NT API 实现，以 DLL 形式被 .exe 加载 |
| Unix .so | `dlls/ntdll/unix/` | ELF .so (native) | Unix 系统调用封装，实现 NT 语义所需的底层操作 |
| wineserver | `server/` | ELF 可执行文件 | 进程/线程管理，同步对象，文件句柄管理，窗口消息队列 |

### 两个关键桥接点

#### (A) NT Syscall Dispatcher (`__wine_syscall_dispatcher`)
- **位置**: `dlls/ntdll/unix/signal_x86_64.c` (汇编实现)
- **功能**: PE 代码调用 NT 系统调用时，切换上下文到 Unix
  - 保存 PE 寄存器状态
  - 切换 FS/GS 段基址：`PE TEB(%gs)` ↔ `pthread TEB`
  - 调用对应的 Unix 侧处理函数
- **关键 x86_64 API**: `arch_prctl(ARCH_SET_GS, ...)`, `arch_prctl(ARCH_SET_FS, ...)`

#### (B) Unix Call Dispatcher (`__wine_unix_call_dispatcher`)
- **位置**: `dlls/ntdll/unix/signal_x86_64.c` (汇编实现)
- **功能**: PE DLL 调用 Unix 函数（如在 PE 代码中加载 Unix .so）
- **调度表**: `dlls/ntdll/unix/loader.c` 中的 `unix_call_funcs[]` 数组

#### (C) wineserver 通信
- **位置**: `dlls/ntdll/unix/server.c`
- **协议**: Unix Domain Socket (`sendmsg`/`recvmsg`)
- **每个线程一个 socket**: 多线程程序有多个 server 连接

---

## 2. Phase 1 架构 (x86_64 HarmonyOS)

```
┌──────────────────────────────────────────────────────────┐
│             Windows x86_64 程序 (.exe)                    │
│             (与控制台应用开始：wineconsole, cmd.exe)        │
├──────────────────────────────────────────────────────────┤
│              Wine PE DLLs (x86_64-w64-mingw32)            │
│              ntdll.dll, kernel32.dll, user32.dll ...      │
├──────────────────────────────────────────────────────────┤
│         __wine_syscall_dispatcher (x86_64 汇编)            │
│              arch_prctl (FS/GS 段切换)                     │
├──────────────────────────────────────────────────────────┤
│          Wine Unix .so (原生 x86_64 ELF, musl libc)       │
│          ntdll.so, winecrt0.so, ...                      │
├──────────────────────────────────────────────────────────┤
│                      musl libc                            │  ← 关键适配层
├──────────────────────────────────────────────────────────┤
│              HarmonyOS Kernel (Linux 5.10/6.6)            │
│              (x86_64, standard syscalls)                   │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│                wineserver (独立进程)                       │
│   ┌────────────┐  ┌──────────────┐  ┌────────────────┐  │
│   │ epoll/poll  │  │ inotify      │  │ Unix Domain    │  │
│   │ I/O 循环    │  │ 文件变更通知   │  │ Socket 通信     │  │
│   └────────────┘  └──────────────┘  └────────────────┘  │
│                musl libc (原生 x86_64 ELF)                 │
│                HarmonyOS Kernel (x86_64)                   │
└──────────────────────────────────────────────────────────┘
```

**关键简化（相对于 ARM64 Phase 2）**：
- ❌ 不需要 CPU 指令翻译 (x86_64→x86_64)
- ❌ 不需要 ARM64EC ABI
- ❌ 不需要 Box64 或 FEX-Emu
- ❌ 不需要跨架构 PE 交叉编译
- ✅ 只需要 musl libc 兼容性适配

---

## 3. 构建流程设计

### 3.1 交叉编译（从 x86_64 Linux 主机到 x86_64 OHOS）

```
主机: x86_64 Ubuntu 26.04
目标: x86_64 HarmonyOS (aarch64-linux-ohosmusl 或 x86_64-linux-ohosmusl)

工具链: OHOS Native SDK (clang 15.0.4 + lld)
构建系统: autotools (configure) 或将其适配为 GN

步骤:
1. 配置 OHOS SDK 环境变量
2. ./configure --host=x86_64-linux-ohosmusl \
              --with-wine-tools=... (或交叉编译时使用原生 wine 工具)
3. make
4. 生成 ntdll.so (ELF), wineserver, wine PE DLLs
```

### 3.2 Wine PE DLL 编译

Wine 的 PE DLL 通常用 `mingw-w64` 交叉编译：
```
--with-mingw=clang   → 使用 llvm-mingw (aarch64-w64-mingw32-clang)
```

对于 Phase 1 (x86_64)，可能需要用：
```
x86_64-w64-mingw32-clang   → 标准 mingw PE 输出
```

**不确定点**: OHOS NDK 是否提供 mingw 交叉编译工具？可能需要从 llvm-mingw 项目单独获取。

### 3.3 预期的 Patch 流程

```
Phase 1 的 Wine 编译流程:
1. 源码: git clone https://github.com/wine-mirror/wine.git
2. Patch 01-15: 逐一打 musl 兼容性补丁
3. configure: OHOS NDK clang 交叉编译配置
4. 编译: 先成功编译 wineserver 和 ntdll.so
5. 测试: 运行 wineconsole + cmd.exe
```

---

## 4. 关键子系统和 HarmonyOS 适配

### 4.1 信号处理 (signal_x86_64.c)

```
关键功能:
- arch_prctl(ARCH_SET_GS, teb)   → 设置 GS 段基址 = TEB
- arch_prctl(ARCH_SET_FS, teb)   → 设置 FS 段基址 = pthread TEB
- arch_prctl(ARCH_GET_GS, &gs)   → 读取 GS 基址

HarmonyOS 内核支持:
- arch_prctl 是 x86_64 标准 syscall (NR=158)
- 如果 OH 使用 Linux 内核，应该支持
- 但需要确认 OH 内核配置是否启用了对应的功能

风险:
- arch_prctl 可能被 seccomp 过滤 (HarmonyOS 可能有安全策略)
- PR_SET_SYSCALL_USER_DISPATCH 可能在较老内核上不可用
```

### 4.2 wineserver I/O 循环

```
fd.c 的事件循环有 4 层 fallback:
1. epoll_pwait2  (Linux 5.11+, 纳秒超时)
2. epoll_wait     (标准 epoll)
3. kqueue         (macOS/FreeBSD)
4. poll()          (POSIX 纯 poll)

HarmonyOS 应该支持 epoll (Linux 内核)。
```

### 4.3 文件变更通知 (change.c)

```
使用 inotify_init / inotify_add_watch
- musl 在 Linux 上支持 inotify
- HarmonyOS 内核应该支持 inotify (如果是 Linux 内核)
- 如果不支持，wine 的文件变更通知功能降级为轮询
```

### 4.4 线程管理 (thread.c)

```
问题 API:
- pthread_setname_np(nameA) — 1 参数形式 (设置当前线程名)
  musl 原生支持此签名；glibc 通过 _GNU_SOURCE 宏提供
- sched_getcpu() — POSIX，musl 支持

不需要适配:
- pthread_create, pthread_join, pthread_sigmask — 标准 POSIX
```

---

## 5. Phase 1 → Phase 2 的演进路径

Phase 1 成功后进入 Phase 2 (ARM64)，需要增加：

```
Phase 2 新增组件:
┌─────────────────────────────────────────┐
│  Windows x86_64 程序 (guest)            │
│  [FEX: x86_64 → ARM64 模拟]            │  ← 新增
│  [ARM64EC ABI 桥接层]                   │  ← 新增
│  Wine PE DLLs (ARM64 native)           │
│  Wine Unix .so (ARM64 native, musl)    │
│  HarmonyOS Kernel (ARM64)              │
└─────────────────────────────────────────┘

Phase 2 新增挑战:
- 需要 port FEX-Emu 到 musl
- 需要 ARM64EC PE 交叉编译 (bylaws-llvm-mingw)
- 需要处理 ARM64 信号处理的 ucontext_t 布局差异
- 需要 Box64 for 32-bit x86 app 支持
```

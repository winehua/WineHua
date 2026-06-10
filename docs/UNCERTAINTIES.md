# 不确定点和技术风险清单

> 本文档列出 Wine on HarmonyOS 移植的所有已识别不确定点，按风险等级排序。

---

## P0 — 阻塞性问题（已验证解决 ✅）

### U01: HarmonyOS 是否支持 x86_64 目标？ → ✅ 已确认
- **验证结果**: OHOS SDK 明确支持 `x86_64-linux-ohos` 目标
- **证据**: 
  - `/apps/harmony/sdk/default/openharmony/native/sysroot/usr/lib/x86_64-linux-ohos/` 存在
  - cmake toolchain 定义了 `OHOS_ARCH=x86_64` → `OHOS_TOOLCHAIN_NAME=x86_64-linux-ohos`
  - CRT 对象完整: `Scrt1.o`, `crt1.o`, `crti.o`, `crtn.o`
  - libc.so: `ELF 64-bit LSB shared object, x86-64`
- **状态**: ✅ **已解决**

### U02: HarmonyOS kernel 是否支持 `arch_prctl` syscall？ → ✅ 头文件确认
- **验证结果**: OHOS x86_64 头文件中完整定义了 arch_prctl
- **证据**:
  - `/usr/include/x86_64-linux-ohos/asm/prctl.h`: `ARCH_SET_GS=0x1001`, `ARCH_SET_FS=0x1002`, `ARCH_GET_FS=0x1003`, `ARCH_GET_GS=0x1004`
  - `/usr/include/x86_64-linux-ohos/bits/syscall.h`: `__NR_arch_prctl = 158`
  - `/usr/include/x86_64-linux-ohos/asm/unistd_64.h`: `#define __NR_arch_prctl 158`
- **注意**: 头文件存在表明编译没问题，但**运行时**需要 OHOS x86_64 内核真正执行此 syscall（需真机验证）
- **状态**: 🟡 **编译期确认，运行时待验证**

### U03: OHOS NDK 是否可用？ → ✅ 已验证
- **验证结果**: OHOS SDK 完整安装在 `/apps/harmony/sdk/default/openharmony/native/`
- **工具链**: Clang 15.0.4, LLD, LLVM tools
- **Sysroot**: `/apps/harmony/sdk/default/openharmony/native/sysroot/`
- **cmake toolchain**: `/apps/harmony/sdk/default/openharmony/native/build/cmake/ohos.toolchain.cmake`
- **状态**: ✅ **已解决**

---

## P0.5 — 新发现的编译期阻塞问题

### U04-NEW: `program_invocation_name` 缺失 (vkd3d)
- **位置**: `libs/vkd3d/libs/vkd3d/utils.c:886,892,899,905`
- **问题**: musl 不提供 `program_invocation_name` 全局变量（glibc 特有）
- **影响**: vkd3d 编译失败
- **修复**: 用 `#ifdef __GLIBC__` 保护，或从 `/proc/self/exe` 获取
- **状态**: 🔴 **需要 patch**

### U05-NEW: `dladdr1` 和 `dlinfo` 双缺失
- **位置**: `libs/winecrt0/dll_soinit.c:99-104`
- **问题**: musl 既不提供 `dladdr1` 也不提供 `dlinfo`。代码的 fallback 链从 `dladdr1` → `dlinfo`，两者都不可用
- **影响**: 动态库初始化时无法获取 link_map
- **修复**: 添加第三个 fallback：使用 `_DYNAMIC` + aux vector `AT_PHDR`，或 `dl_iterate_phdr`
- **状态**: 🔴 **需要 patch**

### U06-NEW: `libc.so.6` 硬编码名称
- **位置**: `dlls/ntdll/unix/signal_x86_64.c:2759`
- **问题**: `strcmp(p, "libc.so.6")` — musl 的库名是 `libc.so` 而非 `libc.so.6`
- **影响**: syscall dispatch 初始化时找不到 musl libc
- **修复**: 同时匹配 `"libc.so"` 和 `"libc.so.6"`，或使用 `dlsym(RTLD_DEFAULT, "malloc")` 反查
- **状态**: 🔴 **需要 patch**（该代码在 `syscall_dispatch_enabled` 时激活）

## P1 — 高风险（影响核心功能）

### U04: `asm/prctl.h` 头文件位置
- **问题**: `signal_x86_64.c:102` 包含了 `#include <asm/prctl.h>`，该头文件来自 Linux kernel headers
- **影响**: 如果在 OHOS musl sysroot 中找不到，需要指定正确的 kernel headers 路径
- **当前代码**: Wine 已内联 `arch_prctl` 函数使用 `syscall(__NR_arch_prctl, ...)`
- **缓解**: 可能可以去除 `asm/prctl.h` 依赖，手动定义需要的常量
- **状态**: 🟡 **需要验证**

### U05: `PR_SET_SYSCALL_USER_DISPATCH` 支持
- **问题**: `signal_x86_64.c:2611` 使用了 Linux 5.11+ 的 syscall user dispatch 特性
- **影响**: 如果 OHOS 内核版本 < 5.11 或没有启用 CONFIG_SYSCALL_USER_DISPATCH，此功能不可用
- **缓解**: 代码已被 `#ifdef PR_SET_SYSCALL_USER_DISPATCH` 保护，不支持时会走 fallback
- **状态**: 🟡 **功能降级，不阻塞**

### U06: Wine 信号处理在 OHOS 上的精度
- **问题**: Wine 依赖精确的信号处理（SIGSEGV, SIGILL, SIGBUS, SIGFPE）
- **影响**: OHOS 可能有 seccomp 策略限制某些信号，或修改了信号处理的默认行为
- **验证方式**: 参考 Box64 OHOS 移植 的 `abitest/signal_test.cpp` 和 `syscall_test.cpp`
- **状态**: 🟡 **需要真机测试**

### U07: futex syscall 在 OHOS 上的可用性
- **问题**: Wine 使用 `syscall(__NR_futex, ...)` 实现 NT 同步原语
- **位置**: `dlls/ntdll/unix/sync.c:126-134`
- **影响**: 如果 OHOS 对 futex 有限制，NT 锁/信号量/事件将不可用
- **缓解**: Box64 OHOS 移植 已验证 futex 在 OHOS ARM64 上工作
- **状态**: 🟢 **ARM64 已验证，x86_64 待确认**

---

## P2 — 中等风险（可能影响部分功能）

### U08: PE 交叉编译工具链
- **问题**: Wine 的 PE DLL 需要用 mingw 交叉编译。OHOS NDK 可能不提供 mingw
- **影响**: 需要单独安装 llvm-mingw (`x86_64-w64-mingw32-clang`)
- **缓解**: llvm-mingw 是独立项目，可以单独获取
- **状态**: 🟡 **需要获取工具**

### U09: 库搜索路径不匹配
- **问题**: Wine 期望 `/usr/lib/wine/`，OHOS 使用 `/system/lib64/` 等路径
- **影响**: Wine 的 .so 文件安装后无法被正确找到
- **缓解**: 通过 `LD_LIBRARY_PATH` 或 linker namespace 配置
- **状态**: 🟡 **配置问题，可解决**

### U10: wineserver 的 Unix Domain Socket 路径
- **问题**: wineserver 使用 `~/.wine/` 下的 socket 文件
- **影响**: OHOS 的文件系统布局可能不同
- **缓解**: 通过 `WINEPREFIX` 环境变量控制
- **状态**: 🟢 **可配置**

### U11: `/proc` 文件系统兼容性
- **问题**: Wine 读取 `/proc/self/maps`, `/proc/self/pagemap`, `/proc/cpuinfo` 等
- **影响**: OHOS 的 `/proc` 格式可能与标准 Linux 不同（从 musl 文档可知有差异）
- **位置**: `dlls/ntdll/unix/system.c`, `virtual.c`
- **缓解**: 检查 OHOS `/proc` 格式，必要时做字段映射
- **状态**: 🟡 **需要验证**

### U12: Wine 的 `--with-mingw=clang` 配置
- **问题**: Wine configure 的 `--with-mingw=clang` 选项需要 llvm-mingw 工具链
- **影响**: 如果没有正确的 mingw 工具链，Wine PE DLL 编译会失败
- **状态**: 🟡 **需要验证工具链可用性**

---

## P3 — 低风险（优化项，不阻塞）

### U13: `pthread_setname_np` 签名检测
- **问题**: glibc (2 arg) vs musl (1 arg)，需要 configure 正确检测
- **缓解**: vkd3d 已有 `HAVE_PTHREAD_SETNAME_NP_1` / `_2` 守卫模式
- **状态**: 🟢 **已有模式可参考**

### U14: `MAP_NORESERVE` 在 OHOS 上
- **问题**: Wine 使用 `MAP_NORESERVE`，如果 OHOS 内核不支持，Wine 已有 fallback 定义
- **状态**: 🟢 **已有 fallback**

### U15: HNP 打包格式
- **问题**: 如何在 HarmonyOS 真机上分发 Wine
- **缓解**: 参考 Box64 OHOS 移植 的 HNP 打包脚本
- **状态**: 🟡 **参考已有，但 Wine 比 Box64 复杂**

### U16: 图形栈 (Phase 1 延后)
- **问题**: Wine 的图形输出需要 X11 或 Wayland
- **Phase 1 策略**: 先不解决，只跑控制台应用
  - 可以用 `wineconsole` (文本模式)
  - 可以用 Wine 的 `ttydrv` 驱动（纯文本渲染）
- **状态**: ⬜ **Phase 2/3 再处理**

---

## 已知的 HarmonyOS 与 Linux 差异（影响评估）

| 差异项 | 来源 | 对 Wine 的影响 |
|--------|------|---------------|
| musl `LINE_MAX` 未定义 | OHOS FAQ | 如果 Wine 某处引用了 `LINE_MAX`，需手动定义 |
| `/proc/cpuinfo` 格式不同 | OHOS FAQ | `system.c` 中的 CPU 特性检测可能失败 |
| 动态链接器: `ld-musl-*.so.1` | musl 标准 | `.so` 文件的 SONAME 和路径需要适配 |
| 动态库 namespace 隔离 | OHOS linker 机制 | Wine .so 需注册到合适的 namespace |
| seccomp 策略 | OHOS 安全机制 | 可能限制某些 syscall |
| `__NR_arch_prctl` = 158 | Linux x86_64 ABI | x86_64 标准值，应兼容 |

---

## Phase 2 的不确定点（预识别）

以下问题在 Phase 2 (ARM64) 才会成为关键，但提前列出以供参考：

1. **FEX-Emu 的 musl 移植** — 目前无已知 musl 版本，工作量大
2. **ARM64EC PE 交叉编译** — 需要 by-laws/llvm-mingw (ARM64EC 目标)
3. **ARM64 `ucontext_t` 布局** — glibc 和 musl 可能有差异
4. **ARM64 段寄存器** — ARM64 没有 FS/GS，使用 TLS 寄存器 (TPIDR_EL0)
5. **Box64 的 `HODLL` 机制** — Hangover 通过 Wine WoW64 加载 emulator DLL

---

## 下一步行动（建议优先级）

1. **获取 OHOS x86_64 SDK** — 验证 x86_64 目标支持
2. **确认 `arch_prctl` 可用性** — 在 OHOS x86_64 上测试
3. **获取 llvm-mingw** — 准备 PE 交叉编译工具链
4. **尝试编译 wineserver** — 先编译最简单的组件，验证工具链链路
5. **编写 configure 适配** — 添加 musl 特性检测

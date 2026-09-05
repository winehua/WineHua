# 方案③ Heaven Qt / DX11 崩溃修复

> 日期: 2026-09-05
> 范围: 原生 aarch64 Wine + FEX (`HODLL64=libarm64ecfex.dll`) + `HODLL=wowbox64.dll`
> 子模块: `thirdparty/wine` `d2ceee0f451` · `thirdparty/box64` `16515448b`

方案②（`box64.so` 吃 Unix SIGSEGV）能跑 Heaven 的 Qt 启动器；方案③把 32 位 PE 交给 **wowbox64.dll**（ARM64 PE），宿主信号先走 OHOS musl sigchain。未声明的 SIGSEGV 被 DFX `ProcessDump` 吃掉，线程冻结，表现为 Qt 白屏。随后 Heaven.exe 在 DXVK 加载前因 FS 基址为 NULL 立刻崩溃。

本文只记录崩溃修复的方法和理由。帧率不在本次范围内。

---

## 1. Qt 白屏：DFX 抢走 SIGSEGV

**现象:** `heaven.bat` → `browser_x86.exe` 白屏。内核 SIGSEGV `SEGV_ACCERR` 打在 dynarec 对客户页的写保护上。

**原因:** OHOS musl sigchain 先跑 special handler。Wine 的 `sigaction(SIGSEGV)` 只填 usr 槽，DFX `ProcessDump` 作为 special 先跑，把 SMC 故障当成真崩溃 dump，线程不再回到 wowbox64。

**方法:**

1. `signal_init_process` 在安装 `segv_handler` 之后调用 `ohos_install_sigchain_fault_handlers`。
2. 通过 `AddSpecialSignalHandlerFn` / `add_special_signal_handler` 声明 SIGSEGV / SIGBUS / SIGILL；找不到符号则 `rt_sigaction` 直连内核。
3. special handler **对 SIGSEGV/SIGBUS 始终 return 1**（认领），避免 DFX dump。不要对 SIGSEGV `return 0`。
4. 非 dynarec 的 SIGILL `return 0`，让 Wine `ill_handler` 继续跑。

---

## 2. SMC 所有权：TEB trampoline + unix mprotect

**现象:** 从 ELF 信号处理器直接调 PE `wowbox64_handle_host_fault` 会挂死；`unprotectDB` → `NtProtectVirtualMemory` 在 POSIX handler 里也会挂；handler 里 `fprintf` 死锁。

**原因:**

| 约束 | 说明 |
|------|------|
| ARM64 PE 的 x18 | 必须是 TEB（Rtl 临界区 / GS cookie） |
| JIT 的 x18 | 客户机 R8，不是 TEB |
| JIT 的 x0 / x27 | `xEmu` / 客户 RIP。`native_epilog` 先存 JIT 寄存器，再 `ldr x18, [x0, #3104]` 取 TEB |
| POSIX 里调 NT | `NtProtectVirtualMemory` 不能在这个 handler 里走 |
| FILE 锁 | `fprintf` 在 handler 里会锁死 |
| W^X | OHOS 拒 RWX；SMC 只需把客户页恢复成可写 |

**方法:**

1. `ohos_call_pe_fault`：保存 x18，把 x18 设成 TEB，调用 PE，再恢复 x18。不要在跳 epilog 之前把 ucontext 的 x18 改成 TEB。
2. wowbox64 经 unixlib 序号 **8**（`unix_ohos_set_wowbox64_fault`）注册 handler，并拿到 `ohos_mprotect_exec` 函数指针。
3. 仅当 `wowbox64_in_host_fault` 时，`unprotectDB` 走 unix `ohos_mprotect_exec`，恢复 **RW 而非 RWX**。
4. `[SMC]` 日志用 `write(2)`。wowbox64 是 `-nostdlib`，不能 `fprintf`。musl 把 `si_addr` 定义成宏，C 形参不要叫 `si_addr`。
5. 同一故障里 SMC unprotect 和 Wine SEH **互斥**：

```c
if (wowbox64_handle_host_fault(...)) return true;  /* 禁止再进 wine_segv_handler */
wine_segv_handler(...);
return true;  /* 仍认领，DFX 不会 dump */
```

期望日志：

```
[SMC] tid=... sig=... addr=... pc_in=... prot=... dynarec=0/1 result=... pc_out=... wine=0/1
```

**不要**出现 `dynarec=1 wine=1 pc_out=KiUserExceptionDispatcher`。

不要把 Wine vprot / `ohos_mprotect_exec` 扩到 SMC 内核 WRITE 恢复以外。

---

## 3. SIGILL / 非 SMC 的 JIT SIGSEGV：走 native_epilog

**现象:** `BOX64_DYNAREC_CALLRET=2` 时 `MarkDynablock` 把 callret 打成 `ARCH_UDF`（`0xcafe`），这是故意的 SIGILL。JIT 里非 SMC 的 SIGSEGV 若进 Wine SEH，会在 `KiUserExceptionDispatcher` 里挂死。

**原因:** Wine SEH 假定当前栈是 ARM64 PE 异常帧。Dynarec 的 SP 不是那套栈。

**方法:**

| 故障 | 处理 |
|------|------|
| SIGILL 且 PC 在 dynablock | `dynablock_leave_runtime`，PC ← `native_epilog`。客户 RIP 已在 x27，不要用 `getX64Address` 去猜 callret |
| SIGILL 且 PC 不在 dynablock | `NOT_MINE`，ntdll `return 0`，Wine `ill_handler` |
| SIGSEGV `PROT_DYNAREC` 写保护 | `unprotectDB`；若写的是当前块则 epilog，否则 retry |
| SIGSEGV `PROT_DYNAREC_R` | 客户本来就不能写：unprotect 后交给 Wine SEH |
| SIGSEGV 且 PC 在 dynablock、但不是 SMC | epilog，**不要** `wine_segv_handler` |

`native_epilog` 需要 ucontext 里仍是 `x0=xEmu`、`x27=xRIP`。

---

## 4. Heaven.exe DX11 立刻崩溃：FS 基址为 NULL

**现象:** Qt 已能点 RUN。`heaven.exe -video_app direct3d11` 立刻：

```
[BOX64] GetSegmentBase does not apply to Wine dlls
wine: Unhandled page fault on read access to 00000000 at address 7AA4FAD8
```

DXVK 还没初始化。随后 winedbg 对 JIT NULL deref 循环 SIGSEGV（`result=epilog`），未限流的 `[SMC] enter` 把 `wine_stderr` 撑到约 494MB。

**原因:** Wine 桩 `GetSegmentBase` 返回 NULL，并覆盖 `BTCpuSimulate` 已经用 `calculate_fs()` 装好的 32 位 TEB（`TEB.WowTebOffset`，TEB+0x180c）。解释器 FS: 前缀和 dynarec MOV/POP FS 都会走 `GetSegmentBaseEmu`，下一句 `fs:[disp]` 读 0。

**方法:**

- `GetSegmentBase` / `GetSeg43Base` 返回 `calculate_fs()`（32 位 TEB），不再返回 NULL。
- `[SMC] enter` 与 result 行同一套限流：前 64 条全打，之后每 32 条打一条。

---

## 5. 宿主侧配套（WineHua）

这些不是信号路径本身，但方案③要能把 Heaven 送到上述修复上：

- `HODLL=wowbox64.dll`，`HODLL64=libarm64ecfex.dll`
- 默认 D3D `dxvk_legacy`；GameHook 走产品 env overlay，不覆盖 `WINEDEBUG` / `DXVK_LOG`
- 方案③ `LIBGL_DRIVERS_PATH=.../libs/arm64`；guest GL 用 `GALLIUM_DRIVER=virpipe`，不要设 `MESA_LOADER_DRIVER_OVERRIDE=virpipe`
- 打包 ARM64X `libc++.dll` / `libunwind.dll` 到 DXVK 旁，避免 `STATUS_DLL_NOT_FOUND`
- GameHook URI 解码启动参数（`-config` 必须编成 `%2Dconfig`）

跳过 launcher、改 `MESA_LOADER_DRIVER_OVERRIDE` 不当作本修复。

---

## 6. 涉及文件

| 文件 | 作用 |
|------|------|
| `thirdparty/wine/dlls/ntdll/unix/ohos_virtual.c` | sigchain、TEB trampoline、`write(2)` SMC 日志与限流 |
| `thirdparty/wine/dlls/ntdll/unix/signal_arm64.c` | 安装 sigchain；无 ESR 的 `SEGV_ACCERR` 且 addr≠PC → WRITE |
| `thirdparty/wine/dlls/ntdll/unixlib.h` | 序号 8 `unix_ohos_set_wowbox64_fault` |
| `thirdparty/box64/wine/wow64/wowbox64.c` | 宿主故障处理、unixlib 注册、FS 指纹日志 |
| `thirdparty/box64/src/custommem.c` | WIN32 `unprotectDB` 恢复 RW；handler 内 unix mprotect |
| `thirdparty/box64/src/os/os_wine.c` | `GetSegmentBase` → `calculate_fs()` |
| `entry/.../wine_child.cpp` | 方案③ HODLL / HODLL64 |
| `entry/.../graphics_broker.cpp` | 方案③ dri 路径 |
| `entry/.../GameHook.ets` | URI 解码、产品 env、可选点 RUN |

---

## 7. 验收

Qt 启动器能出来，点 RUN 后 D3D11 Heaven 进游戏且不弹崩溃框。

`wine_stderr` 应有：

```
[ntdll] OHOS sigchain claimed SIGSEGV/SIGBUS/SIGILL ...
[wowbox64] registered host fault handler status=00000000 ...
[wowbox64] GetSegmentBase FS=WowTebOffset
```

SMC 行里 dynarec 故障应为 `result=retry` 或 `result=epilog` 且 `wine=0`。

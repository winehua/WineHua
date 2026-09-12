# OHOS/ARM64EC：RPC stubless 代理读错参数槽（Steam 致命崩溃的根因）

> 日期：2026-09-12
> 复现入口：`aa start ... --ps winehua.mode game --ps winehua.game_path 'Z:\games\comprobe.exe'`
> 结论：**官方 x86-64 Steam 客户端崩溃不是 Steam 的问题，也不是 Valve 基线的问题——
> 是我们这份 ARM64EC Wine 的 RPC stubless 客户端代理在编组参数时读到了错误的内存，
> 触发 `0xC0000005` 写地址 `0x10`。最小复现是 `OpenSCManagerW`。**

## 1. 一条命令级的最小复现

探针源码在 `docs/proton-parity/tools/fontprobe/comprobe.exe`（x86-64 PE，无需改 HAP）：

| 步骤 | 调用 | 结果 |
| --- | --- | --- |
| 1 | `CoInitializeEx` | `hr=0x00000000` |
| 2 | `CoCreateInstance(CLSID_ShellLink)` | `hr=0` |
| 3 | `CoCreateInstance(CLSID_FileOpenDialog)` | `hr=0` |
| 4 | `CoCreateInstance(CLSID_StdGlobalInterfaceTable)` | `hr=0` |
| 5 | `UuidCreate` | `rs=0` |
| 6 | `RpcStringBindingComposeW(...)` | `rs=0` |
| 7 | `RpcBindingFromStringBindingW` | `rs=0` |
| 8 | `RpcBindingFree` | `rs=0` |
| 9 | **`OpenSCManagerW(L"", L"", ...)`** | **进程死亡（`exit=5`）** |

把非空调用放在前面、`OpenSCManagerW(NULL, NULL, …)` 放在最后再跑一次：

```text
STEP 9 OpenSCManagerW(L"", L"") - non-NULL strings
（此处进程直接死亡，没有 RESULT 行）
```

⇒ **触发条件不是 NULL 指针，而是这个 RPC 调用本身**。`[in,unique]` 允许 NULL，
服务端 `programs/services/rpc.c: svcctl_OpenSCManagerW` 也显式处理 NULL/空串，
所以是我们这边的编组问题。

## 2. Steam 的崩溃与它是同一个

官方客户端的 dump（三次冷启动三次一致）：

```text
exceptionCode = 0xC0000005
address       = 0x6FFF864AAC   → rpcrt4.dll+0x64aac
param[0] = 1（写）, param[1] = 0x10（被访问地址）
```

把设备上的 `files/wine/bin/aarch64-windows/rpcrt4.dll` 拉下来反汇编，
`+0x64aac` 正好是：

```asm
180064aa0: and  w8, w8, #0x60
180064aa4: cmp  w8, #0x20
180064aa8: b.ne 0x180064ab0
180064aac: str  xzr, [x19]      ; ← 崩溃点：写 0 到 [x19]，x19 = 0x10
```

打开 RPC 通道后（`WINEDEBUG=err+all,warn+all,+rpc`）拿到决定性日志：

```text
0178:trace:rpc:client_do_args param[0]: 0000007E9BA3EC80 type 12 MustSize MustFree IsIn
0178:trace:rpc:client_do_args param[1]: 0000007E9BA3EC88 type 12 MustSize MustFree IsIn
0178:trace:rpc:client_do_args param[2]: 0000007E9BA3EC90 type 08 IsIn IsBasetype
0178:trace:rpc:client_do_args param[3]: 0000007E9BA3EC98 type 30 IsOut IsSimpleRef
0178:warn:seh:dispatch_exception EXCEPTION_ACCESS_VIOLATION exception (code=c0000005) raised
0178:trace:rpc:RpcExceptionFilter 0xc0000005
wine: Unhandled page fault on write access to 0000000000000010 at address 0000006FFF864AAC (thread 0178), starting debugger...
```

崩溃发生在 **`client_do_args` → 格式串解释器** 里，也就是 NDR stubless 客户端代理
处理 `svcctl_OpenSCManagerW` 的 4 个参数时（`type 12` = `FC_UP` unique 指针 ×2，
`type 08` = `FC_LONG`，`type 30` = out 简单引用）。

Steam 调用它的路径与栈证据吻合：

```text
steamui.dll → sechost.dll（服务控制）→ rpcrt4.dll → 崩溃
steam 自己的 service_log.txt: "Failed to load Steam Service (GLE 126)"
```

## 3. 相关代码（都在上游 Wine 里，不是我们新加的）

`dlls/rpcrt4/ndr_stubless.c`：

```c
#elif defined(__arm64ec__)
CLIENT_CALL_RETURN __attribute__((naked)) NdrClientCall2( PMIDL_STUB_DESC desc, PFORMAT_STRING fmt, ... )
{
    asm( ...
         "stp x29, x30, [sp, #-0x20]!\n\t"
         "stp x2, x3, [x4, #-0x10]!\n\t"   /* 把两个寄存器实参压到 x4 指向的 x64 栈上 */
         "mov x2, x4\n\t"                  /* stack_top */
         "mov x3, #0\n\t"                  /* fpu_stack */
         "bl \"#NdrpClientCall2\"\n\t" ... );
}
```

对应上游提交：`3c8fc4927d7 rpcrt4: Leave some space on the stack for varargs when called from ARM64EC code.`

即：**这段 ARM64EC trampoline 是上游代码，我们并未改动**。而 `StackTop + params[i].stack_offset`
给出的四个参数地址是连续的 `+0,+8,+0x10,+0x18`，正是 x86-64 上该 trampoline 搬移实参后的布局。

因此故障面只有两种可能，且**必须二选一验证**：

| 候选 | 含义 | 验证方式 |
| --- | --- | --- |
| C1 | `x4`（ARM64EC 里代表 x64 的 RSP）**没有按 x64 ABI 传进来** —— 也就是 **FEX 的 ARM64EC 调用约定实现有偏差** | 在 trampoline 里把 `x4`、`x2`、`x3` 与调用方真实栈位置 dump 出来比对；对照参考 FEX（`1cc4b93e`，Proton 锁定版本）中 `Source/Windows/ARM64EC/*` 的实现 |
| C2 | Wine 侧 `rpcrt4` 的 ARM64EC 格式串 / `args_regs_to_stack` 路径与实际 trampoline 布局不一致 | 在 `NdrpClientCall2` 里 dump `stack_top`、`stack_size`、每个 `params[i].stack_offset`，与 x64 逐项对照 |

**本轮不做无证据的补丁**：改 `ndr_stubless.c` 的汇编或 FEX 的 ARM64EC 约定都属于
ABI 级改动，必须先拿到 C1/C2 的判定证据。

## 4. 为什么这条线索重要

1. 它把「Steam 起不来」从「兼容性玄学」变成了**一个有 9 步最小复现的 ABI 缺陷**。
2. 它与 Valve/WineHQ 基线之争**无关**：`OpenSCManagerW` 在两个基线上都会走同一段
   `ndr_stubless.c`（这段 ARM64EC trampoline 上游就有）。
3. 它同时解释了两个次要现象：`Failed to load Steam Service (GLE 126)`
   与客户端 ~20–40 s 后的必崩。
4. 它给出了明确的下一步：**先判定 C1 还是 C2**。若是 C1，修的是 FEX 的 ARM64EC 约定
   （并可借 R 线的 FEX 参考版本做对照）；若是 C2，修的是我们 Wine 的 rpcrt4。

## 4.1 二分结果：只有 64 位 ARM64EC 路径坏（32 位全过）

把同一份 `comprobe.c` 交叉编译成 **i686**（32 位 PE）再跑一次，
**17 步全部走完**，包括 `OpenSCManagerW(L"",L"")`、`OpenServiceW`、
`EnumServicesStatusExW`、`CoMarshalInterface`、以及最后那个 `OpenSCManagerW(NULL,NULL)`：

```text
STEP 9  OpenSCManagerW("","")     handle=001E6390 gle=0
STEP 10 OpenServiceW(Steam)       handle=001E63C0 gle=0
STEP 11 EnumServicesStatusExW     ok=0 needed=3392 gle=234
STEP 13 CoMarshalInterface        hr=0x00000000
STEP 17 OpenSCManagerW(NULL,NULL) handle=00CE4FD8 gle=0
DONE - all steps completed
```

⇒ 结论：

- Wine 的 RPC / 服务控制 / 命名管道**功能本身是好的**，`services.exe` 链路通；
- 坏的只有 **x86-64 应用 → ARM64EC Wine DLL** 这条 64 位路径；
- 因此**不是 C2（格式串）**：同一份 stubless 格式生成逻辑在 32 位侧工作正常。
  剩下的就是 **C1：x64↔ARM64EC 的互操作约定**。

## 4.2 C1 的具体位置：FEX 的 ARM64EC 退出/进入 thunk

我方 Wine 的 `NdrClientCall2` ARM64EC trampoline 与 **上游 winehq master（2026-09-09，
`788d90c4`）逐字相同**，也不是我们改坏的：

```asm
/* dlls/rpcrt4/ndr_stubless.c, __arm64ec__ 分支（上游 3c8fc4927d7 之后） */
stp x29, x30, [sp, #-0x20]!
stp x2, x3, [x4, #-0x10]!     /* 把两个寄存器实参压栈，x4 -= 0x10 */
mov x2, x4                    /* stack_top = x4 */
bl NdrpClientCall2
```

而 `x4` 由运行时的 ARM64EC 调度器提供 —— 在我们的架构里就是 **FEX**：

```asm
/* thirdparty/fex/Source/Windows/ARM64EC/Module.S: ExitFunctionSuspendResumePoint */
mov x4, sp
tbz x4, #3, ret_sp_misaligned
ldr lr, [x4], #0x8            /* 弹出返回地址，x4 += 8 */
mov sp, x4
ret_sp_aligned:
br x17                        /* 进入 ARM64EC 入口 thunk，此时 x4 = 原 RSP + 8 */
```

### 量纲对不上：差一个 32 字节 shadow space

x86-64 调用约定里，`[RSP]` 是返回地址，`[RSP+8 .. RSP+0x28]` 是 **32 字节 home/shadow space**，
**栈传参从 `[RSP+0x28]` 开始**。

| 路径 | `stack_top` | `stack_top+0x10`（格式串认为的第 3 个参数） | 真实第 3 个参数位置 |
| --- | --- | --- | --- |
| x86-64 原生 trampoline | `RSP+0x18` | `RSP+0x28` ✅ | `RSP+0x28` |
| ARM64EC trampoline + FEX 现有的 `x4`（=`RSP+8`） | `(RSP+8)-0x10 = RSP-8` | `RSP+8` ❌（读到 shadow space） | `RSP+0x28` |

**偏差正好是 0x20（32 字节 shadow space）。** 这与本轮观测完全吻合：

```text
params 地址 = stack_top + 0, +8, +0x10, +0x18   ← 格式串假设四个参数连续
```

即：第 1、2 个参数（寄存器传参，被 trampoline 正确压栈）没问题，
**第 3、4 个参数（栈传参）读到了 shadow space / 返回地址那一带**，
于是 `SC_RPC_HANDLE *handle` 这个出参指针变成垃圾值，
最终在格式串解释器里写出 `str xzr,[x19]`（x19 = 0x10）——正是 Steam dump 里那个
"写地址 0x10"的 `0xC0000005`。

也解释了为什么**普通 ARM64EC 调用不出问题**：像 `CreateFileW`（7 个参数）这类
非可变参数函数，ARM64EC ABI 直接用 `x0..x6` 传参，根本不经过这条栈路径。
只有**可变参数**（`NdrClientCall2` 就是）才走 x64 ABI 的栈传参。

### 4.3 又一步二分：普通可变参数调用是好的，坏的只有 Wine 那个手写 trampoline

新增探针 `tools/fontprobe/varargprobe.c`（x86-64 PE），专门打「x64 调 ARM64EC 可变参数函数
且实参走栈」这条路径：

```text
STEP 0 local_sum(8, 11..88) = 396 (expect 396)      ← 本模块内 x64 可变参数，基线
STEP 1 sprintf(msvcrt, 8 个 %d)  out = [11 22 33 44 55 66 77 88]   ✅
STEP 2 wsprintfW(user32, 8 个 %d) out = [11 22 33 44 55 66 77 88]   ✅
STEP 3 wsprintfA(user32, 8 个 %d) out = [11 22 33 44 55 66 77 88]   ✅
STEP 4 sprintf("a=%d b=%s c=%d d=%s e=%d", 1,"two",3,"four",5)      ✅
```

8 个实参里前 2 个走寄存器、后 6 个**必须走 x64 栈**，全部正确。

⇒ **推翻了「FEX 的 x4 全局传错」这个版本**：如果 x4 全错，clang 编译的
`sprintf` / `wsprintf` 的可变参数也必然读错。既然它们对，说明编译器侧的
ARM64EC 可变参数约定与环境是自洽的。

⇒ 因此问题**收敛到唯一一处**：Wine `dlls/rpcrt4/ndr_stubless.c` 里
**手写的 ARM64EC `NdrClientCall2` trampoline**，它对 `x4` 的假设与运行环境不一致。

### 4.4 结论：`x4` 语义已由 clang 代码生成确定，Wine 的 trampoline 缺了 shadow-space 跳过

用 llvm-mingw 的 clang 直接编译一个 ARM64EC 变参函数，看它自己生成的入口 thunk：

```asm
$ientry_thunk$cdecl$i8$varargs:
    stp  q6, q7, [sp, #-176]!
    ... 保存 x29/x30 与 q6-q15 ...
    add  x4, x4, #32        ; ★ 编译器自己加 32
    mov  x5, xzr
    blr  x9
```

函数体里则用调整后的 `x4`：

```asm
vsum:
    stp  x1, x2, [x4, #-24]!
    str  x3, [x4, #16]
    str  x4, [sp, #8]       ; va_list 的溢出区指针
```

⇒ 权威结论（来自编译器，不是推断）：

```text
进入 ARM64EC 变参函数时，x4 = x64 RSP + 8（shadow space 起点）；
第一个 x64 栈上实参在 x4 + 0x20。
```

而 Wine 手写的 `NdrClientCall2` ARM64EC trampoline **没有这一步**，
所以它读到的第 3/4 个实参落在 shadow space 里。

**修复**：在 4 个手写 ARM64EC thunk 里补上跳过 shadow space 的一步
（与编译器生成的 thunk 完全一致）。补丁：

```text
scripts/patches/wine-arm64ec-ndr-shadow-space.patch
sha256 46b79b0356ca12d0968d36d35ae40ef1c4807291f13e3689b06717463db6c954
改动  dlls/rpcrt4/ndr_stubless.c ×4：
      NdrClientCall2 / NdrAsyncClientCall / NdrClientCall3 / Ndr64AsyncClientCall
```

等价写法（我们做真机快速验证时用的字节级改动，**同长度、无插入**）：

```text
stp x2,x3,[x4,#-0x10]!   (0xA9BF0C82)  →  stp x2,x3,[x4,#0x10]!   (0xA9810C82)
str x3,  [x4,#-0x8]!     (0xF81F8C83)  →  str x3,  [x4,#0x18]!    (0xF8018C83)
补丁点（文件偏移，来自设备上的 aarch64-windows/rpcrt4.dll）：
  0x73e24, 0x7541c, 0x75ea4, 0x76138
注意：同一编码在该 DLL 里还有 2 处属于别的函数，绝不能一起改。
```

### 4.5 真机验证结果（2026-09-12 13:39）

把打过上述字节补丁的 `rpcrt4.dll` 推到设备（`bin/aarch64-windows/` 与
`drive_c/windows/system32/`），再跑 `comprobe.exe`：

**参数基址按预期整体右移 0x20**：

```text
打补丁前: param[0] 0000007E9BA3EC80 ... param[3] 0000007E9BA3EC98
打补丁后: param[0] 0000007E9BA3ECA0 ... param[3] 0000007E9BA3ECB8  (+0x20 ✓)
```

**RPC 完整闭环**（打补丁前死在 CALCSIZE）：

```text
trace:rpc:ndr_client_call SENDRECEIVE
trace:rpc:I_RpcSendReceive
trace:rpc:RPCRT4_ReceiveWithAuth buffer length = 24
trace:rpc:ndr_client_call UNMARSHAL
trace:rpc:client_free_handle Explicit generic binding handle #1
trace:rpc:RpcBindingFree (...) = 0
trace:rpc:NdrpClientCall2 RetVal = 0x0        ← 调用成功返回
```

⇒ **`x4`/shadow-space 这一段已经修对了**，`OpenSCManagerW` 现在能完成到
`services.exe` 的完整 RPC 往返。

### 4.6 剩余问题（独立、已精确刻画，尚未解决）

RPC 返回之后立刻还有一个 `EXCEPTION_ACCESS_VIOLATION`，而且 Wine **无法派发**它：

```text
warn:seh:dispatch_exception EXCEPTION_ACCESS_VIOLATION exception (code=c0000005) raised
err:seh:call_seh_handlers invalid frame 1400033fe (0000007E9B942000-0000007E9BA40000)
err:seh:NtRaiseException Exception frame is not in stack limits => unable to dispatch exception.
```

`0x1400033fe` 是**模块内代码地址**（probe 镜像基址 0x140000000），却被当成 SEH 帧指针 —— 
也就是**帧链走飞了**。它发生在 `NdrpClientCall2` 返回之后，落点应在
「trampoline 收尾 / FEX 的 ARM64EC→x64 返回路径 / 调用方」这一段。

#### 4.6.1 给探针加 VEH 后的精确结果

在 `comprobe.exe` 里装了 `AddVectoredExceptionHandler`，把异常地址/寄存器/栈都打出来：

```text
STEP 9 OpenSCManagerW(L"", L"") - non-NULL strings
  IAT __imp_OpenSCManagerW = 00000001F18A25FF (slot 0000006FFF7514D0)
  OpenSCManagerW address   = 0000006FFF7514D0     ← Wine 生成的 ARM64EC 入口 thunk
  OpenServiceW address     = 0000006FFF7514E0     （相距 0x10）
VEH[0] code=0xC0000005 address=00000001400034F8 rip=00000001400034F8 rsp=0000007E9BA3ECB0 rbp=00000001400034F8
VEH[0]   parameter[0]=0000000000000008 parameter[1]=00000001400034F8    ← 8 = **执行**
VEH[0]   stack[0] = 0000000000000005      ← 正是那次调用的 access mask
VEH[0]   stack[1] = 0000007E9BA3ECF0
VEH[0]   stack[2] = 0000006FFF671534      ← Wine 模块内的返回地址
VEH[0]   stack[3] = 0000007E9BA3ECD0
```

查过 PE 布局：`0x1400034F8` **在探针自己的 `.rdata` 里**（`.text` 是 0x140001000-0x140002ba6，
`.rdata` 是 0x140003000 起）—— 也就是说 **CPU 试图“执行”一个数据地址**
（该地址是代码里当字符串常量用的），并且 `rbp` 也等于它。

#### 4.6.2 已排除：不是 x4 被破坏

做了对照实验：把补丁改成**不破坏 x4** 的等价写法
（`stp x2,x3,[x4,#0x10]` 无回写 + `add x2,x4,#0x10`），其余语义完全相同。

```text
结果：AV 的所有数值逐字节一致（rip / rsp / rbp / stack[0..3] 全同）
⇒ 剩余 AV 与 x4 是否被改写无关。
```

#### 4.6.3 当前的首要怀疑：ARM64EC thunk 的「返回」没有走派发器

对照 clang 为 ARM64EC 变参函数生成的入口 thunk，它是**两头都走派发器**的：

```asm
$ientry_thunk$cdecl$i8$varargs:
    ... 保存 x29/x30 与 q6-q15 ...
    add x4, x4, #32          ; 入口：跳过 shadow space
    mov x5, xzr
    blr x9                   ; 调函数体
    adrp x8, __os_arm64x_dispatch_ret
    ldr  x1, [x8, :lo12:__os_arm64x_dispatch_ret]
    mov  x8, x0
    ... 恢复寄存器 ...
    br   x1                  ; ★ 返回：走 __os_arm64x_dispatch_ret
```

而 Wine 手写的 4 个 thunk 结尾是裸 `ret`：

```asm
    "ldp x29, x30, [sp], #0x20\n\t"
    "ret\n\t"
```

ARM64EC 里 `ret` = `br x30`，**不会经过派发器**。若此刻 x30 里是 x64 的返回地址，
CPU 就会在 ARM64 模式下**把 x64 机器码当 ARM64 指令执行** —— 这与我们观测到的
「执行一个数据地址」的现象高度吻合（x64 字节被误译成 `br <数据地址>`）。

#### 4.6.4 下一轮要做的对照实验（按代价从低到高）

1. **把 4 个 thunk 的 `ret` 换成走 `__os_arm64x_dispatch_ret`**（照 clang 的写法），
   再跑 `comprobe.exe`。这是当前最强的假设。
2. 若仍崩，在 thunk 收尾处 dump `x30` / `sp` / `[sp-8]`，确认返回目标是谁。
3. 再查 FEX 的 `RetToEntryThunk` / `check_target_ec` 与 Wine `signal_arm64ec.c`
   的帧链遍历。

### 4.7 两条被推翻的中间假设（留档）

中途曾怀疑「FEX 的 `x4` 全局传错」，并据此推断要么改 FEX、要么改 Wine。该假设已被
§4.3 的 `varargprobe` 与 §4.4 的 clang 代码生成共同推翻：**`x4` 由环境和编译器约定一致**，
错的只有 Wine 手写 thunk。留档以免后人重复这条路。

另一条被推翻的是「剩余 AV 由 `x4` 被破坏引起」——见 §4.6.2 的对照实验。

补充事实：参考 FEX（`1cc4b93e`，Proton 锁定版本）在 `86ff33bbe..1cc4b93e` 区间内
**只有 3 个 ARM64EC 相关提交**（offline compiler backend / EC map 优化 / 分配 TOP_DOWN），
都不涉及这条路径 ⇒ **换 FEX 版本不会自动修好这个问题**。

## 5. 复现材料清单

| 文件 | 说明 |
| --- | --- |
| `tools/fontprobe/fontprobe.c` | GDI 字号/文本度量探针（本轮排除了「GDI 文本度量本身坏掉」） |
| `tools/fontprobe/comprobe.c` | COM/RPC/服务控制探针，本缺陷的最小复现 |
| `tools/fontprobe/*.exe` | 已交叉编译的 x86-64 PE（llvm-mingw 20260826，**本地构建产物，未入 git**） |
| 设备 `/data/.../temp/wine_stderr_20260912.log` | Wine 子进程 stderr 落盘（**关键工具面**，见 §6） |

## 6. 顺带修正一个此前的错误结论

此前记录「Wine 子进程 stderr 没有可读通道」是**错的**。
`entry/src/main/cpp/proc/wine_child.cpp` 早就把子进程 stderr 经 pipe 转发到
`hilog`（tag `[WineChild-stderr]`）**并且落盘**：

```c
#define WINE_LOG_DIR "/data/storage/el2/base/temp"
... WINE_LOG_DIR "/wine_stderr_%04d%02d%02d.log"
```

设备上今天这个文件已经 **14 MB**，包含每个子进程的 `=== PID=… entryParams=… ===` 分段
和完整的 Wine `err:/warn:` 输出。之前没找到是因为只 grep 了 hilog，没有去读这个文件。
后续所有 Wine 侧调试都应当直接读它。

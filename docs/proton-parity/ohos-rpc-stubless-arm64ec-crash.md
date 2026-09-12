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

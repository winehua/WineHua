# Steam win64 / ARM64EC FEX 启动失败: 两个真实根因与修复

日期：2026-09-18（Asia/Shanghai）
设备：`192.168.180.71:36875`（MLR-AL10 / HarmonyOS 7.0.0.105）
HAP：`F:\WineHua\proton-ohos-worktree\entry\build\default\outputs\default\entry-default-signed.hap`
（本轮最后一份：07:02 构建，payload `46ea83e9…`；含 FEX 修复 + 诊断器修复）

---

## 0. 一句话结论

之前"任何真正的 AMD64 (0x8664) PE 都在进入程序前确定性 SIGSEGV"有**两个独立根因**，
本轮都已修掉并真机验证：

1. **FEX ARM64EC 的 lookup cache 只 Reserve 不 Commit**（`---p`），JIT 第一次读 L1 表项
   必然 fault；Wine-OHOS 不会把该 fault 交回 FEX 的 overcommit handler，于是逃逸成
   host SIGSEGV。WOW64 分支早有对应修复，ARM64EC 分支被 `&& !defined(ARCHITECTURE_arm64ec)`
   排除在外。
2. **WineHua 自己的 early-fault 诊断器会二次 fault**：它把 `sp`（常常正好落在 `---p`
   区间起点，即 FEX guest stack 之上就是 PROT_NONE）当成可读内存直接解引用 96 个字，
   于是 handler 内部再次 SIGSEGV → Wine 报 `nested exception on signal stack` →
   `abort_thread`，把本来可恢复的 guest 异常变成进程死亡。

修完之后：

* **官方 win64 Steam 客户端第一次真正跑起来**：CEF/webhelper 全链路启动，
  `logs/webhelper.txt`、`cef_log.txt`、`steamui_html.txt`、`webhelper_gpu.txt`、
  `steamsysinfo.txt` 都在写，屏幕上**出现了 Steam 自己的窗口**。
* FEX 的 ARM64EC 非对齐原子处理开始工作：`[FEX-UNALIGNED] … code=0x80000002 jit=true
  handled=true`（一次会话 379 次），这是之前从未出现过的。

当前剩余阻塞已经不是"起不来"，而是 Steam 自己报
`There was a problem with your Steam installation. Please reinstall steam.`
（见 `device-evidence-20260918-steam-win64-fex/steam-now.jpeg`），
以及 x64 探针 `LoadLibraryExA(steamclient64.dll)` 会挂住 —— 两者很可能是同一件事。

---

## 1. 修复一：FEX ARM64EC lookup cache 预提交

### 证据链（修复前）

```
[early-fault] sig=11 code=2(SEGV_ACCERR) pc=<FEX JIT> addr=<FEXMem_Lookup_L1 页内>
[fault-map] pc   HIT 6ffc511000-6ffc6fa000 r-xp (匿名 = FEX JIT)
[fault-map] addr HIT 7efefd0000-7efffd0000 ---p [anon:FEXMem_Lookup_L1]
```

* `FEXMem_Lookup_L1` 整段是 `---p`（Reserve 未 Commit）；FEX 的设计是靠 overcommit
  handler 接住首次访问再提交该页。
* Wine-OHOS 侧不会把 CPU DLL/JIT 里的 native 访问异常交回 FEX（WOW64 分支同样如此），
  所以这颗 fault 直接变成 host SIGSEGV，进程在 entry point 之前就死了。
* 同一个 `pc`（低 16 位恒为 `0x69e4`）在 steam.exe、vulkandriverquery64.exe、
  以及一个 25KB 的 x64 探针上都稳定复现 → 与 Steam 无关，是 x64 执行路径本身。

### 修法

把已有的 "预先 Commit + 不 Decommit" 处理从 WOW64 分支扩展到 ARM64EC 分支：

* `scripts/patches/fex-arm64ec-lookup-cache-commit.patch`（新增，已加入 `scripts/build_fex.sh`
  的 patch 序列）
* 改动点：
  * `FEXCore/Source/Interface/Core/LookupCache.cpp`：3 处 `#if defined(_WIN32) &&
    !defined(ARCHITECTURE_arm64ec)` → `#if defined(_WIN32)`
    （构造函数的 `VirtualAlloc(TotalCacheSize, false, true)` = 预提交；
    `ClearL2Cache` / `ClearThreadLocalCaches` 的 `VirtualDontNeed(..., true)`）
  * `FEXCore/Source/Interface/Core/LookupCache.h`：动态 L1 缩容时同样保持 committed

### 验证

修复后同一个 x86_64 探针：

```
[probe] x86_64 probe start
[probe-T0] GetSystemInfo.arch=9(AMD64) GetNativeSystemInfo.arch=12(ARM64)
[probe-T0] HeapAlloc(1MB)=0000000041270040
[probe-T0] VirtualAlloc(1MB,RW)=0000000041380000
[probe-T0] VirtualProtect(RW->RO) ok=1 old=0x4
```

即 x64 进程已经可以正常执行到 Win32 API 层；`[anon:FEXMem_Lookup_L1]` 不再是
`---p`。Steam win64 也从"entry point 之前死亡"推进到"CEF 全链路 + 窗口"。

---

## 2. 修复二：early-fault 诊断器自身二次 fault

### 证据链

* 加了 `[early-fault] logger-exit` 尾标记后，日志里**一次都没出现** → handler 没跑完。
* 上一条永远是 `[fault-map] sp=… HIT <start>-<end> ---p`（sp 正好落在 PROT_NONE 段起点）。
* Wine 紧接着打印：
  `err:virtual:virtual_setup_exception nested exception on signal stack addr 0x7f42c52fdc stack 0x1ec3b0`
  其中 `addr` 落在 **libwine_child.so** 的 r-xp 区（= 诊断器自己的代码），`stack` 在
  signal stack 上 → 就是 handler 内部二次 fault。
* 结果：guest 线程直接消失（`[probe-T1] wait=0 exitCode=0 vehHits=0`），
  Wine 的异常链、以及 ARM64EC → FEX 的 `ResetToConsistentState` 都没机会运行。
  这也是 Steam 之前那颗"非对齐原子 SIGBUS"看起来致命的原因 —— 它本来应该被
  FEX 的 `HandleUnalignedAccess` 吃掉。

### 修法（`entry/src/main/cpp/proc/wine_child.cpp`）

* 栈扫描改成**只经 `/proc/self/mem` + `pread`**（失败只是 EIO，不可能 fault），
  并且要求 sp 所在 VMA 带 `r` 权限：
  `if (OhosVmaBounds(maps, sp, &vma) && vma.perms[0] == 'r')`。
* 加 `[early-fault] logger-exit` 尾标记与 `[early-fault] reentry#N` 重入标记，
  以后能立刻分辨"fault 发生在 handler 里"这种情况。
* 加 `WINEHUA_EARLY_FAULT=0` 关闭开关（注意：目前是在 `Main()` 里注册时读取，
  而 entryParams 的 env 覆盖在那之后才生效，所以这个开关暂时还要挪到运行时读取）。

### 验证

```
[early-fault] pid=59975 … sig=11 code=2 addr=0x41380000 pc=0x7fedfd732c
[early-fault] logger-exit                      <-- handler 跑完了
```

紧接着（Steam 会话）：

```
[FEX-UNALIGNED] pid=812 tid=816 code=0x80000002 jit=true handled=true skip=-4 next_pc=…
```

FEX 的 ARM64EC 非对齐处理真正被调用并 `handled=true`。

---

## 3. 本轮新增的诊断能力（对应专家方案的 P0 任务）

| 任务 | 状态 | 说明 |
| --- | --- | --- |
| P0-X64-1 Process Tree | 部分 | `broker.cpp` 新增 `[PROC-SPAWN] parentHostPid/childHostPid/createStatus/exe` |
| P0-X64-2 Fault Mapping | **完成** | 全量读 `/proc/self/maps`（旧实现 4KB 截断）、pc/lr/sp/addr 的 VMA+权限、AArch64 通用寄存器、fault 处机器码、maps 落盘 `temp/fault-maps-<pid>.txt`、`WINEHUA_FAULT_FREEZE` |
| P0-X64-3 Guest RIP 映射 | 未做 | 现在有了 `FEX-UNALIGNED` 的 `pc/opcode/skip`，但还没有 host PC → guest RIP |
| P0-X64-4 Smoke Ladder | 部分 | x64 探针（T0/T1）已能跑；`C:\smoke\x64\*` 在方案③上仍是 ARM64 PE，真正 AMD64 只有 `smoke/amd64/` 的 cube |
| P0-X64-5 steamclient64 Loader Probe | **完成代码，结果待查** | `smoke/winehua_steamclient64_probe.c`（T0/T1/T2/T3/T4）；实测 T0/T1 通过，**T2 `LoadLibraryExA(steamclient64.dll, DONT_RESOLVE_DLL_REFERENCES)` 会挂住** |

诊断用法（设备侧）：

```
--ps winehua.mode game --ps winehua.game_path 'C%3A%5Csmoke%5Cx64%5C<exe>'
# 可选 env（GameHook 白名单已放行）:
#   WINEHUA_EARLY_FAULT / WINEHUA_FAULT_FREEZE / WINEHUA_WINEDEBUG / WINEDEBUG / WINEHUA_STEAM_WEBHELPER_DIAG
```

---

## 4. 当前剩余阻塞（下一轮目标）

1. **Steam bootstrap 报 "There was a problem with your Steam installation"** ——
   **已定位并修复，见第 7 节**。
2. x64 探针 `LoadLibraryExA(steamclient64.dll, DONT_RESOLVE_DLL_REFERENCES)` 曾经"挂住"，
   实测在 DONT_RESOLVE 只映射这一步只花 ~40ms（不是挂）；真正失败的是完整加载
   `LoadLibraryA(steamclient64.dll)` → `err=998 (ERROR_NOACCESS)`，根因同第 7 节。
3. T1 只读页写测试仍然 `vehHits=0 / exitCode=0`（写只读页没有抛给 guest）。
   * 这与 `dlls/ntdll/unix/virtual.c` 里 `EXCEPTION_WRITE_FAULT` 的
     "ignore fault if page is writable now" 分支有关（Wine 把页放开后重试），
     需要单独确认是不是 OHOS/WineHua 侧的写保护语义问题。
   * 影响面：任何依赖 `PAGE_READONLY`/guard page 的客户端（含 Steam/CEF）都可能受影响。

---

## 7. 根因三：Wine 把 tier0/vstdlib 的导入重定向到 ntdll（Proton 遗留 hack）

### 现象

win64 Steam 客户端能起来、CEF 也在跑，但 **SteamUI.dll 弹**
`There was a problem with your Steam installation. Please reinstall steam.`
（截图 `device-evidence-20260918-steam-win64-fex/steam-now.jpeg`），
`console_log.txt` 不更新，`bootstrap_log.txt` 停在 `Verification complete`。

x64 探针把这条二分到了单个 API：

| 步骤 | 结果 |
| --- | --- |
| `LoadLibraryExA(audio64.dll, DONT_RESOLVE)` | OK |
| `LoadLibraryA(audio64.dll)` | OK |
| `LoadLibraryExA(steamclient64.dll, DONT_RESOLVE)` | OK（26MB 镜像映射 ~40ms） |
| `LoadLibraryA(steamclient64.dll)` | **NULL, err=998 (ERROR_NOACCESS)** |

`WINEDEBUG=+module,+loaddll,+seh` 下（现在是靠 `WINEHUA_WINEDEBUG` 打开，
见第 8 节）关键轨迹：

```
MODULE_InitDLL (0000006FFAA80000 L"steamclient64.dll",PROCESS_ATTACH) - CALL
  ... load_dll api-ms-win-core-fibers-l1-1-2.dll -> kernelbase
MODULE_InitDLL (0000006FFAA80000 L"steamclient64.dll",PROCESS_DETACH) - CALL
warn:module:process_attach Initialization of L"steamclient64.dll" failed
```

以及 298 条：

```
warn:module:import_dll No implementation for ntdll.dll.g_pMemAllocSteam
     imported from ...steamclient64.dll, setting to 00000000630D14F4
warn:module:import_dll No implementation for ntdll.dll.V_UTF8ToUTF32 ...
```

寄存器/SEH 现场（探针进程）：

```
[early-fault] #3 sig=11 code=1(SEGV_MAPERR) addr=0x6ffff5ce70b948
              pc=<FEXMemJIT>  rax=00000000630d14f4  rcx=006ffff5ce70b948
trace:seh:dispatch_exception rip=0000006ffb317ec9 (steamclient64.dll+0x897EC9)
   info[1]=006FFFF5CE70B948  rcx=006ffff5ce70b948
```

反汇编 `steamclient64.dll+0x897EC9`（文件偏移 0x8972C9）:

```
53: mov rax, [rip+...]      ; rax = g_pMemAllocSteam (IAT, 被 Wine 指到 stub 0x630D14F4)
5d: mov rcx, [rax]          ; rcx = 从 stub 内存里读到的垃圾 0x6ffff5ce70b948
60: mov rax, [rcx]          ; <-- AV
65: call [rax+0x18]
```

即：**Steam 的 `g_pMemAllocSteam` 是数据型导入，Wine 却把它指到一个"代码 stub"上**，
解引用 stub 内存自然是垃圾 → DllMain 访问违例 → `steamclient64.dll` 初始化失败
→ 客户端加载失败 → SteamUI 报"安装有问题"。

### 真正的来源

`g_pMemAllocSteam` / `V_UTF8ToUTF32` / `_DMsg` / `WeakRandom*` 等 298 个符号**本来属于**
Valve 的 `tier0_s64.dll` / `vstdlib_s64.dll`（设备上两个文件都在、都是 x86_64、
导出齐全：tier0_s64.dll 543 个导出里就有 `g_pMemAllocSteam`）。

但是 `thirdparty/wine-valve/dlls/ntdll/loader.c` 里有一段 **Proton lsteamclient 的遗留
hack**：

```c
    if (use_lsteamclient())
    {
        if ((!strcmp(name, "tier0_s64.dll") || !strcmp(name, "vstdlib_s64.dll")) &&
            (模块名是 steamclient64.dll 或 gameoverlayrenderer64.dll))
        {
            name = "ntdll.dll";      /* 把导入改成从 ntdll 找 */
        }
    }
```

`use_lsteamclient()` 默认返回 **true**（只看 `PROTON_DISABLE_LSTEAMCLIENT` 环境变量）。
Proton 场景下这是对的：Proton 自带 `lsteamclient.dll`（跳板）并且它的 ntdll 提供这些
导出。但**本树根本没有 `dlls/lsteamclient`**，于是 298 个导入全部落到 Wine ntdll 的
"stub"，其中数据型导入（`g_*`）被当成代码 stub 指针 → 上面的崩溃。

顺带解释了"为什么 32 位一直好使"：这段 hack 只匹配 `steamclient64.dll` /
`gameoverlayrenderer64.dll`，32 位客户端叫 `steamclient.dll`，根本不走这条路径
（32 位日志里 `No implementation for` 计数为 0）—— **64 位要补的就是"按 32 位的走法：
老老实实加载 Valve 自己的 tier0/vstdlib"**。

### 修法

`thirdparty/wine-valve/dlls/ntdll/loader.c`：

1. 把 `static HMODULE lsteamclient = NULL;` 从 `build_module()` 内部提到文件作用域；
2. `import_dll()` 的重定向条件改成 `if (use_lsteamclient() && lsteamclient)` ——
   只有 Proton 的 `lsteamclient.dll` **真的装上跳板**之后才重定向；
   本树没有 lsteamclient，变量恒为 NULL，重定向自然失效，tier0/vstdlib 按正常路径加载。

这样既修好 WineHua（原生 Steam 客户端），又不会破坏将来真要接 Proton lsteamclient 的场景。

### 修复后的实测（2026-09-18 08:26–08:33）

运行时刷新到新 payload（`655d3a5f…`）后，同一个 x64 探针：

```
[T2a] LoadLibraryEx(audio64, DONT_RESOLVE)          = 0000006FFC4B0000 err=0
[T2b] LoadLibrary(audio64, full)                    = 0000006FFC4B0000 err=0
[T2c] LoadLibraryEx(filesystem_stdio, DONT_RESOLVE) = 0000006FFC4B0000 err=0
[T2d] LoadLibraryEx(steamclient64, DONT_RESOLVE)    = 0000006FFAA80000 err=0
[T3 ] LoadLibrary(steamclient64, full)              = 0000006FFAA80000 err=203   <-- 成功 (之前 err=998)
[T4 ] GetProcAddress(CreateInterface)               = 0000006FFB76C790 err=0     <-- 导出可取到
```

Wine loader 轨迹对照（`WINEHUA_WINEDEBUG=+module,+loaddll,+seh`）：

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| `tier0_s64.dll -> ntdll.` 重定向 TRACE | 有 | **0** |
| `No implementation for …` 缺失导入 | **298** | **0** |
| `Loaded …\tier0_s64.dll / vstdlib_s64.dll` | 无（被重定向掉） | 正常加载（native） |
| win64 Steam 客户端 | 弹 `problem with your Steam installation` | **正常启动** |

win64 Steam 客户端实测（08:30–08:33，25+ 个 wine 子进程）：

* `logs/console_log.txt`：`[2026-09-18 08:30:30] Client version: 1788652215` /
  `Loaded SDL version 3.4.0-1359-g70e9cc86d` / `Console Log Start` —— 客户端本体在跑。
* `logs/webhelper.txt`：`Created window: size: 1280,800 … mode: System`、
  `SP Desktop_uid0-'Steam': WasHidden 0` —— CEF 窗口已创建并显示。
* `logs/steamui_html.txt`：`BrowserReady: handle:196610` —— SteamUI 已加载。
* 屏幕：**Steam 启动画面（logo + 转圈）**，不再是安装错误对话框
  （截图 `device-evidence-20260918-steam-win64-fex/after-t3fix-162s.jpeg`）。

剩余：UI 仍停在启动画面（CEF 窗口已 visible，但界面没推进到登录页），
属于下一步要查的"启动画面之后的 UI/登录流程"，不再是加载/安装问题。

停在启动画面的直接线索（`console_log.txt`，win64 会话）：

```
[08:30:30] Client version: 1788652215
[08:30:30] Loaded SDL version 3.4.0-1359-g70e9cc86d
[08:30:30] Failed to init SteamVR because it isn't installed
[08:30:55] Console Log Start
[08:32:01] roaming config store loaded successfully - 165 bytes.
[08:32:03] Error: texture file 'public\steam_cloudsync' does not exist or is invalid
           <-- 之后不再前进; 32 位当年在这条之后还会打印
               "System startup time: 36.77 seconds" / "ExecCommandLine: …"
```

也就是说 win64 客户端卡在"生成/加载 resource 纹理之后、报 startup time 之前"这一段，
同时 `webhelper.txt` 显示 `SP Desktop_uid0-'Steam': WasHidden 0`（窗口已显示）、
启动动画在动（CEF 渲染正常）。下一步应从这一段之间的初始化步骤入手
（resource 打包 (`resources_*`/`public_all` 是否完整解包)、steamservice、
以及 `SteamUI` 与 client 的 IPC 握手）。

---

## 9. 根因四：VGUI2 字体链断裂（32 位当时踩过的同一个坑）

### 现象

win64 `steam.exe` 写出一份 assert dump：

```
Assert( winFont ): C:\buildworker\steam_rel_client_win64\build\src\vgui2\src\surface_gdiwin32.cpp:1336
```

对应 `logs/console_log.txt` 里的
`src\vgui2\src\surface_gdiwin32.cpp (1336) : winFont`
（同一条信息在 **2026-09-17 21:57 的 32 位会话**里也成批出现过 —— 见
`steam-review-20260916.md` §3.3「字体链」）。

### 32 位当时的结论（直接复用）

* Wine 注册的 family 里**没有** `HarmonyOS Sans SC`；`HarmonyOS_Sans_SC.ttf` 的真实 family 名是
  **`鸿蒙黑体`**；`FontSubstitutes`/`Replacements` 若指向不存在的名字，整条 Windows 默认 UI
  字体链就是断的 → VGUI2 拿不到 GDI 字体 → 断言。
* 当时的对照实验：指向 `Noto Sans CJK SC`（`.ttc`）中文能出来但度量不稳；
  **指向 `鸿蒙黑体`（静态 TTF）→ 中文正常、断言消失**（用户实测）。

### 本轮做的修复（prefix 级，可回退）

`F:\WineHua\tmp\dev_fontfix_win64.sh`（设备侧执行）：

1. 停 Wine 会话后把 `HKCU\Software\Wine\Fonts\Replacements` 里
   `"Noto Sans CJK SC"` 全部改成 `"\x9e3f\x8499\x9ed1\x4f53"`（= 鸿蒙黑体，registry 转义形式）；
2. 补回 `HKLM\System\CurrentControlSet\Control\Nls\CodePage` 的 `ACP/OEMCP=936`
   （32 位报告里提到过这段会被 Wine 重写抹掉，Steam 是简体中文应用，多字节文本度量依赖它）；
3. 重新启动 win64 Steam。

备份：`/data/local/tmp/user.reg.bak-fontfix64`。

### 验证（2026-09-18 08:48–08:49）

| 指标 | 修复前（08:30 会话） | 修复后（08:48 会话） |
| --- | --- | --- |
| `winFont` 断言次数 | 有（写 dump） | **0** |
| `console_log` 进度 | 停在 `texture file 'public\steam_cloudsync'` | **继续到 `System startup time: 63.60 seconds`** |
| SteamUI JS | 停在 friendsui 初始化 | **登录完成**：`OnLoginStateChange … 5`、`CloudStorage resuming`、`Updated Steam Version Info`、`OnLoginUsersChanged` |
| CEF 主窗口 | `WasHidden 0` | `WasHidden 0`（1280x800 窗口创建成功） |

> 也就是说：**"卡在启动画面"的直接原因就是 VGUI2 字体链 + 中文代码页**，
> 修完后客户端进到"已登录 + SteamUI 就绪"。

### 仍未完成

1. 这些字体/代码页设置目前只写在**这台设备的 prefix**里（32 位报告 §3.3 也标注了同样问题），
   **产品化需要在 App 每次会话启动时写入**（`entry/` 侧尚无这段逻辑）。
2. 窗口内容：合成器快照显示 Steam 主窗口
   `visible=1 contentSource=SHM shmFrame=1 inputTarget=6 class=SHM`（有 SHM 帧、输入目标正确），
   但屏幕上看到的仍是启动画面/空白 —— 需要下一步确认 SHM 帧内容本身
   （CEF OSR 是否真的画了 library 页）以及 win64 CEF 的 present 路径是否与 32 位一致。

---

## 10. 字体/代码页自愈产品化（本轮落地）

按"避免第一次安装缺功能"的要求，把 §9 的手工 prefix 修改做进了**产品代码**：

* 新函数 `ensure_prefix_fonts_and_codepage()`（`entry/src/main/cpp/proc/wine_child.cpp`），
  在 `Main()` 里 `refresh_wine_session_paths()` 之后、加载 ntdll 之前调用；
* 它做三件事（幂等，只在有改动时写回，首次改动会留 `<reg>.winehua.bak`）：
  1. `HKCU\Software\Wine\Fonts\Replacements`：43 个 UI family（Arial / Calibri / Candara /
     Comic Sans MS / Segoe UI / Motiva Sans / sans / …）→ `\x9e3f\x8499\x9ed1\x4f53`（鸿蒙黑体）；
  2. `HKLM\…\FontSubstitutes`：把指向**不存在** family 的值（`HarmonyOS Sans SC`、
     `Noto Sans CJK*`、`Noto Serif*`、`Noto Sans Mono`）纠正为鸿蒙黑体；
  3. `HKLM\System\CurrentControlSet\Control\Nls\CodePage`：补齐 `ACP=936 / OEMCP=936 /
     MACCP=10008`（Wine 重写注册表时会把这段抹掉，不补的话简体中文文本度量会失败）。

**验证**（新装场景模拟）：先把 prefix 还原成坏状态
（Replacements 指 `Noto Sans CJK SC`、`Nls\CodePage` 段删除），再装新 HAP 启动会话：

```
[WineChild] font Replacements patched / system.reg font/codepage patched
user.reg:    "Arial"="\x9e3f\x8499\x9ed1\x4f53"        (原: "Noto Sans CJK SC")
system.reg:  "Arial"="\x9e3f\x8499\x9ed1\x4f53"        (原: "HarmonyOS Sans SC")
system.reg:  [System\CurrentControlSet\Control\Nls\CodePage]
             "ACP"="936"  "OEMCP"="936"  "MACCP"="10008"
```

即：**新装/重置 prefix 不再缺字体链和中文代码页**。

### 重跑 win64 Steam 的结果（09:12–09:14 会话）

* `console_log`：`System startup time: 83.15 seconds` → `ExecCommandLine: "…Steam.exe"
  -cef-disable-gpu -no-cef-sandbox -cef-force-gpu"`（完整启动序列）；
* `winFont` 断言 **0 次**，无新 dump；
* SteamuUI JS：`SteamApp Init - After Login`、`WaitForServicesInitialized`、
  `DynamicUserStore`（真实 UI 页面在跑）；
* **屏幕**：Steam 主界面出现，**中文标题/菜单/页脚文字全部正常**
  （菜单「Steam 查看 好友 游戏 帮助」、页签「商店 库 社区」、页脚「添加游戏 / 好友与聊天」）；
* 合成器：主窗口 `binding=bound producer=0x… extent=1280x800`
  （此前是 `binding=none producer=0x0`）。

### 仍待处理

1. **CEF 内容区仍是黑的**：UI 骨架（client 自己的 VGUI 绘制）正常，但网页内容区没有画面。
   下一步按 32 位当时的"OSR 内容不到窗口"路线查：
   * 新会话的 `cef_log.txt` / `webhelper_gpu.txt`（`SharedImageManager ... non-existent
     mailbox` 目前只出现在 09-16/09-17 的旧会话里，要看本次会话有没有）；
   * 渲染进程是否出帧、client 如何把 CEF 结果合成进窗口；
   * `-cef-disable-gpu` 的 A/B（本轮跑到了完整启动，但抓图时手机屏幕已灭，需人工确认）。
2. `.winehua.bak` 目前是空文件（用 `O_CREAT|O_EXCL` 占位），下一次构建改成写入原始内容。

---

## 11. 内容区黑屏：CEF 软件开关没生效 + 透明窗口不支持（本轮继续修）

### 11.1 找到"内容区黑"的直接机制

本轮会话（09:19–09:20）的 `cef_log.txt` / `webhelper_gpu.txt`：

```
WARNING:angle_platform_impl.cc(49) GL error: HIGH: Error: 0x00000502 (GL_INVALID_OPERATION)
ERROR:shared_image_manager.cc(223) SharedImageManager::ProduceSkia:
      Trying to Produce a Skia representation from a non-existent mailbox.   (多条)
```

与 32 位文档 §4 完全一致：**ANGLE 的 D3D11 / Vulkan-venus / GL 三条路在本 runtime 都初始化不了**
→ renderer 拿不到 GL → shared image 分配失败 → 窗口内容区没有帧 → 全黑。

### 11.2 关键发现：32 位当时的软件开关"从来没开过"

`thirdparty/wine-valve/dlls/kernelbase/process.c` 里的
`winehua_cef_append_software_switches()` 会给 `steamwebhelper` 追加
` --disable-gpu --disable-gpu-compositing`（Steam 自己不提供这两个真正的 Chromium 开关，
`-cef-disable-gpu` 只给 `--use-gl=disabled`，实测不够），但它有一段门禁：

```c
    if (GetEnvironmentVariableW( L"WINEHUA_CEF_FORCE_SOFTWARE", flag, 2 ) != 1 || flag[0] != '1')
        return NULL;
```

而设备上 **`WINEHUA_CEF_FORCE_SOFTWARE` 在任何地方都没有设置**
（prefix 注册表 0 条、进程 env 0 条）→ 这条 hook 一直没生效 → CEF 一直走 GPU 路径 → 一直黑。

### 11.3 本轮产品化

`entry/src/main/cpp/proc/wine_child.cpp`（entry env 覆盖之后、加载 ntdll 之前）：

```cpp
    if (!getenv("WINEHUA_CEF_FORCE_SOFTWARE"))
    {
        setenv("WINEHUA_CEF_FORCE_SOFTWARE", "1", 1);
        OH_LOG_INFO(LOG_APP, "[WineChild] CEF software rendering forced (default)");
    }
```

并放行 Want 覆盖（`GameHook.ets` 白名单加 `WINEHUA_CEF_FORCE_SOFTWARE`），
需要做 A/B 时传 `=0` 即可。

**验证**：新 HAP 跑起来后 `webhelper.txt` 里 `disable-gpu-compositing` 出现 **18 次**
（之前 0 次）→ 软件开关确实注入了；本次会话 CEF 报错从
"ANGLE GL error + non-existent mailbox" 变成下面 11.4 这一条。

### 11.4 新暴露的一层（与 32 位文档 §4.4 完全对应）

```
[2026-09-18 09:26:58] Browser requested transparent background, but it is not supported   (20 次)
```

即 Steam 的 UI 窗口是**逐像素透明（layered）**窗口；软件路径已经走到，但窗口的
alpha/透明语义在本移植的 present 通道里没有被支持 → 内容仍然合成不出来（黑）。
与此同时：

* `webhelper_js.txt`：`SteamApp Init - After Login total time: 14022ms`、
  `WaitForServicesInitialized: 8922ms` —— renderer 活着、UI JS 跑完了；
* 合成器：三个窗口都是 `contentSource=SHM shmFrame=1 inputTarget=6 class=SHM`
  （有 SHM 帧、输入目标正确），但 UI 内容区仍黑。

**下一步**：让 present/合成通道支持"透明/分层窗口"（现有代码里
`dlls/winewayland.drv/wayland_surface.c` 已有 per-pixel alpha 与预乘处理、
`compositor/toplevel/toplevel_manager.h` 也有 alpha→mask 的补丁），
或者让窗口对应用表现为"不支持透明"从而让 CEF 走不透明路径
（`WS_EX_LAYERED` / `SetLayeredWindowAttributes` / `Dwm*` 系列的支持度）。

---

## 12. 定位到"client 自己画黑帧"：SteamChrome 共享内存流没接上

### 12.1 新诊断：把合成器收到的帧原样 dump 出来

在 `entry/src/main/cpp/compositor/frame/zc_bridge.cpp` 的窗口快照循环里加了
TEMP-DIAG(FRAME-DUMP)：把每个窗口 **合成器实际收到的 SHM 帧**落盘
（`temp/frame-w<W>-h<H>-own<pid>-<surf>.raw`，头行是 `W/H/FMT/TLV/OWNER/PIX`）。
本地用 `F:\WineHua\tmp\raw2png.py` 转 PNG 直接看内容。

**结果（决定性）**：

| 窗口 | 帧内容 |
| --- | --- |
| 1280x20 桌面条 | 有内容（图标条） |
| 400x129 / 217x109 对话框 | 有内容（Steam 对话框、标题栏文字正常） |
| **1280x800 Steam 主窗口** | **全黑（0/800 行非黑）** |

⇒ 合成器/上屏链路是好的（别的窗口都能正确显示），
**主窗口这一帧本身就是黑的**，即 client（x64 steam.exe）自己画了个黑帧。

### 12.2 病根方向：`SteamChrome_MasterStream` 反复重建

对比 `console_log.txt`：

| 会话 | 现象 |
| --- | --- |
| 32 位（2026-09-17 18:51，UI 可用） | `Created mapping SteamChrome_MasterStream_spid516_mem when set to fail if created` **只出现 1 次**，随后 `CAPIJobRequestUserStats`、`ExecuteSteamURL: "steam://open/defaultdialog/maininstance"` 正常推进 |
| win64（09:10–10:24 多次会话） | 同一句 **每 ~10 秒重复一次**（`spid240_mem`），顺序永远是"重建 mapping"→没有后续 UI 事件 |

`SteamChrome_MasterStream` 是 client 与 CEF 之间传递"界面内容"的共享内存流；
每 10 秒重建一次说明**消费端（CEF/browser 侧）始终没接上这个流**，
于是主窗口拿不到内容 → 画黑 → 合成器如实显示黑帧（与 12.1 完全一致）。

### 12.3 下一步（指向很明确）

1. 抓这个命名 mapping 的两侧：谁创建（`NtCreateSection`/`CreateFileMappingW` 的名字与大小）、
   谁按什么名字去 `OpenFileMapping`。名字里带 `spid<N>`：
   `docs/STEAM_WINDOW_IDENTITY_20260917.md` 已经证实本移植**同时存在 Host PID 与 Wine(guest) PID
   两套命名空间**（`hostPid≠winPid`），64 位路径很可能就是这里对不上——两侧算出的
   `spid` 不同，于是永远握不上手。32 位路径能对上，所以只创建一次就成功。
2. 顺带确认 `src\vgui2\vgui_surfacelib\Win32Font.cpp (1129) : Couldn't get string length`
   （当前 UI 仍会打这条）——这是 32 位文档 §3.3 里"即便中文替换已生效仍存在"的那条，
   属于字体度量的残留项，优先级低于 12.3.1。

---

## 13. 按后续计划做的第一轮观测（Phase B：对象时间线）

### 13.1 新增的可关闭观测（不改行为）

`thirdparty/wine-valve/dlls/kernelbase/sync.c` 里加了 TEMP-DIAG(IPC-TRACE)：

* 默认关闭，只有 `WINEHUA_IPC_TRACE=1`（或 `2`）才输出；名字过滤 = 含 `SteamChrome`；
* 覆盖 `CreateFileMappingW` / `OpenFileMappingW` / `CreateEventW` / `OpenEventW` / `SetEvent` /
  `ResetEvent` / `WaitForSingleObjectEx`；
* **严格不污染被测 API 的 LastError**：进入时快照、打印后 `SetLastError(快照)` 还原；
* 句柄类 API（Set/Reset/Wait）只打印本进程记过的句柄（`=2` 时全部打印），避免刷屏；
* Want 白名单已放行 `WINEHUA_IPC_TRACE`。

### 13.2 实测对象时间线（win64 会话，guest pid 624 / 800）

```
pid=800 CreateFileMappingW "SteamChrome_MasterStream_spid624_mem-IPCWrapper"          size=8208      exists=0
pid=624 OpenFileMappingW   "SteamChrome_MasterStream_spid624_mem-IPCWrapper"          access=0x4     handle ok
pid=624 CreateFileMappingW "SteamChrome_MasterStream_spid624_mem-IPCWrapper"          size=8208      exists=1 (183)
pid=624 CreateFileMappingW "SteamChrome_MasterStream_624_27427_mem-IPCWrapper"        size=8208      exists=0
pid=800 CreateFileMappingW "SteamChrome_MasterStream_624_27427_mem-IPCWrapper"        size=8208      exists=1
pid=800 CreateFileMappingW "SteamChrome_ClientStream_624_9747_server_mem-IPCWrapper"  size=26214416  exists=0
pid=800 CreateFileMappingW "SteamChrome_ClientStream_624_9747_client_mem-IPCWrapper"  size=26214416  exists=0
pid=624 CreateFileMappingW "SteamChrome_ClientStream_624_9747_client_mem-IPCWrapper"  size=26214416  exists=1
pid=624 CreateFileMappingW "SteamChrome_ClientStream_624_9747_server_mem-IPCWrapper"  size=26214416  exists=1
```

另外 `SetEvent` / `WaitForSingleObjectEx` 在这些句柄上有稳定推进
（`WAIT_OBJECT_0` 与 `WAIT_TIMEOUT(0x102)` 交替）。

### 13.3 结论：**对象身份/命名空间没有问题，不是 PID 错配**

* 4 个命名对象（2 个 8KB 的 MasterStream + 2 个 25MB 的 ClientStream）都被 **两个进程共同访问**；
* 后访问方拿到 `STATUS_OBJECT_NAME_EXISTS`（Win32 `ERROR_ALREADY_EXISTS=183`）——
  这是 Windows 文档里"CreateFileMapping 命中已存在对象"的**正常语义**，不是失败[W1]；
* 也解释了之前的 `Created mapping SteamChrome_MasterStream_spid240_mem when set to fail if created`
  与 32 位的差异：那句日志本身只是这一套 create-or-open 惯用法的产物，
  **不能据此推断 spid/命名错配**（后续计划 §2.2 的提醒是对的）。

⇒ 按计划 §5.4 判定表，落入"名字与对象域相同" → **进入 Phase C：数据与通知**。

### 13.4 下一轮观测要补的点（已明确）

1. `MapViewOfFileEx`（在 `dlls/kernelbase/memory.c`，本轮未覆盖）：
   谁把 25MB 的 ClientStream 映射成视图、`access`/`offset`/`count` 是多少，
   是否出现 `FILE_MAP_COPY`（私有/COW）这类"看起来共享其实各写一份"的语义（计划 §6.3 第一类分叉）；
2. `CreateEventExW`（真正的实现，`CreateEventW` 只是转发）——本轮没有捕到带 `SteamChrome` 名字的
   event 创建，需要确认事件对象是用什么名字/哪条路径建立的；
3. 在上述两项就位后，做"双端同一对象、同一 offset 的原始字节快照 + hash"对比，
   判断是数据不可见、通知不推进，还是两者都在推进而内容生产侧没产出有效帧。

---

## 14. 统一结论（供专家判断）：IPC 健康，问题在"内容帧 → 窗口"这一段

### 14.1 已闭环并保留的修复（回归项）

| # | 修复 | 证据 | 状态 |
| --- | --- | --- | --- |
| 1 | FEX ARM64EC lookup cache 预提交 | x64 探针可执行到 Win32 API；L1 不再是 `---p` | 保留 |
| 2 | early-fault 诊断器不再自伤 | 出现 `logger-exit`；FEX 非对齐进入 `handled=true` | 保留（仍建议后续收敛） |
| 3 | tier0/vstdlib 导入重定向条件化 | 缺失导入 298 → 0；`LoadLibrary(steamclient64)` 成功 | 保留 |
| 4 | 字体链 + 936 代码页（已产品化自愈） | `winFont` 断言 0；`System startup time` 出现；中文菜单正常 | 保留 |
| 5 | CEF 软件开关（`WINEHUA_CEF_FORCE_SOFTWARE` 默认开） | `--disable-gpu --disable-gpu-compositing` 实际注入 | 保留（基线） |
| 6 | env 生效顺序（WINEDEBUG/early-fault） | 日志可见最终值 | 保留 |

### 14.2 SteamChrome IPC：**对象与事件层是健康的**（不是 PID/命名错配）

用新加的可关闭观测 `WINEHUA_IPC_TRACE`（kernelbase：Create/Open FileMapping、Create/Open Event、
Set/Reset/Wait，打印前后还原 LastError）在 win64 会话实测：

```
pid=800 CreateFileMappingW "SteamChrome_MasterStream_spid624_mem-IPCWrapper"         size=8208      exists=0
pid=624 OpenFileMappingW   "SteamChrome_MasterStream_spid624_mem-IPCWrapper"         access=0x4     成功
pid=624 CreateFileMappingW "SteamChrome_MasterStream_spid624_mem-IPCWrapper"         size=8208      exists=1(183)
pid=624 CreateFileMappingW "SteamChrome_MasterStream_624_27427_mem-IPCWrapper"       size=8208      exists=0
pid=800 CreateFileMappingW "SteamChrome_MasterStream_624_27427_mem-IPCWrapper"       size=8208      exists=1(183)
pid=800 CreateFileMappingW "SteamChrome_ClientStream_624_9747_server_mem-IPCWrapper" size=26214416  exists=0
pid=800 CreateFileMappingW "SteamChrome_ClientStream_624_9747_client_mem-IPCWrapper" size=26214416  exists=0
pid=624 CreateFileMappingW "SteamChrome_ClientStream_624_9747_client_mem-IPCWrapper" size=26214416  exists=1(183)
pid=624 CreateFileMappingW "SteamChrome_ClientStream_624_9747_server_mem-IPCWrapper" size=26214416  exists=1(183)

两条进程各自 CreateEventExW 同一批 9 个事件名 (每流 *_avail / *_written, 以及
MasterStream_Event_spid<N>), 同样互为 exists=1; SetEvent / WaitForSingleObjectEx 在这些句柄上
持续推进 (WAIT_OBJECT_0 与 WAIT_TIMEOUT 交替)。
```

**结论**：命名对象与事件对象**两侧共享同一个对象域**，`exists=1 / ERROR_ALREADY_EXISTS` 是
"CreateFileMapping 打开已存在对象"的正常语义；`SteamChrome` IPC **不是** PID/命名错配，
也不存在"建一次失败所以每 10 秒重建"的证据（那句 `Created mapping ... when set to fail if created`
就是这套 create-or-open 惯用法的产物）。

### 14.3 内容帧：两条路径、同一处黑

用 FRAME-DUMP（把合成器实际收到的 SHM 帧落盘 + 周期性抓最新帧）与合成器窗口快照对照：

| 会话形态 | 主窗口 (1280x800) | 结果 |
| --- | --- | --- |
| 未强制软件（客户端走 native producer） | `binding=bound producer=0x…19 extent=1280x800` | 屏幕可见 **UI 骨架 + 中文菜单/页签/页脚**，**内容区黑** |
| 强制软件（当前基线，SHM 路径） | `binding=none producer=0x0 contentSource=SHM shmFrame=1` | 主窗口 SHM 帧 **全黑 (0/800 行非黑)**；其它窗口（对话框 400x129、217x109、1280x20 桌面条）帧内容都正常 |

补充事实（有效负控）：**同一台设备、同一 HAP、同一会话里**，小窗口的帧是有内容的，
所以"合成器/上屏坏掉"被排除；黑只出现在 Steam 的网页内容区。

### 14.4 与 32 位经验对应的那条线：窗口/表面绑定（wayland id 匹配）

`docs/STEAM_WINDOW_IDENTITY_20260917.md` 记录的 32 位结论是：

* Steam 的 UI 窗口 surface **不是**经 `wayland_surface_create()` 建的，而走 client surface 路径，
  身份靠 token / `wlSurfaceId` 传播；
* 32 位的修法在 `entry/src/main/cpp/compositor/frame/zc_bridge.cpp` 的
  `windowBindings_` / `presentBindings_`：key = `(clientPid << 32) | protocolId`，
  按尺寸 + claim 逻辑把 **present(producer) surface 绑到 window(owner) surface**。

当前 win64 稳态实测：**`binding=bound` 计数 = 0**（所有 Steam 窗口 `binding=none`），
即 64 位会话里 **present 侧绑定根本没建立**，只能退回 SHM 路径；而 SHM 路径下 client 画的是黑。
这与"32 位靠绑定把 CEF 内容接上窗口"的路径正好对应，是下一步最该查的点。

### 14.5 建议的下一步（给专家定夺）

1. **绑定路径探针**：在 `zc_bridge.cpp` 的 bind 逻辑里记录（可关闭、过滤 Steam）
   * present 侧候选：producer pid / surface id / 帧尺寸 / 绑定代次；
   * window 侧候选：clientPid / protocolId / 窗口尺寸；
   * **拒绝原因**（尺寸不符 / 已被 claim / generation 不符）——把现有 `bindingRejectedLogged_` 扩展成带原因；
   目的：回答"win64 webhelper 是否创建了 present surface、为什么没绑上"。
2. **producer 侧帧链**：对绑定成功的窗口，记录 Venus/zero-copy 侧最后一帧的时间与尺寸
   （现有 `presentBindings_` 已有 `lastProducerUs`/`frameWidth/Height`，补一条日志即可）。
3. 在拿到 1/2 的结论后，再决定是修"present→window 绑定"（首选，和 32 位一致）
   还是去动 CEF 的 alpha/透明语义——**在绑定问题明确之前不要动 compositor/alpha**。
4. 复核项（计划 §9）：T1 页保护探针仍 `vehHits=0`，需要用独立进程 + 私有页 + 证明写指令真的执行；
   以及 `DONT_RESOLVE` 与 full-load 拆到不同进程，`err=203` 不再作为失败判据。

---

## 15. 绑定探针（按专家判据实现）与 Native 模式实测结论

### 15.1 探针实现（只观察，不改绑定规则）

`entry/src/main/cpp/compositor/frame/zc_bridge.cpp` 新增（`WINEHUA_...` 无关，随 compositor 常开但去重有界）：

* `BIND-PRODUCER`：producer 到达（key / hostPid / surfaceId / extent / frames / bound）；
* `BIND-WINDOW`：每个候选窗口的**四套尺寸** —— `rawSurface`（committed 内容尺寸）、
  `viewport`（vpDst 目标尺寸）、`outerWindow`（toplevel 合成尺寸 / subsurface 自身尺寸）、
  `contentRect`（xdg_surface.window_geometry 原值 + offset），角色/可见/最小化/claimedBy；
* `BIND-REJECT`：每条 producer×window 的拒绝原因（`DEGENERATE_SIZE` / `NO_WINDOW_ROLE` /
  `DESKTOP_ROOT` / `NOT_VISIBLE` / `MINIMIZED` / `ALREADY_CLAIMED` / `OUTER_SIZE_MISMATCH` /
  `MULTIPLE_CANDIDATES` / `NO_CANDIDATE`），并带 `contentRectWouldMatch` 只诊断标记；
* `BIND-PRODUCER-STATE`：周期性输出每个 producer 的 extent / frames / ageMs / binding /
  lastReject / contentRectWouldMatch（帧链与绑定状态一并可见）；
* 所有 BIND 行同时写 hilog 和 **落盘** `temp/bind-diag-<pid>.log`（hilog 环形缓冲会滚掉历史）。

### 15.2 Native 模式实测（12:21–12:26，WINEHUA_CEF_FORCE_SOFTWARE=0）

Producer（全部来自同一个 webhelper hostPid=55571）：

| producer | extent | frames | binding | lastReject | contentRectWouldMatch |
| --- | --- | --- | --- | --- | --- |
| 0xd91300000019 | **1280x800** | **33,448**（ageMs 1–7，持续出帧） | **bound** | bound:unique-geometry | 1 |
| 0xd9130000001c | 706x800 | 7 | bound | bound:unique-geometry | 1 |
| 0xd91300000028 | 300x650 | 4 | bound | bound:unique-geometry | 1 |
| 0xd91300000003 | 700x440 | **18,414** | **none** | NO_WINDOW_ROLE(contentRectWouldMatch) | 1 |
| 0xd91300000022 | 690x714 | 5 | none | none | 0 |
| 0xd91300000016 / 25 / 2e | 1x1 | 2–14 | none | NO_WINDOW_ROLE / none | 0 |
| 0xd9d90000002a | 110x32 | 1 | none | OUTER_SIZE_MISMATCH | 0 |

窗口候选（BIND-WINDOW 抽样；四套尺寸全部相等，本会话**没有出现** 1280x779 / 1278x657 这类外框≠内容区）：

```
toplevel=6 clientPid=55539 rawSurface=1280x800 viewport=1280x800 outerWindow=1280x800 contentRect=(0,0 1280x800) claimedBy=0xd91300000019
toplevel=7 clientPid=55539 rawSurface=706x800  viewport=706x800  outerWindow=706x800  contentRect=(287,0 706x800)  claimedBy=0xd9130000001c
toplevel=8 clientPid=55539 rawSurface=300x650  viewport=300x650  outerWindow=300x650  contentRect=(500,85 300x650) claimedBy=0xd91300000028
toplevel=5 clientPid=55539 rawSurface=700x440  ... claimedBy=0xd91300000003（随后 binding 被释放 → 该 producer 状态回到 none）
```

合成器侧（hilog）：主窗口所在 toplevel 的窗口快照显示
`binding=bound producer=0xd91300000019 extent=1280x800 contentSource=VenusNative`，
对应 producer 的 `ageMs` 只有 1–7ms（**持续出帧**）。

屏幕：窗口外框（深蓝边）可见、**内容区纯黑**（截图 `native-bind-300s.jpeg`）。

### 15.3 结论（对应专家判定表）

> **Producer 已绑定 + 帧持续增长（33k 帧、ageMs≈1–7ms），但画面内容仍黑
> → 停止改 Binding，转 CEF frame/content。**

* 专家的"1280x779 内容 producer 被尺寸规则拒绝"这一假设**在本轮没有出现**：
  主窗口 producer 的 extent 就是 1280x800，且 `contentRectWouldMatch=1`；
  绑定走的也是 `unique-geometry`（正常）。
* 新增一条值得追的支线：**未绑定的 700x440 producer（18,414 帧、活跃）**
  与那次 11:28 会话里出现的 `SteamBrowser-'data:text/' 1278x657 @(1,92)` 子窗口，
  提示"CEF 网页内容"可能跑在另一个窗口/producer 上；需要确认它对应哪个 Wayland 窗口，
  以及为什么它（或它的父窗口）没有被当作内容候选。
* 下一步（按专家给的方向）：查 **CEF renderer/content path** —— 即 CEF 是否真的把页面画进了
  已绑定的 producer（VenusNative 层），以及该层在合成时是否被正确消费；
  在此之前不再修改绑定规则。

---

## 16. 状态跟踪（12:35）：不是黑帧，是 CEF 网络服务进程起不来 → UI 停在启动画面

### 16.1 现场快照

| 项 | 观测 |
| --- | --- |
| 进程 | 25 个 wine 子进程存活；Steam 会话 12:21 启动 |
| 客户端 | `console_log`：`System startup time: 103.20s` → `ExecCommandLine: … -no-cef-sandbox -cef-force-gpu` |
| 登录 | `connection_log`：`ConnectionCompleted(...WebSocket)` → `RecvMsgClientLogOnResponse() 'OK'` → `Logged On` |
| 客户端自己的 HTTPS | `bootstrap_log`：`Manifest download: finished` → `Download skipped by HTTP 304 Not Modified`（**winhttp 路径正常**） |
| 合成器 | 主窗口 `binding=bound producer=0xd91300000019 extent=1280x800 contentSource=VenusNative producerAgeMs=1-2 class=OK`（producer 已出 94,190 帧） |
| 屏幕 | Steam 启动画面 + 转圈（与用户描述一致） |

### 16.2 真正卡住的地方：CEF 的 network / storage 服务进程崩溃重启循环

UI JS 的最后进展与错误：

```
SteamApp Init - After Login total time: 34270ms
WaitForServicesInitialized: 31130ms
WARNING: Timed out waiting for initial server clock drift adjustment
WARNING: FriendStore Initialization - Still no friends list from server. Waiting.
INFO: Error calling GetBestEventsForCurrentUser Generic: AxiosError: Network Error
INFO: Will retry GetBestEventsForCurrentUser in 10000ms
WARNING: CDynamicStore.InternalLoad Unknown Error: AxiosError: Network Error
WARNING: LoadLanguages caught [object Object] retry in 30 seconds
"Slow network is detected."
```

`webhelper.txt` 里的子进程启动计数（**同一台设备累计**）：

```
1384  --utility-sub-type=storage.mojom.StorageService
 389  --utility-sub-type=network.mojom.NetworkService
  81  --utility-sub-type=chrome.mojom.…
   4  --utility-sub-type=audio.mojom.…
```

最近一次 network 服务命令行（注意 sandbox 已经关掉）：

```
steamwebhelper.exe --type=utility --utility-sub-type=network.mojom.NetworkService --lang=zh-CN
  --service-sandbox-type=none --no-sandbox --start-stack-profiler --enable-chrome-runtime
  --user-data-dir="C:\users\steamuser\AppData\Local\Steam\htmlcache" …
```

⇒ **CEF 的 network/storage utility 进程起来了就立刻死**（没有 steamwebhelper 的 crash dump，
属于早期退出），于是页面的所有 HTTP 请求都失败 → SteamUI 的数据 store 空转 → 界面停在启动画面。

### 16.3 结论更新

* 「内容区黑」的完整解释是：**不是合成/绑定问题，而是 CEF 拿不到任何网络数据，UI 停在启动页**；
  启动页本身（logo + 转圈）是 CEF 正常渲染的，所以帧数据看起来"黑"。
* 与客户端自身 winhttp 对比（同样在 x64 guest 里）：**Wine 的 winhttp/wininet/crypt32 路径是好的**，
  失败只出现在 **Chromium/CEF 自己的 network service 子进程**里。
* 下一步诊断（按优先级）：
  1. 给 `steamwebhelper` 追加 `--enable-logging --v=1`（复用已有 kernelbase hook，env 门禁），
     拿 Chromium 自己关于 network service 退出/崩溃的日志；
  2. 同机对照 32 位客户端（`cef.win7`）：看它的 network/storage utility 是否也这样重启
     （若 32 位正常 → 问题定位在 win64 CEF + ARM64EC/FEX 这条组合上）；
  3. 必要时用最小 x64 探针复现"utility 进程早退"（例如检查 FEX 对 CEF 子进程用到的
     特定指令/API 的处理），再决定是 FEX 侧还是 Wine API 侧补。

---

## 8. 顺带修掉的两个 env 生效顺序问题

`Main()` 里 `select_winedebug_profile()` 在 `apply_entry_param_env_overrides()` **之前**
执行，导致 Want 传进来的 `WINEHUA_WINEDEBUG` / `WINEDEBUG` 永远被 `-all` 覆盖；
同理 `WINEHUA_EARLY_FAULT=0` 也读早了。现在：

* entry env 应用后会再选一次 WINEDEBUG 并打印 `final WINEDEBUG=… (after entry env)`；
* `g_early_fault_enabled` 在 env 应用后重新判定，handler 运行时读该标志。

于是现在可以这样开 Wine 通道（GameHook 白名单已放行）：

```
--ps winehua.d3d_env_count 1 \
--ps winehua.d3d_env_key0 WINEHUA_WINEDEBUG --ps winehua.d3d_env_value0 +loaddll,+module,+seh
```

---

## 5. 复现步骤（当前设备状态可直接跑）

```bash
HDC="/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"

# 1) 装 HAP（含 FEX 修复 + 诊断器修复）
"$HDC" file send entry/build/default/outputs/default/entry-default-signed.hap /data/local/tmp/winehua.hap
"$HDC" shell "bm install -p /data/local/tmp/winehua.hap -r"

# 2) 冷启一次让运行时刷新（payload 变化时才会重解压）
"$HDC" shell "aa force-stop app.hackeris.winehua; aa start -b app.hackeris.winehua -a EntryAbility"

# 3) 启动 win64 Steam
"$HDC" shell "aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode game \
  --ps winehua.game_path 'C%3A%5CProgram%20Files%20%28x86%29%5CSteam%5CSteam.exe' \
  --ps winehua.container_id default"
```

关键证据文件：

| 内容 | 位置 |
| --- | --- |
| 设备 stderr（含 `early-fault` / `FEX-UNALIGNED`） | `…/temp/wine_stderr_YYYYMMDD.log`（hdc 可直接读） |
| fault 现场整张 maps | `…/temp/fault-maps-<pid>.txt` |
| Steam 日志 | `…/drive_c/Program Files (x86)/Steam/logs/` |
| 本轮证据快照 | `F:\WineHua\device-evidence-20260918-steam-win64-fex\` |

---

## 6. 本轮代码改动清单

| 文件 | 改动 |
| --- | --- |
| `scripts/patches/fex-arm64ec-lookup-cache-commit.patch` | 新增：ARM64EC lookup cache 预提交 |
| `scripts/build_fex.sh` | patch 序列加入上面这条 |
| `entry/src/main/cpp/proc/wine_child.cpp` | 诊断器重写：全量 maps、寄存器、机器码、maps 落盘、VMA 判定；栈扫描改 pread（不再自伤）；`logger-exit`/`reentry#` 标记；`WINEHUA_EARLY_FAULT=0` 开关 |
| `entry/src/main/cpp/proc/broker.cpp` | `[PROC-SPAWN]` 进程树打印 |
| `entry/src/main/ets/game/GameHook.ets` | env 白名单放行 `WINEHUA_FAULT_FREEZE` / `WINEHUA_FAULT_DUMP_MAPS` / `WINEHUA_EARLY_FAULT` / `WINEHUA_WINEDEBUG` / `WINEHUA_STEAM_WEBHELPER_DIAG` |
| `smoke/winehua_steamclient64_probe.c` | x64 分层探针（T0/T1/T2/T3/T4） |

> 注：`build/fex-src` 是 staged 源码，patch 序列幂等；重新 stage 时会自动按顺序应用
> （WOW64 两条 → 新的 ARM64EC 一条）。

---

## 17. 修正 §16：CEF utility 没有"重启循环"，问题是内容帧本身

§16 把 `webhelper.txt` 里的 `StorageService 1384 次 / NetworkService 389 次` 读成了
"CEF utility 子进程早退重启循环"，并据此把根因定成 *win64 CEF Utility Process early-exit loop*。
本轮按专家给的"观测包"（Chromium verbose + utility 全生命周期）实测后，**这条结论撤回**。

### 17.1 本轮新增的观测能力（产品化，默认开启）

| 位置 | 能力 |
| --- | --- |
| `entry/src/main/cpp/proc/cef_utility_probe.{h,cpp}` | CEF/Steam 子进程生命周期观测：创建侧（broker）解析 entryParams 认出 `steamwebhelper.exe`（browser / gpu-process / renderer / `--utility-sub-type=…` / `cef.win64|cef.win7`）与 `steamservice.exe`，用 `SO_PEERCRED` 拿到**真正的父进程 hostPid**；退出侧在 NCP 退出回调（`HandleProcessDeath`）里用 pid 配对，算出 `lifetimeMs` 与 `signal`。落盘 `temp/cef-utility-diag.log`（>4MB 轮转 `.1`）+ hilog。 |
| `thirdparty/wine-valve/dlls/kernelbase/process.c` | `WINEHUA_CEF_FORCE_SOFTWARE=1` 追加 `--disable-gpu --disable-gpu-compositing`；`WINEHUA_CEF_LOGGING=1` 追加 `--enable-logging=file --log-file=C:\windows\temp\cef-verbose.log --v=1`（两条门禁独立叠加） |
| `entry/src/main/ets/game/GameHook.ets` | env 白名单放行 `WINEHUA_CEF_LOGGING` / `WINEHUA_CEF_UTILITY_PROBE` |

日志行格式（可直接喂脚本统计）：

```
CEF-UTILITY-CREATE brokerPid= childHostPid= parentHostPid= key= type= subtype= cefDir= sandbox= HODLL= HODLL64= mojo= t= image="…"
CEF-UTILITY-EXIT   childHostPid= parentHostPid= key= type= subtype= cefDir= lifetimeMs= signal=… createMs= exitMs=
CEF-UTILITY-COUNT  key= spawned= exited= t=
```

配套分析脚本 `F:\WineHua\tmp\cef_util_report.py`：输出 cefDir × key 的启动次数、
存活时长分位（p10/p50/p90）、退出信号分布。

### 17.2 实测结论（win64，Native 模式，2026-09-18 12:49 会话）

进程树（`parentHostPid` 来自 SO_PEERCRED，不是 broker 自己的 pid）：

```
steam.exe(wine child 13930)
└─ steamwebhelper.exe browser 14929
   ├─ gpu-process              15570
   ├─ network.mojom.NetworkService 16373
   ├─ storage.mojom.StorageService 16529
   ├─ renderer                 17777 / 22381 / 22673
   ├─ chrome.mojom.ProcessorMetrics 18848
   └─ chrome.mojom.UtilWin     21674
```

观测 7 分钟：**9 次创建 / 2 次退出**。退出的是两个"一次性"utility：
`ProcessorMetrics`（lifetime 3000ms）与 `UtilWin`（1754ms），两者 NCP 回调都报 `signal=11`。
`browser` / `gpu-process` / `NetworkService` / `StorageService` / `renderer` **全部存活到最后**。

`webhelper.txt` 的累计计数按天拆开后（`grep 'utility-sub-type=storage' | sed 's/^\[\([0-9-]*\).*/\1/' | uniq -c`）：

| 日期 | storage 启动次数 |
| --- | --- |
| 09-13 | 180 |
| 09-14 | 163 |
| 09-15 | 227 |
| **09-16** | **748（16:32–17:24 每分钟约 50 次，这才是真正的循环）** |
| 09-17 | 30 |
| 09-18 | 37（每个会话 NetworkService/StorageService 各 1 次） |

⇒ 1384/389 是**跨天累计**，且被 09-16 的调试窗口主导；09-18 的 win64 客户端里
NetworkService/StorageService **只启动一次并长期存活**。所以：

* §16 的 "early-exit loop" 判定不成立；
* "CEF 网络能力不可用" 也不能再靠"网络服务反复重启"来解释 —— 网络服务是活的。

### 17.3 同一轮里发现的两个真实异常（记录下来，尚未定性）

1. **`steamservice.exe` 反复拉起**：broker 侧 `[PROC-SPAWN]` 在 2.5 分钟内为
   `C:\Program Files (x86)\Common Files\Steam\steamservice.exe /RunAsService` 发起 **27 次**创建，
   子进程侧只留下 `=== PID=… entryParams=… |steamservice.exe|/RunAsService|…` 头行、没有任何输出，
   NCP 回调报 `signal/reason=53`。它不影响黑屏判定，但属于"客户端服务起不来"的真问题。
2. **落盘帧全黑**：`temp/frame-w1280-h800-own14929-32.raw`、`frame-w706-h800-own14929-28.raw`
   转 PNG 后 `non-black rows 0/800`。但这一轮 CEF 内容走的是 **VenusNative 纹理**（native 模式），
   SHM 只是空壳，**不能据此说"CEF 给的就是黑帧"** —— 需要在软件模式（内容走 SHM）复测才有判定力。

### 17.4 屏幕实况与 UI 进度（12:52 截图）

VGUI 外框（菜单栏 `Steam 查看 好友 游戏 帮助`、页签 `商店 库 社区 …`、底栏 `添加游戏` / `好友与聊天`）
正常绘制，**内容区全黑**。同时 SteamUI 的 JS 已经跑起来：

```
SteamUI: VERBOSE: SteamApp Init - After Login total time: 19694.65 ms
SteamUI: VERBOSE: WaitForServicesInitialized: 17251.53 ms
… SP Desktop_uid0-'Steam': INFO: https://steamloopback.host/chunk~2dcc5aaf7.js…: Slow network is detected
… MessageDisplay-'store.stea': INFO: https://store.cdn.…/main.js…: Suspense
```

即：**渲染进程活着、JS 在跑、chunk 能下载**，但网页内容不上屏。问题范围因此收紧到
"CEF 合成出来的内容 → 窗口" 这一段，而不是 utility 子进程生死。

### 17.5 三个必须写进流程的坑

* **改 wine 源码必须重建运行时 payload**：`scripts/build_wine.sh` → `scripts/w1-m2-assemble.sh`
  → `scripts/w1-m3-build-hap.sh`。只跑 `w1-m3` 只会重打 HAP，`wine-data.zip` 里还是旧 DLL
  （本轮第一次跑就踩了：`strings -el …/kernelbase.dll | grep enable-logging` 为空 → 注入无声失效）。
  好消息是增量重建很快（wine ≈ 20s、assemble ≈ 45s、HAP ≈ 10s）。
* **`--enable-logging=stderr` 会弹控制台窗口**：Chromium 在无控制台的 GUI 进程里分配一个
  conhost 控制台，把日志画在 Wine 桌面上（截图可见 `…steamwebhelper.exe` 控制台 + 满屏
  `network_change_notifier_win.cc(268)] WSALookupServiceBegin failed with: 8` 之类），既污染界面
  又抢焦点。产品路径改用文件模式 `--enable-logging=file --log-file=C:\windows\temp\cef-verbose.log`。
* **屏幕 2 分钟无操作会休眠**：休眠期间 `snapshot_display` 抓到的是全黑图（不是应用画面），
  观测脚本必须 `power-shell wakeup` 保活，否则证据全丢。

---

## 18. 观测包实测矩阵：结论又一次被翻转（这次是配置相关）

§17 只用 Native 模式（`WINEHUA_CEF_FORCE_SOFTWARE=0`）跑了一轮就下结论说"没有重启循环"。
按产品默认配置（强制软件渲染）复测后，**循环是真的**，但它只在"加了 CEF 开关"的配置下出现。

### 18.1 四种配置的对照（同一 HAP、同一设备、同一账号）

| # | 配置 | CEF 子进程 创建/退出 | 渲染进程结局 | 屏幕实况 |
| --- | --- | --- | --- | --- |
| run2 | `FORCE_SOFTWARE=0`（native，无注入开关） | 9 / 2 | renderer ×3 **全部存活**（1 分钟观察窗） | VGUI 外框（菜单栏/页签/底栏）可见，**内容区全黑** |
| run4 | `=1`（`--disable-gpu --disable-gpu-compositing`，产品默认） | 34 / 30 | renderer ×3 **全崩**（6.7s / 8.0s / 44.9s，2×SIGSEGV + 1×exit0） | 桌面几乎空白（主窗口 `visible=0`），只有 1280×20 底栏条可见 |
| run6 | 同上（复测） | 32 / 26 | renderer ×3 中崩 1（44.4s，SIGSEGV） | 同上 |
| run7 | `=compositing`（只加 `--disable-gpu-compositing`） | 43 / 38 | renderer ×8 **崩 7**（6.0/6.4/7.9/10.7/16.2/112.9/161.7s） | 登录窗出现但**整体全黑**（连 VGUI 外框也没有） |

稳定出现的还有 **`steamservice.exe` 重启循环**：run4/6/7 里分别 25 / 24 / 29 次，
每次寿命 155~229ms（p50 179ms），NCP 回调统一报 `signal=53`，子进程侧除了 Wine 启动 trace
什么也没留下（连失败信息都没有）。Native 模式（run2）下同样在循环（hilog 里 1.5s 一次）。

### 18.2 修正后的结论

1. **"CEF utility 重启循环"看着像什么，取决于配置**：native（不加开关）下
   NetworkService / StorageService / GPU / renderer 全部长寿（9 创建 / 2 退出）；
   强制软件（产品默认）下 renderer 会在观测窗口里反复重启（见 run4/6/7），
   但 **run8（同样的软件开关、只是不加 CEF 日志）里 renderer 3 创建 / 0 退出** ——
   说明 renderer 崩溃主要是**日志注入带来的副作用**（见 18.6），不是软件开关本身。
2. **唯一稳定复现、且与配置无关的循环是 `steamservice.exe`**（native 与软件模式都在循环，
   155~229ms 就死、`signal=53`）。它就 §16 想找的那个"反复早退的子进程"，
   只不过角色是 Steam 客户端服务而不是 NetworkService/StorageService。
3. **两种配置各坏一处**：native → 进程稳、VGUI 外框可见、**内容区全黑**；
   强制软件（产品默认）→ 进程稳、登录窗**整体全黑**（连 VGUI 外框都没有，run8 截图）。
   **当前没有一种配置能出画面**，但两者的黑不是同一个原因。
4. Steam 自己的 `-cef-force-gpu` 与我们注入的 `--disable-gpu*` **互相打架**，
   很可能就是"半 GPU 状态"的来源 —— 下一步应把 `-cef-force-gpu` 的注入也做成条件项。

### 18.6 run8：把日志注入的副作用分离出来（关键校准）

| | run4/run6/run7（带 `WINEHUA_CEF_LOGGING=1`） | run8（同软件开关，`LOGGING=0`） |
| --- | --- | --- |
| CEF 日志开关 | `--enable-logging=file --log-file=C:\windows\temp\cef-verbose.log`（覆盖了 Steam 自己的 `--enable-logging=handle`），并且**必然弹一个 conhost 控制台窗口** | 保留 Steam 自己的 `--enable-logging=handle --log-file=<fd>`，无控制台 |
| renderer | 3 创建 / 3 退出；8 创建 / 7 退出（run7） | **3 创建 / 0 退出** |
| steamservice | 25 / 24 / 29 次循环 | 24 次循环（同样） |
| 屏幕 | 控制台窗口 + 黑块 | 居中黑窗（任务栏 `登录 Steam`），无外框 |

⇒ 结论：**评估 CEF 进程稳定性时必须关掉 `WINEHUA_CEF_LOGGING`**（日志注入会自己制造
renderer 崩溃），日志只用于"读 CEF 内部报错"。这条已经写进观测脚本约定。

另外，run8 里 26 条 `sig=11 code=2` 的 SMC 现场显示：guest 侧访问的地址落在
`6fbb642000-6fbb680000 rwxp`（匿名 RWX，FEX 代码缓存区）或 `0x40890f90` 这类小地址，
且 `x27(guest rip)` 出现 `0xa4 / 0xe0 / 0x48c` 这类"跳飞了"的值 —— 但这些 fault 都被
Wine SEH 接住了（进程没死）。要判定它们是否与黑屏相关，还需要把"被 SEH 接住的 fault"
和"CEF 自己报告的错误"对齐，不是本轮的目标。

### 18.3 Chromium verbose 日志（本轮打通，文件模式）

`WINEHUA_CEF_LOGGING=1` → `--enable-logging=file --log-file=C:\windows\temp\cef-verbose.log --v=1`
（`C:\windows\temp` 即 `<prefix>/drive_c/windows/temp/cef-verbose.log`）。实测内容：

```
WARNING:chrome_main_delegate.cc(748)] This is Chrome version 126.0.6478.183
ERROR:network_change_notifier_win.cc(268)] WSALookupServiceBegin failed with: 8
WARNING:angle_platform_impl.cc(49)] vk_renderer.cpp:4121 (rx::vk::Renderer::initFeatures): Unknown GPU architecture
ERROR:gl_display.cc(497)] EGL Driver message (Error) eglCreateContext: Requested GLES version (3.0) is greater than max supported (2, 0).
INFO:CONSOLE(2)] "FriendsUI ReadyToRender - Clock drift - ERROR undefined", source: https://steamloopback.host/library.js
INFO:CONSOLE(0)] "Uncaught (in promise) #<Object>", source: https://steamloopback.host/index.html?...&ARCH=x64
```

两条值得盯的：

* `eglCreateContext: Requested GLES version (3.0) is greater than max supported (2, 0)` ——
  guest 侧 EGL/GLES 只报 **2.0**，而 CEF/ANGLE 要 3.0。native 模式下"内容区全黑"很可能就是
  这条：合成器拿不到可用的 GL 上下文 → 交上来的就是黑帧。修 guest GL advert（virpipe/vtest
  下的 GLES 版本）比继续调 binding 更对症。
* `Uncaught (in promise)` + `Clock drift - ERROR undefined` —— SteamUI 初始化在等
  "服务器时钟漂移校准"，正好对上 JS 侧 `Timed out waiting for initial server clock drift adjustment`。

### 18.4 新的故障归属能力（产品化诊断，已进 wine 运行时）

* `dlls/ntdll/unix/ohos_virtual.c`：SIGSEGV 时按需读一次 `/proc/self/maps`，给
  `addr / x27(guest rip) / pc` 各打一行 `[SMC-MAP]`（每进程上限 24 组）。只对 SIGSEGV 打 ——
  每个 x64 进程启动阶段都有一串 **SIGBUS(7)** 探针 fault，会把名额吃光（第一版实测踩到）。
  样例：

  ```
  [SMC-MAP] addr=0x4127e4c9 41270000-41460000 ---p 00000000 00:00 0
  [SMC-MAP] x27=0x4413f0   00440000-0045b000 rw-p 00000000 00:00 0
  [SMC-MAP] pc=0x7eee0054b4 7eedfd0000-7eeefd0000 rwxp [anon:FEXMemJIT]
  ```

  → 一眼能分辨"guest 代码经 FEX JIT 访问了 PROT_NONE 页"这类结构性故障。
* `entry/src/main/cpp/proc/wine_child.cpp`：early-fault 诊断器从"只打前 4 颗"改成
  **按 (sig, pc) 去重、上限 12 种** —— 原来那 4 个名额被每个进程启动时的 4 颗同形态
  SIGBUS 探针 fault 吃光，真正致命的 SIGSEGV 现场反而全丢。

### 18.5 两个坑（已写进流程）

* **`--enable-logging` 一定会弹一个控制台窗口**（stderr 模式、file 模式都弹，
  Steam 自己的 `--enable-logging=handle` 不弹）。这个 conhost 窗口会盖在 Wine 桌面上抢焦点，
  **对"窗口是否上屏"的观测有污染** —— 评估窗口/合成行为时应关掉 CEF 日志（`WINEHUA_CEF_LOGGING=0`）。
* **32 位对照跑不起来（本轮受阻）**：把 `steam.exe.old`（i386，4.7MB）覆盖回 `steam.exe` 后，
  客户端 30 秒内就自更新回 win64（`bootstrap_log: Downloaded new manifest /steam_client_win64 …
  正在展开安装包`）。要阻断自更新需要 `steam.cfg`（`BootStrapperInhibitAll=enable`），
  而 Steam 目录**在设备侧不可新建文件**（`hdc file send` / `touch` 都是 permission denied，
  SELinux 限制；只有覆盖已存在文件可以）。可行路径：① 在 Wine 内创建 steam.cfg；
  ② 用独立 prefix/副本跑 32 位客户端；③ 用移动介质目录（`/storage/...`）里的 Steam 副本。

---

## 19. P0-GL 主线落地：三层能力探测 + Steam 参数整理

按 `WineHua_P0_GL_VirGL_GLES3_Plan_20260918` 执行（原则：**修真实能力链，不伪造 GLES3 版本号**）。

### 19.1 新增的探测能力（都产品化，默认开启、只读）

| 层 | 落点 | 输出 |
| --- | --- | --- |
| P0-GL-1 Host App EGL | `entry/src/main/cpp/graphics/gl_capability_probe.{h,cpp}`（首个窗口 Init 时后台执行一次） | `GL-CAP host layer=app …`：EGL vendor/version/client_apis/extensions + ES2/ES3.0/ES3.1/ES3.2 逐档探测 + GL vendor/renderer/version/GLSL |
| P0-GL-2 VirGL Host EGL | `thirdparty/virglrenderer/src/vrend/vrend_winsys_egl.c` | `GL-CAP virgl layer=host-egl …`：egl_init、client_apis、选中的 renderable bits、每档 context 创建结果、以及 create 上下文真实 gl_version |
| P0-GL-3 VirGL capset | `thirdparty/virglrenderer/src/vrend/vrend_renderer.c` | `GL-CAP virgl layer=capset …`：max_version / gl_ver / gles_ver / glsl_level / use_gles / capset2 |
| P0-GL-5 Windows WGL | `smoke/winehua_opengl_smoke.c`（x64=原生 ARM64 PE、amd64=真 x86_64 PE 走 FEX、x86=WOW64） | `GL-CAP guest layer=wgl …`：GL0 建窗/像素格式、GL1 legacy context + GL 字符串 + 扩展、GL2 `wglCreateContextAttribsARB` 逐档 core、GL3 glClear+glReadPixels、GL4 FBO 完整性 |

三层共用同一个日志文件 `temp/gl-capability.log`（hilog 环形缓冲会滚掉历史，文件才是权威），
配套证据目录 `F:\WineHua\device-evidence-20260918-steam-win64-fex\p0-gl\`。

### 19.2 实测能力矩阵（2026-09-18 15:00，Run A clean）

| 层 | 结果 |
| --- | --- |
| Host App EGL（OHOS 系统 EGL / Maleoon 910） | EGL 1.4 OpenHarmony，`client_apis=OpenGL_ES`；ES2 / ES3.0 / ES3.1 / **ES3.2 全部 PASS**；`OpenGL ES 3.2 B312`、`HUAWEI`、`Maleoon 910`、`GLSL ES 3.20`；`surfaceless_context=1` |
| VirGL Host EGL（virglrenderer） | `egl_init=1.4 gles=1`；版本试探 4.6→3.3 全部 FAIL（`EGL_BAD_MATCH 0x3009`），**3.2 OK**；active `OpenGL ES 3.2 B312`、`epoxy_gl_version=32`、`use_gles=1` |
| VirGL capset（暴露给 guest 的能力） | `set=2 max_version=2 gl_ver=0 **gles_ver=32 glsl_level=430 use_gles=1 capset2=1**` → capset 明确宣告 **GLES 3.2 / GLSL 4.30** |
| Windows WGL guest（amd64 走 FEX） | `GL_VERSION=2.1 Mesa 25.0.1`、`GL_RENDERER=virgl (Maleoon 910)`、`GLSL=1.20`；`wglCreateContextAttribsARB` 要 3.0/3.2/3.3/4.3/4.6 **全部 FAIL**；GL3 清屏回读 `64,128,191,255` PASS；GL4 FBO `0x8CD5` complete |
| Windows WGL guest（x64 原生 ARM64 PE） | **与上面逐字一致** |

### 19.3 结论（比计划 §11 的分支更精确）

1. **Host 与 VirGL host 都已经是 GLES 3.2** → 不是驱动瓶颈，**不需要伪造版本号**（计划 §22 的红线）。
2. **capset 也正确地把 GLES 3.2 / GLSL 4.30 传下去了** → 不是计划里的"情况 A"。
3. **天花板出现在 guest 侧**：Windows GL 只拿到 **GL 2.1 / GLSL 1.20**，且所有 ≥3.0 的 core context 都建不出来。
   这正好解释 CEF 的 `eglCreateContext: Requested GLES version (3.0) is greater than max supported (2, 0)`。
4. **arm64 原生与 amd64(FEX) 结果完全一致** → 与 FEX/ARM64EC 无关；也解释了"32 位能显示"不是因为它绕开了 GLES3，
   而是两边其实共享同一条被压到 GL2.1 的 guest GL 栈（计划 §29 的推断被实测证实）。
   → 对应计划 §11 **情况 B**：修 `guest Mesa virpipe / guest EGL / Wine WGL`，而不是继续改 Steam。

### 19.4 Steam 参数与运行环境整理（计划 §18/§19）

* `-cef-force-gpu` 不再无条件注入，改成 `WINEHUA_CEF_FORCE_GPU=1` 才加（Run B）；
  默认 **Run A clean**：不注入任何 CEF backend 参数，让 CEF 自己选。
* `--disable-gpu*` 仍是 `WINEHUA_CEF_FORCE_SOFTWARE=1|both|compositing|gpu` 显式开关（Run C 兼容回退），
  不再作为产品默认 —— 实测它会造成"窗口整体黑 + renderer 反复 SIGSEGV"。
* 新增 **前台屏幕常亮**（`entry/src/main/ets/service/ScreenAwake.ets` + `EntryAbility` 前后台挂钩）：
  熄屏会自动锁屏，而锁屏时 `aa start` 被系统拒绝（10106102，表现为"命令成功但什么都没起"），
  自动化回归与 Steam 会话都会被它打断；前台保持常亮同时符合游戏会话的使用预期。

### 19.5 P0-GL 下一步（都是便宜且可直接判定的动作）

### 19.6 执行结果（2026-09-18 20:30）：ES3 天花板已修，暴露出的却是"栈越界"

**① UBO +1 修复（已落地、已验能力）**

`thirdparty/virglrenderer/src/vrend/vrend_renderer.c`：host 原本写
`caps->v1.max_uniform_blocks = GL_MAX_VERTEX_UNIFORM_BLOCKS + 1 - 1;`（+1 又减掉），
而 guest Mesa 的 `st_init_limits` 会 `pc->MaxUniformBlocks -= 1; /* 第一个留给普通 uniform */`，
于是 host 报 12 → guest 得 11 → 撞上 Mesa 的 ES3 门禁 `if (pc->MaxUniformBlocks < 12) can_ubo = false;`
→ `ARB_uniform_buffer_object` 关掉 → `ver_3_0` 失败 → **整条 guest GL 掉到 ES2.0**
（实测：`renderable_counts gl=84 es1=84 es2=84 es3=0`，ES3 context 报 `EGL_BAD_CONFIG`，
CEF/ANGLE 由此报 `Requested GLES version (3.0) is greater than max supported (2, 0)`）。

改成 `max + 1` 后：guest 侧 `max_uniform_blocks=13 → st 减 1 = 12`，门禁通过，实测
`guest-egl es3.0 create_ctx OK` / `es3.1 create_ctx OK`。
保留 A/B 开关 `WINEHUA_VIRGL_UBO_FIX=0`（回到上游行为）。

**② 32 位路径的参考性（历史证据）**

32 位能用时的 webhelper GPU 进程命令行是 `--use-gl=disabled`（GL 关闭、走非 GL 路径），
而且它是在**同一个 ES2 天花板**下出画面的。把这条复刻到 win64（Steam 原生 `-cef-disable-gpu`，
由 `WINEHUA_CEF_STEAM_DISABLE_GPU=1` 注入）后 **browser 进程仍然 41/41 崩** ——
说明"选哪个 GPU 后端"不是 win64 的瓶颈，32 位只是没踩到 x64 这条初始化路径。

**③ GL proc dispatch 探针（新增 `WINEHUA_GL_PROC_TRACE=1`）**

`dlls/win32u/winehua_gl_proc_trace.{c,h}`：挂在 Wine WGL/EGL 唯一的解析漏斗
`egldrv_get_proc_address()`（PE 侧 `wglGetProcAddress` → `UNIX_CALL(wglGetProcAddress)`
→ `p_get_proc_address`），每进程保留最近 64 条 ring，`(pid,name,source)` 去重打印
`GL-PROC:`，NULL 时立刻 dump `GL-PROC-HISTORY:`；early-fault handler 也会通过
`dlsym(RTLD_DEFAULT,"winehua_gl_proc_trace_dump")` 把 ring 一起打出来
（win32u.so 以 RTLD_GLOBAL 加载）。

实测（ES3 可用 + 不注入任何后端开关，browser 进程）：
**893 条解析、507 个不同入口、NULL = 0**。→ 排除"某个 GL/EGL 入口解析成 NULL"。

**④ 崩溃真身：线程栈越界（不是 NULL 调用）**

新增 `[SMC-NEIGHBOR]`（打印 fault 地址所在 VMA 的前后各 3 条）后，现场变成：

```
[SMC] enter tid=39081 sig=11 code=2 addr=0x221f88 pc_in=0x6fe9f50d08 x27_in=0x0
      x0_in=0x40d2c440 lr=0x6fe9f813d8 sp=0x221f60 x16=0xd63f0200
[SMC-NEIGHBOR]     00100000-00211000 rw-p            [anon:stack:…]
[SMC-NEIGHBOR] >>> 00220000-00222000 ---p            ← sp/addr 都在这里
[SMC-NEIGHBOR]     00222000-00320000 rw-p            ← 栈体
```

`sp=0x221f60`、`addr=0x221f88`（= sp+0x28）都落在 **PROT_NONE 的 guard 页**里，上方就是 rw 栈体，
`x16=0xd63f0200` 是 `blr x16`（ARM64EC thunk 调用）→ **线程栈被打爆**，
`x27_in=0` 只是 thunk 里未设置的 guest-RIP（不是"跳到 NULL"）。

⇒ 结论：GLES3 打通后 CEF 走进更深的 GL/GPU 初始化路径，把该线程的 Windows 侧栈用爆了。
下一步：量化该线程 `[anon:stack:<tid>]` 的区间与用量，然后二选一——
(a) 放大 Win32 线程栈（Wine `CreateThread`/`RtlCreateUserThread` 路径）；(b) 放大 OHOS 原生线程栈
(`pthread_attr_setstacksize`)。修完再跑 Steam 看 browser 是否稳定、内容是否出画面。

### 19.7 栈修复落地 + 实测（2026-09-18 20:50）：CEF 不再崩了

改在 `thirdparty/wine-valve/dlls/ntdll/unix/thread.c`（`init_thread_stack`），两条栈都只放大 *reserve*：

| 项 | 上游值 | 现在的默认 | env 开关 |
| --- | --- | --- | --- |
| ARM64EC 模拟器栈（CHPE v2，FEX 跑 x64 用） | `0x40000` (256 KB) | **2 MB** | `WINEHUA_EMU_STACK_MB` |
| Win32 线程栈 reserve（仅 arm64ec 进程） | 镜像默认（~1 MB） | **8 MB 下限** | `WINEHUA_MIN_THREAD_STACK_MB` |

启动时会打一行 `[thread] WineHua stack policy: reserve=0x800000 commit=0 min=0x800000`。

实测（`WINEHUA_CEF_UTILITY_PROBE=1`，不注入任何 CEF 后端开关，ES3 可用）：

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| CEF 子进程 | browser 41 创建 / 41 退出（lifetime 3.8–5.8s，signal=11） | **browser / gpu-process / NetworkService / StorageService 各 1 次创建、0 退出** |
| `webhelper.txt` | 反复 `Startup - webhelper launched` | `Starting message loop` → `CreateBrowser … type:12 … 0x0` → 稳定 |
| 桌面 | 空桌面 | Wine 桌面 + 任务栏；Steam 主窗口已建立但 `visible=0`（另一 toplevel `minimized=1`） |

⇒ "GLES3 可用即崩"这一层被修掉了。剩下的是**窗口可见性/合成**这一段（Steam 主窗口存在但没上屏，
且主 UI browser 这次只建了一个 0x0 的），与 §15 的绑定结论合流。

### 19.8 栈修复后的新停点：renderer 从未启动（2026-09-18 21:10）

同一配置（ES3 可用 + 栈修复，不注入任何 CEF 后端开关）下对照两轮 `webhelper.txt`：

```
可用会话 (12:49, ES2 天花板 + 旧栈):
  12:49:25 Starting message loop
  12:49:52 CreateBrowser … type:12 … 0x0
  12:49:52 UB-65536: SetName: SP Shared JS Context     ← 有
  12:49:52 launching … --type=renderer                 ← 有
  12:50:29 CreatingPopup SP DesktopLoginWindow_uid0 700x440
  12:50:59 CreatingPopup SP Desktop_uid0 1280x800      ← 主 UI

本会话 (21:06, ES3 可用 + 栈修复):
  21:06:47 Starting message loop
  21:06:53 CreateBrowser … type:12 … 0x0
  21:06:53 UB-65536: AfterCreated handle:65536 type:12: (0,0) 0x0
  （此后 10+ 分钟没有任何新行: 没有 SetName / 没有 renderer / 没有 popup）
```

进程状态：browser/gpu-process/NetworkService/StorageService 全部存活、CPU 空闲（S 状态、非忙等），
`cef-verbose.log` 只有 DNS/DHCP 周期噪声，无 GL 错误、无崩溃 —— **浏览器进程在等，renderer 从未被拉起**。

结论：崩溃问题已消除，当前停点前移到"共享 JS context browser 之后、renderer 之前"，
最可能是 browser 在等 GPU channel / GPU 进程的就绪信号。下一步用 **`WINEHUA_CEF_FORCE_SOFTWARE=1`**
（现在它不再与 `-cef-force-gpu` 冲突，因为后者已改为可选）做隔离：若 UI 能起来 → 阻塞在 GPU 进程；
若仍卡在同一位置 → 与 GPU 无关，转查 browser 的 renderer 创建条件。

### 19.9 卡点定位：browser 主线程卡在 futex（2026-09-19）

沿"32 位历史对照"这条线又清掉两个环境缺陷：

1. **SDL2**（`gldriverquery.exe`/`vulkandriverquery.exe` 是 i386 且仍链接 SDL2.dll）：
   HAP 随包官方 x86 SDL2（libsdl.org 2.30.9 win32，`machine=0x14c`），App 在会话就绪前补到
   `Steam/bin/SDL2.dll`（Valve 自带优先）→ `0xC0000135 (DLL_NOT_FOUND)` 消失。
2. **相对路径启动逃过 FEX 选择**：这两个工具是用 `.\bin\gldriverquery.exe` 拉起的，路径里没有
   "steam" 字样 → 没命中按路径兜底 → 落到 box64（09-16 记录里这些工具在 box64 上立刻 SIGSEGV）。
   现在"相对路径 + `WINEHUA_WORKING_DIRECTORY` 在 Steam 树下 ⇒ Steam 客户端进程" → 走 FEX，
   日志可见 `HODLL=libwow64fex.dll (steam-client override, exe=.\bin\vulkandriverquery.exe)`。
   （切到 FEX 后这两个 i386 工具仍 exit=11，属于它们自身的崩溃，另记。）

日志通道按专家意见改回：注入只加 `--v=1`，保留 Steam 自己的 `--enable-logging=handle`；
另加 `WINEHUA_CEF_VMODULE` 支持 —— 但实测这个 CEF release 构建把
`render_process_host_impl` 等 VLOG 裁掉了（开关确实进了命令行，日志零命中），该路不可用。

**新的卡点证据（本轮最关键）**：

* SIGPROF 采样器的装载点原本挂在从不被执行的 `dlopen_dll()` 上（实测 dlopen_dll trace 0 命中、
  `load_builtin_unixlib` 15 万次）→ 挪到 `load_builtin_unixlib()` 后 `[prof] armed` 出现。
  但它只在**烧 CPU** 时触发，对"空闲等待"型卡点零输出。
* 因此新增 **空闲卡死看门狗**（`WINEHUA_STALL_DUMP=<秒窗口>`，app 侧）：窗口内 CPU 占用率低于阈值
  就把每个线程的 `comm/wchan` 落到 wine_stderr。实测抓到：

```
[stall-dump] pid=42962  (steamwebhelper browser, 113 线程)
[stall-dump] tid=… comm=CrBrowserMain  wchan=hm_futex_wait_interruptible
[stall-dump] tid=… comm=Chrome_IOThread/CompositorTileW/ThreadPool*/CacheThread_…  全部 futex 等待
[stall-dump] pid=43036  (gpu-process) CrGpuMain / dxvk-submit|queue|cs / VizCompositorTh → futex / EVENTPOLL
```

⇒ **browser 进程没有崩、也没有自旋，所有线程（含 `CrBrowserMain`）都停在 futex 等待上**，
与"`CreateBrowser` 成功、`AfterCreated` 返回、此后不再推进、renderer 从未被请求"完全一致
（`--type=renderer` 的进程创建请求 = 0，已在进程创建层确认）。

`/proc/self/task/<tid>/stack`（内核栈）在本设备上不可读，所以只有 wchan 没有调用栈；
下一步要做的是**用户态栈采样**：对卡住进程的每个线程 `tgkill(SIGPROF)`，在 handler 里记录
`pc/lr/fp` 并用已有模块表解析 —— 这样就能看到主线程到底等在哪个锁/API 上。

1. **区分天花板在 Wine WGL 还是 guest Mesa**：用 `WINEHUA_WINEDEBUG=+wgl,+opengl`
   跑同一个 `winehua_opengl_smoke.exe`，看 Wine 向底层 driver 请求的 context 版本与返回。
2. **核对 guest GL 库选择**：`WINEHUA_EGL_LIBRARY_PATH`（系统 `libEGL.so`，其 `client_apis` 只有 `OpenGL_ES`）
   与 `GALLIUM_DRIVER=virpipe` + `MESA_LOADER_DRIVER_OVERRIDE=swrast` 的组合是否让 Mesa 走了只报 GL2.1 的路径
   （renderer 串仍显示 `virgl (Maleoon 910)`，必须靠 `+wgl/+egl` 日志确认）。
3. 计划 P0-GL-6（WineD3D D3D9 → OpenGL）留待 guest GL 修好后再当门禁。
  正在展开安装包`）。要阻断自更新需要 `steam.cfg`（`BootStrapperInhibitAll=enable`），
  而 Steam 目录**在设备侧不可新建文件**（`hdc file send` / `touch` 都是 permission denied，
  SELinux 限制；只有覆盖已存在文件可以）。可行路径：① 在 Wine 内创建 steam.cfg；
  ② 用独立 prefix/副本跑 32 位客户端；③ 用移动介质目录（`/storage/...`）里的 Steam 副本。

---

## 20. 2026-09-19 第二轮：卡点定到 host 侧（缺 BrowserReady），并排除一批假设

### 20.1 决定性证据：host 日志缺 `BrowserReady`

`Steam/logs/steamui_html.txt`（**host 侧**）对比：

```text
可用会话 (09-18 14:52:50 → 14:55，最后一个能出画面的会话)
  14:52:50 Client version: 1788652215
  14:52:50 Started webhelper process 620
  14:52:50 CreateBrowser id:1646944249 type:12 flags:1000000
  14:53:20 CreateResponse: id:1646944249 handle:65536
  14:53:20 BrowserReady: handle:65536          ← 同一秒
  14:53:30 GetDesiredSteamUIWindows: starting processing of 1 windows
  14:53:31 PopupHTMLWindow: idx:131073 → BrowserReady:131073        (登录窗)
  14:54:48 CreateMainWindow: creating shared JS context
  14:54:52 PopupHTMLWindow: idx:196610 handle:65536                 (主窗口 1280x800)
  …friendslist / PopupWindow / contextmenu ×12

现在 (09-19 每一轮，含 09:58 / 10:12 / 10:16 / 10:27 / 10:34 / 10:38 / 11:01 / 11:10)
  CreateBrowser id:… type:12 flags:1000000
  CreateResponse: id:… handle:65536
  （此后 10+ 分钟无任何 host 侧新行，**没有 BrowserReady**）
```

browser 侧（`webhelper.txt`）两边前 7 行**完全一致**，差别只在 AfterCreated 之后：

```text
可用: CreateBrowser … → UB-65536: AfterCreated … → UB-65536: SetName: SP Shared JS Context
      → Browser - launching child process … --type=renderer → CreatingPopup …
现在: CreateBrowser … → UB-65536: AfterCreated … → (没有 SetName / 没有 renderer / 没有 popup)
```

⇒ 卡点 = **client(steam.exe) 在 CreateResponse 之后等 webhelper 的"就绪"通知，通知永远没到**；
`--type=renderer` 的进程创建请求因此为 0（CEF 探针 `key=renderer` 计数 0）。

### 20.2 本轮被排除的假设（都有 A/B 证据）

| 假设 | 做法 | 结果 |
| --- | --- | --- |
| 启动参数（`-cef-force-gpu`）变了 | 用 `winehua.game_arg_enc0=%2Dcef-force-gpu` + `enc1=%2Dno-cef-sandbox` 复刻 12:49 配置 | browser 命令行**确实**出现 `--ignore-gpu-blocklist`（与 12:49 那次一致）→ **仍然卡住** |
| 关 GPU（产品默认软件路径） | `WINEHUA_CEF_FORCE_SOFTWARE=1`（`--disable-gpu --disable-gpu-compositing`） | 同样卡在 AfterCreated，cef=4、renderer 请求 0 |
| 线程栈放大引入回归 | 新加 `WINEHUA_MIN_THREAD_STACK_MB=0` / `WINEHUA_EMU_STACK_MB=0`（= 上游行为） | 日志确认 `reserve=0 commit=0 min=0`（真的关掉了）→ **仍然卡住**；栈修复可以保留 |
| App 侧 Host-GL 探测（14:28 新增、默认开） | `WINEHUA_GL_PROBE=0` | 仍然卡住 |
| CEF profile/缓存损坏 | 容器内 `ren htmlcache htmlcache.bak0919`（现在是全新 profile） | 仍然卡住 |
| 我们注入的 env 造成（零 env 基线） | 一个 d3d/CEF env 都不传 | 仍然卡住 |

**顺带修掉一个真实缺口**：`WINEHUA_MIN_THREAD_STACK_MB` / `WINEHUA_EMU_STACK_MB` 不在
`GameHook.ets` 的 env 白名单里 → 之前那次"栈回退"实验其实没生效（白名单已补，另补
`WINEHUA_WAIT_TRACE`）。

### 20.3 本轮新增的两个诊断能力（已进产物）

1. **容器内执行任意 Windows 程序**：App 的 `winehua.mode game` 只要给
   `game_path=C:\windows\system32\cmd.exe` + `game_arg_enc1=<URL编码命令>` 就能在同一个
   prefix/wineserver 里跑命令，输出重定向到 `C:\` 再从设备侧读（SELinux 只挡住"设备侧写"，
   挡不住"Wine 内写"）。用它做了上面那条 htmlcache 改名实验，并跑过 `winedbg`。
   - `winedbg --help` / `winedbg --file` 可用，`info process` 能列进程树；
   - 但 **`attach` 会挂住**（线程挂起不返回），所以拿不到反汇编级栈。
2. **`WINEHUA_WAIT_TRACE=1`**（`dlls/ntdll/unix/sync.c`）：只记录"无超时"的
   `NtWaitForSingleObject`，输出 `[wait-trace] pid/tid/handle`（每进程 64 条上限）。
   实测一轮 201 条 —— 其中 **Steam.exe 主线程**不断做无限等待（handle 0x40 / 0x7c），
   与"等 webhelper 就绪"吻合。
3. **guest(x64) 上下文采样**（`thread.c: ohos_prof_dump_guest_context` + app 侧 SIGPROF
   链式调用）：读 `TEB->ChpeV2CpuAreaInfo->ContextAmd64` 与 WoW64 CPU area，输出
   `[prof-guest] src=… rip=… rsp=…` + 栈上返回地址。实测 637 条**全是 `src=none`**——
   说明本 runtime 里 x64 guest 由 `libarm64ecfex` 仿真，其上下文既不在 CHPE v2 CPU area
   也不在 WoW64 CPU area，而在 **FEX 自己的 CPUState** 里（下一步要么按 FEX 结构体偏移读，
   要么在 x64→unix 转换点抓）。

### 20.4 下一轮的两条主线（按性价比排序）

1. **在 x64→unix 转换点抓 guest RIP**：`NtWaitForSingleObject` 的 unix 实现里已经能拿到
   pid/tid/handle，只缺"调用者"。可行做法：
   (a) 读 FEX 的 CPUState（`libarm64ecfex` 的结构体布局，`RIP`/`RSP` 在固定偏移）；
   (b) 或在 ARM64EC thunk 入口 / `__wine_unix_call` 入口处记录 guest 上下文再传到 unix 侧。
   拿到调用者后就能区分"client 内部逻辑没发"与"IPC 没送到"。
2. **直接给 Steam 的 HTML IPC 打点**（`WINEHUA_IPC_TRACE`，已产品化）：这次要盯的是
   **通知方向**——webhelper 是否往共享内存流写了 ready 记录、是否 SetEvent、client 的
   `HTMLController Commands` 线程（`steam.exe` 里那个线程）是否被唤醒。

时间线提醒：**最后一次成功是 09-18 14:55**（之后 15:00 起一直失败），而这段时间里 wine 侧
只有 `opengl_diag.c`(16:57)、`gl_proc_trace`(20:20)、`opengl.c`(20:21)、`ohos_virtual.c`(20:30)、
`vulkan.c`(21:00)、`process.c`(22:37)、`wayland_surface.c`(04:00)、`virtual.c`(08:35) 改过；
但 §19.7 又记录 20:50 那轮**是能走到"主窗口已建立"的**，说明这条链路还带**非确定性**——
下轮务必把"同一配置连跑 3 次"作为判定条件。

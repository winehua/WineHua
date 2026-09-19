# Windows 架构语义表（P0-2B，2026-09-17）

> 依据《WineHua_Steam_x64_CEF_Architecture_Semantics_Plan_20260917.md》§3–§10
> 探针：`tools/arch-probe/arch_probe.c`（构建脚本 `scripts/build_arch_probe.sh`，产物 `artifacts/arch-probe/`）
> 设备：HarmonyOS arm64 平板 ｜ prefix：`app.hackeris.winehua` 桌面容器
> 原始数据：`F:\WineHua\device-evidence-20260916-zcfix1\arch-probe-x86.json`（x64 见 §4 阻塞说明）

---

## 1. 结论摘要

1. **x86 侧语义总体是"WOW64 on 64-bit Windows"的样子**：`IsWow64Process=TRUE`、
   `IsWow64Process2{ProcessMachine=I386, NativeMachine=ARM64}`、
   `PROCESSOR_ARCHITEW6432=ARM64`、`ProgramW6432=C:\Program Files`、`syswow64` 目录存在。
2. 但存在**一组互相矛盾的值**（下节标红项）：`GetNativeSystemInfo` 报 **AMD64(9)**，
   而 `IsWow64Process2.NativeMachine` 与 `PROCESSOR_ARCHITEW6432` 都报 **ARM64(12/ARM64)**；
   注册表 `Session Manager\Environment\PROCESSOR_ARCHITECTURE` 又是 **ARM64**。
3. 注册表里 **`ProgramFilesDir` / `ProgramFilesDir (x86)` / `ProgramW6432` 缺失**
   （只有环境变量有值）——Steam/安装器常见判据。
4. **x64 执行路径当前不可用**：x86/x64 探针与**原生 smoke x64 exe** 全部在
   `libarm64ecfex` 初始化后立即 SIGSEGV（详见 §4）。这是 P0-2 的前置阻塞。

---

## 2. x86 probe 原始值（`arch-probe-x86.json`）

| API / 语义 | x86 当前值 | 目标语义 | 备注 |
| --- | --- | --- | --- |
| PE Machine | `I386` (332) | I386 | ✅ |
| pointer bits | 32 | 32 | ✅ |
| `GetSystemInfo.wProcessorArchitecture` | `INTEL/x86` (0) | INTEL/x86 | ✅（WOW64 进程返回 x86 是正常的） |
| **`GetNativeSystemInfo.wProcessorArchitecture`** | **`AMD64` (9)** | **ARM64 (12)** | ⚠️ 与 `IsWow64Process2`/环境变量矛盾 |
| `GetSystemInfo` cores / page | 12 / 4096 | — | ✅ |
| `IsWow64Process` | `TRUE` | TRUE | ✅ |
| **`IsWow64Process2.ProcessMachine`** | `I386` (332) | I386 | ✅ |
| **`IsWow64Process2.NativeMachine`** | **`ARM64`** (43620) | ARM64 | ✅（与 host 一致） |
| `GetSystemWow64Directory2W` | **NOT_IMPLEMENTED** | 可用 | ⚠️ 新 API 缺失 |
| `GetMachineTypeAttributes` | **NOT_IMPLEMENTED** | 可用 | ⚠️ 新 API 缺失 |
| `PROCESSOR_ARCHITECTURE` | `x86` | x86 | ✅ |
| `PROCESSOR_ARCHITEW6432` | `ARM64` | ARM64 | ✅ |
| `PROCESSOR_IDENTIFIER` | `ARMv8 (64-bit) Family 8 Model D03 Revision 200, Unknown` | 应含 vendor | ⚠️ 尾部 `Unknown` |
| `ProgramFiles` | `C:\Program Files (x86)` | 同 | ✅ |
| `ProgramFiles(x86)` | `C:\Program Files (x86)` | 同 | ✅ |
| `ProgramW6432` | `C:\Program Files` | 同 | ✅ |
| `CommonProgramFiles(_x86/_W6432)` | 三个都有 | 同 | ✅ |
| `GetSystemDirectoryW` | `C:\windows\system32` | 32 位视图 | ✅ |
| `GetSystemWow64DirectoryW` | `C:\windows\syswow64` | 同 | ✅ |
| `HKLM\HARDWARE\...\CentralProcessor\0\Identifier` | `ARMv8 (64-bit) Family 8 Model D03 Revision 200` | 同 | ✅ |
| `HKLM\HARDWARE\...\ProcessorNameString` | **缺失** | 有值 | ⚠️ |
| `HKLM\SYSTEM\...\Session Manager\Environment\PROCESSOR_ARCHITECTURE` | **`ARM64`** | ARM64（或按视图 x86） | ⚠️ 与 GetNativeSystemInfo 矛盾 |
| `...\Environment\ProgramFilesDir` | **缺失** | `C:\Program Files` | ⚠️ |
| `...\Environment\ProgramFilesDir (x86)` | **缺失** | `C:\Program Files (x86)` | ⚠️ |
| `...\Environment\ProgramW6432` | **缺失** | `C:\Program Files` | ⚠️ |
| `ntdll.dll` / `kernel32.dll` / `kernelbase.dll` | I386（`C:\windows\system32\*`） | i386 ntdll + WOW64 backend | ✅ |
| `wow64.dll` / `wow64win.dll` / `wow64cpu.dll` | 未加载 | 运行时自研 WOW64/FEX，可解释 | — |
| `libwow64fex.dll`（HODLL, 32 位 WOW64 引擎） | 未在 x86 probe 中加载 | — | 见启动日志 `HODLL=libwow64fex.dll (feX)` |

> **Steam 影响推测**（待 P0-2C trace 证实）：
> * 若 updater 用 `GetNativeSystemInfo` 判断"OS 是否支持 x64"，它会看到 **AMD64** → 应判定支持；
> * 若它改用 `IsWow64Process2.NativeMachine` 或 `PROCESSOR_ARCHITEW6432`，看到 **ARM64** → 可能判定
>   "非 x86-64 平台" → 继续使用 `steam_client_win32`；
> * 注册表缺 `ProgramFilesDir (x86)` / `ProgramW6432` 也会让部分安装器认为不是完整 64 位环境。
> 三者**互相矛盾**本身就是风险：任何组合判断都可能得出"这是 32 位系统"的结论。

---

## 3. 目标语义（Windows-on-ARM 自洽模型）

```text
Physical host        : ARM64 / HarmonyOS
Windows environment  : 64-bit capable
x86 process view     : PROCESSOR_ARCHITECTURE=x86
                       PROCESSOR_ARCHITEW6432=ARM64
                       IsWow64Process=TRUE
                       IsWow64Process2={ProcessMachine=I386, NativeMachine=ARM64}
                       GetSystemInfo=INTEL/x86, GetNativeSystemInfo=ARM64   ← 需一致
x64 process view     : PROCESSOR_ARCHITECTURE=ARM64? / AMD64（取决于模拟语义，需与 x64 probe 实测后定）
```

> 关键原则（方案 §11/§12）：`Process Architecture ≠ OS Capability`。
> 不能把 x86 进程伪装成 AMD64；也不能让"是否支持运行 x64"这件事在不同 API 间自相矛盾。

---

## 4. ⛔ 当前阻塞：x64 执行路径不可用（P0-2 前置）

实测（2026-09-17 15:1x–15:2x）：

| 程序 | 结果 |
| --- | --- |
| `arch_probe_x86.exe`（本方案新探针） | ✅ 正常运行并写出 `C:\smoke\results\arch-probe-x86.json` |
| `arch_probe_x64.exe`（本方案新探针） | ❌ 启动 ~80ms 后 SIGSEGV |
| `C:\smoke\x64\triangle.exe`（**探测前就有的原生 x64 smoke**） | ❌ 同样 SIGSEGV（`reason=11`） |

崩溃现场（wine_stderr）：

```
[dlopen-trace] load_builtin_unixlib .../libarm64ecfex.so
starting FEX based libarm64ecfex.dll
.../kernel32.so  .../kernelbase.so  .../ucrtbase.so  .../advapi32.so  .../sechost.so  .../msvcrt.so
[early-fault] pid=11260 tid=11260 sig=11 code=2 addr=0x7ffefe3a08 pc=0x6ffc5269e4 lr=0x6ffc526c00 sp=0x2078feb0
[early-fault] pc=0x6ffc5269e4 <unmapped>      ← FEX/ARM64EC 代码区（JIT/宿主地址）
```

* `sig=11 code=2` = `SEGV_ACCERR`（权限错误，不是空指针）；
* 两次独立启动命中**同一 pc/lr**（0x6ffc5269e4 / 0x6ffc526c00）→ 确定性失败；
* 与 arch probe 无关：**原生的 x64 smoke 程序同样失败**。

另注：不带 `WINEHUA_WOW64_ENGINE=fex` 时，x64 exe 会被默认路由到 `wowbox64.dll`
（已降级线路）并立即退出；带上 fex 才走 `libwow64fex.dll` / `libarm64ecfex.dll` 路径，
但随后即出现上述 SIGSEGV。

> **给专家的两个问题**：
> 1. 我们今天的 Wine 全量重编（为 P0-1 的 win32u/winewayland 改动）是否会破坏 arm64ec/FEX64 运行时一致性？
>    （HAP 内 `libarm64ecfex.dll` 等 PE 与 `aarch64-unix` 侧是否处于同一构建批次/版本）
> 2. x64 执行路径在本 runtime 里**最近一次确认可用**是什么时候、哪个 HAP？（若有更早的通过记录，
>    可直接二分 HAP 定位是"今天引入"还是"更早就坏了"。）

---

## 5. 下一步（按方案顺序）

1. **先恢复 x64 可用性**（P0-2 前置；否则"让 Steam updater 选择 x64"无意义）；
2. P0-2C：只 trace Steam updater 的架构判据（`GetSystemInfo/GetNativeSystemInfo/IsWow64Process2/`
   `GetSystemWow64Directory*/GetEnvironmentVariableW/RegQueryValue*`），产出 `docs/STEAM_ARCH_DECISION_TRACE.md`；
3. P0-2D：按证据**只修**语义错误的一层（优先顺序见方案 §16）；
4. P0-2E/F：让官方 updater 自己迁移到 x64 client + x64 CEF，并做稳定性 Gate。

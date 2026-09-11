# Wine / FEX 正式接口盘点（P1）

> 采样：2026-09-11，只读静态检查
> 对象：`WineHua-proton-parity` 内 `thirdparty/wine@dc5204ecb0c`、`thirdparty/fex@86ff33bbe`，
> 对照 `FEX@1cc4b93e`（FEX-2607，本地可得）
> 方法：源码符号核查 + 构建脚本核查。**未包含真机运行证据。**

## 0. 一句话结论

当前锁定链路的 **Wine 侧接口已经存在**（`__wine_unix_call_dispatcher`、
ARM64EC dispatcher/trampoline、`HODLL/HODLL64` 加载、`ohos_virtual` 等），
但 **FEX 侧停留在 UnixLib 之前的世代（`86ff33bbe`，FEX-2605 线）**：
没有 `Source/Windows/UnixLib/`，没有 `FEXUnixLib.cpp`，不产出两个 AArch64 UnixLib `.so`，
也不存在 `MemoryWineLoadUnixLibByName` / `MemoryWineUnixFuncs` 这套正式获取路径。

因此方案 P2 的"成套 FEX + UnixLib"在当前基线上**无法直接构建**，必须先做 FEX 版本决定。

## 1. 证据

### 1.1 FEX 两个快照的实际差异

| 项 | `86ff33bbe`（当前） | `1cc4b93e`（参考） |
| --- | --- | --- |
| describe | `FEX-2605-2-g86ff33bbe` | `FEX-2607` |
| 提交日期 | 2026-01-10 | 2026-07-02 |
| `Source/Windows/` 子目录 | `ARM64EC Common Defs WOW64 include` | `ARM64EC Common Defs WOW64 UnixLib include` |
| `Common/FEXUnixLib.cpp` | 无 | 有 |
| `UnixLib/{CMakeLists.txt,FEXUnixLib.cpp,FEXUnixLib.h}` | 无 | 有 |
| 产出 `libwow64fex.so` / `libarm64ecfex.so` | 否 | 是（`CreateLib` 两个 SHARED 目标，`SUFFIX ".so"`） |
| `MemoryWineUnixFuncs` 获取路径 | 仅枚举值，无获取代码 | `Common/FEXUnixLib.cpp` 内正式使用 |
| `MemoryWineLoadUnixLibByName` | 无 | 有（`winternl.h` 枚举 + `FEXUnixLib.cpp` 调用） |
| `__wine_unix_call_dispatcher` 直连 trampoline | 无 `Direct` 变量 | `UnixCallDispatcherDirect`（绕过间接调用检查） |
| 版本关系 | — | `merge-base = a04b0241c`，两者**分叉**，参考侧领先 321 提交 |

> 两个快照**不是**同一条线上的新旧关系，切换不是快进。这是本阶段最重要的结构性事实。

### 1.2 Wine 侧已有能力（`dc5204ecb0c`）

| 能力 | 位置 | 状态 |
| --- | --- | --- |
| `__wine_unix_call_dispatcher` 导出 | `thirdparty/wine/libs/winecrt0/unix_lib.c:60-72` | 已有 |
| ARM64EC 变体 + 内联 asm trampoline | `.../unix_lib.c:78-92`（`__wine_unix_call_dispatcher_arm64ec`） | 已有 |
| `__wine_unixlib_handle` / `load_so_dll` 路径 | `dlls/ntdll/loader.c:73,80,3507` | 已有 |
| `HODLL64` / `HODLL` 选择 CPU DLL | `dlls/ntdll/loader.c:4435` 起 | 已有 |
| OHOS 平台适配文件 | `dlls/ntdll/unix/ohos_virtual.{c,h}`、`ohos_broker.{c,h}`、`ohos_file.{c,h}` | 已有 |
| 硬件 TSO 的 `prctl(PR_GET_MEM_MODEL)` | `dlls/` 内 **未出现** | 缺口（应由 FEX UnixLib 承担） |

**Wine 侧已经实现了 UnixLib 协议的服务端**（这是本轮最重要的正面发现）：

```text
dlls/ntdll/unix/virtual.c:6243   case MemoryWineLoadUnixLibByName:
dlls/ntdll/unix/virtual.c:6244   case MemoryWineLoadUnixLibByNameWow64:
dlls/ntdll/unix/virtual.c:6255   get_unixlib_funcs( handle, info_class == ...Wow64, ... )
dlls/wow64/virtual.c:695,701,707 同族处理（WoW64 侧）
include/winternl.h:2438-2439     MemoryWineLoadUnixLibByName / ...Wow64 枚举
```

也就是说：**Wine 已经会说这套协议，缺的是 FEX 侧的调用方**。

结论：Wine 侧接口与方案 §5.4 描述一致；**瓶颈不在 Wine，而在 FEX 侧没有对应的 UnixLib 实现**。

### 1.2.1 UnixLib 在参考版本由 5 个提交引入（回移可行性已实测）

```text
dbaf22372  Windows: Adds empty Linux side unix library
c09225f86  Windows: Load unixlib if possible
201bb7398  Windows/UnixLib: Adds support for Hardware TSO support
954581c75  Windows/UnixLib: Adds remaining helpers
6c58fef22  Windows/UnixLib: Fix loading with new MemoryWineLoadUnixLibByName mechanism.
```

在 `86ff33bbe` 上顺序 `git cherry-pick -n` 这 5 个提交，**全部无冲突通过**；
且 `86ff33bbe → 1cc4b93e` 的 `.gitmodules` **没有差异**（嵌套子模块 pin 未变）。产出改动集：

```text
M  Source/Windows/ARM64EC/Module.cpp
M  Source/Windows/Common/Allocator.cpp
M  Source/Windows/Common/CMakeLists.txt
A  Source/Windows/Common/FEXUnixLib.cpp
A  Source/Windows/Common/FEXUnixLib.h
M  Source/Windows/Common/SHMStats.{cpp,h}
M  Source/Windows/Common/TSOHandlerConfig.h
A  Source/Windows/UnixLib/{CMakeLists.txt,FEXUnixLib.cpp,FEXUnixLib.h}
M  Source/Windows/WOW64/Module.cpp
M  Source/Windows/include/wine/unixlib.h
M  Source/Windows/include/winternl.h
```

这使"最小回移"从"未知风险"变成"已知可做"。

### 1.3 构建与打包脚本

`scripts/build_fex.sh`（当前）：

```text
只做两件事：
  build_fex_ec()  MINGW_TRIPLE=arm64ec-w64-mingw32  → build/fex-ec/Bin/libarm64ecfex.dll
  build_fex_pe()  MINGW_TRIPLE=aarch64-w64-mingw32  → build/fex-pe/Bin/libwow64fex.dll
构建类型：RelWithDebInfo；ENABLE_LTO=False；BUILD_TESTING=False
```

- 没有 UnixLib 目标，也没有任何 `.so` 安装 / 拷贝步骤。
- `Makefile` 与 `scripts/assemble.sh` 只把上述 **两个 PE DLL** 放进
  `wine-data/bin/<pe_dir>/`，运行时包里没有 UnixLib。

`FEX_Config.json`：**全仓库不存在**（`find` 只命中 `scripts/patches/fex-*.patch`、
`scripts/build_fex.sh`；`entry/` 下无 `FEX_*` 环境变量）。
因此当前进程用的是 FEX 编译期默认值，与 Proton 参考配置（MaxInst 500、
`X87ReducedPrecision=1`、`ProfileStats=1`、TSO 子项）不一致，且**无法确认最终生效值**。

### 1.4 统计结构（参考版本）

`FEXCore/include/FEXCore/Utils/SHMStats.h @1cc4b93e`：

```text
STATS_VERSION = 2
AppType: LINUX_32 | LINUX_64 | WIN_ARM64EC | WIN_WOW64
ThreadStatsHeader { Version, app_type, ThreadStatsSize, fex_version[48], Head, Size, Pad }
ThreadStats { Next, TID, AccumulatedJITTime, ... }
```

读取工具必须按 `STATS_VERSION`、`ThreadStatsSize`、`fex_version[48]` 对齐；
时间字段是**未缩放的 CPU 周期**，不能按主频直接换算成毫秒。

## 2. 接口盘点表

| # | 接口 | 现状 | 分类 | 验证方法 |
| --- | --- | --- | --- | --- |
| I-01 | `libarm64ecfex.dll` 构建与 ARM64EC 属性 | 已有，`RelWithDebInfo` | 需对齐 | `llvm-readobj --file-headers` 断言 `COFF-ARM64EC` |
| I-02 | `libwow64fex.dll` 构建与 aarch64 属性 | 已有 | 已有（保留） | `COFF-ARM64` 断言 |
| I-03 | `libarm64ecfex.so`（AArch64 UnixLib） | **已回移并构建**（`bba5d2e`） | `pass_new`（构建层） | `readelf -h` 断言 `AArch64` + `__wine_unix_call_funcs` 导出；待设备加载验证 |
| I-04 | `libwow64fex.so`（AArch64 UnixLib） | **已回移并构建** | `pass_new`（构建层） | 同上 |
| I-05 | `MemoryWineLoadUnixLibByName` 获取 UnixLib | **已随回移接入** | `pass_new`（源码层）/ 待运行验证 | 设备日志确认走新机制而非回退 |
| I-06 | `MemoryWineUnixFuncs` 获取 UnixLib | 同上，且作为回退路径保留 | `pass_new` | 同上 |
| I-07 | `__wine_unix_call_dispatcher` 加载 | Wine 已有；FEX 旧版直连 ntdll 符号 | 已有（形态不同） | `GetProcAddress` 成功日志 |
| I-08 | ARM64EC dispatcher 直连 trampoline | Wine 已有；FEX 旧版无 `Direct` 变体 | 需对齐 | 对照 `UnixCallDispatcherDirect` 使用点 |
| I-09 | 硬件 TSO `prctl(PR_GET/SET_MEM_MODEL)` | **代码已接通**（UnixLib 内实现） | 待设备返回值（不可用属 G2） | 记录返回值 / errno / 回退 |
| I-10 | 未对齐原子控制 | **代码已接通**（`PR_ARM64_SET_UNALIGN_ATOMIC`） | 待设备返回值（G2） | 同上 |
| I-11 | SHM 统计创建 / 扩容 / 清理 | **代码已接通**（UnixLib 内 `shm_open`/`ftruncate`/`mmap`/`shm_unlink`） | 待验证 | 读到 `STATS_VERSION`、线程槽增长、退出清理 |
| I-12 | 异常 / SMC / 保护页 | Wine `ohos_virtual.c` + `signal_arm64ec.c` 已有 | 已有（待真机回归） | 受控 SEH / 保护页测试 |
| I-13 | `HODLL64` / `HODLL` 选择 | 已有；默认 `wowbox64.dll`，`WINEHUA_WOW64_ENGINE=fex` 切 FEX | 已有 | 启动日志 `[WineChild] final` |
| I-14 | FEX 生效配置导出 | **缺失**（无配置文件） | 需新增（观测） | 导出编译期能力 + 进程生效值 |

## 3. 需要回补的最小集合

按方案 §8 分层，**不要混在一次提交里**：

1. **构建层**：把 FEX UnixLib 目标接入 `build_fex.sh`（独立 build 目录，例如
   `build/parity/fex-unixlib`），产出两个 AArch64 `.so`，并断言 ELF machine。
2. **打包层**：`assemble.sh` / `Makefile` 把两个 `.so` 放进运行时包的 Unix 侧目录；
   文件名、目录与 Wine 的 UnixLib 搜索路径必须一致。
3. **配置层**：新增可审计的 FEX 配置文件（Proton 参考配置），并保证能被导出哈希。
4. **观测层**：统计读取工具 + 生效配置导出脚本（只读，不改运行时行为）。

## 4. 明确不做

- 不把 86ff33bbe 直接"改名"成新版；不伪造版本对齐。
- 不为了"让接口存在"写成功桩；TSO / 未对齐原子不支持时要返回真实错误并验证回退。
- 不在这一批里同时升级 DXVK / vkd3d / Mesa / virglrenderer。
- 不使用第三方参考仓库的固定二进制偏移补丁。

## 5. 待补证据

| 项 | 需要的命令 / 手段 |
| --- | --- |
| FEX 旧版编译期能力（是否有 profiler / SHM 编译开关） | 检查 `build/fex-ec/CMakeCache.txt` 与 `-D` 传参 |
| FEX 旧版是否读取 `FEX_Config.json` | `git -C thirdparty/fex grep -n Config.json 86ff33bbe` |
| 设备实际加载的 CPU DLL 与 UnixLib | 真机 `hilog` + NF 加载日志（`OHOS-03`） |
| 进程最终生效的 FEX 配置 | 启动日志 / 调试接口导出（`OHOS-12`） |

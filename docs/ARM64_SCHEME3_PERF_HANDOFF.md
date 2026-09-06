# 方案③ vs 方案② 性能交接（Heaven ~2× / Unity 帧率下滑）

> 日期: 2026-09-06
> 读者: 后续性能分析（不必读过本会话）
> 仓库: `WineHua-arm64ec` · 分支 `feature/arm64-heaven-port`
> 当前设备包: WineHua HAP **1.0.13**（`vendor=example`），USB 平板已从 gtav-ohos hap-smoke 换回本包

本文只整理**代码路径、已核实事实、可证伪假说、建议 A/B**。不改代码、不下结论说「就是某一处」。崩溃修复见 `docs/ARM64_SCHEME3_HEAVEN_CRASH_FIX.md`（明确不含帧率）。

---

## 0. 现象（交接方口述）

1. **Unity 64-bit 已能跑**（`Z:\games\kqcs\LustFromTheDeep.exe`，Unity 2022.3.62f2 AMD64）。此前 `Player.log` 的 `80029c4a` / `could not load dd3d11.dll` 是 ntdll overlay 只搜 `x64/` 漏了 `arm64x/`，已修。
2. **整体帧率呈下降趋势**（同一局随时间变慢，不是「开局就固定低一档」）。
3. **Box 版 Heaven 与当前 ARM64EC 框架 Heaven 大约差一倍**。口述「图形优化点应该都一样」。

对照对象在本仓库里应对 **方案②**（`box64.so` 转译整份 x86_64 Wine），不是「同一个 wowbox64 换了个开关」。若对照的是仓库外另一份 Box64 模拟器，第 2 节仍然适用：图形 env 键名可以相同，**执行 ISA 与 thunk 层不同**。

---

## 1. 先拆开「优化点应该一样」

产品 DXVK / Venus / Host VirGL 的**键名表**两边大体同源（`AppendD3dBackendEnv` + `AppendStableDxvkEnv`）。下面这些**不是**同一条执行路径，不能凭 env 相同推断帧率该相同。

| 看起来一样 | 方案②（Box64+x64 Wine） | 方案③（本 HAP，ARM 原生 Wine） |
|---|---|---|
| 64-bit 游戏 CPU | `box64.so` 转译 x86_64 ELF Wine + x64 PE | `HODLL64=libarm64ecfex.dll`（FEX）转译 x64 PE |
| 32-bit 游戏 CPU（Heaven） | 仍在 box64 转译的 **x64 Wine WoW64** 里 | `HODLL=wowbox64.dll`（ARM64 PE Box64） |
| wineserver / unixlib | x86_64 ELF，被 box64 转译 | **aarch64 原生** |
| 64-bit `d3d11.dll` | `dxvk/.../x64/`（x64 PE，box64 转译） | `dxvk/.../arm64x/`（ARM64X，**不再转译 DXVK**） |
| 32-bit `d3d11.dll` | `dxvk/.../x86/`（x86 PE） | 同路径 x86 PE，但由 **wowbox64** 转译 |
| Guest Venus ICD | `venus_icd.x86_64.json`，x86_64 `.so`，box64 转译 | `venus_icd.aarch64.json`，**aarch64 原生** `.so` |
| `BOX64_DYNAREC_*` | 管整条 Wine+DXVK+Venus | Heaven 上 wowbox64 **会读**；Unity/FEX **完全不读** |
| FEX 调优 | 无 | **entry 里只 set `HODLL64`，没有任何 `FEX_*`** |
| SIGSEGV / SMC | box64 ELF 自己吃 Unix 信号 | OHOS sigchain + DFX 认领 + TEB trampoline + unix `mprotect`（崩溃修复引入） |

「图形优化开关一样」最多说明 **Host virglrenderer / DXVK fork 的 *意图* 一样**。Guest 谁在跑 ICD、谁在跑 DXVK、每帧 D3D 调用要不要过 ARM64X thunk，已经不是同一条栈。

---

## 2. 两条产品方案（编译期钉死）

`entry/src/main/cpp/wine_scheme.h`：

| ID | 宏 | 含义 |
|---|---|---|
| ② | `__aarch64__` + `WINEHUA_WINE_ARCH_IS_X86_64` | arm64 设备，Wine 是 x86_64，进程入口 `dlopen("box64.so")` → `box64_hmos_main` |
| ③ | `__aarch64__` 且 **无** 上宏 | arm64 设备，Wine 是 aarch64；ntdll 按 `HODLL`/`HODLL64` 加载 CPU dll |

判定与目录名：`entry/src/main/cpp/wine/wine_constants.h`（`WINE_UNIX_SUBDIR` / `WINE_PE_SUBDIR` / `WINE_WINE_ARCH`）。

子进程入口：`entry/src/main/cpp/proc/wine_child.cpp`

- 方案② Wine：约 L751，`dlopen box64.so`
- 方案② wineserver：约 L884，同样 box64 转译 x86_64 `wineserver`
- 方案③ CPU dll：约 L353–L363，`HODLL64=libarm64ecfex.dll`，默认 `HODLL=wowbox64.dll`（`WINEHUA_WOW64_ENGINE=fex` 才换 `libwow64fex.dll`）

---

## 3. 按 titl 拆执行栈（不要混 Heaven 和 Unity）

Heaven 4.0 实际是 **32-bit**（`browser_x86.exe` / `heaven.exe`）。Unity `LustFromTheDeep.exe` 是 **AMD64**。两边瓶颈可以完全不同。

### 3.1 Heaven x86（和「差一倍」最对得上的对照）

```text
方案②
  heaven.exe (i386)
    → box64 转译的 x64 Wine WoW64
    → dxvk/legacy/x86/d3d11.dll     （x86 PE，仍被转译）
    → x64 winevulkan unixlib        （被 box64 转译）
    → guest_vulkan x86_64 Venus ICD （被 box64 转译）
    → vtest socket
    → Host virglrenderer（arm64 原生）→ SurfaceQueue → XComponent

方案③（当前 HAP）
  heaven.exe (i386)
    → 原生 aarch64 Wine + HODLL=wowbox64.dll
    → dxvk/legacy/x86/d3d11.dll     （同一份 x86 PE，wowbox64 转译）
    → aarch64 winevulkan unixlib    （原生）
    → venus_icd.aarch64.json        （原生）
    → vtest → Host 同左
```

Heaven 上 **DXVK 二进制可以是同一份 x86 PE**。若仍差 ~2×，优先不要从「DXVK 优化开关没带上」找，而要从：

1. **转译器形态**：ELF `box64.so` 包住整条 Wine，vs ARM64 PE `wowbox64.dll` 只转 32-bit 用户代码；
2. **Guest Venus ISA**：x86_64+TSO 转译 vs 原生 aarch64 弱内存；
3. **信号/SMC 税**：方案③ 崩溃修复那条 sigchain（见第 5.2 节）；
4. **WoW64 边界**：32-bit DXVK → 64-bit unixlib 的 thunk 在原生 ARM Wine 里和「x64 Wine 被整进程转译」不是同一实现。

### 3.2 Unity x64（能跑、帧率下滑）

```text
方案②
  LustFromTheDeep.exe (x64)
    → 全程 box64：游戏 + x64 DXVK + x64 Venus

方案③（当前）
  LustFromTheDeep.exe (x64)
    → FEX（libarm64ecfex.dll）转译游戏 CPU
    → ARM64X d3d11/dxgi（原生 ARM 执行，FEX native view）
    → 每次 x64 游戏代码 ↔ ARM64 DXVK 走 ARM64X dispatch
      （thirdparty/wine/dlls/ntdll/signal_arm64ec.c：
        __os_arm64x_check_call / dispatch_call / RetToEntryThunk）
    → 原生 aarch64 winevulkan + 原生 aarch64 Venus
    → Host 同前
```

这里 DXVK **不再吃转译**，按理图形库应更快。若整体仍慢或越跑越慢，更像：

- FEX 比 box64 慢（尤其 IL2CPP / JIT / 自修改）；
- **每帧海量 D3D11 API 的 ARM64X 进出税**；
- FEX SMC / 代码缓存膨胀（下滑形态）。

ntdll overlay（本次能跑的前提）：`thirdparty/wine/dlls/ntdll/loader.c` `search_winehua_dxvk_overlay`，搜索顺序 `arm64x` → `x64` → `x86`。32-bit ntdll 对 ARM64X 返回 `STATUS_NOT_SUPPORTED` 后继续 `x86`。

---

## 4. Env 管线（「开关」真正注入的地方）

唯一顺序在 `entry/src/main/cpp/wine/env_profiles.h`：

```text
BuildWineEnv                  L0–L5 基线、Box64 出厂表、LD_LIBRARY_PATH、GraphicsBroker
  → AppendD3dBackendEnv       dxvk/vkd3d 路径、ICD、VN_PERF、WINEDLLDIR*
  → AppendStableDxvkEnv       桌面稳定化（explorer 和 runWineProgram 同源）
  → extraEnv                  per-app 最后覆盖
```

实现：

| 步骤 | 文件 |
|---|---|
| `BuildSessionEnv` | `entry/src/main/cpp/wine/env_profiles.cpp` |
| `BuildWineEnv` / `AppendD3dBackendEnv` | `entry/src/main/cpp/wine/wine_env.cpp` |
| `AppendStableDxvkEnv` | 同上 `env_profiles.cpp` |
| Box64 出厂表 | `entry/src/main/cpp/wine/wine_env_baseline.h` `Box64PerfTable` |
| UI 档位（stability…performance） | `entry/src/main/ets/service/Box64Dynarec.ets` — **只进 runWineProgram extraEnv，不进 FEX** |
| 子进程落地 + `HODLL*` | `wine_child.cpp` `setup_wine_env` / `reassert_arch_wine_runtime_env` |
| 桌面/直启组装 | `wine_launch.cpp` / `wine_exe.cpp`（`applyStableOverlay = true`） |

### 4.1 方案③ 相对方案②，DXVK 路径上**会变**的键

`AppendD3dBackendEnv`（`wine_env.cpp` DXVK 分支约 L348–L451）：

- `overlay64`：方案③ → `.../dxvk/<legacy|modern-2.6>/arm64x`；方案② → `.../x64`
- `WINEHUA_VULKAN_LOADER_ARCH` / `WINEHUA_VENUS_ICD_ARCH`：`aarch64` vs `x86_64`
- `VK_DRIVER_FILES` / `VK_ICD_FILENAMES`：`venus_icd.aarch64.json` vs `venus_icd.x86_64.json`
- **不**注入 `USE_LIBBOX64`、`BOX64_LD_LIBRARY_PATH`、`BOX64_EMULATED_LIBS`（那是方案② 才有）

方案③ **仍然**注入（与方案② DXVK 桌面链同类）：

- `VN_DEBUG=vtest`
- `VN_PERF`：legacy = `no_fence_feedback,no_query_feedback,no_multi_ring`（**不含** `no_semaphore_feedback`）；modern 2.6 才加 `no_semaphore_feedback`
- `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
- `WINEDLLOVERRIDES=d3d11=n;dxgi=n`

`AppendStableDxvkEnv`（`env_profiles.cpp`）在 DXVK/VKD3D 会话上再夹：

- `DXVK_LOG_LEVEL=warn`
- `BOX64_DYNAREC_WEAKBARRIER=0`（`#ifdef __aarch64__`，方案③ Heaven 的 wowbox64 **看得到**）
- `WINEHUA_PERF_PROFILE` 默认 `shadow-precise-dirty-ring-inline-upload-coverage-sort`
- `DXVK_WINEHUA_PRECISE_SHADOW=1`
- `VN_WINEHUA_STRONG_RING_BARRIER=1`（可用已有值覆盖）

Host 侧影子/upload：`entry/src/main/cpp/graphics/virgl_host_config.cpp`（`VKR_WINEHUA_SHADOW_FROM_HOST`、`GPU_UPLOAD`、`COVERAGE_SORT` 等）。Guest 键与 Host 键成对，**Host 进程是原生 arm64，两边方案相同**。

### 4.2 方案③ Guest GL（Heaven Qt 启动器走 OpenGL）

`graphics_broker.cpp` 约 L968–L975：

- 正确：guest env 自带 `LIBGL_ALWAYS_SOFTWARE=1` + `MESA_LOADER_DRIVER_OVERRIDE=swrast` + `GALLIUM_DRIVER=virpipe`
- 方案③ 再 `UpsertEnvLine(LIBGL_DRIVERS_PATH=/data/storage/el1/bundle/libs/arm64)`，因为 musl 会拒 el2 `LD_LIBRARY_PATH`
- **不要**把 `MESA_LOADER_DRIVER_OVERRIDE` 改成 `virpipe`（virpipe 是 gallium pipe，不是独立 dri）

方案② 的 dri 在 el2 `guest_gfx/lib/dri`，由 box64 转译加载。

### 4.3 明确没有接到 FEX 上的「优化」

`wine_child.cpp` 方案③只设置 `HODLL64`。全树 `entry/` 没有 `FEX_*`。

`Box64PerfTable` 会进 `BuildWineEnv`（aarch64 都灌），对 Unity **无效**。UI 兼容档位同样只影响 wowbox64 / 方案②。

---

## 5. 假说（按优先级，均可做 A/B）

### 5.1 P0 — CPU 转译器不是同一套（解释「开关一样仍差一倍」）

- Heaven：wowbox64 PE + OHOS 信号 vs 方案② 整进程 `box64.so`。Dynarec 参数即使同名，**实现/版本/热路径**可以不同（当前 box64 子模块 `16515448b`，方案② HAP 未必同 commit）。
- Unity：FEX 默认块大小、TSO、SMC 策略 vs box64 `BIGBLOCK=3` `CALLRET=2` `FORWARD=1024`。没有对照过 FEX 默认值。

**建议：** 同一 HAP 上 Heaven 做 `WINEHUA_WOW64_ENGINE=fex`（`HODLL=libwow64fex.dll`）vs 默认 wowbox64，只换 32-bit CPU dll。Unity 无法用 BOX64 档位对比，只能找 FEX 文档/环境变量做 TSO、multiblock、SMC 的 A/B。

### 5.2 P0 — 方案③ 信号/SMC 税（尤其 Heaven，也适合「越跑越慢」）

崩溃修复让 **每次** dynarec 写保护故障走：sigchain special handler → 认领 SIGSEGV → TEB trampoline → unix `ohos_mprotect_exec`（恢复 RW）→ 可能 `native_epilog`。`CALLRET=2` 会故意 `UDF` 出 SIGILL。

Unity IL2CPP / 运行时代码修补会持续 SMC。若 handler 或 `[SMC]` 日志（已限流，仍是 `write(2)`）随时间变密，就会表现为**单调下降**。

**建议：** 对比 `wine_stderr` 里 `[SMC]` 频率 vs 帧时间；`BOX64_DYNAREC_CALLRET=0` 只测 Heaven；临时把 SMC 日志再打稀（或关）看斜率是否消失。文件：`thirdparty/wine/dlls/ntdll/unix/ohos_virtual.c`、`thirdparty/box64/wine/wow64/wowbox64.c`。

### 5.3 P0 — Unity：ARM64X 每调用 thunk

64-bit 游戏（x64 视角）调 ARM64X `d3d11.dll` 必须过 `signal_arm64ec.c` 的 check_call / exit-to-x64。D3D11 每帧几千次 API 时，这项可以吃掉「DXVK 原生」省下来的时间。

**建议：**

- 对照：同一 Unity 进程，强制走 `dxvk/.../x64/d3d11.dll`（x64 PE，整库 FEX 转译）vs 现在的 `arm64x/`。overlay 搜索 `arm64x` 优先，测 x64 需要暂时改搜索顺序或挪开 `arm64x/d3d11.dll`。
- 若 x64 DXVK+FEX 反而更稳/更高，瓶颈就在 ARM64X 边界，不在 Host GPU。

### 5.4 P1 — Guest Venus ISA / 内存模型

`docs/archive/DXVK_GUEST_HOST_ARCHITECTURE_AND_PERF.md`：方案② 上 x86 release store 发布 ring tail 依赖 TSO，原生 `memcpy` 不够，后来加了 SC publish + `VN_WINEHUA_STRONG_RING_BARRIER=1` + `BOX64_DYNAREC_WEAKBARRIER=0`。

方案③ Guest ICD 是 **aarch64 原生**，弱内存。同一套 barrier 是否过强（每帧多余 fence → 低帧率）或过弱（失步 → 重试/卡住 → 下滑）都未知。`no_multi_ring` 把提交串到单 ring，负载高时队列只会长、不会平行消化。

**建议：** 打开 `VN_WINEHUA_PERF_SUMMARY=1`（`AppendStableDxvkEnv` 在 `VKR_WINEHUA_SHADOW_TRACE=perf` 时会开）和 Host `VKR_WINEHUA_PERF_SUMMARY`；看 ring wait、CS error、Host→Guest shadow 字节是否随时间涨。不要先改 `no_multi_ring`（曾导致 command stream 失步）。

### 5.5 P1 — 默认 DXVK 档 + 设备白名单

`EntryAbility.ets`：仅 `VYG-AL00` + `incrementalVersion=26.0.0.32` 默认 `dxvk_modern_2_6`，其余平板默认 **`dxvk_legacy`（1.10.3）**。对照用的 Box HAP 若默认 2.6，Heaven 不能直接比。Unity 启动后看 hilog `[WineChild] final D3D env` 的 `dxvkVersion` / `x64=(present,present)`。

### 5.6 P1 — 帧率下滑的其它机制（Unity 更像）

| 机制 | 怎么证伪 |
|---|---|
| FEX/wowbox64 代码缓存或 SMC 风暴 | `[SMC]` 速率、进程 RSS、FEX 若有 cache 统计 |
| DXVK/Vulkan 资源泄漏 | `DXVK_LOG_LEVEL=info`，`C:\windows\temp\` 下 `*_d3d11.log` 是否 shader/memory 持续涨 |
| Host 影子全量 refresh 回归 | `WINEHUA_PERF_PROFILE` 是否仍是 precise；Host perf 里 Host→Guest MiB/帧 |
| `wine_stderr` / hilog 写爆 | 方案③ 曾把 stderr 写到数百 MB；看 `/data/storage/el2/base/temp/wine_stderr_*.log` 增长速度 |
| 热节流 | 设备温度 vs FPS 斜率 |
| Unity GC / 着色器编译 | `Player.log` 编译与 GC 尖峰是否对齐掉帧 |

### 5.7 P2 — 次要差异

- 方案③ `LD_LIBRARY_PATH` 只有 el1 `.../libs/arm64`（musl 拒 el2）。ICD/`libvulkan` 扫错会静默走软件或错误 ICD。启动时已有 `[WineChild] vulkan scan` 和 `dlopen libvulkan.so.1` 日志。
- Qt/Heaven 若误用 softpipe 而不是 virpipe，OpenGL 启动器会极慢（D3D11 游戏本体仍走 Venus）。查 `GALLIUM_DRIVER`、`LIBGL_DRIVERS_PATH`。
- 音频/控制桥：`EnsureBridgeForWineLaunch` 在 `BuildWineEnv` 里，一般不是 2× 主因。

---

## 6. 建议的实验顺序（给接手人）

同一台平板、同一 DXVK 档（先都锁 **1.10.3 legacy**）、同一 Heaven 预设、Qt 启动器直出（不要 skip-launcher）。

1. **确认方案与 overlay（5 分钟）**  
   hilog：`[Scheme]` 必须是方案③；`[WineChild] final D3D env`；`VK_ICD_FILENAMES` 含 `venus_icd.aarch64.json`。Heaven 进程应加载 `dxvk/legacy/x86/d3d11.dll`；Unity 应加载 `arm64x/d3d11.dll`。

2. **Heaven：只换 32-bit CPU dll**  
   默认 wowbox64 vs `WINEHUA_WOW64_ENGINE=fex`。图形栈不动。若 fex 更接近或更差，2× 主要在 CPU dll / 信号，不在 DXVK 键。

3. **Heaven：CALLRET / SMC**  
   额外 `BOX64_DYNAREC_CALLRET=0`（方案③ 出厂是 `2`，见 `wine_env_baseline.h`）。看平均 FPS 与是否还下滑。

4. **Unity：ARM64X vs x64 DXVK**  
   见 5.3。记录平均 FPS 和 10 分钟斜率。

5. **图形剖分**  
   `C:\windows\temp\winehua_display_fps.txt`、Host `VKR_WINEHUA_PERF_SUMMARY`、Guest ring summary。区分 GPU/影子 vs CPU 转译。历史 cube 基线见 archive 备忘录（Host present ~5ms 时低 FPS 仍可能在 Guest shadow）。

6. **长局下滑**  
   同时采：FPS 文件、`[SMC]` 计数、RSS、`wine_stderr` 大小、Player.log、温度。哪条曲线先拐，哪条就是主因。

---

## 7. 现场日志位置

| 用途 | 路径 |
|---|---|
| Wine 子进程 stderr | `/data/storage/el2/base/temp/wine_stderr_YYYYMMDD.log` |
| DXVK | `C:\windows\temp\`（`DXVK_LOG_PATH`） |
| 显示 FPS | `C:\windows\temp\winehua_display_fps.txt` |
| vtest frontbuffer | `/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log` |
| Unity | 游戏目录 `Player.log` |
| 启动 env | hilog tag `WL_NAPI` / `[WineEnv]` / `[WineChild]` / `[Scheme]` |

---

## 8. 相关文档与代码索引

| 文档 | 内容 |
|---|---|
| `docs/ARM64_SCHEME3_HEAVEN_CRASH_FIX.md` | DFX/SMC/FS 崩溃，不含帧率 |
| `docs/archive/DXVK_GUEST_HOST_ARCHITECTURE_AND_PERF.md` | Guest→Host、TSO、shadow-precise、cube FPS 基线（方案② 语境为主） |

| 代码 | 内容 |
|---|---|
| `entry/src/main/cpp/wine_scheme.h` | 方案①②③ |
| `entry/src/main/cpp/wine/wine_constants.h` | `WINE_WINE_ARCH` |
| `entry/src/main/cpp/wine/wine_env.cpp` | 基线 + D3D overlay |
| `entry/src/main/cpp/wine/env_profiles.cpp` | 稳定化 overlay |
| `entry/src/main/cpp/wine/wine_env_baseline.h` | `BOX64_DYNAREC_*` 出厂 |
| `entry/src/main/cpp/proc/wine_child.cpp` | `HODLL*`、box64.so 入口 |
| `entry/src/main/cpp/graphics/graphics_broker.cpp` | virpipe / el1 dri |
| `entry/src/main/cpp/graphics/virgl_host_config.cpp` | Host 影子/upload |
| `entry/src/main/ets/entryability/EntryAbility.ets` | 默认 DXVK 档 |
| `thirdparty/wine/dlls/ntdll/loader.c` | DXVK overlay 搜索 |
| `thirdparty/wine/dlls/ntdll/signal_arm64ec.c` | ARM64X dispatch |
| `thirdparty/wine/dlls/ntdll/unix/ohos_virtual.c` | sigchain / SMC |
| `thirdparty/box64/wine/wow64/wowbox64.c` | 32-bit 宿主故障 |

---

## 9. 已知不要当「优化」来做的事

- 把 `MESA_LOADER_DRIVER_OVERRIDE` 改成 `virpipe`
- skip Heaven Qt launcher 当性能修复
- 未过 x86/x64 command-stream 门禁就去掉 `no_multi_ring`
- 以为改 `Box64Dynarec.ets` 能加速 Unity（FEX 不读这些键）
- 用方案② 的 x64 Venus ICD 硬塞给方案③ 原生 Wine（ISA 对不上）

---

## 10. 当前会话里已核实、与性能弱相关的事实

- Unity `LoadLibrary(d3d11)` 失败已用 overlay `arm64x` 搜路修好，HAP 1.0.13 已装到 `5KPBB25818203996`。
- 平板上 `files/wine/dxvk/legacy/{arm64x,x64}/d3d11.dll` 在 O2 重编后均为 **7 927 296** 字节（旧 O0 约 16.6 MB）；aarch64 `ntdll.dll` strip 后约 1.7 MB。
- `.wine` 在换包过程中重建过（一次完整初始化）。
- O2 ARM64X 已装到设备并哈希对得上 HAP；Unity 手感无明显提升。下一步是 FEX/`FEX_*` 与 ARM64X thunk，而不是再拧 DXVK `-O`。
- 本交接**没有**新的 FPS 采样；2× 与「下降趋势」以口述为准，接手后请先用第 6 节实验打出数字。

---

## 11. ARM64EC Release Audit（2026-09-06）

手搓 ARM64X 链没有继承 meson `buildtype=release`。Wine PE 走的是 Wine 自己的 `${arch}_CFLAGS`，**不是** `WINE_CFLAGS`（那只给 Unix `.so`）。

| 组件 | 证据 | 结论 |
|---|---|---|
| DXVK 1.10 / 2.6 **x86/x64** | `scripts/build_dxvk.sh` / `build_dxvk_modern.sh` `-Dbuildtype=release` | 绿（meson） |
| DXVK 1.10 / 2.6 **ARM64X** | 原 `clang -marm64x` 无 `-O*` | 已改为 `$ARM64X_OPT_FLAGS`（默认 `-O2 -DNDEBUG`）+ 断言。**2026-09-06 已重编并装到平板**（legacy `d3d11.dll` 7 927 296） |
| FEX `libarm64ecfex.dll` / `libwow64fex.dll` | `build/fex-ec/CMakeCache.txt`：`RelWithDebInfo`，`CMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g -DNDEBUG` | 绿 |
| wowbox64.dll | 子工程 `CMAKE_BUILD_TYPE=Release`，`CMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG` | 绿 |
| Wine Unix `.so` | `WINE_CFLAGS="-g -O2 ..."` | 绿 |
| Wine **PE** `aarch64` / `arm64ec` / `i386` | `build/wine-ohos/Makefile`：`arm64ec_CFLAGS = -g -O2`（实际规则：`$(arm64ec_CC) ... -target arm64ec-windows ... $(arm64ec_CFLAGS)`） | 绿，**不是 O0** |
| HarmonyOS 上 `CROSSCFLAGS` | 曾只 `export CROSSCFLAGS="-I.../include"`，Wine 会用整串替换默认 `-g -O2` | 已改成 `-g -O2 -I...`，避免下次在鸿蒙宿主编出 O0 PE |

装进 HAP 的系统 DLL 来自 `aarch64-windows/`（`aarch64_CFLAGS=-g -O2`）。`arm64ec-windows/` 在本工程主要产 `.o` 做 ABI 校验，同样 `-g -O2`。所以「Wine ARM64EC PE 全是 O0」可以排除；**当前最脏的已装产物仍是 ARM64X DXVK**。

门禁：

- `scripts/env.sh`：`require_optimization_flags` / `require_ndebug` / `require_cmake_not_debug`
- 各构建脚本在 compile 前断言
- `make arm64ec-release-gate` → `scripts/check_arm64ec_release_gate.sh`

ARM64X `-O2` 已落地。Unity 相对 Box64 master 仍无明显提升，瓶颈在 FEX 默认策略（无 `FEX_*`）和 ARM64X 每调用 thunk，不在 DXVK 优化等级。下一步做 FEX TSO/SMC/块大小 A/B，以及强制走 meson `x64` DXVK 对照 thunk 税。


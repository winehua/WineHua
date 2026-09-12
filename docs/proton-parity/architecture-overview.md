# 当前架构总览（Proton ARM64 对齐分支）

> 分支：`feature/proton-arm64-parity`（工作树 `/home/liufeng/src/WineHua-proton-parity`）
> 基线：`feature/arm64-heaven-port @ 2728523`
> 本文只描述**现在实际是什么样**，以及每一块归谁管。逐项证据见 `README.md` 里列的各篇文档。

## 1. 运行时的分层（方案③：aarch64 原生 Wine + FEX）

```text
┌─ App 进程 (ArkTS + libentry.so) ─────────────────────────────────────┐
│  UI / WineWindowAbility / 窗口管理 / 输入采集 / 音频采集             │
│  WineEnvService    : 组装会话环境、决定 DXVK 档位、启动引擎           │
│  GameHook / SmokeHook : 用 Want 参数驱动"跑游戏 / 跑 smoke"（自动化） │
└───────────────┬──────────────────────────────────────────────────────┘
                │ OH_Ability_StartNativeChildProcess (NCP)
                ▼
┌─ wine_child.so  (每个 Wine 进程一个 NCP 子进程) ─────────────────────┐
│  dlopen("ntdll.so") → __wine_main         ← aarch64 原生 Wine        │
│                                                                      │
│  CPU 后端 (由 HODLL/HODLL64 选择, 都在 aarch64-windows/)             │
│    x64 应用 : HODLL64 = libarm64ecfex.dll   (FEX, ARM64EC ABI)       │
│    x86 应用 : HODLL   = libwow64fex.dll     (FEX)                    │
│                      或 wowbox64.dll        (Box64, WINEHUA_         │
│                                              WOW64_ENGINE=box)       │
│                                                                      │
│  FEX 的 Unix 侧配套 (entry/libs/<arch>/, 由 ntdll 的                 │
│  MemoryWineLoadUnixLibByName 按名 dlopen)                            │
│    libarm64ecfex.so / libwow64fex.so  (aarch64 ELF)                  │
│      提供: 硬件 TSO 探测/开关、未对齐原子、SHM 统计内存、VMA 命名     │
└───────────────┬──────────────────────────────────────────────────────┘
                │ vtest socket / SurfaceQueue
                ▼
┌─ virgl_child.so (NCP 子进程) = 宿主图形 ─────────────────────────────┐
│  virglrenderer + vkr (宿主 Vulkan)                                  │
│  SurfaceQueue → egl_renderer → XComponent 上屏                      │
│  virgl_surface_presenter / venus_surface_presenter (两条上屏路径)    │
└─────────────────────────────────────────────────────────────────────┘

Guest 图形（都走上面那条 vtest → 宿主）:
  D3D11 → DXVK (x86 PE / ARM64X) → winevulkan → Venus ICD (aarch64) → vtest
  D3D9  → WineD3D → guest Mesa virpipe (GL) → vtest
```

**两条 CPU 后端、两条图形栈**是理解这套东西的关键：
后端决定"谁在执行 guest 指令"，图形栈决定"帧怎么从 guest 走到屏幕"。
本次对齐证明**两者都不是当前帧率的瓶颈**（见 §4）。

## 2. 本分支相对基线改了什么

**没有移动任何子模块指针**（`thirdparty/*` 保持基线提交）。
需要动子模块源码的地方，一律走项目既有约定：**打成 patch，由构建脚本在构建时应用**。

| 层 | 文件 | 做了什么 |
| --- | --- | --- |
| L2 FEX 源码 | `scripts/patches/fex-unixlib-backport.patch` | 回移上游 6 个提交，补上 `Source/Windows/{Common,UnixLib}/FEXUnixLib.*` + `winternl.h` 枚举 → 产出两个 UnixLib `.so` |
| L2 FEX 构建 | `scripts/build_fex.sh` | ①参数对齐 Proton 官方（Release / profiler=True / LTO off / BUILD_TESTING=False / TUNE_CPU=none / RANGES_NATIVE=OFF）；②写入 `OVERRIDE_VERSION/HASH`（stats 里能反查版本）；③新增 UnixLib 构建目标（aarch64 ELF，断言 `__wine_unix_call_funcs` 导出）；④用"参数签名"判断是否需要重新 configure，修掉旧缓存坑 |
| L2 诊断 | `scripts/patches/fex-unixlib-probe.patch` | 临时探针（`FEX_UNIXLIB_PROBE=1` 才应用）：证明 UnixLib 被加载、打印 TSO/未对齐原子返回值、dump SHM 统计头 |
| L4 配置 | `FEX_Config.json`（新增） | Proton 参考配置（ProfileStats=1、X87ReducedPrecision=1、MaxInst=500 等） |
| L4 打包 | `scripts/assemble.sh` | ①`FEX_Config.json` → 运行时 `share/fex-emu/Config.json`；②两个 UnixLib `.so` 归位到 `entry/libs/<arch>/`；③新增 `p3-wow64-ab` / `p4-amd64-fex` / `p5-paths` / `p5-headroom` 四组 smoke 套件 |
| L1 平台 C++ | `entry/src/main/cpp/proc/wine_child.cpp` | 设 `FEX_APP_CONFIG_LOCATION`；把 32 位默认引擎切到 FEX（并入的原工作树改动）；`FEX_*` 环境变量透传与日志 |
| L1 平台 ArkTS | `GameHook.ets` / `WineEnvService.ets` | 并入的原工作树改动：`winehua.perf_diagnosis` / `run_id` / `wow64_engine`，perf 档位覆盖 |
| 图形诊断 | `venus_surface_presenter.cpp` | 并入的原工作树改动：present 日志增加帧间隔分位 `frame_gap_us p50/p95/p99` |
| 子模块补丁 | `patches/mesa-ohos-wow64-map-fd.patch`、`patches/wine-wow64-shared-map.patch` | 并入的两处 WoW64 修复（**目前只被构建脚本引用，尚未生效**，见 §5） |
| L5 工具 | `smoke/winehua_d3d_switch_cube.c`、`docs/proton-parity/tools/*` | cube 的 `--bench` / `--bench-offscreen` / `--draw-loop`；无 Docker 出包工具（改 HAP 内层 payload、重打 UnixLib） |

## 3. 关键数据流（三条，各自归谁管）

**A. CPU DLL 与 UnixLib 的加载链**
`wine_child` 设 `HODLL64/HODLL` → ntdll `loader.c` 按名加载 PE CPU DLL
→ FEX 起来后通过 `NtQueryVirtualMemory(MemoryWineLoadUnixLibByName, "wow64fex"|"arm64ecfex")`
→ ntdll `load_unixlib_by_name()` 在 `entry/libs/<arch>/` 里 `dlopen` 对应 `.so`
→ `get_unixlib_funcs()` 取 `__wine_unix_call_funcs`。
**Wine 侧本来就有这条链**，本分支只是把 FEX 侧的调用方补上。

**B. FEX 生效配置**
FEX 只按环境变量找配置目录 → `wine_child` 指 `FEX_APP_CONFIG_LOCATION` 到
运行时包的 `share/fex-emu/` → FEX 读 `Config.json`。
（拿掉配置文件即回落到源码默认值。）

**C. 帧路径**
Guest（DXVK 或 WineD3D）→ vtest socket → 宿主 virglrenderer/vkr →
SurfaceQueue → egl/XComponent。**宿主 presenter 按屏幕周期 pacing**（这台设备 90 Hz）。

## 4. 已经验证过的结论（都有真机证据）

| 结论 | 关键证据 |
| --- | --- |
| UnixLib 真的被加载 | 探针构造函数在两个 wine 进程里触发 |
| 内核能力实测 | `PR_GET_MEM_MODEL` → `0xffffffff / errno=22`；`PR_ARM64_SET_UNALIGN_ATOMIC` → `-1 / errno=22`（**不支持**，非接口问题） |
| SHM 统计可读 | `version=2 app_type=3 slot_size=80 capacity=4096 fex_version=FEX-2604-99-g86ff33b` |
| 生效配置确实被读 | 打开 `ProfileStats` 后 SHM 统计被创建 |
| x86 后端对比 | wowbox64 vs FEX 帧时间差 <1%（都在显示节拍附近） |
| x64 链路 | 真 AMD64 + FEX + ARM64X `d3d11/dxgi` 实际加载，与原生 ARM64 持平 |
| 帧时间构成 | 90 Hz 节拍 11.129 ms；宿主 present 仅 2.97 ms；render 0.09–0.17 ms；**DXVK/Venus 每帧超预算约 1 ms** |
| WoW64 整块复制未复现 | `[WOW64-MAP-PERF] copy_total=0 / copy_bytes=0 / failed_total=0` |

## 5. 还没做 / 没验证的

| 项 | 状态 |
| --- | --- |
| **目标游戏性能 A/B** | 未完成。真实游戏帧率**能读**（合成器 FPS 文件 + `winehua_gl_present_bridge readbacks`），但缺"停止当前游戏"的自动化入口，做不了同一会话的干净重复；且合成器那行不绑定具体窗口。**按你的意思，测试另作安排。** |
| **两个 WoW64 补丁生效** | 已并入但未生效 —— 需要重编 `thirdparty/wine` 与 guest Mesa（长构建）。 |
| FEX 探针 | 仍随 `FEX_UNIXLIB_PROBE=1` 可选加入；默认不应用。`presenter_common.h` 的诊断开关 `kForcedOn` 已还原为 `false`。 |
| 真机窗口/交互回归 | 未做（本轮没接屏幕交互）。 |

## 6. 回退方式（都已确认可执行）

| 想退到哪 | 动作 |
| --- | --- |
| 完全放弃实验 | 丢弃 `feature/proton-arm64-parity`（产品分支从未被修改） |
| 只用 Box64 跑 32 位 | `WINEHUA_WOW64_ENGINE=box` |
| 回到旧 FEX 构建参数 | `FEX_BUILD_TYPE=RelWithDebInfo FEX_PROFILER=False` |
| 去掉 UnixLib 回移 | 从 `build_fex.sh` 的 `FEX_PATCHES` 移除 `fex-unixlib-backport.patch` |
| 去掉生效配置 | 删除 `FEX_Config.json` |

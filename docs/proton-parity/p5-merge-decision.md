# P5：合入判定与回退策略

> 依据：方案 §7 P5 的合入前要求
> 「正确性通过、启动/退出稳定、目标游戏性能有可解释结果、配置明确、
>   稳定后端可回退，且没有混入无法追踪的图形路径变化」
> 结论日期：2026-09-12

## 1. 逐条核对

| 合入要求 | 证据 | 判定 |
| --- | --- | --- |
| 正确性通过 | `p3c/p3d/p4c/p5b/p5c/p5d/p5e` 共 7 轮 smoke 全 PASS；`angleRegressions=0`；D3D11 `featureLevel=11_0`、`init/present HResult=0`；x64 用例实际加载 `dxvk/legacy/arm64x/{d3d11,dxgi}.dll`（未退回 x64 图形 DLL） | ✅ |
| 启动/退出稳定 | 7 次 `bm uninstall` + `bm install` + 冷启动全流程成功；每次运行后有 19–30 个 wine 子进程存活，`hilog` 无 `CRASH/SIGSEGV/SIGABRT/fatal` | ✅ |
| 配置明确 | `FEX_Config.json`（Proton 参考值）随包部署到 `share/fex-emu/Config.json`，由 `FEX_APP_CONFIG_LOCATION` 定位；真机 SHM 统计创建成功证明配置**确实被读取** | ✅ |
| 稳定后端可回退 | `WINEHUA_WOW64_ENGINE=box` 真机验证可切回 wowbox64；完整 x86_64 Wine + Box64 方案②链路未被本分支触碰 | ✅ |
| 未混入无法追踪的图形路径变化 | 本分支对图形的改动只有：`assemble.sh` 归位两个 FEX UnixLib `.so`、smoke cube 增加 `--bench`。presenter 上做过一次临时诊断开关，**已还原**（`kForcedOn = false`）。DXVK / vkd3d / Mesa / virglrenderer / Host present 策略均未改 | ✅ |
| **目标游戏性能有可解释结果** | **不满足** —— 见 §2 | ❌ |

## 2. 为什么"目标游戏性能"这一条不满足

本分支的性能测量全部基于内置 smoke cube。打到宿主日志后确认
（`p3-p4-smoke-ab.md` §3.5）：

```text
display_period_us = 11129   （屏幕 90 Hz）
两个 presenter 都按 11.129 ms pacing 出帧
```

**这个负载是显示节拍受限的**：帧时间主体就是屏幕节拍，四条 CPU 配置
（wowbox64 / FEX x86 / 原生 ARM64 / 真 AMD64+FEX）全部落在 12.05–12.33 ms，
差异 <1%；D3D9/WineD3D 路径甚至正好卡在节拍上（+0.04–0.19 ms）。

它能回答的是「有没有超出 90 Hz 预算、超了多少」，**不能**回答「这个后端在真实
游戏里快多少」。要给出目标游戏的可解释性能结论，必须换一个不吃显示节拍的负载
（离屏渲染、或每帧多次提交），或者直接在目标游戏上测。

### 2.1 直接上真实游戏：启动通了，但游戏没到渲染

用 `winehua.mode=game` 的 Want 通道（`GameHook`）启动设备上的目标游戏
`Z:\games\kqcs\LustFromTheDeep.exe`（Unity 2022 AMD64，交接文档里的同一款）：

```bash
aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode game --ps winehua.game_path "Z:\games\kqcs\LustFromTheDeep.exe"
```

启动链路是通的 —— 游戏进程起来了（pid 57966），宿主日志确认它拿到了完整的托管环境：

```text
WINEHUA_DXVK_ROOT=.../dxvk/legacy   WINEHUA_DXVK_VERSION=1.10.3
VK_ICD_FILENAMES=.../venus_icd.aarch64.json
WINEHUA_VULKAN_LOADER_ARCH=aarch64  WINEHUA_WINE_UNIX_ARCH=aarch64
WINEHUA_PERF_PROFILE=shadow-precise-dirty-ring-inline-upload-coverage-sort
```

**但它没有进入渲染阶段**（观察 ~6 分钟）：

| 观测点 | 结果 |
| --- | --- |
| 进程 CPU 时间 | 6 分钟只用了 **4 秒** —— 大部分时间在阻塞，不是在算 |
| 游戏 stderr | 停在 `[SMC]` 条目上不再前进；没有任何 wanewayland / 显示驱动活动 |
| 宿主 FPS 文件 | `winehua_display_fps.txt` 一直是 smoke 那轮的旧值 `169 77.941 3`，**没有新的 toplevel 发布** |

也就是说：**这一条现在不是"测量方法不够"，而是"目标游戏在这个构建上没跑到能测的状态"。**
两者要分开，前者是我的工具问题，后者是真实的兼容性/启动问题。

> 顺带记录：该游戏的 stderr 里累计了 **857 条 `[SMC]` 记录**（Unity IL2CPP/Burst 的
> 自修改代码），与方案 §11/交接文档提到的 SMC/信号税吻合。但 4 秒 CPU 说明它并不是
> 被 SMC 拖慢，而是**卡在渲染之前**，需要单独诊断（隐藏模态对话框 / 缺依赖 / 初始化路径）。

### 2.2 这一步的下一步

1. 诊断 Unity 卡在哪：抓 `[SMC]` 之后到阻塞点的调用栈，或用 `GameHook` 的点击自动化
   （`winehua.click_title_prefix` / `click_button_text`）排除"首启模态框等输入"。
2. 换一个更容易到渲染的目标（设备上还有 `games/SA/Game.exe`、DX SDK 样例集
   `games/dx11_test/`），先拿到**任意真实 D3D11 应用**的显示帧率，
   把"真实应用 FPS 可读"这条打通，再回到 Unity 目标游戏。

### 2.3 并入原工作树的其余未提交改动（关键）

排查过程中发现：parity 分支此前**只并入了 6 处未提交改动中的 1 处**
（`wine_child.cpp`），而剩下 5 处恰好就是"真实游戏测量"需要的全部工具，
外加两个子模块补丁文件。已全部并入（`git apply` 自原工作树 diff，未改动原工作树）：

| 文件 | 内容 |
| --- | --- |
| `entry/src/main/ets/game/GameHook.ets` | `winehua.perf_diagnosis` / `winehua.run_id` / **`winehua.wow64_engine=box\|fex`**；D3D env 白名单加 `FEX_` 前缀；perf 模式注入 `FEX_PROFILESTATS` 等 |
| `entry/src/main/cpp/graphics/venus_surface_presenter.cpp` | present 日志增加 **帧间隔分位 `frame_gap_us_p50/p95/p99`** |
| `entry/src/main/ets/service/WineEnvService.ets` | perf 档位可由 GameHook 覆盖（诊断时切到 frame-timeline） |
| `scripts/build_ohos_guest_vulkan.sh` + `patches/mesa-ohos-wow64-map-fd.patch` | Venus WoW64 backing-fd 生命周期修复 |
| `scripts/build_wine.sh` + `patches/wine-wow64-shared-map.patch` | Wine WoW64 shared-map 修复 |

**这说明之前"真实游戏测不到"的一部分原因是我们的树本身就不完整**，不是游戏或平台的问题。
（这两个补丁只被构建脚本引用，**尚未真正生效** —— 生效需要重编 wine 与 guest Mesa，
那是长构建，留给下一轮。）

### 2.4 真实游戏帧率：已经能读到

并入后用同一条 Want 通道启动 `Z:\games\SA\Game.exe`，带 `perf=1`：

```text
game want armed path=Z:\games\SA\Game.exe ... perf=1 run=p6game wow64=fex
```

宿主合成器的 FPS 文件给出真实游戏帧率：

```text
50.268 / 44.322 / 56.383 / 47.987 / 45.832   (toplevel 3, wow64=fex)
37.446 / 39.107 / 39.957 / 44.012 / 41.567   (toplevel 1, wow64=box)
```

**但这两组还不能直接对比**：`winehua_display_fps.txt` 只有一行、记录的是"最后被合成的那个
toplevel"，而两次运行报的 toplevel id 不同（3 vs 1）。`WL-STAT` 显示游戏起来后
toplevels 从 3 涨到 7，游戏确实建了窗口（`#10 520x411`、`#2 1280x20`）。

**所以下一步很明确：把 FPS 采样绑定到游戏自己的 toplevel id**（或让它成为唯一前台
toplevel），再做 fex/box 的 A/B。**现在离"目标游戏性能有可解释结果"只差这一步。**

### 2.5 本轮补到的三条实测事实

**（a）宿主诊断档位在 virgl 子进程启动时就固化 —— 要在冷启动带参数。**
第二次 Want 里传 `winehua.perf_diagnosis=1` 不会改已有 virgl 子进程的 profile，
所以必须 `aa force-stop` 后，把 `winehua.mode=game` 与诊断参数一起交给**冷启动**：

```bash
aa force-stop app.hackeris.winehua
aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode game --ps winehua.game_path "<path>" \
  --ps winehua.perf_diagnosis 1 --ps winehua.run_id <id> --ps winehua.wow64_engine fex
```

**（b）`winehua_display_fps.txt` 不是 surface-bound 指标。**
宿主合成器每次只写"最后被合成的那个 toplevel"一行（`<seq> <fps> <toplevelId>`），
而 toplevel id 会随窗口创建顺序变化，所以跨会话拿到的 id 不同（我们观测到 3 / 1），
**不能拿它直接做 A/B**。要做有效对比，必须绑定到游戏自己的 surface/toplevel。

**（c）交接文档的"第一优先级"假设（WoW64 整块复制）在当前运行里没有复现。**
部署的运行时里带 `[WOW64-MAP-PERF]` 探针（逐秒输出提交数、copy 字节/耗时/锁等待、
direct/alias/copy/reused/failed 计数）。已观测到的全部条目都是：

```text
[WOW64-MAP-PERF] pid=10605 interval_ns=1085325000 submits=6
  active_copy_maps=0 active_copy_bytes=0 copy_ops=0 copy_bytes=0 copy_ns=1562
  lock_wait_ns=1042 max_copy_ns=521
  direct_total=0 alias_total=0 copy_total=0 reused_total=0 failed_total=0
```

即 **`copy_total=0` / `copy_bytes=0` / `failed_total=0`** —— 没有落到 CPU 副本回退。
这与交接文档里"低地址共享映射失败 → 每次 submit 整块复制"的成本模型**不符**，
至少在目前的样本上没有发生。后续要看这个假设是否只对特定游戏成立。

**（d）真实应用的启动分成两类。** 同一 Want 通道下：

| 目标 | 结果 |
| --- | --- |
| `games/SA/Game.exe` | 建了窗口（`WL-STAT` toplevels 3→7），117–124% CPU，合成器 FPS ≈41–56 |
| `games/kqcs/LustFromTheDeep.exe`（Unity x64） | **没建窗口**（toplevels 保持 3），6 分钟只用 4 秒 CPU，卡在渲染前 |
| `games/dx11_test/.../BasicHLSL11.exe`（D3D11 样例） | **没建窗口**，几秒后进程退出 |

所以"真实游戏性能"这条要落地，先得解决"哪些应用能起到窗口/渲染"这一类问题，
而不是继续调测量口径。

### 2.6 真实应用的 SMC / 信号链开销（有量化）

真实应用跑起来后，wine stderr 里会出现密集的 `[SMC]` 记录（自修改代码 / 保护页
故障走 WineHua 的 sigchain）。本节会话（含 BasicHLSL11 与随后的 Steam 启动）里：

```text
grep -c 'SMC'              -> 2034 行
grep -c 'result=not_mine'  -> 664 次（决定"不是我处理的"，转交 Wine SEH）
```

每种故障的形态都是：

```text
[SMC] enter tid=12810 sig=7 addr=0x1e40de pc_in=0x7ff57ea608 teb=0x88000 fn=0
[SMC] tid=12810 sig=7 addr=0x1e40de ... result=wine_seh pc_out=... wine=1
[SMC] tid=12810 sig=7 addr=0x1e40de ... result=not_mine  ...
```

即**每次故障都要走完整的 sigchain 判定 + 每条 2 行 `write(2)` 日志**。
交接文档把"信号/SMC 税"列为主要嫌疑之一，这里给出的是真实应用上的量级样本
（**注意：这些日志本身也计入开销**，所以"关掉/限流 SMC 日志再看帧率"是一个值得做的对照）。

顺带记录一个干扰项：本节会话里应用**自行启动了 Steam**
（`cmd.exe /s/c Z:\games\Steam\1.bat` → `start.exe steam.exe -nocrashmonitor ...`，
以及 `Z:\games\Steam\steam.exe`）。做真实游戏测帧率时必须先确认没有这类
后台启动在抢 CPU，否则数据不可比。

### 已经可以下的性能结论（有证据）

| 结论 | 证据 |
| --- | --- |
| CPU 转译器不是瓶颈 | 原生 ARM64 12.159 ms vs 真 AMD64+FEX 12.153 ms（差 0.006 ms）；x86 FEX vs wowbox64 差 0.14 ms |
| 转译工作量极小 | cube 的 `renderMs`（全部 D3D11 调用 + Draw + 变换 + 上传）只有 0.09–0.17 ms |
| 剩下的是公共图形成本 | 宿主 present 只有 2.97 ms（有余量）；DXVK/Venus 比 D3D9/WineD3D 每帧多约 1.0 ms，因而错过 90 Hz 落到 ~81 fps |
| 构建参数对齐不提速 | Release + profiler + TUNE_CPU=none 对齐前后帧时间同噪声范围（`fex-build-parity.md`） |
| render 侧余量极大 | 每帧绘制量 ×4（K=1→K=4）后 `renderMs` 只动 0.01–0.02 ms，帧时间不动（`p3-p4-smoke-ab.md` §3.6） |
| 离屏方案在本运行时不可用 | 不 Present 时无消费者，`Flush` 会灌满 vtest ring，第 60 帧卡死（同样记录在 §3.6） |

## 3. 判定

**P5 判定：ARM Runtime 达到"功能对齐、可回退"的状态，但不建议现在切换产品默认后端。**

理由：

1. 功能面已经闭合：UnixLib 加载、SHM 统计可读、x64+ARM64X 图形链路、FEX 构建参数对齐，
   都有真机证据；正确性、稳定性、配置、回退路径四条的验证也都通过。
2. 唯一缺的是"目标游戏性能有可解释结果"。在当前测量手段下这一条**无法**被满足，
   而不是"结果不好"——所以不能拿现有数据宣称对齐完成或未完成。
3. 现有数据显示的 ~1 ms DXVK/Venus 超预算问题**对所有配置一视同仁**，
   不是 Proton 对齐引入的；把它记成独立课题更合适。

## 4. 回退策略（都已验证可执行）

| 层级 | 回退动作 | 影响面 |
| --- | --- | --- |
| 整个实验 | 丢弃 `feature/proton-arm64-parity` 分支（产品 `feature/arm64-heaven-port` 从未被修改） | 零 |
| 运行时后端 | `WINEHUA_WOW64_ENGINE=box` 把 32 位切回 wowbox64（真机已验证） | 单个启动会话 |
| FEX 构建 | `FEX_BUILD_TYPE=RelWithDebInfo FEX_PROFILER=False` 回到旧基线参数 | 下次出包 |
| FEX 补丁 | `scripts/build_fex.sh` 的 `FEX_PATCHES` 去掉 `fex-unixlib-backport.patch` | 下次出包 |
| 生效配置 | 删除 `FEX_Config.json` → FEX 回落源码默认值（`maxInst=5000` 等） | 下次出包 |
| 探针 | `FEX_UNIXLIB_PROBE` 默认关闭；`presenter_common.h` 的 `kForcedOn` 已为 `false` | 已恢复 |

## 5. 要做到"目标游戏性能有可解释结果"，下一步需要什么

1. **一个不吃显示节拍的负载**：cube 加离屏渲染模式（渲染到 RTV、不 Present），
   或每帧多次提交；用它做后端排名。当前 `--bench` 已经解决了"被 `Sleep(1)` 掩盖"
   的问题，但还没解决"被屏幕节拍掩盖"。
2. 在 1) 的基础上重跑 x86 两后端 + x64 两形态，给出**余量倍数**而不是 fps。
3. 有条件时在目标游戏（Unity x64 / Heaven x86）上做同样口径的对照。

## 6. 分支交付物清单

P0/P1/§12 的文档见 `README.md`；P2–P5 的证据文档：
`p2-release-probe.md`、`p2-unixlib-build.md`、`p2-device-validation.md`、
`p2-effective-config.md`、`p3-p4-smoke-ab.md`、`fex-build-parity.md`、本文。

实验工具（不进产品构建路径）：
`tools/build_bench_cubes.sh`、`tools/patch_hap_payload.py`、`tools/stage_runtime_fex.py`、
`tools/repack_hap_unixlib.py`；探针补丁 `scripts/patches/fex-unixlib-probe.patch`。

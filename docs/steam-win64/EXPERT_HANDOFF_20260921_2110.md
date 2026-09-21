# 32 位游戏（PAL2 / PAL4 / Heaven）与 Steam 显示链：现状、证据与判断（2026-09-21，截点 21:10 CST）

> 后续交接：[2026-09-21 23:25，v33 实机与新增证据](F:/WineHua/proton-ohos-worktree/docs/steam-win64/EXPERT_HANDOFF_20260921_2325.md)。本文保留为历史记录；PAL4 保护层归因、PAL2 显示链结案、Vulkan DLL 改名方案及当前装机版本，均以新文档更正为准。

本文接续 [2026-09-21 0940 交接](F:/WineHua/proton-ohos-worktree/docs/steam-win64/EXPERT_HANDOFF_20260921_0940.md)。冲突以本文的新实测为准。本轮集中在**万花（app.hackeris.winehua）应用**里跑 `Z:\home\*` 下的老游戏，以及 Steam 的显示链回归。

## 0. 接手先看

**当前主要问题（按优先级）**

| # | 问题 | 现状 | 性质 |
| --- | --- | --- | --- |
| 1 | **PAL2**：游戏在跑、按键可用，但画面要么黑屏，要么停在静态帧（黑底 + 一条白带） | 显示链已修好一半（窗口绑定从 `none` 变 `bound`），但**进程在自旋**（`State=R`，system time 持续增长） | 游戏/32 位运行时侧 |
| 2 | **PAL4**：完全白屏，游戏没跑起来 | 窗口只有初始白帧（`SERIAL 2`）；进程循环读 `PROT_NONE` 地址 `0x2280000`，guest RIP `0x667fd3`（pal4.exe 内） | 游戏侧（疑似自带保护驱动 `PAL4P.drv`） |
| 3 | **Heaven（DX11，32 位）** | 直接跑 `Heaven.exe` 会报 `can't open bin\unigine.cfg`；用 `data\heaven_4.0.cfg` 生成后能起、DXVK 能初始化，但随后仍随 32 位故障路径退出 | 与 PAL2/PAL4 同一类 32 位问题 |
| 4 | **Steam 显示链回归** | v24 后 V24 安装、运行时重解压、拉起 Steam 都正常，`gpu/renderer EXIT = 0`；但截点前 CEF 还没起来（bootstrap 慢），**需要补一次完整回归** | 待确认 |

**两条已经确认修好的问题**

1. **Steam CEF GPU 进程崩溃循环**（25 次 spawn/exit）→ 根因是 `cef.win64\vulkan-1.dll` 遮蔽 Wine 的 winevulkan 加载器；已提交修复（`179507a`：env 加 `vulkan-1=b` + 会话启动自愈改名遮蔽文件）。修复后 GPU 进程稳定 >110 秒、DXVK 日志干净、UI 正常渲染。
2. **PAL2 黑屏的显示链部分**：游戏帧全部提交在 640x480 的 **subsurface** 上（serial 4245，与 GL present 桥 `readbacks` 同步），而合成器合成的是 1280x800 的 toplevel（serial 2）；`UpdateSubsurfaceOnCommit()` 只认“直接父级带 toplevel 角色”，于是整段丢弃。已改为**沿父链向上找最近 toplevel 祖先**（`entry/src/main/cpp/compositor/wl_core.cpp`，未提交）。修复后 `VIRGL-ZC query tl=1 count=0 → 1`，PAL2 窗口 `binding=none → binding=bound contentSource=VenusNative extent=640x480`。

## 1. 设备、版本与产物

| 项目 | 值 |
| --- | --- |
| 设备 target | `192.168.180.71:36875`（另有 USB target，命令仍须显式 `-t`） |
| 截点 | 2026-09-21 21:10:48 CST，`/data` 剩 61 G |
| 已装 HAP | **v24**（本轮 app 库 + v23 运行时 payload） |
| 设备 FEX | `libarm64ecfex.dll` = `a9b75e5e22a83779b2c65f2a6c7f095fe2eeb37778dc3dbc08ca77d2796e5ee3`（**仍是 v21 返回缓存绕过诊断版**，不是产品 FEX） |
| 设备 box64 | `wowbox64.dll` = `369250bc064a15e36a34ab674388fba884d9f211929188fa3739e9bfac701bcd`（v23，JIT 故障一律 epilog） |
| 本轮证据目录 | `F:\WineHua\device-evidence-20260921-v21` |

本轮产出的候选（都在 `artifacts/fex-rwx-probe/`，本地另有副本在 `F:\WineHua\artifacts\`）：

| 候选 | 内容 | signed HAP SHA256 |
| --- | --- | --- |
| v22 | box64「同址只认领一次」的 JIT→epilog（有界） | `4c073f5f67d87449bad63a72e98346980ec628273a047e6772e825941917c779` |
| v23 | box64「JIT 内故障一律 epilog」（对齐 arm64 参考） | `f345cbddc1c8f1709861e4c6c96b65ba48e40c35d7ff403ad2014dc14e5349c2` |
| **v24（当前装机）** | 本轮重建的 app 库（含 `wl_core.cpp` 修复）+ v23 运行时 payload（`payloadSha256=cb409d6ef83623d7d4ad5634324b889d1f767f89bb1b2caeef3d5532b8db84d6`） | `97d5f32737cf121f969ed4e04b4f055c7e5d7dd2bd5a86610deb47500730e881` |

设备上还有两处手工状态（**接手必读**）：

- registry：`HKCU\Software\Wine\DllOverrides` 里 `vulkan-1 = builtin`（为压 CEF loader 遮蔽手工加的；产品化落点是已提交的 `wine_env.cpp`）。
- `.../Steam/bin/cef/cef.win64/` 下同时存在 `vulkan-1.dll`（Steam 自检会恢复）、`vulkan-1.dll.cefshadow`、`vulkan-1.dll.shadow2`、`vulkan-1.dll.winehua-shadow`（后者是 v24 应用自愈生成的）。

## 2. 32 位运行时：与 arm64 参考分支的差异（重要）

对照 `feature/arm64-heaven-port`（用户确认该分支下这批游戏能跑）：

- `WINEDLLDIR0..5` / `WINEDLLOVERRIDES` / `WINEDLLPATH` **完全一致**（我们只多 `vulkan-1=b`）→ **不是“DLL 没覆盖全”**。
- 唯一实质差异是 **32 位引擎默认值**：

| | arm64 参考分支 | 我们分支 |
| --- | --- | --- |
| `HODLL64`（64 位） | `libarm64ecfex.dll` | `libarm64ecfex.dll` |
| **`HODLL`（32 位）默认** | **`libwow64fex.dll`（FEX）**，`WINEHUA_WOW64_ENGINE=box` 才切 box64 | **`wowbox64.dll`（box64）**；`steamapps\common` 下的游戏被**强制** box64 |

- box64 子模块：我们 `a8e7ed5c`（9-19，codex）是参考版 `16515448b`（9-5，hackeris）的**后继**；两者只差 `wine/wow64/wowbox64.c` 一处——参考版把「JIT PC 但不是 SMC」的故障也送去 `native_epilog`，我们当时为压 32 位 CEF 的 SIGSEGV 死循环删掉了，只保留 SIGILL 分支。
- `16515448b` 与本文档同源的 `docs/ARM64_SCHEME3_HEAVEN_CRASH_FIX.md` 明确列出方案③的四处修复（sigchain 认领 SIGSEGV、TEB trampoline + unix mprotect、JIT SIGILL→native_epilog、**32 位 FS 基址 `GetSegmentBase→calculate_fs()`**）。我们这棵树里：sigchain 相关代码在（`ohos_install_sigchain`/`AddSpecialSignalHandlerFn`）、box64 的 FS 修复在（`src/os/os_wine.c` 已返回 `calculate_fs()`）、WOW64 `vkMapMemory` 32 位别名映射在（`wine-valve/dlls/win32u/vulkan.c` 有 `WineHua: WOW64 aliased vkMapMemory`）；**只有 epilog 那处被我们改窄过**，本轮已按参考恢复（v22 有界 → v23 无界）。

**实测两条引擎都试过**：`WINEHUA_WOW64_ENGINE=fex` 时 PAL4 直接 `exit=11`；默认 box64 时 PAL4 在同一地址死循环。PAL2 在 box64 下能出画面（用户手动 x86 smoke 在 virgl / dxvk 下都通过）。

## 3. 关键证据（可复查）

### 3.1 PAL2：帧在 subsurface 上，合成器只认 toplevel

```
修复前（v23）:
WINDOW-REG: ownerHostPid=16137 wlSurfaceId=3  toplevelId=4 role=toplevel  geometry=1280x800 fullscreen=1 serial=2
WINDOW-REG: ownerHostPid=16137 wlSurfaceId=26 toplevelId=0 role=subsurface geometry=640x480 visible=0 serial=4245
STEAM-WINDOW: window=(16137,3) binding=none producer=0x0 contentSource=SHM shmFrame=1 class=SHM
winehua_gl_present_bridge: readbacks=3120 pid=16137 surface=24 mapped=yes   ← 帧确实在出

修复后（v24）:
VIRGL-ZC[MAIN][DIAG] query tl=1 count=0 → count=1
STEAM-WINDOW: window=(16137,3) toplevel=4 geometry=1280x800 visible=1 binding=bound producer=0x3f09…0018 extent=640x480 contentSource=VenusNative producerAgeMs=0
```

修复后画面仍是**静态黑底 + 一条白带**（两次截图 SHA256 相同），且 `Pal2.exe` `State=R`、`utime/stime=5849/21808` 并持续增长 → **游戏侧自旋**，不是显示链。

### 3.2 PAL4：游戏侧在 PROT_NONE 上死循环

```
[SMC] enter pid=41989 tid=41989 sig=11 code=2 addr=0x2280000 pc_in=0x10a853fd4 x27_in=0x667fd3 … prot=--- 
[SMC] pid=41989 … kind=1 result=epilog wine=0        (v23，box64 认领后无限重试)
dispatch_exception code=c0000005 info[0]=0 info[1]=0x02280000 pc=0x10A72E314 (v22，交给 Wine SEH 后同样循环)
```

帧 dump：`frame-w806-h625-own60276-3.raw` = 标题栏 + 白底，`SERIAL 2` → 游戏从未画出内容。PAL4 目录含 `PAL4P.dll` / **`PAL4P.drv`** / `Pal4ExtendP.dll`（内核态保护组件），与“保护层在 Wine 下建不起映射区”一致。

### 3.3 Steam：GPU 进程崩溃循环的根因与修复

```
CEF-UTILITY-COUNT key=gpu-process spawned=25 exited=25
CEF-UTILITY-EXIT  … lifetimeMs=1658 signal=11 / 2386 signal=1 / 2541 signal=1
蒸汽侧: eglInitialize D3D11 failed (No available renderers) / SwANGLE Internal Vulkan error (-3)
        DxvkInstance::createInstance: Failed to create Vulkan 1.1 instance
根因: cef.win64 自带 vulkan-1.dll 遮蔽 winevulkan；venus ICD 是 OHOS .so，只有 winevulkan 能解析
修复后: gpu-process 存活 >110 秒、EXIT=0、DXVK 日志仅 MakeWindowAssociation、UI 正常渲染
```

注意：**Steam 的 bootstrap 自检每次启动都会把 `cef.win64\vulkan-1.dll` 恢复回来**（本轮被恢复过两次），所以真正兜住的是 env/registry 的 `vulkan-1=b`，文件改名只是辅助；v24 里应用自愈已生效（生成 `.winehua-shadow`）。

### 3.4 诊断方法论（本轮踩过的坑）

- **宿主报的 `exit=11` 不是失败判据**：x64 的 d3d11/d3d8/vulkan/GL smoke 都会在 `[EXIT-PROBE] phase=libc-exit status=0` 之后被宿主报 11（子进程退出阶段崩溃）。判 smoke 结果要看它自己的输出/JSON，而不是退出码。
- **`uitest uiInput keyEvent`（Enter=2054）到不了应用窗口**：系统日志有 `system_ui_controller InjectKeyEventSequence`，但应用侧没有 `[Input] KEY` 行 → 不能拿它验证游戏键盘；键盘验证要用物理键盘或应用自己的注入路径。
- **PAL4 的自旋会把 `wine_stderr` 写到 30 GB**（本轮实测 30,482,695,575 字节，分区余量一度掉到 34 G）。已把该日志清零（现回到 61 G）。ntdll 里 `[SMC]` 的限流（>64 后每 32 条）是有的，30 GB 来自别的路径，尚未定位。
- **设备会锁屏**：锁屏时 `aa start` 返回 `Error Code:10106102 … device screen is locked`；需要先解锁（本轮用 `uitest uiInput swipe` 解开过）。
- 游戏路径（万花 app 的 `Z:\`）：PAL2 `Z:\home\pal2\pal2\Pal2.exe`；PAL4 `Z:\home\PAL4\PAL4\pal4.exe`；Heaven `Z:\home\HEAV~PVM.0\HEAV~PVM.0\bin\Heaven.exe`（**应经 `heaven.bat` → `browser_x86.exe -config ../data/launcher/launcher.xml` 启动**；直接跑引擎需要 `bin\unigine.cfg`，本轮已用 `data\heaven_4.0.cfg` 生成过一份）。

## 4. 本轮代码改动与提交状态

已提交：

- `179507a fix(ohos): 消除 CEF 自带 vulkan-1.dll 对 Wine 加载器的遮蔽`（`wine_env.cpp` 两处 `WINEDLLOVERRIDES` 加 `vulkan-1=b`；`wine_child.cpp` 新增 `repair_shadowing_vulkan_loaders()` 并在会话启动调用）。
- 之前的 `a8f0067` / `8d74aae` / `a76431c` / `91bc0ac` / `4f02d30` / `47f7ecf` / `959de49`（v21 构建/校验脚本、窗口枚举器、0940 交接、显示链修复、Wine/FEX 补丁与工具落库）。

**未提交（接手第一件事）**：

| 文件 | 内容 |
| --- | --- |
| `entry/src/main/cpp/compositor/wl_core.cpp` | PAL2 修复：`UpdateSubsurfaceOnCommit()` 沿 `parentSurface` 链向上找最近带 toplevel 角色的祖先（原来是只看直接父级） |
| `thirdparty/box64/wine/wow64/wowbox64.c` | 恢复参考分支规则：JIT 内故障（含 SIGSEGV）一律 `dynablock_leave_runtime → native_epilog` |
| `scripts/package_payload_entry.py`（新） | 只替换 payload 单文件；`--payload-from` 可整体换 payload（app 库变更但保留设备上运行时） |

## 5. 我的判断

1. **显示链的两个问题已经结案**：PAL2 的 subsurface 归属（本轮修好、v24 验证绑定成功）与 Steam 的 vulkan-1 遮蔽（`179507a`）。剩下的“画面不动/不铺满”是**合成策略**问题（游戏内容 640x480 提交、窗口 1280x800，需要决定缩放/信箱策略），不是绑定问题。
2. **PAL2/PAL4 剩余症状都在游戏侧，且强烈指向自保护层**：两者的进程都在自旋、都不产生 Present，且两者都带保护组件（PAL2 目录有 `First.exe`/`Taiwan.dll` 等；PAL4 有 `PAL4P.drv`）。PAL2 在 box64 下“能出标题画面 + 按键可用”，说明 32 位图形与输入链路本身是通的，问题在特定游戏代码路径。
3. **32 位引擎默认值与 arm64 参考不一致，是最值得先做的单变量实验**：把 32 位默认切到 FEX（`WINEHUA_WOW64_ENGINE=fex` 等价于参考分支默认），配合已恢复的 epilog 规则，做 PAL2/PAL4 的 A/B。我们分支当年改默认到 box64 的理由（“FEX 下 32 位游戏秒崩”）来自更早的 FEX/wow64fex 构建，未必还成立。
4. **Steam 链路目前看起来没被本轮改动破坏**（安装/解包/拉起正常、GPU/renderer 退出 0、自愈生效），但 CEF 起来得慢，**需要在下一轮补一次完整回归**（GPU 进程存活 + UI 可达 + 库操作）。

## 6. 建议的接手顺序

1. **先提交上表三个文件**（改动都很小，且 v24 已上机验证过绑定效果）。
2. **补 Steam 回归**：等 CEF 起来 → 看 `CEF-UTILITY-COUNT key=gpu-process` 的 spawned/exited、`steamwebhelper_dxgi.log` 是否仍报 Vulkan 实例失败、UI 是否能到库界面。
3. **32 位引擎 A/B**：同一 PAL2/PAL4，分别用 `WINEHUA_WOW64_ENGINE=fex`（= arm64 参考默认）与默认 box64，比较首发故障（自旋点/Present 是否发生/画面是否出现）。这一项最可能直接解释“arm64 分支能跑”。
4. **PAL2/PAL4 循环点取证**：PAL2 已能拿到 guest RIP（`x27_out`），用 `[SMC-MAP]`/fault-maps 定位到模块+偏移；PAL4 起点 `0x667fd3`。若证实落在保护组件，走“驱动兼容”或“无保护版本对照”。
5. **合成策略**：决定游戏内容（640x480）在 1280x800 窗口里的呈现方式（等比放大 / 信箱 / 保持 1:1），这是显式产品决策，别顺手改。
6. **顺手修的**：定位 30 GB 日志的来源并限流（不是 `[SMC]` 那两条）；把 `scripts/package_payload_entry.py` 的两种模式写进构建说明。

## 7. 本轮验证过的事实清单（供快速核对）

- x64 Vulkan smoke：**PASS**（JSON status=PASS，device=Virtio-GPU Venus (Maleoon 910)，30 帧 present）。
- x64 OpenGL smoke：**PASS**（`winehua_gl_present_bridge: readbacks=1 pid=… mapped=yes`）。
- x64/x86 D3D11（DXVK）smoke：DXVK 正常初始化（`DXVK: Creating new state cache file`），无 Vulkan 实例失败。
- 用户手动验证：x86 smoke 在 virgl 与 DXVK 下均可。
- PAL2（v24）：`VIRGL-ZC query tl=1 count=1`；窗口 `binding=bound / VenusNative / extent=640x480 / producerAgeMs=0`；GL 桥 `readbacks` 持续增长；但画面静态、进程自旋。
- PAL4（v23/v24）：窗口 `SERIAL 2` 白底；进程在 `addr=0x2280000 / x27=0x667fd3` 循环。
- Steam（v24）：安装成功、payload 重解压、`steam.exe` 拉起；`gpu/renderer EXIT = 0`；`steamwebhelper_dxgi.log` 里的失败行是 18:12 旧内容。
- 设备余量：61 G（曾因 30 GB 日志掉到 34 G，已清理）。

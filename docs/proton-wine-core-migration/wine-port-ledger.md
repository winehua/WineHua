# Wine 移植台账（W0）

> 目的：给 WineHua 在 Wine 上的**每一组自有改动**一个明确决策，避免 W1 时"凭印象挑补丁"。
> 分类取值：`NEED_PORT` / `PREFER_UPSTREAM` / `AUDIT_AND_PORT` / `DROP_IF_REDUNDANT` / `REVALIDATE` / `REVALIDATE_LAST`
> 复现：`tools/wine-delta-audit.sh`（提交与文件清单由它产出）

## 0. 分类定义（本台账采用）

| 分类 | 含义 |
| --- | --- |
| `NEED_PORT` | OHOS 平台独有，Valve 树完全没有，必须写到新基线上 |
| `PREFER_UPSTREAM` | 优先用新基线的上游实现，只有确认缺失才自己写 |
| `AUDIT_AND_PORT` | 两边都有对应代码，必须先比对着改，不能整段覆盖 |
| `DROP_IF_REDUNDANT` | 调试/清理/临时产物，默认不迁 |
| `REVALIDATE` | 可能被新基线覆盖，必须在新基线上重跑复现后再决定 |
| `REVALIDATE_LAST` | 影响面小或属游戏特殊处理，放到最后再议 |

## 1. 总表

| ID | 分类 | 涉及文件（WineHua 树） | 原始目的 | Valve Proton Wine 对应 |
| --- | --- | --- | --- | --- |
| W-01 | `NEED_PORT` | `dlls/ntdll/unix/ohos_broker.{c,h}`、`unix/process.c`、`ntdll_misc.h`、`ntdll.spec` | OHOS 不能自行 fork/exec：子进程必须由主进程 broker 经 NCP 拉起；SPAWN 协议支持 `__env` 转发与命名多 fd | **无**（Valve 无 broker/NCP 概念） |
| W-02 | `NEED_PORT` | `server/{main.c,directory.c,process.c,winstation.c,mapping.c,object.h,Makefile.in}`、`server/musl_compat.c` | wineserver 生命周期：持久化、`WINESERVERSOCKET` 死连接回退、`start_server` 走 broker、`--no-auto-close` | 有 server 代码，但无 OHOS 生命周期改造 |
| W-03 | `NEED_PORT` | `ohos_file.{c,h}`、`unix/file.c`、`setupapi/{install.c,queue.c}`、`kernel32`、`mountmgr.sys/unixlib.c`、`shell32/shlfolder.c` | OHOS 无 `symlink()`：dosdevices 不可用时的四条回退、`unix_to_nt_file_name` 走 `\??\unix`、盘符枚举回退 | **无** |
| W-04 | `NEED_PORT` | `ohos_virtual.{c,h}`、`unix/virtual.c`、`unix/signal_{i386,x86_64}.c`、`signal_{i386,x86_64}.c`、`signal_arm64ec.c` | OHOS noexec 文件系统下的 JIT/prot:exec；Box64/FEX 的 SMC、CALLRET 信号必须**先于** Wine SEH/DFX 路由 | 有 signal 代码，无 OHOS 路由 |
| W-05 | `NEED_PORT` | `unix/loader.c`、`loader.c`、`unix/env.c`、`unix/server.c`、`unixlib.h`、`heap.c`、`ntdll/Makefile.in` | 原生 ARM64 Wine 适配：路径推算、DLL 搜索、env 传递、`#ifdef __OHOS__` 守卫 | 有对应文件但为 11.0，**漂移待抽样** |
| W-06 | `AUDIT_AND_PORT` | `dlls/wow64/{process.c,syscall.c,wow64.spec}`、`unix/virtual.c`、`loader.c` | `HODLL` 选择 32 位转译器（ARM64 默认 `libwow64fex.dll`）；4G 分配下限与高区探测策略；`vkMapMemory` 别名 | 有 wow64，**无 HODLL/FEX 策略**；必须逐行比 |
| W-07 | `NEED_PORT` | `loader.c`、`unix/loader.c` | ARM64X DXVK/VKD3D overlay 搜索顺序（arm64x → x64 → x86），mixed overlay 声明保持可移植 | **无** |
| W-08 | `AUDIT_AND_PORT` | `win32u/{opengl.c,vulkan.c,driver.c,dce.c,sysparams.c,window.c,Makefile.in}`、`win32u/opengl_diag.{c,h}` | 最小化窗口豁免 present_rect 替换；`WINEHUA_SIMULATE_RESOLUTION` 进程级模拟 CDS；Vulkan 诊断 | 有 win32u，**漂移待抽样** |
| W-09 | `AUDIT_AND_PORT` | `winewayland.drv/*`（8 个）+ `wayland_surface_ohos.{c,h}`、`winehua-toplevel.xml`、`modal.c`、`opengl_{diag,readback}.{c,h}` | OHOS Wayland surface 私有扩展、虚拟桌面坐标经 window_geometry 传递、app_id 后缀、相对模式校准、readback 重建 | 有 winewayland 基线，**无 OHOS 扩展** |
| W-10 | `NEED_PORT` | `dlls/wineohos.drv/*`（10 个）、`mmdevapi/client.c`、`mciqtz32/*` | OHOS 音频/MIDI 后端（走宿主 IPC）、waveOut 后端拆分、MP3 解码 | **无** |
| W-11 | `NEED_PORT` | `winebus.sys/{bus_ohos.c,bus_sdl.c}`、`kernel32/thread.c` | OHOS 手柄总线；Controller Hub 需显式开启（默认恢复 Wine 原行为） | **无** |
| W-12 | `NEED_PORT` | `win32u/freetype.c` | OHOS 无 fontconfig 时加载 `/system/fonts/` | 有 freetype.c，**无 OHOS 字体源** |
| W-13 | `NEED_PORT` | `dnsapi/libresolv_musl.c`、`dnsapi/Makefile.in` | musl 下无 `libresolv` 的内置解析回退 | **无** |
| W-14 | `REVALIDATE` | `ntdll/version.c` | 移植 CrossOver 的 `WINEHUA_WINDOWS_VERSION` 环境变量补丁 | 可能已被 Proton 的 Windows 版本处理覆盖 |
| W-15 | `REVALIDATE_LAST` | `po/zh_CN.po`、`shell32/shlfolder.c` | explorer 菜单翻译修正、desktop.ini CLSID 解析失败回退 | 有对应文件，属细节 |
| W-16 | `NEED_PORT` | `programs/winehua_keep/*` | 桌面保活辅助进程，替代 `win32u`/server 的自动关闭 | **无** |
| W-17 | `REVALIDATE_LAST` | `programs/winehua_dinput_probe/*`、`programs/winehua_graphics_smoke/*` | 真机诊断探针 | **无**（纯工具，不进产品包也不影响迁移） |
| W-18 | `DROP_IF_REDUNDANT` | 多笔 `清理:` 提交、`#ifdef __OHOS__` 注释整理、`tools/makedep.c` | 临时调试日志（OHOS-DBG / BMP dump）、格式回退、CRLF 修正 | 不迁 |
| W-19 | `NEED_PORT` | `configure`、`configure.ac`、各 `Makefile.in`、`wineboot.c`、`include/winternl.h` | 把上面各模块接进构建；`wineboot` OHOS 兼容 | 需逐个重放（构建系统漂移风险最高） |
| W-20 | `PREFER_UPSTREAM` | （不在 wine 主树）FEX 侧 `Source/Windows/UnixLib/*`、`FEXUnixLib.*` | FEX 的 UnixLib 机制 | **优先用新基线/上游**；我们已完成的回移继续保留在 FEX 层不重做 |
| W-21 | `AUDIT_AND_PORT` | `dlls/rpcrt4/ndr_stubless.c`（**parity 分支上的新修复**） | ARM64EC 手写 thunk 未跳过 32 字节 shadow space，导致 `OpenSCManagerW` 必崩 | Valve 树里这段**逐字相同** ⇒ 换基线不会修好，必须带着走 |
| W-22 | `REVALIDATE` | `programs/wineboot/wineboot.c` | 避免 legacy profile folder thunk | 需在新基线上重新复现 |

## 2. 逐项明细（依赖 / 风险 / 迁移方式 / 验收 / 回退）

| ID | 依赖 | 风险 | 迁移方式 | 验收 | 回退 |
| --- | --- | --- | --- | --- | --- |
| W-01 | NCP 接口、主进程 broker 实现（在 HAP 侧，不在 wine 树） | 高：注入点分散在 `unix/process.c`；NCP 16-FD 上限是硬边界 | 先整体拷 `ohos_broker.*`，再逐个重放 `unix/process.c` 的注入点 | `wineboot`/`cmd` 能经 broker 启动；子进程父子关系正确 | 去掉 broker 注入点，退回单进程（功能缺失但不崩） |
| W-02 | W-01 | 高：server 协议与 Wine 版本强绑定，禁止新旧混搭 | 以新基线 server 为底，重放生命周期改动；`musl_compat.c` 直接加 | 反复启动/退出 5 次稳定；wineserver 不残留 | 保留原 server，仅 `--no-auto-close` |
| W-03 | W-01 | 中：文件语义回退影响面广（安装/更新/重命名） | 整文件移植 `ohos_file.*`，逐个重放回退分支 | C:/Z:/临时目录的 open/stat/lstat/rename 行为矩阵通过 | 关闭回退，恢复上游路径解析 |
| W-04 | W-06（FEX/HODLL） | 高：信号路由顺序错了会掩盖真实崩溃 | 先移植 `ohos_virtual.*`，再按提交顺序重放 signal 改动 | 受控 SEH/保护页/SMC 回归；Box64 与 FEX 两种后端都不回归 | 关闭 SMC 前置路由，回到 Wine 默认 SEH |
| W-05 | — | 中高：`unix/loader.c`/`env.c` 是 11.10 改动密集区 | 逐个提交重放，`#ifdef __OHOS__` 守卫优先于改逻辑 | `wineboot` + 基础 exe 启动 | 单条提交回退 |
| W-06 | FEX 四产物 | 高：地址空间策略写错会导致随机崩溃 | **先审计**：把 Valve 11.0 的 wow64 与本地 11.10 对照，再决定改写哪几处 | x86/x64 两种 exe 都能起；4G 边界用例 | 去掉 HODLL 覆盖，回到默认后端 |
| W-07 | — | 中：搜索顺序影响 DLL 选型（DXVK/VKD3D） | 直接移植搜索顺序改动 | 真 AMD64 应用加载 `dxvk/legacy/arm64x/d3d11.dll` | 恢复上游搜索顺序 |
| W-08 | W-09 | 中：win32u 改动与上游漂移交织 | 逐个重放；`opengl_diag.*` 直接新增 | 窗口/弹窗/多窗口/最小化还原 | 单条回退 |
| W-09 | compositor（HAP 侧） | 中高：协议扩展两端必须同时改 | 先移植 `wayland_surface_ohos.*` + xml，再重放 window.c/wayland.c | 窗口上屏、几何/坐标、输入命中 | 关闭 OHOS 扩展，退回标准 xdg-shell 路径 |
| W-10 | 宿主音频 IPC（HAP 侧） | 中：`audio_ipc_protocol.h` 与宿主 ABI 绑定，两端要同步 | 整目录移植 + `mmdevapi`/`mciqtz32` 重放 | 播放/录音/MIDI 冒烟 | 去掉 `wineohos.drv`，退回 null 音频 |
| W-11 | 手柄服务（HAP 侧） | 中低 | 整目录移植 | 手柄识别与 dinput 通道 | 关闭 hub，回默认 Wine 行为 |
| W-12 | — | 低 | 直接移植 | 字体枚举非空、GDI 文本度量正常 | 恢复上游字体查找 |
| W-13 | — | 低 | 直接移植 | DNS 解析在无 libresolv 时可回退 | 恢复上游 dnsapi |
| W-14 | — | 中：可能重复实现 | **先在新基线上复现**再决定 | 特定应用 Windows 版本行为正确 | 丢弃该补丁 |
| W-15 | — | 低 | 最后处理 | 界面文本与行为正确 | 丢弃 |
| W-16 | — | 低 | 直接移植 | 桌面不自动关闭 | 丢弃 |
| W-17 | — | 低 | 可后置 | 探针可运行 | 丢弃 |
| W-18 | — | 无 | **不迁** | — | — |
| W-19 | 全部 | **最高**：构建系统是迁移失败的最常见原因 | 最后统一处理，逐个 `Makefile.in` 对照 | `make` 全绿 + 产物清单与旧基线一致 | 逐步回退到上一档 |
| W-20 | FEX 版本 | 中：不要把"回移组合"说成"与上游一致" | 保留现状，不重做 | UnixLib 四产物加载正常 | 保持现状 |
| W-21 | — | 中：ARM64EC thunk 是 ABI 级 | 直接带着走；并在新基线上重跑 `comprobe.exe` | `OpenSCManagerW` 不再崩；RPC 闭环 | 撤销该 patch |
| W-22 | — | 低 | 复现后再定 | `wineboot` 正常 | 丢弃 |

## 3. 三条硬约束（W1 不得违反）

1. **成套验收**：loader / wineserver / ntdll / win32u 必须来自**同一次构建**，
   禁止用新 wineserver 配旧 Wine DLL。
2. **不默认迁移**：临时 crash bypass、旧 FEX/旧 WoW64 workaround、Steam 专用 workaround、
   固定偏移 binary patch、游戏专用 patch —— 全部要在新基线上**重新复现**后再定。
3. **不动其它层**：FEX / DXVK / vkd3d / Mesa / virglrenderer / Host presenter 首轮固定，
   否则出问题无法归因。

## 4. 本台账的边界

- `BOTH` 的 61 个文件尚未逐个做内容级 diff；「风险」列是基于文件角色与提交语义的**判断**，不是实测。
- `W-06`（WoW64 地址空间）与 `W-19`（构建系统）是已知的两个高风险区，W1 应先做它们的小样验证。
- 本台账会随 W1 实测更新；任何"已迁"标记必须以 Gate 通过为准，不以"文件已拷过去"为准。

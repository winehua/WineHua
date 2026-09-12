# Proton Wine 本体迁移状态（R0）

> 日期：2026-09-12
> 依据：`WineHua_Steam_Next_Phase_2026-09-12.md` §7 R0 / §10 门禁
> 结论：**当前 Wine 不是指定 Valve Proton Wine 基线。「指定 Proton Wine 本体已迁入」= 未完成。**
> 本轮新增一手核验：此前锁定的 Valve Wine 提交 `local_available: false`，本轮已通过代理直连 GitHub
> 把该提交对象拉进本地探针仓库并逐项核对，R0 不再依赖推断。

## 1. 结论先行

| 问题 | 回答 | 证据 |
| --- | --- | --- |
| 我们的 Wine 是 `ValveSoftware/wine@dc26e618` 吗？ | **不是** | 两个工作树的 wine 对象库里 `git cat-file -e` 均返回 ABSENT（§3.2） |
| 那我们的 Wine 是什么？ | `winehua/wine` 的 **WineHQ 上游分叉**（叠加 OHOS 平台补丁），`VERSION = "Wine version 11.10"` | 最早提交 1993-06-29 Alexandre Julliard；fork 作者 59 个提交（§3.3） |
| 「关键接口回移」完成了吗？ | 完成（FEX UnixLib 回移 + 四产物 + 真机加载） | `p2-unixlib-build.md`、`p2-device-validation.md` |
| 这两件事是一回事吗？ | **不是** | 回移的是 FEX 侧的 UnixLib 接口，不是 Valve Wine 的源码树（§5） |
| R 线还需要做吗？ | 需要。最小迁移清单见 §6 | |

## 2. 参考基线复核（本会话实测）

### 2.1 Proton 参考提交与子模块 pin

`git ls-remote https://github.com/ValveSoftware/Proton HEAD` 返回
`5b89db940e0ebe3a137a6009a3589232fe084c09`，与既定参考一致。

对该提交读取 tree 得到的子模块 pin（`160000 commit`）：

| 子模块 | pin | 与既有锁定值（`proton-arm64-parity.lock.yaml`） |
| --- | --- | --- |
| `wine` | `dc26e61847081a1b5cb0733dc30feba6ee575482` | 一致 |
| `FEX` | `1cc4b93e7a71c883ec021b71359f136394dc1f3c` | 一致 |
| `dxvk` | `a6764047e587178283fcde4073ae6e1410af594f` | 一致 |
| `vkd3d-proton` | `212991fc2c266bc0d59f4c4ce8f80f7126508d71` | 一致 |

### 2.2 Valve Wine 参考提交的元数据（本轮首次可读）

```text
commit  dc26e61847081a1b5cb0733dc30feba6ee575482
author  Arkadiusz Hiler <ahiler@codeweavers.com>
date    Thu Jul 30 11:12:46 2026 +0300
subject winebus: Fix initial axis values for evdev gamepads.
VERSION Wine version 11.0
branch  该提交是 github.com/ValveSoftware/wine 分支 proton_11.0 的 tip
```

`git ls-remote --heads` 显示 `ValveSoftware/wine` 的 Proton 分支族为
`proton_3.7 … proton_9.0 / proton_10.0 / proton_11.0`（另有 `-rc`），
**不存在名为 `master` 的 Proton 交付分支**。

## 3. 我们的 Wine 实测身份

### 3.1 锁定值（两个工作树一致）

```text
repo        git@github.com:winehua/wine.git（origin）
HEAD        dc5204ecb0c3c1bf6624542ecf42f931fcc1a3b0
subject     fix(ntdll): search arm64x DXVK overlay before x64
author      hackeris <hackeris@qq.com>
date        Sun Sep  6 15:41:00 2026 +0800
VERSION     Wine version 11.10
describe    无 tag（本地未取 tag）
```

### 3.2 不存在 Valve 提交对象

```text
git -C WineHua-arm64ec/thirdparty/wine        cat-file -e dc26e618…  -> ABSENT
git -C WineHua-proton-parity/thirdparty/wine  cat-file -e dc26e618…  -> ABSENT
```

即：**不是「同一个仓库取了不同分支」，而是对象库层面就没有 Valve 那棵树。**

### 3.3 这是 WineHQ 分叉，不是 Proton 分叉

```text
git log --reverse --format='%h %ad %an %s' --date=short | head -3
  2c25c3e9442 1993-06-29 Alexandre Julliard Release 0.0.2
  066d1e09a45 1993-07-01 Alexandre Julliard Release 0.0.3
  121bd98c16b 1993-07-08 Alexandre Julliard Release 0.1.0
```

历史根是 WineHQ 的 1993 年提交；`origin/master` 上共有 **59 个 winehua 署名提交**
（`git log --author='hackeris\|winehua\|liufeng'`），其余为 WineHQ / CodeWeavers 上游提交。
`origin/master` 与本分支的 merge-base 是 `57bad82108f`（2026-08-14，winehua 提交
`secur32: schannel 禁用 CHACHA20-POLY1305 优先级`），本分支另有 **19 个独有提交**。

结论：**我们的 Wine = WineHQ 上游（11.10 开发线）+ OHOS 平台补丁 + ARM64/WoW64 补丁**，
与 Valve 的 Proton Wine（11.0 稳定线 + Proton 补丁集）是**两条不同谱系**。

## 4. OHOS 补丁面（R1 的输入）

本会话用两种口径统计，互相印证：

1. **命中 `ohos` 的提交**：`origin/master` 上 45 个（`git log -i --grep=ohos`）。
2. **新增文件（按名字可判定为 OHOS 专有）**：

```text
dlls/ntdll/unix/ohos_broker.{c,h}          # 进程 broker 客户端 / SPAWN 协议
dlls/ntdll/unix/ohos_file.{c,h}            # drive->unix 路径映射与文件语义回退
dlls/ntdll/unix/ohos_virtual.{c,h}         # noexec / JIT / sigchain / SMC
dlls/wineohos.drv/                         # OHOS 音频与 MIDI 后端（含 tml.h / tsf.h）
dlls/winewayland.drv/wayland_surface_ohos.{c,h}
dlls/winebus.sys/bus_ohos.c
dlls/ntdll/signal_arm64ec.c
dlls/msvcrt/except_arm64ec.c
libs/winecrt0/arm64ec.c
libs/compiler-rt/lib/builtins/arm64ec/chkstk.S
libs/symcrypt/lib/arm64ec/*.S
```

3. 被这些提交修改的**上游既有文件**共 89 个，主要集中在：

```text
dlls/ntdll/{loader.c,unix/*.c,heap.c}
dlls/win32u/{vulkan.c,opengl.c,driver.c,winstation.c,freetype.c}
dlls/winewayland.drv/{wayland.c,window.c,wayland_surface.c,waylanddrv_main.c}
server/{process.c,main.c,directory.c}
dlls/kernel32/{process.c,path.c,module.c}
programs/wineboot/wineboot.c
configure.ac / tools/makedep.c
```

这是 R1 需要迁移的**真实补丁面**（不是全量上游差异，只是本项目的 OHOS 改动）。

## 5. 不要把两件事混为一谈

| 事项 | 实测状态 | 归属 |
| --- | --- | --- |
| FEX 6 个 UnixLib 提交回移到 `86ff33bbe`，产出两个 AArch64 `.so` + 两个 PE DLL | 已完成，有构建哈希 | **FEX 接口回移** |
| UnixLib 在 MLR-AL10 上被真实加载 | 已完成（`p2-device-validation.md`） | **FEX 接口回移** |
| `FEX_Config.json`（Proton 参考值）随包部署并被读取 | 已完成 | **配置对齐** |
| 换成 `ValveSoftware/wine@dc26e618` 的源码树 | **未做**（对象库中不存在该提交） | **R 线本体迁移** |
| Wine loader / wineserver / ntdll / win32u 成套来自同一 Valve 基线 | **未做** | **R 线本体迁移** |

> 2026-09-12 设备侧复核补充：当前安装的运行时的 `bin/aarch64-unix/` 目录为**空**，
> 全盘也找不到 `libwow64fex.so` / `libarm64ecfex.so`。也就是说**此刻设备上跑的这份运行时
> 连 P2 的 UnixLib 产物都没有**（详见 `runtime-provenance.json` 与 `steam-first-blocker.md`）。
> 此前「UnixLib 已加载」的结论属于**当时的探针包**，不能自动继承给现在的设备状态。

## 6. R1：最小 OHOS 补丁迁移清单（建议）

目标：在不引入 Linux Steam 容器、不引入单进程全包方案的前提下，把上表的 OHOS 补丁
按需迁移到 `ValveSoftware/wine@dc26e618`（或 `proton_11.0` 最新 tip），并在同一架构下成套验收。

建议顺序（先能起、再对齐，每步单独留证据）：

1. **进程与装载**：`ohos_broker.{c,h}`、`loader.c` 的 HODLL/HODLL64 与 ARM64X overlay 搜索、
   `unix/loader.c`、`unix/process.c`、`unix/server.c`、`server/*`、`musl_compat`。
2. **内存与异常**：`ohos_virtual.{c,h}`、`unix/virtual.c`、`signal_arm64ec.c`、
   `signal_x86_64.c`、`msvcrt/except_arm64ec.c`。
3. **文件语义**：`ohos_file.{c,h}`、`unix/file.c`、`unix/env.c`、dosdevices 回退链、
   `/system/fonts/` 字体加载。
4. **窗口与输入**：`winewayland.drv` 全套（含 `wayland_surface_ohos.*`、
   `winehua-toplevel.xml`、`wayland_surface_update_min_max` 拆分）。
5. **音频**：`wineohos.drv`（`audio_ipc_protocol.h` 与宿主 ABI 已绑定）、
   `winebus.sys/bus_ohos.c`、`mmdevapi` 改动。
6. **winevulkan 边界**：`win32u/vulkan.c` + guest Venus ICD 接法。
7. **构建系统**：`configure.ac`、`tools/makedep.c`、`dlls/*/Makefile.in` 新增模块。

成套验收要求（沿用本轮方案 §7 R1/R2）：

- loader / wineserver / ntdll / win32u 必须是**相互匹配的一次构建产物**，不允许新旧混搭。
- 先重放现有 x86/x64 smoke，再走 Steam 启动/登录，最后才是目标游戏。
- 失败不覆盖已验证的现有运行时包。

## 7. FEX 侧状态（供 R1 控制变量）

```text
本地 HEAD      86ff33bbe299cd8959a6610198c169b67ec419db   FEX-2605-2-g86ff33bbe  2026-01-10
参考 pin       1cc4b93e7a71c883ec021b71359f136394dc1f3c   FEX-2607               2026-07-02
对象是否存在   存在（本地对象库里有 1cc4b93e7）
关系           divergent（不是祖先），merge-base = a04b0241c2fe3911729842205cd8643981108aad
               参考侧独有 321 个提交
工作树         脏：14 个已跟踪文件修改 + 未跟踪 Source/Windows/Common/FEXUnixLib.{cpp,h}、
               Source/Windows/UnixLib/{FEXUnixLib.cpp,FEXUnixLib.h,CMakeLists.txt}
```

回移补丁已落盘并记录哈希：

```text
scripts/patches/fex-unixlib-backport.patch   sha256 91e067a2a4c2558897c30911f33a9a2319d1866882e5a6740d92ad696e831498
scripts/patches/fex-unixlib-probe.patch      sha256 bdde138be5d089af4763458ee8b167dba49f8a3ea4aaccc43bf57426d89830bb
```

> 命名约束：这是「在 `86ff33bbe` 上回移参考侧提交」，**不得**表述为「已与上游逐提交一致」。

## 8. 未验证 / 待补

- 未取得 Valve Wine `dc26e618` 的完整源码树（本轮只拉取该提交对象做元数据与 pin 核对），
  因此没有做「winehua wine vs Valve Wine」的逐文件 diffstat。
- 未在设备上核对已安装 Wine 的 `ntdll.so` 等 Unix 侧产物哈希（沙箱内可读，宿主 shell 不可读 bundle）。
- 安装中的 HAP 文件 sha256 未能从 `hdc shell` 取得
  （`/data/app/el1/bundle/public/app.hackeris.winehua/` 对 shell 用户 `Permission denied`），
  故以设备内 `.winehua-runtime-manifest.json` 的 `payloadSha256` 作为运行时标识。
- R1 尚未开工，本文只列范围。

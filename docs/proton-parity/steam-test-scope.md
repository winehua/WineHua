# Steam 测试范围（S0）

> 日期：2026-09-12
> 依据：`WineHua_Steam_Next_Phase_2026-09-12.md` §4.1 / §6 S0
> 目的：把「用哪份运行时、哪份客户端、在哪个 prefix、测什么、不测什么」先写死，
> 避免后面把「进程活着」「配置文件落盘」当成登录成功的替代品。

## 1. 运行时（本阶段冻结，不换）

| 项 | 取值 | 来源 |
| --- | --- | --- |
| HAP / 应用 | `app.hackeris.winehua` 1.0.13 (versionCode 1000013) | 设备 `bm dump` |
| 运行时包 | `wine-data.zip`，`payloadSha256 = 07ec815b…` | 设备 `files/wine/.winehua-runtime-manifest.json` |
| Wine | `winehua/wine@dc5204ecb0c`（WineHQ 11.10 分叉 + OHOS 补丁） | `runtime-provenance.json` |
| CPU 后端 | x86 → `libwow64fex.dll`（默认），x64 → `libarm64ecfex.dll`；`wowbox64.dll` 为可选对照 | 宿主 hilog `[WineChild] CPU dll` |
| FEX 配置 | `share/fex-emu/Config.json`（Proton 参考值，sha256 `9ea32f4d…`） | 设备实测 |
| 图形 | DXVK legacy 1.10.3 + `WINEDLLOVERRIDES=d3d11=n;dxgi=n` + guest Venus ICD（aarch64） | 宿主 hilog `[WineChild] final D3D env` |
| 桌面 | Wine 虚拟桌面 `explorer /desktop=shell,1280x800` + WineHua 多窗口合成 | 宿主 hilog `[MW]` |

**本阶段刻意不做**：不换 Wine 本体（R 线独立）、不换 FEX 版本、不换 DXVK/Mesa/virgl 档位、不改 Host present 顺序。

设备侧注意：当前安装的运行时 `bin/aarch64-unix/` 为空，**不含 P2 的 UnixLib `.so`**
（`runtime-provenance.json` → `runtimeOnDevice.unixLib`）。所以本阶段的结论
**只能描述这份运行时**，不能当作「P2 完成后」的表现。

## 2. 客户端

### 2.1 现状（不合格，但先用它取第一手阻塞）

设备上唯一的 Steam 是 `Z:\games\Steam`：

- 主程序自述 `updater built Jul 10 2023 17:00:59`，主要二进制 mtime = 2023-07-10；
- `steam.cfg` 写了 `BootStrapperInhibitAll=Enable`，每次启动都打印 `Suppressing Steam update`；
- 自带 `1.bat`，里面是一整套旧参数（`-cef-single-process`、`-cef-disable-sandbox`、`-cef-disable-gpu` 等）。

按方案 §6 S0，这**不能作为唯一验收对象**。它只用来回答一个问题：
「换成真正官方客户端之前，这条链路最先在哪一步断」。

### 2.2 验收客户端的取得方式（待办，需用户确认）

推荐路径：Valve 官方 Windows 客户端（`https://store.steampowered.com/about/` → Windows 版 `SteamSetup.exe`），
允许其自更新到当前版本，安装到**独立目录**，不复用这份 2023 包。

待用户决定两点：

1. 是否由我下载官方安装器并在设备上安装（需要联网 + 会写入共享目录）；
2. 安装目录用哪里（例如 `Z:\games\Steam-new`），现有 `Z:\games\Steam` 是否原样保留。

## 3. prefix

| 项 | 取值 |
| --- | --- |
| 当前 prefix | `files/.wine`（`WINEPREFIX=/data/storage/el2/base/files/.wine`） |
| 是否可切 | 目前**不可**：`GameHook` 的环境变量白名单是 `WINEDEBUG / DXVK_ / VN_ / VKR_ / WINEHUA_DXVK_ / WINEHUA_VKR_ / FEX_`（+ 若干 BOX64_），不含 `WINEPREFIX` |
| 结论 | 「隔离 Steam 测试 prefix」这件事**当前实现不了**，需要一次应用层改动才能落地 |

本阶段采取的折中：仍在现有 prefix 内测试，但明确记录**这一轮写过哪些路径**
（见 `steam-startup-trace.md` §6），并且**不动运行时包、不动别的应用数据**。
真正隔离 prefix 的做法留作 S0 的补票项。

## 4. 启动方式

统一使用应用自己的 Want 通道（`GameHook`），而不是手工改 `1.bat`：

```bash
aa force-stop app.hackeris.winehua
aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode game \
  --ps winehua.game_path "Z:\games\Steam\steam.exe" \
  --ps winehua.run_id <run-id>
```

`winehua.game_path` 支撑多个参数时用 `winehua.game_argc` + `winehua.game_arg_enc<i>`。
**首轮不加任何参数**，走客户端自己的默认（默认即多进程 CEF）。

## 5. 本轮范围

| 做 | 不做（首版排除） |
| --- | --- |
| 启动真实客户端，记录进程树 / broker / 日志 / 窗口 | Overlay、录制、商店内购买、VR |
| 记录 CEF 的真实进程数与窗口几何 | 内核反作弊游戏、复杂第三方启动器 |
| 登录界面可见可交互（由用户完成账号操作） | 用协议登录/下载替代客户端验收 |
| 登录 → 游戏库 → 重启保留会话 | 手工 `winehua.mode=game` 补拉游戏进程冒充 Steam 启动 |
| 下载一款小型已拥有游戏 | 改/换游戏的 Steam API 库、伪造授权、绕过反作弊 |

## 6. 每轮必须留的证据

1. `ps -ef` 现场（Win PID 无法直接取；用宿主 hilog `Main() ENTER pid=… entryParams=…wine|<exe>` 把 NCP child 映射到 Windows 可执行名）。
2. 客户端自己的日志：`logs/bootstrap_log.txt`、`logs/console_log.txt`、`logs/connection_log.txt`、`logs/cef_log.txt`。
3. 屏幕截图（`snapshot_display -f …​.jpeg`）。
4. 宿主 hilog 的窗口侧事件：`[WL_Xdg]` / `[MW] FireToplevel` / `[MW-COMMIT]` / `[MW-MOVE]` / `Input`。
5. 脱敏：账号名、PersonaName、token、cookie 一律不进文档、日志与 Git。

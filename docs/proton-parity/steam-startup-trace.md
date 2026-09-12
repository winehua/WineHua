# Steam 启动追踪（S1）

> 日期：2026-09-12
> 设备：MLR-AL10 / HarmonyOS 7.0.0.105 / API 26
> run id：`steam-s1-20260912a`
> 客户端：`Z:\games\Steam`（2023-07-10 冻结包，详见 `steam-client-manifest.json`）
> 启动方式：**默认参数、默认多进程 CEF**（未使用 `1.bat` 的旧参数）

```bash
aa force-stop app.hackeris.winehua
aa start -a EntryAbility -b app.hackeris.winehua \
  --ps winehua.mode game \
  --ps winehua.game_path "Z:\games\Steam\steam.exe" \
  --ps winehua.run_id steam-s1-20260912a
```

客户端自己的日志确认本轮参数是干净的：

```text
[2026-09-12 12:02:57] Startup - updater built Jul 10 2023 17:00:59
[2026-09-12 12:02:57] Startup - Steam Client launched with: "Z:\games\Steam\steam.exe"
[2026-09-12 12:02:57] Suppressing Steam update
```

（对照：历次由 `1.bat` 启动的记录里，这一行会带上
`-cef-single-process -cef-in-process-gpu -cef-disable-sandbox …`。）

## 1. 时间线

| 时刻 (CST) | 事件 | 证据 |
| --- | --- | --- |
| 12:02:37 | `aa force-stop`，清空 hilog 缓冲 | 命令输出 |
| 12:02:40 | `aa start` 成功；virgl 子进程 `Native_libvirgl_child0` 起来 | `ps -ef` |
| 12:02:41 | child1 = `wineserver -f -p`（pid 44627） | hilog `Main() ENTER` |
| 12:02:42–44 | `wineboot` → `services.exe` / `winedevice.exe` / `plugplay.exe` / `svchost.exe` | 同上 |
| 12:02:45 | toplevel #1 = Wine 桌面（`shell - Wine 桌面`，1280x800，desktop_root）→ 启 `DesktopAbility` | hilog `WineWM` |
| 12:02:44 | child19 = `Z:\games\Steam\steam.exe`（pid 44832） | hilog `Main() ENTER` |
| 12:02:57 | 客户端 bootstrap 自述启动命令行（无附加参数）+ 抑制自更新 | `logs/bootstrap_log.txt` |
| 12:03:01 | `steamwebhelper.exe` 第 1 个进程（pid 45142） | hilog `Main() ENTER` |
| 12:03:04 | `Client version: 0`；连通性测试开始 | `logs/connection_log.txt` |
| 12:03:07 | **IPv6 HTTP 连通性测试 SUCCESS；IPv6 UDP 连通性测试 SUCCESS；`Connectivity test: OK!`** | 同上 |
| 12:03:10 | `console_log`：SDL 3.0.0-1782-g214d5daa3；`Win32Font.cpp(963) Couldn't get string length` ×4；`CalcUnIPThisBox - GetAdaptersAddresses returned 50`；`Failed to init SteamVR because it isn't installed` | `logs/console_log.txt` |
| 12:03:01–12:03:15 | 5 个 `steamwebhelper.exe`；`steamerrorreporter.exe`（pid 45157，12:03:14 退出码 0）；`gldriverquery{,64}.exe`、`vulkandriverquery{,64}.exe`、2× `conhost.exe` | hilog + `ps` |
| 12:03:01–12:08 | `cef_log.txt` 出现 `browser / gpu-process / utility / renderer` 四类进程 → **多进程 CEF 成立** | `logs/cef_log.txt` |
| 12:03:23.837 | **toplevel #5 created `{w:640,h:480}`；`app_id=steamwebhelper.exe`；`title=登录 Steam`** | hilog `WL_Xdg` + `FireToplevel` |
| 12:03:23.864 | `limits` min=(705,440) max=(0,0) 随后 min=max=(705,440) | hilog |
| 12:03:23.865 | `[MW-VP] set_source surf=… tl=5 src=(0,0 705x440)` | hilog |
| 12:03:23.885 | `[MW-MOVE] initial pos tl=5 (287,180)`（= 1280x800 里居中 705x440 的兜底位置） | hilog |
| 12:03:23.886 | `resize {w:705,h:440}` | hilog |
| 12:03:31 | 屏幕点击 px=(1074,1086) 被路由到 **tl=5**，local=(250,363)，并注入 Enter | hilog `WL_Input` |
| 12:03:33 | 屏幕点击 px=(270,1560) 被路由到 **tl=2**（桌面任务条），local=(75,15) | hilog `WL_Input` |
| 12:04:07 起 | `[MW-COMMIT] surface w=705 h=440 stride=2820 stored=0 content=705x440 geo=no` 持续提交（≈60/s） | hilog `WL_Server` |
| 12:03 / 12:04 / 12:09 | 三次截图 **sha256 完全相同**（`bc71549f…`）：只有桌面底色 + 任务条 + 「登录 Steam」按钮，**没有 Steam 窗口内容** | `snapshot_display` |

## 2. 进程树

完整机读版本见 `steam-process-tree.json`。要点：

- 宿主主进程 43975 + 1 个 virgl 子进程；
- NCP wine 子进程共 40 个（`Native_libwine_child1..40`），其中真正属于 Steam 的：
  `steam.exe` 1 个、`steamwebhelper.exe` 5 个、`steamerrorreporter.exe` 1 个（已退出）、
  `gldriverquery(64)` / `vulkandriverquery(64)` 各 1 个、`conhost.exe` 2 个；
- 其余是 Wine 会话基础设施：`wineserver`、`wineboot`、`services`、`winedevice`、
  `plugplay`、`svchost`、`rpcss`、`explorer`（含桌面 `shell`）、`winehua_keep`。

CPU 后端逐进程可验（所有 Steam 进程一致）：

```text
[WineChild] CPU dll HODLL64=libarm64ecfex.dll HODLL=libwow64fex.dll engine=(default fex)
[WineChild] final D3D env backend=dxvk_legacy dxvkVersion=1.10.3 override=d3d11=n;dxgi=n
[WineChild] cwd=/storage/Users/currentUser/Download/app.hackeris.winehua/games/Steam
```

即 32 位 `steam.exe` 走 `libwow64fex.dll`，64 位 `steamwebhelper.exe` 走 `libarm64ecfex.dll`
——没有落到 `wowbox64`。

## 3. 网络路径（本轮的好消息）

客户端自己的连通性测试**在 guest 内是通的**：

```text
[12:03:04] Connectivity test: Starting test, fetching 'http://test.steampowered.com/204'
[12:03:07] IPv6 HTTP connectivity test (ipv6check-http.steamserver.net / [2404:3fc0:a:100::6]:80) - SUCCESS
[12:03:07] IPv6 HTTP connectivity test ... server indicated we are using ipv6, external address = '2408:…'
[12:03:07] Connectivity test: OK!
[12:03:07] IPv6 UDP connectivity test (ipv6check-udp.steamserver.net / [2404:3fc0:2:101::671c:369a]:27019) - SUCCESS
```

同时存在的 Windows 网络配置 API 缺口（**不是**本轮的第一阻塞，但必须单独测掉）：

```text
cef_log: WSALookupServiceBegin failed with: 0
cef_log: Failed to read DnsConfig.        ← 每 ~5 s 一次，持续到 12:08
cef_log: Unexpected error retrieving WPAD configuration from DHCP.
console_log: Assertion Failed: CalcUnIPThisBox - GetAdaptersAddresses returned 50
```

## 4. CEF 渲染路径

`cef_log.txt` 的关键行：

```text
[120301] Crash reporting enabled for process: browser
[120303] Crash reporting enabled for process: gpu-process
[120306] Crash reporting enabled for process: utility
[120315] Crash reporting enabled for process: renderer
[120304] ERROR:gl_surface_egl.cc(741)] EGL Driver message (Critical) eglInitialize: No available renderers.
[120304] ERROR:gl_surface_egl.cc(1247)] eglInitialize D3D11 failed with error EGL_NOT_INITIALIZED, trying next display type
[120304..120310] ERROR:gl_surface_egl.cc(741)] EGL Driver message (Error) eglQueryDeviceAttribEXT: Bad attribute.
```

解释边界：

- 四类 CEF 进程都起来了 ⇒ **已经不是「多进程起不来」的问题**；
- ANGLE/D3D11 初始化失败是**预期的**（guest 里没有可用的 D3D11 适配器），CEF 随后退到
  软件路径，并仍然产出了 705x440 的帧提交；
- 但本会话**没有**证明这 705x440 帧的内容是什么（见 `steam-first-blocker.md` 的两个候选）。

## 5. 窗口侧观测汇总（对应方案 §5.2 的核查清单）

| 方案要求记录 | 本轮拿到 | 状态 |
| --- | --- | --- |
| 窗口创建者 PID | `steamwebhelper.exe`（NCP child23/25/26/27/36 之一） | 部分（未逐帧绑定到具体 pid） |
| 父子 HWND | — | **缺** |
| Win32 矩形 | — | **缺** |
| 可见 / 最小化状态 | — | **缺** |
| DPI | 宿主侧 `deviceDPI=2.38 boost=0.84 → effective=2.0` | 有（宿主侧） |
| Wayland surface | `wl_surface id=51` / `id=55`，toplevel #5，`[MW-VP] set_source tl=5 src=(0,0 705x440)` | 有 |
| buffer 尺寸 | `705x440`，`stride=2820`，`content=705x440` | 有 |
| configure / ack / commit | commit 有（≈60/s）；**`hasWindowGeometry=false`（`geo=no`）**，即客户端从未对 #5 调 `xdg_surface.set_window_geometry` | 有 |
| 输入命中区域 | 命中 tl=5（local 在 705x440 内）；原点用 `(287,180)` | 有 |

## 6. 本轮写到磁盘的路径（未覆盖其它应用数据）

```text
Z:\games\Steam\logs\{bootstrap_log,console_log,connection_log,cef_log}.txt
Z:\games\Steam\config\{config.vdf,loginusers.vdf,libraryfolders.vdf}
Z:\games\Steam\bin\*_driverquery* 的日志（若有）
C:\users\100\AppData\Local\Steam\htmlcache\…       （CEF profile）
C:\windows\temp\*_dxgi.log / *_d3d11.log           （宿主 DXVK 日志）
```

没有重新生成 prefix，没有替换运行时包。
**注意：这里仍是产品 prefix**（`files/.wine`），「隔离 Steam 测试 prefix」尚未实现（见 `steam-test-scope.md` §3）。

## 7. 结论

1. 进程链路、CPU 后端、图形 DLL、网络出口**都是通的**：`steam.exe` 起了 5 个 webhelper，
   在 guest 内完成了 Steam 自己的 IPv6 HTTP/UDP 连通性测试，并创建了标题为「登录 Steam」的 toplevel。
2. 断点不在「起不来」，而在**登录窗口没有出现在屏幕上**，且客户端本身过旧（自更新被抑制）。
3. 因此本轮**不写「Steam 基本可用」**：没有可交互的登录界面，也没有登录成功。

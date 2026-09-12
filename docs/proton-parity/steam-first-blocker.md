# 第一处可复现阻塞（S1）

## 0. 2026-09-12 13:00 更新：换官方客户端后，第一处阻塞变成「steam.exe 断言后崩溃」

> 用户已按 S0 要求装上 Valve 官方 Windows 客户端。旧的 2023 冻结包不再在设备上。
> **本节取代 §1 成为当前的第一处阻塞**；§1 保留为「用旧包时」的记录。

### 0.1 安装面（都成功，不是问题所在）

```text
安装位置     C:\Program Files (x86)\Steam
构建号       1788652215（updater built Sep  2 2026 18:32:43）
自更新       已开启：Checking for update -> Manifest download -> Verifying all executable
             checksums -> Verification complete，且 installed version 与 manifest 相同
GpuTopology  查询成功：Virtio-GPU Venus (Maleoon 910)，k_EGpuDriverId_MesaVenus，25.0.1
```

新客户端是 **x86-64**（`steam.exe` / `SteamUI.dll` / `bin/cef/cef.win64/steamwebhelper.exe`
实测 PE Machine 均为 `0x8664`），走 `libarm64ecfex.dll`；旧包是 i386，走 `libwow64fex.dll`。
**两者报的是同一条文本度量错误**，所以更像上层 Wine 文本 API 的问题，而不是某个 CPU 后端。

### 0.2 崩溃链（3 次冷启动 3 次复现）

```text
13:00:45  Steam Client launched with: "C:\Program Files (x86)\Steam\steam.exe"
13:00:50  steamsysinfo.exe -> GpuTopology OK
13:00:51  steamwebhelper.exe (bin/cef/cef.win64) + gpu-process + utility(network) + utility(storage)
13:01:29  写出 dumps/assert_steam.exe_20260912130129_1.dmp
13:01:32  写出 dumps/crash_steam.exe_20260912130132_2.dmp
13:01:35  [ProcMon] pid=5468 no longer alive name=steam.exe exit=1 source=ncp-exit
          -> 随后所有 steamwebhelper 退出（父进程没了）
```

**assert 原文**（从 `assert_steam.exe_*.dmp` 里读出的字符串）：

```text
Assert( Couldn't get string length ):C:\buildworker\steam_rel_client_win64\build\src\vgui2\vgui_surfacelib\Win32Font.cpp:1129
```

**crash dump 的异常流**：

```text
exceptionCode = 0xC0000005  (ACCESS_VIOLATION)
address       = 0x0000006FFF864AAC
param[0]      = 1           (写)
param[1]      = 0x10        (被访问地址 —— 典型的「空指针 + 0x10 偏移」写)
```

### 0.3 关键判断：这不是新客户端带来的新缺陷

同一句话在**旧包**里也出现过，只是当时只写一行日志、不致命：

```text
（2023 包）logs/console_log.txt:
  src\vgui2\vgui_surfacelib\Win32Font.cpp (963) : Couldn't get string length   ← 出现 4 次
```

⇒ **我们这份 Wine 一直存在文本度量（GDI 字体/排版）缺陷；老客户端记日志，新客户端直接断言退出。**
这是一条可以在**我们自己仓库**里定位和修掉的缺陷，不需要换 Wine 基线。

### 0.4 字体侧现状（相关，未定因）

```text
prefix:  drive_c/windows/Fonts/   -> 空
字体来源: 注册表指向 \??\unix\system\fonts\*.ttf（/system/fonts，共 207 个文件）
替换表:   "Lucida Console"="Noto Sans Mono"
          "Tahoma"="HarmonyOS Sans SC"
          "Tahoma"=str(7):"SIMSUN.TTC,SimSun\0..."
可疑形态: /system/fonts/NotoSansMono[wdth,wght].ttf  —— 变量字体，文件名带方括号
          /system/fonts/NotoSansCJK-Regular.ttc      —— 字体集合（collection）
```

这两类正是字体枚举/加载容易踩坑的形态。**本轮不下结论**，只记为待验证假设。

### 0.5 本轮暴露的工具缺口（必须先补）

为了拿到 Wine 自己的视角，本轮用 `winehua.d3d_env_json` 传了
`WINEDEBUG=err+all,warn+all,+dwrite`，子进程日志确认**参数已生效**：

```text
[WineChild] final WINEDLLDIR=... WINEDEBUG=err+all,warn+all,+dwrite HODLL64=libarm64ecfex.dll HODLL=libwow64fex.dll
```

但一整轮复现之后，hilog 里**一条 Wine 的 `err:`/`warn:`/`fixme:` 都没有**
（抓到的 `err:` 全是 HarmonyOS 系统组件自己的）。也就是说
**Wine 子进程的 stderr 目前没有落到任何可读通道**。
这会让后面所有 Wine 侧缺陷都变成盲调，必须先修。

### 0.6 下一步（最小、可复现）

1. 打通 Wine stderr（落 hilog 或落文件），确认 `+dwrite` / `+gdi32` 真能看到输出。
2. 写一个受控探针 exe：按 Steam 的字体列表依次 `CreateFontIndirectW` +
   `GetTextExtentPoint32W`，找到第一组返回 FALSE / 全 0 尺寸的 (字体, 字符串)。
3. 在**我们自己仓库的 Wine** 里做有界修复（字体枚举 / 变量字体 / ttc、替换表、
   `GetTextExtentExPointW` 的失败路径），改完先跑 smoke 回归。
4. 验收：`dumps/` 不再新增 `assert_steam.exe_*.dmp`，且登录窗口出现。

### 0.7 为什么不换基线也能收敛（决策规则）

原则：**按 Steam 客户端实际踩到的 Wine 缺陷逐条修**；只有当缺陷变成「要重写 Proton 的整块
子系统」时才考虑换基线。

| 缺陷形态 | 处置 |
| --- | --- |
| 单点 API 语义错 / 字体枚举 / 文本度量（如本条） | 在自己 Wine 里修，单独留证据 |
| 需要 steamclient.dll 导入修复、Steam Input、overlay 这类整套机制 | 先评估成本；成规模再谈换基线 |
| 只是「配置 / 参数」问题 | 改配置，不动源码 |

参考量级（本会话实测）：Valve 的 `proton_11.0` 上有 44 条提交标题直接含 steam/CEF，
其中 `ntdll: HACK: Partially fixup imports for Win Steam libs`、
`HACK: ntdll: Fixup OptionalHeader.ImageBase back in steamclient.dll`、
多条 `Force CEF swiftshader / CEF in-process GPU` 属于「整块机制」；
而字体类补丁（`dwrite` 回退、Proton 字体注册）恰恰说明**字体这条线本来就该在 Wine 侧修**——
Valve 只是在他们的树上修，我们可以修在自己的树上。

> 日期：2026-09-12
> run id：`steam-s1-20260912a`
> 结论：**链路没有「起不来」的问题；断点是「登录窗口创建了、有帧提交，但没有出现在桌面上」，
> 并且叠着一个前置致命问题——客户端是 2023 冻结包且自更新被关掉了。**
> 本文只给观测与下一步测量，不给未取证的根因。

## 1. 阻塞 A：登录窗口不出现在屏幕上（用户可见的第一处）

### 1.1 观测（全部为实测）

```text
12:03:23.837 [WL_Xdg]   app_id tl=5 steamwebhelper.exe
12:03:23.837 [WL_Xdg]   title  tl=5 登录 Steam
12:03:23.885 [MW-MOVE] initial pos tl=5 (287,180)      # 1280x800 中 705x440 的居中位置
12:03:23.886 [MW] FireToplevel id=5 event=resize {"w":705,"h":440}
12:04:07.357 [MW-COMMIT] surface w=705 h=440 stride=2820 stored=0 content=705x440 geo=no
            （该行以约 60 次/秒持续到 12:08 之后）
```

三次截图（12:03 / 12:04 / 12:09）**sha256 完全相同**（`bc71549fc33d3673…`）：
画面只有桌面底色 + 底部任务条 + 蓝底的「登录 Steam」按钮，**没有 Steam 窗口内容**。

同时，输入链**认得**这个窗口：

```text
12:03:31 [Input] TARGET a=1 px=(537,543) → tl=5 surf=0x5e1ef18900 origin=(287.0,180.0) → local=(250.0,363.0)
```

### 1.2 两个候选，尚未区分

| 候选 | 支持 | 反对 / 待测 |
| --- | --- | --- |
| A1：窗口**没有**被合成进桌面 | `geo=no`（客户端从未对该 toplevel 调 `xdg_surface.set_window_geometry`）；桌面模式下 toplevel 不单独开 ArkUI 窗口（`[MW] Desktop mode: toplevel #5 created, skipping Ability`），必须由桌面层合成 | 桌面根 #1 与任务条是**正常显示**的，说明桌面合成本身在跑 |
| A2：窗口被合成了，但**帧内容是空白的** | 截图里桌面底色本身接近纯白，空白窗口在截图里看不出来；CEF 的 `eglInitialize: No available renderers` 说明它走了软件路径 | 缺少 toplevel #5 帧内容的转储，无法证伪 |

**区分实验（下一步）**：把 toplevel #5 的 `st.FrameData()` 按帧转储一次
（或对同一帧做一次像素直方图/非白像素计数），
若内容非空白 ⇒ A1；若全空白 ⇒ A2。
这是**新增一个受限探针**，不是「全局截断坐标」或「把映射改成匿名私有副本」这类越界改动。

### 1.3 还缺的证据（方案 §5.2 清单未打勾项）

```text
窗口创建者 PID（具体是哪个 steamwebhelper）
父子 HWND
Win32 矩形
可见 / 最小化状态
```

这三项要从 Wine 侧取（例如 `win32u` 侧一次性 dump），本轮没有取，
因此**不能**把「窗口几何不对」当成已确认的根因——只能说「宿主侧拿到的几何是兜底居中值，
而客户端从未发过 window_geometry」。

## 2. 阻塞 B：客户端过旧 + 自更新被抑制（前置致命）

```text
logs/bootstrap_log.txt
  [2026-09-12 12:02:57] Startup - updater built Jul 10 2023 17:00:59
  [2026-09-12 12:02:57] Suppressing Steam update
logs/console_log.txt
  [2026-09-12 12:03:10] Client version: 0
  [2026-09-12 10:58:15] LogonFailure License expired
logs/connection_log.txt
  [2026-09-12 10:58:15] [Logging On, 4, 7] … RecvMsgClientLogOnResponse() : [I:0:0] 'License expired'
  [2026-09-12 10:58:15] … not auto reconnecting due to eResultLogonSession specifies no reconnect
```

`steam.cfg` 里写着：

```text
BootStrapperInhibitAll=Enable
BootStrapperForceSelfUpdate=False
```

也就是说：**即使把窗口显示问题修好，这份客户端也会用 `License expired` 拒绝登录。**
这不是 Wine 的缺陷，是客户端来源问题；方案 §6 S0 明确要求换用官方客户端。

## 3. 阻塞 C（不是本轮第一处，但必须先量掉）

Windows 网络配置类 API 的缺口：

```text
cef_log: WSALookupServiceBegin failed with: 0
cef_log: Failed to read DnsConfig.                       （每 ~5 s）
cef_log: Unexpected error retrieving WPAD configuration from DHCP.
console_log: Assertion Failed: CalcUnIPThisBox - GetAdaptersAddresses returned 50
```

但**同一进程内** Steam 自己的连通性测试是通的（12:03:07 HTTP + UDP 均 SUCCESS），
所以这一组不能被当作登录失败的根因；它属于 §5.6「登录前并行验证网络/TLS」的待办。

## 4. 建议的处理顺序（与方案的门禁一致）

1. **先换客户端**（S0 硬要求，也是阻塞 B 的唯一解）：取得官方 Windows Steam，
   装进独立目录，允许其自更新；现有 `Z:\games\Steam` 原样保留。
   理由：在即将被替换的客户端上修窗口几何，等于把工夫花在会变的变量上。
2. **再复测窗口**：用新客户端跑同一套 Wish 通道，重新采集 §1.1 的同一组日志。
   若到时 toplevel 仍然 `geo=no` 且不上屏，再按 §1.2 的区分实验定位到具体层。
3. **并行把 prefix 隔离补上**：目前 `WINEPREFIX` 不在 `GameHook` 的环境白名单里，
   「隔离 Steam 测试 prefix」这条 S0 要求**当前实现不了**，需要一次有界改动。
4. 网络/TLS 的 Windows 配置 API 缺口按 §5.6 单独建项，不与窗口问题混在同一次改动里。

## 5. 本轮明确不做的

- 不改全局 CEF 窗口坐标截断；
- 不把映射改成匿名私有副本；
- 不为「让它看起来能跑」而默认开 `-cef-single-process`、关 sandbox、关证书校验；
- 不用自己手动再拉一个 exe 冒充「Steam 启动了游戏」；
- 不把这个结果写进产品默认值。

## 6. 交付状态

| 项 | 状态 |
| --- | --- |
| Steam 进程能起来 | ✅ 实测 |
| 多进程 CEF | ✅ 实测（browser/gpu/utility/renderer + 5 个 webhelper） |
| guest 内网络连通性 | ✅ 实测（HTTP + UDP SUCCESS） |
| 登录界面可见可交互 | ❌ 未达成（窗口有 toplevel 与帧提交，但不在画面上） |
| 登录成功 / 游戏库 | ❌ 未达成（客户端自身 `License expired`） |

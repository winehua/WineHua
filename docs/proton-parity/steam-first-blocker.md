# 第一处可复现阻塞（S1）

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

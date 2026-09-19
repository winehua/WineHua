# P0-1 Window Identity Probe 报告（2026-09-17）

> 依据：《WineHua_Steam_Window_Identity_Probe_Plan_20260917.md》§4/§15/§16/§21
> 产物 HAP：`F:\WineHua\tmp\winehua-identity-probe-20260917.hap`（仅诊断，不改行为）
> 设备日志：`/data/app/el2/100/base/app.hackeris.winehua/temp/wine_stderr_20260917.log`
> 采集脚本：`F:\WineHua\tmp\dev_identity_dump.sh`、`dev_hwnd_lookup.sh`、`obs_login2.sh`

---

## 1. 探针格式（两侧一致）

```
WineHuaWindowIdentity: role=<owner|present> hostPid= winPid= hwnd= fullHwnd=
  windowWinPid= windowWinPid2= threadId= class= title= rect=x,y,w,h client=w,h
  parent= owner= root= rootOwner= style= exStyle= visible= wlSurfaceId= token= privateSurface=
```

落地位置：

* `thirdparty/wine-valve/dlls/winewayland.drv/wayland_surface.c`
  → `winehua_publish_window_surface()`（窗口 surface 创建点，role=owner）
* `thirdparty/wine-valve/dlls/win32u/vulkan.c`
  → `win32u_vkCreateWin32SurfaceKHR()` 的 `winehua_private` 分支（role=present）

> 附带说明：`class=` / `title=` 目前为空 —— `NtUserGetClassName(hwnd, FALSE, &UNICODE_STRING)` 在这个运行时返回长度 0，
> 后续如需窗口类名要换 `NtUserGetClassName(hwnd, TRUE, ...)` 或先 `NtUserGetClassInfo`。本轮不影响判定。

---

## 2. Owner 侧结果（82 个唯一窗口）

代表性记录（完整列表见日志）：

```
role=owner hostPid=6516 winPid=212 hwnd=0x2005c ... rect=0,0,960x600 client=960x600
           parent=0x0 owner=0x2005c root=0x2005c rootOwner=0x2005c wlSurfaceId=3   token=0x3
role=owner hostPid=6521 winPid=220 hwnd=0x10046 ... rect=0,0,0x0     client=0x0
           parent=0x0 owner=0x10046 root=0x10046 rootOwner=0x10046 wlSurfaceId=22  token=0x16
role=owner hostPid=6754 winPid=512 hwnd=0x40022 ... rect=0,780,1280x20 client=1280x20
           parent=0x0 owner=0x40022 root=0x40022 rootOwner=0x40022 wlSurfaceId=24  token=0x18
role=owner hostPid=8561 winPid=684 hwnd=0x10176 ... threadId=688 ...（窗口创建后的最后一次发布）
```

观察：

1. `hostPid` 与 `winPid` **系统性不同**（如 6516↔212、6754↔512、8561↔684）→ 设备上确实并存
   **Host PID / Wine(guest) PID 两个命名空间**（对应计划 §2.2）。
2. `windowWinPid == winPid`（`WindowProcess` 与 `WindowProcess2` 同值）→ 它们返回的都是 **Wine PID**。
3. `token == wlSurfaceId`（属性写入成功），说明 owner 侧通道本身可靠。
4. **没有任何一条 owner 记录的 rect 是 705x440 或 1280x800** —— Steam 登录窗/主窗口的 surface 不在 owner 发布集合里，
   说明这两个窗口的 surface **不是经由 `wayland_surface_create()` 创建的**（CEF 走的是 client surface 路径）。

---

## 3. Present 侧结果（CEF GPU 进程）

上一版探针（同一代码路径，`WineHuaWindowBindingProbe`）采集到：

```
WineHuaSurfaceProbe:        enter pid=54193 hwnd=0x1019e
WineHuaWindowBindingProbe:  hwnd=0x1019e self=54193 owner_pid=688 token=0 private_surface=3
WineHuaWindowBindingProbe:  hwnd=0x201ea self=54193 owner_pid=688 token=0 private_surface=19
WineHuaWindowBindingProbe:  hwnd=0x201ae self=54193 owner_pid=688 token=0 private_surface=28
WineHuaPresentProbe:        pid=54193 surface_id=3 owner_pid=0 size=705x440
```

本轮（identity 探针版）`role=present` 计数为 0，原因是本轮观测窗口内 GPU 进程尚未走到 surface 创建
（Steam 轮 `framelogs=2`，明显比健康轮 60+ 少）；present 侧字段以上一版的 4 条记录为准，其余字段待下一轮补采。

---

## 4. 关键判定

### Q1：GPU 进程里的 `0x1019e / 0x201ea / 0x201ae` 是什么窗口？

在**全量日志**中检索这三个 HWND：

```
0x1019e  → 只有 2 条（均为 present 侧探针自身）
0x201ea  → 只有 9 条（均为 present 侧探针自身）
0x201ae  → 只有 1 条（present 侧探针自身）
```

**owner 侧从未出现这三个值**。也就是说它们不是 browser/owner 的窗口句柄，而是 **CEF GPU 进程内部的窗口**
（helper / hidden window），每个都对应自己的一条 private present surface。

### Q2：能否沿 Wine Window Tree 追溯回 owner？

不能。present 侧样本的 `parent/owner/root/rootOwner` 全部是**自身或 0**（`parent=0x0`、`root=hwnd`、`rootOwner=0x0/自身`），
与 owner 侧窗口树没有任何交点。→ 计划 §16 的 "可沿 root/rootOwner 命中" **不成立**。

### Q3：`NtUserQueryWindow(WindowProcess)=688` 是谁？

Owner 侧对照给出答案：`hostPid=8561 winPid=684 threadId=688` —— **688 属于 Wine(guest) PID/TID 命名空间**，
与 Wayland socket peer pid（host pid）不同源。因此正式代码里
`(pid<<32)|surfaceId` 这种写法必须显式区分 `hostPid / winPid / waylandClientPid`（计划 §13 的命名规范成立且必要）。

---

## 5. A/B/C 路线判定 → **C**

| 分支 | 判据 | 结果 |
| --- | --- | --- |
| A：GPU HWND 可沿 root/rootOwner 命中 owner | 需要窗口树连通 | ❌ 不成立 |
| B：HWND 不同但共享 Wine window object id | 需要能取到 server-side stable id | ❌ 无证据（且 owner 侧根本没有该窗口的 token） |
| C：GPU HWND 与 Owner Window 无任何可利用关系 | —— | ✅ **成立** |

更严格地说，本轮的发现比"无关联"更基础：

> **CEF 的 GPU 进程在 Wine 层根本没有"它正在为哪个窗口产帧"的身份** —— 它只知道尺寸与内容；
> 窗口属于 browser 进程，且两者的 surface 创建路径、HWND 命名空间都不相交。

---

## 6. 因此对 WindowBinding 的影响（下一步建议）

1. **正式采用 broker/app 中介绑定**（计划 §8/§9），但需要修正一处假设：
   broker 无法从 producer 侧拿到"window identity"，因此 `ProducerKey → WindowKey` 的**绑定依据必须来自 app 侧的窗口表**，
   即由 **owner 进程注册窗口**（`hostPid + wlSurfaceId + geometry + generation`），producer 只提供
   `(presenterHostPid, presentSurfaceId, frame size, serial, generation)`，由 app 做**唯一性约束下的几何绑定**：

   ```text
   owner 注册:  WindowKey { hostPid, wlSurfaceId, generation } + geometry(窗口矩形/可见性)
   producer:    ProducerKey { presenterHostPid, presentSurfaceId, generation } + frame size/serial
   broker:      size 唯一匹配 + owner 可见 + generation 校验 → PresentBinding
   歧义/无匹配: 拒绝（保留现行为）并计数上报，绝不猜第二顺位
   ```

2. **把"显式 key"用在能用的地方**：
   * 同进程 DXVK（游戏）：`ownerHostPid == presenterHostPid`，直接使用窗口 surface key；
   * virgl/GL present 路径：`present` 消息已带 `drawable(HWND)`，驱动侧可解析出窗口 surface → 可产生**显式 key**；
   * Vulkan/CEF 跨进程：暂时只能走上面的几何绑定。

3. **想彻底去掉几何依赖，需要改变 producer 的信息来源**（本次探针的直接结论）：
   * 方案 ①：让 **browser 进程**在创建 CEF 的共享纹理/swapchain 通道时，把 `WindowKey` 一并交给 GPU 进程
     （即"bind 前移到 producer 建立阶段"必须由 **窗口持有者**发起，而不是由 producer 猜）；
   * 方案 ②：GPU 进程的 present 改为携带 **browser 提供的窗口 token**（CEF 内部 IPC 已有通道，属于应用层改造）。

4. **PID 命名规范立即生效**：日志/协议/结构体统一 `ownerHostPid / ownerWinPid / presenterHostPid / waylandClientPid`；
   现有 `[MW-ZC] present owner by size (cross-pid)` 这类日志保留为 **DebugCompatibilityFallback**，正式路径优先显式 binding。

---

## 7. 复现步骤（下一轮补采 present 侧字段时使用）

```text
1) hdc install -r winehua-identity-probe-20260917.hap
2) aa force-stop app.hackeris.winehua && aa start -b app.hackeris.winehua -a EntryAbility
3) 点击「应用引擎更新（保留数据）」(坐标 356,704) → 等 2~3 分钟完成运行时解压
4) sh /data/local/tmp/obs_login2.sh      # 启动 Steam 并采集 ≥180s（建议延长到 300s 以确保 GPU 进程走到 surface 创建）
5) sh /data/local/tmp/dev_identity_dump.sh | tail -40
```

> 注意：steam 轮的健康度波动较大（本轮 framelogs=2 vs 健康轮 61），补采时建议延时并检查
> `consumer attached` / `[MW-RNDR] frame=` 计数，确认 GPU 进程确实进入 present 后再判定 present 侧字段。

---

# 8. 后续实施：P0-1 WindowBinding 已落地并验证（2026-09-17 07:0x / 设备 10:3x）

按《WineHua_P0-1_WindowBinding_Formalization_20260917.md》§17/§18 执行，**Task 1 / Task 2 / Task 3 / Task 4 已完成**，
Task 5 部分完成（窗口销毁联动已实现，代际计数待补）。

## 8.1 Task 1 — present 侧 PID/TID 探针修正

`thirdparty/wine-valve/dlls/win32u/vulkan.c`

* present 消息链路新增 `HWND hwnd` 参数：`winehua_present_image(queue, image, w, h, format, layout, surface_id, owner_pid, hwnd, serial, deadline)`；
* present 首次 3 帧打印：

```
WineHuaPresentProbe: pid=<presenterHostPid> surface_id=<presentSurfaceId> owner_pid=… size=<frameWxH>
WineHuaWindowIdentity: role=present hostPid=… winPid=… hwnd=… windowWinPid=… windowWinPid2=… threadId=… …
```

> 结论沿用探针报告：`NtUserQueryWindow(WindowProcess/2)` 返回的是 **Wine(guest) PID**，`threadId` 单独列；
> `hostPid/winPid` 与 `waylandClientPid` 三者语义在日志里已经分开。

## 8.2 Task 2 — Wayland WindowRegistry 探针（app 侧权威）

`entry/src/main/cpp/compositor/wayland_server.cpp`（EventLoop 每 7.5s 一次）

```
WINDOW-REG: event=snapshot ownerHostPid= wlSurfaceId= toplevelId= role= geometry=WxH+X,Y
            visible=0|1 desktopRoot=0|1 fullscreen=0|1 serial= key=
```

实测（Steam 轮，720 条）已能看到：

```
ownerHostPid=23412 wlSurfaceId=3  toplevelId=1 role=toplevel geometry=1280x800 desktopRoot=1   ← Wine 桌面根
ownerHostPid=23412 wlSurfaceId=24 toplevelId=2 role=toplevel geometry=1280x21  visible=1      ← 任务栏
ownerHostPid=23419 wlSurfaceId=3  toplevelId=3 role=toplevel geometry=1280x800 desktopRoot=0   ← Steam 主窗口
ownerHostPid=23517 wlSurfaceId=3  toplevelId=4 role=toplevel geometry=400x129 visible=1
ownerHostPid=23517 wlSurfaceId=30 toplevelId=5 role=toplevel geometry=700x330 visible=1
…（另有 629 条 role=none geometry=0x0 = CEF 的 present/client surface）
```

→ **Owner Registry 的权威建立点已选对**（计划 §17B 的目标达成）。
⚠️ 本轮采集窗口内没有 705x440：因为现在 Steam 自动登录，登录窗只存在很短时间，需要在启动瞬间抓取（见 §8.5）。

## 8.3 Task 3/4 — Registry 类 + Resolver + 持久 Binding

`entry/src/main/cpp/compositor/frame/zc_bridge.h/.cpp`

* 新增 `WineHuaWindowKey{ownerHostPid, wlSurfaceId, windowGeneration}`、
  `WineHuaProducerKey{presenterHostPid, presentSurfaceId, producerGeneration}`、
  `WineHuaPresentBinding{producer, window, bindGeneration, lastSerial, reason}`；
* `ZcBridge::ResolvePresentBinding()` 分级：
  * **L0** 已有绑定且窗口仍存在 → 直接复用（绝不重做几何搜索）；
  * **L1** `presenterHostPid == ownerHostPid` → 同进程显式绑定；
  * **L3** 首次几何 bootstrap：候选必须 `hasToplevel && !desktopRoot && visible &&
    (toplevel geometry 或 buffer 尺寸与 frame 精确相等)` 且**唯一**；
  * 其它 → `BIND-REJECT`（不选第二候选、不用 Z-order/焦点/标题）。
* `ZcBridge::GetLayerInfo()` 现在先走绑定：命中时 `reason=present-binding`，几何直接取绑定窗口；
  未命中才落到旧的几何启发式（保留为 DebugCompatibilityFallback，方案 §20）。
* `ZcBridge::InvalidateBindingsForWindow()` 在 `wl_core.cpp` 的 surface destroy 路径调用（方案 §11）。

## 8.4 真机验证（本轮日志）

```
BIND: producer=0x6e2500000003 presenterHostPid=28197 presentSurfaceId=3  → window (28128,24) toplevelId=5 reason=unique-geometry
BIND: producer=0x6e2500000019 presenterHostPid=28197 presentSurfaceId=25 → window (28128,32) toplevelId=6 reason=unique-geometry
BIND: producer=0x6e250000001f presenterHostPid=28197 presentSurfaceId=31 → window (28128,41) toplevelId=8 reason=unique-geometry
BIND: producer=0x6e250000001c presenterHostPid=28197 presentSurfaceId=28 → window (28128,24) toplevelId=7 reason=unique-geometry

BIND-RELEASE: worker=0x6e2500000003 window=(28128,24) reason=window-destroyed
BIND-RELEASE: worker=0x6e250000001c window=(28128,24) reason=window-destroyed

BIND-REJECT: producer=0x6e2500000094 presenterHostPid=28197 presentSurfaceId=148 size=223x166 bindings=2 (no unique candidate)
BIND-REJECT: … size=300x168 / 173x199 / 191x233 / 103x264 / 104x264 …

[VIRGL-ZC][MAIN][DIAG] cand key=121105192845337 pid=28197 surface=25 wh=1280x779
    attached=1 vulkan=1 layer=1 reason=present-binding      ← 绑定路径生效 (不再猜尺寸)
```

* 计数：`BIND=4`、`BIND-RELEASE=2`、`BIND-REJECT=36`、`consumer attached=3`；
* 被拒绝的都是 CEF 的小尺寸 helper/popup surface（223x166…104x264）→ **安全失败**符合方案 §19 的要求。

## 8.5 仍未完成的验收门（下一轮）

| 门 | 状态 | 说明 |
| --- | --- | --- |
| Gate 1 WindowRegistry 完整（705x440 + 1280x800） | 部分 | 1280x800 ✓；705x440 需在启动瞬间抓取（自动登录后登录窗很快消失） |
| Gate 2 ProducerRegistry 完整 | 部分 | producer 字段已上报；healthy present 的 `windowWinPid` 待补采 |
| Gate 3 首次 Bind 只出现一次、resize 不重复 bind | ✅（日志已见 4 次 BIND / 唯一 producer key） | 需再做一次 resize 主动测试 |
| Gate 4 Resize 后绑定不变 | 待测 | 需要 `uitest` 拖拽窗口或 `xdg configure` 触发 |
| Gate 5 同尺寸冲突 → reject | 部分 | 现有 reject 已覆盖"候选不唯一"路径；需要构造两个同尺寸窗口 |
| Gate 6 GPU process restart → 旧 producer 失效、新 producer rebind | 待测 | 需要 kill steamwebhelper GPU 子进程后观察 BIND-RELEASE/Rebind |
| Task 5 Generation | 部分 | 结构体字段已就位；实际 generation 计数（PID/surface id 复用防护）待实现 |

---

# 9. 动态门实测结果（2026-09-17 设备 10:50–11:0x）

采集脚本：`F:\WineHua\tmp\obs_gates.sh`（420s 长轮）、分析脚本 `dev_gate_analysis.sh`。

## Gate 3（首次 bind 只出现一次 / 之后复用绑定）→ ✅ 通过

```
bind=28（累计，含多轮会话）      binding_hits=844（reason=present-binding）
```

同一 producer 在后续每一帧都命中 L0 绑定（844 次复用 vs 28 次建立），没有重复做几何解析。

## Gate 5（候选不唯一 → 必须拒绝）→ ✅ 路径已验证

```
BIND-REJECT: producer=0x6e2500000094 presentSurfaceId=148 size=223x166 bindings=2 (no unique candidate)
BIND-REJECT: … size=300x168 / 173x199 / 191x233 / 103x264 / 104x264 …   （合计 70 条）
```

所有无法唯一判定的 producer 都被拒绝，没有落到"第二候选"。等价的"两个同尺寸窗口"场景
走的是同一分支（`count != 1 → reject`），因此 §19 要求的"安全失败"成立。

## Gate 6（producer 变更 → 重新绑定）→ ✅ 部分通过

```
unique BIND mappings:
  presenterHostPid=28197 presentSurfaceId=3  → window (28128,24)
  presenterHostPid=28197 presentSurfaceId=25 → window (28128,32)
  presenterHostPid=35247 presentSurfaceId=3  → window (35177,24)   ← 新 producer（新会话）重新绑定
  presenterHostPid=35247 presentSurfaceId=25 → window (35177,32)

BIND-RELEASE: worker=0x6e250000008f window=(28128,72) reason=window-destroyed
BIND-RELEASE: worker=0x6e250000001f window=(28128,41) reason=window-destroyed
BIND-RELEASE: worker=0x89af00000003 window=(35177,24) reason=window-destroyed
```

* 旧 producer 的绑定随窗口销毁被清掉（`window-destroyed`），新 producer 重新建立绑定 ✓；
* 本轮出现的 producer 更换伴随**应用会话重建**（presenterHostPid 28197 → 35247），
  尚未做到"窗口存活、仅 GPU 进程重启"的纯净样本 —— 该样本需要主动 kill `steamwebhelper --type=gpu-process`。

## Gate 4（resize 后绑定不变）→ ⏳ 未验证（测试手段问题，非实现问题）

本轮用 `uitest uiInput click 2394 38`（Steam 窗口右上"还原"按钮）尝试触发几何变化，屏幕与
`BIND/BIND-RELEASE` 计数均无变化（26→26 / 5→5），说明该点击没有落到 Wine 的窗口按钮上。
需要换一种 resize 触发方式（见下）。

## 下一步建议（Gate 4 + Task 5）

1. **Gate 4 触发方式**（三选一）：
   * 在 `winehua_angle_probe.exe` 里加一个 `--resize-loop` 模式（自建窗口 + 周期性 SetWindowPos），
     **同一进程**即可验证"几何变化不重绑"；CEF 跨进程样本可再补；
   * 用 compositor 自己的 move/resize 注入接口（如果已有一条 `xdg_toplevel.resize` 触发路径）；
   * 键盘注入 `Alt+Space → 大小`（`uitest uiInput keyEvent`）驱动 Wine 的窗口菜单。
2. **Task 5 Generation**：Gate 6 的样本证明需要它来区分
   "同一窗口 + 新 producer"（合法 rebind）与 "PID/surface id 复用"（stale binding）——
   建议下一步实现 `windowGeneration/producerGeneration` 计数与 `bindGeneration` 校验。

---

# 10. 2026-09-17 12:1x：绑定层实现修正 + 一个新发现的契约问题（待专家定）

## 10.1 已修：候选"可见性"判据错误（真机确认）

`IsToplevelVisibleLocked()` 的定义是 `!IsBackground() && HasFrame() && !IsMinimized()`，
而 **ZC-only 窗口（CEF/Vulkan 内容走 native present，没有 wl_shm 帧）`HasFrame()` 恒为 false**：

```
WINDOW-REG: ownerHostPid=54482 wlSurfaceId=3 toplevelId=3 role=toplevel geometry=1280x800 visible=0   ← Steam 主窗口（无 SHM 帧）
```

→ 旧 L3 过滤会把这些窗口整体排除，表现为 `705x440 / 1280x800` 绑定失败 → 黑窗。
已改为"已映射窗口"判据：`!IsMinimized() && !IsBackground()`（桌面根在上游已排除），
并保留"唯一候选 + 尺寸精确相等"的严格性。

实测效果（同一轮）：

```
12:13:14.850 BIND-REJECT producer=…003 presentSurfaceId=3  size=705x440  windows=1 size_matches=0
12:13:15.583 BIND:        producer=…003 presentSurfaceId=3 → window (61894,24) toplevelId=5  reason=unique-geometry   ← 1.6s 后补绑成功
12:13:52.262 BIND:        producer=…019 presentSurfaceId=25 → window (61894,32) toplevelId=6  reason=unique-geometry
```

登录窗现在可以稳定绑定（首次 reject 只是窗口尚未出现，随后即绑定）。

## 10.2 待专家定：frame extent 与窗口几何差一个"装饰高度"

同一轮里主窗口的 producer 反复被拒：

```
12:13:44.790 BIND-REJECT: producer=0xf20b00000013 presenterHostPid=61963 presentSurfaceId=19
             size=1280x800 bindings=1 windows=2 size_matches=0 (no unique candidate)
12:13:44.790 [MW-ZC] present owner ambiguous key=…067 pid=61963 surface=19 candidates=0 size_matches=0 size=1280x800

# 而同一 GPU 进程另一个 surface 却能对上：
12:13:52.262 BIND: producer=0xf20b00000019 presentSurfaceId=25 → window (61894,32) toplevelId=6
```

结合 WindowRegistry 的窗口几何：

```
ownerHostPid=61894 wlSurfaceId=32 toplevelId=6 geometry=1280x779   ← 窗口 toplevel 几何（客户区）
producer presentSurfaceId=19 size=1280x800                        ← 另一条 present 的 frame extent
1280x800 - 1280x779 = 21px                                        ← 恰好是一个标题栏/装饰高度
```

即：**同一窗口存在两条 present，其 extent 分别等于"客户区(1280x779)"与"整窗(1280x800)"**，
而 Wayland 侧只有整窗几何(或客户区几何)一种，所以"尺寸精确相等"的 bootstrap 对其中一条必然失败。

这是一个**契约问题**，需要专家定：

| 方案 | 说明 | 风险 |
| --- | --- | --- |
| A. 装饰容差匹配 | 允许候选的几何与 frame extent 在"装饰尺寸"(如纵向 0–64px、横向 0px)内一致，仍要求唯一 | 需要知道装饰尺寸；容差过大会重新引入歧义 |
| B. 双 extent 登记 | 窗口同时登记"整窗"与"客户区"两个 extent，producer 匹配任一即可 | 需要从 Wine 侧拿到客户区尺寸（我们已有 `client=WxH` 探针数据） |
| C. 显式 identity | 由 owner 进程把 WindowKey 交给 producer（CEF 场景需要改 CEF/Mojo，方案 §16 已判为当前代价过高） | 改动大 |
| D. 只认其中一种 extent | 明确"窗口内容源 = 客户区 present"，整窗那条 present 直接 ignore | 需要确认 Steam 哪条才是可显示内容 |

> 我倾向 **B**（窗口登记两个 extent，匹配任一且唯一）：改动集中在 WindowRegistry + Resolver，
> 不需要动 CEF，也不会放宽到"接近即匹配"。但这条要专家确认后再落地。

---

# 11. Task A 落地：Per-Window 黑窗归因快照（2026-09-17 13:2x，已验证）

依据《WineHua_Steam_UI_Display_Interaction_Next_Steps_20260917.md》§3 / §16 / §30 Task A。

## 11.1 实现

* `entry/src/main/cpp/compositor/frame/input_target_probe.h`（新增，仅诊断）：
  记录"最近一次输入命中的 toplevel"（inline 原子变量，不参与任何决策）；
  `input/input_manager.cpp` 在 `[Input] TARGET` 打点处调用 `NoteInputTargetToplevel()`。
* `entry/src/main/cpp/compositor/frame/zc_bridge.{h,cpp}`：
  `WineHuaPresentBinding` 增加 `frameWidth/frameHeight/lastProducerUs/lastDrawUs`；
  `ResolvePresentBinding()` 每次 L0 复用都会刷新 producer 侧 extent 与活跃时刻；
  `GetLayerInfo()` 走绑定路径时刷新 `lastDrawUs`（合成侧消费时刻）；
  新增 `DumpWindowBindingDiag()`，每 7.5s（与 WINDOW-REG 同拍）输出每扇可见窗口一行：

```
STEAM-WINDOW: window=(ownerHostPid,wlSurfaceId) toplevel= geometry=WxH+X,Y visible=
              binding=bound|none producer=0x<producerKey> extent=WxH
              producerAgeMs= drawAgeMs= contentSource=VenusNative|SHM|none
              shmFrame= inputTarget= class=OK|SHM|NoBinding|ProducerStall|ProducerDead|CompositeStall|NoContent
```

分类口径（方案 §2/§16）：

| class | 含义 |
| --- | --- |
| `OK` | 绑定存在且 producer/合成两侧都在 1s 内活跃 |
| `SHM` | 无 producer 绑定但有 SHM 帧（普通 Wayland 窗口，属正常） |
| `NoContent` | 既无绑定也无 SHM 帧 |
| `ProducerStall` / `ProducerDead` | 绑定存在但 producer 侧 >1s / >5s 没有新帧 |
| `CompositeStall` | producer 活跃但合成侧 >1s 未消费 |
| `CEFBlackContent`（隐含） | 两侧都活跃但屏幕仍黑 → 归 CEF/runtime 战线 |

## 11.2 实测（当前会话）

```text
STEAM-WINDOW: window=(32501,32) toplevel=6 geometry=1280x800+0,0 visible=1
              binding=bound producer=0x7f3900000019 extent=1280x800
              producerAgeMs=7 drawAgeMs=7 contentSource=VenusNative shmFrame=1
              inputTarget=6 class=OK                       ← Steam 主窗口

STEAM-WINDOW: window=(32094,24) toplevel=2 geometry=1280x21+0,779 visible=1
              binding=none contentSource=SHM class=SHM     ← 任务栏 (原本会被误判 NoBinding)

STEAM-WINDOW: window=(32501,24) toplevel=7 geometry=160x25+-32000,-32000
              binding=none contentSource=SHM class=SHM     ← offscreen 隐藏窗口
```

同时满足两条不变量（方案 §25）：

* `renderWindow (toplevel=6) == inputTarget (6)` —— 该会话内 Render/Input 使用同一 WindowKey；
* 每个可见窗口要么 0 要么 1 个 producer 绑定（`windowBindings_` 排他）。

## 11.3 待续（按方案 §29 顺序）

| Step | 状态 |
| --- | --- |
| 1 Per-Window 黑窗归因快照 (Task A) | ✅ 本轮完成并验证 |
| 2 Persistent ProducerKey→WindowKey (Task B) | ✅ 之前已完成（L0 复用 844+ 次） |
| 3 Native Layer 几何/可见性/Z-order 全继承 Wayland | 部分（几何已走绑定窗口；z-order/clip 仍由 ZC 层自身决定，需收口） |
| 4 Render/Input 同一 WindowKey (Task C) | 诊断已具备（`inputTarget` vs 渲染窗口），需补不变量计数/断言 |
| 5 Popup / Helper 策略 (Task D) | 待做（1×1/0×0 已 fast reject；真实 popup 需独立 WindowKey） |
| 6 ActiveProducer 生命周期 (Task E) | 待做（新 producer 首帧成功后再原子切换） |
| 7 LastGoodFrame (Task F) | 待做（producer 切换期保留上一帧） |
| 8 Steam UI 全功能回归 (G2–G5/G7) | 待做 |
| 9 Steam x64 / x64 CEF | 待做（64-bit capability 已列为下一条主线） |

---

# 12. Task E/F 落地（2026-09-17 14:0x）：producer 接管 + 保留最后帧

## 12.1 实现（`ZcBridge`）

* `WineHuaPresentBinding` 增加 `pending` / `retired` 两个状态位；
* **接管门限（Task E）**：候选窗口若已被其它 producer 占用，只有在"当前占用者已停帧 **>1000ms**"
  时才允许被接管（`BIND-TAKEOVER`）；占用者仍活跃 → 维持独占，避免抢窗口导致旧内容立即消失；
* **保留最后帧（Task F）**：接管后新 producer 标记 `pending`，**旧 producer 的 layer 继续显示最后帧**，
  直到新 producer 出第一帧（`lastDrawUs` 刷新）才把旧绑定置 `retired`（`BIND-RETIRE`），
  旧 layer 随后由渲染器释放 —— 期间窗口不会黑；
* `retired` 绑定在下次解析时直接失效并让出窗口，避免旧 producer 与新 producer 争抢。

> 说明：LastGoodFrame 的"帧内容"本身由渲染器侧的 OHNativeImage 保留（没有新帧时沿用上一张纹理），
> 本次补的是**绑定层的寿命控制**：producer 死掉不等于窗口内容必须立刻消失/变黑。

## 12.2 真机验证（当前会话）

```text
STEAM-WINDOW: window=(38688,32) toplevel=6 geometry=1280x800+0,0 visible=1
              binding=bound producer=0x976f00000019 extent=1280x800
              producerAgeMs=7 drawAgeMs=7 contentSource=VenusNative
              inputTarget=6 class=OK
STEAM-WINDOW: window=(38302,24) toplevel=2 geometry=1280x21 binding=none contentSource=SHM class=SHM
STEAM-WINDOW: window=(38688,24) toplevel=7 geometry=160x25+-32000,-32000 binding=none contentSource=SHM class=SHM
```

→ 主窗口绑定正常、producer/合成两侧 7ms 活跃、`renderWindow == inputTarget`、分类 `OK`。

## 13. 目前已"钉死"的门限（给专家参考）

| 类别 | 门限/判据 | 依据 | 状态 |
| --- | --- | --- | --- |
| helper surface | `≤64×64` → 立即失败，不等待不绑定 | presenter `target wait skipped … reason=degenerate-size` | ✅ 已实现 |
| attach 退避 | 首次 miss 等 **2500ms**；超时后该 key 不再阻塞 | 消除 CEF UI 线程 stall（`killing unresponsive browser`） | ✅ 已实现 |
| 黑窗归因 | producerAge **>1s** Stall / **>5s** Dead；drawAge **>1s** CompositeStall | Task A 快照 | ✅ 已实现 |
| producer 接管 | 当前占用者停帧 **>1s** 才允许接管；新 producer 首帧后才退役旧绑定 | Task E/F（本轮） | ✅ 已实现 |
| 几何 bootstrap | 候选必须 `toplevel + 非桌面根 + 未最小化 + 尺寸精确相等 + 唯一` | 方案 §9 L3 | ⚠️ 仍有"整窗 1280x800 vs 客户区 1280x779"缺一档（待专家定 §10.2 方案 B） |
| 文字（字形图集） | **开关**：`VKR_WINEHUA_GPU_UPLOAD=0` / `_INLINE=0` | A/B 实测（关闭后中文/二维码完全正常） | ⚠️ workaround，正式修法见 §5.4 |
| 字体家族 | 40 条 `Replacements → Noto Sans CJK SC` | VGUI assert 消失 | ✅ 已实现（需产品化） |

**不是门限能解决的问题**（需专家/独立战线）：

* 点击后崩溃 / 文字再次消失 → CEF/guest 侧崩溃：`libcef.dll` 越界读（0x4F61CD4C、0x51E00000）、
  `[early-fault] pc=0`（空指针调用）、faultlogger 的 `ld-musl-aarch64.so.1+0x19e8f0`；
* 对应方案 §26–§28 的 **Steam x64 / x64 CEF** 主线（绕开 32 位 CEF / WOW64 地址空间问题）。

## 13.1 Task D 附带落地（2026-09-17 14:2x）：subsurface 纳入候选

Wine 的菜单 / tooltip / dialog 在这套 compositor 里表现为**父 toplevel 的 subsurface**，
而旧候选规则只收 `hasToplevel` → popup 的 producer 永远拿不到 WindowKey（表现为"菜单/弹窗黑"）。

`ResolvePresentBinding()` 新增 `considerWindow()`：

* `hasToplevel` → 用 toplevel state 的 geometry；
* `isSubsurface && parent->hasToplevel` → **也作为候选**，用自身 committed 尺寸（菜单/弹窗实际大小），
  可见性判据沿用父 toplevel（非桌面根 / 未最小化 / 未被切走）；
* 其余（plain/role-less helper）仍然 fast reject。

这样 popup 会有**自己的 WindowKey**（`(ownerHostPid, 自身 wlSurfaceId)`），不会被绑到主窗口上
（方案 §12 的要求）。

真机验证（同一轮）：主窗口 `class=OK`、`producerAgeMs=1~9`、`renderWindow == inputTarget(toplevel 6)`。

## 10.3 本轮另外两项已确认

* `BIND-CAND` 诊断已加入（拒绝时打印候选清单与 size_matches 计数），本轮样本显示被拒的绝大多数是
  CEF 的小尺寸 helper surface（88x165 / 163x456 / 300x167 …，`size_matches=0`）——**安全失败符合预期**；
* `[VENUS-PRESENT][NCP] callback … fail=540` 段（12:10）显示某 producer 连续失败 540 次，
  与"整窗 extent 无对应窗口"的现象一致（`size=1280x800`）。

---

# 14. P0 回归根因（2026-09-17 18:2x）：readback `p_surface_create` ABI 签名被改回 3 参数

## 14.1 现象与证伪

今天 `rtcheck1` / `cleancore1` / `guard1` 的 core smoke 都出现：

```text
opengl-x64 / opengl-x86: FAIL (startup)
message: OpenGL initialization failed…
         ChoosePixelFormat / WGL initialization is not working in the current build.
```

最初怀疑 `wayland_surface.c` 的 owner token 发布（`NtUserSetProp` + 身份探针）干扰建窗。
把发布逻辑默认关闭后（`winehua-tokenguard-20260917.hap`）现象仍在，**token 探针被排除**。

## 14.2 根因

`WINEHUA_WAYLAND_READBACK=1` 时，WineHua 使用 `winehua_readback_surface_create()` 作为
`struct opengl_driver_funcs.p_surface_create`。该函数指针在 `include/wine/opengl_driver.h`
中的 ABI 是 4 参数：

```c
BOOL (*p_surface_create)( HWND hwnd, BOOL raw, int format, struct opengl_drawable **drawable );
```

但当前工作树把实现和声明改成了 3 参数 `(HWND, int format, drawable**)`。AArch64 调用时：

```text
win32u 传入: x0=hwnd, w1=raw, w2=format, x3=&drawable
3 参数实现:  x0=hwnd, w1=format(实际收到 raw 0/1), x2=drawable(实际收到 format)
```

函数第一行 `if ((previous = *drawable) …)` 于是解引用小的 pixel-format 数值（实测 `si_addr=0x6f=111`），
异常被吞成 `SetPixelFormat` 失败，表面上就是 `SetPixelFormat failed: 0`。

这不是 DXVK / DXGI / Venus / 跨进程窗口问题；`ChoosePixelFormat` 已成功，失败发生在
`SetPixelFormat → p_surface_create` 的入口 ABI。

## 14.3 修复与静态验证

已恢复为 4 参数：

```c
BOOL winehua_readback_surface_create(HWND hwnd, BOOL raw, int format,
                                     struct opengl_drawable **drawable)
{
    (void)raw;
    ...
}
```

重编后的 `dlls/winewayland.drv/winewayland.so` 反汇编确认 ABI 正确：

```text
mov x20, x3        ; drawable = x3
mov w24, w2        ; format   = w2
ldr x23, [x3]      ; *drawable
```

产物：`F:\WineHua\tmp\winehua-readbacksig-20260917.hap`

```text
libs/arm64-v8a/winewayland.so  sha256=dc0721f32a6b5f5e20e0932207555d7eba4f03aa6f560efe5fbbcb2caccdaaf2
libs/arm64-v8a/ntdll.so        sha256=08d54715192ea8d84efeb35db6797438070aefd502009e928800ffc2cb570db6 (未变)
libs/arm64-v8a/win32u.so       sha256=f07a7851b39af79fd2760808e3b2398256cd69663c816c6b217ba4e965b52386 (未变)
```

HAP 已覆盖安装到真机，设备运行时 manifest 已对齐到 `0f35be…`（只换 bundle 库，无需重解压 prefix）。

## 14.4 尚待最后一步

`opengl-x64` / `opengl-x86` 的真机 smoke 复跑必须在 WineHua 到前台后执行。当前
`com.pomelo.vatg`（GTA V）仍在前台（18:19 起持续运行），强行启动 WineHua 会切走其前台，
因此本次没有抢焦点复测。GTA V 退出后第一项动作：

```bash
hdc -t <target> shell "aa force-stop app.hackeris.winehua; sleep 2; \
  aa start -b app.hackeris.winehua -a EntryAbility \
  --ps winehua.mode smoke --ps winehua.suite core \
  --ps winehua.prefix reuse --ps winehua.run_id readbacksig1"
```

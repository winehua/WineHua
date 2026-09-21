# 第三轮现场复核：presenter→窗口绑定在 15:52 断链（2026-09-20 16:15 CST）

> 承接 `AI_HANDOFF_20260920_FONT_INPUT_WINDOWS.md`。本轮**未部署任何行为补丁**，
> 只在**同一世代**做只读采集 + 一次无破坏消息探针。世代：App broker PID 36971，
> 15:50:18 启动（与交接文档里的 30473/33090 不是同一轮）。

## 0. 一句话结论

本轮把"主窗口黑"和"UI 无响应"收敛到**同一条可判定链**：Steam 主窗口的像素只存在于
**另一个进程（CEF gpu-process）**提交的 present surface 上，而该 surface
**没有 toplevel role**，只能靠 `zc_bridge` 的"尺寸唯一匹配"绑到窗口；本轮首次提交时
窗口还是 1280x780（present 是 1280x800）→ `BIND-REJECT size_matches=0`；
窗口 15:53:01.95 才变成 1280x800（此时已是唯一候选），但 **producer 再也没有提交第二帧**
（21 帧后停止），绑定只在 producer 交帧时求值 → 窗口一直停在**全黑的 SHM 帧**上，
CEF 浏览器窗口线程随后被 Windows 自己判定为 hung。

## 1. 同一世代事实（全部为只读采集）

| 观察项 | 值 / 证据 |
| --- | --- |
| App broker | PID 36971，`MW-NAPI Init` 15:50:18；Steam 客户端宿主 39119，webhelper browser 39187，gpu-process 39230，renderer 39384 |
| 桌面根 | tl=6，owner=explorer 39399（15:51:51 的**第二个** explorer /desktop），1280x800，`visible=0` 且此后一直为 0 |
| 主窗口 | `(39187,32)` tl=8，1280x800+0,0，visible=1，`binding=none producer=0x0 contentSource=SHM` |
| 主窗口 SHM 帧 | 合成器落盘 `frame-w1280-h800-own39187-32.raw`：**100% 纯黑**（0/1024000 非黑像素，SERIAL 3） |
| 对照窗口 SHM 帧 | 任务栏 1280x20（tl=2）99.6% 非黑、mean 184；explorer 640x480 98.5% 非黑 → 合成器与 SHM 通路本身正常 |
| 最后一次真实上屏 | `[GL-PERF] displayed=3600 fps=60.57` @15:57:37.424（那是**图形 smoke 进程** 41385 的画面）；此后到 16:14 无任何 swap |
| 屏幕 | `uitest screenCap` 与 `snapshot_display` 两次抓取均为**纯 0 像素**；RenderService 报 `POWER_STATUS_ON`、backlight=257 |

## 2. 绑定断链的时间线（本轮新增的决定性证据）

```text
15:52:54       CEF 创建 "SP Desktop_uid0"（请求 1280x800，maxH=780）
15:52:55.542   producer 0x993e00000013 (gpu-process 39230, surface 19, 1280x800) 首帧
               → BIND-REJECT ... windows=3 size_matches=0 (no unique candidate)
15:52:58.259   登录窗 producer 0x993e00000003 (700x440) 最后一帧（frames=7901，此前后已 bound）
15:53:00±      producer 0x993e00000013 最后一帧（frames=21，此后 ageMs 只增不减）
15:53:01.954   [XDG] tl_set_min_size/max_size toplevel=8 1280x800 → 主窗口变成 1280x800 fullscreen
15:53:06.753   首个 STEAM-WINDOW 快照：主窗口 1280x800 visible=1 binding=none
16:14:08       仍为 binding=none，producer ageMs≈650000
```

候选窗口快照（BIND-WINDOW，去重后）证明 15:53 之后 1280x800 的**唯一**合法候选就是主窗口：

```text
windowKey=0x991300000020 clientPid=39187 protocolId=32 toplevel=8 role=toplevel visible=1
    outerWindow=1280x800 contentRect=(0,0 1280x800)        ← 主窗口（唯一合法候选）
windowKey=0x99e700000003 clientPid=39399 protocolId=3 toplevel=6 visible=0 1280x800  ← 桌面根（排除）
windowKey=0x959f00000003 clientPid=38303 toplevel=1 visible=0 minimized=1 1280x800  ← 旧桌面（排除）
```

即：**绑定规则本身没错，错在求值时机**。窗口几何在首帧之后才收敛，而绑定只在
producer 交帧时求值一次；producer 停止交帧后，窗口永远是黑的 SHM。

## 3. 同世代无破坏探针：Steam 窗口线程确实不 pump

`window-response2.c`（ARM64 原生 PE，占位部署到 `C:\smoke\x64-fex\font-contract-amd64.exe`，
运行后已按原 SHA256 `7f145e4c…` 还原）同一世代结果：

```text
foreground=0x20298
hwnd=0x20298 pid=892 tid=896 rect=0,0,1280,800 hung=1
    abort=0 err=1460 ms=0   noblock=0 err=1460 ms=1001   class=SDL_app title=Steam
hwnd=0x30036 pid=252 tid=256 rect=0,780,1280,800 hung=0 abort=1 err=0 ms=0  class=Shell_TrayWnd
hwnd=0x100DA pid=592 tid=596 iconic=1 hung=0 abort=1 err=0 ms=1  class=ExplorerWClass
```

* Steam UI 窗口（pid 892 = steamwebhelper browser）：`IsHungAppWindow=1`，WM_NULL
  带 ABORTIFHUNG 直接失败（ms=0），不带也超时（1001 ms）。
* 同一世代的 **Wine 任务栏/explorer 线程立即应答**（ms=0/1）→ 输入与消息通路不是整体失效，
  卡点就在 Steam 浏览器进程自己。
* `/proc` 线程快照（12 s 间隔比对）：browser/gpu 全部线程 **0 CPU（空闲等待，不是忙等）**；
  gpu 的 `VizCompositorThread` 仍周期性干活（≈0.37 s/12 s）；Steam 客户端主线程 ≈15% CPU。

## 4. 与 32 位经验的对应（用户提问的落点）

历史证据（本仓库文档，非本轮实测）：

| 项 | 32 位客户端当年 | 当前 win64 |
| --- | --- | --- |
| CEF 后端 | webhelper GPU 命令行是 **`--use-gl=disabled`**（GL 关闭，走非 GL 路径）— `STEAM_WIN64_FEX_ARM64EC_FIX_20260918.md` §11.2/§19.6 | clean 策略：**不注入任何 CEF 后端开关**，CEF 自选 → 本次日志里 gpu-process 带 `--gpu-preferences=…`，即**真 GPU 路径** |
| 主窗口像素来源 | *推断*：client 自己画进窗口缓冲（GDI/SHM 路径）→ 合成器 SHM 通路直接出画（**待验证**：未找到 32 位会话的 FRAME-DUMP/BIND 证据，属"32 位能显示"的机制推断） | 窗口自己的 SHM 帧实测**纯黑**，真实内容在 **别的进程** 的 present surface 上（本文件 §1–§3 实测） |
| 绑定需求 | 主窗口不依赖跨进程绑定（`STEAM_WINDOW_IDENTITY_20260917.md` 的 `windowBindings_` 主要服务 popup/子表面） | 主窗口**必须**靠 present→window 绑定，否则全黑 |

本轮从 `webhelper_gpu.txt` 的 GPU Report（15:51:55 段，当前世代）读到 win64 实际后端：

```text
graphics_backend : Skia/126 be621ea04206d8fae23952783d1d588d6ce0d9b3
GL_VENDOR   : Google Inc. (0x000019E5)
GL_RENDERER : ANGLE (0x000019E5, Virtio-GPU Venus (Maleoon 910) (0x00001000)
              Direct3D11 vs_5_0 ps_5_0, D3D11-65535.65535.65535.65535)
GL_VERSION  : OpenGL ES 2.0.0 (ANGLE 2.1.23105 git hash: 5d4df51d1d7d)
Driver Vulkan API version : Not supported
vulkan      : disabled_off
```

也就是说：win64 走的是 **ANGLE + D3D11（over Venus）**，而历史会话里 ANGLE 反复报
`WGL_NV_DX_interop2 is required but not present`（`angle_platform_impl.cc` 12289）；
32 位当年是 `--use-gl=disabled`（ANGLE/GL 全关）。另一个历史阻塞
（`Browser requested transparent background, but it is not supported`）在
`webhelper.txt` 里最后一次出现是 **14:26:15**，**当前世代没有再出现**——需要确认这是
"软件路径不再触发透明请求"还是"透明/alpha 通路已修好"。

因此"32 位经验"可复用的是**presentation 配方**，而不是某一个补丁：

1. **首选对照实验（无需改代码）**：用现有开关把 win64 也推回 32 位那条非 GL 路径
   —— `WINEHUA_CEF_STEAM_DISABLE_GPU=1`（注入 Steam 自带的 `-cef-disable-gpu`，
   代码注释明确写着这就是 32 位 `--use-gl=disabled` 的复刻；Want 白名单里已允许）。
   若主窗口 SHM 帧出现真实像素 → 直接拿回 32 位行为，绑定问题绕过；
   若仍是全黑 → 说明 CEF 126 win64 的软件合成也不再画进窗口缓冲，必须修绑定。
2. **绑定修复（无论上面的结果如何都值得做）**：producer 的绑定不能只在交帧时求值一次。
   最小改动：窗口 toplevel 变为可见/几何或 role 变化时，对"最近有帧但未绑定"的 producer
   重新求值（`bindDiagProducers_` 已有 extent/frames/lastUs，可直接驱动）。这一条能把
   "首帧早于窗口收敛"这一类时序问题彻底消掉，且不改变现有唯一几何语义。
3. 不建议现在做：改 IPC/同步语义、按"最新 producer"或"排除某窗口"凑绑定（交接文档已列为禁区）。

### 4.1 下一轮（需要一次新 Steam 世代）

当前世代已无法用于验证：屏幕自 15:57:37 起无新 swap，主窗口 UI 线程被 Windows 判 hung，
producer 停帧 20 分钟。要做 32 位对照必须起一轮新的（`GameHook.started` 为真时二次 game Want
会被忽略，所以只能 stop 后重启）：

```text
① 32 位配方对照（单变量，不改代码）
   启动参数加 winehua.d3d_env_keyN=WINEHUA_CEF_STEAM_DISABLE_GPU / valueN=1
   验收点（同一世代内）：
     a. webhelper 命令行/`webhelper_gpu.txt`：应出现 `--use-gl=disabled`，ANGLE/D3D11 不再初始化
     b. FRAME-DUMP：主窗口 SHM 帧是否出现真实像素（当前是 100% 零）
     c. STEAM-WINDOW：主窗口是否 contentSource=SHM 且 class=SHM（不再依赖 producer 绑定）
     d. window-response2：主窗口线程是否不再 hung、WM_NULL 是否即时返回
     e. 抓屏 + 人眼确认
② 若 ① 仍黑：回到绑定修复（最小改动，见上面第 2 点），保留 GPU 路径
③ 若 ① 有画面：以 ① 为可用基线，再单独评估是否值得修绑定以恢复 GPU 路径
```

## 5. 本轮新增/使用的工具与私有证据

* `tools/steam-boundary/window-response2.c` → `artifacts/steam-font-contract/window-response2-arm64.exe`
  （在 `window-response.c` 基础上加 `IsHungAppWindow` + 不带 ABORTIFHUNG 的第二次测量）。
* 私有证据目录 `F:\WineHua\device-evidence-20260920-steam-review`：
  `next1-hilog-36971.log`（15:50–16:03 全会话）、`next2/next3-hilog-36971.log`、
  `next1.png`/`next2.jpeg`/`next3.png`（纯黑抓屏）、`next2-layout.json`（ArkUI 树：全屏 XComponent）、
  `frames-1603/*.raw`（SHM 帧落盘）、`threads-t0/t1.txt`（12 s 线程快照）、
  `window-response2-1615.txt`（探针输出）。
* 分析脚本（临时，未入库）：`F:\WineHua\tmp\{zc_timeline.py,raw_frame_stats.py,dump_layout.py,log_window.py,device-thread-sample.sh,diff_threads.py}`。

## 6. 边界声明

* 没有改任何产品代码、没有改 FEX/字体/GPU 参数、没有清 prefix、没有重新 assemble。
* 探针只发 `WM_NULL`；没有向 Steam 提交任何应用命令。
* 本轮仍是**同一世代同一进程**；producer 为何在 21 帧后停止**尚未证明**
  （候选解释：present 等待被消费、或 CEF 因 resize 重建 swapchain 后停摆），
  需在下一轮用"窗口几何收敛后是否恢复交帧"来区分。
* 屏幕上"整屏纯黑"目前只有抓屏证据；平板实际观感需人眼确认（见交接提问）。

## 7. 第二轮现场（用户手动启动，16:37 起）：绑定成功仍然黑 → 关键缺陷在"绑定活性"

### 7.1 事实（同一世代 48384，全部只读采集）

```text
16:37:29.338  consumer attached key=…643(700x440)  → 连续 frame=120/240/…/1200 signals 同步增长（≈60fps）
16:37:54.782  release key=…643（登录窗结束）
16:37:58.840  consumer attached key=…665(1280x780) → frame=1/120/240/360；用户此时看到主画面
16:38:13.369  [VIRGL-ZC][MAIN] update failed tl=1 update=40601000 transform=-1 failures=1   ← 唯一一次失败
16:38:14.853  frame=360 key=…665 signals=363 failures=1   ← 之后 15 分钟内再无任何 ZC 帧
16:38:16/16:39:13  [GL-PERF] displayed=1800 fps=14.30 → 1920 fps=2.07 以后完全停止
16:38:17~19   pid=52840 的新 surface 60/67/71/73/74/78/79/85 以 **1x1** present
              → BIND-REJECT … bindings=2 windows=3 size_matches=0
16:53:18/26   STEAM-WINDOW 仍显示 binding=bound extent=1280x780
              **producerAgeMs=4~8 drawAgeMs=4~8**（看似活跃）；好友窗 producerAgeMs=904377（15 分钟）
```

另外：主窗口/登录窗自己的 SHM 帧 dump 全部为 `FMT 1 (XRGB8888)` 且 RGB 全 0（100% 黑），
即 native-only 窗口在 SHM 侧只有**全零占位**，没有可供回退的像素。

### 7.2 关键缺陷：`lastProducerUs` 被"消费者查询"刷新，绑定永远不会判定为死亡

* `zc_bridge.h:137 lastProducerUs = 0; // 最近一次该 producer 的 present 被消费`
* 但唯一写入点是 `zc_bridge.cpp:579`（L0 复用分支）与 `:773`（首次绑定）；
  `ResolvePresentBinding` 只被 `GetZeroCopyLayerInfo` 调用，而后者是**渲染循环每帧查询**的路径
  （实测 producerAgeMs ≈ 8 ms = 120 Hz 渲染循环周期）。→ 该字段实际语义 = "渲染器上次查询时刻"，
  与 guest 是否还在 present 无关。
* 同一字段又被用于：`zc_bridge.cpp:693` 的**接管守卫**（`claimAgeMs <= 1000 → continue`）与
  `:884` 的黑窗归因（STEAM-WINDOW class）。因此：
  1. producer 失效/表面被弃用后，绑定永远显示"活跃"，新表面**无法接管**该窗口；
  2. 新表面（本次是 1x1 的退化 swapchain）present 时被拒，guest 侧按既有语义
     （代码注释：无人消费 → 每次 present 等满 2.5s 返回 -EAGAIN → DXVK 视作 SUBOPTIMAL）
     拖慢/卡死，CEF 合成器停摆 → "好友列表加载黑、主画面闪一下就没、UI 无响应"。
  3. native-only 窗口没有像素回退 → 屏幕直接黑（32 位当年内容本来就在窗口缓冲里，没这个暴露面）。

### 7.3 建议的关键修改（单点，先诊断再改）

1. **诊断（只读，~30 行）**：在 `EglRenderer` 里对已 attach 的 ZC 层做一次 readback
   （把导入纹理画进 FBO 读回 or glReadPixels 图层区域），连同 `zeroCopyFrameSignals_/updates_/
   failures_` 落盘。判据：纹理黑+signals 不涨 → guest 侧没内容；纹理正常但画布黑 → 合成/遮挡擦了；
   纹理正常且画布正常 → 上屏路径。
2. **关键修改**：让"producer 活性"来自真实 present（broker signal 时间戳），
   不再在查询路径刷新 `lastProducerUs`；接管守卫/黑窗归因都用真实时间戳。
   这样"新表面接管旧窗口"能正常工作，帧流可恢复。
3. **配套**：`update failed` 时把该绑定标为 stale（允许立即重绑/重 attach），
   并在窗口可见但无新帧时不要持续采样可能已被 guest 复用的旧纹理。
4. **结构性（更大）**：present→窗口 SHM 的像素级回退（等价 32 位行为），
   让 ZC 只做优化而不是唯一像素来源。

### 7.4 本轮排除项（避免无谓修复）

* 绑定几何/唯一性启发式：**本轮 bound 且 60fps 消费过**，不是本次黑屏主因。
* DXVK 2.6：日志明确 `Skipping Vulkan 1.2 adapter … Vulkan 1.3 required`，设备特性不支持（用户已确认不做）。
* CEF 软件模式（32 位配方）：实测窗口 SHM 仍全黑 + `Browser requested transparent background, but it
  is not supported` + 16:25:38 CEF 全家 SIGSEGV；且 CEF 126 报
  `eglCreateContext: Requested GLES version (3.0) is greater than max supported (2, 0)`
  —— 软件路径在本 runtime 不成立。
* `steamwebhelper_dxgi.log` 里反复出现的 `DxgiAdapter::QueryInterface: Unknown interface query
  f0db4c7f-… / 17399d75-…` 属 DXVK-legacy 未实现的 DXGI 新接口（Chromium 在重试），
  记为**候选触发因素**，待 1 的 readback 诊断后再决定是否处理。

## 8. 修复落地（2026-09-20 17:0x–17:2x）：三项改动 + 一条诊断，隔离候选实机验证

### 8.1 改了什么（全部在 App native 侧, 未动 wine/运行时）

| # | 位置 | 改动 | 依据 |
| --- | --- | --- | --- |
| F1 | `compositor/frame/zc_bridge.{h,cpp}` | 新增 `NoteProducerPresent()`（由 native present 回调写入的真实交帧时间表，独立小锁）+ `NoteLayerConsumed()`；`ResolvePresentBinding` **不再**用查询时刻刷新 `lastProducerUs`；接管守卫与 `STEAM-WINDOW` 归因都读真实 present 时间 | §7.2：失效 producer 因查询刷新而永远"活跃" |
| F2 | `compositor/frame/zc_bridge.cpp` | 接管/退役三处修正：① 新增 `PruneStaleWindowBindings()`（清掉指向已消失 producer/资源的窗口映射）；② L3 候选里若 claim 的绑定已不存在则清掉该占用；③ **退役收尾只在窗口仍指向该被退役 producer 时才删窗口映射** | §7.2 + 现场 17:14:02.745 `BIND-RETIRE` 后 17:14:03.332 窗口立刻变 `binding=none` |
| F3 | `graphics/egl_renderer.{h,cpp}` | 消费者自愈：连续 ≥2 次 `update failed` 即 `ReleaseZeroCopyBinding()` 重建消费者（归还队列缓冲、下轮重 attach）；另加保守的"陈旧消费者"重建（从未消费成功 >5s / 出现过失败 >8s） | 现场：`update failed 40601000` 后双方互等（app 取走 flag 不再重试、guest 队列占满） |
| D1 | `graphics/egl_renderer.cpp` | 只读诊断：ZC 层绘制后读回画布中心条带，落盘 `temp/zc-pixel-dump.txt`（首 5 帧 + 每 300 帧 + 自愈后 2s，最多 400 行）；`BIND-PRODUCER-STATE` 增 `presentAgeMs` | 区分"纹理黑"与"合成后黑" |

### 8.2 打包与部署（隔离候选，不覆盖已验证运行时）

`scripts/package_app_candidate.py`：基线 = 已部署字体候选 HAP
（payload `b651fac6…`），**只替换** `libs/arm64-v8a/libentry.so`，其余条目逐字节断言不变；
签名沿用 `sign.py`（输出只进私有日志）。安装后设备
`files/wine/.winehua-runtime-manifest.json` 仍是 `b651fac6…` → **未触发重新解包**，字体修复与
FEX/smoke 载荷保持不变。

| 轮次 | libentry.so SHA256 | HAP SHA256 | 结果 |
| --- | --- | --- | --- |
| v1 | `984ac5b9…` | `b5bd4848…` | 用户实测：**库界面能显示了**；拖动/点击窗口后变黑 |
| v2 | `aa1f4d2d…` | `bff0867d…` | 本轮 |

### 8.3 v1 实机证据（关键：旧机制做不到的事发生了）

```text
17:14:00.207  [MW-ZC] surface created pid=63728 surface=112        ← configure 后 guest 重建 present 表面
17:14:00.250  BIND-PRODUCER: key=0xf8f000000070 extent=1280x800 frames=12
17:14:00.251  BIND-TAKEOVER: window=(63675,44) new=0xf8f000000070 extent=1280x800 (old producer stalled)
17:14:00.251  BIND: producer=…070 → window(63675,44) toplevel=12 reason=unique-geometry-takeover
17:14:02.745  BIND-RETIRE: window=(63675,44) old=…02e replaced_by=…070 (first frame of new producer)
17:14:02.849  consumer attached tl=4 key=273709675839600 source=1280x800 layer=1280x800+0,0
17:14:03.332  STEAM-WINDOW: window=(63675,44) binding=none producer=0x0   ← 被退役收尾误删
17:14:05…07   pixel dump frame=1 signals=1 nonblack=0/16384 mean_luma=0  ← 新 producer 只交 1 帧且为黑
```

* **旧代码在这条路径上必然失败**：`lastProducerUs` 会被查询刷新（`producerAgeMs≈8ms`），
  于是"旧 producer 仍在活跃"→ 永不接管。
* 黑屏的直接原因由 17:14:03.332 那一行给出：窗口映射被退役收尾删除 → 回退到全零 SHM 占位。
  F2③ 正是修这一条（v2）。
* 健康阶段画布确有内容：`pixel dump … nonblack=16384/16384 mean_luma=41..107`（17:11 段）。

### 8.4 仍未解决 / 下一轮观察点

1. **configure 后 guest 只交 1 帧**（`signals=1`）就停：怀疑 2.5s present 等待与 host attach 时延
   （17:14:00.250 present → 17:14:02.849 attach ≈2.6s）互相拉扯导致 DXVK 视作 SUBOPTIMAL 后重建/停摆。
   下一轮用 `presentAgeMs` + pixel dump + `update failed/re-attach` 三组日志判定是 guest 停还是 host 丢。
2. **CEF 子进程 SIGSEGV**（v2 首次启动：renderer 9.9s、ProcessorMetrics 1.1s、gpu-process 1.9s 全部
   signal=11；browser 进程在 `addr=0x6341050e` 反复 SEH 自陷）→ 属 guest 侧崩溃，与绑定无关，
   需要单独一轮（对照 17:09 能出界面那次与本次的差别）。

### 8.5 复现命令（下一轮照抄）

```powershell
# 1) 构建 + 隔离候选 + 签名（容器内）
docker exec -w /data/src/winehua wineohos-build bash scripts/w1-m3-build-hap.sh
docker exec -w /data/src/winehua wineohos-build python3 scripts/package_app_candidate.py
docker exec -w /data/src/winehua wineohos-build bash -lc "export TOOL_HOME=/apps/harmony; \
  source scripts/env.sh >/dev/null 2>&1; python3 sign.py artifacts/app-candidate/app-candidate-unsigned.hap \
  artifacts/app-candidate/app-candidate-signed.hap > /tmp/sign-app.log 2>&1"

# 2) 覆盖安装（不卸载、不清 prefix）
hdc -t 5KPBB25818203996 file send <hap> /data/local/tmp/app-candidate.hap
hdc -t 5KPBB25818203996 shell 'bm install -p /data/local/tmp/app-candidate.hap -r'
hdc -t 5KPBB25818203996 shell 'grep payloadSha256 \
  /data/app/el2/100/base/app.hackeris.winehua/files/wine/.winehua-runtime-manifest.json'

# 3) 默认路径起 Steam 并采集
hdc -t 5KPBB25818203996 shell 'aa start -b app.hackeris.winehua -a EntryAbility \
  --ps winehua.mode game --ps winehua.game_path C%3A%5CProgram%20Files%20%28x86%29%5CSteam%5Csteam.exe \
  --ps winehua.container_id default'
# 之后抓: hilog -x -P <brokerPid>、temp/zc-pixel-dump.txt、STEAM-WINDOW/BIND-* 行
```

### 8.6 v2 实机（17:23 起，broker 5145）当前状态

```text
17:29:16  SP DesktopLoginWindow_uid0-'登录 Ste': WasHidden 0 (700x440)   ← 登录窗出现
17:30:49  STEAM-WINDOW (8273,43) tl=12 1280x780 binding=bound prod=0x20730000001f
17:30:49  STEAM-WINDOW (8273,24) tl=13  300x650 binding=bound prod=0x20730000001c
17:31:02… STEAM-WINDOW 两窗持续 binding=bound（本次没有再现 v1 的 binding=none）
          pixel dump frame=300/600 nonblack=16384/16384 mean_luma=101..103
```

屏幕（抓屏 g7e-after-drag.png）：Steam 顶栏/页签（商店/库/社区）/**页脚**都在，
**网页内容区仍黑**。→ 下一轮把 pixel dump 改成"分层带采样"（顶栏带/内容带/页脚带），
才能判断"内容带在画布上就是黑的"还是"只有内容区被后画的层盖掉"。

本轮未复现用户那次"拖动窗口后变黑"（合成 drag 没抓到窗口标题，日志
`[MW-MOVE] end interactive move tl=0`），所以 F2③ 目前只有代码 + 时间线证据；
需要用户在平板上再拖一次窗口复验。

## 9. 崩溃根因调查（用户要求"不要只做缓解"）——已拿到带进程归属的现场

### 9.1 插桩（隔离候选，只换 2 个 libs 条目）

`ohos_virtual.c`：`[SMC]`/`[SMC] enter` 增加 `pid=`；归属地图配额从"仅 SIGSEGV、每进程 24 组"
改为"每进程前 64 组（SIGSEGV 96 组）"，并同时解析 `addr/x27/pc/lr`。
打包：`scripts/package_probe_candidate.py` —— 基线仍是已部署字体候选 HAP，
**只替换 `libs/arm64-v8a/libentry.so` 与 `libs/arm64-v8a/ntdll.so`**（wine unix builtin 是以
HAP libs 条目发布的，不在 wine-data.zip 里），payload 与 manifest 逐字节不变
（`b651fac6…`）→ 设备不重新解包。候选 HAP `97559727…`（libentry `aa1f4d2d…`、ntdll `ff097457…`）。

### 9.2 实测（2026-09-20 17:48 起，broker 16380）

```text
# browser (pid 17294)：同一对地址反复 SIGBUS(unaligned)，进程活着但卡死
[SMC] enter pid=17294 tid=17294 sig=7 code=1 addr=0x63450d0e pc_in=0x37fff0fad0
      x27_in=0x140618820 x0_in=0x63450d06 lr=0x37fff251fc x16=0xd63f0200   ← CEF 镜像内 guest RIP
[SMC] enter pid=17294 ... addr=0x63270d0e ... （与上一条交替，无限循环）

# renderer (pid 17478)：致命
[SMC] enter pid=17478 tid=17585 sig=11 code=2 addr=0x37bb7bc830 pc_in=0x7cf55bc180
      x27_in=0x37bb7bceb1 x0_in=0x37bb7bc830 lr=0x7cf90b4bf8 sp=0x6de50000
      → result=wine_seh → not_mine                              ← 未处理，进程死
[SMC] enter pid=17478 tid=17478 sig=7 code=1 addr=0x1403039f1c ... x27_in=0x1402ca5621
[SMC] enter pid=17478 tid=17478 sig=7 code=1 addr=0x140331682c ... x27_in=0x1403316819

# 同世代 CEF 退出：18582/18589/18595 在 17:51:06 同一秒内全部 SIGSEGV，
# gpu-process 18966 在 17:51:36 SIGSEGV（与上一轮"GPU 先死 → renderer 批量死"一致）
```

`fault-maps-17478.txt` 里 `addr=0x37bb7bc830`、`x27=0x37bb7bceb1`、`pc=0x7cf55bc180`
**都不在任何映射中** → guest 跑进了未映射内存。注意该 maps 文件是**进程首次故障时**写的
（时间 17:49），对致命那次已经过期——这正是下一步要补的插桩点。

### 9.3 下一步插桩（同一隔离打包路径，改 ntdll.so 一个文件）

1. **致命故障时刷新 maps**：`sig==SIGSEGV` 且命中 `<unmapped>` 时重新读一次 `/proc/self/maps`，
   再解析 `addr/x27/lr/pc` —— 才能把"野地址"落到具体模块/rva。
2. **打印调用来源**：致命故障时 dump 栈顶 8 个字并逐个做模块归属（找 return address），
   同时打印 x0..x7 原始值 → 定位给出野指针的那条调用。
3. **JIT 代码字节**：`pread(pc)` 现在失败（JIT 区不可 pread）；改用 `process_vm_readv(self)`
   或 `/proc/self/mem` 读 pc 前后 64 字节并反汇编，确认 guest 指令。
4. 复现后按"browser 活锁（0x63450d0e/0x63270d0e，guest RIP 在 CEF 镜像内）"与
   "renderer 致命（0x37bb7bc830 未映射）"两条分别定因：前者像 ARM64EC 出口 thunk 的目标
   地址异常，后者像 guest 自身跳入未映射区（需看第 2 步的返回地址）。

### 9.4 插桩落地与第二轮证据（2026-09-20 18:0x，候选 HAP `305b2134…`）

已实现 9.3 的 1/2 项并部署（隔离候选只换 `libs/arm64-v8a/{libentry,ntdll}.so`，
payload/manifest 不变）：

* `ohos_smc_reload_maps()`：配额内每次归属查询都重读 `/proc/self/maps`（原实现每进程只读一次，致命那次的现场早已过期）。
* `ohos_smc_attribute_stack()`：SIGSEGV 时对栈顶 12 个字逐个做模块归属（找 return address）。
* **致命现场优先且不受限流**：`SIGSEGV && x27==0` 时（跳 NULL）先打印 maps 归属 + 邻接 VMA +
  `[SMC-stack]` 栈顶 + 栈字归属，每进程 6 次配额 —— 之前这类现场正好落在限流窗口里被整块丢掉。
* 生效验证：新 build 的 `[SMC] enter pid=` 已出现 4697 行（旧 build 无 `pid=`）。

第二轮（18:0x）抓到的**新证据**：

```text
# renderer 致命：guest RIP == 0 → 调用了 NULL 代码指针
[SMC] enter pid=19670 tid=19670 sig=11 code=2 addr=0x37bb70a0be pc_in=0x7d63a585f0
      x27_in=0x0 x0_in=0x37bb70a070 lr=0x7d63a56430 sp=0x20e90000
[SMC] enter pid=19670 tid=19670 sig=11 code=2 addr=0x40f910b0 pc_in=0x7d6305cbb4
      x27_in=0x14003d4175 x0_in=0x421 lr=0x7d6304bb00        ← addr 落在 FEX 线程状态区

# browser 活锁（同世代、另一进程）：guest RIP 恒定 = libcef+0x618820
[SMC] enter pid=17294 tid=17294 sig=7 code=1 addr=0x63XX0X0e pc_in=0x37fff0fad8
      x27_in=0x140618820 x0_in=0x63XX0X06 lr=0x37fff251fc x16=0xd63f0200   ← blr x16 出口 thunk
      （addr 在 0x6327050e/0x6329050e/0x632a050e/0x6341050e/0x63420d0e/0x6343050e/0x6345050e 间循环）

# 栈现场（新插桩）：
[SMC-stack] sp+000 = 0x7a7a7a7a7a7a7a7a
```

**0x7a7a7a7a7a7a7a7a** 是"栈被 poison 填充"的典型图案 —— guest 的 SP 指到被 poison 过的
内存上（配合 RIP==0 的 NULL 调用），指向"调用链用到了不该用的栈区域 / 栈已被复用或越界"。
这条比"未映射地址"更接近根因。

第三轮（18:08，broker 26110）到 18:11 为止 **0 个 CEF 退出**（崩溃是间歇性的），
需要继续跑着等 NULL 跳转现场出现，届时第 9.4 的致命现场块会给出 return address 与模块归属。

### 9.5 根因收敛：FEX ARM64EC call-ret stack（2026-09-20 18:2x）

第三轮先发现 broker 26110 其实停在启动器页（Steam 没被拉起，`运行中进程` 只有 desktop），
重新下发 game want 后 18:23 Steam 正常起、到 18:28 仍 0 个 CEF 退出（间歇性）。

但当轮抓到的栈现场指向一个**很具体的机制**：

```text
[SMC-NEIGHBOR]     69200000-69400000 rw-p ...
[SMC-NEIGHBOR]     69400000-69402000 ---p
[SMC-NEIGHBOR]     69402000-69c00000 rw-p
[SMC-NEIGHBOR]     69c00000-69c01000 ---p                    [anon:FEXMem_CallRetStacks]
[SMC-NEIGHBOR] >>> 69c01000-6a001000 rw-p                    [anon:FEXMem_CallRetStacks]
[SMC-stack] sp+008 = 0x6ffff00001
[SMC-stack] sp+160 = 0x3e8000003e5
[SMC-stack] sp+312 = 0x101010100000000
```

源码侧对应（`build/fex-src`）：

* `Source/Windows/Common/CallRetStack.h`：每个线程一份 call-ret 栈（两侧 guard page），
  **X17 = callret SP**；`HandleAccessViolation()` 只在故障地址落在
  `[Base-PAGE, Base+SIZE+PAGE)` 时打印 `Call-ret stack inbalance` 并把 X17 重置到
  `DefaultLocation` —— 这是**唯一**的失衡恢复路径。
* `ARM64EC/Module.cpp:893` 在异常路径里调用它（传入 `NativeContext->X17`）。
* 我们的故障行里反复出现 **`addr == x17`**（例如 `addr=0x40f910b0 x17=0x40f910b0`），
  以及 `x27_in=0x0`（RIP=0）与 `x27_in=0x37bb7bceb1`（未映射）两类致命现场，
  栈现场又落在 `FEXMem_CallRetStacks` 邻域 → **call-ret 栈失衡/逃出处理窗口**是最贴合的解释。

**下一步（按顺序）**：

1. 在 call-ret 路径加带界日志：`HandleAccessViolation` 命中/未命中（pid/tid/addr/X17 前后值），
   以及"故障地址 == X17"这一类单独计数 —— 抓失衡发生的确切时刻与当时的 guest RIP。
2. 查 FEX 是否有可关闭 call-ret 栈优化的配置/环境开关；没有就给 ARM64EC 路径做一个"失衡即重置且
   放宽窗口"的候选补丁（只换 `libarm64ecfex.dll`，用同样的隔离打包）。
3. 实机对照：硬化的 FEX 上 renderer 是否还崩（这才是"崩溃根因"的验收）。

### 9.6 验证结果：call-ret 假设**不成立**；致命故障根本不到 Wine 的信号路径（18:3x–18:5x）

按 9.5 先做"可分类证据"（不重编 FEX）：`ntdll.so` 侧新增 `[CALLRET-FAULT]` 分类
——故障地址落在 `FEXMem_CallRetStacks` 区域、或 `addr == X17` 时单独打标（每进程 32 次，不受限流）。
候选 HAP：`ace1b985…`（v4）、`857f505a…`（v5，另把**所有 SIGSEGV 现场**提到限流之前，每进程 16 次，
并复位归属查询计数），payload 均未变。

**两轮回放结论**：

| 轮次 | CEF 退出 | `[CALLRET-FAULT]` | 说明 |
| --- | --- | --- | --- |
| v4（18:41 Steam 起） | renderer 40089 @18:42:11 (8.7s, sig11)、gpu 40921 @18:44:01 (2.2s, sig11)、ProcessorMetrics | **0** | call-ret 假设不成立 |
| v5（18:49 Steam 起） | renderer 43589 @18:50:38 (7.8s, sig11)、gpu 44343 @18:52:30 (2.1s, sig11)、ProcessorMetrics | **0** | 同上 |

更关键的一条：**崩溃进程的致命 SIGSEGV 根本没有经过 Wine 的信号/早故障路径**——
`grep 'pid=43589' wine_stderr` 只有成串 `sig=7`（未对齐，被 FEX 正常处理），
**没有任何 `sig=11` 记录**，而同进程的 `fault-maps-43589.txt` / `[early-fault] #1` 只对应首次故障。
即：**Chromium/CEF 自己的崩溃处理器截获了致命故障并终止进程**（我们的 chokepoint 看到的是"引擎已退出"，
不是"引擎为什么退出"）。这也解释了为什么之前所有 SMC/early-fault 现场都只能看到"崩溃前"的噪声故障。

另：`Steam/dumps/` 里只有 `assert_steam.exe_*` / `crash_steam.exe_*`（客户端自身断言，最新 17:38），
**没有 webhelper/renderer 的 dump** → CEF 侧崩溃报告没有落到那里。

### 9.7 下一步（把致命现场重新暴露出来）

1. **关掉 webhelper 的进程内崩溃处理器**，让故障回到 Wine 信号路径：
   在既有注入点（`wine_child.cpp` / `kernelbase` 追加 CEF 开关）加一个默认关闭的 env 开关，
   注入 `--disable-breakpad --disable-crash-reporter --noerrdialogs`
   （诊断用，等价于"把 Crashpad 摘掉"）。然后 9.4/9.6 的致命现场块（全 SIGSEGV + 栈归属）就能抓到。
2. 或者**让 CEF 写自己的 dump**（`--enable-crash-reporter` + dumps 目录），我再写个小 minidump 解析器
   取异常线程上下文（PC/SP/寄存器）+ 模块表，同样能定位到"哪个模块的哪条指令"。
3. 拿到致命现场后，才是"改哪里"的决策（FEX/Wine/CEF 补丁），并用同样隔离打包做单点对照。

### 9.8 致命现场已抓到（2026-09-20 20:0x，候选 HAP `e7dbea73…`）

9.7 第 1 条实施并验证：

* App 侧 `libwine_child.so` 新增 `apply_steam_webhelper_diag_args()`：当
  `WINEHUA_CEF_NO_CRASH_HANDLER=1` 时给所有 steamwebhelper 系进程追加
  `--disable-breakpad --disable-crash-reporter --noerrdialogs`（默认关闭）。
* 第一次没生效，定位到 **app 的 Want env 白名单把键挡了**：
  `GameTest: ignoring invalid D3D environment key=WINEHUA_CEF_NO_CRASH_HANDLER`。
  在 `GameHook.ets` 白名单加入该键后（连同 `ets/modules.abc`，隔离候选现在带 4 个条目），
  实机可见：

  ```text
  [WineProgram] parsed options exe=steam.exe argc=0 env=1 [WINEHUA_CEF_NO_CRASH_HANDLER=1]
  [WineChild] webhelper diag args injected: breakpad/crash-reporter disabled   (x2, 两个 CEF 子进程)
  ```

**致命现场（renderer pid=2073 tid=2135，早故障 + fault-map + 寄存器）**：

```text
[early-fault] #10 pid=2073 tid=2135 sig=11 code=2 addr=0x6fbb6425dc pc=0x7d6bae0510 lr=0x7efffe02f0 sp=0x691f0000
[fault-regs] x0=0x6fbb6424f0 x1=0x800b39ee8 x2=0xec x19=0x6fbb6425dc x27=0x0 x30=0x7efffe02f0
[fault-map] addr=0x6fbb6425dc HIT 6fbb640000-6fbb645000 **r-xp** 00000000 00:00 0   ← 匿名可执行页
[fault-map] pc=0x7d6bae0510 HIT [anon:FEXMemJIT]
[fault-map] sp=0x691f0000 HIT 691b0000-691f3000 rw-p
```

同一世代其它 renderer 的致命行（`grep 'sig=11'`）：
`pid=2073 addr=0x6fbb6424f0 x27=0x448`、`pid=43589 addr=0x6fbb64346d x27=0x0`、
`pid=43589 addr=0x6fbb64ccb0 x27=0x468` —— **全部落在 0x6fbb64xxxx 这一小段**。

该段在 maps 里的形态（`fault-maps-2073.txt`）：

```text
6fbb600000-6fbb601000 r-xp           ← 匿名可执行
6fbb601000-6fbb640000 ---p
6fbb640000-6fbb645000 r-xp           ← 崩溃地址所在
6fbb645000-6fbb646000 rwxp
6fbb646000-6fbb651000 r-xp
6fbb651000-6fbb652000 rwxp
… （交替的小块可执行/可写可执行，直到 6fbb6c2000）
6fbb700000-6fdb600000 ---p           ← 其后是 512MB PROT_NONE 预留
```

**判读**：`sig=11 code=2 (SEGV_ACCERR)` + 目标是 **r-xp（可执行、不可写）** 的匿名页，
`x0` 也指向同一段；同时 **guest RIP（x27）= 0 / 0x448 之类的非代码值**。
即：guest 在 ARM64EC 下做了"**写代码页 / 控制流跑飞后落到代码页**"的动作，
FEX JIT 内的 host pc 触发故障，Wine 的两个 handler 都没接管 → 进程死。
这与仓库里那套 SMC（self-modifying code）处理路径直接相关，是下一步要查的点。

### 9.9 下一条要回答的问题

1. `6fbb600000-6fbb6c2000` 这组匿名 r-xp/rwxp 小块是谁分配的（guest VirtualAlloc？
   V8/Chromium 代码空间？Steam 自修改 stub？）—— 用 `[fault-regs] + maps + 该页内容` 对照，
   或在 Wine 侧对 `VirtualAlloc(PAGE_EXECUTE*)` 加一次性归属日志（只记录来源模块+RVA）。
2. 致命动作到底是"写 r-xp 页"（SMC）还是"从非代码地址执行"（x27 已被清零）——
   由 `code=2`（ACCERR）+ `x0` 同区 + x27 非代码值判断更偏向前者。
3. 若确认是 guest SMC 写代码页：查 FEX/Wine 的 SMC 路径为何没有接管这次写入（这是修复点）。

### 9.10 与"OHOS RWX 限制"的对应（用户提示，2026-09-20 20:1x）

用户提示"会不会是 OHOS 的 RWX 权限问题，别处有类似 workaround"。核对后**高度吻合**：

* 仓库里已有的机制（`thirdparty/wine-valve/dlls/ntdll/unix/ohos_virtual.c`）：
  * `ohos_jit_enable()` / `ohos_jit_disable()`：OHOS 上 mmap 可执行内存前的 JIT 开关（成对）；
  * `ohos_mprotect_exec()`：先 `ohos_jit_enable()` 再 `mprotect`；
    **若请求 RWX 被拒（OHOS W^X），退化为"去掉 EXEC、只保留 WRITE"** —— 源码注释写明
    "Dynarec unprotect only needs the guest page writable — native execution is in a separate JIT mapping"；
  * `ohos_map_exec_section()`：noexec 文件系统下用匿名 mmap + pread 映射 PE 代码段。
* 文档侧同样的结论：`docs/ARM64_SCHEME3_HEAVEN_CRASH_FIX.md`
  "W^X | OHOS 拒 RWX；SMC 只需把客户页恢复成可写"、"仅当 `wowbox64_in_host_fault` 时，
  `unprotectDB` 走 unix `ohos_mprotect_exec`，恢复 **RW 而非 RWX**"；
  `docs/NOEXEC_MMAP_ANALYSIS.md` 记录了 `prctl(0x6a6974)`（"jit"）与匿名化要求。
* 而我们的致命现场正好是这套 workaround 的**缺口形态**：
  `sig=11 code=2 (SEGV_ACCERR)`、目标是 **r-xp（可执行、不可写）匿名页**、`x0` 指向同段、
  guest RIP(x27) 非代码值；r-xp 允许读/执行、**只拒绝写** → 说明**guest 往代码页写了东西**
  （典型自修改代码：V8/CEF/Steam stub），而"把该页恢复成可写"的动作没有被执行到。
* 现有的 SMC 写恢复是**为 box64/wowbox64（i386 路线）**做的（`unprotectDB` → `ohos_mprotect_exec(RW)`）；
  **FEX/ARM64EC（x64 路线）没有对应的恢复**——这就是要补的那一点。

**候选修复（下一步实现，先插桩再改行为）**：

1. 在 `ohos_virtual.c` 的链式 fault handler 里，对
   `sig==SIGSEGV && si_code==SEGV_ACCERR && 故障页为 r-xp` 的情况做**写故障恢复**：
   `ohos_mprotect_exec(页首, 页大小, PROT_READ|PROT_WRITE)` 后返回"已处理"(retry)，
   等价于给 x64 路线补上 box64 那条 `unprotectDB`。先加**只打日志不处理**的一档做对照
   （记录页范围/次数/进程），确认命中率后再打开行为。
2. 打开行为前必须确认**FEX 的 SMC 失效语义**：FEX 用写保护故障来使转译块失效
   （`SyscallsSMCTracking.cpp`），我们若直接放行写入而不通知 FEX，可能执行到过期代码。
   因此第 1 步的日志里同时记录 FEX 侧是否已有对应处理（例如该故障是否曾被
   `HandleAccessViolation` / SMC 跟踪路径认领），据此决定"我们补恢复"还是"把信号正确转给 FEX"。
3. 二者都只动 `ohos_virtual.c` → 走既有隔离打包（只换 `libs/arm64-v8a/ntdll.so`，payload 不变）。

### 9.11 实测：RWX 机制确认，但"互切权限"不够（v8–v11，2026-09-20 20:2x–20:5x）

按 9.10 做了三步（都是隔离候选，只换 HAP libs 条目，payload 未动）：

| 候选 | 改动 | 结果 |
| --- | --- | --- |
| v8 `44645701…` | `[SMC-CODEWRITE]` 只读对照（写 r-xp 页） | 崩溃前该 renderer **12 次**写 r-xp 页（0x6fbb64xx–0x6fbb68xx），随后 signal=11 |
| v9 `def7dda2…` | 写故障恢复（r-xp → RW 后重试） | `mprotect_rw=0` 成功，但 renderer 仍死；**发现恢复被"缓存的 maps"跳过**（致命页在崩溃时仍 r-xp） |
| v10 `9774bdb2…` | 恢复前**现刷 maps**（修上面这个真 bug） | 恢复生效，但 renderer 仍死；致命 fault 出现**第二类**：`addr` 落在 **rw-p**（执行故障） |
| v11 `797d1838…` | **双向 W^X 互切**（r-x 写→RW；rw- 执行→R+X） | 互切成功（`[SMC-WX-RECOVER] ... rc=0`），但 renderer/gpu-process 仍在启动后 6–10s 死 |

结论：**用户猜测的 RWX 机制成立**——guest 的 JIT 代码页在 OHOS 上只能是 `r-x` 或 `rw-`，
而 guest 需要**同时** W 和 X，于是两个方向都 ACCERR：写 `r-x` 页 / 执行 `rw-` 页。
但**靠"互切权限"修不了**：切过去就制造另一类故障（v11 实测 renderer 先被切成 RW 的页
随后以"执行 rw-p 页"再次 ACCERR）。

另一个关键事实：**同一进程里 RWX 是能拿到的** —— FEX 自己的 JIT 区就是 `rwxp`
（`[anon:FEXMemJIT] 7d6f090000-7d73090000 rwxp`）。说明这不是 OHOS 的绝对禁止，
而是 **guest 的那类分配被 Wine 降级成了 W^X 安全形态**：
`ohos_virtual.h` 里 `ohos_jit_enable()` = `prctl(0x6a6974 /* "jit" */,0,0)`（配对 `…,0,1` 关闭），
`ohos_mprotect_exec()` 在 RWX 被拒时会**主动去掉 EXEC、只留 WRITE**。

**下一步（针对"降级点"而非"故障后补救"）**：

1. 给 `ohos_jit_enable/disable`（`ohos_virtual.h`）与 `virtual.c:2073` 附近的映射路径加**带返回值的日志**
   （prctl 的返回值、请求的 prot、最终 perms），确认 guest 的 `PAGE_EXECUTE_READWRITE`
   分配到底在哪一步被降级、当时 JIT 开关是开还是关。
2. 两条候选修复（按证据选一条）：
   * **进程级常开**：Wine 子进程启动时 enable 一次且不再 disable（JIT 开关是 per-process 的，
     现在的 enable/disable 配对很容易在"guest 真正分配/写入"时处于关闭态）；
   * **双映射别名**（JIT 在 W^X 系统上的标准做法）：同一 backing 映射成 RW 一处、R+X 一处，
     guest 写用前者、执行用后者——彻底避免"同一页既要 W 又要 X"。

### 9.12 降级点实测：**OHOS 并没有拒绝 RWX**（v12 `e6634214…`，2026-09-20 21:0x）

给 `ohos_mprotect_exec()` 加了带返回值的日志（每进程 32 条），记录 prctl 返回值、首次
mprotect 的结果与 errno、是否走 W^X 安全回退。实机结果（Steam 各子进程）：

```text
[WX-MPROTECT] pid=31149 req=0x7 rwx=1 jit_rc=0 first=OK base=0x7ffffe0000 size=16384
[WX-MPROTECT] pid=31149 req=0x7 rwx=1 jit_rc=0 first=OK base=0x7fedfd0000 size=16777216
[WX-MPROTECT] pid=31458 req=0x7 rwx=1 jit_rc=0 first=OK base=0x950000   size=65536     （x6 个进程同形）
```

`req=0x7` 就是 `PROT_READ|PROT_WRITE|PROT_EXEC`：**RWX mprotect 全部成功**（`first=OK`），
`jit_rc=0` 说明 prctl 也生效。→ **"OHOS 拒绝 RWX 导致降级"这一假设被实测否定**：
本 runtime 上 RWX 是拿得到的（FEX 自己的 JIT 区也是 rwxp）。

因此 `r-xp` / `rw-p` 的 guest JIT 页是**别的东西**在管权限 —— 看 FEX 侧源码
（`build/fex-src/Source/Windows/ARM64EC/Module.cpp`）就清楚了：FEX 在
`EXCEPTION_ACCESS_VIOLATION` 上有一条**专门的 RWX/SMC 处理链**：

```c
1) CallRetStack::HandleAccessViolation(...)                      // call-ret 栈失衡
2) JITGuardPage::HandleJITGuardPage(...)                         // JIT 缓冲 guard page
3) InvalidationTracker->HandleRWXAccessViolation(Thread, Pc, FaultAddress)
     → 认得该页就"解保护 + 标记 SMC + 返回 true"，并单独处理 inline SMC
       （重构建上下文后单步执行那条写指令）
   ...（未认领则继续往下走 → 最终 "Passing through exception" / 返回 false 交给 Wine）
```

我们的致命 fault 显然**没有被这三级里任何一级认领**（否则进程不会死），所以下一步是：
在 FEX 这一层插桩（只加日志）回答三件事：

1. `InvalidationTracker` 指针是否为 null（SMC/RWX 跟踪是否根本没建）；
2. `HandleRWXAccessViolation` 对 `0x6fbb6xxxxx` 这类**guest 自己分配的 JIT 页**是否返回 false
   （即 FEX 的 writable-page 跟踪只覆盖 guest PE 代码页，不覆盖 guest 运行时分配的 JIT 页）；
3. 那两级之前是否被 `IsAddressInCodeBuffer/IsJIT` 之类的判断挡掉。

**为什么这条线是"根治"而不是"补救"**：如果第 2 条成立 —— 我们就该把 guest 运行时分配的
可执行页纳入 invalidation/writable-page 跟踪（或在 Wine 侧把那类分配做成 RWX，实测可行），
而不是在故障发生后去猜该给 W 还是 X（v9–v11 已证明"猜"必然在两类故障间来回搬）。

**注**：FEX 侧改动要动 `libarm64ecfex.dll`（在 payload 里），且必须先核对
`build/fex-ec/Bin/` 当前是不是诊断版（交接文档记录过 `89297b45…` ≠ 实机 `8aba586e…`），
构建/打包要走"只换 payload 内单个 dll"的隔离路径。

### 9.13 FEX 身份核对 + 插桩前提（2026-09-20 21:1x）

| 对象 | SHA256 | 说明 |
| --- | --- | --- |
| payload / 设备实机 `bin/aarch64-windows/libarm64ecfex.dll` | `8aba586e…` | **原版**（与保存的 `libarm64ecfex-original.dll` 一致） |
| `build/fex-ec/Bin/libarm64ecfex.dll` | `89297b45…` | **诊断版**（交接文档记录过） |
| `build/fex-src/`（staged 源码） | — | 含 `[FEX-UNALIGNED]` 诊断、Steam trace，**并含 `ThreadTerm` 行为补丁**（`Module.cpp:1221`） |

→ **直接从这份源码重编会静默带回未验证的行为改动**（交接文档明确警告过）。要插桩 FEX，必须先
建立"行为干净"的基线：拷贝 `build/fex-src` 到 scratch → 撤销 ThreadTerm 等行为补丁 → 重建 →
与实机 `8aba586e` 对照（至少确认差异只在日志/诊断），再加**只加日志**的三级插桩。

FEX 侧设计（`Source/Windows/Common/InvalidationTracker.h`）已读清：

* 每进程一个 `InvalidationTracker`（`ARM64EC/Module.cpp:814` `emplace` 创建，非 null）；
* 维护 `XIntervals`（可执行区间）与 `RWXIntervals`（写保护跟踪区间）；
  `HandleRWXAccessViolation(Thread, HostPC, FaultAddress)` **只认自己跟踪区间内的故障**
  → 解保护 + 失效代码 + 返回 true；区间外的故障原样返回 false；
* 另有 `DisableSMCDetection()` / `SMCDetectionDisabled` —— SMC 检测会被主动关闭。

结合 v12 的实测（RWX mprotect 成功、不是 OHOS 拒绝），最可能的真相是：
**这些 fault 不在 FEX 的跟踪区间内**（guest 运行时自己 JIT 出来的页，FEX 从没当成"guest 代码"），
或者 **SMC 检测已被 `DisableSMCDetection()` 关掉**。下一步 FEX 插桩就要直接回答这两点：
对 `0x6fbb6xxxxx` 这类地址打印 `QueryExecutableRange` 结果、interval 命中与否、
`SMCDetectionDisabled` 状态、以及三级处理各自的返回值。

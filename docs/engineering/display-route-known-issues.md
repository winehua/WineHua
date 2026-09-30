# 显示路线已知问题与后续处置清单

> 来源：M1 收尾 whole-branch 评审（2026-09-29，范围 60e26c7..7237771）的
> Minor 缓办项 + 技术悬案 + 操作协议。执行 ledger（git-ignored 工作区）
> 已按流程删除，**本文件是其耐久承接地**。
> 用法：M2 开工时逐条过一遍（§1 是开工前必修/顺手修清单，§2 是排查
> 悬案入口，§3 是必须遵守的操作协议）；修掉一条就删一条，删完本文件
> 即可整体移除。
> 设计 spec：`docs/superpowers/specs/2026-09-27-display-route-x11-wlroots-design.md`
> （§6.3 = M1 实测结论）；M1 验收：`docs/superpowers/specs/2026-09-29-display-route-m1-acceptance.md`。

## 1. 代码埋雷（按触发时机排序）

### 1.4 800×600 三处独立写死（随 M2 分辨率参数化一起收敛）

`ohos_output.c`（输出实际尺寸）、`display_input.c`（注入脚本几何分母 +
xclient_child 窗口摆位耦合）、`SmokeDevPanel.ets`（4:3 aspectRatio）。
注释已互相点名但仍是漂移起点：改分辨率漏一处即"注入整体偏移"或"比例
失配"，且三层分散难一次看全。**修法**：随 M2 PC 模式/多 output 的分辨
率参数化设计单一来源（ohos_output 导出查询，ETS 侧从配置/ready 标记
读），不要现在单独修——M2 改分辨率还得再动一次。

### 1.5 单槽 pending key + 孤儿 release（与 §2.1 绑定排查，不单独修）

`display_input.c`：settle 窗口（焦点切换后 40ms）内的待发键只有**一个**
槽位（`g_pending_key`），第二个键覆盖第一个（第一个永久丢）；press 记
入 `g_held_keycode` 后若按键因失败路径未发出（如 settle 期焦点 surface
销毁、DeliverPendingKey 无焦点直接 return），release 定时器仍会发出
无配对的 release。**处置**：不凭空加固——按原则 14 先拿可复现时序，
与 §2.1「最后一键」排查绑定；修的时候把两处一起治（队列化 pending、
release 与 press 配对所有权）。

## 2. 技术悬案（M2 排查入口）

### 2.1 「最后一键」间歇不入编辑控件

T5 起挂起：脚本末两键 H/I 四轮真机 2 轮全成、1 轮只有 "h"、1 轮全无；
注入侧日志逐字节同形，差异在 wine 进程内部非确定路径。嫌疑：① §1.5
的单槽机制；② caret blink/damage 竞态；③ 模态关闭后 edit 重聚焦时序。
**嫌疑④（记事本「错误」弹框抢焦点）已随 §2.8 的修复消失**：用例不再喂参数给
记事本 ⇒ 弹框不再出现；修复后实测一轮 H/I 均落文本区（"abchi"）。该症状本身
仍未复现定论（一轮不成证据），下次复现按下方武器查。
**排查武器已就位**：`smoke/jobs/displayroute-notepad.json` 重放自带
`WINEHUA_WINEDEBUG=+win,+x11drv,+event`，复现时 wine_stderr 有逐键完整
轨迹（T5 当时缺的就是它）——一旦复现，哪一跳吃键有据可查。

### 2.2 两条 GL 路线回读带宽差 16 倍的机制（M2-T3 后改形）

T6 实测：同 guest mesa virpipe、renderer 串相同，wayland-EGL 读回
19.2MB/s vs「X 路线」310.9MB/s。**M2-T3 实测推翻了其中一项前提**：那条
「X 路线」没有走 X——winex11 的 GL 段在本构建里根本没编入（见 2.4），
WGL 落到 win32u 通用 EGL 驱动 + FBO，无 GLX、无 drisw、无 X 窗口参与。
所以 16 倍差不是「drisw 本地 shadow vs vtest 往返」的路线差，而是两条测
量各自的读回管路差。**待查**：两边具体差在哪一段（嫌疑：wayland 侧读回
经 WineHua 呈现/读回机制，FBO 侧走 virgl 自身 transfer）。**顺带作废**：
「X 路线 310MB/s ≈150fps」不能作为 X 路线 GL 能力或性能的依据，R1 的改
判理由不成立（见 2.4）。

### 2.3 引擎冷启 wineboot 偶发 box64 SIGSEGV 崩溃循环 / 停滞

T5 期 t5q 一例：同套二进制 force-stop 重试一次即成；M1 期未复现。位
于引擎启动路径（wine+box64），本分支代码之外。**复现时先保现场**：
hilog 全量落盘 + stderr 只截尾部 ~1MB（t5q 整拉 744MB 失败教训），再
重试恢复。M2 期复发 ≥3 次升级专项（方向：box64 dynarec 对 wineboot 某
代码段的翻译）。

**变体（M2 收尾，2026-09-30）**：core 首轮 r20260930-064926 停在第一个
用例（opengl-x64 结果停在 STARTED，app 与 wine 子进程全 idle、无自旋，13
分钟不终态），重跑 r20260930-065058 **4/4 PASS**。与上面同形（冷启期），
区别是「停滞」不是崩溃 —— 同样按「保现场 + 重试」处理；判据：设备侧进程
CPU 全 idle 即不是自旋死锁，先别当代码 bug 查。

### 2.4 X 路线 GL 呈现：走私有通道（2026-09-30 落地；GLX 补栈为被否决方案）

**现状（已修，实测）**：`winex11.drv` 增私有 present 驱动
（`dlls/winex11.drv/opengl_winehua.c`，由 `X11DRV_OpenGLInit` 在无 GLX 时装入，
通道未武装时返回 `STATUS_NOT_IMPLEMENTED` 退回上游行为）。它保留**已经跑通的
surfaceless EGL 渲染**，只把 drawable 从 FBO 换成 **pbuffer**（有 display target
才有真正的 present 语义——这正是 T3 实测「`WINEHUA_VTEST_FRONTBUFFER_LOG` 一次未
写」的原因），swap 时发布 **X window id**（与 Vulkan 私有面同一把钥匙），宿主
presenter 把该纹理 blit 进同窗 scene 节点。

- 判据（设备 .5，`smoke/jobs/displayroute-gl-baseline.json`）：
  RED 基线 `frames=3028 producerFps=447 displayFps=-1.0` + 程序侧
  `no compositor display sequence observed`（r20260930-014632 系）；
  GREEN `frames=849 producerFps=117.2 displayFps=117.07`，
  `opengl-x64 PASS`（job-r20260930-234637）——`displayFps` 由宿主发布显示序列
  （见 §2.9）后才有值。
- 宿主侧配套：`display_guest_frames` 的面发现从「只收 vulkan=1」改为两类都收
  （virgl 面走 `SurfaceQueueTarget`，与 wayland 路线同一个 present 目标），挂接
  仍以「X 窗在册且已 associate」的锚判定把关。
- 行序：wlroots 消费**不带任何采样变换**，而 wayland 渲染器的采样变换是
  `flipY = vulkanSource`（`graphics/egl_renderer.cpp:329`）⇒ GL(virgl) 面要显式
  翻一次、Vulkan(venus) 面不翻。已在 `wl_ohos_output_client_frame_set` 按面类型
  下发（`WL_OUTPUT_TRANSFORM_FLIPPED_180` / `NORMAL`）；判反的症状是图像上下颠倒。
- id 发布/就绪握手从 wayland 的 readback 文件提取为共享实现
  （`dlls/win32u/winehua_present.c` + `include/wine/winehua_present.h`），两条
  路线共用一份（原语对齐 Mesa 侧的页格式，见下）。

**T3 的定位仍然成立**（它解释了为什么必须换 pbuffer）：X 路线无 GLX ⇒ WGL 落到
win32u 通用 EGL 驱动，`egldrv_surface_create` 造 **FBO drawable**，
`framebuffer_surface_swap` 是**空实现**（`dlls/win32u/opengl.c:407`）⇒ 帧进 FBO、
没有任何真 present。原来的三件套「补栈清单」（① Xwayland `-Dglx=true`；② guest
mesa 出 `libGL` + `GL/glx.h`；③ 撤 `WINEHUA_ALLOW_X11_NO_GLX` 重编 wine）**未采
用**：guest 是 box64 翻译的 x86_64，而 GLX 直通要求 guest 与宿主同架构共享
DRM/GEM（DRI3），在该形态下结构性不成立；私有通道复用宿主已在用的
presenter 与 id 键，代价与风险都更低。**未做**：`dx-glx-present` 的判据改造
（程序侧 CRC 序列 + 主机的区域裁剪判定）——`opengl-*` 已用宿主显示序列门替代。

T3 的实测证据（保留，它是「必须 pbuffer 而非 FBO」的判据）：

| 环节 | 现状 | 证据 |
|---|---|---|
| Xwayland GLX 扩展 | 无（`-Dglx=false -Dglamor=false`，M0 shm-only 决定） | `scripts/build_xwayland.sh:172` |
| guest libGL（GLX 客户端） | 无：guest_gfx 只有 EGL/GLES/gallium + `dri/swrast_dri.so` | 设备 `guest_gfx/lib` 清单 |
| winex11 GL 段 | 未编入：`WINEHUA_ALLOW_X11_NO_GLX=1`，configure 拿不到 `GL/glx.h` → `X11DRV_OpenGLInit` 落 `#else` | 运行期 `display_funcs_init Failed to initialize the driver OpenGL functions, status 0xc0000002` |

状态码是判据：上游 stub 返回 `STATUS_NOT_IMPLEMENTED`(0xC0000002)，真实的
libGL 加载失败返回 `STATUS_NOT_SUPPORTED`(0xC00000BB) 并附 ERR 行——实测是前者，
故为编译期缺件，非运行期缺库。私有通道下该状态码的含义不变：**通道未武装时
本驱动同样返回 `NOT_IMPLEMENTED`**，行为回到上游（不出图）。

**实际行为（修前）**：WGL → win32u 通用 EGL 驱动 → FBO drawable、swap 空实现 ⇒
5568 帧 @542fps、`WINEHUA_VTEST_FRONTBUFFER_LOG` 全程一次未写（winsys present
从未被调用）、X 窗口零 damage、固帧四象限始终不出现。**X 窗口 → scene → 输出
→ XComponent 这一段一直是好的**（注入测试窗正常出图并动），缺口只在 GL 客户端
出图这一跳。

**残留判据债**：`visual:rgba-quadrants` 对**全屏截图**做四象限，而 displayroute
的出图面是侧栏里的**预览小框**（`SmokeDevPanel` 的 XComponent，4:3、约 500×390
物理像素）⇒ 该判定器在本场景失效（`opengl-x64` 跑 X 路线时它没有下判定；出图
与否目前由**宿主显示序列门**（§2.9）与人工目视兜底）。按区域裁剪的视觉判定器
未做。

### 2.5 dxvk 套件的 `dxvk-legacy`（d3d11-smoke）既有失败（M2-T6 期间实锤）

**现象**：`dxvk-legacy-x64` FAIL，报文 `D3D11 initialization or required
feature contract failed`；设备端 metrics 给出失败点：`featureLevel=11.0`、
`adapter="DXVK Vulkan"` 都已拿到，`presentResult=-2147024809`
（`E_INVALIDARG`）、`presentFrames=0` ⇒ 卡在
`D3D11CreateDeviceAndSwapChain`（`programs/winehua_d3d11_smoke/main.c:5249`）。

**判据（三条，缺一不足以定性）**：
1. **与显示路线无关**：wayland（r20260930-063529）与 X 路线（r20260930-063322）
   同构建同报文；
2. **不是静态能力拒绝**：同参数同调用的 `d3d-switch-cube`
   （`BufferCount=2 / R8G8B8A8_UNORM / RENDER_TARGET_OUTPUT / DISCARD / windowed`，
   与 smoke 逐字段相同）在两条路线上都 PASS（`cube rendered and presented`）；
   **⚠️ 但这条判据的前提从未被仪器化（2026-09-30 补注）**：cube 的建链带三级
   降级梯子（HARDWARE+BC=2 → HARDWARE+BC=1 → WARP，`smoke/winehua_d3d_switch_cube.c:723-743`），
   结果 JSON 只记 `initHresult`，**不记停在哪一级**；其 `app_log` 写 CWD 的
   `wined3d_switch_cube.log`（不可写 ⇒ 从未落盘）。即 cube 的 PASS 可能是降级
   档拿到的，"同参数同调用能过"在坐实之前不能当证据用。
3. **与 M2-T5 无关（对照实验）**：wine fork stash 回 `a46a545169c`（T5 wine
   侧改动全部移除）后完整构建 + 卸载重装，wayland 上 **2/2 同样失败**
   （r20260930-064027 / r20260930-064134）。

**这不是"天生如此"（2026-09-30 归档复盘）**：
- **8/5 有 PASS 记录**，但是**另一台设备（910）**上的（`docs/archive/evidence/
  VKD3D_DXVK_REGRESSION_910_20260805.md`：`dxvk-legacy-x64/x86` 各 60 帧、
  present 成功、DXVK 也是 1.10.3，测试程序与今日同一份）；
- **测试程序自 8/1 起一行未改**（wine fork `programs/winehua_d3d11_smoke/main.c`
  无提交）；DXVK submodule 自 8/9 起未变。⇒ 变的是**栈或设备**。
- **当前这台设备（.5，MOR-M1 2in1 笔记本）上从未见过绿**：归档里 6 条记录
  （r20260930-063232/063322/063529/064027/064134/205638）全部同一报错；平板
  （.6）期间没跑过该用例。⚠️ 设备换过的时点：9/27~9/30 01:15 用平板，02:27
  起换成笔记本。

**两个开放假设，15 分钟可分开**：
- **A 设备特定**（这台笔记本的 GPU/驱动下，私有 WSI 建链被拒）：同一构建在
  **另一台设备**（平板 .6 在线；910 不在线）跑 `dxvk-legacy-x64` —— 过 ⇒ A 成立。
  头号候选差异：smoke 在 `WS_OVERLAPPEDWINDOW`（640×480 **外层**尺寸）窗口上
  请求 640×480 **客户区**建链，cube 走 `WM_SIZE` 后按**客户区**尺寸建链
  （`smoke/winehua_d3d_switch_cube.c:1046-1066` vs `winehua_d3d11_smoke/main.c:5233-5251`）。
- **B 8/5 之后的栈回归**：若是 B，按时间二分。候选提交（wine fork）：
  `b7f43d4bd96` 8/10 CDS 分辨率模拟 / `037984bdc80` 8/15 present_rect /
  `6569cd8dbd9` 8/20 上游 merge / `3ce65ee4225` 9/14 最外 1px 圈 /
  `cf39fdf1df6` 9/16 窗口尺寸锁死修复。

**快复现器**（做 A/B 都先用它，避免每次重建整个 wine）：把 cube 改成 smoke 的
精确复现——去掉梯子 + 采用 smoke 的建窗方式（`thirdparty/wine/` 下改一次 =
整条 Wine 构建，二十分钟起；cube 是宿主编译，秒级迭代）。

**已排除的假线索**（防下次绕路）：两份结果 JSON 里 DXVK 模块路径不同
（`legacy/x64/d3d11.dll` vs `legacy_x64_d3d11.dll`）是 smoke 的
`safe_json_text()` 把 `\` 替换成 `_` 所致，**不是两份 DLL**。

**影响面**：DXVK 矩阵的 cube 用例与 vkd3d 侧不受影响（M2-T6 出图判据由 cube
兑现）；它只影响 d3d11-smoke 这一条深度用例。**本轮（2026-09-30）裁决：用户
决定先跳过**——按上面两步入口留档，不投入本轮。

### 2.6 guest 帧归还的 GPU 同步由 present 侧 glFinish 提供（2026-09-30 定性更正 + 不变量检查器）

**原记录（「归还传 fence -1 = 不做 GPU 侧同步，采样可能还在飞」）不成立**：归还
确实传 -1（`ohos_buffer.cpp` ConsumerReleaseQueueSlot），但链路里**不是没有同步**
—— present 路径**每帧 glFinish**：零拷贝分支在 FlushBuffer 前（`ohos_output.c`
HandleOutputCommit「显示消费前必须 GPU 写完」），拷贝分支在读像素前。而归还发生
在**下一帧**替换 scene 节点时（wlr_buffer 引用归零 → ConsumerDestroy）。

次序：本帧采样 → 本帧 present 前 glFinish → 下一帧 set 时归还槽位 ⇒ 生产者覆写
时 GPU 早已读完。**真正的债是耦合**：这条保证藏在两处相距很远、互不引用的代码里，
谁为性能拿掉 glFinish（或换 fence 而不改归还侧）就会**静默**打开竞态。

**已装不变量检查器（本次落地）**：`wl_ohos_egl_finish` 内自增 GPU 同步点计数
（`ohos_egl_import.c`，注释写明「换 fence 必须同时改这里」）；guest 帧交给 scene
时记下那一刻的 present 序号 + 同步点计数；归还时比对并计数（`[GUEST-FRAMES]
stats` 行的 `rel` / `rel_synced` / `rel_unsynced_presented` / `rel_unrendered`）。
`rel_unsynced_presented > 0`（上屏过却没有同步点）= 不变量破损，归还路径同时打
ERROR（限流 5 条）。

**RED→GREEN（设备 .5，X 路线 dxvk 用例，2026-09-30）**：
- GREEN（正常构建）：`rel=201 rel_synced=201 rel_unsynced_presented=0
  rel_unrendered=0`，告警 0 条（job-r20260930-222811）
- RED（临时拿掉 present 前的 glFinish，验完立即还原重建）：215/215 次归还算作
  「已上屏无同步」，告警 5 条（job-r20260930-223143）⇒ 检查器确实会叫

**升级项（性能，未做）**：每帧 glFinish 是 CPU 阻塞等 GPU 的固定成本；可换成
native fence（归还侧与输出 present 侧都能带 fd）。**原语 wlroots 里已有**：
`wlr_egl_create_sync` / `wlr_egl_dup_fence_fd`（`include/render/egl.h`），能力由
`render/egl.c` 初始化时检查（`EGL_KHR_fence_sync` + `EGL_ANDROID_native_fence_sync`）
—— 动手前先打一行启动日志确认本机命中。换 fence 时必须让同步点计数继续自增
（或改检查器判据），否则会被当破损报出来（刻意设计，防静默退化）。

**120Hz 下的实测预算（2026-09-30，帧时钟改 VSync 后）**：`segment scene
render+commit` 在 120Hz 输出下 avg=3.4~3.9ms / max≈8~13ms（含 present，预算
8.33ms）⇒ **当前未超预算，fence 属备选而非必需**；同一时段
`rel=840 rel_synced=840 rel_unsynced_presented=0`（job r20260930-233209 系）
⇒ 高帧率下不变量仍成立。

### 2.7 同 id 复用窗口的「销毁即失效」是按 id 判的，不是按记录判的（窄竞态，未复现）

X window id 会回收复用，而 T5 的失效判据全是**按 id** 做的：
`wl_ohos_output_client_xwindow_alive(id)`（display_guest_frames 每拍 sweep）与
`FindClientByWindow(id)`（frame_set 落点）都只比 `xs->window_id`。于是存在这样
一条时序：窗 A(id=X) 销毁 → 新窗 B 立刻复用 id X → sweep 查 `X` **在册**（那是
B 的记录）⇒ 老绑定不摘；若 A 的 guest 尚未退出、又往老路由键
`(A.pid<<32)|X` 投了最后一帧，该帧会经 `frame_set(X, …)` 落进 **B 的窗口**。

**为什么是窄竞态**：需要「同 id 在一个 33ms 拍内被复用」且「A 的 guest 在窗销毁
后仍投帧」。当前证据面里未出现（present 6/6 + 销毁竞态 4 轮 + dxvk-cube 均无
错帧），**未复现 ⇒ 不修**（竞态类不拿到可复现时序不动手，原则 14）。

**修法（拿到时序后再做）**：失效判据改按**记录身份**——给 `ohos_client_surface`
加进程内单调 generation，`wl_ohos_output_client_frame_anchor` 一并回填，绑定存
generation 并在 `frame_set` 里核对（或直接存记录指针 + generation 防 ABA）。
**先装仪器再修**：绑定创建时存该记录的 generation，每拍比对不一致即计数 + 打
日志（等价于一个不变量检查器），跑一轮有窗口 churn 的用例（notepad 类多 helper
窗口场景最可能命中）看它是否真会触发。

### 2.8 notepad 类用例的「错误」对话框（已修，2026-09-30）

**现象**：跑 `displayroute-notepad`（内联 `m1t5-notepad`）或 displayroute 套件的
`dx-notepad`，屏幕/预览框里会弹出一个标题为**「错误」**的小对话框；用例随后
90s 超时、结果文件不写出（X 与 wayland 两条路线同形，见验收报告红项表）。

**根因（源码 + trace 双向对上）**：用例把 harness 参数交给了**真实的内置记事本** ——
启动行是 `C:\smoke\x64\notepad.exe --automation --run-id … --result …`（guest stderr
可查），而 payload 的 `notepad.exe` 是 wine 内置记事本（`programs/notepad/` 无任何
automation 支持）⇒ 记事本把 `--automation` 等**当文件名**逐个打开、失败，走到
`programs/notepad/main.c:645-650` 的 `MessageBoxW(hMainWnd, …, STRING_ERROR, …)`
⇒ 弹「错误」。X 侧 trace 吻合：`#32770` 对话框、`parent=0x10058`（记事本窗）、
384×319@176,161、`set_window_text 0x10078, L"\9519\8bef"`（=「错误」）。

**修复（2026-09-30 落地，判据见下）**：

1. **载体用例语义**（`judge: "external"`，`smoke/tests` + `SmokeRunner`，
   字段文档在 `docs/engineering/testing-cases.md` §4）：真实应用写不出 smoke
   结果文件，等文件必然拖满 timeout 并被判失败 —— 那是把「无自报」误判成
   「没跑成」。改为：runner 只跑声明时长的窗口，到点 `terminateWineProcess`，
   落一条 `status=SKIP` / `stage=external-vehicle` 的**无结论**结果（设备端不判），
   行为判定归编排与外部证据。SKIP ≠ 绿：它不含「行为正确」的断言。
2. **启动参数**：`argvMode: "raw"` + `argv: []`（套件条目与 job inline 两处）
   ⇒ 记事本**无参数**启动，不再有「文件不存在」弹框。
3. **注入脚本**（`display_input.c`）：删掉应答该弹框的 `KEY_N` 步骤，H/I 直接
   进文本区。

**验收（r20260930-190858 / r191114，设备 .5）**：用例不再超时（SKIP，job 判定
PASS 且两条 bring-up marker 全过）；guest trace 该轮**对话框创建 0 次**；
注入侧日志 evdev=35/23 按时下发；截屏文本区出现 **"abchi"**（A/B/C 因记事本在最上层
盖住两个 X 客户端窗而落进它，H/I 落进文本区 ✓）。

**残留（要门禁才需要）**：行为判定仍是人工/截屏（"abc"/"hi" 是眼睛看的）。
把它变成自动门禁需要新判定器（程序侧帧内容 CRC 或主机侧按区域裁剪的视觉判定），
与 §2.4 那条判定器欠账是同一件事。

**未决观察（同日实测边界，别再重复这一步）**：跑该用例时观察到「对话框画面持续
闪烁」。**未复现**：把当天 4 次运行的 guest 日志分段核对，每次运行该对话框
**只创建 1 次、销毁 1 次**（无窗口管理层高频循环），存活期 ~25s 内 `nc_paint` 5 次、
`show_window` 2 次（≈1Hz 量级）；主机侧 `snapshot_display` 最快 ~1.2s/帧，36 帧
连拍（覆盖 45s）里预览框静止、未拍到该对话框 ⇒ **采样分辨力不足以定性**。
**要定性就装帧级仪器**：合成器收到标记后把连续 N 帧输出落盘（PNG/JPEG），
再比对帧间差异；在那之前不要按「显示链缺陷」修 —— 现有一切证据都指向它是
用例自身的对话框，而非输出链在循环。

### 2.9 帧率上限：宿主的 30fps 节拍 + 硬编码 guest pacing（2026-09-30 修复）

**两个独立的上限**，都不是 guest 慢：

1. **帧时钟自建 30Hz**：`display/ohos_output.c` 用
   `wl_event_source_timer_update(…, 33)` 当帧时钟 ⇒ 输出被钳在 ~30fps。已换
   系统 VSync 主驱动（`OH_NativeVSync`，期望区间 `{60,120,120}`，回调写 eventfd
   → `wl_event_loop_add_fd` 唤醒 event loop；渲染仍在 loop 线程），33ms 定时器
   降级为看门狗/兜底（VSync 停摆 200ms 判定、250ms 看门狗、恢复自动切回）。
   遥测：`rate … ticks=N vsync=N timer=N` 直接看得出节拍来源。
2. **guest 被宿主按 33ms 回压**：`display_guest_frames.cpp` 硬编码
   `kFramePeriodNs = 33ms` 传给 `AttachZeroCopyTarget`，而 presenter 正是按
   `framePeriodNs` 节流 guest 的 present（`kPresentThrottled`）⇒ 即使帧时钟到
   120Hz，guest 仍按 30fps 生产。改为跟随 `wl_ohos_output_frame_period_ns()`
   （VSync 上报的显示周期，兜底 33ms），周期变化时逐面
   `SetZeroCopyFramePeriod` 重发；上报值逐拍抖动几微秒，故只认 >0.5ms 的变化
   （与 `egl_renderer` 同阈值）。

**实测（设备 .5，X 路线，`displayroute-dxvk` 同一 job 前后对照）**：

| | 修前 | 修后 |
|---|---|---|
| cube 帧数（同 job 同时长） | 225 | 857（wayland 路线参照 903） |
| `outCommits`/s | 30 | 111~114 |
| `ticks`/s | 30（timer） | 120（vsync=120 timer=0） |
| `segment scene render+commit` | 3.0~4.1ms | 3.4~3.9ms |

GL 用例同源：`winehua_graphics_smoke` X 路线 `producerFps=117.2 / displayFps=117.07`
（job-r20260930-234637）。

**附带判据（宿主显示序列）**：guest 用例的 `displayed` 门读
`WINEHUA_DISPLAY_FPS_FILE`，此前只有 wayland 渲染器写、X 路线永远缺（判据是
路线外来的）。现在 X 路线合成器在每秒 `rate` 行处发布同一文件（
`common/display_fps.h` + `common/perf_utils.cpp` 一份实现），序号 = 输出提交
计数，**只有该秒真提交过帧才推进** ⇒ 「序号不动 = 宿主没出图」这条判据在两
条路线上一致。

**仍开着**：多窗口/子窗 GL 的落点（子窗帧按窗几何拉伸，无子矩形偏移；
要正确须把 client rect 偏移带进通道或做 guest 侧子窗合成），未做。

### 2.10 同一个 displayroute job，有一次落到了 wayland 路线（2026-10-01，一次，未复现）

**现象**：`smoke/jobs/displayroute-gl-baseline.json`（env 钉死
`WINEHUA_DISPLAY_ROUTE=x11`）连跑三次，其中 **23:57 那次**的 GL 帧被 **wayland
渲染器**取走：guest 日志里是 `WL_EGL: [VIRGL-ZC][MAIN] frame=… key=49147310768154
source=960x540`（key 的 pid/surface 段是 wayland 形态的小 id），
**一条 `[GUEST-FRAMES] attach` 都没有**；而 23:47 与 00:01 两次都是 X 路线
（`[GUEST-FRAMES] attach …` 有，且 00:01 那次带 `flip=1`）。

**为什么危险**：两条路线的宿主消费者在同一个进程里都在跑，guest 用哪条路线决定
帧落到谁手里。**判据会因此说谎**：那次运行 `displayFps=39.7`（wayland 渲染器
发布的数字），而 X 路线消费者一帧没收到 —— 只看 `opengl-x64 PASS` 会以为 X 路线
GL 通了。**判 X 路线必须同时看 `[GUEST-FRAMES] attach`/`stats bindings=1`**，
`displayFps>0` 单独不构成证据（X 路线的显示序列是按整个 output 的提交数发布的，
任何 X 面 damage 都会推进它）。

**候选原因（未验证，别按记忆修）**：① 穿透 NCP/Box64 边界的子进程拿不到 per-launch
env（`include/wine/winehua_vulkan.h` 里记过同类现象）；② 宿主两条链的启动/存活时序
让 guest 在解析路线前先摸到了 wayland。**下次复现时的取证**：guest stderr 里的
driver 装载行（`OHOS: display route=x11, loading winex11`）与该进程的 `DISPLAY`/
`WAYLAND_DISPLAY` 实际取值。



### 3.1 hdc -b 热更后必须 sha256 对账

实测（M1-T6，t6d/t6e/t6f 三轮）：`hdc file send -b` 热更沙箱文件后，
app 挂载视图与 portal 视图**按文件不一致**（同一次 push，manifest 新/
exe 旧字节；host 回拉却都是新的），seed 按 manifest 版本比对被跳过不
重播，旧 exe 一直被执行且无任何报错。根因疑似 OHOS 沙箱 FUSE per-file
缓存（未追到平台根因）。**协议**：任何 `hdc -b` 热更**二进制**后，
`file recv -b` 回拉 + sha256 与本地对账；不一致或涉及新 exe 直接走
`bash scripts/package.sh deploy`（卸载重装，HAP 内置载荷 app 自解压，
写读同进程自洽）。对账实例见 M1-T7 两次推送。

### 3.2 套件回归前必须冷启动

displayroute 会话状态残留会污染后续套件的帧采集（t5v、T8 两度复现：
displayroute 任务后直接跑 core，抓到 app UI+虚拟桌面合成帧而非测试
全屏窗口；生产者帧率正常，仅采集面错位）。**协议**：跑 core/wine-vulkan
等门禁套件前 `aa force-stop` + 冷启动。已写入 spec §6.3。

### 3.3 glx-bridge-probe 不得用作回归门

该 case 设计为"只出数据不下结论"（R1 裁决在 spec 层），结果 JSON 在
GL 严重退化时仍 PASS——唯一 FAIL 方式是协议层失败（进程崩/无结果文
件）。它是测量仪不是门禁：**不得加进任何作为门禁的套件**。M2 若需要
GL 读回带宽门禁，新写带阈值用例（如 `readMBps < 100` 判 FAIL）。

### 3.4 测 X 路线内容率必须保证内容源可见

X 面节点的 scene damage 按**可见区域**算：被完全遮挡的窗口不产生 damage。
实测对照（同一 job，只换遮挡条件）：注入 X 客户端双窗被记事本窗（988×741
@0,0，覆盖整个 800×600 输出）全盖时，合成器 `needsFrame/outCommits`=2/s；
移开遮挡后 23~27/s。**协议**：量内容率/帧率前先确认内容源没被别的窗口盖住
（干净入口 `smoke/jobs/displayroute-rate.json`：bring-up + 无窗 guest 程序
占位）；读数配合两行速率仪（客户端 `XCLIENT-STAT` / 合成器 `rate`）交叉看。

### 3.5 视觉判定取的是整屏截图，FAIL 先看归档帧

`snapshot_display` 取**整屏**（含其他应用窗口），判定因此取决于截图瞬间前台是谁。
实测（2026-09-30，新构建重装后首轮 core，r20260930-205405）：`opengl-x64` 设备端
`result-json` PASS（778 帧 / 112fps），主机侧 `visual:rgba-quadrants` 四帧全 FAIL
——归档帧内容是**该程序自己的动画相位**（立方体 + HUD `FPS --.--`），而用例固定帧
（四色象限）只在最后 2s 渲染（`winehua_graphics_smoke/main.c:781-802`，套件
`seconds: 8`）。同构建重跑 4/4 PASS（r20260930-205740：两用例都在第 0 帧采到象限，
四色各 ~3.3 万采样）。与 §3.2 同形（生产者帧率正常、仅采集面错位），差别是本次无
前置 displayroute 任务，触发条件**未复现**（候选：全新安装后的首轮冷启动）。
**协议**：视觉判定 FAIL 时先看归档 `frames/` 再定性——设备端 `result-json` 为 PASS
且帧内容不是固定帧相位 ⇒ 采集面问题，重跑判定，不得记为回归红项。

## 4. 前置条件（场景切换才触发）

- **沙箱安全审计**：wlroots/xserver 补丁含三处在桌面 Linux 语境"看起来
  危险"的改动——`-ac`（关 X 访问控制，peercred 豁免沙箱不生效的实测
  解）、shm fchmod 失败容忍（见 §5）、waitpid 移除（NCP 形态收尸归
  属）。补丁注释均有实测依据；当前威胁模型（单应用沙箱内部）可接受。
  **若方案出给第三方设备/多用户场景，必须先做整体沙箱安全审计**，不
  得带 these 补丁直接出包。
- ~~**帧率债**：displayroute 合成 18.6fps~~ **已清（M2-T4 零拷贝 + 2026-09-30
  实测）**：合成器分段 copy+flush 19011us→1572us、scene 19446us→3202us；
  X 路线持续出图逐跳实测 23~27fps（spec §6.4 速率仪），帧时钟 30fps 跟得上。
  T3 的分段遥测仍留在代码里作对照基线。

## 5. 文档欠账

- spec §6.2/§6.3 未记 M1-T1 的 shm fchmod EACCES 平台约束（OHOS 沙箱
  对 tmpfs 文件 fchmod 返回 EACCES/errno 13，SELinux setattr 拒绝；桌
  面 Linux fchmod 仍成功、日志静默）。证据在
  `scripts/patches/wlroots-ohos-shm-fchmod-tolerant.patch` 注释内。**下
  次提交碰 spec §6 时顺手补一行**——补丁注释是证据正本，spec 是记忆
  层；没有这一行，将来升级 wlroots 重打补丁时可能有人把它按"错误经验"
  修回致命版。
- `WineEnvService.ets` readTextIfExists 格式化 churn 已入史（教训：格
  式变更不与功能变更同 diff），无可挽回动作。

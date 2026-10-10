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

**现状清单（2026-10-01 复核，按符号记避免行号漂移）**：

| 层 | 位置 | 内容 |
|---|---|---|
| 输出（真相源） | `display/ohos_output.c` `wl_ohos_output_chain_start` | `wlr_headless_add_output` / `wlr_output_state_set_custom_mode` / 背景 `wlr_scene_rect_create` / `SET_BUFFER_GEOMETRY`，同一函数内 4~5 个 `800, 600` |
| 注入 | `display/display_input.c` | 归一化分母**已**走查询（`wl_ohos_output_frame_size()`）；但注入脚本里的窗口摆位/点击坐标是按 800×600 画布实测的常量（注释：win1 @0,320 320x240、notepad 坐标=真机实测几何） |
| 界面 | `ets/smoke/SmokeDevPanel.ets` | 预览 XComponent `.aspectRatio(4 / 3)` |

**触发条件（到点必做，不是"有空再说"）**：PC 模式 / 多 output / 要按真机
分辨率出图时。**今天实测的代价面**：真机 3120×2080，我们只在 800×600 画布上
合成再放大进侧栏预览框 ⇒ 分辨率损失（判定侧的比例错配已由 D22 区域裁剪
判定器消解，见 §2.4）。**与阶段 C（每窗直进系统窗）同批做**——
那一批会让"输出尺寸"这个概念本身变成每窗尺寸。

### 1.5 单槽 pending key + 孤儿 release（与 §2.1 绑定排查，不单独修）

`display_input.c`：settle 窗口（焦点切换后 40ms）内的待发键只有**一个**
槽位（`g_pending_key`），第二个键覆盖第一个（第一个永久丢）；press 记
入 `g_held_keycode` 后若按键因失败路径未发出（如 settle 期焦点 surface
销毁、DeliverPendingKey 无焦点直接 return），release 定时器仍会发出
无配对的 release。**处置**：不凭空加固——按原则 14 先拿可复现时序，
与 §2.1「最后一键」排查绑定；修的时候把两处一起治（队列化 pending、
release 与 press 配对所有权）。

### 1.6 虚拟桌面模式在 X 路线不可用（2026-10-04 实锤，产品决策待做）

wine 虚拟桌面把 `root_window` 重定义为桌面窗（`winex11.drv/desktop.c:57`），
此后**所有应用窗口都是桌面窗的 X 子窗口**；rootless Xwayland 的 XWM 只为
根窗口子窗口建 wl_surface ⇒ notepad 等应用窗在 X 路线永不可见。真 X + 桌面
环境能显示是因为合成器把重定向的桌面窗整棵子树（含子窗内容）当作一个面，
本栈没有这层。三个连带症状一次说清（2026-10-04 仪表实测，client-state 见
`ohos_output.c` rate 块）：

- **"全黑"实为暗化底色**：屏显 (17,34,50)，注册表桌面背景
  `[Control Panel\Colors] Background="37 111 149"` 的暗化版——桌面窗的
  buffer 有内容且每秒 +2 commit、Direct scan-out 直通；xclient 测试窗
  同链路色彩鲜正，失色发生在桌面窗自身内容（glamor 路径，mirror buffer
  是 GL/dmabuf，`wlr_buffer_begin_data_ptr_access` denied）。暗化机制
  未再深挖——前提（虚拟桌面）本身已判死。
- **无任务栏不是故障**：wine 虚拟桌面本无任务栏组件（`explorer /desktop`
  只画背景 + launchers，当前启动参数不带 launchers）。
- **fusion（多窗口）模式 X 路线全链路通**（同日实测）：每程序窗 = 根
  子窗口 = XWM 可建面，notepad 上屏 + 键入回显 + 窗口叠层正确。

**触发条件**：产品 UI 选「虚拟桌面」+ 显示路线 x11。**处置**：不是修
 补问题而是产品决策——要么明确「X 路线配多窗口模式」（推荐，fusion 已
 通），要么做子窗合成兼容层（自己枚举桌面窗子树并逐窗建面，大工程）。
 在决策落地前，x11 路线的套件/文档一律按 fusion 语义声明
 （`winehua.desktopMode=fusion`）。

**连带产品缺口**：fusion + x11 的产品会话 X 链路不自启——无桌面页 ⇒
 无人调 `StartWithSurface` ⇒ 引擎退回 wayland 合成器（日志特征：
 只有 `[WL] compositor started OK` 而无 `DisplayRoute: started`），
 notepad 静默变成 wayland 客户端。需要给 wlroots output 接产品侧
 surface 提供方（M3a T3/T4 范围）。

### 2.11 D50：fusion 多窗模式 wine 程序不绘制（M4a-T4 发现，2026-10-10）

**现象**：x11 fusion 多窗模式直启 notepad / GL 探针，窗口
created→attach→呈现链全通但内容全黑；virtual 模式（explorer /desktop
存在）同程序完整渲染。

**已实锤（勿重复排查）**：合成器读到的 client buffer 全 0（fmt=
XRGB8888，stride 正确）；xwayland_stderr `OHOS-damage: win 0x800003
dmg=0,0 0x0 n=1`——wine 从未提交有效 damage（0×0 疑似 XClearArea 类
Expose 请求，wine 在等 Expose）；wl_surface mapped=1；DISPLAY=:0 已
下发（entryParams 实录）。M4a 承载层无罪：呈现链红底实验实证
（pixman→slot→Flush→上屏）。WINEHUA_DESKTOP_MODE 只被 winewayland.drv
消费，winex11 无该 env 分叉——managed/virtual 在 wine 侧的差异只剩
explorer 存在与否，根因在那条线上。

**诊断通道**：`--ps winehua.env 'WINEHUA_WINEDEBUG=+event,+win'`
（EntryAbility runProgram 的 K=V;K=V 通道，88bdfe8）；smoke 套件 env
同可用。判因指纹：A=`+win` 无 CreateWindow 日志（winex11 未达）；
B=有 map 无绘制 + 等 Expose（事件未回）；C=有绘制请求但 damage 0
（XPutImage 分叉）。

**附带缺口（M4b）**：多窗模式 guest GL 帧无消费者——FrameStep
multiwindow 早退跳过 display_guest_frames_tick()；consumer buffer 无
DATA_PTR，per-xs 消费需 EGL 渲染器。fusion_probe（T6）用 GDI 自画绕开。

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
presenter 把该纹理 blit 进该窗的 scene 节点。

- 判据（设备 .5，`smoke/jobs/displayroute-gl-baseline.json`）：
  RED 基线 `frames=3028 producerFps=447 displayFps=-1.0` + 程序侧
  `no compositor display sequence observed`（r20260930-014632 系）；
  GREEN `frames=849 producerFps=117.2 displayFps=117.07`，
  `opengl-x64 PASS`（job-r20260930-234637）——`displayFps` 由宿主发布显示序列
  （见 §2.9）后才有值。
- 宿主侧配套：`display_guest_frames` 的面发现从「只收 vulkan=1」改为两类都收
  （virgl 面走 `SurfaceQueueTarget`，与 wayland 路线同一个 present 目标），挂接
  以「X 窗在册且已 associate」的锚判定把关。虚拟桌面的应用窗是桌面顶层的
  **X 子窗口**，锚定在 D23 补齐：xwm 维护子窗几何表
  （`scripts/patches/wlroots-ohos-xwm-child-geometry.patch`，查询走
  `wlr_xwayland_query_child_geometry`），`ohos_output` 顶层记录查不到时回退
  子窗表（face 节点 + generation 防同 id 复用，周期 sweep 回收）。挂接目标
  类型按面声明（`AttachZeroCopyTarget(..., vulkan)`），不再按全局 d3d 档位
  猜——vkd3d 档位下 GL 面曾被错装 Venus target，每帧 present 收
  kPresentInvalid（2026-10-06 实测 blit=-22）。
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

**~~残留判据债~~（D22 已还，2026-10-06）**：`visual:rgba-quadrants` 原对**全屏
截图**做四象限，而 displayroute 的出图面是侧栏里的**预览小框**（`SmokeDevPanel`
的 XComponent，4:3、约 500×390 物理像素）⇒ 该判定器在本场景结构性失效，X 路线
只能 SKIP。现按区域裁剪判定：`SmokeDevPanel` 在 smoke 运行期间把预览框 on-screen
物理矩形写成 `displayroute-preview-rect.json`（files 根，随 `DISPLAYROUTE_MARKERS`
归档进 device-results/ —— 设备端只产数据），判定器读归档矩形先裁剪再四象限，
X 路线同样出 PASS/FAIL。矩形未归档（老归档 / 面板未挂载）保持 SKIP 语义。
宿主显示序列门（§2.9）继续并行把关「帧是否还在推进」。

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

**新观察（2026-10-01，未查清）**：X 路线 GL 用例每轮稳定出现
`rel_unsynced_presented=3`，且**计数不随时间增长**（962 → 1245 帧期间恒为 3）
⇒ 集中在启动期；resize 与不 resize 两轮都复现（`job-r20261001-034611` /
`job-r20261001-035107`），wayland 路线此前为 0。判定代码见
`display/ohos_buffer.cpp` `ConsumerReleaseQueueSlot`（"上过屏却没有同步点"）。
**待查方向**：宿主挂接前后的前若干帧 in-flight 归还是否走了一条没有 glFinish
的路径（present 路径的 glFinish 只覆盖"提交给 output"的分支）。在查清前，这个
计数在 X 路线上的基线是 3，不是 0。

### 2.7 同 id 复用窗口的「销毁即失效」是按 id 判的（2026-10-01 已按记录身份修掉）

**已修（2026-10-01）**：`ohos_client_surface` 加进程内单调 `generation`，
`wl_ohos_output_client_xwindow_generation()` 把身份给消费者；`display_guest_frames`
的绑定在挂接时记下 generation，每拍（sweep 与 PullFrame 两道闸）按**记录身份**
而不是 id 判失效 —— 同 id 新窗会让老绑定失效（计数 `reused_windows` + WARN），
老 guest 的最后一帧不会再落进新窗。原「先装仪器再修」的方案已并入本次修复：
那个计数器现在就是真仪器（此前 `stale_frames` 永远是 0，见 §2.12）。

原记录（保留作判据依据）：X window id 会回收复用，而 T5 的失效判据全是**按 id**
做的：`wl_ohos_output_client_xwindow_alive(id)`（display_guest_frames 每拍
sweep）与 `FindClientByWindow(id)`（frame_set 落点）都只比 `xs->window_id`。
于是存在这样一条时序：窗 A(id=X) 销毁 → 新窗 B 立刻复用 id X → sweep 查 `X`
**在册**（那是 B 的记录）⇒ 老绑定不摘；若 A 的 guest 尚未退出、又往老路由键
`(A.pid<<32)|X` 投了最后一帧，该帧会经 `frame_set(X, …)` 落进 **B 的窗口**。

**为什么当时不修**：需要「同 id 在一个 33ms 拍内被复用」且「A 的 guest 在窗销毁
后仍投帧」，当前证据面里未出现（present 6/6 + 销毁竞态 4 轮 + dxvk-cube 均无
错帧）。本次按原则 15 落在机制层（记录身份），不再依赖"未复现"这一前提。

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

### 2.10 同一个 displayroute job 落到 wayland 路线（2026-10-01，已定位：CLI 覆盖丢 job params）

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

**根因（已定位并修复，同日）**：不是设备/路线选择，是**判定设施自己的 bug** ——
`automation/smoke.py` 的 `build_job` 把 CLI 覆盖参数写成一个**全新的 `params` 字典
整体赋给 `job["params"]`**，于是 `--seconds`/`--timeout-ms`（以及 `--env`/`--d3d`/
`--dxvk` 同形）会把 job 文件里的 `params.env` 一起丢掉。取证是宿主日志的启动 env
计数：X 路线那次 `env=5 [WINEHUA_DISPLAY_ROUTE=x11;DISPLAY=:0;WAYLAND_DISPLAY=]`，
落到 wayland 那次 `env=2`。**两次「跑偏」都恰好是带 `--seconds` 的那两次**。

修法：`params` 从 job 已有 `params` 起底，`--env` 按键合并而不是整表替换
（提交 `fix(automation): CLI 覆盖参数改为并入 job params`）。**验证**：同 job 加
`--seconds 200` 后启动 env 回到 5 个变量、`[GUEST-FRAMES] attach … kind=gl flip=1`
出现、`displayFps=113.2 ≈ producerFps=111.7`（job-r20261001-000822）。

教训（判据层）：**跑 displayroute 用例时，`--seconds`/`--env` 这类覆盖必须核对
启动 env 计数**；`displayFps>0` 单独不构成「X 路线出图」的证据。

### 2.11 全景 review（2026-10-01）：guest 侧 GL 私有通道 —— 残留项

四个方向并行 review 的产物（完整报告为会话临时物，结论已固化在本节）。
**2026-10-01 当天已修并实测的部分**（提交在 wine/mesa 子模块，验收证据见括号）：

- pbuffer 尺寸冻结 ⇒ 跟随 client rect（`winehua_x11_drawable_flush` 处理
  `GL_FLUSH_UPDATED`，重建 pbuffer + 重绑 context）。验收：resize job
  `job-r20261001-034611` 的 present 日志 size 从 960x540 跟到 632x326，显示
  序列继续推进到 117fps（`smoke/jobs/displayroute-gl-resize.json`）。
- 子窗 GL 不再静默不呈现（id 改取 `NtUserGetAncestor(hwnd, GA_ROOT)` 的受管顶层）。
- EGLSurface 双重销毁（X 与 wayland 两条驱动一起删驱动侧销毁）。
- ready 标记由 X 路线消费者发布/撤销 ⇒ `host has no present target` 假告警消失
  （同一 job 的 guest stderr 里计数为 0）。
- `winehua_present_surface_mapped()` 探针接上（通道断了 vs 宿主没挂接可区分）、
  `.shm` 页退出时删除、ready 探测的 getenv 结果缓存、`STATUS_NOT_SUPPORTED`
  的两种来源显式区分。

**残留（未修）**：

- **子窗落位仍按整窗矩形**（`GA_ROOT` 修好了"有没有 id"，没修"落哪"）：present
  通道只带 surface_id 与源尺寸，没有子矩形偏移字段 ⇒ 子窗内容会占满顶层窗。
  **修法**：present 线协议加子矩形（x,y,w,h）字段 —— 跨仓库契约（mesa ↔
  virglrenderer ↔ 宿主），按 §25 的规矩成对改、升版本、先改文档、跑
  `scripts/check-contracts.sh`。
- **全局 present 锁横跨会阻塞的 swap（PLAUSIBLE）**：`win32u/winehua_present.c`
  的 begin 持锁到 end，临界区包住 `glFlush` + `eglSwapBuffers`；guest mesa 的
  present 是同步请求/应答且带 pacing（重试最多 8 次、按宿主 deadline 睡，单次
  上限 50ms）⇒ 同进程多 GL 窗互相串行化，表现为两窗共同抖动放大。锁本身必要
  （防串窗），问题只在临界区越过了阻塞点。**判据**：双 GL 窗用例，比对 B 的
  封包等待时间是否 ≈ A 的。

### 2.12 全景 review（2026-10-01）：宿主合成器与帧时钟 —— 残留项

**已修并实测**：

- output 窗口 `SET_TIMEOUT 0`（帧时钟线程上不再可能阻塞 3s；两个 presenter
  的对照片是同一写法）。
- 显示序列临时名冲突 ⇒ 每次发布用唯一临时名（此前 `%s.tmp.%d` 仅 pid，
  同进程多发布者互截）。
- `AttachZeroCopyTarget` 早退不再假成功：按**目标身份**判幂等，同窗重挂 =
  幂等，换窗重挂 = 重新下发并留痕（`attach retarget`）。
- `stale_frames` 恒 0 ⇒ 换成按**记录身份**（generation）的仪器，并顺带把 §2.7
  的竞态修对：同 id 新窗会让老绑定按记录失效（新计数 `reused_windows`，命中
  即 WARN + 解绑）。实测一轮 X 路线：`destroyed_windows=1 reused_windows=0
  stale_frames=0`，退役路径正常。

**残留（未修）**：

- **兜底→VSync 交接瞬间的双泵（CONFIRMED，低危）**：同一次 `epoll_wait` 同时
  返回定时器与 eventfd 时，一轮 dispatch 会走两次 `FrameStep`（相隔几百 µs）：
  第二次 commit 被 damage 门控挡掉，但 `wlr_surface_send_frame_done` 无条件发
  ⇒ 按 frame_done 驱动的 guest 多产一帧，`ticks` 多记一拍。指纹：rate 行同一秒
  同时出现 `vsync>=1` 与 `timer>=1`。只发生在每次 VSync 恢复时。
- **帧时钟资源没有释放路径（当前不可达，属埋雷）**：`OH_NativeVSync`/eventfd/
  事件源/定时器全仓无销毁对应物，`g_stop` 无正常退出路径、`chain_start` 每进程
  最多跑一次 ⇒ 今天只泄漏不复用。若将来接上真停止入口，第二轮 `memset(&g_out)`
  会把 `vsync_fd` 清 0，在途回调会向 fd 0（stdin）写 eventfd 计数。
- **小项（均有据）**：拷贝路径 `OH_NativeBuffer_Map` 失败完全静默；`FrameCrc`
  对非 4 倍数长度越界读 3 字节（当前 stride 4 对齐，不可达）；
  `wl_ohos_consumer_buffer_release` 绕过 §2.6 的 rel/rel_synced 计数（全仓无
  调用，是现成旁路）；`VsyncRearm` 失败无限流、`frame_timer` 建失败无日志。

### 2.13 全景 review（2026-10-01）：跨层契约 —— 残留项

**已修**：

- `WINEHUA_VTEST_PRESENT` 值语义两端对齐（mesa 侧新增 `util/winehua_env.h`
  的 `winehua_env_enabled()`：未设/空/"0" = 关；两处调用点改用它）。
- presenter 目标表加上界与淘汰（无目标且 30s 未投帧的条目在查询路径清掉；
  有目标的活绑定不动）。
- 跨路线撞车加一道 id 段闸（guest X 窗口 id ≥ 0x200000，wayland proxy id 是
  小整数 ⇒ 低于该段一律不挂）。
- 路线真相源：`wine_child.cpp` 在"要求 x11 却没给 DISPLAY"时显式 ERR（那种
  组合会落到 null driver，窗口全无），不再只有一行 INFO。
- 手抄常量有检查脚本：`scripts/check-contracts.sh`（vtest WineHua 段、
  present 页结构、display-fps 路径、guest 判定字段，共四组；做了负向验证）。
- `presentedSelf` 的 pid 语义（见 §2.14 ⑥）。

**残留（未修）**：

- **路线仍无宿主侧真相源**：全库没有任何宿主 C/C++ 写 `WINEHUA_DISPLAY_ROUTE`
  或 `DISPLAY`（只在 job env 与 ArkTS 每应用 env），默认 wayland 由
  `wine_child.cpp` 无条件 `setenv("WAYLAND_DISPLAY", …)` 制造 ⇒ 丢了不报错。
  现在的兜底是**判定层**：`presented-route` 用 guest 自报的 requested/presented
  与 job 声明三方对照（§2.14）。要根治得让宿主把生效路线写进可归档的载体。
- **帧几何 = X 窗矩形 vs 帧内容 = client rect**：2026-10-01 实测（顶层窗场景）
  `src=960x540` 与 client rect 一致，未发现偏差；**子窗场景**是已知限制（见
  §2.11 残留第一条）。
- 驱动单槽 CAS 败者静默 free；`__wine_set_user_driver(NULL,…)` 在 wayland
  init 失败时会拆掉已装的 x11drv —— 均为上游行为，未动。

### 2.14 全景 review（2026-10-01）：门禁层

**已全部修完（2026-10-01），逐条列改动与验收**：

①**F1 用例定义加载同源**：run 侧 entries 改从下发的 job 取 suite/tests（此前
只认 CLI `--suite` ⇒ entries 空、用例声明的判定器一条不加载、不抓帧）。
验收：X 路线 job（`r20261001-013542`）出现 `frame: opengl-x64 captured x4`
且三个判定器全部加载。
②**F2 check 与 run 同源**：check 读归档 job.json 且**有 job.json 就只信它**。
验收：`wine-vulkan-r20260930-063302` 由 FAIL 1/4 → PASS 1/1。
③**F3 套件级 checks 取并集**（套件管覆盖、job 管 bring-up 探针）。
④**F9 marker 归档与清理同条件**（都看 `job.displayroute`）。
⑤**F4 全 SKIP 不再记 PASS**（无 FAIL 且有断言通过才 PASS；suite 级通过也算）。
⑥**presented-route 的 x11 归属**改用宿主两侧事实对账：`presentedKey>>32` 与
设备端 `tests[].pid`（运行器记录的 spawn pid）。验收：`r20261001-013542`
`路线一致 (x11) 且归属本进程 (pid=18425)`。
⑦**visual 对 X 路线 SKIP** 并写明原因（§2.4；D22 后按归档矩形裁剪判定，矩形
缺失才 SKIP）。
⑧**F5 归档污染**：归档目录非空即拒绝复用；`poll_run` 按 runId 认领 summary。
⑨**F6 判定新鲜度**：guest 新增 `displayStallMs`（显示序列最后一次推进距结束），
判定阈值 2500ms。**踩坑记录**：固定帧阶段（最后 2s）原 `continue` 跳过了 fps
轮询，导致"没在观测"被算成"没在推进"（实测 stall=2978ms 而 displayed 一路
117fps）—— 已让固定帧阶段继续观测。
⑩**F7 期望路线取 job 声明**：`build_job` 在合并 CLI 覆盖**之前**抄下 job 文件
的 `WINEHUA_DISPLAY_ROUTE` 并下发 `WINEHUA_SMOKE_DECLARED_ROUTE`；判定层在
"呈现 ≠ 声明"时 FAIL（CLI 覆盖可以改实跑，改不掉声明）。
⑪**F10 job/inline 护栏**：job 文件的 `params.env` 与 inline 条目现在也过
`reject_unreachable_env` / `reject_bad_backend`。
⑫**F11 死 job**：`smoke/jobs/displayroute-glx-present.json` 已删（GLX 为被否决
方案，用例早已移出套件）。
⑬**F12 视觉门的 seconds 下限**：run 前拦（声明 visual 且 seconds<2.5 直接 die）；
判定侧在 fixedFrame 缺失时把"时长不足"写进失败信息。
⑭**F8 归档可复盘"谁呈现的帧"**：新增 `device-evidence/`（显示序列文件本体 +
wine stderr + vtest present 日志），已在每轮 run 落盘 —— 本次多项验收证据就取自
它（present 日志的 size 序列、guest stderr 的 resized 行）。

**新发现（本轮实测，未修）**：§2.6 的不变量 `rel_unsynced_presented` 在 X 路线
每轮稳定出现 3 次（resize 与不 resize 两轮都复现，且计数不随时间增长 ⇒ 集中在
启动期；wayland 路线此前实测为 0）。判定逻辑见 `display/ohos_buffer.cpp`
`ConsumerReleaseQueueSlot`：该格交出去后"上过屏但无 GPU 同步"。**待查方向**：
启动期的前若干帧（宿主挂接前后的 in-flight 归还）是否走了一条没有 glFinish 的
归还路径；若是，需要在 present 路径补同步点或调整归还时机。


### 2.15 零拷贝 present 的背景黑帧（2026-10-03 已修：present 路径每帧全幅 damage）

**现象**：DisplayRoute 双窗场景偶发整块背景闪黑。帧 dump 实测：黑区占 71%
（≈ 窗外全部面积），即宿主送出的帧只有窗口内容、背景根本没画；集中在场景
启动后第一秒（frame 14~47，每 5 帧一次）后自愈。设备相关性极强：设备 .5
三次会话 3/3 复现（帧号稳定 14~47、步长 5），设备 .206 全新场景两轮 0 复现
——强堆布局相关。

**与「矩形移动滞留」的关系：无关。** 滞留是测试客户端自身节奏：xclient
mode=2 每 3 帧移 8px（实测每位置保持 97~134ms，端点 210~232ms），合成器
随动 p50 0~14ms（1~2 个显示帧内生效）。M1 时代客户端内容率 ~2fps、画面接近
静止所以看不出跳步；M2-B 内容率修复后 23fps 才显出来 —— 测试资产行为变化，
非管线回归。

**根因**：零拷贝 present 每帧从 NativeWindow 队列借一格 buffer 并新建
wlr_buffer 包装。wlroots damage ring 的跨帧局部记账默认 buffer 由它自己的
swapchain 全生命周期持有；本路径的包装每帧新建、生命周期又被 GPU 在途引用
拉长，记账与物理存储错配时，局部 damage 落到从未画过背景的槽位 = 背景黑帧。

**修复**（`display/ohos_output.c` PresentFrameZeroCopy）：借来的队列 buffer
内容对本帧不可信 → 提交前按全幅 damage 重绘。用显式全幅矩形
`wlr_damage_ring_add`，不用 `wlr_damage_ring_add_whole`（后者从 ring 现存
buffer 取尺寸，ring 为空时是空操作）。静止仍零渲染（needs_frame 门控在前）；
实测 .206 全幅重绘后 segment render+commit avg 3.1ms（修复前局部重绘 4.3ms，
clip 复杂度下降反而更低），max 6.5ms，120Hz 预算 8.3ms 内。

**验收**：常驻门禁 = `DiagPresentFrame` 每帧送屏前背景采样点校验（偏离即
ERROR + framedump 落 PPM）。修复前 .5 基线 8~9 次偏离/会话（3/3）；修复后
.206 rate job 0 偏离 + core 4/4 PASS（r20261003-163023 /
core-r20261003-163312）。**待补**：黑帧只在 .5 复现，治愈判定（修复前 3/3 →
修复后 0/3）要在复现机上做；.5 当前不可达，可达后补。

**伴随诊断开关**（默认关，drive_c 放 marker 文件即开，免重编）：
`diag-present-timeline`（XPOS/PRES 呈现时间线）、`force-copy-present`
（强制拷贝路径对照）、`force-timer-clock`（33ms 定时节拍对照）；
`WINEHUA_PRESENT_DUMP`（异常帧落盘预算）。


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

### 3.6 push 前应用在跑会删不掉 drive_c/smoke

实测（2026-10-01，连跑两轮）：`smoke.py run` 的 push 阶段先删设备上的
`files/.wine/drive_c/smoke` 再校验，应用进程活着时占用该目录 ⇒ 删除失败、
`remove verification failed (still exists)`，本轮直接不跑（不是设备故障）。
**协议**：出现该报错时先 `aa force-stop app.hackeris.winehua`（或直接重跑——
push 前会重新拉起应用并重试），不要在设备上手工 `rm` 后当作已修。同一次连跑的
上一轮能过是因为那时目录没被持有，属于时序，不是版本差异。

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

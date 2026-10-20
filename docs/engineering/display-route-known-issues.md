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

### 1.4 800×600 独立写死（随 M2 分辨率参数化一起收敛）

`ohos_output.c`（输出实际尺寸）。（`SmokeDevPanel.ets` 的 4:3 aspectRatio
已随面板 2026-10-20 退役删除。）
（`display_input.c` 的注入脚本几何分母 + xclient_child 摆位耦合 2026-10-13
随脚本退役整体删除，本层写死不复存在。）注释已互相点名但仍是漂移起点：
改分辨率漏一处即出图/呈现比例失配，且两层分散难一次看全。**修法**：随 M2
PC 模式/多 output 的分辨率参数化设计单一来源（ohos_output 导出查询，ETS
侧从配置/ready 标记读），不要现在单独修——M2 改分辨率还得再动一次。

**现状清单（2026-10-01 复核，按符号记避免行号漂移）**：

| 层 | 位置 | 内容 |
|---|---|---|
| 输出（真相源） | `display/ohos_output.c` `wl_ohos_output_chain_start` | `wlr_headless_add_output` / `wlr_output_state_set_custom_mode` / 背景 `wlr_scene_rect_create` / `SET_BUFFER_GEOMETRY`，同一函数内 4~5 个 `800, 600` |
| 注入 | `display/display_input.c` | 归一化分母**已**走查询（`wl_ohos_output_frame_size()`）；注入脚本（窗口摆位/点击按 800×600 实测的常量）已于 2026-10-13 退役删除，本层无分辨率写死 |

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

### 1.6 虚拟桌面模式在 X 路线不可用（2026-10-04 实锤；2026-10-12 核账：已收口）

**原判据（2026-10-04）**：wine 虚拟桌面把 `root_window` 重定义为桌面窗
（`winex11.drv/desktop.c:57`），应用窗都是桌面窗的 X 子窗口；若按 rootless
per-window 建面（XWM 只为根窗口子窗口建 wl_surface），桌面窗子树里的应用
窗不可见。

**收口事实（2026-10-12 核账）**：虚拟桌面不走 per-window 建面，改走
**桌面窗画布**（managed）呈现——桌面窗整体经画布上屏 + 自绘呈现自泵
frame done（D50），per-window 映射只服务 fusion 形态。虚拟桌面功能面
（双击 #137、滚轮 D29、退出 D45、装饰 #130）均已收口并由套件守门：
**21 套件钉 virtual 为基线、2 套件钉 fusion**（#144；原「一律按 fusion
语义声明」的处置已推翻）。x11 × virtual 链上 core/opengl 四象限固定帧
判定四轮 PASS（gate r20261012-000708…000926，判据面见 §2.12）。

**连带产品缺口（fusion + x11 产品会话 X 链不自启）**：已由 D8（产品路径
x11 链早启）收口——现冷启日志 `displayRoute applied: x11` 必现
（2026-10-12 gate capture 实测，四轮冷启全命中）。

### 2.11 D50：fusion 多窗模式 wine 程序不绘制（M4a-T4 发现，2026-10-10 破案）

**现象**：x11 fusion 多窗模式直启 notepad / GL 探针，窗口
created→attach→呈现链全通但内容全黑；virtual 模式（explorer /desktop
存在）同程序完整渲染。

**根因（已修复）**：multiwindow 呈现路径绕过 wlr_output（手动
render pass + OH_NativeWindow present），没人回发客户端 frame
callback。Xwayland 的 damage 提交被 pending frame 节流——xserver
xwayland-screen.c block handler 里 `if (xwl_window->frame_callback)
continue`，post_damage 首次 commit 时 `wl_surface_frame` 挂上回调后，
后续全部 damage 永久卡在 `continue`，直到合成器回发 frame done。
结果：每窗只 commit 一次（map 时的初始全 0 buffer），内容永不更新。
normal 路线无此问题——wlroots 场景图输出循环自动回发
（ohos_output.c 的 g_clients frame done 泵）。

**证据链**：wine 侧绘制完整正常（`+event,+win` 诊断轮：Expose 到达 →
BeginPaint → `window_surface_flush` 全窗 dirty）；host 侧
attach→render→present ok 只跑一次后 commit 永不再来（dirty 恒 0）；
修复后同一场景 render 894 次 / present 447 次，notepad 完整绘制
（菜单栏/编辑区/状态栏），z 序正确压在 explorer 桌面窗之上。

**修复**：`x11_toplevel_render_tick` 开头对全部活动 xs 泵
`wlr_surface_send_frame_done`（每帧，FrameStep 90Hz 时钟）——角色
等价真合成器的 output frame。任何绕过 wlr_output 的自绘呈现路径都
必须自己泵 frame done，否则 Xwayland 客户端首帧后全部冻结。

**判因教训**：「buffer 全 0 + damage 0×0」最初被解读为「wine 在等
Expose」（指纹 B/C），方向性错误——wine 侧证据（flush 全窗 dirty）
当时即可排除，二手推断（XClearArea/Expose 机制）拖了两天。断点类
问题按「现场最后一条日志 = 最后一跳」分层取证，每跳拿本侧硬证据。

**附带缺口（M4b → 2026-10-11 已修，MW-GPU）**：多窗模式 guest GL/Vulkan
帧无消费者——FrameStep multiwindow 早退跳过 display_guest_frames_tick()，
present target 永不挂（`target missing`）；且 fusion 无 g_clients，帧路由的
client_frame_anchor / client_xwindow_generation 恒查空（[GUEST-FRAMES] 0 行）。
修复（97811a7）：多窗分支补 tick；两查询在 multiwindow 下转接 XTL 在册表
（记录身份序号 generation 新增）；帧消费 = x11_toplevel_set_guest_frame →
render_entry 优先画 guest 零拷贝帧（flip 口径与 D23 同款：GL 翻 / Vulkan
不翻），GDI 窗仍走 surface->buffer。设备验证（SLG-W10，x11+fusion，
dxvk_modern_2_6）：switch cube attach `kind=vulkan flip=0` 持续 ~90fps
渲染 3.5min+（截屏出图）；GL 探针 `kind=gl flip=1` 出图方向正确。
首版逐帧纹理只建不毁在 ~50s 触发系统内存清理（2906 张 GL FBO 泄漏，
memmgrservice 批量写 OomScoreAdj → ability 销毁）；修复（1413406）：纹理
随帧入 entry、set 时建、替换/entry_remove/session_reset 三路 destroy+unlock
配对。fusion_probe（T6）GDI 自画绕开的历史口径随之作废。

**附带发现**：managed 模式 wine user32 的 `get_desktop_window` 会自动
spawn explorer /desktop（非 wine_launch 分支行为）；其 desktop 窗也是
managed xs 并参与呈现，z 序由点击激活驱动（实测 notepad 正确置前）。

### 2.12 M4a 收口：fusion 多窗验收全绿与已知边界（2026-10-10）

**验收状态**：x11-fusion 套件（fusion_probe_x64）判定 PASS——双窗按
guest 坐标并排呈现、byTitle 按窗点击命中（a=1, b=0 不泄漏）、路线自报
x11；T7 回归 displayroute-win32-interactive 有效 20/21（唯一缺口 =
keyboard char-count，XIM 已知）+ core 3/4（opengl-x86 = D49 已知）。

**收口修复**（D50 之后的三件，指纹与根因见各自提交）：
承载层定位（fusion 走 FusionWindowManager 子窗而非
WineWindowAbility——修复必须落在实际承载路径，7d340f7）；title 异步
时序（created 落地补发，3816df1）；byTitle 按钮码 evdev 契约
（9cc6878，日志指纹 `[PIPE] ptr btn=0x1` + `inject button evdev=0x1`
同现即按钮码未映射）。

**判据不适用**：fusion 模式无 desktop shell（per-xs 承载窗，无
DesktopAbility 画布），§2.9/D45 的「退出桌面」类判据与
closeX11Canvas 路径不适用——fusion 会话的清理语义 = 最后一个承载窗
销毁即回主页面。后续在 fusion 会话上复用 D45 判据会得到假 FAIL。

**判据缺口补记（2026-10-11 实测，r20261011-225150）**：core 的
opengl 两项 displayed 门依赖虚拟桌面画布发布的 display-seq
（graphics_smoke main.c 的 "no compositor display sequence observed"
观察），fusion 下该发布源不存在 → 探针自报 FAIL（当轮 guest-frames
`bindings=1`、探针 624 帧走 fusion 通路后被 detach —— 渲染正常，判据
缺源）。设备持久化的 desktopMode（UI 选择持久化，tablet 缺省 virtual）
会被未声明 desktopMode 的套件静默继承：用户会话选过「多窗口模式」后，
未钉档的 core 即在该形态下假 FAIL。处置（已收口 #144）：全仓 23 个套件
全部显式声明 desktopMode —— core / displayroute-dxvk 与其余 19 个审计后
统一钉 `"desktopMode": "virtual"`（历史基线均为虚拟桌面单画布形态，
探针级 display-seq 依赖仅 winehua_graphics_smoke；core/displayroute-dxvk
另带 _desktopMode_note），fusion 形态由 displayroute-dxvk-fusion 与
fusion 两套件显式声明覆盖。类推约束：（route × backend × desktopMode）
三元组每个未钉死的维度都是一个「继承设备现场」的假 PASS/FAIL 面。

**2026-10-12 续（钉档的携带面：钉死 ≠ 生效）**：gate 重跑 2/4-4/4 全红
（opengl 双档 `no compositor display sequence observed`，fusion 无发布源
指纹）破案——钉档声明齐了，但携带面有洞：`cmd_run` 收尾必 force-stop
（finally），skip-push 轮次（gate 第 2 轮起）的主 `aa start` 就是冷启，而
desktopMode 只随 push 的 `ensure_app_running` 冷启携带，主 start 不带 ⇒
冷启读设备持久化（本机 fusion）⇒ multiwindow=true。1/4 带 push 为对照组
PASS。修复：suite_mode 解析移出 skip_push 块，钉档随**每次** aa start 携带
（主 start / ready-degraded 自愈 restamp 冷启 / retry start 三处补齐；二次
want 上 setOverrideMode 同值幂等，native multiwindow 位只认冷启——见
display_route_napi `g_started` 语义）。验收：gate 4/4 全绿（core-
r20261012-000708/000748/000837/000926），四轮冷启日志
`early-boot multiwindow=false`（修复前同场景 = true）。

**route 同构残余（未修，记档）**：displayRoute 同样只随 push 冷启携带——
skip-push 冷启落设置页持久化路线（#136 preferredRoute），x11 套件 skip-push
跑批前须确认持久化路线或走带 push 的 run。不随主 start 补的原因：显式 route
的二次 want 会走 `g_started` retrigger（script 门按最新调用方意图刷新为
false），与 displayroute 编排（script=true）的交互「幂等性无保」
（EntryAbility 注释原话），需独立取证再动。

**explorer 桌面窗处置（M4b Task-0 决策 = 接受）**：managed 模式
user32 `get_desktop_window` 自动 spawn 的 explorer /desktop 桌面窗
（class = atom #32769，`explorer/desktop.c:40`）会作为普通承载窗进入
fusion 窗口集（全屏、永远在最底层）。接受的理由：D50 验证轮实测程序窗
正确压其上、点击桌面 raise 桌面 = Windows 正确语义；过滤方案的特征值
（atom → X 侧 WM_CLASS 字符串）需实测且随 wine 版本漂移，维护成本大于
被盖住的无害窗。**M4b+ 判定器须容忍其存在**（状态序列帧里的桌面底色/
图标不算 FAIL）；它的承载窗参与 z 序仲裁但恒在底层。

**环境干扰记录**：设备上 Sandbox POC（app.hackeris.sandbox）会间歇性
抢回前台，污染 snapshot 帧采集——判定 FAIL 时先看帧内容是否 POC UI，
是则重跑，不进代码排查。

### 2.13 M4b 收口：状态面三语义端到端 PASS 与遗留边界（2026-10-11）

**验收状态**：minimize/restore、fullscreen/unfullscreen、activate 置前
三语义端到端 PASS（guest win32 确认 + 宿主 FWM 事件链均验证）；modal
owner 仅 win32 侧确认，宿主链未触发（见本节遗留观察，宿主侧验证属
M4c）——fusion_probe stateSeq 5/5（win32 侧逐步确认；T4 两轮 + T5 复核
共三轮一致）；x11-fusion 5 checks（result-json / visual-a / visual-b /
fusion-clicks / fusion-state）全 PASS（fusion-r20261011-003113）。宿主
侧事件到达以 FWM hilog 人工核验（minimize → show×2 → fullscreen →
restore 全链到达），该 capture 未归档，不进自动判定。回归门禁（T5 独立
复跑，不引用 T4 轮数字）均 = 基线：前缀组
displayroute-win32-interactive（x11 + virtual）20/21
（job-r20261011-002451，唯一缺口 = input-keyboard-x64 char-count，
XIM 已知；D35 注入间歇家族 input-mouse-x64 / input-relative-x64 本轮
未出现，无需复跑）；core 3/4（core-r20261011-002802，opengl-x86 = D49
已知）。

**关键根因（minimize 协议闭环，b569684）**：映射层（x11_toplevel）收到
request_minimize 只发 bus 事件、从不调
`wlr_xwayland_surface_set_minimized` 回写 WM_STATE → wine 侧
wm_state_serial 永不清零 → window_set_wm_state Iconic→Normal 的 Mutter
workaround 入口（`if (data->wm_state_serial) return`）让 restore 整体
成无操作——X 层无 withdraw/map、map_request 不产生、Restored 事件结构性
缺发。修复两半齐备：request_minimize 补 ICCCM 应答（set_minimized 只写
属性不解 map，可视性仍归 ArkTS 承载窗）+ map_request listener
（createdPosted 门）→ Restored（wine 重显窗的唯一宿主信号），restore 时
配对 set_minimized(false) 防 stale minimized 写出假 Iconic。附带收益：
wine 状态机解封后 fullscreen/unfullscreen 语义事件同链到达。

**判定器盲区教训**：修复前的 RED 轮里 stateSeq（win32 事实）5/5 全过
而 visual-a FAIL——win32 侧 IsIconic 已解、OHOS 承载窗却停在 minimize()
隐藏态，「状态面通了」与「画面回来了」完全脱钩。单层判定（只看 result
JSON 或只看帧）都会漏掉这类断链；多层互证（win32 事实 + 帧内容 + FWM
日志时序）是判定器的下限配置。

**Pad 限制注记（spec §5 风险 #4）**：subWindow 承载的 hide/minimize 是
近似实现——Window 无隐藏 API（hideWindow 不存在），FWM.hide() 用
minimize() 近似，非自由窗口设备上不生效也不报错，最坏 = 窗口保持显示
（`FusionWindowManager.ets` hide() 注释载明，与 maximize 同款设备行为
差异）= 继承 incumbent 已知限制、不新增债。M4d 才做 Pad 承载，本阶段
Pad 形态仍走旧路，fusion/x11 状态语义不向 Pad 下发。

**遗留观察（不阻塞，M4c 前置排查点）**：
- modal 步宿主侧 set_parent 未触发，根因已实锤（final review I1）：wine
  只在创建/管理路径写 `XSetTransientForHint`（set_style_hints，
  window.c:1162，由 create_window → set_wm_hints 驱动）；运行期 owner
  变更（SetWindowLongPtr GWLP_HWNDPARENT）不经过任何重写路径（owner 非
  style、不触发 style-change hook，也不单独触发 SWP_FRAMECHANGED），
  hint 不更新 → xwm 的 WM_TRANSIENT_FOR handler 不 fire → 宿主
  set_parent 结构性不可达（T4 真机实测一致）。probe 的 modal 步因此只
  覆盖 win32 记账，宿主链（Modal 事件 → ArkTS 模态跟随）零覆盖且套件
  无法感知回归。宿主侧验证属 M4c：走 owned 窗创建时建立关系（探测
  CreateWindowExA 带 owner 的路径——创建即写 transient → manage →
  set_parent fire），不用运行期 owner 变更驱动。
- wine SW_HIDE 幽灵窗（final review I2）：guest 对受管顶层窗 SW_HIDE
  在 X 侧只是 unmap——xwm dissociate + set_withdrawn(true)
  （xwayland/xwm.c:1613-1624），映射层无对应 listener、宿主侧无事件 →
  OHOS 承载窗保持显示最后一帧（幽灵窗），直至 re-show（map_request →
  Restored 已覆盖该半边）。任何隐藏顶层窗的 win32 应用（splash、
  launcher→game 交接、托盘应用）都会留下死窗影像。低成本处置 = 本条
  记录；withdraw/unmap listener（发 hide 族事件）留 M4c 评估。
- fullscreen 语义事件虽通，wine 对「WS_POPUP + 全屏矩形」走的几何路
  （request_configure resize）与 _NET_WM_STATE 路并存的边界未系统梳理。

### 2.14 M4c 收口：popup 链套件全 PASS 与两项现场破案（2026-10-11）

**验收状态**：x11 fusion popup 链（OR 菜单窗 → OHOS popup 子窗）套件化
完成——fusion 套件 3 用例（fusion-probe / fusion-popup / fusion-popup-oob）
判定全 PASS（fusion-r20261011-040837：既有 5 checks 不回归 + visual:
fusion-popup-menu 窗内/越界两形态菜单渲染 + fusion-popup 开关链 +
fusion-popup-selection SKIP 移交标记），复跑轮（041228）popup 两用例
再绿、probe 轮 missing-frame 为 host 抓帧时序（设备端 PASS，单点复跑
041623 PASS）。回归门禁达基线：前缀组 displayroute-win32-interactive
（x11 + virtual）20/21（job-r20261011-041723，唯一缺口 = input-keyboard
char-count，XIM 已知；D35 间歇家族本轮未现）；core 3/4
（core-r20261011-042220，opengl-x86 = D49 已知）。判定器合成自测 11/11
形态 GREEN（含真机 T3 截图真实像素形态，输出记 SDD ledger）。

**现场破案一：entry_remove memmove 错位 wl_listener —— fusion 会话收尾
必炸（已修）**。多用例 fusion 跑批首轮（r20261011-032733）在 probe → popup
切换时整会话死亡。faultlogger（cppcrash-20261011033916）：app 进程
SIGABRT，`Assertion failed: wl_list_empty(&xsurface->events.destroy.
listener_list) (xwayland/xwm.c:701)`。根因：`entry_remove` 用 memmove 压缩
entry 数组，而 entry 内嵌 wl_listener 的 link 节点地址被活跃信号链直接
引用——移除前位 entry 使后位 entry 的 link 全部指向错位地址，下一个销毁
的 surface 走不到 destroy 回调、listener 摘不掉，wlroots 断言炸掉组合器
（全部 wine 子进程陪葬）。M4a 起「先删前位 entry + 后销毁后位 entry」
即触发；单用例套件 summary 先落地把它掩盖成收尾噪音（faultlogger 在库
最早同签名记录 2026-10-10 23:24，M4b 期；032733 轮起套件多用例化才把
crash 移进跑批中段现形）。修复 = 墓碑槽 + 复用（entry 地址终身稳定），
`x11_toplevel_xs_of` 补 dead 守卫。D45 的「wl_signal_add 必须配对销毁且
整链审计」教训由此有了第一个实锤实例：配对本身没错，错在销毁后数组还
会挪动。WS_POPUP 探针窗同轮暴露第二问题：wine 把无 caption 的 POPUP 窗
判为不托管 → X 侧 override_redirect → 窗和菜单全走 OR-popup 路径且无
owner 可挂 → 什么都不渲染——探针窗样式回归 popup_overflow 同款托管样式
（本栈非客户区不绘制，视觉判定不受标题栏影响）。

**越界点选悬案（已闭合，M4d 破案——见 §2.15；以下为 M4c 阶段记录）**：
越界（整条菜单越出窗 rect 底缘）场景下点击菜单项，wine 收到按钮事件且
菜单关闭，但不产生 WM_COMMAND——探针以 command-item2 自证落空。四轮控制
变量全部排除：几何（窗界内对照同败）、owner 先导点击（两段式复刻
notepad 流同败）、SetForegroundWindow 前置（对齐 e2e_menu 同款同败）、
坐标换算（OHOS 子窗尺寸 = w×scale 精确，guest local 恒在目标项区内）。
关键证据（wine +event trace）：点击的 Enter/Motion/Press/Release 四事件
送达 explorer 桌面窗（hwnd 0x10068 / X 1000003）而非探针菜单窗；wine
菜单 grab 收到点击（菜单关闭）但命中判定按错误位置处理。当时收敛到
「Xwayland OR 菜单窗 enter 路由 / X 堆叠 hit-test」与「探针消息循环与
wine 菜单跟踪交互差异」两说——**均被 M4d 推翻**：真因在我方 multimode
注入口缺 motion 半边（按钮携带陈旧光标坐标，与 OR 路由/探针形态无关，
§2.15）。影响面（M4c 时认定，现仍有效）：窗界内菜单（notepad 形态）
不受影响。复现资产：`smoke/jobs/popup-overflow-x11.json`
（`--desktop-mode fusion --display-route x11`，uitest 按物理坐标点选）。
**门禁接线（2026-10-13，判定重连）**：该 job 从 raw/argv=[] +
judge=external（探针自证永远不落盘，归档只有 external-vehicle SKIP 占位）
改为缺省 std 协议 —— runner 下发 `--automation/--result`，探针 checks[]
随归档由 result-json 判定器裁决；几何前置断言同步重设计（删除恒假的
menu-below-window 自比，改用菜单显示后 GetMenuItemRect 实测矩形 + Δ
修正，见探针注释），`menu-item2-rect/center` 落归档供点选定位。点选仍
由人工 uitest 触发，无人点击 = FAIL（不进无人套件）。
套件判定口径（Ruling）：fusion 套件内的自动判定只覆盖已验证行为（渲染/
关闭链）—— `fusion-popup-selection` 判定器对套件内越界用例仍恒 SKIP 并
注明「门禁在专用 job」，不做会假 PASS 的点选判据。负全局坐标（菜单越出
屏幕左/上，RF#5 后半）行为未测，不进自动化套件，只有手动路径。

**OHOS 拖拽 resize / 拖动承载窗在 x11 路线不回写 wine（M4a 模型缺口，
2026-10-11 任务 #135 治本闭合）**：原文（症状 + 根因）：fusion+x11 下拖拽
子窗边框，OHOS 子窗缩放只是渲染拉伸，几何不回写 guest——
`NotifyToplevelResize` 找不到 xdg resource 直接早退（wayland_server.cpp
原 L470）；位置半边全仓无通道。修前实录 `.temp/geom/r1-*`（边缘拉伸后
win32 事实原地不动 640x480/sizes=0；同会话 `[NAPI] notifyToplevelResize`
75 条 vs `[MW] NotifyToplevelResize` 0 条）。**治本（全部落我方层，
thirdparty/wine 零改动）**：① 尺寸半边——`NotifyToplevelResize` 入口加
x11 分支（门 = `x11_toplevel_active() ∧ x11_toplevel_owns(id)`，防路线
旗标与 id 空间错配误劫 wayland）；② 位置半边——新 NAPI
`notifyToplevelMove`（ArkTS 上报点 = FWM subWindow / WWA mainWindow 的
`windowRectChange`，值 = windowRect 原点 / effectiveScale，gated
`DisplayRouteService.isX11()`）。两半同纪律：napi 线程只入队 → loop
线程 dispatch → `wlr_xwayland_surface_configure`（x11_toplevel.c
geom_writeback）；门禁 = 与 xs 现值全等跳过（防回声第一道门，实测 860
条 NAPI 上报中 432 条被无变化跳过）+ minimized 跳过 + echo 抑制
（handle_request_configure 对 ε=2px 内回声只发协议应答、不上送 resize
事件）。**实测（真机 192.168.1.5，2026-10-11 18:01-18:24 冷启同代际；
证据 `.temp/geom2/` + 归档 `build/automation-logs/job-r20261011-180034`
与 `job-r20261011-181026`）**：边缘拉伸 +151 逻辑 → win32 事实 640→790、
WM_SIZE 计数 56，帧为新尺寸重排（白十字臂宽 22px 物理 ≈ 新帧 24px；
拉伸指纹应 28-30px，对照修复前 r1-01=28px）；顶条拖动 +100,+100 →
win32 原点 100→200 精确（WM_MOVE 计数 51）；4 轮往返收敛精确、空闲
25s 计数冻结、echo 抑制 0；wine 侧 `+x11drv` 轮 108 条 config 采纳行走
(104,130)-(736,576)→(204,230)-(986,676) 与回写逐拍对应、drag 期间
`requesting config` 0 条（无回写风暴）。回归 fusion 3/3、core 3/4
（opengl-x86=D49 已知）；**wayland 对照轮**（同探针同拖拽编排，route=
wayland）回写标记 0 行 + geomA 全零（隔离红线实测；wayland 虚拟桌面态
无承载窗 resize 手势、surface 事件链 0 条——该拓扑下回写通道无触发面）。
**与 GAP2（§2.15 多 app 重叠输入错投）解耦维持**：几何写回不修重叠
路由（命中仍归 X 堆叠，GAP2 侧 restack 仍必要）；写回改善的是非重叠区
的 hover/命中保真与拖动后视觉一致。

**三个执行时核实点结论（plan Self-Review #2 的实测沉淀）**：
- OR 窗 associate 行为：wlroots 对 OR 窗与受管窗同样发 associate/
  dissociate（xwm.c:1306 无 OR 门禁）；OR 窗无 map_request/
  request_configure（X 对 OR 直通 map/configure），生命周期信号收敛为
  show = associate+buffer、hide = dissociate（X unmap 与 destroy 都经
  xwayland_surface_dissociate，xwm.c:668，单点覆盖双终态）、几何 =
  set_geometry（ConfigureNotify，xwm.c:1512）。
- createRenderer popup 查表：零改动——popupId 存进 entry.toplevelId 后
  attach/resize/detach/输入 dispatch 全走 entry_of_id 既有路径。
- offX/offY 语义：PopupWindowManager.positionEntry 定位公式 = 「父窗
  windowRect 全局原点 + off×scale」，映射层把 X 全局坐标换算成父相对
  偏移直传（off = popup.x − owner.x，单点在
  try_post_popup_show/handle_popup_geometry）；负偏移路径未测（菜单都在
  父窗右/下方），与 RF#5 后半同属待测。

**遗留观察（不阻塞）**：① popup 用例的 byTitle owner 点击
（ownerClicked）命中率不稳定——**记录更正（2026-10-11 复核）**：原「035938/
040837 两轮均 false」与归档不符（040837 实为 true/true；全部 False 轮 =
035938 + 102439/pad-regress/pad-regress3，后三家属设施档位假阳性，见下方
102439 更正）；真瞬态仅 035938 一例（该轮 fusion-probe a=1 实证确在
fusion 模式）。**M4d review gate 处置（2026-10-11）：已随 8a1e975 motion
修复闭合**。其不稳定与悬案同根因族（焦点不变时按钮携带陈旧光标坐标，
命中判定错位）；有效复证锚点 = fusion-r20261011-pad-regress4
popupSeq.ownerClicked=true（oob-result.json 引用见下方 §2.14 更正）；
② fusion 套件跑批中段 crash 修复后，host 抓帧时序（missing-frame）成为
新的间歇项（D35 家族新形态），单点复跑即可判绿。

### 2.15 M4d 收口：x11 fusion Pad 承载贯通 + 越界点选悬案闭合（2026-10-11）

**退役条件判定（2026-10-11，证据分级；判定权在用户，本表只登记证据面与
未取证门）**：用户定义 = x11 虚拟桌面 / PC 多窗口 / Pad 多窗口三形态都有替代
（自研窗口栈退役前提，见 memory x11-retirement-three-mode-parity）。现状：

- **虚拟桌面**：替代成立——displayroute 前缀组 21/21（job-r20261011-101858，
  超历史基线 20/21）+ 交互手测链；今日闭环 #137 双击治本（fd98603）、
  #136 路线切换 UI（0a66f7d）。（2026-10-11 复核更正：原引「§2.1」是悬案
  记录，不作正向锚点。）
- **PC 多窗口**：替代成立（M4a/M4b：#119/#120-125，x11-fusion 套件双窗/状态
  序列常态绿）。**未取证门 = #133**：真 PC/2in1 硬件上的系统装饰显形（代码
  已按 isX11 分叉落地 + 能力缺失 graceful 降级，Pad 观察不作跨设备结论 =
  用户裁决；锁 arm64 笔记本）。
- **Pad 多窗口**：替代成立（#128 四项目测 + #129 顶条 13af1ee + #131 GAP2
  565c707 + #135 GEOM 394e321；近似限制清单见下）。**补注（2026-10-11 夜）**：
  上述验收均经 smoke 显式参数路径；**产品路径（UI 持久化 fusion + 裸冷启）
  当时未经取证且实际带病**——multiwindow 位时序错致窗口不出（用户实测），
  当日修复 4b83f93 并补测产品路径日常主链（冷启→文件管理启动 explorer/
  notepad 出窗→两窗并存→窗内交互→关窗，全部实拍取证）。
- **日常使用**：三形态即用户当前日常操作面（引擎/虚拟桌面/多窗/交互/键盘/
  中文链），本会话全程真机手测驱动、发现的问题当日修复闭环（#137/b405607）。
  **非阻断项**：#116 首启偶发失败（止血 90c26c7，待部署验证；影响面 = 全新
  安装首启一次重试，已装用户不受影响）、#117/#118 套件间歇（低优记档、复跑
  即愈）、#134 测试资产清理（待用户叫停）。
- **复核补注（2026-10-11，R1-R5 怀疑式复核；findings 全文 `.temp/review/`）**：
  分级不变，但补：① B1/B2 两处**未登记缺口**——wine 程序驱动 maximize
  （`_NET_WM_STATE_MAXIMIZED` 无宿主侧事件路径）与程序化 move
  （`_NET_WM_MOVERESIZE` 无监听）在 x11 多窗形态未落地；退役评估前修或
  显式记档为限制；② 下表 maximize/双击标题栏行按复核修正；③ 记录更正见
  本节各条（102439 假阳性、ownerClicked 归档、oob 引用）。
- **复核批次 1 执行（2026-10-11 夜，9d4dd68/1d83b64/1843af8 已推，构建
  hap RC=0）**：#136 回切（早退/镜像判据改 preferredRoute）、D47 F1/F2、
  R2-02 Cancel 不点击已修；**R2-01「双击目标同一性」延期**——作用面仅虚拟
  桌面（fusion 每窗一页天然同目标）且该模式 ArkTS 无按窗数据，方向 =
  host 侧 X 命中查询（xwm xcb），已立项；F3B（epoch）随批。
- **R2-01 设计冻结（2026-10-12 固化；源 = .temp/review 复核全文 + 执行会话
  分析，.temp 清理后以本条为准）**：
  - 缺陷（CONFIRMED，r2-code R2-01）：x11 双击判据缺目标同一性（wayland
    有 `pendingTl === lastTapTl`）——A/B 两窗击点相距 <30px 且间隔 ≤400ms
    → 误判双击：B 的点击被吞、按 A 首击坐标重放；后果 = B 零点击 / A 被误开。
  - 数据源勘误（已核实）：FWM 矩形表不可用（entries 仅 fusion 填充；虚拟
    桌面为 root 单例）。数据源 = host 侧 X 命中查询。
  - 冻结方案：display_route napi 增同步查询 `queryXWindowAt(fx,fy)`——
    xcb 连接取 xwm（`wlr_xwayland_get_xwm_connection`，GAP2
    SyncXInputFocus 先例）或专用连接（互斥串行化）；一次
    `XTranslateCoordinates(root → 桌面承载窗, fx, fy)` 拿 app 窗级命中
    （返回桌面窗的直系子窗 id；虚拟桌面下桌面窗满屏于 root 下）。
    fx/fy 与注入路径同空间，换算与 `inject_motion` 同源（防经典坐标错位）。
  - ArkTS 门（DesktopWindow.ets x11 分支）：isDouble 追加
    `target === lastTapTarget`——两击同目标才重放；不同 → 各按本方坐标
    单击（对齐 wayland 语义）。「均为桌面窗」（图标双击）视为同目标 ✓。
    查询失败（-1）回退现行 time+slop（零回归姿态）；wayland 分支不动。
  - 时序基石：Down 必发 motion（DesktopWindow.ets:423）；本地 X socket
    往返 <1ms ≪ Down-Up 间隔。
  - 验收判据（先于实现确定）：① 跨窗 <30px 先后点（≤400ms）= 各收单击、
    无重放（wayland 对照）② 同窗/图标双击照常（#137 回归面，uitest 复刻）
    ③ 查询失败路径 = 现行行为（xcb 不可用对照）④ core 不回归。
  **部署验证（2026-10-11 夜，自动化，设备 .5）**：全新安装冷启 ×2 均干净
  （starting:wineboot→ready ≈55s，零 [AUTO-RETRY] —— D47 彩票未中，[AUTO-
  RETRY] 链验证待故障注入）；core ×2 = 3/4（唯一 FAIL = D49 已知签名 #118，当晚判定修正后重判 4/4，见下）；
  **#136 已过自动判定（含严格版）**：uitest 驱动设置页（dumpLayout 定位），
  选 Wayland 后引擎整重启（41 条 native 消息 = 41 次 mirror 重跑）高亮零回
  翻，hilog 双行 `preferred set: wayland→x11`，截图 `.temp/batch1-verify/`
  （r0-r3/s0-s6）。F2 复测口径修正：前台 busy 被模态 overlay 结构挡住，真实
  可复现面 = bg 自动重试期间（骑彩票）；Cancel 维持手测。auto-retry 判定件
  已入库（0754ea3）并首跑 SKIP（no-evidence，未触发=不下结论）。
- **冷启 multiwindow 位时序（2026-10-11 夜，4b83f93 已修）**：用户实测
  「Pad x11 多窗口 + 文件管理启动 explorer 窗口不出」。根因 = x11 链早启
  （EntryAbility.publishLaunchRequest）调 shouldUseMultiwindow() 决定
  chain_start multiwindow 位，而 DesktopModeService.init（读持久化
  'fusion'）在 WineWindowManager.init 里晚 26ms 才跑（实测 displayRoute
  applied 40.756 早于 mode policy 40.782）→ 读默认 'virtual' → native 链
  落虚拟桌面（x11_toplevel_active=false，窗走 g_clients 画布路径），而
  ArkTS 实时查询判 fusion 不开画布 → 窗无宿主不可见。**两路径差异判据**：
  smoke 冷启带显式 `--ps winehua.desktopMode`（setOverrideMode 在查询前）
  故正常——「同一操作 smoke 出窗、产品路径不出」即此时序指纹。修复 =
  publishLaunchRequest 里提前 DesktopModeService.init + chain_start 日志
  带 multiwindow 值。真机验证：全新安装 + UI 持久化 fusion + 冷启无参数，
  early-boot multiwindow false→true；notepad/explorer 直启均出窗；core 4/4。
  教训 (原则 7/10)：**启动位接线点的依赖必须在读点前备齐**，且接线点日志
  不可省——本值错判时全链无一条线索，只能从「窗走哪条路径」间接反推。
  **同类面审计（当晚随修）**：展示路线（DisplayRouteService.init 先于读）、
  d3d/dxvk preference（PersistentStorage.persistProp 语义为同步恢复，SDK
  common_ts_ets_api.d.ts 明言「通常先于 AppStorage 访问调用」，且 onCreate
  注册先于读取）、fusionHost/testDecor（走 want 显式参数无持久化）均无此
  类时序依赖——desktopMode 是唯一一例。
- **悬而未决**：#133（硬件）；用户对最新构建（双击/滚动/路线 UI，含复核
  批次 1 修复项）的手测判词；B1/B2 的处置口径（修 or 记档）。

**D49 破案收口（2026-10-11 夜，automation 判定修正）**：core×x11 的
opengl-x86「pid 归属」FAIL **根因推翻旧诊断**（原记「displayed 粘性值被
桌面帧抢位」不成立）：x86 (i386) 程序经 wine loader **二次 NCP spawn** 出
实际承载进程，runner 记录的 spawn pid 只是它的启动器 wine_child（dlopen
box64 → box64_hmos_main 跑 wine loader）。硬证据（r20261011-204807 全量
采集离线过滤）：`WL_Broker StartNativeChildProcess childPid=34474` →
`WineChild[34474] calling box64_hmos_main`；0.7s 后 `WL_Broker` 再
`childPid=34486` + `WL_NAPI [ProcReg] add name=winehua_graphics_smoke.exe`；
窗口（gl 960x540 = 用例渲染尺寸）归属 34486。**判定修正**：归属断言从
「key pid == spawn pid 记账相等」改为**帧 key 同一性**——归档
[GUEST-FRAMES] attach/detach 行的 key（=(unix pid<<32)|xwin，异进程窗口
不可能同 key）与本轮结果自报 presentedKey 全等 ⇒ 上屏帧就是 ledger 登记的
本次窗口面；attach 行几何 src（WxH）与本轮渲染尺寸作佐证，不等按 FAIL 交
人工。x64 未再 spawn 的恒等路径保留；「本 pid attach」回落保留（16 归档
实测零触发）；残余盲区（本窗口从未上屏但上一窗粘性值恰在日志窗口内的假过
形态）已写进判定器注释，16 轮归档实测均未出现。**合成自测** 15 形态 GREEN
（旧判定器下恰 3 个新形态 MISMATCH = RED 证据；`selftest/presented_route/`，
README 已挂）。**全量重判**：按旧消息指纹 grep 定位全部 16 个走到该分支的
归档（10-10/10-11 两日 core），key 同一性证据（attach 或 detach）16/16
齐备、**全部翻绿 4/4**——即两日 16 轮「3/4（opengl-x86 = D49 已知）」记录
全部重判 4/4，无一是真实呈现失败；本节及更早各处「opengl-x86 = D49 已知
签名」之述随之作废。任务 #118 关闭。

**验收状态**：M4 多窗分期收官——x11 fusion 的 Pad 承载（subWindow 路线）
四项手测全通过：承载/双窗（fusion 套件复跑全绿，见下）、拖动/手势、
6 窗并发压测；§2.14 越界点选悬案**破案闭合**（见下）。回归门禁：前缀组
displayroute-win32-interactive（x11 + virtual）**21/21**（job-r20261011-
101858，超历史基线 20/21——input-keyboard-x64 char-count 本轮亦绿）；
core **3/4**（core-r20261011-102209，opengl-x86 = D49 已知签名）；
fusion 套件 3/3（job-r20261011-103109，multimode 注入改动后的回归轮）。
**记录更正（2026-10-11 复核）**：r20261011-102439（及同日 pad-regress/
pad-regress3）1/3 曾记「D35 家族间歇、机理未查」——实为**设施档位假阳性**：
只带 --job 跑时 suite 名空 → 冷启缺 winehua.desktopMode → 落 virtual 桌面，
fusion 套件 byTitle/按窗点击全败（指纹与真瞬态不同：probe a=0 vs 035938
的 a=1）；commit 4af1dcb 当日修复并自证（「三轮假阳性实测 explicit=none
铁证」）。「同构建复跑全绿」实为「同构建+钉档位」；102439 第三用例
popup-oob 实为 PASS（非「三用例全败」）。截图/日志归档 `.temp/m4d-*`。

**GAP2 追加收口（任务 #131, 2026-10-11 晚）**：M4d 记录的多 app X rect
重叠输入错投**治本闭合**（根因/修复/证据详见本节末「多 app 重叠输入错
投 —— 破案闭合」）。回归门禁（含修复构建）：fusion **3/3**
（fusion-r20261011-154809，popup 两用例含菜单点击链在案）、core **3/4**
（core-r20261011-155005，opengl-x86 = D49 已知签名，无新增失败）；
wayland 对照轮（共享文件 display_input.c 改动）：点击+打字正常，
GAP2 标记日志 0 行（改动 gated 于 x11 multimode，wayland 侧逐项零变化）。
证据 `.temp/gap2/`；SDD ledger `.superpowers/sdd/2026-10-11-x11-gap2-overlap-input/`；
mini-plan `docs/superpowers/plans/2026-10-11-x11-gap2-overlap-input.md`。

**§2.14 越界点选悬案闭合（根因 + 修复 8a1e975）**：multimode 注入口
（`wl_ohos_input_multimode_pointer`）的 wire 序列缺 motion 半边——
完整序列为 enter→motion→press→release（D15），原实现只在焦点 surface
切换时发 enter（携带坐标），焦点不变时直发 button。Xwayland 的光标
位置由 motion 驱动；wine 菜单跟踪持 X grab 期间，enter 被 X 侧抑制
（trace 实证：无 EnterNotify、光标不更新）→ 按钮携带**上一条 motion
的陈旧坐标**经 grab 重定向投给菜单窗 → 命中判定全错。两个数据点同根因：
① 越界菜单项点击：坐标错位 → 菜单关闭但无 WM_COMMAND（§2.14 主证）；
② 菜单外点击：陈旧坐标恰为菜单锚点（在菜单 rect 内）→ win32 语义的
dismiss 不发生。修复 = 按钮前无条件补 motion+frame（grab 重定向下坐标
同样为真）。修复后复测：popup_overflow 探针（短窗 + 整条菜单越出窗底）
uitest 点第 2 项 → **command-received True（WM_COMMAND id=202）+
command-item2 True**（runId manual / build/m4d-oob-inline.json 编排）；
notepad 上下文菜单外点 → popup-hide 正常 dismiss。`fusion-popup-selection`
的恒 SKIP 标记保留（越界用例的点选自动化**另立判定器已立**：专用 job
`smoke/jobs/popup-overflow-x11.json` 2026-10-13 起走 std 协议随归档判定，
人工 uitest 点选 + result-json 裁决；套件内保持恒 SKIP —— 门禁在 job）。
**记录更正（2026-10-11 复核）**：原引 `.temp/m4d/oob-result.json` 作探针
自证不成立——该文件整体 status=FAIL（1/6：menu-below-window 159<162 三像素
off-by，未披露），且写入时刻早于 8a1e975 修复 commit 11 分钟（其「修复后」
构建归属不可证）；修复后有效复证锚点 = fusion-r20261011-pad-regress4
（ownerClicked=true、菜单链在案）。

**执行时核实点（plan Self-Review 实测沉淀）**：
- **RF#3 caption 结构性缺失**：x11 下 wine 的 X 窗 = win32 rect 减
  WM 装饰区——D52 created 坐标 = win32 原点 + caption+border 偏移
  （notepad (4,30)、fusion_probe 640x480 → 632x446@(104,130)）。fusion
  栈无 WM 装饰提供方 → caption 无人绘制、不进任何 X 窗 → HTCAPTION
  触摸路径不存在，wine `_NET_WM_MOVERESIZE`（winex11 mouse.c:1468）
  无触发面；菜单栏是 X 窗顶部的真实命中区（popup off y=19 = 菜单栏
  底）。wayland Pad 同构（xdg 亦无 caption），非 x11 独有。
  **#129 处置（2026-10-11）**：caption 缺口由 x11 fusion 子窗自绘顶条补位
  （承载层 ArkTS chrome，不进 guest 像素/X 窗）；wayland Pad 的 caption 由
  winewayland 自绘 CSD 提供（本非缺口，实测截图 t3-wl-control）。顶条高度
  40px 物理即原菜单栏拖动区（f43c764）上方的独立节点，短按/拖动语义分离。
- **拖动定案落地（f43c764）**：WineWindow 触摸状态机新增顶条分支——
  拖动起点 ≤40 物理像素且 fusionUsesSubWindow → `startMoving()` 系统拖拽
  （wayland Pad incumbent 同通道），整手势零注入 wine；短按不受影响
  （顶条点击照开菜单）。真机三次拖动全部跟手（位移实测吻合）。
- **uitest 注入口径（Pad 192.168.1.5）**：左缘 x≲64 且屏幕上段的
  点击 press 被系统手势消费（app 只收 Up，CLICK-PIPE 零日志）——点
  wine 菜单栏左端（文件(F)）用 x≥70；uitest 日志的
  `IMultimodalInputConnect 29201` 为伴随噪音，注入本身可用。
  **键盘（#129 追记）**：单键可达 wine（修复后实测 `keyEvent 2017` →
  notepad 编辑区出现 'a'，状态栏 1行2列）；**组合键（Alt+F4）不可注入**
  ——uitest 模拟修饰键不记录态（KeyEventNormalize "simulated modifier
  key status record switch is not enabled"），裸 F4 被 SceneBoard
  KeyEventInputSubscribeManager 截走（app 收不到 F4 down）。组合键排障
  须物理键盘。

**多 app 重叠输入错投 —— 破案闭合（GAP2 #131, 2026-10-11）**：
修复前实录（notepad×2 完全重叠, 注入目标 B 落 A）与根因、修复、实测见下。

- **根因（两半, 机制级）**：
  - 指针：按窗注入只决定局部坐标 + 目标 surface；X 投递由**服务端
    hit-test（X 窗树/堆叠）** 裁决（xwayland-input.c `dispatch_absolute_motion`:
    root 点 = focus_window 的 X 窗位置 + local）。x11 fusion 下各 app 的
    X 窗都钉在创建位（当轮 host→wine 几何回写缺失, §2.14——该缺口已由
    #135 治本；本节结论不依赖它：两窗几何本就可能重叠，命中仍归堆叠）→
    rect 恒重叠 →
    堆叠决定命中；而 **X 堆叠无人同步**：wine 跨进程不 restack（X z 序
    归 WM 管）、xwm 把堆叠留给合成器（map_notify 对新窗还 restack(BELOW),
    xwm.c:1609）——即**宿主 WM 职责缺口**（M4 多窗实现了呈现半边 + 注入
    瞄准，路由状态同步半边未做）。实测：点 B 承载窗（tl=17）的
    ButtonPress 全落 A 的 X 窗（0x10060/e00003），B 自激活（wine
    foreground）后 X 顶仍是 A。
  - 键盘（独立第二因）：KeyPress 走 DIX 输入焦点；xwm activate 对 wine
    （ICCCM globally-active, WM_TAKE_FOCUS 模型）发的消息恒带
    `data32[1] = CurrentTime = 0`，wine 的接受门（winex11 event.c:738 +
    window.c:2146, 时间戳须新于前台窗 WM_STATE 事件时间）对 time=0
    恒拒绝（wine trace WARN 原文在案）→ **合成器驱动焦点切换对 wine
    结构性不可达**；wine 自接受路径（set_focus）在有 EWMH 时同样只做
    XSetInputFocus（不动 win32 前台）。
- **修复（全在我方侧代码, thirdparty 零改动）**：
  - F1 指针/轴：注入前 X 堆叠回写（`x11_toplevel.c mm_restack_target` →
    `wlr_xwayland_surface_restack(ABOVE)`, click-to-raise; 不变式 = root
    点按构造 ∈ 目标 X rect ∧ 目标在顶 ⇒ hit-test 必中目标）。
  - F2 键盘：`display_input.c SyncXInputFocus` —— FocusClient 每次调用
    （短路口前, wine 自置前台会漂移 X 焦点故须重断言）经 xwm 连接
    `xcb_set_input_focus` 对齐目标（= wine 自接受路径同原语, 绕过
    time=0 门）；门 = x11_toplevel_active()（虚拟桌面/wayland 零行为
    变化）+ 非 OR + surface 在册。
- **实测（真机, 修复前→后同场景对照）**：点击落点 错窗
  `0xe00003` → 目标 `0xc00003`/`0x800003`（双向可切）；键盘同编排
  KeyPress 错窗 → 目标窗（wine WARN 同签名但已被绕过）；截图字符各归
  各窗；拖动后点击 / 纯键盘切换（顶条 tap→打字）/ 双指滚动全 PASS。
  证据 `.temp/gap2/`（wine_trace_*）、SDD ledger
  `.superpowers/sdd/2026-10-11-x11-gap2-overlap-input/`。
- **与 §2.14 几何回写的关系（解耦结论）**：host→wine 几何回写缺失是
  独立缺口——**几何写回单独不能修重叠路由**（重叠点仍由 X 堆叠裁决，
  两 rect 都含该点；写回改善的是非重叠区 hover/命中保真）。两者各自
  成立, 互不替代。
- **Deferred**：raise 侧堆叠同步（ArkTS raiseWindowGroup → X restack）
  不加——dispatch 侧"使用时不变式"已覆盖全部输入路由消费面, 且对 wine
  自 restack/时序竞态免疫（判据只留一处实现）。**触发条件**：未来出现
  X 堆叠的**非输入**消费方（如按 X 序渲染/弹窗定位）再评估补齐。
- 影响面：多 wine 应用 X rect 重叠时的按窗输入路由；单应用与不重叠
  布局不受影响。wordpad 类的真实应用多窗轮次不再受阻（M4d 改道的
  原因已消除）。

**Pad 承载近似限制清单（逐项对照 FusionWindowManager incumbent）**：
| 项 | incumbent（wayland Pad） | x11 Pad（M4d 现状） |
|---|---|---|
| hide/minimize | `minimize()` 近似，非自由窗口不生效不报错 | 同 incumbent（FWM.hide 不变） |
| maximize | 全屏化 = resize 到显示尺寸（FWM.fullscreen） | **顶条 max 键路径同 incumbent；程序驱动路径不成立**（2026-10-11 复核）：x11 无 maximized/unmaximized 事件（B1），wine SW_MAXIMIZE 至多以 resize 落地、isFullscreen 不置位 |
| 标题栏装饰 | 无系统装饰，wine 像素自带（无 caption; wayland 有 winewayland 自绘 CSD） | **自绘顶条落地（#129, 2026-10-11）**：几何补偿式（承载窗高 +40px 物理 = 20 逻辑px×scale，XComponent 下移同量、surface 保持 wine 尺寸 → 输入零换算），标题 + min/max/close 三键 + 拖动（startMoving, f43c764 同通道）。系统装饰在本形态结构性不可得：官方文档（非自由窗口态子窗恒沉浸式，decorEnabled 仅自由窗口态生效；@ohos.window.d.ts L1847/L8469）+ decorEnabled 三水实验（.temp/decor/）。三键走 M4b 动作链（min=FWM.hide / max↔restore / close=WM_DELETE_WINDOW）；菜单 popup 定位 +strip 补偿（PopupWM.positionEntry，实测 move ok (8,138)） |
| 双击标题栏最大化 | **有**（winewayland 自绘 CSD caption，t3-wl-control 实证——2026-10-11 复核修正） | x11 无 caption；最大化可达入口 = 顶条 max 键 + 程序驱动 `_NET_WM_STATE_MAXIMIZED`（后者宿主侧无事件路径 = B1，未落地） |
| 物理滚轮 | **同断（#132 实测）**：本 build 轴事件到不了任何 ArkUI 节点——物理滚轮非 wayland 可用项，两路线同灭（平台级，见下） | **双指滚动已通**（M4d review gate：sendScrollEvent 补 x11 multimode 轴分支，motion 先行；真机 7 步注入 = 7 次 ButtonPress 一一对应）。物理滚轮 = 平台断点（#132 定界）：轴事件送达承载窗（`HandleInputEvent wid:… ac:5/6/7`）但 ArkUI 命中链在任意坐标找不到节点（探针零回调 + TTHRTI 空 + 系统 app 同灭），与注入口无关，见下「物理滚轮平台断点」 |
| 边缘拖拽 resize | 系统拉伸 + xdg 回写 wine 几何 | **已治本（#135, 2026-10-11）**：x11 分支 `wlr_xwayland_surface_configure` 回写，win32 事实跟随（640→790 拉伸、原点 ±0 拖动）+ 重排帧实证；拖动承载窗位置半边（新 NAPI notifyToplevelMove）同轮补齐；wayland 对照零变化（详见 §2.14） |
| 点击置顶 | raiseToAppTop + raiseWindowGroup | 同 incumbent；X 侧堆叠同步已补齐（GAP2 #131：注入前 restack，见上「破案闭合」） |
| subWindow 上限 | 表驱动无显式上限 | 6 窗并发实测无上限表现（创建/渲染/置顶/交互全正常）；wayland incumbent 无可比数据 |

**窗口装饰落地与关闭出路（任务 #129, 2026-10-11）**：

- **Pad x11 fusion = 自绘顶条**（判据单点 `FusionWindowManager.stripPxOf`：
  x11∧fusion 子窗非 0，wayland 恒 0；`fusionUsesSubWindow` 不可用——wayland
  Pad 也是子窗）。承载窗 h+strip、XComponent 下移、全部 resize/restore/
  popup 定位补偿同点。三键实测：min→`FWM.hide`；max↔restore（`isFullscreen`
  态判定，全屏 2800x1840 截图）；close→`sendToplevelClose` 补 x11 分支 =
  `x11_toplevel_close`（队列化 → `wlr_xwayland_surface_close` =
  WM_DELETE_WINDOW，wine 收 WM_CLOSE 走 win32 关窗语义）。顶条区点击不落
  guest（独立节点零注入实测）；拖动 = `startMoving` 跟手实测；菜单 popup
  定位 +strip 补偿实测（`[POPUP] move ok → (8,138)`）。弹窗对话框（页面设置
  等 264x94）同为 fusion 子窗、同带顶条（实测 #7/#8）。
- **关闭出路盘点（Pad）**：顶条 × ✓（WM_DELETE 全链 33ms 闭环）；wine 应用
  自退 ✓（对话框 ESC/外点关闭 → destroyed → 承载销毁完整链，实测算例 #7/#8；
  fusion-probe 自退 = 套件常态）；Alt+F4 = 通道半程（Alt 达 wine；F4 被
  uitest 注入限制截走，物理键盘可验，见上 uitest 条目）；Pad 任务卡片 = 无
  （subWindow 非应用级窗口，结构如此；min 后无系统恢复入口 = incumbent
  「minimize 近似」语义的既有面，非本任务新增）。
- **键盘链修复（#129 副产物）**：build() 加根包裹层后页面根节点成为焦点
  落点，`onKeyEvent` 只挂 Stack 时键盘全丢（WinFocusMove → Column/onFocus
  定证）。修复 = 键逻辑抽 `handleWineKey`，Stack 与 strip 分支根 Column
  双挂点（stopPropagation 防双发）。**凡改渲染树根，必须重验键盘链**。
- **PC/2in1 形态（T2, WWA 系统装饰）**：装饰按 `isX11()` 分叉（x11: decor
  visible + 标题按钮 + titleMove；wayland 逐字不变），分调用 step 日志
  （装饰 API 能力面按调用不同，合并 try 会吞错误面）。**真机验证
  pending-hardware**——Pad 上（非自由窗口态、承载窗全屏）观察不作跨设备
  结论（用户裁决）；代码在能力缺失时为 graceful 降级（每步独立 catch）。

**M4d review gate 收尾（2026-10-11，final-review I1/I2/Minor 落地）**：

- **I1 滚轮接线（修复 + 真机实证）**：`SendScrollEvent` napi 补 x11 multimode
  分支（pointer/key 同款分流），新增 `wl_ohos_input_multimode_axis` /
  `x11_toplevel_input_axis(_dispatch)`——axis 不携带位置，投递前按 8a1e975
  同类纪律先 enter/motion+frame 定位到滚动点；discrete 走 v120 格语义
  （`display_input_inject_axis`），步进归一化 ±1 格（DesktopWindow.ets
  同款）。真机 A/B：修复前双指滚动后 wine +event trace 全程零
  Button4/5（439k 行）；修复后 7 步注入 = 7 次 ButtonPress 一一对应、
  每步 MotionNotify 先行，落 notepad 的 X 窗。证据
  `.temp/m4drev/wine_stderr_{prefx,postfx}.log`。
- **物理滚轮平台断点（#132 定界收窄, 2026-10-11；原「新记档待查」闭环）**：
  轴事件送达承载窗（wm 客户端 `HandleInputEvent wid:… ac:5/6/7` 全链在案）后，
  ArkUI 命中链在**任意坐标都找不到任何节点**——判定为**平台侧（ace 适配/
  命中段）坐标缺陷**，非我方配置/挂载/焦点问题。证据链（全量落盘
  `.temp/gap3/`，本轮三路日志 gap3-r1/r2/r3-full.log）：
  - **incumbent 对照（wayland 实测同断）**：winewayland + fusion 冷启（uinput
    轴通道，与物理鼠标 m4drev 实测签名一致），轴 `ac:5/6/7` 送达 + 容器
    `Consumed` 在案 + **TTHRTI 空 + onAxisEvent 探针零行 + 无 `[ArkTS] SCROLL`/
    `[PIPE] scroll`**（三路日志全量零匹配）⇒ 断点与 x11 无关，两路线同灭。
    原档 WWA 形态「有 Consumed 仍无回调」同机制：Consumed 只记容器处理完成，
    不表示节点收到（wayland 轮同现 Consumed + 零回调）。
  - **全 app 面（系统 app 同灭）**：Settings（同窗轴 `axisId:10000` 后 TTHRTI
    空；1 秒后手指触摸同窗 TTHNI={page,NavRouter,List} + PanRecognizer 命中）、
    浏览器（点击命中 Flex/Row 识别器在案，轴事件 TTHRTI 空、长页不滚）、
    启动器（轴送达，界面无反应）。
  - **探针重建（x11 fusion，两处坐标）**：XComponent onAxisEvent 加无条件入口
    日志 + 根 Stack 第二挂点 → 轴事件零行（回调从未触发，非轴值=0 静默路径）；
    窗口局部 (492,600) 与 (12,50)（相距约 480px）均落空 ⇒ 任何 ≤~25× 的等比
    缩放/小平移都会让至少一处留在窗内触发回调 ⇒ 排除近恒等类坐标变换，退化
    为更大幅度或异空间坐标；精确值点在闭源 ace 内，app 侧不可观测。同窗
    同会话 onMouse 正常且带动 GAP2 标记链 ⇒ 鼠标链健康、断点在轴命中。
  - **已排除**：轴→触摸兼容转换（FeatureManager 配置失败 + isAppDevelopedForPC=1，
    实测轴时刻无触摸事件）；z 序/重叠/焦点态（系统全屏窗同灭）；日志丢行
    （两测量分钟设备级丢行 0/4 行，同时刻其它 app 行在流）。
  - **app 侧替代通道全排**：OH_Input 轴监视（INPUT_MONITORING 系统权限）、
    输入拦截器（INTERCEPT_INPUT_EVENT 系统权限）、窗口鼠标过滤器
    （`WindowSessionImpl::FilterPointerEvent` 显式排除轴 action——读码件归档
    `.temp/gap3/sdk-src/wm_filter.cpp`；设备实测同窗 5 次回调全为 move/DOWN/UP、
    轴 0 次，含轴事件送达同窗后 1 秒的对照）⇒ **无可达替代轴通道，无我方侧
    修复面**。
  - 影响面：本 build（`param get const.ohos.fullname` = OpenHarmony-7.0.0.105
    实测）物理滚轮对**全 app** 不生效（两路线、subWindow/WWA 形态）；触摸路线
    不受影响（双指滚动 M4d I1 已通）。治本在平台侧（ArkUI 轴派发/命中段）；
    平台修复前我方无可行替代（通道面已全排）。
- **I2 PC 形态复跑（证据刷新）**：调试键 `winehua.fusionHost` 原实现是死键
  ——`AppStorage.set` 对从未 setOrCreate 的键是静默 no-op，跨设备强制从未
  生效（本次 Pad 实测：带参冷启 FWM 仍建 subWindow）；已改 setOrCreate。
  修复后带 `--extra-ps "winehua.fusionHost ability"` 跑 fusion 套件
  （fusion-r20261011-pc-ability，MANAGED MODE explicit=fusion）：
  result-json / fusion-clicks（a=1 b=0）/ fusion-state / popup 两用例全
  PASS；唯一 FAIL = visual:fusion-window-a，因 ability 承载下窗 B 以全屏
  自由窗叠在窗 A 上（Pad 模拟 PC 形态的布局差异，真 PC/2in1 自由窗级联
  不重叠；点击/状态不受 z 序影响）。三形态对账 PC 行证据据此刷新。
- **Minor**：DRAG_STRIP_PX 硬编码 40（scale-2 特例）改为 20×getEffectiveScale
  现算（review Minor#3）；其余 Minor（#4/#5/#6/#7）记 ledger deferred 不修。
- **自动化设施**：`smoke.py run` 新增 `--extra-ps`（附加冷启调试键）；
  修复只带 `--job` 跑时套件档位不钉死的问题（suite 名取 CLI > job 声明，
  否则全新安装按持久化默认落 virtual 桌面，fusion 用例全败、貌似 D35 间歇
  ——本次 3 轮假阳性即此因）。

**三形态对账更新（退役条件评估输入）**：x11 虚拟桌面 ✓（M3 起，前缀组
持续守门）/ x11 PC 多窗 ✓（M4b 承载/状态面/popup 全绿；2026-10-11 review
gate 在当前构建复跑：fusionHost=ability 强制承载，功能面全绿——
fusion-r20261011-pc-ability，唯一缺口为上页所述 Pad 模拟布局差异的视觉
判定项）/ x11 Pad 多窗 ✓（M4d：本节）——**三形态硬前提全部达成**，
自研窗口栈退役条件评估的事实基础齐备（退役评估本身 = 独立决策，输入见
`docs/superpowers/specs/2026-10-07-display-route-x11-three-mode-gap-analysis.md`
与本节限制清单）。**窗口装饰缺口（#129, 2026-10-11）**：Pad（x11 fusion
子窗）自绘顶条落地，关闭出路盘点闭合；PC/2in1 系统装饰分叉已落、
pending-hardware 实测。

### 2.16 游戏全屏 letterbox 适配：Pad 呈现 + 输入（2026-10-15 落地；PC 承载跟随待硬件）

**现象与归属**：虚拟桌面里游戏 CDS 切 800×600 后，画面只在左上角、四周是
宿主背景色。不是坏点：guest 链是上游 wine 原语义（CDS → 桌面窗缩至 800×600，
`win32u/defwnd.c:2957`，本机 wine-10 trace 实证）；Linux 上「桌面窗 = 屏幕」，
没有宿主层需要介入，所以两侧都不存在「认出桌面窗 / 缩放桌面窗」的逻辑——
缺口在宿主 x11 臂呈现（wlroots scene 按 X 坐标 1:1 摆，无 fit 概念）。
**本适配是产品语义扩展（2026-10-15 用户拍板），不是对齐 Linux**：Pad 画布
保持全屏，桌面内容等比缩放居中 + letterbox 纯黑 + letterbox 吞输入；
PC/2in1 承载窗跟随内容尺寸（Linux 式）。

**机制落点**：
- 身份与矩形：`display/ohos_output.c` 首映射 X client = 桌面窗（与既有
  `g_desktop_shell_mapped` 同一判据）+ FrameStep 变化检测上报
  `evt:desktop-content-resize:WxH`；实测交叉校验 title="shell - Wine 桌面" /
  class="explorer.exe"。
- 呈现（单点消费）：`display/ohos_egl_import.c` present 层 fit——u_uv 子矩形
  采样 + glViewport + 缩小时纯黑 glClear；恒等路径逐像素不变（零回归）。
  不走 per-node 缩放：wlroots scene 每次 commit 都会 `set_dest_size` 收回。
- 输入：`display/display_input.c` motion 逆映射 + letterbox 内钳到内容边；
  按键/滚轮在 letterbox 吞掉（press/release 配对掩码，非吞按下自愈）。
- 几何唯一实现：`display/desktop_fit.h`(C 面) → `compositor/frame/geometry.cpp`
  `ComputeFitRect`/`FitUnmapDisplayX/Y`（COMPOSITOR_UNIFICATION S4 四变换纪律）。
- PC 臂：`ProcessService.ets` 事件转发 → `WineWindowManager.applyDesktopContentSize`
  （`desktopSeparateWindow` 门控 + 等尺寸短路）。

**实测证据（.7，r20261015-020216 会话）**：rect 链 1400×920→800×600→1400×920
（两次进出，含首次启动游戏自退）；letterbox 截图实测（物理 px）：左边条
171 / 内容 2455 / scale 1.5344（设计值 173/2453/1.5333）；letterbox 内 uitest
点击 = press+release 成对 `SWALLOWED (letterbox)` 且游戏无响应；退出游戏后
桌面全屏恢复。用户手测「基本 OK」（含菜单点选进对局）。
**回归（2026-10-15 本构建）**：core 4/4、wine-vulkan 4/4、dxvk 3/3；x11 路线门
gl-baseline / vulkan-present / displayroute-dxvk 各 1/1，全 PASS
（r20261015-020853 … 023329；displayroute-dxvk 首轮撞 D48 残留与「job 型 run
不收尾 force-stop」两次落空，force-stop 后重跑即绿）。

**边界/待办**：PC/2in1 承载窗跟随 = pending-hardware 实测（与 #133/#154 同批）；
多 output 未涉及；本适配无套件守门（人工 + 日志实证），如需门禁可加
desktop-rect 断言。

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

**D47 处置（2026-10-11，止血 + 现场判读）**：首启窗口（本进程从未 ready）首次
`state:failed` 自动重试一次 —— 与失败卡片「重试」同款 `restartEngine` 编排
（`WineEnvService.scheduleColdStartAutoRetry`），再败停在失败卡片；bgStart
后台拉起失败的重试保持无感呈现。
**现场证据（19:3x 一次首启失败，回拉 stderr 在
`.temp/scroll/wine_stderr_20261011.log`）**：失败那次 wineboot 尾段
`syswow64 rundll32.exe setupapi,InstallHinfSection`（32 位/box32）栈溢出 ——
wine 自判 `Unhandled stack overflow ... starting debugger`，此后该次尝试零输出；
~50 份历史归档 stderr 中该行 0 次。重试秒过的机制同源：失败前
mono/msiexec/大部分 wine.inf 已装完（该 stderr 的 spawn 表可证），重试只收尾。
**信号判读纪律（防再误读）**：`Signal 3` = box64 正常机制（2026-10-10 已定
性）；`Signal 11` + box64 行 `siglongjmp ... resume emu` = box64 自处理并恢复，
**全绿批次同样出现**（10-04 批次 S11=5~8 全过）——看到 seg 不等于有问题；唯一
有实质影响的是 wine 自判 `Unhandled ... starting debugger` 一类（本次即该类）。
**未定性（治本待办）**：32 位 InstallHinfSection 为何栈溢出（候选：box32 线程
栈尺寸 / INF 递归深度 / 首启并发状态依赖）；复发时保现场（stderr + hilog
全量落盘再分析）。
**止血的复核与部署验证判据（2026-10-11 R3 复核；findings
`.temp/review/r3-init.md`）**：止血留分支等部署验证（主场景成立、终点与手工
重试一致）。验证时**必须认 hilog `[AUTO-RETRY]` 行**——自动化路径
（winehua.program）的 ensureEngineReady 会抢先走同款 restartEngine 恢复，
把它的成功当成 D47 生效会误判。**复核收敛项已修（2026-10-11 夜，1d83b64/
9d4dd68，构建 hap RC=0）**：F1 失败卡片「关闭」/显式重试作废待定自动重试
（`[AUTO-RETRY] cancelled` 日志指纹；并覆盖 restartEngine 入口——手动重试
窗口内不再追加超预期自动重试）；F2 重试期间「重置」忙时出 toast 反馈。
F3B（skip 也消耗重试预算）未做——timer 句柄已入位，最小改动只剩 epoch。
部署验证时 F1/F2 一并复测（关闭后引擎不得自启；忙态重置有可见反馈）。
**首轮部署验证（2026-10-11 夜）**：全新安装冷启 ×2 均未触发（D47 未复现，
两轮 starting:wineboot→state:ready ≈55s 零 AUTO-RETRY）——[AUTO-RETRY] 链的
验证需彩票命中或故障注入；归档判定件 auto-retry 已入库待信号（无信号时
SKIP 不误报，见 automation/checks/auto_retry.py）。

**故障注入尝试（2026-10-11 夜，4 轮未触发）**：为免等自然彩票试了注入
（编排工具 `scripts/d47-fault-inject.sh` 入库，方法与踩坑全在脚本头注释）：
① 外部 kill wineboot 子树——实测 wineboot loader 正常退出码即 `exit=9`
（暖启成功 boot 同值），supervisor 的 waited-for-exit **不看退出码**，杀子树
走「完成」假路径不复现 failed；② 外部 kill wineserver——`Operation not
permitted`（hdc shell uid 2000 跨 uid 无权限），外力只剩 `aa force-stop`
（整 app）；③④ 首启 wineboot 中途 force-stop 打断（+18s / +42s，后者 =
mono 安装相、与自然失败同相位）→ 重拉新进程——**两次重拉均自愈**（50s 上下
重建到 ready，无 failed）。结论：4 轮未触达 state:failed，链验证挂起待自然
复现——**自动捕获已就位**：彩票命中时 [AUTO-RETRY] 行随 core 套件归档并被
auto-retry 判据自动判定（该行出现即有 PASS/FAIL 结论）。**用户决定
（2026-10-11）：暂缓追 wineboot 崩溃排查**（不好排查；止血已在库，影响面 =
全新安装首启一次重试）。附带收获：打断首启后重拉自愈路径实测稳健（首启可
中断性）；`hdc shell` 不传远端 rc / 杀不动 app 进程 / hilog 起读回环等设备
通道教训已入 memory（ohos-device-evidence-channels）。

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

**~~残留判据债~~（D22 已还 2026-10-06；裁剪机制 2026-10-20 随面板退役）**：
`visual:rgba-quadrants` 曾因 displayroute 出图面是侧栏预览小框而对全屏截图
结构性失效，D22 按归档矩形（`displayroute-preview-rect.json`）先裁剪再判定。
冷启自动起链落地后出图面已是桌面画布（全屏面），预览窗渲染前提消失，
SmokeDevPanel 连同裁剪机制退役 —— x11 与 wayland 同走全屏四象限。
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

**状态刷新（2026-10-11 复核，以归档为准）**：「与显示路线无关」定性**作废**
——wayland 基线已绿：10-06 三轮 dxvk 套件全 PASS（dxvk-r20261006-093803 /
193907 / 234729，`dxvk-legacy-x64/x86` 均过；同夜 19:29 一轮全 FAIL 系
**D24S8 cube-array pending** 拖超时，重跑即愈，与本题不同源）。失败面收敛为
**x11 专属未过**：x11 上最后一轮 dxvk-legacy（job-r20261007-003719，job.json
钉 `WINEHUA_DISPLAY_ROUTE=x11`）拖死 suite，按裁定排除出 x11 判据（见
`smoke/suites/displayroute-dxvk.json` 头注释）；x11 出图门禁（cube，
`displayroute-dxvk-cube-x64`）当夜会话修复后 PASS（job-r20261007-012820，
此前 011201/012006 两轮为会话内中间态）。修复归因（9-30→10-06 之间哪笔改动
修好 wayland 侧）未考古，登记待查。**vkd3d/d3d12 侧：当前归档体系（223 个）
无任何 d3d12 套件运行记录——x11 路线无验证证据，待补跑**。

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
   行为判定归编排侧判定器（wine-trace 等）与外部证据。SKIP ≠ 绿：它不含
   「行为正确」的断言。
2. **启动参数**：`argvMode: "raw"` + `argv: []`（套件条目与 job inline 两处）
   ⇒ 记事本**无参数**启动，不再有「文件不存在」弹框。
3. **注入脚本**（`display_input.c`）：删掉应答该弹框的 `KEY_N` 步骤，H/I 直接
   进文本区。（脚本本体 2026-10-13 已退役删除，本步骤为历史记录。）

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

**2026-10-12 补充（删除重试）**：`remove_sandbox_path` 已加 3 次重试（间隔
0.5s，最新报错文案带 `3 attempts`）——覆盖第二类瞬态：app 冷启被拉起瞬间
el2 视图切换窗口内，`rm -rf` 对真实路径**静默无效**（同时刻同一 shell 对
同树 mkdir 被 SELinux 恒定拒；亚秒后手动 rm 即成功，gate 重跑即愈）。持续
持有（本例：app 运行中占用）重试耗尽后照旧报错，force-stop 协议不变。

### 3.7 XIM 判据前置检查（server spawn 一次性，无 ready 即无 server）

`TrySpawnXimServer`（`display_compositor.cpp`）只由 `HandleXwaylandReady`
触发，且 `static spawned` 在 spawn 尝试**之前**置位——进程生命周期内只试一次，
失败（`xim_bridge_channel_init` 失败 / `StartNativeChildProcess` 非 0）不重试；
也没有 xclient 测试窗那样的 retrigger 补拉（`TrySpawnXclientTestClient` 在
displayroute script retrigger 时补拉，防「产品链先建、门开晚于 ready」）。
⇒ 凡 ready 时机发生在当前二进制生效之前（复用已存会话 / 热更未重启进程），
本进程内 XIM server 不存在，XOpenIM 类判据静默失败（M4-T4 判据实测，
2026-10-08）。

**判据前置**：先取证 xim 活着——沙箱 `.x11-rt/xim_server_stderr.log` 有追加，
或 hilog 有 `xim NCP spawned`；两者皆无则重启会话再判，不带着无 server 的
会话解释判据结果。**未定性**：Xwayland 链重启（退出桌面后再进入）后旧 XIM
子进程的存活与复用行为未实测；`spawned` 置位早于尝试（失败也算已试）是后续
改进（重试 / 补拉如要做）必须一并处理的点。

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

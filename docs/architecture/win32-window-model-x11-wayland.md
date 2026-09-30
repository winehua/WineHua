# Win32 窗口模型覆盖度：X11 vs Wayland 对比清单

适用场景：评估显示方案（winex11+X server vs winewayland+自研合成器）时看这篇；
给「通用 Windows 兼容层」定位下的显示架构决策提供逐维度事实。
最后核实：2026-09-26（winex11.drv 31k 行 / winewayland.drv 上下游两版 / 协议 XML 逐维度盘点）。

参照对象：`.temp/Proton/wine`（Valve wine 11.0）。我们的栈：`thirdparty/wine`（定制 winewayland.drv）+ `entry/src/main/cpp/compositor/`（自研合成器）+ 私有协议 `winehua_toplevel`。

## 先行的三个结构性事实

1. **两个驱动都不用平台窗口树**。winex11 只为父窗口是桌面的顶层窗口建真 X 窗口，WS_CHILD 层级由 Wine 客户端合成（`winex11/window.c:3187`、`bitblt.c:1992`）；winewayland 把非受管窗全部变成 subsurface（`window.c:294`）。Win32 的层级语义在两条路线上都是 Wine 自己重建的——「X11 窗口树同构 Win32」不成立。
2. **X11 的真正优势是旁路机制，不是模型同构**：override-redirect（程序自管窗口）、confine grab、XShape、root drawable 读屏——winex11 靠这些绕开 WM。总评（源码证据支持）：「并不是 X 模型天然更接近 Win32，而是它给了足够多旁路让 Wine 绕开 WM」。
3. **同步状态语义在 X11 上同样是黑洞**：Win32 的 SetWindowPos 是同步的，X 的最小化/全屏经 WM 异步回执，winex11 被迫维护 desired/pending/current 三态并按 WM 打补丁（steamcompmgr 10 处、Mutter 4 处、KWin 3 处、SteamGameId 白名单 4 处，`window.c:1776-1884/2107-2245`）。我们的合成器同步应答，此处占优。

## 15 维度对比

| # | 维度 | X11 机制（winex11） | Wayland 现状（winewayland） | 判定 | 我们现状 |
|---|---|---|---|---|---|
| 1 | 全局坐标 | X root 坐标系即虚拟屏，`XReconfigureWMWindow` 改几何，天然同构（`display.c:269`） | `configure` 只有 w/h 无 x/y；`set_window_geometry` 是单向提示 | 标准**结构性缺失**；合成器可救 | 已绕开：借 `set_window_geometry` 前向坐标，合成器读回（`wayland_surface.c:601`） |
| 2 | 窗口树 | 顶层才落 X 窗口；owned→`XSetTransientForHint`；popup 非 managed→override-redirect（`window.c:1175`） | owned 无协议（`set_parent` 未用）；child→单层 subsurface；xdg_popup 未用 | owned **结构性缺失**，合成器可救 | `winehua_toplevel.set_modal` 已补；child 走 subsurface；`tl_set_parent` 留空 |
| 3 | Z 序 | 只有 Above 可靠，Below 交给 WM 启发式 + `force_below_hack`（`window.c:2446/3599`） | xdg_toplevel 无 raise/lower；仅 subsurface 能 place_above | **结构性缺失**，私有协议可补 | 合成器自持 z-order（`zorder_policy`）；私有 raise/lower 可加未加 |
| 4 | 最小化/还原 | `XIconifyWindow`+WM_STATE 三态机 + 按 WM 补丁（上条） | 只有 `set_minimized`，无 unset | **结构性缺失**（协议不对称） | 合成器猜尺寸 commit + drv 回发 SC_RESTORE 握手（`compositor_utils.h:13`）；治本=私有协议加 unset_minimized（周级） |
| 5 | 全屏/模式切换 | XRandR CRTC + `_NET_WM_STATE_FULLSCREEN`；无 exclusive/borderless 区分（注释自认）；gamescope 特判 10 处（`xrandr.c:352`） | `set_fullscreen` 只是铺满请求，无模式设置协议 | **结构性缺失** | 虚拟桌面缩放模拟（ChangeDisplaySettings→缩桌面，语义合理） |
| 6 | 异型/分层 | per-pixel→重建窗口换 ARGB visual；SetWindowRgn→XShape；**colorkey 无实现**（FIXME，`window.c:3853`） | ARGB shm 可做；均匀 alpha 被忽略（`window.c:642`）；shape 转 alpha 但不裁 input region；无 opacity 协议 | 均匀 alpha/区域裁剪**结构性缺失**（标准） | per-pixel 可用；均匀 alpha 与真裁剪可补（合成器加 per-surface opacity，实现缺口） |
| 7 | NC 区/装饰 | `_MOTIF_WM_HINTS` 抹装饰；NC 区 Wine 自绘（`client_side_graphics` 默认） | 完全不用 xdg-decoration，全自绘 | 平价 | 走 subsurface+桌面模式，整体绕开 |
| 8 | 光标 | `XDefineCursor`/`XWarpPointer`+回声抑制/`XGrabPointer(confine)`——成熟但有 workaround（`mouse.c:244/1476`） | `set_cursor`+cursor-shape（DPI FIXME×2）；warp=`wp_pointer_warp_v1`（2024，可用）；relative-pointer/constraints 已接 | **平价**（DPI 两边都有缺口） | warp/relative/constraints 全接；enter 静默校准 |
| 9 | 输入捕获/焦点 | `XGrabPointer` 与 Win32 SetCapture 语义几乎一一对应；焦点双模型并存 | constraints 的 activate 由 pointer focus 隐式决定，客户端不能主动激活 | capture 主动语义**结构性缺失** | 桌面模式单 root+subsurface 使 Win32 内部焦点不经过协议，已绕开 |
| 10 | 菜单/tooltip | 过不了 managed 启发式→override-redirect 自管理，全局定位；风险全在启发式误判（`window.c:465`） | xdg_popup+positioner 协议够但上游未用；走 subsurface | popup 自管理语义结构性缺失，已被架构绕开 | 上游 subsurface 路线；PC 模式 ArkTS 子窗承载（模态事件携 dx,dy,w,h） |
| 11 | 屏幕捕获 | root drawable `XGetImage`，同构且便宜（读不到 GPU overlay） | 无任何客户端 screenshot 协议 | 标准**结构性缺失** | 合成器持有全部 toplevel 帧（`ToplevelManager::Pixels()`），**缺 Wine GDI 回灌**——实现缺口，非协议 |
| 12 | 多显示器 | XRandR/Xinerama→GDI 设备树，单一 root 坐标系天然跨屏（`display.c:450`） | `wl_output`+`zxdg_output_v1` 枚举可做；跨 output 移窗无通知 | **原生可做** | OHOS 实际单 output，未涉及 |
| 13 | IME | 完整 XIM（`xim.c`），但 XIM 在 XWayland 上支持差 | text-input-v3 已落实（`wayland_text_input.c:224`） | **Wayland 占优** | IME HKL 判定+切换已做 |
| 14 | 剪贴板/拖放 | CLIPBOARD+PRIMARY、INCR、XFixes 异步、格式转换、XDND v3 全套——超出 Win32 能力（`clipboard.c:1923`） | drv 支持 data-control/wlr 与 data_device_manager | **可补**（协议现成） | **合成器两者都未实现，ArkTS 无 pasteboard 桥——完全缺失，当前最大实际硬伤** |
| 15 | DPI | driver 层**零实现**，全在 win32u 层（`display.c:486`） | `wl_output.scale` 整数可做；fractional-scale 缺协议 | 平价（fractional 双方都缺） | DPI 按形态在 ArkTS 层定，未接 fractional |

## 结构性缺失清单（标准协议层面）与抵消状态

| 缺失 | 性质 | 在「自研合成器」架构下 |
|---|---|---|
| configure 无位置回传 | 协议 | 已绕开（坐标前向自报） |
| 无 unset_minimized | 协议不对称 | 已工作（握手补丁）；治本=私有协议加一条 |
| 客户端无 Z 序/激活请求 | 协议 | 合成器自持；私有消息可加未加 |
| 无 per-surface opacity | 协议 | 合成器可加（未做） |
| 无客户端屏幕捕获 | 协议 | 合成器有帧，缺回灌通道（未做） |
| 无模式切换 | 协议 | 虚拟桌面缩放模拟（合理映射） |
| popup 自管理（override-redirect） | 模型 | subsurface/子窗承载已绕开 |
| SetCapture 主动语义 | 模型 | 单 root+subsurface 架构绕开 |

**结论：6+2 项结构性缺失中，8 项被架构绕开或可私有协议/合成器侧补齐；无一两头堵死。**

## 真实差距（合并两侧盘点后的行动项）

按「通用兼容层开箱率」重要性排序，全部为实现级缺口（周级、有参考实现）：

1. **剪贴板/拖放通道**（合成器实现 data-control + ArkTS pasteboard 桥）——当前唯一「完全没有」的维度，通用使用的硬伤，最优先
2. **跨窗口屏幕捕获回灌**（合成器帧 → Wine GDI）——录屏/远程协助/依赖读屏的程序
3. **均匀 alpha（per-surface opacity）**——`SetLayeredWindowAttributes` 类窗口
4. **Z 序私有消息（raise/lower）**——SetWindowPos 的 Z 序语义完整化
5. **unset_minimized 私有消息**——替换尺寸猜测（治本待办转正）

## 对方案之争的含义

- X11 路线在 OHOS 的正确形态是 `winex11 → Xwayland（交叉编译复用）→ 我们的合成器（补协议面）`——「OHOS 没有 WM」不构成反对理由，两条路线的合成器是同一个
- **真正的分歧在图形内存层**：X 生态的 GL 加速链（DRI3/glamor/dmabuf）全部建立在 dma-buf 之上，OHOS 无 dma-buf（Maleoon 无导出）——真游戏 GL 帧要么软件渲染、要么为 X 栈重造 shadow/ZC 式内存共享；而 shadow/ZC 是 Wayland 路径上已建成的核心资产。图形层上「复用优先」裁决反向：现状方案复用自家已验证资产，X11 路线从零
- winex11 的优势面（坐标/capture/读屏/剪贴板/多屏平铺语义）在本架构中已绕开大半；它的黑洞（同步状态三态机+17 处 WM 补丁、弃用子窗口树客户端合成、colorkey 无实现、DPI 零实现）证明 X11 也不是 Win32 的天然宿主
- 因此：**维持 winewayland + 自研合成器路线**，把上述 5 个实现缺口收敛后，窗口模型覆盖度与 winex11+X 的实质差距收敛到语义折扣级（坐标自报 vs 权威、私有协议 vs 标准协议）；通用兼容层的长尾开箱率瓶颈在显示模型上的部分出清，剩余差距在 dll/API 层，与显示方案无关
- 保留的复核点：切 wine-valve（0005-P2）时若上游 winewayland 出现维护倒退，或长尾程序出现「窗口树/override-redirect 类结构性不兼容」的聚集模式，重开本评估

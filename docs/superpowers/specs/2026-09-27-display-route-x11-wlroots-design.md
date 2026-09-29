# 显示路线重构设计：winex11 + Xwayland + wlroots → 鸿蒙窗口

> 状态：设计提案（待实现计划）
> 日期：2026-09-27
> 取代关系：本文显式取代 [0001-self-built-compositor.md](../../decisions/0001-self-built-compositor.md) 的「不替换合成器」结论——该结论的前提是 winewayland 驱动路线；其自设的重开条件（[win32-window-model-x11-wayland.md](../../architecture/win32-window-model-x11-wayland.md) §复核点：「长尾程序出现窗口树/override-redirect 类结构性不兼容的聚集模式」）已被实际触发（2026-09-27 用户实证：长尾程序结构性不兼容 + 窗口语义长尾两类痛点聚集）。
> 证据纪律：本文所有论断要么带 file:line 源码依据，要么显式标注为「M0 设备探针」。

## 1. 背景与动机

现窗口栈三层全自养：魔改 winewayland.drv + 自研合成器（native ~22,900 行 + ArkTS service 4,205 行）+ 私有协议 `winehua_toplevel`。两类实际痛点：

1. **长尾程序结构性不兼容**（重开条件触发项）
2. **窗口语义长尾**：Z 序/最小化还原/popup/模态每补一处都要动 驱动+私有协议+合成器 三处

## 2. 目标架构

```
Wine 程序 → winex11.drv（上游） → Xwayland（上游, shm-only） → wlroots（上游, 钉 tag + 1 处隔离启动补丁, §5 R-SPAWN）
              → OHOS 适配层（唯一自养, 薄壳） → 鸿蒙窗口
```

- **出图**：wlroots headless output → 适配层 → NativeWindow/XComponent。像素搬运走自定义 `wlr_allocator`：帧直接落进 OH_NativeBuffer 承载的自定义 `wlr_buffer`，output commit 后适配层直推 NativeWindow（NativeWindow 直推的具体 API 与 CPU 写入 usage 组合为 **M0 真机首验项**）。渲染器两态：**M0 起步 = pixman 软合成**（bootstrap，游戏热路径有每帧两次全幅 CPU 搬运代价）；**M2 必达终态 = wlroots GLES2 + OH_NativeBuffer EGLImage 导入**（`EGL_OHOS_image_native_buffer` 扩展，SDK `eglext.h:1441-1444` + `egl.h:329` eglCreateImage），全链 GPU 零 CPU 拷贝、拷贝数与旧路线持平（§5 R-ZC②）。PC 模式 = 每 toplevel 一个 output 配一个鸿蒙系统窗；Pad 模式 = 单 output 虚拟桌面（策略层合成，非 X root 桌面）
- **Xwayland 固定 rootless**：钉版 wlroots 启动 Xwayland 的 argv 硬编码 `-rootless`（`xwayland/server.c:50`）；且 rootful 下不存在每窗 wl_surface，§4.2 的 present 重锚与 ZC per-window 通路整体失效。Pad 的「虚拟桌面」= 策略层把全部 xwayland surface 合成到单 output（与 gamescope 形态一致），不是 X root 窗
- **输入**：OHOS 事件 → `wlr_seat_notify_*` 注入（`wlr_seat.h:461-679`；notify 函数不收设备参数，seat 创建不依赖 backend，`types/seat/wlr_seat.c:282`）。键盘侧需自建假 `wlr_keyboard`（`wlr/interfaces/wlr_keyboard.h:20` `wlr_keyboard_init` + `wlr_seat_set_keyboard`）为客户端提供 keymap，缺这步 Xwayland 键盘死
- **IME**：鸿蒙输入法 ↔ text-input-v3（wlroots 内置 `wlr_text_input_v3.c`）↔ Xwayland（需移植下游 IME 桥，见 §5 R-IME）
- **图形加速链不动**：VirGL/Venus/DXVK/vkd3d 的 vtest 链原样保留，仅 present 落点重接（§4.2）

## 3. 选型依据（全部源码实证）

| 选型 | 判定 | 关键证据 |
|---|---|---|
| **wlroots 钉上游稳定 tag** | ✅ | 最小集合 `-Dauto_features=disabled -Dxwayland=enabled`；GBM/udev/libinput/session 全可关（`wlroots meson.build:96-134`、`backend/meson.build:11-29`）；全树无 udev/systemd/logind 硬引用。**gamescope 钉 0.20.2 零补丁**（`.gitmodules` 直指 freedesktop，无 Valve fork）；headless 可动态多 output（`headless.h:25`）；自定义 `wlr_buffer` 公开 API（`wlr_buffer.h` impl 五函数指针）；text-input-v3 内置（`types/wlr_text_input_v3.c`，350 行全请求实现） |
| **Xwayland 纯 shm** | ✅ 一等公民 | `-Dglamor=false` 编译 + `-shm` 运行时（`xwayland.c:109,214`），glamor 失败自动回退（`xwayland-screen.c:1102-1107`）；无 glamor 内容走 memfd/tmpfile→wl_shm（`xwayland-shm.c:126-305`）；rootless 下 managed toplevel 不经 xdg_toplevel，是裸 wl_surface + xwayland_shell_v1 标记（`xwayland-window.c:1538-1541`；wlroots 侧 `xwayland/shell.c:173` 建 global，`wlr_xwayland_create` 自动创建 `xwayland/xwayland.c:139`）；global 缺失无 fatal 项（bind 清单 `xwayland-screen.c:520-601`，wl_seat 在 `xwayland-input.c:3411` 且要求 version≥3） |
| **winex11.drv 上游原版** | ✅ 极简且零包袱 | 硬依赖仅 libX11+libXext（`configure.ac:1278-1280`），其余 X 扩展可选 soname 探测逐个降级；驱动源码零 Linux 桌面设施（grep /proc、/tmp、udev、dbus、abstract 全空）；**上游零修改**（git log 全上游作者）；与 winewayland 可同编（各驱动独立 enable 变量 `configure.ac:1466,1474,1478`；`:3383-3384` 仅 MAKEFILE 注册） |
| **模态语义走 X11 标准机制** | ✅ | WM_TRANSIENT_FOR + _NET_WM_STATE_MODAL 经 XWM 解析为 `wlr_xwayland_surface` 的 `modal`/`parent` 字段 + request_* 信号（`xwayland.h:181,197-203`）；PC 模式上报由此重建 |
| 同形态先例 | Steam Deck gamescope | Valve 官方（ValveSoftware/gamescope），wlroots 内核，游戏链路 = winex11→Xwayland→gamescope→屏幕，与本方案只差最后一跳（DRM ↔ NativeWindow） |

## 4. 现行机制实证（迁移的对接面）

### 4.1 ZC 传输机制

virglrenderer 跑在**独立 NCP 子进程**（`graphics_broker.cpp:1322` libvirgl_child.so；`:196` dlopen 备选）。ZC 跨进程传输 = **OHOS BufferQueue**：合成器侧 `OH_NativeImage_AcquireNativeWindow` 得生产者窗口（`egl_renderer.cpp:225-228`）→ 交 virgl_child 渲染 → `OH_NativeImage_UpdateSurfaceImage` 回收为 GL 纹理（`native_image.h:114,157`）。握手文件 `winehua_zc_surface_*`（`graphics_broker.cpp:81-82`）只做就绪协商。

### 4.2 Vulkan present 锚定（C7-C10）

WSI 整体私有化：guest 永远拿不到真 VkSurfaceKHR，窗口身份 = 高位 tag `0x574853` + wl_surface id（`contracts.md:360`），venus 经 `VCMD_WINEHUA_VK_PRESENT` 把 SURFACE_ID 送 virglrenderer（`contracts.md:461`），宿主回调回主仓库。**M2 重锚 = 把 id 来源从 winewayland 的 wl_surface id 换成 X window id（经 `wlr_xwayland_surface` 双字段映射，`xwayland.h` window_id+surface），链路其余不动。**

### 4.3 其他对接事实

- Wine 窗口 shm：wineserver section 转 fd（`wayland_surface.c:880-908` `wine_server_handle_to_fd` → `wl_shm_create_pool`）——沙箱内跨进程 fd 共享已在生产验证
- GL 现状：winewayland GL = EGL window surface（`opengl.c:122-123`）+ pbuffer 模拟（`:141,168-172`）+ readback（`opengl_readback.c`）
- winex11 GLX 面：`winex11.drv/opengl.c` 内部 40 个唯一 pglX 入口声明 / 46 个动态加载点（声明块 `opengl.c:265-317`，Proton wine 11.0 实测口径）= GLX-over-EGL 桥的覆盖清单
- DXVK d3d9：**编了没打包**（`build_dxvk.sh:61` 只装 d3d11/dxgi；`assemble.sh:325-334`）——启用是打包+验证工作
- 驱动选择单点：`dlls/win32u/driver.c:1015-1033` OHOS bypass 强制直载 winewayland——切换的唯一 Wine 侧硬改动
- 隐藏依赖：wayland-server 头/.pc 目前靠 `BUILD_GUEST_GFX=1` 才装（`build_ohos_guest_gfx.sh:589-601,654-666`）——先固化进 build_wayland.sh

### 4.4 窗口管理体系差异分析（X11/ICCCM ↔ wlroots 策略层 ↔ 鸿蒙）

层级澄清：X11 的窗口管理不是 X server 的能力，而是 WM（普通客户端）与程序间的约定（ICCCM/EWMH）。因此真正的对比是四层错位：winex11 期望 ↔ XWM 翻译（上游养）↔ **wlroots+适配层=策略层（权威归我们）** ↔ 鸿蒙。鸿蒙"缺"的都发生在策略层与鸿蒙之间，而虚拟 root/输出/桌面尺寸均由策略层自定义。

| X11/ICCCM 期望 | 落点 | 判定 |
|---|---|---|
| 全局 root 坐标系 | 自定义虚拟 root：Pad=单 output、PC=多 output | 平价（与现状同构） |
| 客户端自由定位 | XWM 转 `request_configure` 信号（`xwayland.h:198`），策略层批准 | 平价，标准路径 |
| Z 序权威 | Pad=策略层全权；PC 跨应用置顶今日同做不到，非新增 | 平价 |
| override-redirect 窗口（菜单/tooltip） | XWM 一等公民：`xwayland.h:145` 字段、`:227` 信号、`:394` wants_focus | **变好**（上游管，gamescope 蒸汽菜单同款） |
| 模态/属主（WM_TRANSIENT_FOR/_NET_WM_STATE_MODAL） | XWM 解析为 `modal`/`parent`（`xwayland.h:181`；parent 字段 `:156`）→ PC 模式沿用现有模态子窗承载 | 已有等价物 |
| 焦点（server 全局） | XWM+适配层三方翻译；winex11 本为双焦点适配谱系 | 平价 |
| XRandR 模式切换 | rootless+viewporter 自动开启（`xwayland-screen.c:120-126`，fake modes 追加 `xwayland-output.c:436`）；钉版 wlroots 亦带 `force_xrandr_emulation` 选项（`xwayland/server.h:21`，gamescope 同款调用）；headless output 尺寸任意设 | **变好**（缩放模拟→真改虚拟 output） |
| 装饰（_MOTIF_WM_HINTS） | winex11 默认自绘 NC，XWM 报 undecorated（MOTIF 解析 `xwm.c:976`；字段/信号 `xwayland.h:165,225`） | 平价 |
| XGrabPointer/confine | Xwayland 原生使用 pointer-constraints：confine（`xwayland-input.c:3876`）、XWarpPointer 仿真走 lock_pointer（`:3627`，前置 relative_pointer+constraints `:3765-3782`）；wlroots 服务端实现 `wlr_pointer_constraints_v1.h` | 平价，上游全链 |
| 剪贴板/X 选区/拖放 | wlroots XWM 内建选区同步（`xwayland/selection/{selection,incoming,outgoing,dnd}.c`） | **变好**：旧路线完全缺失（旧盘点列为最大实际硬伤，`win32-window-model-x11-wayland.md:32`）；剩 OHOS pasteboard 桥适配 |
| 屏幕捕获（root 读屏类） | rootless 下 root 无内容，`XGetImage` 读 root 为黑；落点改走 wlroots 输出导出或适配层合成帧回灌 | **不继承旧盘点「X 读屏便宜」的结论**，落点 M2 定 |

真正要写的策略代码：①窗口放置/焦点策略——Pad 模式近免费（winex11 虚拟桌面模式按弱 WM 设计：`winex11.drv/window.c:1749` 忽略 WM 配置变更、`:2712-2715` 自建桌面窗），PC 模式 = output↔OHOS 系统窗生命周期绑定（异步时序，等价物为现有 ArkTS service 4,205 行的平移）；②参考实现 = gamescope steamcompmgr + 本项目 `toplevel_manager` 语义资产。PC 模式的 surface→output 绑定规则（X 全局坐标编址、output 建/销时机——headless 0.20.2 仅 `wlr_headless_add_output`，销毁走通用 `wlr_output_destroy`、OR 菜单跟随父窗所在 output）**为显式留白，M1 设计定**。M1/M2 验收补：PC 多窗、菜单 tooltip、模态对话框用例。

## 5. 风险清单

### A 类：已源码实证 + 缓解路径明确

| ID | 风险 | 证据 | 缓解 |
|---|---|---|---|
| R1 | GLX 管线断点：D3D8/9/OpenGL 现走 winewayland EGL readback；winex11 需 GLX，shm Xwayland 无 GLX 扩展 | `build_wine.sh:118-122`、`configure.ac:1408-1425`（缺 libGL 仅 WARNING，会静默失效） | **M1-T6 spike 已裁决（2026-09-29，真机 r0929115004-t6j，探针 `winehua_glx_bridge_probe`）**：①GLX-over-EGL 桥 **no-go**——桥按设计走 EGL wayland + virpipe + glReadPixels，实测回读带宽仅 19.2MB/s，800×600 全帧 96.3ms（gen 1.1 + read 95.2）≈10fps，超 30fps 预算 3 倍；512×512 同样超（53.2ms）。**实测发现第三路线（改判依据）**：X 路线（DISPLAY=:0）WGL→winex11 GLX 上下文创建**成功**（预期失败点未发生）——guest mesa 经 drisw（GLX on X）+ `GALLIUM_DRIVER=virpipe` 到同一 virgl (Maleoon 920)，800×600 全帧 6.5ms（读 310.9MB/s，比 EGL-wayland 回读快 16 倍）≈150fps，大幅达标 ⇒ **R1 改走 wine 原生 winex11 GLX（drisw+virpipe），自研桥取消**。M2 待验：可见窗 present 链（探针 A 段用隐藏窗上下文，drisw present→XPutImage→Xwayland→scene 未过真机）；renderer 字符串两路线相同但 16 倍读出差的机制未解释（drisw 本地 shadow vs vtest 往返，挂起）。②DXVK d3d9 收窄面降级为 x11drv GLX 不可用时的备胎。B 段投递（BitBlt→winex11）实测 0.10ms/1MB——量到的是 GDI 本地 blit，真实屏幕投递走 surface commit 路径，其成本已由 M1-T3 合成帧率（≥25fps）与 T5 端到端覆盖 |
| R-ZC | wlroots 只认 buffer 的 fd 门（dma-buf，OHOS 无）或 data_ptr 门（`render/pixman/renderer.c:251-255`、`render/gles2/texture.c:419-422`），不认识 BufferQueue；pixman 起步态下游戏热路径 = GPU→CPU map→CPU 合成→GPU，每帧两次全幅搬运（bootstrap 代价，非终态） | 同左 | ①（bootstrap）data_ptr 门零补丁：自定义 wlr_buffer 包 OH_NativeBuffer。获取链三跳（API 实证）：`OH_NativeImage_AcquireNativeWindowBuffer`（SDK `native_image.h:300`，`AcquireLatestNativeWindowBuffer` `:485`，since 22 ≤ 本档 23）→ `OH_NativeBuffer_FromNativeWindowBuffer`（`native_buffer.h:286`）→ `OH_NativeBuffer_Map`（`:233`）。**口径：API 存在已证、组合未验证**——现行合成器零 `OH_NativeBuffer` 命中（全程纹理模式 `UpdateSurfaceImage` `native_image.h:157`），纹理/缓冲两种消费模式能否共存同一 OH_NativeImage 未验证 ②**M2 必达终态（GL 门）**：wlroots gles2 加 OHOS 导入分支——`eglCreateImage` + `EGL_NATIVE_BUFFER_OHOS`（SDK `eglext.h:1441-1444`），输入帧与输出 buffer 全程 GPU，拷贝数与旧路线持平。传输层已被现行 ZC 生产验证。**② 运行时可用性 M2-T2 已裁决（2026-09-30，真机 r0930013431-t2d，探针 `ohos_egl_import_probe`）：可用，EGL 导入全链通（分配→Map 写→eglCreateImage→glEGLImageTargetTexture2DOES→FBO 回读逐像素比对一致）。关键实测约束：`EGL_NATIVE_BUFFER_OHOS` 的 EGLClientBuffer **不能是裸 `OH_NativeBuffer*`**（被 EGL 实现按 native_window 对象校验拒，EGL_BAD_PARAMETER 0x300c），必须先经 `OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer` 包成 `OHNativeWindowBuffer*`（`external_window.h:468`）再传——这是 gles2 导入分支的实现前提。探针在每次 displayroute run 自动重跑（harness 先清标记），结论经 `marker:` 判定器裁决（smoke/jobs/displayroute-notepad.json 声明）** |
| R-SPAWN | **决策（平台硬约束）：子进程必须走 NCP**，fork/exec 不可用；而 wlroots 启动 Xwayland 只有 fork+execvp（`xwayland/server.c:133`；`server.h:64` 同源内部 exec），无「接管外部进程」公开 API → 启动路径必须 NCP 化适配 | 同左 | 三件套（全部落在已有机制上）：①xserver 构建增出 `libxwayland` 共享库（构建级小补丁，代码零改）②NCP shim 子进程 `libxwayland_child.so`：入口同 `virgl_child.cpp:471` `Main(NativeChildProcess_Args)` 形态——起 abstract unix socket（生产先例：wine NCP 子进程连合成器即走 abstract，P4）收 SCM_RIGHTS 传来的 {x_fd×2, wl_fd[1], wm_fd[0], displayfd}，以 `-listenfd` argv 直调 Xwayland main ③wlroots `server.c` 启动点补丁：跳过 fork/execvp，改 NCP spawn + fd 下发。fd 跨进程传递先例：winewayland section→fd→wl_shm（`wayland_surface.c:880-908`，NCP 子进程↔app 通道在产）。wlroots 钉版口径由此修正为「1 处隔离启动补丁」。**M0 已落地（T7/T9，2026-09-28）**：三件套按此形态实现并真机跑通；补丁增量两条实测教训——`wl_client_create` 后移到 spawn 之后时 `'start'` 事件 emit 必须随之后移（消费方 handle_server_start 读 `server->client` 给 shell_v1 设白名单，时序破即 bind 拒）；两段式 `wlr_xwayland_create_with_server` 调用方（C++ 宿主）不经 `wlr_xwayland_create` 包装层，shell_v1 全局须由 with_server 自建（否则 xwm_create 挂 NULL 信号 SEGV） |
| R2 | wlroots shm 分配器走 shm_open→/dev/shm（`util/shm.c:30`），沙箱可能没有 | 同左 | **已探针解除（2026-09-27，见 B 类 P1/P2）**：shm_open 与 memfd_create 在沙箱均可用，默认分配器直接工作；自定义 `wlr_allocator` 降级为备胎 |
| R-VER | wlroots 0.20.2 与 0.21 均要求 wayland-server ≥1.24（`wlroots meson.build:88-90`）；项目现有 1.22 | 同左 | **升 thirdparty/wayland ≥1.24 为必做项**（关键路径） |
| R-xkb | xkbcomp 是 Xwayland 硬运行时依赖，缺 = 键盘死 | `xkb/ddxLoad.c:105-212` → `xwayland-input.c:372-374` BadValue | xkbcomp 二进制 + XKB 数据树（项目已带 share/X11/xkb）+ `-xkbdir` 进沙箱包。**M0 已落地（T7）**：xkbcomp 不可执行（NCP exec 禁令）→ 宿主侧预编译 xkm 缓存（assemble 期 `xkbcomp -w 1 -R<xkb树> keymap.txt → wine-data/xkm/server-0.xkm`），xserver RunXkbComp 打缓存命中补丁（命中即跳过 fork/exec），shim 拷入 `$XDG_RUNTIME_DIR`；真机日志 "XKB: Reusing cached keymap" 实证，键盘激活 FatalError 解除 |
| R-IME | 主线 xserver 无 text-input 桥 | grep XWAYLAND_IME 零命中；mutter 树亦无 Xwayland IME 桥（text-input 均为 Wayland 客户端侧，实查） | 下游补丁源待定位；否则自写 XWM↔text-input 桥或 OHOS 应用层注入，M2 裁决 |
| R-WSI | Venus 是否暴露 VK_KHR_xcb_surface 未实测 | `winex11.drv/vulkan.c` 走 X11 WSI | **M1-T7 探针已裁决（2026-09-29，真机 r0929121315-t7b，`wsi_xcb_probe` 两路线 SKIP=能力缺席）**：venus **不暴露** `VK_KHR_xcb_surface`（也不暴露 xlib；`VK_KHR_surface` 在，spec=25）⇒ winex11 路线无 Vulkan present，X 路线的 D3D/Vulkan 呈现只能走 M2 重锚（SURFACE_ID 来源换成 X window id，§4.2）；Vulkan 程序切到 X 路线前（M2 前）呈现仍依赖 wayland 路线私有 WSI。与 R1 裁决合并后的 X 路线能力边界：GL 有（winex11 GLX/drisw，M2 验 present 链）、Vulkan 无（重锚制） |
| R-CPL | win32u present_rect 补丁按 wayland 握手写 | `dlls/win32u/window.c:2312-2335` | 切换时回归 war3 全屏场景 |
| R-REPRO | wayland-server 头/.pc 隐藏依赖 | `build_ohos_guest_gfx.sh:589-601` | 先固化进 build_wayland.sh，rm sysroot-ext 重建演练 |
| R-ARK | ArkTS modal 事件源随私有协议消失 | `toplevel_event_bus` | 用 `wlr_xwayland_surface` 事件重建（§4.4 模态行；request_*/set_* 信号群 `xwayland.h:197-203,220-227`） |

### B 类：设备探针（本机源码不可验，真机裁决）

**2026-09-27 已在真机 192.168.1.6:33363 执行完毕**（探针件：`feature/sandbox-probe` 分支 `cc03d0d`，结果落盘 el2/base/temp/sandbox_probe_result.txt，uid=20020250）：

| ID | 探针 | 结果 | 对设计的影响 |
|---|---|---|---|
| P1 | shm_open（/dev/shm） | ✅ OK（access W_OK 通过、O_CREAT\|O_EXCL 得 fd=30；opendir EACCES 仅不可枚举，不影响） | **wlroots 默认 shm 分配器可直接用**，R2 的自定义 allocator 降级为备胎 |
| P2 | memfd_create + ftruncate + mmap | ✅ OK | Xwayland `xwayland-shm.c` 首选路径可用；wl_shm 全链成立 |
| P3 | tmpfile 候选目录 | XDG_RUNTIME_DIR 未设（启动时注入即可）；el2/base/temp、el2/base/cache mkostemp ✅ | env 注入机制现成，无阻塞 |
| P4 | unix socket | abstract 绑定 ✅；路径绑定在 app temp ✅、在 /tmp ❌（/tmp 不存在） | X11 客户端可走 abstract 传输（不依赖文件系统） |
| P5 | /tmp | **/tmp 整个不存在**（ENOENT） | 触发路径策略决策，见下 |

**路径策略决策（2026-09-27）**：一切硬编码宿主路径（`/tmp` 系）**统一重定向到沙箱目录**，不做「/tmp 可否创建」的探针与适配。涉及：wlroots `xwayland/sockets.c`（lock/socket 路径常量硬编码 `/tmp/.X%d-lock`、`/tmp/.X11-unix/X%d`，`sockets.c:19-23`；重定向 = 换常量 + 确保 socket_dir 存在，~20 行）；Xwayland xkm 输出与 shm tmpfile 已有 `$XDG_RUNTIME_DIR` 回退链（`xkb/ddxLoad.c:62-95`、`xwayland-shm.c:135-167`）。X11 abstract socket 名保持原字符串（P4 证明 abstract 不触碰文件系统，双侧均为我方构建，无需改名）。

**T9 实测修正（2026-09-28）**：决策方向正确但「abstract 名保持原字符串」不完整——libX11 客户端（XCB 传输）只探测**标准名** `@/tmp/.X11-unix/X<n>`，而 filesystem 侧必须落 XDG（/tmp 不存在）。落地形态：wlroots `sockets.c` abstract 侧固定绑标准名（abstract 名是 netns 内纯字符串，无需 /tmp 存在）、fs 侧按「标准 /tmp 优先，失败回退 XDG」解析；xtrans 1.5.2 客户端 `SocketUNIXConnect` 写死 fs 路径（`abstract=0`），补丁 `scripts/patches/xtrans-abstract-client.patch` 让 local 传输客户端走 abstract 名。

## 6. 交叉构建清单

```
必做升级:  thirdparty/wayland 1.22 → ≥1.26（wlroots 0.20.2 要 server/client ≥1.24, xserver 主线要 client ≥1.26, 取高者一并满足）
钉版已裁:  Xwayland = xwayland-24.1.13（xorg/xserver 仓库 xwayland-24.1 分支 tag c5a47fda8, 2026-07-08 发布）
新增依赖:  libdrm ≥2.4.129（wlroots 无条件依赖, 关不掉——像素格式头; 另 pixman ≥0.43.0 有版本门槛）
X client:  xorgproto → xcb-proto → libxcb(shm/randr/xfixes/composite/res 分模块) → libX11 → libXext
           可选扩展: Xrender/Xrandr/Xfixes/Xcursor/Xi/Xinerama/Xcomposite/Xxf86vm（soname 逐个降级）
Xwayland:  pixman, libxau, libxdmcp, xtrans, libfontenc, libXfont2, libxshmfence, libxcvt, libxkbfile, libsha1 + XKB 数据（xkbcomp 可执行不可得, 见 R-xkb 备注）
wlroots:   pixman(共用), xkbcommon ≥1.8, wayland-protocols ≥1.47, libdrm ≥2.4.129
```

### 6.1 Xwayland 钉版裁决（M0-T5/T6 实测, 2026-09-27）

**结论：钉 xwayland-24.1.13。** 证据（候选项 24.1.13 vs 主线 master 0da4d248）：

| # | 判据 | xwayland-24.1.13 | master | 裁决 |
|---|---|---|---|---|
| 1 | xwayland-shell-v1（rootless toplevel 标记） | ✓ `hw/xwayland/meson.build:50` | ✓ 同 | 平手 |
| 2 | wlroots 消费的 pc 变量（have_listenfd/terminate_delay/no_touch_pointer_emulation/force_xrandr_emulation 全 true）+ have_initfd（fd 直传, R-SPAWN 关键） | ✓ `:167-177` | ✓ 同 | 平手 |
| 3 | OHOS 构建面 | 整分支即独立 Xwayland 项目（root meson `project('xwayland')`），无 Xorg DDX 选项面 | 全量 xserver, 需逐项裁剪 | **24.1 胜** |
| 4 | 维护/安全 | Xwayland 独立发布线（24.1.13 于 2026-07-08 发布），安全修复主线 | 快速演进, API 漂移不利钉版 | **24.1 胜** |
| 5 | NCP 适配面（executable→shared 补丁） | 同样补丁, 分支面小 | 面大 | **24.1 胜** |

行为面实测补充：`wayland_req >= 1.21.0`（`meson.build:66`，我方 1.26 满足）；`-shm` 选项在（`xwayland.c:214`，M0 shm-only 依据）；xwayland_shell_v1 bind 以 rootless 为门（`xwayland-screen.c:533-535`，与 §2 一致）；`-force-xrandr-emulation` 在（`xwayland.c:247`）。

### 6.2 M0 实测结论（2026-09-28，T7-T9 全链真机）

**出口状态：M0 达成。** 全链 = libX11 client → abstract socket → Xwayland（NCP）→ XWM/xwayland_shell_v1（wlroots）→ pixman/自定义 allocator → NativeWindow 直推 XComponent，真机出图，5 分钟+ 存活 0 喷溃，伴随轮 core 套件 PASS 4/4。

**NativeWindow 首验矩阵（T8，全部实测）**：

| 路径 | 结果 | 结论 |
|---|---|---|
| AttachBuffer 后直接 FlushBuffer | 41207000 BUFFER_STATE_INVALID | 不可行 |
| Request→GetBufferHandleFromNative→virAddr 写 | handle 在但 virAddr=NULL（BufferQueue 不自动 map 给生产者），帧 5 起 Request 报 40601000 | 不可行 |
| LockBuffer/UnlockAndFlushBuffer（API 23） | 设备无符号（dlsym 守卫落空） | 老设备不可用 |
| **Request → mmap(handle->fd) → memcpy → FlushBuffer（采纳）** | 稳定出图 | stride 语义 = 字节/行（size=stride*height，误 ×4 曾写越界）；CRC 必须在 wlr_buffer end_data_ptr_access 之前算（Unmap 后指针失效） |

格式：wlr_buffer 背书 OH_NativeBuffer，DRM↔OHOS 映射 ABGR/XBGR8888→RGBA_8888、ARGB/XRGB8888→BGRA_8888；usage = CPU_READ\|CPU_WRITE\|CPU_READ_OFTEN；stride 由 OH_NativeBuffer_GetConfig 回读（800 宽实测 3328）。headless output 首次 enable commit 必须带 custom_mode。帧率实测 ~8fps（每帧完整 Request→mmap→memcpy→munmap→Flush + BufferQueue 节流），零拷贝归 M1。

**合成器最小件实测必备集（缺一即断链）**：`wlr_renderer_init_wl_display`（wl_shm 全局——缺则 Xwayland 首窗 Map 即 NULL proxy SEGV）、`wlr_compositor_create` + headless backend + 自定义 allocator、XWM 的 `request_configure` 应答（`wlr_xwayland_surface_configure`）与每帧 `wlr_surface_send_frame_done`（无 scene 栈须手动，否则 client 内容不提交）；X 窗口像素读 `surface->buffer`（wlr_client_buffer）而非 `current.buffer`（空 commit 后者为 NULL）。

**X11 连接面（T9）**：libX11/XCB 客户端只探标准 abstract 名 `@/tmp/.X11-unix/X<n>` 与 fs 路径 `/tmp/.X11-unix/X<n>`；沙箱 /tmp 不存在 → 服务端 abstract 固定绑标准名 + xtrans 客户端补丁（见 §5 路径策略修正）；Xwayland 无 -auth 也拒本地 client（peercred 豁免沙箱未生效）→ NCP 形态加 `-ac`。

### 6.3 M1 实测结论（2026-09-29，T1-T8 全链真机）

**出口状态：M1 达成。** 出口判据 = notepad 经 winex11→Xwayland→xwm→scene→NativeWindow 端到端输入出字（T5 "hi" 实测截图）；伴随轮 core 4/4 PASS（T5 两轮 + T8 冷启一轮）。

**winex11 接入形态（T4/T5）**：guest X11 栈（libX11 + xkb 数据）随 wine-data 进沙箱；驱动选择 = `DISPLAY` 环境变量分支（`WINEHUA_DISPLAY_ROUTE=x11` + `WAYLAND_DISPLAY` 清空 → winex11，否则 winewayland），不改 win32u bypass 单点。三个 M1 实测必修点（各自独立断链）：

1. **X screen 尺寸 = wl_output 全局镜像**：无 `wlr_output_layout` 时 Xwayland 报 screen 0×0 → wine `xinerama_init` 得 0×0 桌面 → `get_desired_wm_state` 永远 WithdrawnState，**wine 永不 XMapWindow**。修 = 合成器持 layout 并 `add_auto` output。
2. **MIT-SHM 沙箱双向致命**：Xwayland 侧 shmat → seccomp SIGSYS（"Server aborting"，全体 X client 掉线）；wine 子进程侧 shmget → SIGSYS → box64 信号处理器 siglongjmp 破坏 wine 执行流（SetWindowPos 中途消失）。修 = Xwayland `-extension MIT-SHM` + wine configure `--without-xshm`（winex11 走纯 XImage 路径）。
3. **重触发管道阻塞读**：libwayland 对新增 fd 不设 O_NONBLOCK，drain 循环第二次 read 永久阻塞 → 合成器+Xwayland 全冻结。修 = pipe 两端 fcntl O_NONBLOCK。

**输入链（T1/T2/T5）**：OHOS 触摸/键盘 → 注入桥 → wlr_seat → xwm → wine。多窗口语义 = surface 列表 + 命中测试 + 焦点随动；模态对话框焦点不依赖几何，`FocusClient(client_xs)` 几何无关寻址（notepad 对话框几何逐轮漂移实测 874×655@4,30 ↔ 562×400@4,23）。**挂起**：最后一键间歇不入编辑控件（注入侧日志逐字节同形，差异在 wine 内部非确定路径），T8 未复现到根因。

**合成帧率（T3）**：scene 化管线贯通，**18.6-18.7fps 持续**（M0 8.5fps 的 2.2 倍），~54ms/帧 = copy+flush 17-19ms + scene render+commit 18-20ms；瓶颈在 NativeWindow BufferQueue copy+flush 段，零拷贝归 M2 present 重构（≥25fps 目标随零拷贝达成）。

**GLX 裁决（T6）**：①GLX-over-EGL 桥 **no-go**（wayland EGL+virpipe 回读 19.2MB/s，800×600 全帧 96.3ms ≈ 10fps，超 30fps 预算 3 倍）；**改判第三路线** = wine 原生 winex11 GLX（mesa drisw + virpipe）800×600 全帧 6.5ms ≈ 150fps 大幅达标，自研桥取消。M2 待验 drisw 可见窗 present 链。

**R-WSI 裁决（T7）**：venus 不暴露 `VK_KHR_xcb_surface`/xlib（`VK_KHR_surface` 在）⇒ winex11 路线无 Vulkan present，X 路线 Vulkan 呈现走 M2 重锚（§4.2）。X 路线能力边界：**GL 有、Vulkan 无（重锚制）**。

**回归与状态残留（T8）**：core + wine-vulkan 在冷启干净态 4/4 + 4/4 PASS。**displayroute 会话状态会残留到后续套件**（T5 t5v、T8 两度复现：displayroute 任务后直接跑 core，帧采集抓到 app UI+虚拟桌面合成帧而非测试全屏窗口；生产者帧率 88fps 正常，仅采集面错位）⇒ **套件回归前 force-stop + 冷启动**为既定流程。

## 7. 里程碑与回退线

| 阶段 | 内容 | 退出条件 |
|---|---|---|
| M0 | Xwayland 钉版裁决 + wlroots OHOS 交叉编译 + Xwayland NCP 启动适配原型（R-SPAWN）+ NativeWindow 直推真机首验 + headless/pixman 出图进 NativeWindow + Xwayland 跑 xterm | 跑不通 → 回退「现有合成器 + Xwayland」（winex11 收益保留；rootless 回退需先给旧合成器补 xwayland-shell-v1——现零实现，协议面很小；旧合成器 xdg_wm_base 已具备 `xdg_shell.cpp:428-433`，rootful 可跑但 present 重锚不成立，仅保窗口不保 ZC） |
| M1 | winex11 交叉接入，窗口语义验收；GLX-over-EGL 桥最小原型裁决；R-WSI 探针 | 记事本类输入/窗口正确 |
| M2 | Venus/ZC 重锚（§4.2/§5 R-ZC）、IME、双窗口形态 | dxvk 套件出图正常 |
| M3 | 旧合成器退役 | core / wine-vulkan / dxvk 套件不回退 + 长尾样本验收 |

## 8. 验收标准

1. 触发重开的那些长尾样本（结构性不兼容类）在 X 路线可启动且行为正确；样本清单与「行为正确」判据在 M1 落定为具体程序列表
2. 现有 core / wine-vulkan / dxvk 自动化套件不回退
3. 档位系统（graphics-matrix 四档）在新链路语义不变
4. 出图/合成性能：M2 起以 dxvk、core 套件运行数据与旧链路对照，作为**观察项**（不设硬线；要设硬线则在 M1 钉基准程序与分辨率）

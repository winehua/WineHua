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
Wine 程序 → winex11.drv（上游） → Xwayland（上游, shm-only） → wlroots（上游, 钉 tag）
              → OHOS 适配层（唯一自养, 薄壳） → 鸿蒙窗口
```

- **出图**：wlroots headless output（pixman 软渲染起步）→ 适配层 → NativeWindow/XComponent。像素搬运走自定义 `wlr_allocator`：帧直接落进 OH_NativeBuffer 承载的自定义 `wlr_buffer`，output commit 后适配层直推 NativeWindow（不走 read_pixels 二次拷贝；与 §5 R-ZC① 同一套自定义 buffer 机制；NativeWindow 直推的具体 API 与 CPU 写入 usage 组合为 **M0 真机首验项**）。PC 模式 = 每 toplevel 一个 output 配一个鸿蒙系统窗；Pad 模式 = 单 output 虚拟桌面（策略层合成，非 X root 桌面）
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
| R1 | GLX 管线断点：D3D8/9/OpenGL 现走 winewayland EGL readback；winex11 需 GLX，shm Xwayland 无 GLX 扩展 | `build_wine.sh:118-122`、`configure.ac:1408-1425`（缺 libGL 仅 WARNING，会静默失效） | ①GLX-over-EGL 桥（winex11 opengl.c 后端替换为 virpipe EGL，蓝图 = 现有 opengl.c 的结构；成败级无理论风险，全是工作量）。present 通路被 shm 约束唯一决定：wine 侧摸不到 X 窗口像素 ⇒ EGL surfaceless/pbuffer 离屏渲染 → glReadPixels → `XShmPutImage` 回 X 窗口，代价与现 winewayland EGL readback 同级；spike 裁决判据 = virpipe EGL surfaceless/pbuffer 可用性 + 常见分辨率 readback 带宽实测 ②DXVK d3d9 打包启用收窄 GL 面 ③DirectDraw/OpenGL 程序由①覆盖。**M0/M1 spike 裁决最小原型** |
| R-ZC | wlroots 只认 buffer 的 fd 门（dma-buf，OHOS 无）或 data_ptr 门（`render/pixman/renderer.c:251-255`、`render/gles2/texture.c:419-422`），不认识 BufferQueue | 同左 | ①data_ptr 门零补丁：自定义 wlr_buffer 包 OH_NativeBuffer。获取链三跳（API 实证）：`OH_NativeImage_AcquireNativeWindowBuffer`（SDK `native_image.h:300`，`AcquireLatestNativeWindowBuffer` `:485`，since 22 ≤ 本档 23）→ `OH_NativeBuffer_FromNativeWindowBuffer`（`native_buffer.h:286`）→ `OH_NativeBuffer_Map`（`:233`）。**口径：API 存在已证、组合未验证**——现行合成器零 `OH_NativeBuffer` 命中（全程纹理模式 `UpdateSurfaceImage` `native_image.h:157`），纹理/缓冲两种消费模式能否共存同一 OH_NativeImage 未验证，M2 探针裁决 ②GL 门小补丁：纹理模式导入分支。传输层已被现行 ZC 生产验证 |
| R2 | wlroots shm 分配器走 shm_open→/dev/shm（`util/shm.c:30`），沙箱可能没有 | 同左 | **已探针解除（2026-09-27，见 B 类 P1/P2）**：shm_open 与 memfd_create 在沙箱均可用，默认分配器直接工作；自定义 `wlr_allocator` 降级为备胎 |
| R-VER | wlroots 0.20.2 与 0.21 均要求 wayland-server ≥1.24（`wlroots meson.build:88-90`）；项目现有 1.22 | 同左 | **升 thirdparty/wayland ≥1.24 为必做项**（关键路径） |
| R-xkb | xkbcomp 是 Xwayland 硬运行时依赖，缺 = 键盘死 | `xkb/ddxLoad.c:105-212` → `xwayland-input.c:372-374` BadValue | xkbcomp 二进制 + XKB 数据树（项目已带 share/X11/xkb）+ `-xkbdir` 进沙箱包 |
| R-IME | 主线 xserver 无 text-input 桥 | grep XWAYLAND_IME 零命中；mutter 树亦无 Xwayland IME 桥（text-input 均为 Wayland 客户端侧，实查） | 下游补丁源待定位；否则自写 XWM↔text-input 桥或 OHOS 应用层注入，M2 裁决 |
| R-WSI | Venus 是否暴露 VK_KHR_xcb_surface 未实测 | `winex11.drv/vulkan.c` 走 X11 WSI | M1 探针 |
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

## 6. 交叉构建清单

```
必做升级:  thirdparty/wayland 1.22 → ≥1.26（wlroots 0.20.2 要 server/client ≥1.24, xserver 主线要 client ≥1.26, 取高者一并满足）
钉版待定:  Xwayland 版本（主线 26.x vs 稳定系 24.1.x, 影响 xkb/xwayland-shell 行为面）—— M0 首项裁决
新增依赖:  libdrm ≥2.4.129（wlroots 无条件依赖, 关不掉——像素格式头; 另 pixman ≥0.43.0 有版本门槛）
X client:  xorgproto → xcb-proto → libxcb(shm/randr/xfixes/composite/res 分模块) → libX11 → libXext
           可选扩展: Xrender/Xrandr/Xfixes/Xcursor/Xi/Xinerama/Xcomposite/Xxf86vm（soname 逐个降级）
Xwayland:  pixman, libxau, libxdmcp, xtrans, libxfont2, libxshmfence, libxcvt, xkbcomp + XKB 数据
wlroots:   pixman(共用), xkbcommon ≥1.8, wayland-protocols ≥1.47, libdrm ≥2.4.129
```

## 7. 里程碑与回退线

| 阶段 | 内容 | 退出条件 |
|---|---|---|
| M0 | 设备探针（本文 B 类）+ wlroots OHOS 交叉编译 + headless/pixman 出图进 NativeWindow + Xwayland 跑 xterm | 跑不通 → 回退「现有合成器 + Xwayland」（winex11 收益保留；rootless 回退需先给旧合成器补 xwayland-shell-v1——现零实现，协议面很小；旧合成器 xdg_wm_base 已具备 `xdg_shell.cpp:428-433`，rootful 可跑但 present 重锚不成立，仅保窗口不保 ZC） |
| M1 | winex11 交叉接入，窗口语义验收；GLX-over-EGL 桥最小原型裁决；R-WSI 探针 | 记事本类输入/窗口正确 |
| M2 | Venus/ZC 重锚（§4.2/§5 R-ZC）、IME、双窗口形态 | dxvk 套件出图正常 |
| M3 | 旧合成器退役 | core / wine-vulkan / dxvk 套件不回退 + 长尾样本验收 |

## 8. 验收标准

1. 触发重开的那些长尾样本（结构性不兼容类）在 X 路线可启动且行为正确；样本清单与「行为正确」判据在 M1 落定为具体程序列表
2. 现有 core / wine-vulkan / dxvk 自动化套件不回退
3. 档位系统（graphics-matrix 四档）在新链路语义不变
4. 出图/合成性能：M2 起以 dxvk、core 套件运行数据与旧链路对照，作为**观察项**（不设硬线；要设硬线则在 M1 钉基准程序与分辨率）

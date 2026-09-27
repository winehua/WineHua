# 显示路线重构设计：winex11 + Xwayland + wlroots → 鸿蒙窗口

> 状态：设计提案（待实现计划）
> 日期：2026-09-27
> 取代关系：本文显式取代 [0001-self-built-compositor.md](../../decisions/0001-self-built-compositor.md) 的「不替换合成器」结论——该结论的前提是 winewayland 驱动路线；其自设的重开条件（[win32-window-model-x11-wayland.md](../../architecture/win32-window-model-x11-wayland.md) §复核点：「长尾程序出现窗口树/override-redirect 类结构性不兼容的聚集模式」）已被实际触发（2026-09-27 用户实证：长尾程序结构性不兼容 + 窗口语义长尾两类痛点聚集）。
> 证据纪律：本文所有论断要么带 file:line 源码依据，要么显式标注为「M0 设备探针」。考证过程中三个初始论断被源码推翻，已按实况修正（见 §4.3）。

## 1. 背景与动机

现窗口栈三层全自养：魔改 winewayland.drv + 自研合成器（native ~22,900 行 + ArkTS service 4,205 行）+ 私有协议 `winehua_toplevel`。两类实际痛点：

1. **长尾程序结构性不兼容**（重开条件触发项）
2. **窗口语义长尾**：Z 序/最小化还原/popup/模态每补一处都要动 驱动+私有协议+合成器 三处

## 2. 目标架构

```
Wine 程序 → winex11.drv（上游） → Xwayland（上游, shm-only） → wlroots（上游, 钉 tag）
              → OHOS 适配层（唯一自养, 薄壳） → 鸿蒙窗口
```

- **出图**：wlroots headless output（pixman 软渲染起步）→ 适配层 → NativeWindow/XComponent。PC 模式 = 每 toplevel 一个 output 配一个鸿蒙系统窗；Pad 模式 = 单 output 虚拟桌面
- **输入**：OHOS 事件 → `wlr_seat_notify_*` 纯 API 注入（`wlr_seat.h:461-679`，seat 创建不依赖 backend，`wlr_seat.c:282`）
- **IME**：鸿蒙输入法 ↔ text-input-v3（wlroots 内置 `wlr_text_input_v3.c`）↔ Xwayland（需移植下游 IME 桥，见 §5 R3）
- **图形加速链不动**：VirGL/Venus/DXVK/vkd3d 的 vtest 链原样保留，仅 present 落点重接（§4.2）

## 3. 选型依据（全部源码实证）

| 选型 | 判定 | 关键证据 |
|---|---|---|
| **wlroots 钉上游稳定 tag** | ✅ | 最小集合 `-Dauto_features=disabled -Dxwayland=enabled`；GBM/udev/libinput/session 全可关（`wlroots meson.build:96-134`、`backend/meson.build:11-29`）；全树无 udev/systemd/logind 硬引用。**gamescope 钉 0.20.2 零补丁**（`.gitmodules` 直指 freedesktop，无 Valve fork）；headless 可动态多 output（`headless.h:25`）；自定义 `wlr_buffer` 公开 API（`wlr_buffer.h` impl 五函数指针）；text-input-v3 完整 |
| **Xwayland 纯 shm** | ✅ 一等公民 | `-Dglamor=false` 编译 + `-shm` 运行时（`xwayland.c:109,214`），glamor 失败自动回退（`xwayland-screen.c:1102-1107`）；无 glamor 内容走 memfd/tmpfile→wl_shm（`xwayland-shm.c:126-305`）；rootless 对合成器最低要求 wl_compositor+wl_shm+wl_seat（`xwayland-screen.c:520-601`） |
| **winex11.drv 上游原版** | ✅ 极简且零包袱 | 硬依赖仅 libX11+libXext（`configure.ac:1278-1280`），其余 X 扩展可选 soname 探测逐个降级；驱动源码零 Linux 桌面设施（grep /proc、/tmp、udev、dbus、abstract 全空）；**上游零修改**（git log 全上游作者）；与 winewayland 可同编（各驱动独立 enable 变量，`configure.ac:3383-3384`） |
| ~~mutter~~ | 砍 | 硬依赖 /dev/input、DRM、logind，OHOS 全拿不到；PC 系统窗口承载做不到 |
| ~~weston~~ | 降为不推荐 | libinput/libdrm/udev/cairo 顶层无条件硬依赖无开关（`weston/meson.build:161-181`）= 补丁发行版；无 text-input-v3 |
| ~~私有协议 winehua_toplevel~~ | 整体拆除 | 12 行单接口，Wine 侧引用全收敛在 winewayland.drv（modal.c 专为此建）；X11 模态走 WM_TRANSIENT_FOR+_NET_WM_STATE_MODAL；PC 模式上报改用 `wlr_xwayland_surface` 的 `modal` 字段 + request_* 信号（`xwayland.h:181,197-203`） |
| 同形态先例 | Steam Deck gamescope | Valve 官方（ValveSoftware/gamescope），wlroots 内核，游戏链路 = winex11→Xwayland→gamescope→屏幕，与本方案只差最后一跳（DRM ↔ NativeWindow） |

## 4. 现行机制实证（迁移的对接面）

### 4.1 ZC 传输真相（考证修正后）

virglrenderer 跑在**独立 NCP 子进程**（`graphics_broker.cpp:1322` libvirgl_child.so；`:196` dlopen 备选）。ZC 跨进程传输 = **OHOS BufferQueue**：合成器侧 `OH_NativeImage_AcquireNativeWindow` 得生产者窗口（`egl_renderer.cpp:225-228`）→ 交 virgl_child 渲染 → `OH_NativeImage_UpdateSurfaceImage` 回收为 GL 纹理（`native_image.h:114,157`）。握手文件 `winehua_zc_surface_*`（`graphics_broker.cpp:81-82`）只做就绪协商。

### 4.2 Vulkan present 锚定（C7-C10）

WSI 整体私有化：guest 永远拿不到真 VkSurfaceKHR，窗口身份 = 高位 tag `0x574853` + wl_surface id（`contracts.md:344-350`），venus 经 `VCMD_WINEHUA_VK_PRESENT` 把 SURFACE_ID 送 virglrenderer（`contracts.md:473`），宿主回调回主仓库。**M2 重锚 = 把 id 来源从 winewayland 的 wl_surface id 换成 X window id（经 `wlr_xwayland_surface` 双字段映射，`xwayland.h` window_id+surface），链路其余不动。**

### 4.3 考证中被源码推翻的三个初始论断（留档防复发）

1. ~~"gamescope 用 Valve 补丁版 wlroots"~~ → 实为上游 0.20.2 纯净 tag 零补丁
2. ~~"virglrenderer 与合成器同进程、EGLImage 句柄直通"~~ → 实为独立 NCP 进程 + BufferQueue
3. ~~"wine 用 memfd/tmpfile 建 wl_shm"~~ → 实为 wineserver section 转 fd（`wayland_surface.c:880-908` `wine_server_handle_to_fd` → `wl_shm_create_pool`）；沙箱内跨进程 fd 共享已被生产验证

### 4.4 其他对接事实

- GL 现状：winewayland GL = EGL window surface（`opengl.c:122-123`）+ pbuffer 模拟（`:141,168-172`）+ readback（`opengl_readback.c`）
- winex11 GLX 面：`winex11.drv/opengl.c` 内部 42 个 glX/FBConfig 实现函数 = GLX-over-EGL 桥的覆盖清单
- DXVK d3d9：**编了没打包**（`build_dxvk.sh:61` 只装 d3d11/dxgi；`assemble.sh:325-334`）——启用是打包+验证工作
- 驱动选择单点：`win32u/driver.c:1015-1035` OHOS bypass 强制直载 winewayland——切换的唯一 Wine 侧硬改动
- 隐藏依赖：wayland-server 头/.pc 目前靠 `BUILD_GUEST_GFX=1` 才装（`build_ohos_guest_gfx.sh:589-601,654-666`）——先固化进 build_wayland.sh

## 5. 风险清单

### A 类：已源码实证 + 缓解路径明确

| ID | 风险 | 证据 | 缓解 |
|---|---|---|---|
| R1 | GLX 管线断点：D3D8/9/OpenGL 现走 winewayland EGL readback；winex11 需 GLX，shm Xwayland 无 GLX 扩展 | `build_wine.sh:118-122`、`configure.ac:1408-1425`（缺 libGL 仅 WARNING，会静默失效） | ①GLX-over-EGL 桥（winex11 opengl.c 后端替换为 virpipe EGL，蓝图 = 现有 opengl.c 的结构；成败级无理论风险，全是工作量）②DXVK d3d9 打包启用收窄 GL 面 ③DirectDraw/OpenGL 程序由①覆盖。**M0/M1 spike 裁决最小原型** |
| R-ZC | wlroots 只认 buffer 的 fd 门（dma-buf，OHOS 无）或 data_ptr 门（`render/pixman/renderer.c:251-255`、`render/gles2/texture.c:442-449`），不认识 BufferQueue | 同左 | ①data_ptr 门零补丁：自定义 wlr_buffer 包 OH_NativeBuffer（`OH_NativeBuffer_Map` CPU 映射，SDK `native_buffer.h:233`，已验证存在）②GL 门小补丁：`OH_NativeImage_UpdateSurfaceImage`（`native_image.h:157`）导入分支。传输层已被现行 ZC 生产验证 |
| R2 | wlroots shm 分配器走 shm_open→/dev/shm（`util/shm.c:30`），沙箱可能没有 | 同左 | 自定义 `wlr_allocator`（公开 API，两函数指针）；自身 swapchain 可纯 malloc（无需 fd）。**真正待探针的只剩 Xwayland 建 shm 的 memfd/XDG_RUNTIME_DIR 路径**（见 B 类） |
| R-VER | wlroots 0.20.2 与 0.21 均要求 wayland-server ≥1.24（`wlroots meson.build:88-90`）；项目现有 1.22 | 同左 | **升 thirdparty/wayland ≥1.24 为必做项**（关键路径） |
| R-xkb | xkbcomp 是 Xwayland 硬运行时依赖，缺 = 键盘死 | `xkb/ddxLoad.c:105-212` → `xwayland-input.c:372-374` BadValue | xkbcomp 二进制 + XKB 数据树（项目已带 share/X11/xkb）+ `-xkbdir` 进沙箱包 |
| R-IME | 主线 xserver 无 text-input 桥 | grep XWAYLAND_IME 零命中 | 移植下游 IME 补丁或 OHOS 应用层注入（M2） |
| R-WSI | Venus 是否暴露 VK_KHR_xcb_surface 未实测 | `winex11.drv/vulkan.c` 走 X11 WSI | M1 探针 |
| R-CPL | win32u present_rect 补丁按 wayland 握手写 | `win32u/window.c:2313-2335` | 切换时回归 war3 全屏场景 |
| R-REPRO | wayland-server 头/.pc 隐藏依赖 | `build_ohos_guest_gfx.sh:589-601` | 先固化进 build_wayland.sh，rm sysroot-ext 重建演练 |
| R-ARK | ArkTS modal 事件源随私有协议消失 | `toplevel_event_bus` | 用 wlr_xwayland_surface 事件重建（§3 私有协议行） |

### B 类：设备探针（本机源码不可验，真机裁决）

| ID | 探针 | 判定标准 |
|---|---|---|
| P1 | shm_open 在应用沙箱可用性（/dev/shm 可写？） | shm_open 返回有效 fd 或明确 errno |
| P2 | memfd_create 可用性（Xwayland/wlroots shm 的首选路径） | syscall 返回 fd |
| P3 | XDG_RUNTIME_DIR tmpfile 路径（xwayland-shm.c:135-167 回退 + xkm 输出） | mkostemp 成功 |
| P4 | unix socket：路径绑定 + abstract 绑定（X11 传输两形态） | bind 成功或 errno |
| P5 | /tmp 可写性（X lock/socket 硬编码路径） | access W_OK + 实建文件 |

## 6. 交叉构建清单

```
必做升级:  thirdparty/wayland 1.22 → ≥1.24（wlroots 0.20.2/0.21 同门槛; xserver 主线要 client ≥1.26, 钉稳定系列时确认）
X client:  xorgproto → xcb-proto → libxcb(shm/randr/xfixes/composite/res 分模块) → libX11 → libXext
           可选扩展: Xrender/Xrandr/Xfixes/Xcursor/Xi/Xinerama/Xcomposite/Xxf86vm（soname 逐个降级）
Xwayland:  pixman, libxau, libxdmcp, xtrans, libxfont2, libxshmfence, libxcvt, xkbcomp + XKB 数据
wlroots:   pixman(共用), xkbcommon ≥1.8, wayland-protocols ≥1.47
```

## 7. 里程碑与回退线

| 阶段 | 内容 | 退出条件 |
|---|---|---|
| M0 | 设备探针（本文 B 类）+ wlroots OHOS 交叉编译 + headless/pixman 出图进 NativeWindow + Xwayland 跑 xterm | 跑不通 → 回退「现有合成器 + Xwayland」（winex11 收益保留） |
| M1 | winex11 交叉接入，窗口语义验收；GLX-over-EGL 桥最小原型裁决；R-WSI 探针 | 记事本类输入/窗口正确 |
| M2 | Venus/ZC 重锚（§4.2/§5 R-ZC）、IME、双窗口形态 | dxvk 套件出图正常 |
| M3 | 旧合成器退役 | core / wine-vulkan / dxvk 套件不回退 + 长尾样本验收 |

## 8. 验收标准

1. 触发重开的那些长尾样本（结构性不兼容类）在 X 路线可启动且行为正确
2. 现有 core / wine-vulkan / dxvk 自动化套件不回退
3. 档位系统（graphics-matrix 四档）在新链路语义不变

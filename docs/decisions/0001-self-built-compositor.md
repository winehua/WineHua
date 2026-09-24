# Compositor 架构决策：自研 Wayland Compositor vs libweston/wlroots

> 决策日期：2026-08-26
> 决策结论：**不替换**。继续维护自研 compositor，以"协议一致性测试 + 定点补齐协议缺口"的方式解决质量担忧。
> 最后核实：2026-09-24（规模数字与协议清单按当前代码重算）
> 重议条件：见文末「何时应当重新考虑替换」一节。

---

## 一、问题背景

本项目（WineHua）在 HarmonyOS 上运行 Wine，窗口系统的核心是一个自研的 Wayland compositor（位于 `entry/src/main/cpp/`）。随着支持的设备和场景增多，出现了一个架构层面的疑虑：

> 自研 compositor 的质量和协议完整性是否有保障？是否应当替换为更成熟的 compositor 方案（libweston + 自定义 backend，或 wlroots），再由其对接鸿蒙窗口系统？

`.temp/` 目录下已有 weston 15.0.91 与 wlroots 0.21.0-dev 的完整源码树可供参考。本文档记录对该问题的完整调查事实与决策推理。

### 当前 compositor 的两种运行形态

- **PC 窗口模式**：每个 Wayland toplevel 映射为一个独立的鸿蒙系统窗口（multiton `WineWindowAbility` + XComponent），窗口的层级、装饰、任务切换由鸿蒙系统窗口管理器负责。
- **Pad 桌面模式**：全屏单一 XComponent，compositor 在内部完成所有 Wine 窗口的 CPU 合成，呈现一个完整桌面。

---

## 二、现状盘点：自研 compositor 的真实规模与质量

### 2.1 代码规模

- compositor 全栈 native 代码约 **22,900 行**，分布在三个目录：`compositor/` 9,458 行（帧合成 2,861 / 窗口状态 2,547 / 输入裁决 1,507 / 协议层文件 2,543）、`input/` 6,800 行（事件注入、相对指针、输入法，含生成的键盘映射数据 2,854 行）、`graphics/` 6,601 行（渲染与图形后端）。不含约 24k 行的生成协议头。
- ArkTS 侧的窗口管理/输入代码同样是数千行量级——仅 `service/` 一个目录就有 4,205 行（鸿蒙窗口生命周期、popup 子窗口、ARGB 子窗口、悬浮条、触控板手势等）。

### 2.2 协议覆盖

注册的 wayland global 共 11 个，**其中 10 个是标准协议或上游 staging 协议，1 个是自定义协议**：

| 协议 | 版本 | 位置 |
|---|---|---|
| `wl_compositor` / `wl_shm` | v4 | `compositor/wl_core.cpp:1011`，`compositor/wayland_server.cpp:78` |
| `wl_subcompositor` | v1 | `compositor/wl_core.cpp:1012` |
| `wp_viewporter` | v1 | `compositor/wl_core.cpp:1013` |
| `wl_output`（虚拟输出） | v3 | `compositor/wl_core.cpp:1014` |
| `wl_seat`（pointer+keyboard） | v5 | `input/seat.cpp:86` |
| `xdg_wm_base` | v3 | `compositor/xdg_shell.cpp:448` |
| `zwp_pointer_constraints_v1` / `wp_pointer_warp_v1` / `zwp_relative_pointer_manager_v1` | v1 | `input/pointer_extras.cpp:36-40` |
| `zwp_text_input_manager_v3` | v1 | `input/text_input.cpp:40` |
| **`winehua_toplevel`**（自定义） | v1 | `compositor/winehua_toplevel.cpp:148` |

其中 `wp_pointer_warp_v1` 是 wayland-protocols staging 协议（2024 年 Neal Gompa 提交），上游 Wine 亦使用同一协议，并非本项目私有发明。

**自定义协议只有 `winehua_toplevel` 一个**（2026-09 支持模态对话框时引入）。xdg-shell 表达不了 Win32 的模态属主关系（一个被属主窗口禁用的弹窗），Wine 侧只能通过这个接口把关系告诉合成器，合成器据此让对话框压在属主之上、并拦截本该送给属主的输入。定义在 `entry/src/main/cpp/protocols/winehua-toplevel.xml`，只有一个请求 `set_modal`。

这说明自研协议面是可控的：自定义协议只有一个，且边界清晰——它只承载 xdg-shell 表达不了的 Windows 语义，不是把通用窗口管理搬进私有协议。

### 2.3 已知的协议缺口（如实记录）

- `xdg_positioner` 为**空实现**（`compositor/xdg_shell.cpp:411`），popup 菜单位置由自研逻辑按 viewporter 语义手搓。
- `wl_surface.set_buffer_transform / set_buffer_scale / damage_buffer / offset` 为空函数（`compositor/wayland_server.h:182-184`）。
- `opaque_region` 被忽略；`set_input_region` 仅记录空/非空布尔（`compositor/wl_core.cpp:449`）。
- 无 `wl_data_device`（剪贴板）、`zwp_linux_dmabuf`、`xdg_output`、`presentation_time`、`idle_inhibit`、`cursor-shape` 等。

### 2.4 与 Wine 的语义层耦合（非协议层）

与 thirdparty/wine（Wine 11.10 fork，118 个提交，其中 26 个触碰 `winewayland.drv`）之间靠**约定**而非私有协议配合：

- app_id 后缀约定（如 `"explorer.exe.taskbar"`，`compositor/frame/compositor_constants.h:44`），用于区分 explorer 的桌面/任务栏窗口；
- 桌面模式下 `window_geometry` 的 x,y 复用为虚拟桌面坐标（wine 提交 `af64af414a7` + `compositor/wl_core.cpp:524-558`）；
- `WINEHUA_DESKTOP_MODE` 环境变量（wine `dlls/winewayland.drv/wayland_surface.c:607`）；
- max_size → maximize 启发式（`compositor/xdg_shell.cpp:107-144`）。

### 2.5 游戏兼容 workaround（约 900~1000 行 native）

这些是逐款游戏适配出来的行为修正，注释中点名实测的游戏包括 PAL2（仙剑 2）、红警 2、war3 等：

- `pointer_extras.cpp` 全文件 468 行：dinput 类老游戏依赖的 pointer constraints / warp / relative pointer 三协议栈，含 `OH_WindowManager_LockCursor/UnlockCursor` 冻结系统光标（IPC 挪独立线程防阻塞 wl 事件循环）。
- `input_manager.cpp`：rawDelta 相对增量通道与 ±512 钳制、点击脉冲拉伸（短于 100ms 的点击延迟发 release）、相对模式下点击跳过 enter 重定位、ClampToContent 防幽灵增量。
- `toplevel_manager.h` / `desktop_compositor.cpp`：fsPriority 全屏取号仲裁、ShouldSkipFullscreenCascade 容错（显示模式切换时 winewayland 批量标记 fullscreen 的连带问题）。
- `wl_core.cpp`：最小化窗口 -32000 坐标补偿、全屏尺寸漂移重发 configure。
- ArkTS 侧另有桌面触控板手势状态机（约 300 行）。

### 2.6 与鸿蒙的耦合方式

- **窗口生命周期全部在 ArkTS**：native 只发 `created / destroyed / popup_*` 事件，ArkTS 负责 `startAbility` 拉起 multiton 窗口、创建子窗口、调用 `setWindowMask` / `setWindowBackgroundColor` 等。这个边界目前划得比较干净。
- native 侧鸿蒙 API 依赖集中在：`graphics/egl_renderer.cpp`（OH_NativeImage 零拷贝消费端、NativeVSync 帧调度）、`graphics/graphics_broker.cpp`（OH_IPC 跨进程传 OHNativeWindow 给 guest 侧 venus/virgl presenter）、`input/pointer_extras.cpp`（LockCursor）、`proc/broker.cpp`（native_child_process 拉起 wine/virgl 子进程）。
- zero-copy 链路完全绕开 wayland 协议，经 side-channel（surfaceKey = clientPid+surfaceId）直连，compositor 仅做层序簿记。

---

## 三、替换候选盘点：weston / wlroots 的落地事实

### 3.1 依赖缺口

`.temp/weston`（15.0.91）与 `.temp/wlroots`（0.21.0-dev）的硬依赖，对照 thirdparty/ 现状：

| 依赖 | weston 15 要求 | wlroots 0.21 要求 | thirdparty 现状 |
|---|---|---|---|
| wayland-server | ≥ 1.22.0 | ≥ 1.24.0 | **1.22.0**（weston 踩线满足，wlroots 不满足） |
| libdrm | ≥ 2.4.108 | ≥ 2.4.129 | **2.4.120**（wlroots 不满足） |
| libxkbcommon | ≥ 0.5.0 | ≥ 1.8.0 | **1.7.0**（wlroots 不满足） |
| pixman | ≥ 0.25.2 | ≥ 0.46.0 | **无** |
| libinput + libevdev | 必需 | libinput backend 需要 | **无** |
| libudev | **无条件依赖** | session/drm/libinput 路径必需 | **无（鸿蒙环境没有 udev，需打桩 + patch）** |
| cairo + libpng | 无条件（shared/decorations 用） | — | **无** |
| libdisplay-info | 必需（有 wrap fallback） | DRM backend 需要 | **无** |

也就是说：weston 需要新移植 5~6 个库并打桩 libudev；wlroots 除新库外还需要升级三个已有 submodule（wayland、libdrm、xkbcommon），升级本身就有连带风险（现有 compositor 链接的正是 1.22.0 的内部头，wine/box64 桥也在用同一套 wayland 库）。在鸿蒙 NDK 的 musl/隔离文件系统环境下交叉调试这一整棵 meson 依赖树，仅构建打通就是数周级的工作。

### 3.2 架构级不适配（决定性因素）

**weston 和 wlroots 都是 output-based 渲染模型**：compositor 把客户端窗口合成到自己的桌面空间，以 output 为单位整体呈现。weston 的 wayland backend 是"每个 output 映射为宿主合成器的一个窗口"，**不存在"把每个客户端 toplevel 映射为宿主一个独立窗口"的 backend 形态**。

而本项目的 **PC 窗口模式的本质恰恰是 surface→鸿蒙窗口**：每个 xdg_toplevel 拉起一个独立的鸿蒙系统窗口，层级与装饰交给鸿蒙系统窗口管理器。这一形态在 libweston 里没有对应物，只有两条路，两条都不通：

1. **一个 output = 整块屏幕**：所有 Wine 窗口活在 weston 自己的 desktop-shell 里，整体渲染进一个鸿蒙窗口——这等于砍掉 PC 多窗口模式，退化成桌面模式。
2. **每个 toplevel 一个 headless output**：再给每个 output 的内容倒到对应鸿蒙窗口——此时 desktop-shell 的窗口管理（它在 output 内部排布窗口）、damage 跟踪（按 output 累积）、seat 焦点路由（跨 output 的指针移动语义）全部与"output=窗口"模型冲突，实质是在 fork libweston 核心并与其主干假设对抗。买来的成熟度在这种用法下所剩无几。

wlroots 比 libweston 组合度高，理论上可以绕开 wlr_scene 的 output 主干、逐 toplevel 渲染到独立目标（`.temp/cage` 是 kiosk 单窗口参考），但同样要自写全部鸿蒙胶水，且 wlroots 的依赖版本缺口更大（见 3.1）。

### 3.3 wine 游戏兼容成本会原样重演

这类缺陷——PAL2 的 dinput 光标漂移与点击失效、红警 2 的纵向偏移、war3 的全屏最小化/黑屏、菜单裁剪、ARGB 异形窗口、zero-copy 层序——**几乎全部位于"Wine 客户端行为 × 鸿蒙窗口系统"的语义层，而不是 wayland 协议状态机层**。weston/wlroots 不提供这些问题的任何答案，替换成它们之后，这约 1000 行 workaround 需要原样重写，并且要用同样的真机游戏矩阵重新回归。

---

## 四、决策推理

### 4.1 换 compositor 能白拿的东西（占小头，且可单独补）

- 成熟的 xdg_positioner 约束规则（flip/slide/resize）、popup grab 语义；
- subsurface 边角 case 的正确处理；
- 成熟的 damage 跟踪与渲染器（pixman/GL）；
- 剪贴板（wl_data_device）、xdg-output、presentation-time 等我们尚未实现的协议。

这些收益真实存在，但每一项都有明确边界，可以逐个增量补齐；补的时候 `.temp/weston` 就是现成的语义参考书（viewport source 修复即是先例）。

### 4.2 换了也躲不掉的东西（占大头）

- 全部游戏兼容 workaround（dinput 三协议栈、点击脉冲拉伸、全屏仲裁、最小化坐标补偿）；
- 全部鸿蒙窗口系统集成（multiton Ability、子窗口、setWindowMask、悬浮条）；
- 全部 zero-copy 链路（OH_NativeImage + OH_IPC side-channel）；
- 与定制 wine 的全部语义约定（app_id、geometry 复用、WINEHUA_DESKTOP_MODE）。

即：当前系统里真正难、真正容易出问题的部分，恰恰是任何现成 compositor 都不覆盖的部分。

### 4.3 成本/收益对照

- 替换路径：依赖移植与打桩（数周）+ 自定义 OHOS backend 与 shell 改造（数千行，且要 patch libweston 核心）+ 全部 workaround 与胶水重写 + 全量游戏回归。**总量级：数月；期间双栈并行，回归风险高。**
- 维护面：weston 本体 15 万行以上，我们将独自维护一个打过核心补丁的 fork；当前自研栈 1 万行、注释完备、团队熟悉。就本项目实际人力而言，自研栈的 bus factor 反而更好。
- 收益：仅协议状态机成熟度一项，而该收益可用 §4.4 的低成本方案获得大部分。

### 4.4 替代方案（采纳）

以替换成本约 1/10 的代价，解决"质量和完整性不放心"的核心诉求：

1. **引入协议一致性测试**。首选接入 wlcs（Mir 出品的 Wayland Compositor Conformance Suite，compositor 无关）；退而求其次的方案是用 wayland-debug + 一组自制 test client，把 xdg-shell popup/subsurface/seat 的行为固化成回归测试并纳入 CI。这给出"完整性"的**可验证证据**，比换引擎带来的心理安全感更实在。
2. **对照 weston 源码定点补缺口**。优先级：`xdg_positioner` 约束规则与 popup grab（菜单类问题的高发区）→ `set_input_region` 真矩形 → `wl_data_device` 剪贴板。`.temp/weston` 保留为语义参考书，一次一个缺口，小步提交。
3. **保持协议状态机与鸿蒙胶水的边界**。当前 ArkTS 管窗口生命周期、native 管协议事件的双层结构是好的，后续重构继续沿这个方向收敛，workaround 注释持续沉淀。

---

## 五、何时应当重新考虑替换

以下任一信号出现时，本文档的结论应当重议：

1. **产品放弃 PC 多窗口模式**（全退到单 output 桌面模式）——output-based 模型即刻适配，wlroots/cage 路线（`.temp/cage` 有现成参考）变为合理选项；
2. **协议需求失控膨胀**——linux-dmabuf、clipboard、fractional-scale、tablet 等需求集中出现，自研协议面的边际成本超过维护一个 weston fork 的成本；
3. **团队规模扩大**到足以长期维护一个带核心补丁的 weston/wlroots fork；
4. 上游 Wine wayland 驱动发生重大演进，与自研 compositor 的语义约定出现不可调和的冲突。

---

## 六、关键事实索引（备查）

- 自研 compositor 规模：native ≈22,900 行（`compositor/` + `input/` + `graphics/` 三个目录）+ ArkTS 侧 ets 共约 7,900 行（其中窗口与输入相关的 service/ 层 4,205 行）；协议 global 11 个 = 10 个标准协议 + 1 个自定义（`winehua_toplevel`）
- weston 版本：15.0.91（`.temp/weston/meson.build:3`）；wlroots 版本：0.21.0-dev（`.temp/wlroots/meson.build:4`）
- thirdparty 现有：wayland 1.22.0、wayland-protocols 1.39、libxkbcommon 1.7.0、libdrm 2.4.120；缺 pixman/libinput/libevdev/libudev/cairo/libdisplay-info
- wayland-server 链接路径：`entry/libs/${OHOS_ARCH}/libwayland-server.so.0`（`entry/src/main/cpp/CMakeLists.txt:117`），由 `scripts/build_wayland.sh` 从 thirdparty/wayland 交叉编译
- wine fork：Wine 11.10 + 118 提交，其中 26 个触碰 `dlls/winewayland.drv/`

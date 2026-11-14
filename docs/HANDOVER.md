# WineHua / HGS 交接文档

当前工作分支为 `feature/using-hgs`。最新的上游合并、差异取舍和验证见 [上游同步与 PR 审查](architecture/hgs-upstream-sync.md)。下文保留 11 月 6 日迁移基线。

> 最后核实：2026-11-06
>
> 适用仓库与分支：
>
> - WineHua：`/workspace/WineHua-root/WineHua`，分支 `feature/hgs-har-scaffold`
> - BetterSH：`/workspace/BetterSH`，分支 `feature/hgs-migration`
> - HGS 子模块：两个宿主里都挂在 `hgs/`，工作区已同步到同一提交 `bad402d`（WineHua 外层指针尚未提交，见 §5.1）
>
> 本文回答四个问题：WineHua 是什么、HGS 分支做了什么、分支之后上游新增了什么、HGS 如何同时服务 WineHua 与 BetterSH。架构细节以 `docs/architecture/` 和 hgs 内的 `docs/architecture/hgs-shared-runtime-phase0.md` 为准；本文负责给出全局地图、当前状态和接手动作。

---

## 0. 三条最重要的结论

1. **WineHua 主线已经切到 x11 路线。**
   分支创建后，上游合入 313 个提交，新增 `entry/src/main/cpp/display/` 第二条显示栈（wlroots + Xwayland），并把 `DisplayRouteService` 的缺省路线设为 `x11`。
   HGS 是 **wayland 路线的合成器**。合并上游后必须显式处理"x11 走 `display/`、wayland 走 HGS"的分流，不能假设 HGS 仍是 WineHua 的默认显示栈。

2. **HGS 已完成"把合成器从 entry 抽出来"的主体工作。**
   本分支删除了 entry 下 109 个合成器/输入/协议文件（约 4.5 万行），迁入 `hgs` 子模块；对外形成三层：HAR 源码依赖 + `libhgs.so` NAPI + `hgs/api/*.h` C++ 契约。
   WineHua 用"原生宿主"接入（保留自己的窗口管理），BetterSH 用 HGS 自带的默认 ArkTS host。

3. **直接 merge 上游会产生 42 个冲突，其中 28 个是"分支里删了、上游又改了"的合成器文件。**
   正确解法不是把 entry 副本恢复，而是把上游在这些文件上的改动**移植进 hgs 对应实现**。合并前先统一并推送 hgs 子模块指针（两宿主工作区已同步到 `bad402d`，但 `origin/main` 仍在 `bad4d5f`，WineHua 外层指针也未提交）。

---

## 1. WineHua 整体

### 1.1 项目定位

WineHua 在 HarmonyOS / OpenHarmony 设备上运行 Windows x86_64 程序：

- **Guest**：Wine（x86_64，musl）由 box64 动态翻译执行；arm64 下 box64 编译为 `box64.so`，在 NCP 子进程内 dlopen，通过 `box64_hmos_main()` 同进程模拟执行 Wine ELF。
- **Host**：HAP 主进程内嵌自研 Wayland compositor，把窗口渲染到 XComponent 的 `OH_NativeWindow`。
- **图形**：guest Mesa（virpipe / venus）经 vtest 协议转发到 host virglrenderer，再翻译成 host GL / Vulkan。
- **音频**：Wine 侧 `wineohos.drv` 经 Unix socket + 共享内存桥到 host `OH_AudioRenderer`。

产品已上架应用市场（PC / Pad）。

### 1.2 四域架构

| 域 | 运行位置 | 职责 | 主要文件 |
|---|---|---|---|
| Guest | `wine_child` NCP 子进程 | x86_64 Wine、DXVK/VKD3D、winewayland.drv、wineohos.drv | `thirdparty/wine` |
| 传输 | Unix socket / Wayland / vtest / 音频 IPC | 跨进程命令与数据 | `virgl_ipc_protocol.h`、`audio_ipc_protocol.h` |
| Host | App 主进程 + `virgl_child` | 合成器、EGL、输入、音频 broker、进程 broker | `entry/src/main/cpp`、`hgs/` |
| 系统 | HarmonyOS API | XComponent / OH_NativeWindow、OHAudio、NCP | SDK |

### 1.3 进程模型

```text
App 主进程（entry.so + libhgs.so）
├── NAPI 桥（napi_init.cpp）：事件注入、进程管理、crash 检测
├── wine_launch 线程：拉起 wine_child
├── Process Broker：Wine CreateProcess → NCP 子进程
├── Graphics Broker：VirGL/Venus 后端选择 + WINEHUA_* 环境变量
├── Audio Broker：OH_AudioRenderer 宿主侧混音
├── WaylandServer / HGS：合成器事件循环
└── XComponent → 每窗口 EglRenderer / presenter

wine_child（NCP 子进程）
├── dlopen box64.so → box64_hmos_main()
├── wineserver（NCP 子进程）
└── explorer / 用户程序（NCP 子进程）

virgl_child（NCP 子进程）
└── libvirglrenderer.so + host EGL / Vulkan
```

要点：

- Wine 内部的 `CreateProcess` 经 Process Broker 转成 `OH_Ability_StartNativeChildProcess`，支持命名多 fd 与环境变量转发。
- virglrenderer 有三种宿主形态：IPC NCP 子进程、进程内 host（phone 模式）、NCP 子进程直连；由 `graphics_broker.cpp` 管理。
- 系统 NCP 不可用时，`phone_adapter/` 用 fork 实现覆盖 `OH_Ability_StartNativeChildProcess` 符号（phone 模式）。
- 音频 Host Broker 在 App 主进程内，Wine 子进程不直接创建设备。

### 1.4 图形链路

两条链路，**DXVK / Venus 为产品主路径，OpenGL / VirGL 为显式 fallback**：

- **D3D11 主路径**：DXVK → winevulkan.dll → `win32u/vulkan.c` 私有 swapchain（`0x574853` tag）→ guest Mesa venus → vtest → host virglrenderer venus → `venus_surface_presenter` → `OH_NativeWindow`。
- **OpenGL fallback**：WGL → winewayland.drv → guest Mesa virpipe → vtest → host virglrenderer → `virgl_surface_presenter`。
- **CPU fallback**：`EglRenderer` 把 shm 帧作为纹理上屏。

三条跨仓库私有接口（`win32u` swapchain / Mesa `vn_winehua_present` / virglrenderer `vkr_winehua_present`）必须成对演进，详见 `docs/architecture/contracts.md`。

### 1.5 输入链路

WineHua 当前的主路径是 **ArkTS 事件 → NAPI → wl_seat 注入**：

- 页面/桌面层捕获触摸、鼠标、滚轮、键盘、手柄事件；
- `WineWindowManager` / `DesktopLayer` 按窗口命中与 letterbox 做坐标反变换；
- NAPI `sendPointerEvent` / `sendKeyEvent` / `sendScrollEvent` 注入合成器；
- 游戏场景另有 pointer lock、relative pointer、光标回中（`pointer_extras`）；
- IME 走 `zwp_text_input_v3`，宿主调用 `sendImeCommit` / `sendImePreedit`。

HGS N1 之后新增 **原生输入** 路径：`OH_NativeXComponent` 回调直通 C++，ArkTS 不注册输入事件；WineHua 保留 ArkTS 路径，BetterSH 默认用原生路径。

### 1.6 窗口与合成

- **PerToplevel**：每个 Wayland toplevel 对应一个 OHOS 窗口（WineHua 融合模式、BetterSH 终端窗口）。
- **DesktopRoot**：所有窗口合成到一个 root surface（WineHua PC / 虚拟桌面模式）。
- WineHua 的窗口管理层：`WineWindowManager`（窗口生命周期）、`FusionWindowManager`（融合模式）、`ModalWindowManager`（模态）、`PopupWindowManager`（xdg_popup）、`DesktopWindow` / `WineWindow` / `WinePopup` 页面。
- HGS 分支之后，合成器内部（`ToplevelManager`、`DesktopCompositor`、`FramePlanner`、`InputResolver` 等）已经整体迁入 hgs。

### 1.7 仓库结构（分支当前形态）

```text
WineHua/
├── entry/src/main/cpp/
│   ├── audio/              # AudioBroker / IPC / stream
│   ├── bridge/napi_init.cpp# wine/audio NAPI + 进程管理
│   ├── common/             # fs/process/ring_buffer/string 工具
│   ├── graphics/           # graphics_broker / virgl / venus presenter
│   ├── phone_adapter/      # NCP 不可用时的 fork 适配
│   ├── proc/               # broker / spawner / wine_child / wine_process
│   ├── protocols/          # entry 自身仍需的 Wayland/音频协议头
│   ├── seam_bridge/        # host_seams.cpp：entry ↔ libhgs 的 dlsym 接缝
│   ├── wine/               # wine_launch / wine_env / wine_exe
│   └── CMakeLists.txt
├── entry/src/main/ets/
│   ├── components/         # HgsSurface 使用方（DesktopLayer 等）
│   ├── entryability/       # Wine/Desktop/VirtualDesktop Ability
│   ├── pages/              # Index / WineWindow / WinePopup / DesktopWindow
│   ├── service/            # WineWindowManager / WineEnvService / 窗口管理器
│   └── smoke/              # SmokeRunner
├── hgs/                    # 子模块：合成器/输入/图形/窗口管理器
├── thirdparty/             # wine / mesa / wlroots / xserver / dxvk ...
├── scripts/                # 构建、打包、资源
├── host_tests/             # 纯函数与契约测试
└── docs/
```

分支已经把 `compositor/`（54 文件）、`input/`（23 文件）、`protocols/`（24 文件）、`bridge/plugin_manager`、`graphics/egl_renderer`、`common/perf_utils`、`smoke/smoke_napi` 等迁出 entry。entry 只保留 Wine 域、音频域、图形 broker/presenter、进程域和宿主接缝。

### 1.8 关键子模块

| 子模块 | 作用 |
|---|---|
| `thirdparty/wine` | Wine 源码与 OHOS 定制（winewayland / wineohos / win32u swapchain） |
| `thirdparty/box64` | x86_64 → ARM64 动态翻译器（共享库形态） |
| `thirdparty/mesa` | guest Mesa（virpipe / venus） |
| `thirdparty/virglrenderer` | host 侧 VirGL / Venus |
| `thirdparty/dxvk` / `dxvk-modern` | D3D11 → Vulkan（legacy 1.10 / modern 2.6） |
| `thirdparty/vkd3d-proton` | D3D12 → Vulkan |
| `thirdparty/wayland` / `wayland-protocols` | 协议与 libwayland |
| `thirdparty/libxkbcommon` / `xkeyboard-config` | 键位映射 |
| `thirdparty/wlroots` / `xserver` | **上游新显示栈**（分支创建后才加入；本分支尚未合并） |
| `hgs` | 本分支抽出的合成器/输入/窗口服务 |

### 1.9 构建与部署

```bash
# 初始化子模块
git submodule update --init --recursive

# 完整构建（arm64 真机）
make NATIVE_ARCH=arm64-v8a

# 仅 HAP（只改 ArkTS / C++）
make NATIVE_ARCH=arm64-v8a hap

# HGS 预编译依赖（libffi / freetype / libxml2 / wayland / xkbcommon）
bash scripts/build_hgs.sh
```

详细环境见 `docs/build/env.md`、`docs/build/guide.md`；设备部署、日志、hdc 见 `docs/cheatsheet.md`、`docs/debugging/`。

### 1.10 文档入口

- 总览：`docs/architecture/overview.md`
- 合成器：`docs/architecture/compositor.md`
- 输入：`docs/architecture/input.md`
- 图形档位：`docs/architecture/graphics-matrix.md`
- 进程模型：`docs/architecture/process-model.md`
- 音频：`docs/architecture/audio.md`
- HGS 方案：`docs/architecture/hgs-shared-runtime-phase0.md`
- HGS 导出面：`docs/architecture/hgs-exported-api.md`

---

## 2. HGS 分支：`feature/hgs-har-scaffold`

### 2.1 背景：为什么要拆 HGS

WineHua 和 BetterSH 原本各自维护一份 Wayland compositor / 输入注入 / EGL 渲染器：

- WineHua：`entry/src/main/cpp/compositor/` + `input/` + `egl_renderer`，服务 Wine 窗口；
- BetterSH：自己的 `wayland_server` / `WaymonyService` / `ClientWindow`，服务终端窗口。

两份实现同源但持续分叉。HGS（HarmonyOS Graphics UserInterface Service）的目标是：

1. 把合成器、输入、渲染、窗口管理抽成 **独立 HAR + 原生库**；
2. 用 **公共 C++ 契约 + NAPI** 隔离宿主，WineHua / BetterSH / HarmonyBox 共用；
3. 宿主只保留业务（Wine 引擎、终端、桌面 UI），不再维护 Wayland 代码。

### 2.2 分支规模

| 指标 | 数值 |
|---|---|
| 基线 | `3eecbd732`（master） |
| 分支 HEAD | `3355dab96` |
| 提交数 | 41 |
| 变更文件 | 147 |
| 增 / 删 | +3,117 / −45,090 |
| 删除的 entry 原生文件 | 109（compositor 54、input 23、protocols 24、common 3、graphics 2、bridge 2、smoke 1） |
| 新增文件 | 9（`hgs` 指针、`seam_bridge/host_seams.cpp`、4 个 host test、2 篇 HGS 文档、`scripts/build_hgs.sh`） |

> 注意：外层仓库只提交 hgs 子模块指针（每笔 1 行），真正的实现历史在 `hgs` 仓库里。

### 2.3 分层架构

```text
┌─────────────────────────────────────────────────────────────┐
│ 宿主 ArkTS（WineHua entry / BetterSH feature/bsh_main）      │
│   - HgsSurface / HgsWindowPage / HgsWindowHostService        │
│   - HgsAppWindowAbility                                     │
└───────────────────────────┬─────────────────────────────────┘
                            │ NAPI（libhgs.so）
┌───────────────────────────▼─────────────────────────────────┐
│ hgs HAR（源码依赖，随宿主重新编译）                           │
│   api/        纯 C++ 契约：types/events/host/instance/window │
│   compositor/ Wayland server、xdg_shell、toplevel、frame      │
│   input/      seat、input_manager、native_input、IME、手柄    │
│   graphics/   egl_renderer、egl_surface（XComponent 生命周期）│
│   bridge/     NAPI 装配、plugin_manager、window_napi          │
│   seam/       宿主生命周期钩子、zero-copy、session control    │
│   logger/     多 sink 日志（stderr / 沙箱文件 / hilog）       │
└───────────────────────────┬─────────────────────────────────┘
                            │ dlsym（可选接缝）
┌───────────────────────────▼─────────────────────────────────┐
│ 宿主原生层（entry.so / libentry.so）                          │
│   Wine 引擎、音频、图形 broker、进程管理、桌面策略             │
└─────────────────────────────────────────────────────────────┘
```

三条边界：

1. **`hgs/api/*.h` 零平台依赖**：只允许标准库；出现 `napi`、`hilog`、`wayland-*`、`EGL/*`、`native_window/*` 即视为设计错误。
2. **NAPI 是稳定边界**：宿主 ArkTS 只依赖 `.d.ts`；旧导出先转发、打 `[HGS-DEPRECATED]`、再按 major 删除。
3. **C++ 跨项目复用走源码**：同一份 HGS 在各宿主内重新编译，不承诺 C++ ABI 稳定。

### 2.4 阶段成果

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 公共契约（types/events/endpoint/host）、`HgsInstance` 门面、事件枚举单一来源 | ✅ |
| P1.1 | canonical NAPI 导出面 + 回调统一走 `IHgsHost` | ✅ |
| P1.2 | `EventSink` 类型化：`(id, ToplevelEventType, json)` | ✅ |
| P1.3 | `setProcessEnvironment` 生效，Start 可按配置跳过 setenv | ✅ |
| N1 | 原生输入：`OH_NativeXComponent` 回调直通 C++ | ✅ |
| W1 | 窗口管理器 core：`WindowRegistry` / `IWindowPolicy` / `HgsWindowManager` | ✅ |
| W2a | Wayland 事件接入窗口管理器 + NAPI 通道 | ✅ |
| W2b | 默认 ArkTS host：`HgsWindowHostService` / `HgsAppWindowAbility` / `HgsWindowPage` | ✅ |
| N2 | 手势 | ⏳ |
| P1.4 | 所有权 / seam / smoke 收口 | ⏳ |
| W3 | BetterSH 接入收口（删旧 compositor） | ⏳ |
| W4 | HarmonyBox 接入 | ⏳（本机无源码） |

### 2.5 迁移边界：哪些进了 HGS，哪些留在宿主

**进 HGS：**

- Wayland 协议实现：`wl_compositor`、`wl_shm`、`wl_subcompositor`、`wp_viewporter`、`wl_output`、`xdg_wm_base`、`wl_seat`、`wl_data_device_manager`（两宿主 hgs 工作区均已包含）、relative pointer、pointer constraints、text-input-v3、WineHua toplevel 协议；
- 合成：`ToplevelManager`、`DesktopCompositor`、`FramePlanner/Blitter`、`CommittedSurface`；
- 输入：`InputManager`、`InputResolver`、`InputSpaceMapper`、`Seat`、原生输入、键位映射、IME、手柄/Controller Hub；
- 渲染：`EglRenderer`、`EglSurface`、`PluginManager`（每 toplevel renderer）；
- 窗口管理 core：`HgsWindowManager`、policy/host 接口；
- 日志：`logger` + ArkTS `LOGI/LOGW/LOGE`。

**留在宿主：**

- WineHua：Wine 启动/环境、音频 broker、图形 broker/presenter、进程 broker/NCP、桌面/任务栏/融合/模态策略、`WineWindowManager` 等 `IWindowHost/IWindowPolicy` 实现；
- BetterSH：终端/shell 业务、IEC、procmgr、资源路径、设置面板、`HgsWindowHostService` 的宿主配置。

### 2.6 seam：双向 dlsym 接缝

HGS 与宿主是**两个独立的 .so**（`libhgs.so` / `libentry.so`），OHOS 的 NAPI so 在各自加载命名空间内，**链接期弱符号不可靠**：

- 实测：weak 数据符号在 GOT 中解析为 0，解引用即 SIGSEGV；weak 函数符号常驻 NULL。
- 因此 `seam/registry.h` 约定三个跨 so 符号，全部**调用期 dlsym**：

| 提供方 | 符号 | 用途 |
|---|---|---|
| `libentry.so` | `Hgs_hostHooks` | 桌面 root 出现/会话结束的生命周期通知 |
| `libentry.so` | `Hgs_zeroCopyBackend` | zero-copy 后端注入 |
| `libhgs.so` | `Hgs_getSessionControl()` | 宿主编排问合成器：桌面模式、输出尺寸、会话重置、手柄桥 |

- 未注入时用 `NullHostLifecycleHooks` / `NullSessionControl` 兜底，宿主照样能跑（走 managed 模式）。
- dlsym 只查符号，不 dlopen 加载，不破坏"entry 不加载 hgs / hgs 不加载 entry"的隔离原则。
- 相关文件：`entry/src/main/cpp/seam_bridge/host_seams.cpp`、`hgs/src/main/cpp/seam/registry.h`、`session_control.cpp`。

### 2.7 两种宿主接入形态

**形态 A：WineHua —— 原生宿主，保留自己的窗口管理**

- ArkTS 仍用 `hgsNapi`（`libhgs.so`）和 `HgsSurface`；
- `WineWindowManager` / `FusionWindowManager` / `ModalWindowManager` / `PopupWindowManager` 继续管理窗口；
- 通过 `HostSessionControl()` 询问 HGS：桌面模式、输出尺寸、`wineboot` 等待、手柄桥；
- 适合 Wine 的 DesktopRoot、explorer、任务栏、模态、游戏全屏等复杂策略。

**形态 B：BetterSH —— 默认 ArkTS host**

```ts
// EntryAbility.onWindowStageCreate
HgsWindowHostService.init(this.context, {
  abilityName: 'AppRunnerAbility',
  windowScale: 2,
});
HgsWindowHostService.start({
  socketPath: '/data/storage/el2/base/files/wl.sock',
  inputBackend: 2,        // Native
  composition: 1,         // PerToplevel
  dataDevice: true,       // GTK 需要 wl_data_device_manager 才建 GdkSeat
  setProcessEnvironment: true,
});
```

- `AppRunnerAbility extends HgsAppWindowAbility`，只做薄子类；
- `HgsWindowHostService` 消费 `HgsWindowEvent`：Created → `startAbility`，Title/Geometry/State/Focus/Close → OHOS window API；
- `HgsWindowPage` 负责 XComponent + `HgsSurface`，原生输入后端；
- 适合"只要窗口服务、不要 Wine 策略"的宿主。

### 2.8 关键 API / 文件

**ArkTS HAR 导出（`hgs/Index.ets`）：**

```ts
export { HgsSurface } from './src/main/ets/components/HgsSurface';
export { HgsWindowPage } from './src/main/ets/components/HgsWindowPage';
export { HgsWindowHostService } from './src/main/ets/service/HgsWindowHostService';
export { default as HgsAppWindowAbility } from './src/main/ets/entryability/HgsAppWindowAbility';
```

**NAPI 稳定面（`hgs/src/main/cpp/types/libhgs/Index.d.ts`）：**

| 域 | 导出 |
|---|---|
| runtime | `startServer`、`stopServer`、`isRunning`、`getWaylandDisplayName`、`getClientEnvironment`、`getApiVersion`、`getLastError` |
| surface | `bindXComponent`、`attachSurface`、`resizeSurface`、`detachSurface`、`attachSurfaceFallback`、`regXComponentCallback` |
| toplevel | `takePendingToplevel`、`destroyToplevel`、`closeToplevel`、`notifyToplevelResize`、`findToplevelAt`、`raiseToplevel`、`setToplevelVisible` |
| input | `sendPointerEvent`、`sendKeyEvent`、`sendScrollEvent` |
| window | `setWindowCallback`、`notifyWindowGeometry/Focus/State/Closed`、`getWindow`、`listWindows`、`getWindowZOrder` |
| Wine 扩展 | pointer lock、IME、gamepad/controller、`registerHostWindow` |
| 日志 | `LOGI` / `LOGW` / `LOGE` |

**C++ 公共头（`hgs/src/main/cpp/api/`）：**

`hgs_types.h`、`hgs_events.h`、`hgs_host.h`、`hgs_endpoint.h`、`hgs_instance.h`、`hgs_window_types.h`、`hgs_window_policy.h`、`hgs_window_host.h`、`hgs_window_manager.h`、`hgs_export.h`。

### 2.9 当前状态与未完成

- 阶段 0 / 1 / 2 W1-W2b 已落地，原生编译通过；**签名 HAP 未出包**（本机未挂载签名材料）。
- 真机回归已完成的部分：单窗口渲染/输入、多窗口（两个 GTK 客户端，renderer=2、wl_pointer=2、按窗点击路由、关闭清理）、最大化/还原、资源 tar 解包。
- 未完成：N2 手势、P1.4 所有权/seam/smoke 收口、W3 BetterSH 删旧 compositor、W4 HarmonyBox。
- `HgsSurface` 仍暴露 Wine 专用 props（`mouseMode` / `floating` / `touchpadOn` / `scaleRawDelta` / `enterSynth`），计划阶段 3 拆到 `WineInputSurface`。

### 2.10 已知坑（真机踩过的）

| 现象 | 根因 | 处置 |
|---|---|---|
| 第二个窗口 no renderer / 首窗口黑 | 同进程第二个窗口的原生 `OnSurfaceCreated` 早于 HGS 回调注册触发，事件丢失 | `NoteRegisteredComponent` + `attachSurfaceFallback(surfaceId)`；首窗口黑其实是第二个窗口的不透明黑窗盖住了 |
| `controller.onSurfaceCreated` 不触发 | 使用 `libraryname: 'hgs'` 时只触发 `onLoad` | 在 `onLoad` 用 `getXComponentSurfaceId()` 主动调 fallback |
| GTK 应用没有输入 | GDK wayland backend 的 seat gate 要求 `wl_data_device_manager` global | HGS 注册 data device stub（`data_device_stub.cpp`） |
| 点击偏 26/23 px | GTK CSD 内容区偏移 | 注入前把 `contentRect.x/y` 加回 surface 坐标 |
| 还原后窗口被拉伸 / 最大化第三次失效 | 设备上 `window.maximize()` 后 `resize()` 永久返回 `1300002`；原生 `restore()` 不还原 rect | 统一走几何最大化/还原（`moveTo + resize`），`floatingOnly` 模式 |
| `GET_BUFFER_GEOMETRY` 尺寸取反 | 出参顺序是 **height, width** | `OH_NativeWindow_NativeWindowHandleOpt(nw, GET_BUFFER_GEOMETRY, &gh, &gw)` |
| HAP 里资源不生效 | app 模块与 HAR 同路径 rawfile 冲突时 app 模块优先 | 资源由宿主显式打包/解包，不依赖 HAR 覆盖 |
| `pkill` 后 renderer 仍为 2 | 沙箱 `pkill/ps` 看不到/杀不掉 NCP 子进程；toplevel 仍存活 | 用窗口关闭按钮验证 teardown（`DestroyRenderer 2→1`） |

---

## 3. 分支之后 WineHua 的新增内容

### 3.1 规模

以分支基线 `3eecbd732` 对比 `upstream/master`（`a001e6d0c`）：

| 指标 | 数值 |
|---|---|
| 提交数 | **313** |
| 变更文件 | **228** |
| 增 / 删 | **+31,753 / −1,823** |
| 主要新增目录 | `entry/src/main/cpp/display/`（22 文件） |
| 新增子模块 | `thirdparty/wlroots`、`thirdparty/xserver` |
| 新增补丁 | `scripts/patches/` 下 8 个（wlroots / xserver / xtrans） |

### 3.2 新显示栈：wlroots + Xwayland

上游在 `entry/src/main/cpp/display/` 新增了与 HGS 并行的第二条显示栈：

| 文件 | 作用 |
|---|---|
| `display_compositor.cpp` | app 进程内 wlroots 合成器：`wl_display` + headless backend + pixman renderer + `wlr_compositor` + `wlr_xwayland_create` |
| `ohos_output.c` | OHOS 输出：headless output、custom mode、背景 scene rect、`SET_BUFFER_GEOMETRY` |
| `x11_toplevel.c` | X11 窗口映射层：X 窗口 → toplevel 事件 |
| `x11_toplevel_bridge.cpp` | X11 映射层与 ArkTS/NAPI 的桥 |
| `display_input.c` | 显示路线输入注入（归一化坐标、键鼠） |
| `display_guest_frames.cpp` | guest 帧路由/纹理生命周期 |
| `ohos_egl_import.c` | OHOS EGLImage 导入（gles2 零拷贝） |
| `ohos_buffer.cpp` | OHOS buffer 封装 |
| `ncp/xwayland_child.cpp` | Xwayland 的 NCP 子进程入口 |
| `ncp/xclient_child.cpp` | X client 子进程入口 |
| `ncp/xim_server_child.cpp` | XIM 输入法服务器（NCP 子进程） |
| `xim_bridge.c` | XIM 协议桥 |
| `desktop_fit.h` | 桌面内容 fit / letterbox |

Xwayland 启动链：

```text
wlroots server.c（补丁）
  → wlr_ohos_spawn_xwayland（display_compositor.cpp）
  → OH_Ability_StartNativeChildProcess("libxwayland_child.so:Main")
  → 命名 fd 五连（x_fd0/x_fd1/wl_fd/wm_fd/displayfd）随 args.fdList 下发
  → 子进程 dlopen libxwayland_ohos.so 调 main()
```

8 个 OHOS 补丁：

- `wlroots-ohos-ncp-spawn.patch`
- `wlroots-ohos-gles2-egl-import.patch`
- `wlroots-ohos-shm-fchmod-tolerant.patch`
- `wlroots-ohos-xwm-child-geometry.patch`
- `wlroots-ohos-xwm-restack-event.patch`
- `wlroots-scene-damage-diag.patch`
- `xserver-xwayland-ohos-ncp.patch`
- `xtrans-abstract-client.patch`

### 3.3 显示路线：默认 x11

`entry/src/main/ets/service/DisplayRouteService.ets`：

```ts
export const ROUTE_WAYLAND: string = 'wayland';
export const ROUTE_X11: string = 'x11';
// 缺省 x11 (2026-10-10 用户拍板: x11 为当前主线, 默认即 x11 方便手工随时测试)
const DEFAULT_ROUTE: string = ROUTE_X11;
```

三级优先：

1. `want` 参数 `winehua.displayRoute`（内存覆盖，smoke/自动化钉档位）；
2. 设置页"显示路线（实验）"持久化选择（`preferences`，`display_route` / `route`）；
3. 缺省 `x11`。

`applyRoute` 是全 app 唯一有权建立/切换"已应用路线"的入口：盖 env 章 + 拆旧 x11 链 + 起 x11 链。消费点白名单：DesktopWindow surface 分发、输入分发、native env 汇集点、窗口装饰形态（`FusionWindowManager.stripPxOf`）。

**这与 HGS 的关系：**

- `ROUTE_X11` 走 `entry/src/main/cpp/display/` 的新 wlroots/Xwayland 栈，**不经过 HGS 合成器**；
- `ROUTE_WAYLAND` 才是 HGS 的用武之地；
- 上游默认 x11，意味着合并后 WineHua 的默认显示路径不再是 HGS。HGS 在 WineHua 里变成"wayland 路线的实现 + 窗口/输入/资源服务"，而不是唯一显示栈。

### 3.4 x11 窗口管理与输入

上游在 x11 路线上补齐了与 wayland 路线对齐的能力：

| 能力 | 关键提交 / 文件 |
|---|---|
| x11 多窗模式（M4a） | `537bd4492` per-xs 呈现循环、`3209e3495` 按窗输入路由、`7d8ed37b9` ArkTS 承载接线 |
| 窗口状态（M4b） | `77bcd813b` minimize/fullscreen/activate/parent；`a708e3269` 最小化/还原闭环 |
| popup（M4c） | `8df092cfc` popup_show/move/resize/hide 直译 |
| 几何回写 | `c870de696` host→wine 几何回写 + echo 抑制 |
| 重叠窗输入 | `6e01edff1` 注入前 X restack + 输入焦点同步 |
| 按窗关闭/滚轮 | `d54023b9d` `WM_DELETE_WINDOW`；`6badc4b2a` 按窗轴口 |
| 模态对话框 | `579e4bf32` 事件时序 + owner 禁用 + 模态顶条 |
| 全屏 letterbox | `06c21b86d` 桌面内容 fit + letterbox 吞输入 |
| frame done / damage | `154313331` 多窗呈现泵；`7af006d27` resize 后强制重渲染 |

这些改动大多在 `entry/src/main/cpp/display/` 和 `entry/src/main/ets/` 的窗口/页面层，与 HGS 的 wayland 实现是**两套并行链**。

### 3.5 IME / XIM

- x11 路线新增 **XIM server**（`ncp/xim_server_child.cpp`，1185 行），运行在 NCP 子进程；
- `xim_bridge.c` 做协议桥；
- x11 IME 全双工：上行激活链 + commit 路线分叉，退役旧文本桥（`9a3647810`）；
- 软键盘文本意图门控：焦点不再即弹键盘（`6cab7c97d`）；
- XIM 健壮性：`MSG_NOSIGNAL`、通道坏头重同步、xkm 全量写（`369980124`）。

wayland 路线仍走 HGS 的 `zwp_text_input_v3`（`input/text_input.*`）。两条路线的 IME 实现不同，合并时不要互相覆盖。

### 3.6 音频与手柄

PR#73（`1702b979c`）合入：

- **per-endpoint audio mix**：`audio_broker.cpp` 改动 691 行，按 endpoint 管理 `audio_stream`，多进程混音；
- **WHGP v2 canonical controller axes**：
  - 统一坐标：Stick X 右正、Stick Y **上正**、Hat X/Y 右/上正、Trigger 0..1；
  - `ControllerHub` 只合并 canonical 状态；
  - WHGP v2 直接拷贝 hub 字段，版本不匹配断开；
  - winebus `bus_ohos` 映射到 HID/XInput 时不做模拟 Y 反转。

新增文档 `docs/controller-canonical-coordinates.md`。HGS 侧对应 `input/controller/`，合并时需要把上游 v2 协议与 HGS 的导出面（`controllerSet*` / WHGP socket）对齐。

### 3.7 其他

- **确定性 assemble**：CI 子模块拉取自管 + 退避重试（`f436bf376`）；
- **路线切换 SEGV 修复**：egl renderer 单例缓存销毁后作废（`6f0deec2f`）；
- **大量 smoke / 验收用例**：`smoke/tests/*`、`smoke/suites/*`、`automation/checks/selftest/*`；
- **文档**：`docs/engineering/display-route-known-issues.md`、`docs/superpowers/plans/*`、`docs/superpowers/specs/*`；
- **子模块 bump**：wine、mesa、wayland、wayland-protocols、libxkbcommon，新增 wlroots / xserver。

### 3.8 对 HGS 的影响（合并前必须想清楚）

1. **路线分流**
   - `x11`：走 `entry/src/main/cpp/display/`，HGS 不参与渲染；
   - `wayland`：走 HGS 合成器。
   - 合并后 `DisplayRouteService` 的默认值仍是 `x11`。如果希望 HGS 继续服务 WineHua 的 wayland 路线，必须保留 HGS 的 `entry` 接线，并确保 `ROUTE_WAYLAND` 分支仍能启动 HGS。

2. **HGS 在 WineHua 中的定位变化**
   - 分支创建时：HGS = WineHua 的合成器（唯一显示栈）；
   - 合并上游后：HGS = wayland 路线的合成器 + 窗口/输入/资源服务；
   - BetterSH：HGS 仍是唯一合成器。
   - 这一定位变化应写进 HGS 的 README / 架构文档，避免后来者以为 HGS 是 WineHua 默认显示栈。

3. **新显示栈是否迁入 HGS？**
   - 短期不建议：`display/` 强依赖 wlroots/Xwayland/NCP，和 Wine 域耦合深；迁入 HGS 会把 HGS 从"通用合成器"变成"Wine 专用显示栈"，破坏 BetterSH 的轻量接入。
   - 长期若要让 BetterSH 也支持 X11 应用，可以再抽 `display/` 为可选模块，但不能和现有 `compositor/` 混在一起。

4. **事件/窗口契约要对齐**
   - 上游 `host_tests/toplevel_event_test.cpp` 已演进到 `CreatedAt`、`Restored`、`Modal` 等事件；
   - HGS 的 `api/hgs_events.h` 是单一来源，合并时需要先扩展 HGS 事件枚举，再改测试，避免两边枚举漂移。

### 3.9 合并冲突与策略

在临时 worktree 里做过 dry-run merge（未污染工作区）：

| 冲突类型 | 数量 | 典型文件 |
|---|---|---|
| 修改/删除（我们删了、上游改了） | **28** | `entry/src/main/cpp/compositor/*`、`input/controller/*`、`input/pointer_extras.cpp`、`input/text_input.*`、`protocols/*`、`bridge/plugin_manager.cpp`、`graphics/egl_renderer.cpp`、`common/perf_utils.*`、`smoke/smoke_napi.cpp` |
| 内容冲突 | **14** | `.gitmodules`、`entry/oh-package.json5`、`entry/src/main/cpp/CMakeLists.txt`、`bridge/napi_init.cpp`、`types/libentry/Index.d.ts`、`wine/wine_launch.cpp`、`DesktopLayer.ets`、`DesktopWindow.ets`、`WineWindow.ets`、`FusionWindowManager.ets`、`PopupWindowManager.ets`、`WineWindowManager.ets`、`SmokeRunner.ets`、`host_tests/toplevel_event_test.cpp` |

**处置原则：**

1. **修改/删除类：不要恢复 entry 副本。**
   这些文件的内容已经在 hgs 里有对应实现。把上游对它们的改动（新字段、新事件、新逻辑）移植进 hgs 对应文件，entry 继续不保留副本。
2. **内容冲突类：按域拆分。**
   - `.gitmodules`：保留 `hgs`，追加 `wlroots` / `xserver`；
   - `oh-package.json5`：保留 `hgs` HAR，按上游增加 `display_route_napi` 等依赖；
   - `CMakeLists.txt`：entry 只编 Wine/音频/图形/进程/显示栈；合成器源文件继续由 hgs 编；
   - `napi_init.cpp`：Wine/音频/display 的 NAPI 留 entry，合成器 NAPI 留 hgs；不要重新合并成一份；
   - `wine_launch.cpp`：上游路线逻辑 + HGS 的 `HostSessionControl()` seam 共存；
   - ArkTS 页面：上游新增的路线分支/几何逻辑与 HGS 的 `HgsSurface`/绑定逻辑合并，保留双方入口；
   - `toplevel_event_test.cpp`：先对齐 HGS 事件枚举，再改测试。
3. **先完成 hgs 基线收尾，再合并。**
   两宿主工作区已同步到 `bad402d`（WineHua 通过从 BetterSH clone 本地 fetch 后 fast-forward，原 `Index.d.ts` 本地修改已被目标版本覆盖，含 `attachSurfaceFallback`）。但 `bad402d` 尚未推到 `origin/main`，WineHua 外层子模块指针也尚未提交；合并前应完成这两步，否则冲突处置仍可能基于旧实现。

**建议的合并批次（每批可独立编译/回滚）：**

| 批次 | 内容 | 验证 |
|---|---|---|
| B0 | 提交 WineHua 外层 hgs 指针到 `bad402d`（工作区已同步），推送 `bad402d` 到 origin | hgs 编译 + 双宿主 host_tests |
| B1 | `.gitmodules` / 子模块 bump / `oh-package` / `CMakeLists` | 空壳编译 |
| B2 | 28 个 modify/delete：上游改动移植进 hgs | `make` + host_tests |
| B3 | `napi_init.cpp` / `wine_launch.cpp` / `Index.d.ts` 接缝 | 启动 wine、wayland 路线冒烟 |
| B4 | ArkTS 页面/窗口管理器：路线分支 + HGS 绑定 | 真机 x11/wayland 双路线 |
| B5 | 事件契约 + `toplevel_event_test` + smoke | `make test` + smoke 套件 |

---

## 4. HGS 如何同时服务 WineHua 与 BetterSH

### 4.1 两个宿主的差异

| 维度 | WineHua | BetterSH |
|---|---|---|
| 宿主业务 | Wine 引擎、explorer、桌面/任务栏/融合/模态 | 终端 / shell / IEC / 包管理 |
| 合成模式 | DesktopRoot（PC/虚拟桌面）或 PerToplevel | PerToplevel |
| 窗口管理 | 自有 `WineWindowManager` / `FusionWindowManager` / `ModalWindowManager` / `PopupWindowManager` | HGS 默认 host：`HgsWindowHostService` + `HgsAppWindowAbility` + `HgsWindowPage` |
| 输入 | ArkTS 注入为主；N1 原生路径可用；Wine 专用 pointer lock / relative / IME | Native（`inputBackend=2`）；GTK CSD |
| 缩放 | effectiveScale / windowScale / density 1.7 | `windowScale: 2` |
| 协议开关 | relativePointer、textInput、winehuaToplevel、externalSurface | dataDevice（GTK seat/clipboard） |
| 资源 | Wine sysroot（rawfile 解包） | xkb-data / fonts tar（HGS 打包脚本产物） |
| 构建 | `make` + `hvigorw` | `scripts/build_hap.sh` / `hvigorw` |
| 窗口装饰 | 系统装饰 + Fusion 顶条（仅 x11 形态） | 无系统装饰 / 隐藏 decor |

### 4.2 设计原则（不能破坏）

1. **core 不依赖平台**
   `hgs/api/*.h` 只允许标准库；`HgsWindowManager` / `WindowRegistry` / policy 不创建 Ability、不调 OHOS window API、不解析宿主路径。
2. **默认值 = 不修改宿主**
   `OutputConfig::width/height = 0`、`CompositionMode::Unspecified` 表示"保持宿主现状"。WineHua 在 `startServer` 前已经算好屏幕尺寸和模式，HGS 不能用 1280x720 / PerToplevel 覆盖它。
3. **seam 可选**
   宿主不注入 `HostLifecycleHooks` / `ZeroCopyBackend` 时，HGS 用 Null 实现；BetterSH 不需要这些也能跑。
4. **默认 host 可替换**
   WineHua 用 `IWindowHost` / `IWindowPolicy` 保留自己的策略；BetterSH 用默认 ArkTS host。两者不要同时启用。
5. **资源自包含**
   HGS 不引用 BetterSH 的路径，也不引用 WineHua 的 sysroot；资源通过 rawfile 或宿主显式传入。此前明确要求：**hgs 不得回退到 BetterSH 路径**。
6. **同一份源码，各宿主重编**
   HAR 是源码依赖，不承诺 C++ ABI；版本差异通过 `getApiVersion()` / `.d.ts` 管理。
7. **日志走项目 logger**
   `hgs_instance.cpp` 不得直接使用 hilog；统一 `logger/`（多 sink：stderr / 沙箱文件 / hilog），ArkTS 用 `LOGI/LOGW/LOGE`。

### 4.3 配置矩阵

> 下表是**迁移到对象配置后的目标值**。当前阶段 `FeatureConfig` 只被解析记录，注册行为仍按旧逻辑（多数 global 恒注册）；`dataDevice` 在两宿主 hgs 工作区里均已由 `data_device_stub.cpp` **无条件注册**（WineHua 外层指针待提交）。

| 配置 | WineHua | BetterSH | 说明 |
|---|---|---|---|
| `composition` | `Unspecified`（当前走旧字符串入口）；PC 模式由宿主 `applyModePolicy` 设 DesktopRoot | `PerToplevel` (1) | `0` 表示不修改 |
| `inputBackend` | ArkTS（1）为主；可切 Native（2） | Native（2） | BetterSH 不需要 ArkTS 输入事件 |
| `dataDevice` | 目标 true（若 Wine 侧需要剪贴板/GTK） | true | GTK 应用必须有 `wl_data_device_manager` |
| `textInput` | true（IME） | false | WineHua IME 走 text-input-v3 |
| `relativePointer` | true（游戏） | false | Wine dinput 需要 |
| `externalSurface` | true（zero-copy / VirGL / Venus） | false | Wine 图形专用 |
| `winehuaToplevel` | true | false | WineHua 专用协议 |
| `setProcessEnvironment` | true（旧行为） | true（shell 子进程继承） | 也可用 `getClientEnvironment()` 自行传 |
| `windowScale` | effectiveScale / 2 | 2 | 逻辑尺寸 → OHOS 窗口尺寸 |
| `floatingOnly` | 视设备/版本 | 建议 true（几何最大化） | 原生 maximize/restore 在部分设备不可用 |

> WineHua 当前仍用 `hgsNapi.startServer(SOCK)` 字符串入口，等价于 `FeatureConfig` 默认值；迁移到对象配置时按上表显式设置。

### 4.4 宿主接入步骤

**BetterSH（默认 host）**

1. `module.json5` 注册 `HgsAppWindowAbility` 子类（`launchType: multiton`）；
2. `main_pages.json` 注册 `pages/HgsWindowPage`；
3. `EntryAbility.onWindowStageCreate`：
   ```ts
   HgsWindowHostService.init(this.context, { abilityName: 'AppRunnerAbility', windowScale: 2 });
   HgsWindowHostService.start({ socketPath, inputBackend: 2, composition: 1, dataDevice: true });
   ```
4. `AppRunnerAbility extends HgsAppWindowAbility {}`；
5. 启动子进程时用 `getClientEnvironment()` 拼 `XDG_RUNTIME_DIR` / `WAYLAND_DISPLAY`；
6. 运行 `pack_xkb_data.sh` / `pack_font.sh`，并在 EntryAbility 解包到 sysroot。

**WineHua（原生宿主）**

1. entry 保留 `WineWindowManager` 等 `IWindowHost/IWindowPolicy` 实现；
2. ArkTS 页面使用 `HgsSurface`，`onLoad` 传 toplevelId（`bindXComponent(id)`）；
3. `startServer` 传对象配置（见 §4.3），或在旧入口后按需 `setOutputSize` / `setDesktopMode`；
4. `seam_bridge/host_seams.cpp` 注入 `Hgs_hostHooks` / `Hgs_zeroCopyBackend`；
5. `wine_launch.cpp` 通过 `HostSessionControl()` 询问桌面模式、输出尺寸、手柄桥；
6. 保留 Wine 专用 NAPI（pointer lock / IME / controller）与 HGS 的 canonical 导出并存。

### 4.5 窗口策略差异

| 能力 | WineHua | BetterSH |
|---|---|---|
| 窗口创建 | `WineWindowManager` 调 `startAbility`，按 Wine 进程/explorer 规则 | `HgsWindowHostService` 收到 Created 即建窗 |
| 桌面 root | `DesktopRoot` + `DesktopWindow` | 无 |
| 任务栏/融合 | `FusionWindowManager` | 无 |
| 模态 | `ModalWindowManager` + owner 禁用 | 由 policy 默认实现 |
| 最大化/还原 | 几何方式（设备兼容） | 几何方式（`floatingOnly`） |
| 关闭 | Wine 进程收尾 + `closeToplevel` | `CloseRequested` → `terminateSelf` |

### 4.6 输入差异

| 能力 | WineHua | BetterSH |
|---|---|---|
| 输入入口 | ArkTS `onTouch/onMouse/onAxisEvent/onKeyEvent` → NAPI | 原生 `OH_NativeXComponent` 回调 |
| 坐标变换 | letterbox 反变换 + CSD contentRect + scale | XComponent 本地坐标 + `windowScale` |
| 指针锁定 | `pointer_extras` + relative pointer | 无 |
| IME | text-input-v3 + `sendImeCommit/Preedit` | 无（终端自己处理键盘） |
| 手柄 | Controller Hub + WHGP v2 | 可选 |
| 多窗口 | 每窗口一个 `HgsSurface` + `attachSurfaceFallback` | 同左（W2b 已验证） |

### 4.7 资源与构建差异

**HGS 资源打包（两宿主 hgs 工作区均已包含）：**

- `scripts/pack_xkb_data.sh`：xkb-data 2.42-1 → `src/main/resources/rawfile/tools/xkb-data.tar`；
- `scripts/pack_font.sh`：DejaVu Sans Mono + WenQuanYi Zen Hei + 生成的 `fonts.conf` → `fonts.tar`（约 17 MB，5 个条目）；
- tar 产物在 `.gitignore` 中（`/src/main/resources/rawfile/tools/*.tar`），可再生产；
- BetterSH `EntryAbility` 启动时 `unpackResource('tools/xkb-data.tar', sysroot)` / `unpackResource('tools/fonts.tar', sysroot)`。

**WineHua：**

- 使用自己的 sysroot / rawfile 打包流程；
- HAP 合并规则：**app 模块与 HAR 同路径 rawfile 冲突时 app 模块优先**（已实测）。因此 WineHua 若也要 HGS 的字体/xkb，需要显式从 hgs rawfile 取或自行打包，不能假设 HAR 覆盖。

**构建顺序（两个宿主都一样）：**

```bash
# 1. hgs 预编译依赖（每个 checkout 一次；libs/ 与 thirdparty/ 被 gitignore）
bash hgs/scripts/build_all.sh

# 2. 资源 tar（BetterSH 需要；WineHua 可选）
bash hgs/scripts/pack_xkb_data.sh
bash hgs/scripts/pack_font.sh

# 3. 宿主 HAP
make NATIVE_ARCH=arm64-v8a hap          # WineHua
./scripts/build_hap.sh                  # BetterSH（按产品 tablet/phone）
```

### 4.8 双宿主回归清单

每次改 HGS 公共面，至少跑：

- [ ] `bash hgs/scripts/build_all.sh` 通过；
- [ ] WineHua `make test`（host_tests：endpoint / event map / window manager / native keymap / toplevel event）；
- [ ] BetterSH `hvigorw assembleHap` 通过；
- [ ] BetterSH 真机：启动终端窗口、输入、缩放、关闭；
- [ ] WineHua 真机：wayland 路线启动、单窗口、多窗口、最大化/还原；
- [ ] WineHua 真机：x11 路线启动（合并上游后）、窗口/输入/IME；
- [ ] `libhgs.so` 导出面与 `.d.ts` 一致（`getApiVersion` 不降级）；
- [ ] 资源 tar 解包后字体/xkb 生效（无 CJK 豆腐块、键盘布局正确）。

### 4.9 禁止事项

- ❌ 在 hgs 里引用 `/workspace/BetterSH` 或 WineHua sysroot 的绝对路径；
- ❌ 在 `hgs_instance.cpp` 直接用 hilog（用 `logger/`）；
- ❌ 把 `HgsConfig` 默认值改成具体屏幕尺寸/模式（必须是 0 / Unspecified）；
- ❌ 删除 Wine 专用导出（pointer lock / IME / controller）而不走 deprecation；
- ❌ 在 BetterSH 和 WineHua 里同时启用 `IWindowHost` 原生 host 与默认 ArkTS host；
- ❌ 先推外层 submodule 指针、后推 hgs（会导致别人 checkout 失败）；
- ❌ 把上游 `display/` 新栈直接搬进 hgs 的 `compositor/`（两者生命周期/依赖不同）。

---

## 5. 当前状态、风险与下一步

### 5.1 指针表（2026-11-06）

| 仓库 / 分支 | HEAD | 状态 |
|---|---|---|
| WineHua `feature/hgs-har-scaffold` | `3355dab96` | 本地分支，41 提交，基于 `3eecbd732`；未见上游跟踪分支 |
| WineHua `hgs` 子模块 | `bad402d` | 工作区已从 BetterSH clone 同步（clean）；外层指针未提交；`origin/main` 仍为 `bad4d5f` |
| BetterSH `feature/hgs-migration` | `7392c3d` | 领先 `origin/feature/hgs-migration` 4 个提交 |
| BetterSH `hgs` 子模块 | `bad402d` | 领先 `origin/main` **7 个提交（未推送）** |
| WineHua `upstream/master` | `a001e6d0c` | 相对基线 313 提交 / 228 文件 / +31,753 −1,823 |

`bad402d` 相对 `bad4d5f` 的 7 个提交（BetterSH 侧已真机验证）：

1. `fc9a1b2` 补 `wl_data_device_manager` global，解除 GDK seat gate；
2. `9a5ea13` 指针坐标补 CSD 内容偏移，输入链路日志可诊断；
3. `5937804` XComponent 回调注册可重试，导出 `regXComponentCallback`；
4. `1fa74e6` 最大化/还原统一走几何方式，修还原后窗口被拉伸；
5. `591eb10` hgs 自产完整 GTK3 栈 + gtk-smoke 冒烟客户端；
6. `abc4843` 新增 xkb / 字体资源打包脚本；
7. `bad402d` 多窗口：同进程第二个窗口拿不到 renderer / 输入。

> `bad402d` 已同步到 WineHua 的 hgs 工作区（从 BetterSH clone 本地 fetch 后 fast-forward；原 `Index.d.ts` 本地修改已被目标版本覆盖，目标版本含 `attachSurfaceFallback`）。因此 WineHua 当前工作区已具备 data_device、CSD 偏移、多窗口、GTK 栈、资源脚本；但 `origin/main` 仍在 `bad4d5f`，WineHua 外层指针也未提交，fresh checkout 仍会拿到旧版本。

### 5.2 风险登记

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R1 | `bad402d` 未推送 + WineHua 外层指针未提交 | fresh checkout 仍拿 `bad4d5f`；协作者/CI 无法复现当前工作区 | 推送 `bad402d` 到 origin；提交 WineHua 子模块指针 |
| R2 | 上游默认 x11 | HGS 不再是 WineHua 默认显示栈，可能 bit-rot | 保留 `ROUTE_WAYLAND` 冒烟；CI 跑 wayland 路线 |
| R3 | 42 个合并冲突，28 个 modify/delete | 容易误恢复 entry 副本，产生第三份分叉 | 按 §3.9 批次；modify/delete 一律移植进 hgs |
| R4 | HGS 源码依赖、无 ABI 承诺 | 两宿主版本漂移时行为不一致 | `getApiVersion` + `.d.ts` + 双宿主回归 |
| R5 | 资源 tar 不入库（17 MB） | 新 checkout 缺字体/xkb；HAP 里没资源 | 构建脚本自动生成；CI 检查；启动自检日志 |
| R6 | 设备差异（density 1.7 / 2880x1920 / windowScale 2） | 坐标/窗口尺寸错位 | 保留 letterbox 反变换测试；真机回归 |
| R7 | 原生 maximize/restore 不可用（`1300002`） | 窗口拉伸/第三次最大化失效 | 几何方式；`floatingOnly` |
| R8 | `display/` 新栈与 HGS 并行 | 维护两套窗口/输入逻辑 | 明确路线边界；短期不合并进 HGS |

### 5.3 建议路线

**第一步：完成 HGS 基线收尾（工作区已同步）**

- 已完成：WineHua hgs 工作区从 BetterSH clone 本地 fetch 并 fast-forward 到 `bad402d`；原 `Index.d.ts` 本地修改被目标版本覆盖（含 `attachSurfaceFallback`），工作区 clean。
- 待做：推送 `bad402d` 到 `origin/main`；提交 WineHua 外层 hgs 指针；两宿主各跑一次构建/冒烟。

**第二步（按需）：决定上游合并策略**

- **方案 A（推荐）：完整合并上游 + 重新接线**
  - 按 §3.9 的 B0-B5 批次；
  - 保留 x11 新栈，wayland 路线继续走 HGS；
  - 工作量大，但主线一致，后续上游修复可以直接跟进。
- **方案 B：先移植单块**
  - 优先 WHGP v2 / 音频 per-endpoint / XIM（与显示路线解耦）；
  - 适合短期要发版、不想动显示栈的情况；
  - 代价是继续与上游漂移，42 个冲突留到以后。

**第三步：HGS W3 收口**

- BetterSH 删除旧 WaymonyService / ClientWindow / 旧 compositor；
- 只保留 `HgsWindowHostService` + `HgsAppWindowAbility` + `HgsWindowPage`；
- 验证 data_device / GTK / 资源 / 多窗口 / 缩放。

**第四步：HGS 文档与边界固化**

- 把"x11 是 WineHua 主线、HGS 服务 wayland 路线 + BetterSH"写进 HGS README；
- 补 `hgs/docs/`（目前只有一行 README）；
- 把 §4.9 的禁止事项写进 CI 检查（路径引用、hilog、默认值）。

---

## 6. 速查

### 6.1 常用命令

```bash
# WineHua
cd /workspace/WineHua-root/WineHua
git submodule update --init --recursive
bash scripts/build_hgs.sh
make NATIVE_ARCH=arm64-v8a hap
make test

# BetterSH
cd /workspace/BetterSH
git submodule update --init --recursive
./scripts/build_hap.sh          # 或 hvigorw assembleHap
bash hgs/scripts/build_all.sh
bash hgs/scripts/pack_xkb_data.sh
bash hgs/scripts/pack_font.sh

# 设备
hdc list targets
hdc install -r <hap>
hdc shell aa start -a <ability> -b <bundle>
hdc shell hilog -x
hdc shell snapshot_display -f /data/local/tmp/s.png
```

### 6.2 路径 / socket

| 项 | 值 |
|---|---|
| BetterSH HGS socket | `/data/storage/el2/base/files/wl.sock` |
| WineHua HGS socket | 由 `WineEnvService.SOCK` 传入（应用 files 目录） |
| HGS 客户端环境 | `XDG_RUNTIME_DIR=...`、`WAYLAND_DISPLAY=...`（`getClientEnvironment()`） |
| 设备字体目录（BetterSH） | `/data/storage/el2/base/files/fonts`（`fonts.conf` 默认） |
| HGS 资源 tar | `hgs/src/main/resources/rawfile/tools/*.tar`（gitignored） |
| hgs 预编译库 | `hgs/libs/$ARCH`（gitignored） |

### 6.3 日志 tag

| 层 | tag / 关键字 |
|---|---|
| HGS NAPI / surface | `HGS_NAPI` |
| HGS 窗口 host | `HGS_WinHost` |
| HGS 输入 | `[Input]`、`[CLICK-PIPE]`、`[HGS-NAPI]` |
| HGS 合成器 | `[WL-STAT]`、`[Compositor]` |
| WineHua 图形 | `wine` / `graphics` / `virgl` |
| 显示路线（上游） | `display` / `x11` / `xim` |

### 6.4 设备

- 真机：2880×1920 平板，density 1.7，windowScale 2；
- `hdc tconn` / `fport` / `file send/recv` / `hilog` / `snapshot_display` / `uitest uiInput click` / `aa start|force-stop` / `hidumper` / `power-shell wakeup`；
- 详细见 `docs/cheatsheet.md`、`docs/debugging/remote-hdc.md`。

---

## 附录 A：术语

| 术语 | 含义 |
|---|---|
| HGS | HarmonyOS Graphics UserInterface Service：抽出的合成器/输入/窗口服务 |
| HAR | HarmonyOS Archive：ArkTS 源码依赖包；HGS 以 HAR + `libhgs.so` 形式提供 |
| toplevel | Wayland 的顶层窗口对象（xdg_toplevel / WineHua toplevel） |
| PerToplevel | 每个 toplevel 一个 OHOS 窗口 |
| DesktopRoot | 所有窗口合成到一个 root surface（WineHua PC/虚拟桌面） |
| NCP | `OH_Ability_StartNativeChildProcess`，鸿蒙原生子进程 |
| seam | HGS 与宿主之间的可选 dlsym 接缝（生命周期/zero-copy/session control） |
| WHGP | WineHua Gamepad Protocol，v2 为 canonical 轴坐标 |
| CSD | Client-Side Decoration（GTK 客户端装饰） |
| letterbox | 保持宽高比的信箱式缩放；输入需要反变换 |

## 附录 B：关键文件索引

| 主题 | 文件 |
|---|---|
| WineHua 总览 | `docs/architecture/overview.md` |
| HGS 方案 | `docs/architecture/hgs-shared-runtime-phase0.md` |
| HGS 导出面 | `docs/architecture/hgs-exported-api.md` |
| HGS 公共 API | `hgs/src/main/cpp/api/` |
| HGS NAPI 声明 | `hgs/src/main/cpp/types/libhgs/Index.d.ts` |
| HGS ArkTS 导出 | `hgs/Index.ets` |
| HGS 默认 host | `hgs/src/main/ets/service/HgsWindowHostService.ets` |
| HGS 窗口 core | `hgs/src/main/cpp/api/hgs_window_manager.*`、`hgs_window_policy.*` |
| HGS 接缝 | `hgs/src/main/cpp/seam/`、`entry/src/main/cpp/seam_bridge/host_seams.cpp` |
| HGS surface 生命周期 | `hgs/src/main/cpp/graphics/egl_surface.cpp` |
| HGS 原生输入 | `hgs/src/main/cpp/input/native_input.cpp` |
| 上游显示栈 | `entry/src/main/cpp/display/`（`upstream/master`） |
| 显示路线 | `entry/src/main/ets/service/DisplayRouteService.ets` |
| 路线已知问题 | `docs/engineering/display-route-known-issues.md` |
| 手柄 canonical | `docs/controller-canonical-coordinates.md` |
| 构建指南 | `docs/build/guide.md`、`docs/build/env.md` |
| 调试 | `docs/cheatsheet.md`、`docs/debugging/` |

---

> 维护建议：本文的 §3.2–§3.9 与上游提交强相关，合并后应更新指针和冲突清单；§4 的配置矩阵在任一宿主改动 HGS 配置时同步；§5.1 的指针表每次 bump 子模块后更新。

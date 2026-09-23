# Wine on HarmonyOS — 架构设计

> **总览入口**：[overview.md](overview.md)（四域总架构图 + 进程拓扑 + 模块索引）。
> 本文聚焦 Wine 内部架构与 Wayland compositor 模块结构。

## 1. Wine 内部架构

```
┌──────────────────────────────────────────────────────────┐
│              Windows PE 可执行文件 (.exe)                  │
│              x86_64 指令，PE 格式                          │
├──────────────────────────────────────────────────────────┤
│                    ntdll.dll (PE 侧)                      │
│   ┌──────────────────────────────────────────────────┐  │
│   │  loader.c   virtual.c   thread.c   heap.c         │  │
│   │  (PE 加载器，Windows 语义)                          │  │
│   └──────────────┬───────────────────────────────────┘  │
│                  │ __wine_syscall_dispatcher              │
│                  │ (汇编 trampoline: PE→Unix 上下文切换)    │
│                  ▼                                        │
├──────────────────────────────────────────────────────────┤
│              ntdll/unix/ (Unix 侧，原生 ELF .so)           │
│   ┌──────────────────────────────────────────────────┐  │
│   │  signal_x86_64.c   thread.c   virtual.c           │  │
│   │  server.c          loader.c   sync.c              │  │
│   │  (POSIX/Linux 系统调用，实现 NT 语义)               │  │
│   └──────────┬───────────────────────────────────────┘  │
│              │ Unix Domain Socket (sendmsg/recvmsg)       │
│              ▼                                            │
│         ┌──────────┐                                      │
│         │wineserver│  (独立进程，事件驱动的 I/O 循环)      │
│         └──────────┘                                      │
└──────────────────────────────────────────────────────────┘
```

### 关键层次

| 层 | 文件位置 | 编译目标 | 功能 |
|---|---------|---------|------|
| PE DLL | `dlls/ntdll/` (不含 `unix/`) | PE (x86_64-w64-mingw32) | Windows NT API 实现 |
| Unix .so | `dlls/ntdll/unix/` | ELF .so (native) | Unix 系统调用封装 |
| wineserver | `server/` | ELF 可执行文件 | 进程/线程管理，同步对象 |

### 桥接点

**(A) NT Syscall Dispatcher** (`__wine_syscall_dispatcher`)
- 位置: `dlls/ntdll/unix/signal_x86_64.c`
- 功能: PE 代码调用 NT 系统调用时，切换上下文到 Unix
- 关键 API: `arch_prctl(ARCH_SET_GS, ...)`, `arch_prctl(ARCH_SET_FS, ...)`

**(B) Unix Call Dispatcher** (`__wine_unix_call_dispatcher`)
- PE DLL 调用 Unix 函数 (如加载 Unix .so)

**(C) wineserver 通信**
- Unix Domain Socket (`sendmsg`/`recvmsg`)
- 每个线程一个 socket

---

## 2. 当前架构 (HarmonyOS ARM64)

```
┌──────────────────────────────────────────────────────────┐
│  Windows x86_64 程序 (notepad.exe 等)                     │
│        ↓ Box64 (x86_64 → ARM64 指令翻译)                  │
├──────────────────────────────────────────────────────────┤
│  Wine PE DLLs (x86_64) + Unix .so (x86_64, musl)         │
│        ↓ winewayland.drv                                 │
├──────────────────────────────────────────────────────────┤
│  Wayland compositor (ARM64 原生, HAP 内)                   │
│  ├── WaylandServer (wl_compositor, xdg_shell, wl_seat)   │
│  ├── InputManager (鼠标/键盘事件注入)                      │
│  └── EglRenderer (EGL/GLES → XComponent 上屏)            │
├──────────────────────────────────────────────────────────┤
│  HarmonyOS Kernel (ARM64, Linux 5.10/6.6)                 │
└──────────────────────────────────────────────────────────┘
```

ARM64 下，Box64 编译为共享库 (box64.so)，由 NCP 子进程 `wine_child.so:Main()` dlopen 加载，
`box64_hmos_main()` 在同一进程内模拟执行 x86_64 Wine ELF。
x86_64 下 Wine 原生 .so 直接由系统 linker 加载，无需 Box64。

wine、wineserver 通过 `OH_Ability_StartNativeChildProcess` (NCP) 创建子进程；
virgl 运行时支持三种启动方式：NCP 子进程、OH_IPC 远程代理（`OHIPCRemoteProxy`）、
进程内 host（`StartVirglInProcessHostLocked`，phone 模式）。
Broker (`broker.cpp`) 中继 Wine 内部 `CreateProcess` → NCP 的转换，支持命名多 fd 和环境变量转发。

### 关键组件

| 组件 | 说明 |
|------|------|
| Box64 | x86_64 → ARM64 指令翻译，Dynarec 模式 |
| Broker | 中继 Wine CreateProcess → NCP，转发 env + fd |
| Wayland compositor | 嵌入式 compositor，在 HAP ARM64 进程中运行 |
| VirGL (fallback) | guest Mesa virpipe → vtest socket → virglrenderer → host EGL，zero-copy surface-queue present |
| DXVK/Venus (D3D11) | guest DXVK 1.10.3 → Wine Vulkan → Mesa Venus (vtest) → virglrenderer Venus → host Vulkan，`venus_surface_presenter` 上屏 |
| XKB 键盘 | xkeyboard-config 打包到 rawfile，XKB_CONFIG_ROOT 指向 |
| noexec 文件系统 | 可执行段用匿名 mmap + pread 替代文件映射 |
| dosdevices | symlink 不可用，四条代码路径硬编码 fallback |

### Wayland compositor 模块结构 (entry/src/main/cpp)

- `wayland_server.{h,cpp}` — display 生命周期、global 注册、toplevel 策略
  (RaiseToplevel / SetToplevel*)、事件派发；单例 WaylandServer 是各模块组装点
- `wl_core.cpp` — wl_compositor / wl_surface / wl_region / wl_subcompositor /
  wl_subsurface / wp_viewporter / wl_output 协议实现；`surface_commit` 按职责
  分段 (HandleNullBufferCommit → BeginShmAccess → ComputeContentArea →
  UpdateToplevelFrameOnCommit → CheckDesktopRootOnCommit →
  UpdateSubsurface(Layer)OnCommit / UpdatePopupOnCommit → FinishCommit)
- `xdg_shell.cpp` — xdg_wm_base / xdg_surface / xdg_toplevel 协议实现
- `compositor/` — owning classes，各管一摊状态（不变式见各类头注释）：
  - `toplevel_manager` — toplevel/popup 聚合状态 + z-order（唯一存放处）
  - `desktop_compositor` — 帧合成（root 帧为基底）+ zero-copy/subsurface layer
  - `input_resolver` — Desktop 模式输入命中裁决（全屏→层→toplevel→root）
  - `desktop_root_manager` — desktop root 识别/切换
  - `move_grab` — xdg_toplevel.move 交互式窗口移动
  - `display_policy.h` — PC/Desktop 模式差异的策略查询唯一入口（四类：
    事件派发 / subsurface / 渲染取帧 / 输入命中；phone 模式不经此，传输层隔离）
  - `geometry.{h,cpp}` — 保比例 letterbox 纯函数（`make test` 宿主单测覆盖）
  - `compositor_constants.h` / `compositor_utils.{h,cpp}` — 命名常量与启发式
  - `debug_assert.h` — MW_ASSERT 不变式断言（默认编译为空）
- `input_manager.cpp` / `seat.cpp` — 输入事件注入与 wl_seat
- `graphics_broker.cpp` — 图形后端管理（Virgl/Venus 选择、IPC 配置、`WINEHUA_*` 环境注入）
- `virgl_surface_presenter.cpp` — VirGL zero-copy 呈现（OH_NativeBuffer + external OES）
- `venus_surface_presenter.cpp` — Vulkan/DXVK 帧呈现（`venus-presenter` TAG）
- `egl_renderer.cpp` — CPU fallback 上屏（`TakeFrame` → glTexSubImage2D）

#### 日志纪律

- 协议层统一 TAG `WL_Server`（hilog 过滤粒度过粗，compositor 内部子模块靠消息前缀
  `[MW]` `[MW-POPUP]` `[MW-SUBSURF]` `[XDG]` 区分）；各独立模块有专属 TAG：
  `WL_Seat` `WL_Input` `WL_Xdg` `WL_EGL` `WL_GFX` `virgl-child` `venus-presenter`
  （TAG 速查见 `.claude/rules/build-and-log.md`）
- 每帧级日志必须降采样（serial % N 或仅状态变化时），禁止逐帧 INFO
- 诊断插桩随用随删，或单独 chore 提交，不留长期桩
- 重构原则与执行记录见 [archive/CPP_REFACTOR_PLAN.md](../archive/CPP_REFACTOR_PLAN.md)

---

## 3. 信号处理

- `arch_prctl(ARCH_SET_GS, teb)` → 设置 GS 段基址 = TEB
- `arch_prctl(ARCH_SET_FS, teb)` → 设置 FS 段基址
- Wine 信号处理器: SIGSEGV, SIGILL, SIGBUS, SIGFPE

## 4. wineserver I/O 循环

4 层 fallback: `epoll_pwait2` → `epoll_wait` → `kqueue` → `poll()`  
HarmonyOS 使用 epoll (Linux 内核)，`epoll_pwait2` 在 musl 上 stub 返回 ENOSYS 后自动 fallback。

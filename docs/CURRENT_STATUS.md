# Wine for HarmonyOS — 当前状态

> 更新: 2026-07-31
> 状态: ✅ DXVK (D3D11) | ✅ VirGL (OpenGL fallback) | ✅ Audio | ✅ Explorer 桌面 | ✅ 输入 | ✅ 多窗口

---

## 里程碑

### 1. ARM64 Pad Box64 .so 方案 ✅

Box64 编译为共享库 (box64.so)，由 NCP 子进程 (`wine_child.so`) dlopen 加载，通过 `box64_hmos_main()` 在同一进程内模拟执行 x86_64 Wine ELF。

### 2. NCP 子进程架构 ✅

wine、wineserver、virgl_test_server 全部通过 `OH_Ability_StartNativeChildProcess` (NCP) 创建。Broker (`broker.cpp`) 中继 Wine 进程内 `CreateProcess` → NCP 的转换，支持命名多 fd 和环境变量转发。

### 3. Wine prefix 初始化 ✅

`wineboot --init` 完整生成 drive_c 目录结构（windows/system32, users, Program Files 等）和 registry。支持 WoW64 (32-bit PE DLL)。

### 4. Explorer 桌面 ✅

`explorer /desktop=shell,<w>x<h>` 桌面窗口通过 Wayland compositor 渲染上屏。桌面生命周期管理（`winehua_keep` 持久化、退出不死锁）已完善。

### 5. 输入框架 ✅

鼠标/键盘事件 ArkTS → NAPI → InputManager → Wayland → Wine 完整链路。支持物理像素 → Wine 逻辑坐标映射。桌面模式输入命中裁决在 `compositor/input_resolver`（全屏→层→toplevel→root）。

### 6. 音频 ✅

Host-broker 音频引擎：Wine 侧通过 IPC + ring buffer 传输 PCM，宿主进程集中持有 `OH_AudioRenderer`。支持 WASAPI / DirectSound / waveOut / MCI / MIDI（含 winehua-gm.sf2 SoundFont）+ 麦克风 capture。详见 [AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md)。

### 7. VirGL / OpenGL ✅（现为 fallback）

guest Mesa (virpipe Gallium driver) → Unix socket → virgl_test_server (NCP 子进程) → virglrenderer → host EGL。zero-copy 显示路径已落地（`virgl_texture+surface_queue+external_oes`，见 [OPENGL_VIRGL_DESIGN.md](OPENGL_VIRGL_DESIGN.md)）。DXVK 稳定后 WineD3D/VirGL 为显式 fallback。

### 8. DXVK profiles ✅

guest DXVK (D3D11) → Wine Vulkan → Mesa Venus (vtest) → virglrenderer Venus → host Vulkan present。Maleoon 920 使用已经完成 WineHua 适配并经过真实 D3D11 游戏验证的 DXVK 2.6.2，其已观察兼容性不低于 1.10.x；DXVK 1.10.3 保留为 910/Vulkan 1.2 设备的稳定回退。DXVK 选档与 VKD3D 选档相互独立，不能因 VKD3D descriptor 门槛而降低 920 的 D3D11 路径。详见 [DXVK_MODERN_UPGRADE_READINESS.md](DXVK_MODERN_UPGRADE_READINESS.md)。

### 9. Compositor 状态重构完成 ✅

`compositor/` 子目录 owning-class 拆分（toplevel_manager / desktop_compositor / input_resolver / desktop_root_manager / move_grab / display_policy），不变式集中管理。复盘见 [archive/CPP_REFACTOR_PLAN.md](archive/CPP_REFACTOR_PLAN.md)。

---

## 当前架构

```
NCP 子进程 (appspawn)
  wine_child.so: Main() / WineserverMain()
    ├─ arm64: dlopen("box64.so") → box64_hmos_main() → Wine x86_64 ELF
    └─ x86_64: dlopen("ntdll.so") → __wine_main()
    └─ 渲染路径（D3D11 应用）:
         DXVK 2.6.2 (920) / 1.10.3 fallback (guest) → winevulkan → Mesa Venus → vtest socket
         → virglrenderer Venus → host Vulkan → venus_surface_presenter → XComponent
    └─ 渲染路径（OpenGL 应用, fallback）:
         Wine OpenGL → guest Mesa virpipe → vtest socket
         → virgl_test_server → virglrenderer → host EGL → virgl_surface_presenter (zero-copy)

  virgl_child.so: VirglTestServerMain()（含 IPC 远程代理与进程内 host 模式）

嵌入式 Wayland compositor (ARM64 原生, HAP 内)
  └─ desktop_compositor 帧合成 → egl_renderer / surface presenters → XComponent 上屏
```

- Broker (`broker.cpp`) 中继 Wine 进程内 `CreateProcess` → NCP，转发环境变量
- 运行时打包：`wine-data.zip` 内含 dxvk/legacy、guest_vulkan、host_vulkan、smoke、audio 等（见 [BUILD_GUIDE.md](BUILD_GUIDE.md)）

---

## 已修复问题

| # | 问题 | 修复方案 |
|---|------|---------|
| 1 | TEB 分配崩溃 | `anon_mmap_alloc()` fallback |
| 2 | Noexec 文件系统 PROT_EXEC | 匿名 mmap + pread（现为 `ohos_map_exec_section` + JIT enable） |
| 3 | prctl(0x6a6974) SIGSEGV | Box64 my_prctl 清零 r10/r8 |
| 4 | PR_SET_NAME fallthrough SIGSEGV | Box64 my_prctl 直接 return 0 |
| 5 | Box64 fork 子进程 RWX 失败 | NCP appspawn + .so dlopen 替代 fork |
| 6 | wineserver ARM64 架构不匹配 | ARM64 下编 x86_64 PIE，Box64 加载 |
| 7 | entryParams `\|wine\|` 多余 | `USE_LIBBOX64` 环境变量按需跳过 |
| 8 | Box64 DEBUG 日志 I/O 过慢 | BOX64_LOG=0, WINEDEBUG=-all |
| 9 | dosdevices symlink 不可用 | 四条代码路径 fallback |
| 10 | XKB 键盘数据缺失 | xkeyboard-config 打包 + XKB_CONFIG_ROOT |
| 11 | 鼠标 action 常量错误 | 对齐 ArkTS MouseAction |
| 12 | dxg_surface use-after-free | wl_resource destroy 回调 + pending auto-destroy |
| 13 | 多窗口 XComponent exports 冲突 | surfaceId + XComponentController |
| 14 | dnsapi / nsiproxy musl 编译错误 | musl 适配完成，DLL 正常构建（不再跳过） |
| 15 | executable PE section 保护缺失 | 恢复 section protection（c31c2a3） |
| 16 | OHOS 系统 DLL 解析失败 (Crysis3 0xC0000135) | 恢复 `C:\windows\system32` 搜索路径（db9ade1） |
| 17 | Tomb Raider 暗色渲染 (RGBA8 SNORM) | `DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto` 默认启用（b2113e3） |
| 18 | steam_api64.dll AV (Crysis3) | Box64 `BOX64_DYNAREC_SAFEFLAGS=1` 兼容默认（2adb3af） |
| 19 | broker env 序列化缺失 | 串行化 wine 程序环境（f51fdcd） |
| 20 | RunWineExe/RunWineProgram 未接入桌面 | 桌面模式进程接入 shell desktop（39dbd7d/68ddea3） |
| 21 | 桌面退出卡死 | 退出后关闭 DesktopAbility（12c30c9） |
| 22 | 桌面持久化缺失 | `winehua_keep` 方案（b9e37b0） |
| 23 | 双击误触发 explorer 打开+重命名 | 修复多发送一次点击（6d64109） |

---

## 已知问题

| 问题 | 影响 | 说明 |
|------|------|------|
| services.exe 无法启动 | 非阻塞 | 错误 267 (目录无效) |
| Explorer 启动的 conhost 崩溃 | console 程序无法从桌面启动 | Box64 Signal 3 (SIGTRAP)；可从文件选择器启动 |
| Box64 dlopen 原生 .so 受限 | 部分依赖 | libfreetype/xkbcommon/xml2 在 Box64 下查找问题（原 U16），非致命，多数游戏已验证可用 |
| 60 分钟 DXVK 长稳门禁暂停 | 未完成 | Phase 2 剩余工作之一（07-30 起由用户指示暂停） |

---

## 构建命令

```bash
# arm64
make NATIVE_ARCH=arm64-v8a

# x86_64
make NATIVE_ARCH=x86_64
```

完整命令与增量构建说明见 [BUILD_GUIDE.md](BUILD_GUIDE.md) 与 `.claude/rules/build-and-log.md`。

---

## 相关文档

- [README.md](./README.md) — 文档索引
- [ARCHITECTURE.md](./ARCHITECTURE.md) — 架构设计
- [PHASE2_DXVK_STATUS_MEMO.md](./PHASE2_DXVK_STATUS_MEMO.md) — DXVK 活文档
- [BUILD_GUIDE.md](./BUILD_GUIDE.md) — 构建指南
- [.claude/rules/submodule-workflow.md](../.claude/rules/submodule-workflow.md) — Submodule 管理方案


### 10. VKD3D-Proton 2.6 limited-500K official D3D12 profile

The recorded Maleoon 910 qualification is now integrated as the product D3D12
default. The qualified 910 session loads VKD3D-Proton 2.6 for d3d12.dll and
DXVK 1.10.3 for d3d11.dll and dxgi.dll because 910 cannot admit DXVK 2.6.2.
This is not a version dependency: on 920, D3D11 remains on the adapted,
game-validated DXVK 2.6.2 path while D3D12 independently uses the qualified
VKD3D profile. The VKD3D profile is x64-only, caps shader-visible resource
descriptors at 500,000, and does not advertise Query Meta support. The official
VKD3D triangle and gears demos rendered successfully on 910, alongside the
descriptor, Gate C, BDA, multi-queue, physical-present, and DXVK regression
evidence.

Normal UI and `game` launches inherit this mixed product default and use the
qualified precise mapped-memory contract without Gate C, ring-notify, or
persistent-map trace output. The manual DX12 smoke and automated Gate C runs
select `shadow-precise-direct-fence` explicitly when those diagnostics are
required.

A capability, host-driver, Mesa/Venus, or Wine runtime change requires the
capability audit and real-device gates to be repeated before shipping a new
runtime payload.

### Revalidation checkpoint (2026-08-06)

The persistent-map synchronization change requires a fresh Gate C validation
before real game testing. Two clean uninstall/reinstall runs on Maleoon 910
reached the readback compare stage but failed with
`buffer_readback_mismatch` (`offset=0 expected=11 actual=0`); no frame count was
credited. The VKD3D `triangle.exe` and `gears.exe` demos still rendered, and the
clean-prefix DXVK Legacy x86/x64 plus cube regression remained passing. The
readback synchronization direction is therefore still under investigation,
and the product must remain on the isolated branch with no game qualification
or remote push until accurate smoke passes again.

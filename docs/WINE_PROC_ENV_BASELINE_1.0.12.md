# WineHua 1.0.12 进程启动参数与环境变量基线 (采集对照)

> 采集时间: 2026-08-30. 设备: 192.168.1.8:33363 (arm64 Pad).
> 位点: main-ui 1.0.12 (`68f90d1`) entry + 快照采集补丁 (wine_child.cpp/virgl_child.cpp dump_proc_snapshot);
> wine 等第三方为当前构建 (用户指令"只打包 hap, 不重新构建其它")。
> 对比对象: 90edaae 基线见 docs/WINE_PROC_ENV_BASELINE.md。

## 1.0.12 d3d cube 完整环境变量 (pid 53199, 帧率高)

argv: `box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d_switch_cube.exe`

```
box64 /data/storage/el2/base/files/wine/bin/wine C:\smoke\x64\winehua_d3d_switch_cube.exe
```

`USER=100`
`SHLVL=1`
`HOME=/storage/Users/currentUser/Download/app.hackeris.winehua/`
`OLDPWD=/`
`TERM=ansi`
`PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
`LANG=zh_CN.UTF-8`
`PWD=/storage/Users/currentUser`
`MALI_REPORT_MEM_USAGE=1`
`UBSAN_OPTIONS=print_stacktrace=1:print_module_map=2:log_exe_name=1`
`DOWNLOAD_CACHE=/data/cache`
`TMPDIR=/data/storage/el2/base/cache`
`PULSE_STATE_PATH=/data/data/.pulse_dir/state`
`PULSE_RUNTIME_PATH=/data/data/.pulse_dir/runtime`
`TMP=/data/local/mtp_tmp/`
`OHOS_SOCKET_NativeSpawn=18`
`HNP_PRIVATE_HOME=/data/app`
`HNP_PUBLIC_HOME=/data/service/hnp`
`SHELL=/bin/sh`
`LD_PRELOAD=libappspawn_helper.z.so`
`APPSPAWN_FD_wine_audio_bootstrap=28`
`hwasanEnabled=0`
`tsanEnabled=0`
`ubsanEnabled=0`
`HAP_DEBUGGABLE=true`
`AppSpawnCheckUnexpectedExitCall=53199`
`PROCESS_START_TIME=86397887`
`LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix`
`BOX64_LD_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_vulkan/lib:/data/storage/el2/base/files/wine/bin/guest_gfx/lib:/data/storage/el2/base/files/wine/bin:/data/storage/el2/base/files/wine/bin/x86_64-unix:/data/storage/el2/base/files/wine/lib/x86_64`
`XDG_RUNTIME_DIR=/data/storage/el2/base/files/.wine`
`WAYLAND_DISPLAY=wine-wayland`
`WINEPREFIX=/data/storage/el2/base/files/.wine`
`PROCESSBROKER=/data/storage/el2/base/files/.wine/../.wine_broker`
`WINEDATADIR=/data/storage/el2/base/files/wine/bin/../share/wine`
`XKB_CONFIG_ROOT=/data/storage/el2/base/files/wine/bin/../share/X11/xkb`
`WINEBINDIR=/data/storage/el2/base/files/wine/bin`
`WINEUNIXDIR=/data/storage/el2/base/files/wine/bin`
`WINEDLLDIR=/data/storage/el2/base/files/wine/bin/x86_64-unix`
`WINEDLLDIR0=/data/storage/el2/base/files/wine/dxvk/legacy/x64`
`WINEDLLDIR1=/data/storage/el2/base/files/wine/dxvk/legacy/x86`
`WINEDLLDIR2=/data/storage/el2/base/files/wine/bin`
`WINEDLLPATH=/data/storage/el2/base/files/wine/dxvk/legacy/x64:/data/storage/el2/base/files/wine/dxvk/legacy/x86:/data/storage/el2/base/files/wine/bin/x86_64-windows:/data/storage/el2/base/files/wine/bin/i386-windows:/data/storage/el2/base/files/wine/bin`
`BOX64_LOG=0`
`BOX64_NOBANNER=1`
`BOX64_SHOWSEGV=1`
`BOX64_DYNAREC_SAFEFLAGS=1`
`BOX64_DYNAREC_BIGBLOCK=3`
`BOX64_DYNAREC_CALLRET=2`
`BOX64_DYNAREC_FORWARD=1024`
`BOX64_DYNAREC_WEAKBARRIER=0`
`BOX64_AVX=0`
`BOX64_AES=0`
`BOX64_PCLMULQDQ=0`
`BOX64_DYNAREC_VOLATILE_METADATA=0`
`USE_LIBBOX64=1`
`MIDI_SOUNDFONT_PATH=/data/storage/el2/base/files/wine/bin/../audio/winehua-gm.sf2`
`WINEDEBUG=-all`
`GST_PLUGIN_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
`GST_PLUGIN_SYSTEM_PATH=/data/storage/el2/base/files/wine/bin/x86_64-unix/gstreamer-1.0`
`WINEHUA_DESKTOP_MODE=1`
`WINEWAYLAND_ENTER_SILENT=1`
`WINEHUA_GRAPHICS_BACKEND=virgl`
`WINEHUA_GRAPHICS_ACTIVE=virgl`
`WINEHUA_SHM_FALLBACK=0`
`WINEHUA_FRAME_ZERO_COPY=1`
`WINEHUA_FRAME_TRANSPORT=virgl_texture+surface_queue+external_oes`
`WINEHUA_GUEST_GFX_READY=1`
`WINEHUA_GUEST_GFX_MODE=mesa-virpipe`
`WINEHUA_GUEST_GFX_DIR=/data/storage/el2/base/files/wine/bin/guest_gfx`
`WINEHUA_VIRGL_SOCKET_READY=1`
`WINEHUA_VIRGL_LIBRARY_READY=1`
`WINEHUA_VIRGL_SOCKET=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
`WINEHUA_VIRGLRENDERER_LIB=libvirglrenderer.so`
`WINEHUA_VIRGL_READY=1`
`WINEHUA_VULKAN_PRESENT=1`
`EGL_PLATFORM=wayland`
`WINEHUA_EGL_LIBRARY_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/libEGL.so`
`LIBGL_DRIVERS_PATH=/data/storage/el2/base/files/wine/bin/guest_gfx/lib/dri`
`WINEHUA_WAYLAND_READBACK=1`
`WINEHUA_GL_STALL_DIAG=1`
`WINEHUA_DISPLAY_FPS_FILE=C:\windows\temp\winehua_display_fps.txt`
`WINEHUA_VTEST_FRONTBUFFER_LOG=/data/storage/el2/base/temp/winehua_vtest_frontbuffer.log`
`WINEHUA_VTEST_PRESENT=surface-queue`
`WINEHUA_ZERO_COPY_READY_DIR=/data/storage/el2/base/cache`
`WINEHUA_GUEST_GFX_PLATFORM=wayland`
`LIBGL_ALWAYS_SOFTWARE=1`
`MESA_LOADER_DRIVER_OVERRIDE=swrast`
`GALLIUM_DRIVER=virpipe`
`VTEST_SOCKET_NAME=/data/storage/el2/base/files/.wine/graphics/virgl.sock`
`WINEHUA_DXVK_ROOT=/data/storage/el2/base/files/wine/dxvk/legacy`
`WINEHUA_DXVK_PROFILE=legacy`
`WINEHUA_DXVK_VERSION=1.10.3`
`WINEHUA_DXVK_RELAXED_FEATURES=1`
`WINEHUA_VULKAN_RUNTIME=1`
`WINEHUA_VULKAN_LOADER_ARCH=x86_64`
`WINEHUA_VENUS_ICD_ARCH=x86_64`
`BOX64_EMULATED_LIBS=libvulkan.so:libvulkan.so.1:libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:libwayland-client.so:libwayland-client.so.0:libwayland-server.so:libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:libglib-2.0.so:libglib-2.0.so.0:libgobject-2.0.so:libgobject-2.0.so.0:libgio-2.0.so:libgio-2.0.so.0:libgmodule-2.0.so:libgmodule-2.0.so.0:libgstreamer-1.0.so:libgstreamer-1.0.so.0:libgstbase-1.0.so:libgstbase-1.0.so.0:libgstvideo-1.0.so:libgstvideo-1.0.so.0:libgstaudio-1.0.so:libgstaudio-1.0.so.0:libgsttag-1.0.so:libgsttag-1.0.so.0:libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:libgstallocators-1.0.so:libgstallocators-1.0.so.0:libgstapp-1.0.so:libgstapp-1.0.so.0:libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:libgstfft-1.0.so:libgstfft-1.0.so.0:libgstnet-1.0.so:libgstnet-1.0.so.0:libgstriff-1.0.so:libgstriff-1.0.so.0:libgstrtp-1.0.so:libgstrtp-1.0.so.0:libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:libgstsdp-1.0.so:libgstsdp-1.0.so.0:libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:libxml2.so:libxml2.so.2:libz.so:libz.so.1`
`VK_DRIVER_FILES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
`VK_ICD_FILENAMES=/data/storage/el2/base/files/wine/bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json`
`VN_DEBUG=vtest`
`VN_PERF=no_fence_feedback,no_query_feedback`
`WINEDLLOVERRIDES=d3d11=n;dxgi=n`
`DXVK_WINEHUA_COMMAND_QUERY_RESET=1`
`DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`
`DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto`
`DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1`
`VN_WINEHUA_REMOTE_MEMORY_SYNC=1`
`LC_ALL=zh_CN.UTF-8`
`WINEHUA_D3D_BACKEND=dxvk_legacy`
`WINEHUA_PRESENT_BACKEND=venus_broker_present`
`WINEHUA_AUTOMATION=0`
`WINEHUA_DESKTOP=shell`
`WINEHUA_WINE_UNIX_ARCH=x86_64`
`WINEHUA_HOST_ARCH=aarch64`
`WINEHUA_WORKING_DIRECTORY=C:\smoke\x64`
`WINE_OHOS_AUDIO_ENABLE=1`
`WINE_OHOS_AUDIO_BOOTSTRAP_FD=28`
`WINE_OHOS_AUDIO_PROTOCOL_VERSION=1`

## 帧率差异判定矩阵 (四样本)

| 样本 | 路由 | 机制组 (venus 影子/强环 + vkd3d 混合) | DXVK | 帧率 |
|---|---|---|---|---|
| A pid 32079 (90edaae 应用库) | vkd3d_limited_500k 混合 | 有 | modern-2.6 (2.6.2) | 高 |
| B pid 62819 (90edaae 应用库+legacy档) | vkd3d_limited_500k 混合 | 有 | legacy 1.10.3 | 低 |
| C pid 33509 (90edaae 文件管理) | vkd3d_limited_500k 混合 | 有 | legacy 1.10.3 | 低 |
| D pid 53199 (1.0.12 应用库) | 纯 dxvk_legacy | **无** | legacy 1.10.3 | **高** |

**隔离因子结论**：
- 低帧率 = **vkd3d_limited_500k 混合路由与 venus 影子内存/强环机制组 (VN_WINEHUA_STRONG_RING_BARRIER=1 /
  PERSISTENT_MAP_SYNC / DIRECT_FENCE_WAIT / VKR_WINEHUA_SHADOW_FROM_HOST=precise /
  FORCE_COHERENT_MAP_SYNC / d3d12=n;dxgi=n;d3d11=n) × DXVK legacy 1.10.3**，两条件同真才低。
- A (机制组+modern-2.6) 高: DXVK 2.6.2 与影子机制设计契合;
- D (无机制组+legacy 1.10.3) 高: 经典 vtest/socket 直传路径与 1.10.3 经典语义无冲突;
- legacy 兼容五件套 (BATCH_MAPPED_FLUSH / FLUSH_DYNAMIC_MAPPED / COMMAND_QUERY_RESET /
  EMULATE_RGBA8_SNORM_RT / RELAXED_FEATURES) 在 D 同样存在且帧率高 → **排除了它们单独作为主因**;
- MANGOHUD / BOX64_SYSINFO / VN_PERF 长短组合 / 启动路径 (应用库 vs 文件管理) 均被 1.0.12 对照排除。

**机理（推断）**: vkd3d 混合路由把 DXVK 引入 venus 影子内存新机制 (precise shadow + 强环 barrier +
direct fence wait) —— 这套机制与 DXVK 1.10.3 的经典上传/同步语义不匹配, 每次资源更新需 host
侧同步/恢复 → 慢路径; DXVK 2.6.2 专为此类机制适配 (全新 ring/upload 路径) → 快; 1.0.12 纯 DXVK
路由不上机制组, 走经典直传 → 快。

**修正**: 版本差异分析中 "legacy 档=主因" 为错误的中间结论, 由 1.0.12 (legacy+无机制组=高帧率)
推翻; 主因修正为 "vkd3d 混合路线 (机制组) × DXVK 1.10.3" 的组合失配。

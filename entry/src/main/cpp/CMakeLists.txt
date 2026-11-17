cmake_minimum_required(VERSION 3.5.0)
project(WineHua)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include("${NATIVERENDER_ROOT_PATH}/cmake/WaylandProtocols.cmake")

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/protocols
    # 合成器按域分组 (frame/toplevel/input) + 协议 C 头 (protocols/): 域内互引
    # 保持裸名 include 经本路径解析。cpp 根下各功能域 (wine/proc/graphics/…)
    # 的新头不注册到此 — 跨域引用一律显式域前缀 (#include "wine/wine_env.h"),
    # 裸名只在同域内合法。同目录优先搜索不变。
    ${NATIVERENDER_ROOT_PATH}/compositor/frame
    ${NATIVERENDER_ROOT_PATH}/compositor/toplevel
    ${NATIVERENDER_ROOT_PATH}/compositor/input
)

# WINEHUA_WINE_ARCH_IS_X86_64: 方案② (arm64 设备 + x86_64 wine + box64 转译) 的
# 编译期标记。由 package.sh 写入 entry/.wine_arch 的文件读取 (hvigor daemon 环境快照
# 丢失 make 的 WINE_ARCH env, ENV{WINE_ARCH} 不可靠 — 实测 daemon 里读不到)。
# 方案③ (arm64 原生, .wine_arch=aarch64) 不定义 → C++ 走原生 wine 路径。
# 注意: 方案① (x86_64 设备) 时 .wine_arch=x86_64 也定义此宏, 但 __aarch64__ 未定义,
# C++ 侧 `__aarch64__ && WINEHUA_WINE_ARCH_IS_X86_64` 组合仍准确锁定方案②。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../.wine_arch")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../../.wine_arch" _wine_arch)
    string(STRIP "${_wine_arch}" _wine_arch)
    if(_wine_arch STREQUAL "x86_64")
        add_definitions(-DWINEHUA_WINE_ARCH_IS_X86_64)
    endif()
endif()

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 预编译 ARM64 wayland-server (放在 libs/ 下，hvigor 自动打包)
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})

# 显示路线 wlroots (静态库) — 构建产物在 build/host-ext (纪律: build/ 内唯一落点),
# 其 NEEDED 闭包 .so 由 assemble.sh 装配进 libs/ (打包视图)
# 注: NATIVERENDER_ROOT_PATH = entry/src/main/cpp, 仓库根在四级上
# Allow a read-only dependency install outside Hvigor's disposable build tree.
set(VPP_HOST_EXT_ROOT "$ENV{VPP_HOST_EXT_ROOT}" CACHE PATH "Existing host dependency install")
if(NOT VPP_HOST_EXT_ROOT)
    set(VPP_HOST_EXT_ROOT ${NATIVERENDER_ROOT_PATH}/../../../../build/host-ext)
endif()
set(WLR_DIR ${VPP_HOST_EXT_ROOT}/${OHOS_ARCH})
set(WLR_STATIC ${WLR_DIR}/usr/lib/libwlroots-0.20.a)
set(WLR_INC ${WLR_DIR}/usr/include/wlroots-0.20)
set(WLR_PIXMAN_INC ${WLR_DIR}/usr/include/pixman-1)
# xwayland.h 需要 xcb/xcb.h 等 (host-ext usr/include)
set(WLR_XCB_INC ${WLR_DIR}/usr/include)

add_library(entry SHARED
    display/x11_external_host.cpp
    display/display_session.cpp
    display/display_compositor.cpp
    display/display_guest_frames.cpp
    display/x11_toplevel.c
    display/x11_toplevel_bridge.cpp
    display/ohos_buffer.cpp
    display/ohos_output.c
    display/ohos_egl_import_probe.c
    display/ohos_egl_import.c
    display/display_input.c
    display/xim_bridge.c
    # ---- Portable runtime support ----
    common/app_log.cpp
    audio/audio_pcm_capture.cpp
    audio/audio_pcm_metrics.cpp
    graphics/graphics_profile.cpp
    graphics/host_vulkan_probe.cpp
    # ---- WineHua proton 方案③ 基线 ----
    direct/vulkan_probe_launcher.cpp
    direct/direct_wine_ipc_probe.cpp
    direct/direct_wine_surface_ipc_probe.cpp
    direct/direct_wine_surface_controller.cpp
    direct/direct_desktop_compositor.cpp
    direct/direct_vulkan_desktop_compositor.cpp
    direct/direct_surface_probe_launcher.cpp
    direct/direct_buffer_import_probe.cpp
    direct/direct_vulkan_context.cpp
    direct/direct_vulkan_presenter.cpp
    direct/direct_shared_buffer_probe.cpp
    direct/native_buffer_socket.cpp
    direct/native_buffer_parcel.cpp
    direct/direct_output_probe.cpp
    audio/audio_broker.cpp
    audio/audio_ipc_server.cpp
    audio/audio_stream.cpp
    graphics/graphics_broker.cpp
    graphics/gl_capability_probe.cpp
    graphics/virgl_host_config.cpp
    input/game_controller_bridge.cpp
    input/controller/controller_hub.cpp
    input/controller/controller_napi.cpp
    input/controller/controller_runtime.cpp
    input/controller/gamepad_bridge.cpp
    input/controller/physical_gamepad.cpp
    bridge/napi_init.cpp
    bridge/runtime_compat.cpp
    smoke/smoke_napi.cpp
    bridge/system_open.cpp
    proc/wine_process.cpp
    wine/wine_env.cpp
    wine/env_spec.cpp
    wine/env_profiles.cpp
    wine/dll_overrides.cpp
    wine/container_session.cpp
    proc/spawner.cpp
    wine/wine_launch.cpp
    wine/wine_exe.cpp
    proc/broker.cpp
    proc/wine_spawn_compatibility.cpp
    proc/wine_child_ipc_launcher.cpp
    proc/ncp_dispatch.cpp
    proc/cef_utility_probe.cpp
    common/perf_utils.cpp
    phone_adapter/phone_process.cpp
    phone_adapter/phone_virgl_relay.cpp
    compositor/wayland_server.cpp
    compositor/wl_core.cpp
    compositor/frame/compositor_blit.cpp
    compositor/toplevel/toplevel_manager.cpp
    compositor/toplevel/popup_manager.cpp
    compositor/toplevel/move_grab.cpp
    compositor/frame/compositor_utils.cpp
    compositor/frame/geometry.cpp
    compositor/toplevel/desktop_compositor.cpp
    compositor/frame/zc_bridge.cpp
    compositor/frame/frame_composer.cpp
    compositor/frame/frame_pipeline.cpp
    compositor/frame/shm_frame_source.cpp
    compositor/input/input_resolver.cpp
    compositor/input/input_space_mapper.cpp
    compositor/input/input_queue.cpp
    compositor/input/input_state_tracker.cpp
    compositor/input/input_injector.cpp
    compositor/toplevel/desktop_root_manager.cpp
    compositor/toplevel/toplevel_event_bus.cpp
    graphics/egl_renderer.cpp
    common/fs_utils.cpp
    bridge/plugin_manager.cpp
    common/process_utils.cpp
    common/ring_buffer.cpp
    input/seat.cpp
    input/data_device.cpp
    graphics/shader_utils.cpp
    common/string_utils.cpp
    input/input_manager.cpp
    input/pointer_extras.cpp
    compositor/xdg_shell.cpp
    compositor/winehua_toplevel.cpp
    input/text_input.cpp
    ${VPP_WAYLAND_PROTOCOLS}/xdg-shell-protocol.c
    ${VPP_WAYLAND_PROTOCOLS}/winehua-toplevel-protocol.c
    protocols/viewporter-server-protocol.c
    protocols/pointer-warp-v1-protocol.c
    protocols/pointer-constraints-unstable-v1-protocol.c
    protocols/relative-pointer-unstable-v1-protocol.c
    protocols/text-input-unstable-v3-protocol.c
)

# 应用日志文件 sink：force-include app_log.h，让所有 OH_LOG_* 调用在保持
# hilog 行为不变的同时追加写入 Download/.../logs/native-YYYYMMDD.log。
target_compile_options(entry PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/common/app_log.h)
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. rev-parse --short=12 HEAD
    OUTPUT_VARIABLE WINEHUA_AUDIO_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT WINEHUA_AUDIO_GIT_COMMIT)
    set(WINEHUA_AUDIO_GIT_COMMIT "unknown")
endif()
target_compile_definitions(entry PRIVATE WINEHUA_AUDIO_GIT_COMMIT="${WINEHUA_AUDIO_GIT_COMMIT}")

# BlitScaled (双线性缩放 blit) 是桌面合成热路径: 全屏游戏 800x600→1400x920
# fit 放大在 -O0 调试构建实测 ~70ms/帧 (440 万输出像素 × 4 采样插值)。
# 单独 -O2 该源文件 (其余保持 -O0 便于调试), 合成崩溃时该文件仍可栈回溯。
# 注意: compositor_blit.cpp 迁移自 compositor_utils.cpp (2026-08-22 拆分),
# -O2 属性必须跟着代码走 — 漏挂曾致 blit 段从 26ms 劣化到 ~240ms。
set_source_files_properties(compositor/frame/compositor_blit.cpp PROPERTIES COMPILE_OPTIONS "-O2")

target_include_directories(entry PRIVATE ${WLR_INC} ${WLR_PIXMAN_INC} ${WLR_XCB_INC})

target_link_libraries(entry PUBLIC
    libuv.so
    ${WAYLAND_LIBS_DIR}/libX11.so.6
    # wlroots 静态库 + 其 NEEDED 闭包 (顺序敏感: .a 在前, 依赖 .so 在后)。
    # start-group 防静态库内部 member 依赖环 (output.c.o→allocator.c.o,
    # 单遍扫描报 undefined wlr_allocator_init, T8 实测)
    -Wl,--start-group
    ${WLR_STATIC}
    -Wl,--end-group
    ${WAYLAND_LIBS_DIR}/libwayland-client.so.0
    ${WAYLAND_LIBS_DIR}/libxkbcommon.so.0
    ${WAYLAND_LIBS_DIR}/libpixman-1.so.0
    ${WAYLAND_LIBS_DIR}/libdrm.so.2
    ${WAYLAND_LIBS_DIR}/libxcb.so.1
    ${WAYLAND_LIBS_DIR}/libxcb-composite.so.0
    ${WAYLAND_LIBS_DIR}/libxcb-ewmh.so.2
    ${WAYLAND_LIBS_DIR}/libxcb-icccm.so.4
    ${WAYLAND_LIBS_DIR}/libxcb-render.so.0
    ${WAYLAND_LIBS_DIR}/libxcb-res.so.0
    ${WAYLAND_LIBS_DIR}/libxcb-xfixes.so.0
    libohhidebug.so
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libnative_image.so
    libnative_buffer.so
    libnative_vsync.so
    libEGL.so
    libGLESv3.so
    libvulkan.so
    libhilog_ndk.z.so
    libchild_process.so
    libipc_capi.so
    libohaudio.so
    libnative_window_manager.so
    ${WAYLAND_LIBS_DIR}/libwayland-server.so.0
)

add_library(wine_child SHARED
    proc/wine_child.cpp
    proc/wine_child_ipc.cpp
)
target_link_libraries(wine_child PUBLIC libhilog_ndk.z.so libchild_process.so libipc_capi.so libnative_window.so)

# D0 is an independent native NCP. It resolves the system Vulkan loader with
# dlopen at runtime and deliberately does not link Wine, Mesa or libvulkan.
option(WINEHUA_BUILD_RUNTIME_PROBES "Build separate diagnostic child libraries" OFF)
if(WINEHUA_BUILD_RUNTIME_PROBES)
add_library(direct_vulkan_probe SHARED
    direct/native_buffer_probe.cpp
    direct/ncp_vulkan_probe.cpp
)
target_link_libraries(direct_vulkan_probe PUBLIC
    libhilog_ndk.z.so
    libchild_process.so
    libipc_capi.so
)

# Phone P0: shared image memory and sync fds, no remote GPU command stream.
add_library(direct_shared_buffer_probe SHARED
    direct/direct_shared_buffer_child.cpp
    direct/direct_native_read_watchdog.cpp
    direct/native_buffer_socket.cpp
    direct/native_buffer_parcel.cpp
)
target_link_libraries(direct_shared_buffer_probe PUBLIC
    libhilog_ndk.z.so libchild_process.so libipc_capi.so
    libnative_window.so libnative_buffer.so libvulkan.so
)

# D1 BufferQueue handoff remains isolated from the Wine/VirGL renderer.
add_library(direct_surface_probe SHARED
    direct/direct_surface_probe_child.cpp
)
target_link_libraries(direct_surface_probe PUBLIC
    libhilog_ndk.z.so
    libchild_process.so
    libipc_capi.so
    libnative_window.so
    libnative_buffer.so
)

# D2 WSI experiment: a separate Vulkan producer NCP uses the D1 parcel contract.
add_library(direct_gpu_surface_probe SHARED
    direct/direct_gpu_surface_probe_child.cpp
)
target_link_libraries(direct_gpu_surface_probe PUBLIC
    libhilog_ndk.z.so libchild_process.so libipc_capi.so
    libnative_window.so libnative_buffer.so libvulkan.so
)

# Shared OHOS NativeBuffer diagnostic, independent of Steam or GNU userspace.
add_library(winehua_native_memory_probe SHARED
    ${CMAKE_CURRENT_LIST_DIR}/../../../../scripts/diagnostics/native_buffer_memory_probe.c)
target_compile_options(winehua_native_memory_probe PRIVATE -Wall -Wextra -Werror)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(winehua_native_memory_probe PRIVATE -Wno-unused-command-line-argument)
endif()
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../../../../scripts/diagnostics/native_buffer_memory_probe.c
    PROPERTIES COMPILE_DEFINITIONS WINEHUA_PROBE_EMBEDDED=1)
target_link_libraries(winehua_native_memory_probe PRIVATE libdl.so libnative_buffer.so libnative_window.so)
endif()

add_library(virgl_child SHARED
    graphics/virgl_child.cpp
    graphics/virgl_host_config.cpp
    graphics/virgl_surface_presenter.cpp
    graphics/venus_surface_presenter.cpp
    graphics/native_window_vk_target.cpp
    graphics/shader_utils.cpp
    phone_adapter/phone_virgl_dispatch.cpp
)
target_link_libraries(virgl_child PUBLIC
    libnative_buffer.so
    libEGL.so
    libGLESv3.so
    libvulkan.so
    libnative_window.so
    libhilog_ndk.z.so
    libchild_process.so
    libipc_capi.so
)

# Xwayland NCP shim: 收父进程命名 fd 五连 + entryParams, dlopen
# libxwayland_ohos.so 调 main (spec R-SPAWN; 入口形态同 wine_child)
add_library(xwayland_child SHARED
    display/ncp/xwayland_child.cpp
)
target_link_libraries(xwayland_child PUBLIC libhilog_ndk.z.so libchild_process.so)

# mini X client NCP 子进程 (M0-T9 出口件): XOpenDisplay 自行连 Xwayland,
# 无命名 fd。依赖面恰 = winex11 硬依赖 (libX11+libXext ONLY, 范围决议
# 2026-09-27); libX11.so.6 由 assemble.sh 装进打包视图, 运行时同目录解析
add_library(xclient_child SHARED
    display/ncp/xclient_child.cpp
)
# X11 头在 host-ext usr/include (T4 libX11 安装位); 链接面仍只有 libX11/libXext
target_include_directories(xclient_child PRIVATE ${WLR_XCB_INC})
target_link_libraries(xclient_child PUBLIC
    libhilog_ndk.z.so
    libchild_process.so
    ${WAYLAND_LIBS_DIR}/libX11.so.6
    ${WAYLAND_LIBS_DIR}/libXext.so.6
)

# XIM server NCP 子进程 (XIM spec §3 NCP 承载): X 连接面同 xclient_child
# (与 Xwayland 同 namespace; 主进程 XOpenDisplay 已实测挂起)。通道子端经
# 命名 fd "xim_fd" 传入 (xwayland 五连同款机制)。
add_library(xim_server_child SHARED
    display/ncp/xim_server_child.cpp
)
target_include_directories(xim_server_child PRIVATE ${WLR_XCB_INC})
target_link_libraries(xim_server_child PUBLIC
    libhilog_ndk.z.so
    libchild_process.so
    ${WAYLAND_LIBS_DIR}/libX11.so.6
    ${WAYLAND_LIBS_DIR}/libXext.so.6
)

# display-route 控制面的产品入口 (D21): 桌面窗/启动链/IME 桥的 ArkTS 调用面。
# 符号来自 entry (同进程运行时解析), 必须与 entry 同包。
add_library(display_route_napi SHARED
    display_route/display_route_napi.cpp
)
target_link_libraries(display_route_napi PRIVATE
    entry
    libace_napi.z.so
    libhilog_ndk.z.so
)

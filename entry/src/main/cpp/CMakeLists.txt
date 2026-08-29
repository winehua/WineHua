cmake_minimum_required(VERSION 3.5.0)
project(WineHua)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/include
    # compositor 按域分组后 (frame/toplevel/input): 域内互引保持裸名 include,
    # 经本路径解析; 跨域由目标目录命中。同目录优先搜索不变。
    ${NATIVERENDER_ROOT_PATH}/compositor/frame
    ${NATIVERENDER_ROOT_PATH}/compositor/toplevel
    ${NATIVERENDER_ROOT_PATH}/compositor/input
)

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 预编译 ARM64 wayland-server (放在 libs/ 下，hvigor 自动打包)
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})

add_library(entry SHARED
    audio_broker.cpp
    audio_ipc_server.cpp
    audio_stream.cpp
    graphics_broker.cpp
    virgl_host_config.cpp
    napi_init.cpp
    wine_process.cpp
    wine_env.cpp
    env_spec.cpp
    env_profiles.cpp
    spawner.cpp
    wine_launch.cpp
    wine_exe.cpp
    broker.cpp
    ncp_dispatch.cpp
    perf_utils.cpp
    phone_adapter/phone_process.cpp
    phone_adapter/phone_virgl_relay.cpp
    wayland_server.cpp
    wl_core.cpp
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
    egl_renderer.cpp
    fs_utils.cpp
    plugin_manager.cpp
    process_utils.cpp
    ring_buffer.cpp
    seat.cpp
    shader_utils.cpp
    string_utils.cpp
    input_manager.cpp
    pointer_extras.cpp
    xdg_shell.cpp
    text_input.cpp
    xdg-shell-protocol.c
    viewporter-server-protocol.c
    pointer-warp-v1-protocol.c
    pointer-constraints-unstable-v1-protocol.c
    relative-pointer-unstable-v1-protocol.c
    text-input-unstable-v3-protocol.c
)

# BlitScaled (双线性缩放 blit) 是桌面合成热路径: 全屏游戏 800x600→1400x920
# fit 放大在 -O0 调试构建实测 ~70ms/帧 (440 万输出像素 × 4 采样插值)。
# 单独 -O2 该源文件 (其余保持 -O0 便于调试), 合成崩溃时该文件仍可栈回溯。
# 注意: compositor_blit.cpp 迁移自 compositor_utils.cpp (2026-08-22 拆分),
# -O2 属性必须跟着代码走 — 漏挂曾致 blit 段从 26ms 劣化到 ~240ms。
set_source_files_properties(compositor/frame/compositor_blit.cpp PROPERTIES COMPILE_OPTIONS "-O2")

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libnative_image.so
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
    wine_child.cpp
)
target_link_libraries(wine_child PUBLIC libhilog_ndk.z.so libchild_process.so)

add_library(virgl_child SHARED
    virgl_child.cpp
    virgl_host_config.cpp
    virgl_surface_presenter.cpp
    venus_surface_presenter.cpp
    shader_utils.cpp
    phone_adapter/phone_virgl_dispatch.cpp
)
target_link_libraries(virgl_child PUBLIC
    libEGL.so
    libGLESv3.so
    libvulkan.so
    libnative_window.so
    libhilog_ndk.z.so
    libchild_process.so
    libipc_capi.so
)

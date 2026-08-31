cmake_minimum_required(VERSION 3.5.0)
project(WineHua)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

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

add_library(entry SHARED
    audio/audio_broker.cpp
    audio/audio_ipc_server.cpp
    audio/audio_stream.cpp
    graphics/graphics_broker.cpp
    graphics/virgl_host_config.cpp
    bridge/napi_init.cpp
    proc/wine_process.cpp
    wine/wine_env.cpp
    wine/env_spec.cpp
    wine/env_profiles.cpp
    proc/spawner.cpp
    wine/wine_launch.cpp
    wine/wine_exe.cpp
    proc/broker.cpp
    proc/ncp_dispatch.cpp
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
    graphics/shader_utils.cpp
    common/string_utils.cpp
    input/input_manager.cpp
    input/pointer_extras.cpp
    compositor/xdg_shell.cpp
    input/text_input.cpp
    protocols/xdg-shell-protocol.c
    protocols/viewporter-server-protocol.c
    protocols/pointer-warp-v1-protocol.c
    protocols/pointer-constraints-unstable-v1-protocol.c
    protocols/relative-pointer-unstable-v1-protocol.c
    protocols/text-input-unstable-v3-protocol.c
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
    proc/wine_child.cpp
)
target_link_libraries(wine_child PUBLIC libhilog_ndk.z.so libchild_process.so)

add_library(virgl_child SHARED
    graphics/virgl_child.cpp
    graphics/virgl_host_config.cpp
    graphics/virgl_surface_presenter.cpp
    graphics/venus_surface_presenter.cpp
    graphics/shader_utils.cpp
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

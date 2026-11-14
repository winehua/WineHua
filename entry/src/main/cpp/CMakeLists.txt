cmake_minimum_required(VERSION 3.5.0)
project(WineHua)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    # audio 协议头留守 (wayland 协议头已随合成器域迁入 hgs)
    ${NATIVERENDER_ROOT_PATH}/protocols
    # 方案 B1b: 合成器域已迁入 hgs; seam/registry.h 等边界契约头经此解析
    ${NATIVERENDER_ROOT_PATH}/../../../../hgs/src/main/cpp
)

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

add_library(entry SHARED
    audio/audio_broker.cpp
    audio/audio_ipc_server.cpp
    audio/audio_stream.cpp
    graphics/graphics_broker.cpp
    graphics/virgl_host_config.cpp
    bridge/napi_init.cpp
    seam_bridge/host_seams.cpp
    proc/wine_process.cpp
    wine/wine_env.cpp
    wine/env_spec.cpp
    wine/env_profiles.cpp
    proc/spawner.cpp
    wine/wine_launch.cpp
    wine/wine_exe.cpp
    proc/broker.cpp
    proc/ncp_dispatch.cpp
    phone_adapter/phone_process.cpp
    phone_adapter/phone_virgl_relay.cpp
    common/fs_utils.cpp
    common/process_utils.cpp
    common/ring_buffer.cpp
    common/string_utils.cpp
)


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
)

add_library(wine_child SHARED
    proc/wine_child.cpp
)
target_link_libraries(wine_child PUBLIC libhilog_ndk.z.so libchild_process.so)


add_library(virgl_child SHARED
    graphics/virgl_child.cpp
    graphics/virgl_host_config.cpp
    graphics/shader_utils.cpp
    graphics/virgl_surface_presenter.cpp
    graphics/venus_surface_presenter.cpp
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

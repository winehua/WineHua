cmake_minimum_required(VERSION 3.5.0)
project(WineHua)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/include
)

# Pad fork-only 模式: 启用 PAD_MODE 编译宏 (禁用 execve, 使用 fork+dlopen)
# DEVICE_TYPE=pad 时由 env.sh 设置 PAD_CFLAGS=-DPAD_MODE
if(DEFINED ENV{PAD_CFLAGS} AND NOT "$ENV{PAD_CFLAGS}" STREQUAL "")
    add_definitions(-DPAD_MODE)
    message(STATUS "WineHua: PAD_MODE enabled (fork-only, no execve)")
endif()

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 预编译 ARM64 wayland-server (放在 libs/ 下，hvigor 自动打包)
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})

add_library(entry SHARED
    napi_init.cpp
    broker.cpp
    wayland_server.cpp
    egl_renderer.cpp
    plugin_manager.cpp
    seat.cpp
    input_manager.cpp
    xdg_shell.cpp
    xdg-shell-protocol.c
    viewporter-server-protocol.c
)

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libEGL.so
    libGLESv3.so
    libhilog_ndk.z.so
    libchild_process.so
    ${WAYLAND_LIBS_DIR}/libwayland-server.so.0
)

# Demo: 验证 OH_Ability_StartNativeChildProcess API 的最小子进程 .so
add_library(child_demo SHARED child_demo.cpp)
target_link_libraries(child_demo PUBLIC libhilog_ndk.z.so libchild_process.so)

# Wine 子进程入口 .so (OH_Ability_StartNativeChildProcess 启动)
add_library(wine_child SHARED wine_child.cpp)
target_link_libraries(wine_child PUBLIC libhilog_ndk.z.so libchild_process.so)

cmake_minimum_required(VERSION 3.5.0)
project(HonWine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/include
)

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 预编译 ARM64 wayland-server (放在 libs/ 下，hvigor 自动打包)
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})

add_library(entry SHARED
    napi_init.cpp
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
    ${WAYLAND_LIBS_DIR}/libwayland-server.so.0
)

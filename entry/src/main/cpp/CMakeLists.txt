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

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 预编译 ARM64 wayland-server (放在 libs/ 下，hvigor 自动打包)
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})

# 显示路线 wlroots (静态库) — 构建产物在 build/host-ext (纪律: build/ 内唯一落点),
# 其 NEEDED 闭包 .so 由 assemble.sh 装配进 libs/ (打包视图)
# 注: NATIVERENDER_ROOT_PATH = entry/src/main/cpp, 仓库根在四级上
set(WLR_DIR ${NATIVERENDER_ROOT_PATH}/../../../../build/host-ext/${OHOS_ARCH})
set(WLR_STATIC ${WLR_DIR}/usr/lib/libwlroots-0.20.a)
set(WLR_INC ${WLR_DIR}/usr/include/wlroots-0.20)
set(WLR_PIXMAN_INC ${WLR_DIR}/usr/include/pixman-1)
# xwayland.h 需要 xcb/xcb.h 等 (host-ext usr/include)
set(WLR_XCB_INC ${WLR_DIR}/usr/include)

add_library(entry SHARED
    audio/audio_broker.cpp
    audio/audio_ipc_server.cpp
    audio/audio_stream.cpp
    graphics/graphics_broker.cpp
    graphics/virgl_host_config.cpp
    input/game_controller_bridge.cpp
    input/controller/controller_hub.cpp
    input/controller/controller_napi.cpp
    input/controller/controller_runtime.cpp
    input/controller/gamepad_bridge.cpp
    input/controller/physical_gamepad.cpp
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
    display/display_compositor.cpp
    display/ohos_buffer.cpp
    display/ohos_output.c
    display/display_input.c
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
    compositor/winehua_toplevel.cpp
    input/text_input.cpp
    protocols/xdg-shell-protocol.c
    protocols/winehua-toplevel-protocol.c
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

target_include_directories(entry PRIVATE ${WLR_INC} ${WLR_PIXMAN_INC} ${WLR_XCB_INC})

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libnative_buffer.so
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
    # wlroots 静态库 + 其 NEEDED 闭包 (顺序敏感: .a 在前, 依赖 .so 在后)。
    # start-group 防静态库内部 member 依赖环 (output.c.o→allocator.c.o,
    # 单遍扫描报 undefined wlr_allocator_init, T8 实测)
    -Wl,--start-group
    ${WLR_STATIC}
    -Wl,--end-group
    ${WAYLAND_LIBS_DIR}/libwayland-server.so.0
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
)

add_library(wine_child SHARED
    proc/wine_child.cpp
)
target_link_libraries(wine_child PUBLIC libhilog_ndk.z.so libchild_process.so)

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

# smoke 测试设施的原生侧 (与 ets/smoke/ 物理隔离对应): 测试专用查询/注入
# 原语, 产品代码不引用。符号来自 entry (同进程运行时解析), 必须与 entry 同包。
add_library(smoke_napi SHARED
    smoke/smoke_napi.cpp
)
target_link_libraries(smoke_napi PRIVATE
    entry
    libace_napi.z.so
    libhilog_ndk.z.so
)

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

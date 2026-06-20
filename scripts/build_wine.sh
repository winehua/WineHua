#!/bin/bash
# build_wine.sh — Wine 交叉编译
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# Wine 编译标志 (Unix .so + wineserver)
WINE_CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -D__ANDROID__ -D__OHOS__ -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables $PAD_CFLAGS"

build_native_tools() {
    log "--- Native 构建 (winegcc + PE DLLs) ---"
    mkdir -p "$WINE_SRC/build-native"
    cd "$WINE_SRC/build-native"
    if [ ! -f "Makefile" ]; then
        # 用 cache variables 模拟 Wayland 检测, 避免在 host 上安装 libwayland-dev 等
        export ac_cv_header_wayland_client_h=yes
        export ac_cv_lib_wayland_client_wl_display_connect=yes
        export WAYLAND_CLIENT_CFLAGS="-I/usr/local/include"
        export WAYLAND_CLIENT_LIBS="-L/usr/local/lib/x86_64-linux-gnu -lwayland-client"
        export ac_cv_header_xkbcommon_xkbcommon_h=yes
        export ac_cv_lib_xkbcommon_xkb_context_new=yes
        export ac_cv_lib_soname_xkbcommon="libxkbcommon.so.0"
        export XKBCOMMON_CFLAGS="-I/usr/include"
        export XKBCOMMON_LIBS="-lxkbcommon"
        export ac_cv_header_xkbcommon_xkbregistry_h=yes
        export ac_cv_lib_xkbregistry_rxkb_context_new=yes
        export ac_cv_lib_soname_xkbregistry="libxkbregistry.so.0"
        export XKBREGISTRY_CFLAGS="-I/usr/include"
        export XKBREGISTRY_LIBS="-lxkbregistry"
        export WAYLAND_SCANNER=/usr/local/bin/wayland-scanner
        ../configure --enable-win64 --disable-tests \
            --without-x --without-freetype --without-alsa \
            --without-opengl --without-vulkan
    fi
    make -j$JOBS
}

build_ohos_unix() {
    log "--- OHOS 交叉编译 (Unix .so) ---"

    mkdir -p "$WINE_SRC/build-ohos"
    cd "$WINE_SRC/build-ohos"

    # 检查是否需要重新 configure (FreeType/Wayland 启用/禁用 状态变更)
    if [ ! -f "Makefile" ] || ! grep -q '#define SONAME_LIBFREETYPE' include/config.h 2>/dev/null \
       || ! grep -q '#define SONAME_LIBWAYLAND_CLIENT' include/config.h 2>/dev/null; then
        export FREETYPE_CFLAGS="-I$SYSROOT_EXT_INC/freetype2"
        export FREETYPE_LIBS="-L$SYSROOT_EXT_LIB -lfreetype"
        export ac_cv_header_ft2build_h=yes
        export ac_cv_lib_soname_freetype="libfreetype.so.6"
        # Wayland 交叉编译缓存
        export ac_cv_header_wayland_client_h=yes
        export ac_cv_lib_wayland_client_wl_display_connect=yes
        export ac_cv_lib_soname_wayland_client="libwayland-client.so.0"
        export ac_cv_header_xkbcommon_xkbcommon_h=yes
        export ac_cv_lib_xkbcommon_xkb_context_new=yes
        export ac_cv_lib_soname_xkbcommon="libxkbcommon.so.0"
        export ac_cv_header_xkbcommon_xkbregistry_h=yes
        export ac_cv_lib_soname_xkbregistry="libxkbregistry.so.0"
        export WAYLAND_CLIENT_CFLAGS="-I$SYSROOT_EXT_INC"
        export WAYLAND_CLIENT_LIBS="-L$SYSROOT_EXT_LIB -lwayland-client"
        export XKBCOMMON_CFLAGS="-I$SYSROOT_EXT_INC"
        export XKBCOMMON_LIBS="-L$SYSROOT_EXT_LIB -lxkbcommon"
        export XKBREGISTRY_CFLAGS="-I$SYSROOT_EXT_INC"
        export XKBREGISTRY_LIBS="-L$SYSROOT_EXT_LIB -lxkbregistry"
        export WAYLAND_SCANNER=/usr/local/bin/wayland-scanner

        CC="gcc" ../configure \
            --host=x86_64-linux-ohos \
            --prefix=/opt/winehua \
            --libdir='${prefix}' \
            --with-wine-tools=../build-native \
            --with-mingw=gcc \
            --disable-tests \
            --without-x --without-alsa \
            --without-opengl --without-vulkan
        sed -i 's/#define HAVE_LINUX_NTSYNC_H 1/\/\* OHOS \*\/\n#undef HAVE_LINUX_NTSYNC_H/' include/config.h
        sed -i 's/#define HAVE_NETIPX_IPX_H 1/\/\* OHOS \*\/\n#undef HAVE_NETIPX_IPX_H/' include/config.h
    fi

    make -k -j$JOBS \
        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
        CFLAGS="$WINE_CFLAGS -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2" \
        LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB" || true
}

build_wineserver() {
    log "--- 编译 wineserver (含 OHOS 修复) ---"
    local out="$BUILD_DIR/wine_server"
    # Pad: 数据文件在应用 sandbox 内, 路径不同
    local bindir datadir
    if [ "$DEVICE_TYPE" = "pad" ]; then
        bindir="$WINE_DEVICE_ROOT/bin"
        datadir="$WINE_DEVICE_ROOT/share"
    else
        bindir="/opt/winehua/bin"
        datadir="/opt/winehua/share"
    fi
    local wine_include="-I$WINE_SRC/include -I$WINE_SRC/include/wine -I$WINE_SRC/server -I$WINE_SRC/build-ohos/include"
    local srv_cflags="--target=$TARGET --sysroot=$SYSROOT -D__MUSL__ -D_GNU_SOURCE \
        -DWINE_UNIX_LIB -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
        -D__ANDROID__ -D__OHOS__ -DBINDIR=\"$bindir\" -DDATADIR=\"$datadir\" \
        -fPIC $wine_include"

    mkdir -p "$out"
    local need_rebuild=0
    # Pad x86_64 检查 libwineserver.so, 其他检查 wineserver
    local target_binary="$out/wineserver"
    if [ "$DEVICE_TYPE" = "pad" ] && [ "$NATIVE_ARCH" = "x86_64" ]; then
        target_binary="$out/libwineserver.so"
    fi
    if [ ! -f "$target_binary" ]; then
        need_rebuild=1
    else
        for f in $WINE_SRC/server/*.c; do
            [ "$f" -nt "$target_binary" ] && { need_rebuild=1; break; }
        done
    fi
    if [ $need_rebuild -eq 0 ]; then
        # Pad x86_64: 确保 libwineserver.so 已复制到 NATIVE_LIBS
        if [ -f "$out/libwineserver.so" ] && [ ! -f "$NATIVE_LIBS/libwineserver.so" ]; then
            cp "$out/libwineserver.so" "$NATIVE_LIBS/"
        fi
        return
    fi
    for f in $WINE_SRC/server/*.c; do
        $CLANG $srv_cflags -c -o "$out/$(basename "$f" .c).o" "$f"
    done

    # musl_compat.c 已在 WINE_SRC/server/ 中, 遍历编译时已打包

    # x86_64 Pad: 编译为共享库 (fork+dlopen 替代 execve)
    if [ "$DEVICE_TYPE" = "pad" ] && [ "$NATIVE_ARCH" = "x86_64" ]; then
        log "  wineserver → libwineserver.so (Pad x86_64)"
        $CLANG --target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld \
            -shared -Wl,-soname,libwineserver.so \
            -o "$out/libwineserver.so" "$out"/*.o -lm
        mkdir -p "$NATIVE_LIBS"
        cp "$out/libwineserver.so" "$NATIVE_LIBS/"
        log "  → $NATIVE_LIBS/libwineserver.so"
    else
        $CLANG --target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld \
            -o "$out/wineserver" "$out"/*.o -lm
        log "wineserver: $out/wineserver"
    fi
}

# ---- main ----
log "=== 构建 Wine ==="

build_native_tools
build_ohos_unix
build_wineserver

log "Wine 构建完成"

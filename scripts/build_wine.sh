#!/bin/bash
# build_wine.sh — Wine 交叉编译
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# Wine 编译标志 (Unix .so + wineserver)
WINE_CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -D__ANDROID__ -D__OHOS__ -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables"

build_native_tools() {
    log "--- Native 构建 (winegcc + PE DLLs) ---"
    mkdir -p "$WINE_SRC/build-native"
    cd "$WINE_SRC/build-native"
    if [ ! -f "Makefile" ]; then
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
            --prefix=/opt/winebox \
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
    local wine_include="-I$WINE_SRC/include -I$WINE_SRC/include/wine -I$WINE_SRC/server -I$WINE_SRC/build-ohos/include"
    local srv_cflags="--target=$TARGET --sysroot=$SYSROOT -D__MUSL__ -D_GNU_SOURCE \
        -DWINE_UNIX_LIB -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
        -D__ANDROID__ -D__OHOS__ -DBINDIR=\"/opt/winebox/bin\" -DDATADIR=\"/opt/winebox/share\" \
        -fPIC $wine_include"

    mkdir -p "$out"
    local need_rebuild=0
    if [ ! -f "$out/wineserver" ]; then
        need_rebuild=1
    else
        for f in $WINE_SRC/server/*.c; do
            [ "$f" -nt "$out/wineserver" ] && { need_rebuild=1; break; }
        done
    fi
    if [ $need_rebuild -eq 0 ]; then return; fi
    for f in $WINE_SRC/server/*.c; do
        $CLANG $srv_cflags -c -o "$out/$(basename "$f" .c).o" "$f"
    done

    # musl compat stub: epoll_pwait2
    cat > "$out/musl_compat.c" << 'EOF'
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <errno.h>
int epoll_pwait2(int fd, struct epoll_event *ev, int n,
                 const struct timespec *ts, const sigset_t *s)
{ errno=ENOSYS; return -1; }
EOF
    $CLANG $srv_cflags -c -o "$out/musl_compat.o" "$out/musl_compat.c"

    $CLANG --target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld \
        -o "$out/wineserver" "$out"/*.o -lm
    log "wineserver: $out/wineserver"
}

# ---- main ----
log "=== 构建 Wine ==="

# 应用补丁
cd "$WINE_SRC"
if git log --oneline -10 | grep -q "ohos:"; then
    log "Wine 补丁已应用，跳过"
else
    log "应用 Wine 补丁..."
    git am "$PATCHES_DIR"/*.patch
fi

build_native_tools
build_ohos_unix
build_wineserver

log "Wine 构建完成"

#!/bin/bash
# build_wine.sh — Wine 交叉编译
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# Wine 编译标志 (Unix .so + wineserver)
WINE_CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables"

build_native_tools() {
    log "--- Native 构建 (winegcc + PE DLLs) ---"
    if [ -f "$WINE_SRC/build-native/tools/winegcc/winegcc" ]; then
        log "Native 产物已存在，跳过"
        return
    fi
    mkdir -p "$WINE_SRC/build-native"
    cd "$WINE_SRC/build-native"
    ../configure --enable-win64 --disable-tests \
        --without-x --without-freetype --without-alsa \
        --without-opengl --without-vulkan
    make -j$JOBS
}

build_ohos_unix() {
    log "--- OHOS 交叉编译 (Unix .so) ---"
    mkdir -p "$WINE_SRC/build-ohos"
    cd "$WINE_SRC/build-ohos"

    if [ ! -f "Makefile" ]; then
        CC="gcc" ../configure \
            --host=x86_64-linux-ohos \
            --prefix=/opt/winebox \
            --libdir='${prefix}' \
            --with-wine-tools=../build-native \
            --with-mingw=gcc \
            --disable-tests \
            --without-x --without-freetype --without-alsa \
            --without-opengl --without-vulkan
        sed -i 's/#define HAVE_LINUX_NTSYNC_H 1/\/\* OHOS \*\/\n#undef HAVE_LINUX_NTSYNC_H/' include/config.h
        sed -i 's/#define HAVE_NETIPX_IPX_H 1/\/\* OHOS \*\/\n#undef HAVE_NETIPX_IPX_H/' include/config.h
    fi

    make -k -j$JOBS \
        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
        CFLAGS="$WINE_CFLAGS" \
        LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET"
}

build_wineserver() {
    log "--- 编译 wineserver (含 OHOS 修复) ---"
    local out="$BUILD_DIR/wine_server"
    local wine_include="-I$WINE_SRC/include -I$WINE_SRC/include/wine -I$WINE_SRC/server -I$WINE_SRC/build-ohos/include"
    local srv_cflags="$CLANG --target=$TARGET --sysroot=$SYSROOT -D__MUSL__ -D_GNU_SOURCE \
        -DWINE_UNIX_LIB -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
        -D__ANDROID__ -fPIC $wine_include"

    if [ -f "$out/wineserver" ]; then
        log "wineserver 已编译，跳过"
        return
    fi

    mkdir -p "$out"
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
git am "$PATCHES_DIR"/*.patch 2>/dev/null || true

build_native_tools
build_ohos_unix
build_wineserver

log "Wine 构建完成"

#!/bin/bash
# build_wayland.sh — Wayland + wayland-protocols 交叉编译 → sysroot-ext
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

WL_SRC="$ROOT/thirdparty/wayland"
WP_SRC="$ROOT/thirdparty/wayland-protocols"
WL_BUILD="$BUILD_DIR/wayland_build"

# 确保 native wayland-scanner 可用
SCANNER="$WAYLAND_SCANNER"
build_scanner() {
    if [ -x "$SCANNER" ]; then return 0; fi
    log "--- 编译 wayland-scanner (native) ---"
    if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
        local host_build="$BUILD_DIR/wayland_native"
        local host_prefix="$BUILD_DIR/host-tools"
        mkdir -p "$host_build" "$host_prefix"
        meson setup "$host_build" "$WL_SRC" \
            --prefix "$host_prefix" \
            -Dlibraries=false -Dscanner=true -Ddtd_validation=false \
            -Ddocumentation=false -Dtests=false --buildtype=release
        ninja -C "$host_build"
        ninja -C "$host_build" install

        # 安装时的 strip 操作破坏了 OHOS SDK clang 的自动签名，所以在 HarmonyOS 上需要重新签名才能运行
        if [ "$HOST_OS" = "HarmonyOS" ]; then
            "$SCRIPT_DIR/ohos-sign-elf.py" "$host_prefix"
        fi
    else
        mkdir -p /tmp/wayland_native
        meson setup /tmp/wayland_native "$WL_SRC" \
            --prefix /usr/local -Ddocumentation=false -Dtests=false --buildtype=release
        ninja -C /tmp/wayland_native
        ninja -C /tmp/wayland_native install
    fi
    log "wayland-scanner: $(which wayland-scanner)"
}

log "=== 构建 Wayland (x86_64) ==="

if [ -f "$SYSROOT_EXT_LIB/libwayland-client.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libwayland-client.so" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-client.h" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-client.pc" ]; then
    log "Wayland 已就绪，跳过"
    exit 0
fi

build_scanner

if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
    export PKG_CONFIG_PATH="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export PKG_CONFIG_PATH_FOR_BUILD="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH_FOR_BUILD:+:$PKG_CONFIG_PATH_FOR_BUILD}"
fi

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$SYSROOT_EXT_SHARE"
mkdir -p "$WL_BUILD"

# 1. 交叉编译 wayland (client + egl)
meson_build "$WL_BUILD/x86_64" "$WL_SRC" \
    -Ddocumentation=false -Dtests=false -Dscanner=false
ninja -C "$WL_BUILD/x86_64"

# 安装 .so (文件名 = SONAME)
cp "$WL_BUILD/x86_64/src/libwayland-client.so.0.22.0" "$SYSROOT_EXT_LIB/libwayland-client.so.0"
cp "$WL_BUILD/x86_64/src/libwayland-server.so.0.22.0" "$SYSROOT_EXT_LIB/libwayland-server.so.0"
cp "$WL_BUILD/x86_64/egl/libwayland-egl.so.1.22.0" "$SYSROOT_EXT_LIB/libwayland-egl.so.1" 2>/dev/null || true
ln -sf libwayland-client.so.0 "$SYSROOT_EXT_LIB/libwayland-client.so"
ln -sf libwayland-server.so.0 "$SYSROOT_EXT_LIB/libwayland-server.so"
ln -sf libwayland-egl.so.1 "$SYSROOT_EXT_LIB/libwayland-egl.so" 2>/dev/null || true

# 头文件
cp "$WL_SRC/src/wayland-client.h" \
   "$WL_SRC/src/wayland-client-core.h" \
   "$WL_SRC/src/wayland-util.h" \
   "$WL_BUILD/x86_64/src/wayland-client-protocol.h" \
   "$WL_BUILD/x86_64/src/wayland-version.h" \
   "$SYSROOT_EXT_INC/"
cp "$WL_SRC/egl/wayland-egl.h" "$SYSROOT_EXT_INC/" 2>/dev/null || true
cp "$WL_SRC/egl/wayland-egl-core.h" "$SYSROOT_EXT_INC/" 2>/dev/null || true

# 2. wayland-protocols
meson_build "$WL_BUILD/protocols" "$WP_SRC" \
    -Dtests=false
ninja -C "$WL_BUILD/protocols"

# 安装协议 XML 到 sysroot-ext
mkdir -p "$SYSROOT_EXT_SHARE/wayland-protocols/stable/xdg-shell" \
         "$SYSROOT_EXT_SHARE/wayland"
cp "$WP_SRC/stable/xdg-shell/xdg-shell.xml" "$SYSROOT_EXT_SHARE/wayland-protocols/stable/xdg-shell/"
cp "$WL_SRC/protocol/wayland.xml" "$SYSROOT_EXT_SHARE/wayland/"

# .pc 文件
cat > "$SYSROOT_EXT_PC/wayland-client.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos

Name: Wayland Client
Description: Wayland client side library
Version: 1.22.0
Requires.private: libffi
Libs: -L\${libdir} -lwayland-client
Cflags: -I\${includedir}
EOF

cat > "$SYSROOT_EXT_PC/wayland-egl.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos

Name: Wayland EGL
Description: Wayland EGL platform library
Version: 1.22.0
Libs: -L\${libdir} -lwayland-egl
Cflags: -I\${includedir}
EOF

cat > "$SYSROOT_EXT_PC/wayland-protocols.pc" << EOF
prefix=$SYSROOT_EXT/usr
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland-protocols
Name: Wayland Protocols
Description: Wayland protocol files
Version: 1.32
EOF

log "Wayland → sysroot-ext"

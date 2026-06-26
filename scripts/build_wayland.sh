#!/bin/bash
# Build Wayland, wayland-egl, and wayland-protocols into sysroot-ext.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

WL_SRC="$ROOT/thirdparty/wayland"
WP_SRC="$ROOT/thirdparty/wayland-protocols"
WL_BUILD="$BUILD_DIR/wayland_build"

# Ensure a host-native wayland-scanner is available without requiring root.
SCANNER="$WAYLAND_SCANNER"

find_wsl_scanner() {
    [ "$HOST_SHELL" = "msys2" ] || return 1
    command -v wsl.exe >/dev/null 2>&1 || return 1

    local scanner_path=""
    scanner_path="$(wsl.exe sh -lc 'command -v wayland-scanner 2>/dev/null || true' 2>/dev/null | tr -d '\r')"
    [ -n "$scanner_path" ] || return 1

    printf '%s\n' "$scanner_path"
}

install_wsl_scanner_wrapper() {
    local wrapper_target="$SCANNER"

    mkdir -p "$(dirname "$wrapper_target")"
    cp "$ROOT/scripts/wsl_wayland_scanner_wrapper.sh" "$wrapper_target"
    chmod +x "$wrapper_target"
    log "wayland-scanner: $wrapper_target (via WSL)"
}

setup_host_scanner_pkgconfig() {
    local host_pc_dir="$HOST_TOOLS_DIR/lib/pkgconfig"
    local host_arch_pc_dir="$HOST_TOOLS_DIR/lib/x86_64-linux-gnu/pkgconfig"
    local host_pc_path="$host_pc_dir:$host_arch_pc_dir"

    mkdir -p "$host_pc_dir"
    mkdir -p "$host_arch_pc_dir"
    cat > "$host_pc_dir/wayland-scanner.pc" << EOF
prefix=$HOST_TOOLS_DIR
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland
bindir=\${prefix}/bin
wayland_scanner=\${bindir}/wayland-scanner

Name: Wayland Scanner
Description: Wayland scanner
Version: 1.22.0
EOF

    cp "$host_pc_dir/wayland-scanner.pc" "$host_arch_pc_dir/wayland-scanner.pc"

    export PKG_CONFIG_PATH="$host_pc_path${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export PKG_CONFIG_PATH_FOR_BUILD="$host_pc_path${PKG_CONFIG_PATH_FOR_BUILD:+:$PKG_CONFIG_PATH_FOR_BUILD}"
}

build_scanner() {
    if [ -x "$SCANNER" ]; then
        if find_wsl_scanner >/dev/null; then
            install_wsl_scanner_wrapper
        fi
        setup_host_scanner_pkgconfig
        return 0
    fi

    log "--- Build wayland-scanner (native) ---"
    local host_build="$BUILD_DIR/wayland_native"

    if find_wsl_scanner >/dev/null; then
        install_wsl_scanner_wrapper
        setup_host_scanner_pkgconfig
        return 0
    fi

    rm -rf "$host_build"
    mkdir -p "$HOST_TOOLS_DIR"
    meson setup "$host_build" "$WL_SRC" \
        --prefix "$HOST_TOOLS_DIR" \
        -Dlibraries=false \
        -Dscanner=true \
        -Ddtd_validation=false \
        -Ddocumentation=false \
        -Dtests=false \
        --buildtype=release
    ninja -C "$host_build" wayland-scanner
    meson install -C "$host_build"
    setup_host_scanner_pkgconfig
    log "wayland-scanner: $SCANNER"
}

ensure_target_libffi() {
    local src="$ROOT/thirdparty/libffi"
    local build="$BUILD_DIR/libffi_build"

    if [ -f "$SYSROOT_EXT_LIB/libffi.so.8" ] && [ -f "$SYSROOT_EXT_INC/ffi.h" ] && [ -f "$SYSROOT_EXT_PC/libffi.pc" ]; then
        return 0
    fi

    log "--- libffi (for wayland) ---"
    mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$build"

    # Windows-synced trees may leave libffi text files with CRLF, which breaks WSL shell execution.
    python3 - "$src" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
for path in root.rglob("*"):
    if not path.is_file():
        continue
    data = path.read_bytes()
    if b"\0" in data or b"\r" not in data:
        continue
    path.write_bytes(data.replace(b"\r\n", b"\n"))
PY

    cd "$build"
    "$src/autogen.sh" 2>/dev/null || true
    CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
    CFLAGS="-O2 -fPIC -D__MUSL__" \
    LDFLAGS="-fuse-ld=lld" \
    "$src/configure" --host=x86_64-unknown-linux-musl --prefix="$build/install" --disable-docs --disable-dependency-tracking
    make -j"$JOBS"
    make install
    cp "$build/install/lib/libffi.so.8.1.4" "$SYSROOT_EXT_LIB/libffi.so.8"
    cp "$build/install/lib/libffi.so.8.1.4" "$SYSROOT_EXT_LIB/libffi.so"
    cp "$build/install/include/ffi.h" "$SYSROOT_EXT_INC/"
    cp "$build/install/include/ffitarget.h" "$SYSROOT_EXT_INC/"
    cat > "$SYSROOT_EXT_PC/libffi.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
Name: libffi
Description: Library supporting Foreign Function Interfaces
Version: 3.4.6
Libs: -L\${libdir} -lffi
Cflags: -I\${includedir}
EOF
}

log "=== Build Wayland (x86_64) ==="

if [ -f "$SYSROOT_EXT_LIB/libwayland-client.so" ] \
   && [ -f "$SYSROOT_EXT_LIB/libwayland-client.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libwayland-server.so" ] \
   && [ -f "$SYSROOT_EXT_LIB/libwayland-server.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libwayland-egl.so" ] \
   && [ -f "$SYSROOT_EXT_LIB/libwayland-egl.so.1" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-client.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-client-core.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-client-protocol-core.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-egl.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-egl-core.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-server.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-server-core.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-server-protocol.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-server-protocol-core.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-version.h" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-client.pc" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-egl.pc" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-egl-backend.pc" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-server.pc" ]; then
    log "Wayland stack already available in sysroot-ext"
    exit 0
fi

build_scanner
ensure_target_libffi

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$SYSROOT_EXT_SHARE"
mkdir -p "$WL_BUILD"
rm -rf "$WL_BUILD/x86_64" "$WL_BUILD/protocols"

# 1. Build Wayland client/server/egl pieces.
meson_build "$WL_BUILD/x86_64" "$WL_SRC" \
    -Ddocumentation=false -Dtests=false -Dscanner=false
ninja -C "$WL_BUILD/x86_64"

# Copy shared libraries into sysroot-ext.
cp "$WL_BUILD/x86_64/src/libwayland-client.so.0.22.0" "$SYSROOT_EXT_LIB/libwayland-client.so"
cp "$WL_BUILD/x86_64/src/libwayland-client.so.0.22.0" "$SYSROOT_EXT_LIB/libwayland-client.so.0"
cp "$WL_BUILD/x86_64/src/libwayland-server.so.0.22.0" "$SYSROOT_EXT_LIB/libwayland-server.so"
cp "$WL_BUILD/x86_64/src/libwayland-server.so.0.22.0" "$SYSROOT_EXT_LIB/libwayland-server.so.0"
cp "$WL_BUILD/x86_64/egl/libwayland-egl.so.1.22.0" "$SYSROOT_EXT_LIB/libwayland-egl.so"
cp "$WL_BUILD/x86_64/egl/libwayland-egl.so.1.22.0" "$SYSROOT_EXT_LIB/libwayland-egl.so.1"

# Copy headers into sysroot-ext.
cp "$WL_SRC/src/wayland-client.h" \
   "$WL_SRC/src/wayland-client-core.h" \
   "$WL_SRC/src/wayland-server.h" \
   "$WL_SRC/src/wayland-server-core.h" \
   "$WL_SRC/src/wayland-util.h" \
   "$WL_SRC/egl/wayland-egl.h" \
   "$WL_SRC/egl/wayland-egl-core.h" \
   "$WL_SRC/egl/wayland-egl-backend.h" \
   "$WL_BUILD/x86_64/src/wayland-version.h" \
   "$WL_BUILD/x86_64/src/wayland-client-protocol-core.h" \
   "$WL_BUILD/x86_64/src/wayland-client-protocol.h" \
   "$WL_BUILD/x86_64/src/wayland-server-protocol-core.h" \
   "$WL_BUILD/x86_64/src/wayland-server-protocol.h" \
   "$SYSROOT_EXT_INC/"

# 2. Build wayland-protocols.
meson_build "$WL_BUILD/protocols" "$WP_SRC" \
    -Dtests=false
ninja -C "$WL_BUILD/protocols"

# Copy protocol XML files into sysroot-ext.
mkdir -p "$SYSROOT_EXT_SHARE/wayland-protocols/stable/xdg-shell" \
         "$SYSROOT_EXT_SHARE/wayland"
cp "$WP_SRC/stable/xdg-shell/xdg-shell.xml" "$SYSROOT_EXT_SHARE/wayland-protocols/stable/xdg-shell/"
cp "$WL_SRC/protocol/wayland.xml" "$SYSROOT_EXT_SHARE/wayland/"

# Generate pkg-config metadata.
cat > "$SYSROOT_EXT_PC/wayland-client.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland

Name: Wayland Client
Description: Wayland client side library
Version: 1.22.0
Requires.private: libffi
Libs: -L\${libdir} -lwayland-client
Cflags: -I\${includedir}
EOF

cat > "$SYSROOT_EXT_PC/wayland-server.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland

Name: Wayland Server
Description: Server side implementation of the Wayland protocol
Version: 1.22.0
Requires.private: libffi
Libs: -L\${libdir} -lwayland-server
Cflags: -I\${includedir}
EOF

cat > "$SYSROOT_EXT_PC/wayland-egl.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos

Name: wayland-egl
Description: Wayland EGL helper library
Version: 1.22.0
Requires: wayland-client
Libs: -L\${libdir} -lwayland-egl
Cflags: -I\${includedir}
EOF

cat > "$SYSROOT_EXT_PC/wayland-egl-backend.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include

Name: wayland-egl-backend
Description: Backend wayland-egl interface
Version: 3
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

log "Wayland stack installed into sysroot-ext"

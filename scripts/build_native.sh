#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

NATIVE_TARGET="${NATIVE_TARGET:-x86_64-linux-ohos}"
WINEHUA_INC="$WINEHUA/entry/src/main/cpp/include"
NATIVE_BUILD="$BUILD_DIR/native_${NATIVE_ARCH}"

log "=== Build native compositor deps ($NATIVE_ARCH: $NATIVE_TARGET) ==="

mkdir -p "$NATIVE_LIBS" "$WINEHUA_INC" "$NATIVE_BUILD"

materialize_native_alias() {
    local source_name="$1"
    local alias_name="$2"

    rm -f "$NATIVE_LIBS/$alias_name"
    cp "$NATIVE_LIBS/$source_name" "$NATIVE_LIBS/$alias_name"
}

gen_native_cross() {
    local cross="$NATIVE_BUILD/ohos-${NATIVE_ARCH}-cross.txt"
    local ffi_prefix="$NATIVE_BUILD/libffi/install"

    cat > "$cross" <<XEOF
[binaries]
c = '$CLANG'
cpp = '$CLANGXX'
ar = '$LLVM_AR'
strip = '$LLVM_STRIP'
pkg-config = '$PKG_CONFIG_BIN'

[built-in options]
c_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-I$ffi_prefix/include']
c_link_args = ['--target=$NATIVE_TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$ffi_prefix/lib']

[host_machine]
system = 'linux'
cpu_family = '$NATIVE_CPU_FAMILY'
cpu = '$NATIVE_CPU'
endian = 'little'
XEOF

    echo "$cross"
}

build_libffi() {
    if [ ! -f "$NATIVE_LIBS/libffi.so.8" ]; then
        log "--- libffi ($NATIVE_ARCH) ---"
        local src="$ROOT/thirdparty/libffi"
        local build="$NATIVE_BUILD/libffi"
        local ffi_host="${NATIVE_CPU}-unknown-linux-musl"

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
        mkdir -p "$build"
        cd "$build"

        "$src/autogen.sh" 2>/dev/null || true
        CC="$CLANG --target=$NATIVE_TARGET --sysroot=$SYSROOT" \
        CFLAGS="-O2 -fPIC -D__MUSL__" \
        LDFLAGS="-fuse-ld=lld" \
        "$src/configure" --host="$ffi_host" --prefix="$build/install" --disable-docs --disable-dependency-tracking

        make -j"$JOBS"
        make install

        cp "$build/install/lib/libffi.so.8.1.4" "$NATIVE_LIBS/libffi.so.8"
    else
        log "libffi ($NATIVE_ARCH) already present"
    fi

    materialize_native_alias "libffi.so.8" "libffi.so"
    log "libffi ($NATIVE_ARCH) -> $NATIVE_LIBS"
}

build_wayland() {
    if [ ! -f "$NATIVE_LIBS/libwayland-server.so.0" ]; then
        log "--- wayland ($NATIVE_ARCH) ---"
        local src="$ROOT/thirdparty/wayland"
        local build="$NATIVE_BUILD/wayland"
        local cross

        cross="$(gen_native_cross)"

        export PKG_CONFIG_PATH="$NATIVE_BUILD/libffi/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        meson setup "$build" "$src" \
            --cross-file "$cross" \
            -Ddocumentation=false -Dtests=false -Dscanner=false

        ninja -C "$build"

        cp "$build/src/libwayland-server.so.0.22.0" "$NATIVE_LIBS/libwayland-server.so.0"
        cp "$build/src/libwayland-client.so.0.22.0" "$NATIVE_LIBS/libwayland-client.so.0"
    else
        log "wayland ($NATIVE_ARCH) already present"
    fi

    materialize_native_alias "libwayland-server.so.0" "libwayland-server.so"
    materialize_native_alias "libwayland-client.so.0" "libwayland-client.so"

    log "wayland ($NATIVE_ARCH) -> $NATIVE_LIBS"
}

build_protocols() {
    local proto_c="$WINEHUA/entry/src/main/cpp/xdg-shell-protocol.c"
    if [ -f "$proto_c" ]; then
        log "wayland protocol sources already present"
        return 0
    fi

    log "--- generate wayland protocol sources ---"
    local scanner="$WAYLAND_SCANNER"
    local wl_xml="$ROOT/thirdparty/wayland/protocol/wayland.xml"
    local xdg_xml="$ROOT/thirdparty/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
    local cpp_dir="$WINEHUA/entry/src/main/cpp"

    "$scanner" server-header "$wl_xml" "$WINEHUA_INC/wayland-server-protocol.h"
    "$scanner" client-header "$wl_xml" "$WINEHUA_INC/wayland-client-protocol.h"
    "$scanner" code "$wl_xml" /dev/null

    "$scanner" server-header "$xdg_xml" "$WINEHUA_INC/xdg-shell-server-protocol.h"
    "$scanner" client-header "$xdg_xml" "$WINEHUA_INC/xdg-shell-client-protocol.h"
    "$scanner" private-code "$xdg_xml" "$cpp_dir/xdg-shell-protocol.c"

    log "protocols -> $WINEHUA_INC and $cpp_dir"
}

install_headers() {
    if [ -f "$WINEHUA_INC/wayland-server-core.h" ]; then
        log "wayland headers already present"
        return 0
    fi

    log "--- install wayland headers ---"
    local src="$ROOT/thirdparty/wayland"
    local build="$NATIVE_BUILD/wayland"

    cp "$src/src/wayland-server-core.h" \
       "$src/src/wayland-server.h" \
       "$src/src/wayland-client-core.h" \
       "$src/src/wayland-client.h" \
       "$src/src/wayland-util.h" \
       "$WINEHUA_INC/"

    cp "$build/src/wayland-server-protocol.h" \
       "$build/src/wayland-client-protocol.h" \
       "$build/src/wayland-version.h" \
       "$WINEHUA_INC/"

    log "wayland headers -> $WINEHUA_INC"
}

build_libffi
build_wayland
build_protocols
install_headers

log "Native compositor deps ready ($NATIVE_ARCH)"
log "  libs:  $NATIVE_LIBS"
log "  inc:   $WINEHUA_INC"
log "  proto: $WINEHUA/entry/src/main/cpp/xdg-shell-protocol.c"

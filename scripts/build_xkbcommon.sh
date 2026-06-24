#!/bin/bash
# build_xkbcommon.sh 鈥?libffi + libxml2 + xkbcommon 鈫?sysroot-ext
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 鏋勫缓 xkbcommon 渚濊禆 (x86_64) ==="

if [ -f "$SYSROOT_EXT_LIB/libxkbcommon.so" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxkbcommon.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxkbregistry.so" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxkbregistry.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libffi.so.8" ] \
   && [ -f "$SYSROOT_EXT_PC/libffi.pc" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxml2.so" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxml2.so.2" ] \
   && [ -f "$SYSROOT_EXT_PC/libxml-2.0.pc" ] \
   && [ -d "$SYSROOT_EXT_INC/xkbcommon" ] \
   && [ -f "$SYSROOT_EXT_PC/xkbcommon.pc" ]; then
    log "xkbcommon 渚濊禆宸插氨缁紝璺宠繃"
    exit 0
fi

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"

# 鈹€鈹€ 1. libffi 鈹€鈹€
build_libffi() {
    local src="$ROOT/thirdparty/libffi"
    local build="$BUILD_DIR/libffi_build"
    if [ -f "$SYSROOT_EXT_LIB/libffi.so.8" ] && [ -f "$SYSROOT_EXT_INC/ffi.h" ]; then return 0; fi

    log "--- libffi ---"
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
    mkdir -p "$build" && cd "$build"
    "$src/autogen.sh" 2>/dev/null || true
    CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
    CFLAGS="-O2 -fPIC -D__MUSL__" \
    LDFLAGS="-fuse-ld=lld" \
    "$src/configure" --host=x86_64-unknown-linux-musl --prefix="$build/install" --disable-docs --disable-dependency-tracking
    make -j$JOBS && make install
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

# 鈹€鈹€ 2. libxml2 鈹€鈹€
build_libxml2() {
    local src="$ROOT/thirdparty/libxml2"
    local build="$BUILD_DIR/libxml2_build"
    local toolchain
    if [ -f "$SYSROOT_EXT_LIB/libxml2.so.2" ] && [ -d "$SYSROOT_EXT_INC/libxml" ] && [ -f "$SYSROOT_EXT_PC/libxml-2.0.pc" ]; then return 0; fi

    log "--- libxml2 ---"
    toolchain="$(gen_cmake_toolchain x86_64 "$TARGET" "x86_64")"
    cmake -S "$src" -B "$build" -GNinja \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_TESTS=OFF \
        -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_HTTP=OFF \
        -DLIBXML2_WITH_FTP=OFF -DLIBXML2_WITH_MODULES=OFF \
        -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_ICONV=OFF \
        -DCMAKE_INSTALL_PREFIX="$build/install"
    cmake --build "$build" --parallel "$JOBS"
    cmake --install "$build"
    cp "$build/libxml2.so.2.12.0" "$SYSROOT_EXT_LIB/libxml2.so.2"
    cp "$build/libxml2.so.2.12.0" "$SYSROOT_EXT_LIB/libxml2.so"
    cp -r "$build/install/include/libxml2/libxml" "$SYSROOT_EXT_INC/"
    cat > "$SYSROOT_EXT_PC/libxml-2.0.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
Name: libXML
Version: 2.12.0
Description: libXML library version2.
Libs: -L\${libdir} -lxml2
Cflags: -I\${includedir}/libxml2
EOF
}

# 鈹€鈹€ 3. xkbcommon 鈹€鈹€
build_xkbcommon() {
    local src="$ROOT/thirdparty/libxkbcommon"
    local build="$BUILD_DIR/xkbcommon_build"

    log "--- xkbcommon + xkbregistry ---"
    find "$src" -type f -exec touch -d '2 seconds ago' {} + 2>/dev/null || true
    meson_build "$build" "$src" \
        -Denable-x11=false -Denable-tools=false \
        -Denable-wayland=false -Denable-xkbregistry=true \
        -Denable-bash-completion=false -Denable-docs=false

    ninja -j"$JOBS" -C "$build" libxkbcommon.so.0.0.0 libxkbregistry.so.0.0.0
    cp "$build/libxkbcommon.so.0.0.0" "$SYSROOT_EXT_LIB/libxkbcommon.so"
    cp "$build/libxkbcommon.so.0.0.0" "$SYSROOT_EXT_LIB/libxkbcommon.so.0"
    cp "$build/libxkbregistry.so.0.0.0" "$SYSROOT_EXT_LIB/libxkbregistry.so"
    cp "$build/libxkbregistry.so.0.0.0" "$SYSROOT_EXT_LIB/libxkbregistry.so.0"
    mkdir -p "$SYSROOT_EXT_INC/xkbcommon"
    cp "$src/include/xkbcommon/"*.h "$SYSROOT_EXT_INC/xkbcommon/"
    cat > "$SYSROOT_EXT_PC/xkbcommon.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
Name: xkbcommon
Description: XKB API common to servers and clients
Version: 1.7.0
Libs: -L\${libdir} -lxkbcommon
Cflags: -I\${includedir}
EOF
    cat > "$SYSROOT_EXT_PC/xkbregistry.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
Name: xkbregistry
Description: XKB API to query available rules, models, layouts, etc.
Version: 1.7.0
Libs: -L\${libdir} -lxkbregistry
Cflags: -I\${includedir}
EOF
}

build_libffi
build_libxml2
build_xkbcommon

log "xkbcommon 渚濊禆 鈫?sysroot-ext"

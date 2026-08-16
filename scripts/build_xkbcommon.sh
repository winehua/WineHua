#!/bin/bash
# build_xkbcommon.sh — libffi + libxml2 + xkbcommon → sysroot-ext
set -euo pipefail
TMPDIR="${TMPDIR:-/tmp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建 xkbcommon 依赖 (x86_64) ==="

if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
    export PKG_CONFIG_PATH_FOR_BUILD="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH_FOR_BUILD:+:$PKG_CONFIG_PATH_FOR_BUILD}"
fi

if [ -f "$SYSROOT_EXT_LIB/libxkbcommon.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxkbcommon.so" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxkbregistry.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxkbregistry.so" ] \
   && [ -f "$SYSROOT_EXT_LIB/libffi.so.8" ] \
   && [ -f "$SYSROOT_EXT_LIB/libffi.so" ] \
   && [ -f "$SYSROOT_EXT_PC/libffi.pc" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxml2.so.2" ] \
   && [ -f "$SYSROOT_EXT_LIB/libxml2.so" ] \
   && [ -f "$SYSROOT_EXT_PC/libxml-2.0.pc" ] \
   && [ -d "$SYSROOT_EXT_INC/xkbcommon" ] \
   && [ -f "$SYSROOT_EXT_PC/xkbcommon.pc" ]; then
    log "xkbcommon 依赖已就绪，跳过"
    exit 0
fi

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"

# ── 1. libffi ──
build_libffi() {
    local src="$ROOT/thirdparty/libffi"
    local build="$BUILD_DIR/libffi_build"
    if [ -f "$SYSROOT_EXT_LIB/libffi.so.8" ] && [ -f "$SYSROOT_EXT_LIB/libffi.so" ] && [ -f "$SYSROOT_EXT_INC/ffi.h" ]; then return 0; fi

    log "--- libffi ---"
    mkdir -p "$build" && cd "$build"
    "$src/autogen.sh" 2>/dev/null || true
    if [ "$HOST_OS" = "Darwin" ]; then
        CC="$CLANG" CCAS="$CLANG" AR="$OHOS_SDK/native/llvm/bin/llvm-ar" \
        RANLIB="$OHOS_SDK/native/llvm/bin/llvm-ranlib" \
        NM="$OHOS_SDK/native/llvm/bin/llvm-nm" LD="$OHOS_SDK/native/llvm/bin/ld.lld" \
        CFLAGS="--target=$TARGET --sysroot=$SYSROOT -O2 -fPIC -D__MUSL__" \
        LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET" \
        "$src/configure" --host=x86_64-linux-gnu --prefix="$build/install" --disable-docs
    else
        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" CFLAGS="-O2 -fPIC -D__MUSL__" \
        LDFLAGS="-fuse-ld=lld" \
        "$src/configure" --host=x86_64-linux-gnu --prefix="$build/install" --disable-docs
    fi
    make -j$JOBS && make install
    cp "$build/install/lib/libffi.so.8.1.4" "$SYSROOT_EXT_LIB/libffi.so.8"
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

# ── 2. libxml2 ──
build_libxml2() {
    local src="$ROOT/thirdparty/libxml2"
    local build="$BUILD_DIR/libxml2_build"
    if [ -f "$SYSROOT_EXT_LIB/libxml2.so.2" ] && [ -d "$SYSROOT_EXT_INC/libxml" ] && [ -f "$SYSROOT_EXT_PC/libxml-2.0.pc" ] && [ -f "$SYSROOT_EXT_LIB/libxml2.so" ]; then return 0; fi

    log "--- libxml2 ---"
    cmake -S "$src" -B "$build" -GNinja \
        -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
        -DOHOS_ARCH=x86_64 -DOHOS_PLATFORM=OHOS \
        -DCMAKE_BUILD_TYPE=Release \
        -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_TESTS=OFF \
        -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_HTTP=OFF \
        -DLIBXML2_WITH_FTP=OFF -DLIBXML2_WITH_MODULES=OFF \
        -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_ICONV=OFF \
        -DCMAKE_INSTALL_PREFIX="$build/install"
    cmake --build "$build"
    cmake --install "$build"
    cp "$build/libxml2.so.2.12.0" "$SYSROOT_EXT_LIB/libxml2.so.2"
    ln -sf libxml2.so.2 "$SYSROOT_EXT_LIB/libxml2.so"
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

# ── 3. xkbcommon ──
build_xkbcommon() {
    local src="$ROOT/thirdparty/libxkbcommon"
    local build="$BUILD_DIR/xkbcommon_build"

    log "--- xkbcommon + xkbregistry ---"
    find "$src" -type f -exec touch -d '2 seconds ago' {} + 2>/dev/null || true
    meson_build "$build" "$src" \
        -Denable-x11=false -Denable-wayland=true \
        -Denable-xkbregistry=true -Denable-docs=false
    ninja -C "$build"

    # 安装 (DESTDIR, 然后拷贝到 sysroot-ext)
    DESTDIR=$TMPDIR/xkc ninja -C "$build" install
    find $TMPDIR/xkc -name "libxkbcommon.so.0.0.0" -exec cp {} "$SYSROOT_EXT_LIB/libxkbcommon.so.0" \;
    find $TMPDIR/xkc -name "libxkbregistry.so.0.0.0" -exec cp {} "$SYSROOT_EXT_LIB/libxkbregistry.so.0" \;
    ln -sf libxkbcommon.so.0 "$SYSROOT_EXT_LIB/libxkbcommon.so"
    ln -sf libxkbregistry.so.0 "$SYSROOT_EXT_LIB/libxkbregistry.so"
    find $TMPDIR/xkc -path "*/include/xkbcommon" -type d | while read d; do
        cp -r "$d" "$SYSROOT_EXT_INC/"
    done
    rm -rf $TMPDIR/xkc
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

log "xkbcommon 依赖 → sysroot-ext"

#!/bin/bash
# build_libffi.sh — libffi → sysroot-ext (wayland/xkbcommon 依赖)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

if [ -f "$SYSROOT_EXT_LIB/libffi.so.8" ] \
   && [ -f "$SYSROOT_EXT_LIB/libffi.so" ] \
   && [ -f "$SYSROOT_EXT_INC/ffi.h" ] \
   && [ -f "$SYSROOT_EXT_PC/libffi.pc" ]; then
    log "libffi 已就绪，跳过"
    exit 0
fi

log "=== 构建 libffi → sysroot-ext ==="

SRC="$ROOT/thirdparty/libffi"
BUILD="$BUILD_DIR/libffi_build"
mkdir -p "$BUILD" "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"
cd "$BUILD"

# 规避 NFS clock skew (autoreconf 生成文件 mtime 可能比源码旧 → configure 报错)
find "$SRC" -type f -exec touch {} + 2>/dev/null || true
(cd "$SRC" && ./autogen.sh) || err "libffi autogen.sh 失败"
find "$SRC" -type f -exec touch {} + 2>/dev/null || true
if [ "$HOST_OS" = "Darwin" ]; then
    CC="$CLANG" CCAS="$CLANG" AR="$OHOS_SDK/native/llvm/bin/llvm-ar" \
    RANLIB="$OHOS_SDK/native/llvm/bin/llvm-ranlib" \
    NM="$OHOS_SDK/native/llvm/bin/llvm-nm" LD="$OHOS_SDK/native/llvm/bin/ld.lld" \
    CFLAGS="--target=$TARGET --sysroot=$SYSROOT -O2 -fPIC -D__MUSL__" \
    LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET" \
    "$SRC/configure" --host=$GNU_HOST --prefix="$BUILD/install" --disable-docs
else
    CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" CFLAGS="-O2 -fPIC -D__MUSL__" \
    LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET" \
    "$SRC/configure" --host=$GNU_HOST --prefix="$BUILD/install" --disable-docs
fi

make -j$JOBS && make install

cp "$BUILD/install/lib/libffi.so.8.1.4" "$SYSROOT_EXT_LIB/libffi.so.8"
ln -sf libffi.so.8 "$SYSROOT_EXT_LIB/libffi.so"
cp "$BUILD/install/include/ffi.h" "$SYSROOT_EXT_INC/"
cp "$BUILD/install/include/ffitarget.h" "$SYSROOT_EXT_INC/" 2>/dev/null || true
cp "$BUILD/install/lib/libffi.a" "$SYSROOT_EXT_LIB/" 2>/dev/null || true

cat > "$SYSROOT_EXT_PC/libffi.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET
Name: libffi
Description: Library supporting Foreign Function Interfaces
Version: 3.4.6
Libs: -L\${libdir} -lffi
Cflags: -I\${includedir}
EOF

log "libffi → sysroot-ext"

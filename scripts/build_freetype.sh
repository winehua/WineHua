#!/bin/bash
# build_freetype.sh — FreeType 交叉编译 → sysroot-ext
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

FT_SRC="$ROOT/thirdparty/freetype"
FT_BUILD="$BUILD_DIR/freetype_build"

log "=== 构建 FreeType (x86_64) ==="

if [ -f "$SYSROOT_EXT_LIB/libfreetype.so.6" ] \
   && [ -f "$SYSROOT_EXT_LIB/libfreetype.so" ] \
   && [ -d "$SYSROOT_EXT_INC/freetype2" ] \
   && [ -f "$SYSROOT_EXT_PC/freetype2.pc" ]; then
    log "FreeType 已就绪，跳过"
    exit 0
fi

rm -rf "$FT_BUILD"

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"
mkdir -p "$FT_BUILD"
cd "$FT_BUILD"

cmake "$FT_SRC" \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
    -DOHOS_ARCH=x86_64 \
    -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_DISABLE_PNG=ON \
    -DFT_DISABLE_BZIP2=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$FT_BUILD/install"

ninja
ninja install

# 安装到 sysroot-ext (文件名 = SONAME)
# OHOS' linker may omit the upstream versioned filename during install.
# Prefer the versioned artifact when present, otherwise use the unversioned
# shared object produced by CMake and expose the SONAME-compatible name.
ft_lib="$FT_BUILD/install/lib/libfreetype.so.6.20.2"
[ -f "$ft_lib" ] || ft_lib="$FT_BUILD/install/lib/libfreetype.so"
[ -f "$ft_lib" ] || { echo "FreeType shared library not found under $FT_BUILD/install/lib" >&2; exit 1; }
cp "$ft_lib" "$SYSROOT_EXT_LIB/libfreetype.so.6"
ln -sf libfreetype.so.6 "$SYSROOT_EXT_LIB/libfreetype.so"
cp -r "$FT_BUILD"/install/include/freetype2 "$SYSROOT_EXT_INC/"
cat > "$SYSROOT_EXT_PC/freetype2.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos

Name: FreeType 2
Description: A free, high-quality, and portable font engine.
Version: 2.13.3
Libs: -L\${libdir} -lfreetype
Cflags: -I\${includedir}/freetype2
EOF

log "FreeType → sysroot-ext ($SYSROOT_EXT_LIB/libfreetype.so.6)"

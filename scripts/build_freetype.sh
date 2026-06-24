#!/bin/bash
# build_freetype.sh 鈥?FreeType 浜ゅ弶缂栬瘧 鈫?sysroot-ext
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

FT_SRC="$ROOT/thirdparty/freetype"
FT_BUILD="$BUILD_DIR/freetype_build"

log "=== 鏋勫缓 FreeType (x86_64) ==="

if [ -f "$SYSROOT_EXT_LIB/libfreetype.so.6" ] \
   && [ -d "$SYSROOT_EXT_INC/freetype2" ] \
   && [ -f "$SYSROOT_EXT_PC/freetype2.pc" ]; then
    log "FreeType 宸插氨缁紝璺宠繃"
    exit 0
fi

rm -rf "$FT_BUILD"

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"
mkdir -p "$FT_BUILD"
cd "$FT_BUILD"

TOOLCHAIN_FILE="$(gen_cmake_toolchain x86_64 "$TARGET" "x86_64")"

cmake "$FT_SRC" \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_DISABLE_PNG=ON \
    -DFT_DISABLE_BZIP2=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$FT_BUILD/install"

ninja
ninja install

# 瀹夎鍒?sysroot-ext (鏂囦欢鍚?= SONAME)
cp "$FT_BUILD"/install/lib/libfreetype.so.6.20.2 "$SYSROOT_EXT_LIB/libfreetype.so.6"
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

log "FreeType 鈫?sysroot-ext ($SYSROOT_EXT_LIB/libfreetype.so.6)"

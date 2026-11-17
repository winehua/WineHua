#!/bin/bash
# build_freetype.sh — FreeType 交叉编译 → sysroot-ext
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

FT_SRC="$ROOT/thirdparty/freetype"
FT_BUILD="$BUILD_DIR/freetype_build"
# 配方守卫: 产物存在即跳过会把配方改动静默吃掉 (同 wlroots 补丁签名教训)。
# zlib 必须关闭: 开着时 CMake 探测到 SDK sysroot 的 libz → 产物 NEEDED libz.so,
# 该产物经 _pick_lib_pad 顶掉 build_native 的 zlib-free 版占据 libs/libfreetype.so.6,
# Xwayland(libXfont2→libfreetype.so.6) 加载它后字体初始化 Fatal ("could not open
# default font", 2026-10-24 真机实测; m0 真机因 build_deps 从未跑过 arm64 分支而
# 侥幸避开, 属潜伏雷非环境差异)。FT_DISABLE_ZLIB=ON 与 build_native.sh 同款配方。
FT_RECIPE_SIG=$(printf '%s' "FT_DISABLE_ZLIB=ON" | sha256sum | cut -d' ' -f1)

log "=== 构建 FreeType ($WINE_ARCH) ==="

if [ -f "$SYSROOT_EXT_LIB/libfreetype.so.6" ] \
   && [ -f "$SYSROOT_EXT_LIB/libfreetype.so" ] \
   && [ -d "$SYSROOT_EXT_INC/freetype2" ] \
   && [ -f "$SYSROOT_EXT_PC/freetype2.pc" ] \
   && [ "$(cat "$SYSROOT_EXT_LIB/.freetype_recipe_sig" 2>/dev/null)" = "$FT_RECIPE_SIG" ]; then
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
    -DOHOS_ARCH=$OHOS_ARCH \
    -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DFT_DISABLE_ZLIB=ON \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_DISABLE_PNG=ON \
    -DFT_DISABLE_BZIP2=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$FT_BUILD/install"

ninja
ninja install

echo "$FT_RECIPE_SIG" > "$SYSROOT_EXT_LIB/.freetype_recipe_sig"

# 安装到 sysroot-ext (文件名 = SONAME)
cp "$FT_BUILD"/install/lib/libfreetype.so.6.20.2 "$SYSROOT_EXT_LIB/libfreetype.so.6"
ln -sf libfreetype.so.6 "$SYSROOT_EXT_LIB/libfreetype.so"
cp -r "$FT_BUILD"/install/include/freetype2 "$SYSROOT_EXT_INC/"
cat > "$SYSROOT_EXT_PC/freetype2.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET

Name: FreeType 2
Description: A free, high-quality, and portable font engine.
Version: 2.13.3
Libs: -L\${libdir} -lfreetype
Cflags: -I\${includedir}/freetype2
EOF

log "FreeType → sysroot-ext ($SYSROOT_EXT_LIB/libfreetype.so.6)"

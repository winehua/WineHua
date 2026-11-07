#!/bin/bash
# build_box64.sh — Box64 ARM64 交叉编译 (box64.so, dlopen in-process)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建 Box64 ==="

if [ "${NATIVE_ARCH:-arm64-v8a}" != "arm64-v8a" ]; then
    log "Box64 仅 arm64 需要 (当前 NATIVE_ARCH=$NATIVE_ARCH)，跳过"
    exit 0
fi

cd "$BOX64_SRC"

# CMake + Ninja (ninja 自行处理增量)
mkdir -p "$BUILD_DIR/box64_build"
cd "$BUILD_DIR/box64_build"

# 抑制大量 warning，聚焦真正的 error
BOX64_WARN_FLAGS="-Wno-macro-redefined -Wno-unused-command-line-argument \
    -Wno-format -Wno-format-security -Wno-error=format-security \
    -Wno-deprecated-declarations -Wno-unused-function -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-int-conversion -Wno-error=int-conversion \
    -Wno-incompatible-pointer-types -Wno-implicit-function-declaration \
    -Wno-string-plus-int -Wno-array-bounds -Wno-ignored-pragmas"

# 编 .so, dlopen 加载 (wine_child.cpp 中 dlopen("box64.so"))
log "  → box64.so (in-process, BOX32=ON)"
cmake "$BOX64_SRC" \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
    -DOHOS_ARCH=arm64-v8a \
    -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DARM_DYNAREC=ON \
    -DBOX32=ON \
    -DLIBBOX64_SO=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_FLAGS="$BOX64_WARN_FLAGS" \
    -DCMAKE_CXX_FLAGS="$BOX64_WARN_FLAGS"
ninja -j"$JOBS" box64_hmos_core
mkdir -p "$NATIVE_LIBS"
cp "$BUILD_DIR/box64_build/box64.so" "$NATIVE_LIBS/"
log "Box64 → $NATIVE_LIBS/box64.so"

#!/bin/bash
# build_box64.sh — Box64 ARM64 交叉编译
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建 Box64 ==="

cd "$BOX64_SRC"

# CMake + Ninja (ninja 自行处理增量)
mkdir -p "$BUILD_DIR/box64_build"
cd "$BUILD_DIR/box64_build"
cmake "$BOX64_SRC" \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
    -DOHOS_ARCH=arm64-v8a \
    -DOHOS_PLATFORM=OHOS \
    -DCMAKE_BUILD_TYPE=Release \
    -DARM_DYNAREC=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
ninja box64

log "Box64 构建完成"

#!/bin/bash
# build_box64.sh — Box64 ARM64 交叉编译
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建 Box64 ==="

BINARY="$BUILD_DIR/box64_build/box64"
if [ -f "$BINARY" ]; then
    log "Box64 已编译 ($BINARY)，跳过。需重编请删除此文件。"
    exit 0
fi

# 应用补丁
log "应用 Box64 OHOS 补丁..."
cd "$BOX64_SRC"
git am "$PATCHES_DIR/box64/"*.patch 2>/dev/null || true

# CMake 构建
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

log "Box64 构建完成: $BINARY"
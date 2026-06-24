#!/bin/bash
# build_box64.sh — Box64 ARM64 交叉编译
#   PC 模式: 编译 box64 可执行文件 (execve 启动)
#   Pad 模式: 编译 box64.so (dlopen in-process)
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

if [ "$DEVICE_TYPE" = "pad" ]; then
    # ARM64 Pad: 编 .so, dlopen 加载, 无 execve
    log "  → box64.so (in-process)"
    cmake "$BOX64_SRC" \
        -GNinja \
        -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
        -DOHOS_ARCH=arm64-v8a \
        -DOHOS_PLATFORM=OHOS \
        -DCMAKE_BUILD_TYPE=Release \
        -DARM_DYNAREC=ON \
        -DLIBBOX64_SO=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    ninja -j"$JOBS" box64_hmos_core
    cp "$BUILD_DIR/box64_build/box64.so" "$NATIVE_LIBS/"
    log "Box64 → $NATIVE_LIBS/box64.so"
else
    # ARM64 PC: 编可执行文件, execve 启动
    log "  → box64 (executable)"
    cmake "$BOX64_SRC" \
        -GNinja \
        -DCMAKE_TOOLCHAIN_FILE="$OHOS_SDK/native/build/cmake/ohos.toolchain.cmake" \
        -DOHOS_ARCH=arm64-v8a \
        -DOHOS_PLATFORM=OHOS \
        -DCMAKE_BUILD_TYPE=Release \
        -DARM_DYNAREC=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    ninja -j"$JOBS" box64
    log "Box64 构建完成 (executable)"
fi

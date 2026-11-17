#!/bin/bash
# build_box64.sh — Box64 ARM64 交叉编译 (box64.so, dlopen in-process)
#
# 仅 box64+wine 方案 (方案②: arm64 设备 + x86_64 wine 全转译) 需要。
# arm64 原生方案 (方案③) 用 HODLL=wowbox64.dll (build_box64_wow64.sh), 不产 box64.so;
# x86_64 方案 (方案①) 原生 wine, 不需要。
#
# 产物: box64.so → $NATIVE_LIBS/ (wine_child.cpp dlopen("box64.so") → box64_hmos_main)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建 Box64 (方案② box64+wine) ==="

# 仅方案② (NATIVE_ARCH=arm64-v8a 且 WINE_ARCH=x86_64) 需要 box64.so
if [ "${NATIVE_ARCH:-arm64-v8a}" != "arm64-v8a" ] || [ "$WINE_ARCH" = "aarch64" ]; then
    log "Box64 仅 box64+wine 方案需要 (NATIVE_ARCH=$NATIVE_ARCH WINE_ARCH=$WINE_ARCH)，跳过"
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
# 注: box64 submodule 的 custommem.c 已加 #ifndef _WIN32 守卫, 原生 OHOS 构建
# (非 _WIN32) 完整保留 mmap 补丁, 与 wowbox64.dll (Windows PE) 互不干扰。
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
ninja box64_hmos_core
cp "$BUILD_DIR/box64_build/box64.so" "$NATIVE_LIBS/"
log "Box64 → $NATIVE_LIBS/box64.so"

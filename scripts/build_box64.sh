#!/bin/bash
# build_box64.sh — Box64 ARM64 交叉编译
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建 Box64 ==="

# 应用补丁 (已应用则跳过)
cd "$BOX64_SRC"

# 先检查是否已打过补丁 (避免 git am --abort 误删已生效的 commit)
patch_applied=0
if git log --oneline -30 | grep -q "ohos:"; then
    patch_applied=1
elif [ -f src/musl_compat.c ] && [ -f src/musl_fts.c ] && [ -f src/include/config.h ]; then
    # 如果文件已存在但 git log 没找到 commit（被 am --abort 冲掉了），也认为已打过
    patch_applied=1
fi

if [ $patch_applied -eq 1 ]; then
    # 清理残留的 am 会话（不影响已应用的 commit）
    git am --abort 2>/dev/null || true
    rm -rf "$ROOT/.git/modules/thirdparty/box64/rebase-apply" 2>/dev/null || true
    log "Box64 补丁已应用，跳过"
else
    # 清理任何残留的 am 状态后再打补丁
    git am --abort 2>/dev/null || true
    rm -rf "$ROOT/.git/modules/thirdparty/box64/rebase-apply" 2>/dev/null || true
    log "应用 Box64 补丁..."
    git am "$PATCHES_DIR/box64/"*.patch
fi

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

#!/bin/bash
# build_deps.sh — 编排所有交叉编译依赖 (freetype → wayland → xkbcommon)
# 所有产物安装到 build/sysroot-ext/，不污染 OHOS SDK
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建所有交叉编译依赖 → sysroot-ext ==="

# 按依赖链顺序执行
bash "$SCRIPT_DIR/build_freetype.sh"
bash "$SCRIPT_DIR/build_wayland.sh"
bash "$SCRIPT_DIR/build_xkbcommon.sh"
# XKB 键盘布局数据 (xkeyboard-config, Wine 键盘驱动依赖)
bash "$SCRIPT_DIR/build_xkbconfig.sh"
# ARM64 native compositor 依赖 (wayland-server for HAP)
bash "$SCRIPT_DIR/build_arm64_native.sh"

log "所有依赖就绪: $SYSROOT_EXT"
echo ""
find "$SYSROOT_EXT" -type f | sort

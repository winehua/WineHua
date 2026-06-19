#!/bin/bash
# build_deps.sh — 编排所有交叉编译依赖 (freetype → wayland → xkbcommon)
# 所有产物安装到 build/sysroot-ext/，不污染 OHOS SDK
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建模拟层交叉编译依赖 (Wine用, x86_64-linux-ohos) → sysroot-ext ==="

# 按依赖链顺序执行 (模拟层依赖, 始终 x86_64-linux-ohos)
bash "$SCRIPT_DIR/build_freetype.sh"
bash "$SCRIPT_DIR/build_wayland.sh"
bash "$SCRIPT_DIR/build_xkbcommon.sh"
# XKB 键盘布局数据 (xkeyboard-config, Wine 键盘驱动依赖, 架构无关)
bash "$SCRIPT_DIR/build_xkbconfig.sh"

# Native compositor 依赖 (wayland-server for HAP) 在 build.sh 中按架构单独调用:
#   bash scripts/build_native.sh

log "模拟层依赖就绪: $SYSROOT_EXT"
echo ""
find "$SYSROOT_EXT" -type f | sort

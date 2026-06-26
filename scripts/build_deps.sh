#!/bin/bash
# Build the x86_64 OHOS sysroot-ext dependencies used by Wine.
# This currently includes FreeType, Wayland, xkbcommon, and xkeyboard-config.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== Build x86_64 OHOS sysroot-ext dependencies ==="

# Runtime deps for the Wine Unix side
bash "$SCRIPT_DIR/build_freetype.sh"
bash "$SCRIPT_DIR/build_wayland.sh"
bash "$SCRIPT_DIR/build_xkbcommon.sh"
bash "$SCRIPT_DIR/build_xkbconfig.sh"

# Native compositor deps are built separately by build_native.sh.
log "sysroot-ext ready: $SYSROOT_EXT"
echo ""
find "$SYSROOT_EXT" -type f | sort

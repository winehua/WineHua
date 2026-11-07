#!/bin/bash
# Build the source-matched host scanner for either native or guest Wayland.
# Run from any directory: bash scripts/build_wayland_scanner.sh
# Outputs stay under build/host-tools and build/wayland_native_<version>.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

src="$ROOT/thirdparty/wayland"
version=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$src/meson.build" | head -1)
[ -n "$version" ] || err "无法解析 Wayland scanner 版本"
if [ -x "$WAYLAND_SCANNER" ] \
   && "$WAYLAND_SCANNER" --version 2>&1 | grep -Fq " $version" \
   && [ -f "$BUILD_DIR/host-tools/lib/pkgconfig/wayland-scanner.pc" ]; then
    exit 0
fi

log "--- 编译 wayland-scanner (native, $version) ---"
host_build="$BUILD_DIR/wayland_native_$version"
host_prefix="$BUILD_DIR/host-tools"
meson setup "$host_build" "$src" --prefix "$host_prefix" --libdir lib \
    -Dlibraries=false -Dscanner=true -Ddtd_validation=false \
    -Ddocumentation=false -Dtests=false --buildtype=release
ninja -C "$host_build"
ninja -C "$host_build" install
if [ "$HOST_OS" = "HarmonyOS" ]; then
    "$SCRIPT_DIR/ohos-sign-elf.py" "$host_prefix"
fi
[ -x "$WAYLAND_SCANNER" ] || err "wayland-scanner 未生成: $WAYLAND_SCANNER"
log "wayland-scanner: $WAYLAND_SCANNER"

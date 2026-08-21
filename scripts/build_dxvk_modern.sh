#!/bin/bash
# Build the WineHua DXVK 2.6.2 compatibility profile for the isolated Modern runtime.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

[ -f "$DXVK_MODERN_SRC/meson.build" ] || err "DXVK Modern source missing: $DXVK_MODERN_SRC"
[ -f "$DXVK_MODERN_SRC/include/vulkan/include/vulkan/vulkan.h" ] || \
    err "DXVK Modern Vulkan-Headers submodule is missing"
[ -f "$DXVK_MODERN_SRC/include/spirv/include/spirv/unified1/spirv.hpp" ] || \
    err "DXVK Modern SPIRV-Headers submodule is missing"

setup_if_missing() {
    local build_dir="$1"
    local cross_file="$2"
    local prefix="$3"
    if [ ! -f "$build_dir/build.ninja" ]; then
        log "Configuring DXVK Modern $(basename "$build_dir")"
        meson setup "$build_dir" "$DXVK_MODERN_SRC" \
            --cross-file "$DXVK_MODERN_SRC/$cross_file" \
            --prefix "$prefix" -Dbuildtype=release
    fi
}

setup_if_missing "$DXVK_MODERN_BUILD_ROOT/build.winehua64" build-win64.txt \
    "$DXVK_MODERN_BUILD_ROOT/x64"
setup_if_missing "$DXVK_MODERN_BUILD_ROOT/build.winehua32" build-win32.txt \
    "$DXVK_MODERN_BUILD_ROOT/x86"

log "--- DXVK Modern profile ($DXVK_MODERN_SRC) ---"
ninja -C "$DXVK_MODERN_BUILD_ROOT/build.winehua64" install
ninja -C "$DXVK_MODERN_BUILD_ROOT/build.winehua32" install

for dll in d3d11.dll dxgi.dll; do
    [ -f "$DXVK_MODERN_BUILD_ROOT/x64/bin/$dll" ] || \
        err "DXVK Modern x64 artifact missing: $DXVK_MODERN_BUILD_ROOT/x64/bin/$dll"
    [ -f "$DXVK_MODERN_BUILD_ROOT/x86/bin/$dll" ] || \
        err "DXVK Modern x86 artifact missing: $DXVK_MODERN_BUILD_ROOT/x86/bin/$dll"
done

log "DXVK Modern profile ready: $(git -c safe.directory="$DXVK_MODERN_SRC" -C "$DXVK_MODERN_SRC" rev-parse --short HEAD)"

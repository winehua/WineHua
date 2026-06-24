#!/bin/bash
# Build xkeyboard-config into sysroot-ext. Prefer the vendored submodule so the
# staged XKB data is deterministic across WSL and MSYS2 hosts.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

XKBC_SRC="$ROOT/thirdparty/xkeyboard-config"
XKBC_BUILD="$BUILD_DIR/xkeyboard-config_build"
XKBC_INSTALL="$BUILD_DIR/xkeyboard-config_install"

log "=== Build xkeyboard-config (XKB data) ==="

if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ] && [ -f "$SYSROOT_EXT_SHARE/X11/xkb/rules/xkb.dtd" ]; then
    log "xkeyboard-config already staged"
    exit 0
fi

if [ ! -f "$XKBC_SRC/meson.build" ]; then
    err "xkeyboard-config submodule is missing; run: git submodule update --init thirdparty/xkeyboard-config"
fi

# Windows-synced trees may leave helper scripts with CRLF, which breaks shell execution.
find "$XKBC_SRC" -type f \( -name '*.py' -o -name '*.pl' \) -exec sed -i 's/\r$//' {} +

fallback_to_host_xkb_data() {
    if [ ! -d /usr/share/X11/xkb ] || [ ! -f /usr/share/X11/xkb/rules/xkb.dtd ]; then
        return 1
    fi

    warn "Using host xkb-data fallback from /usr/share/X11/xkb"
    mkdir -p "$SYSROOT_EXT_SHARE/X11"
    rm -rf "$SYSROOT_EXT_SHARE/X11/xkb"
    cp -r /usr/share/X11/xkb "$SYSROOT_EXT_SHARE/X11/"
    log "xkb data -> $SYSROOT_EXT_SHARE/X11/xkb"
    du -sh "$SYSROOT_EXT_SHARE/X11/xkb"
    return 0
}

log "Building xkeyboard-config from vendored submodule..."
rm -rf "$XKBC_BUILD" "$XKBC_INSTALL"

if ! {
    meson setup "$XKBC_BUILD" "$XKBC_SRC" \
        --prefix=/usr \
        -Dxorg-rules-symlinks=false &&
    ninja -C "$XKBC_BUILD" &&
    DESTDIR="$XKBC_INSTALL" meson install -C "$XKBC_BUILD"
}; then
    warn "xkeyboard-config submodule build failed"
    fallback_to_host_xkb_data || err "xkeyboard-config build failed and host fallback is unavailable"
    exit 0
fi

mkdir -p "$SYSROOT_EXT_SHARE"
rm -rf "$SYSROOT_EXT_SHARE/X11/xkb" "$SYSROOT_EXT_SHARE/xkeyboard-config-2"
cp -r "$XKBC_INSTALL/usr/share/xkeyboard-config-2" "$SYSROOT_EXT_SHARE/"
cp -rL "$XKBC_INSTALL/usr/share/X11" "$SYSROOT_EXT_SHARE/"
rm -rf "$XKBC_INSTALL"

log "xkb data -> $SYSROOT_EXT_SHARE/X11/xkb"
du -sh "$SYSROOT_EXT_SHARE/X11/xkb"

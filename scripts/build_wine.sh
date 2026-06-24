#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

WINE_CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -D__ANDROID__ -D__OHOS__ -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables $PAD_CFLAGS"

find_readelf() {
    if command -v llvm-readelf >/dev/null 2>&1; then
        command -v llvm-readelf
        return 0
    fi
    if command -v readelf >/dev/null 2>&1; then
        command -v readelf
        return 0
    fi
    return 1
}

describe_ohaudio_runtime_profile() {
    local libohaudio="$SYSROOT/usr/lib/x86_64-linux-ohos/libohaudio.so"
    local readelf_bin
    local dep
    local needed=()

    if [ ! -f "$libohaudio" ]; then
        warn "OHAudio runtime not found in SDK sysroot: $libohaudio"
        return 0
    fi

    if ! readelf_bin="$(find_readelf)"; then
        warn "cannot inspect OHAudio dependency profile; no readelf tool found"
        return 0
    fi

    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        needed+=("$dep")
    done < <("$readelf_bin" -d "$libohaudio" 2>/dev/null | sed -n 's/^.*Shared library: \[\(.*\)\].*$/\1/p' | sort -u)

    if [ "${#needed[@]}" -eq 0 ]; then
        log "OHAudio DT_NEEDED -> (none)"
        return 0
    fi

    log "OHAudio DT_NEEDED -> ${needed[*]}"
}

normalize_wine_scripts() {
    python3 - "$WINE_SRC" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
names = {"configure", "configure.ac", "config.guess", "config.sub", "install-sh", "missing", "Makefile.in"}
suffixes = {".in", ".sh", ".pl", ".py"}

for path in root.rglob("*"):
    if not path.is_file():
        continue
    if path.name not in names and path.suffix not in suffixes:
        continue
    data = path.read_bytes()
    if b"\r" not in data:
        continue
    path.write_bytes(data.replace(b"\r\n", b"\n"))
PY
}

refresh_wine_configure() {
    local need_regen=0

    if ! grep -q 'WINE_CONFIG_MAKEFILE(dlls/wineohos.drv)' "$WINE_SRC/configure.ac" 2>/dev/null; then
        return 0
    fi

    if [ ! -f "$WINE_SRC/configure" ]; then
        need_regen=1
    elif ! grep -q 'dlls/wineohos.drv' "$WINE_SRC/configure" 2>/dev/null; then
        need_regen=1
    elif ! grep -q 'programs/winehua_audio_smoke' "$WINE_SRC/configure" 2>/dev/null; then
        need_regen=1
    fi

    if [ "$need_regen" -eq 0 ]; then
        return 0
    fi

    if ! command -v autoconf >/dev/null 2>&1; then
        err "autoconf is required to regenerate thirdparty/wine/configure"
    fi

    log "Regenerating Wine configure from configure.ac"
    (
        cd "$WINE_SRC"
        autoconf --warnings=all
    )
}

configure_host_pkg() {
    local pkg="$1"
    local cflags_var="$2"
    local libs_var="$3"
    local header_var="$4"
    local symbol_var="$5"
    local soname_var="${6:-}"
    local soname_value="${7:-}"

    if ! command -v "$PKG_CONFIG_BIN" >/dev/null 2>&1; then
        return 1
    fi

    if ! "$PKG_CONFIG_BIN" --exists "$pkg" >/dev/null 2>&1; then
        return 1
    fi

    printf -v "$cflags_var" '%s' "$("$PKG_CONFIG_BIN" --cflags "$pkg")"
    printf -v "$libs_var" '%s' "$("$PKG_CONFIG_BIN" --libs "$pkg")"
    printf -v "$header_var" 'yes'
    printf -v "$symbol_var" 'yes'
    export "$cflags_var" "$libs_var" "$header_var" "$symbol_var"

    if [ -n "$soname_var" ] && [ -n "$soname_value" ]; then
        printf -v "$soname_var" '%s' "$soname_value"
        export "$soname_var"
    fi

    return 0
}

disable_host_pkg() {
    local header_var="$1"
    local symbol_var="$2"
    local cflags_var="$3"
    local libs_var="$4"
    local soname_var="${5:-}"

    printf -v "$header_var" 'no'
    printf -v "$symbol_var" 'no'
    printf -v "$cflags_var" ''
    printf -v "$libs_var" ''
    export "$header_var" "$symbol_var" "$cflags_var" "$libs_var"

    if [ -n "$soname_var" ]; then
        printf -v "$soname_var" ''
        export "$soname_var"
    fi
}

build_native_tools() {
    log "--- Build native Wine tools ---"
    mkdir -p "$WINE_SRC/build-native"
    cd "$WINE_SRC/build-native"
    if [ ! -f "Makefile" ]; then
        if configure_host_pkg \
            wayland-client \
            WAYLAND_CLIENT_CFLAGS \
            WAYLAND_CLIENT_LIBS \
            ac_cv_header_wayland_client_h \
            ac_cv_lib_wayland_client_wl_display_connect \
            ac_cv_lib_soname_wayland_client \
            libwayland-client.so.0; then
            log "Host Wayland support detected via pkg-config"
        else
            warn "Host Wayland support not found; disabling Wayland-dependent native Wine features"
            disable_host_pkg \
                ac_cv_header_wayland_client_h \
                ac_cv_lib_wayland_client_wl_display_connect \
                WAYLAND_CLIENT_CFLAGS \
                WAYLAND_CLIENT_LIBS \
                ac_cv_lib_soname_wayland_client
        fi

        if configure_host_pkg \
            xkbcommon \
            XKBCOMMON_CFLAGS \
            XKBCOMMON_LIBS \
            ac_cv_header_xkbcommon_xkbcommon_h \
            ac_cv_lib_xkbcommon_xkb_context_new \
            ac_cv_lib_soname_xkbcommon \
            libxkbcommon.so.0; then
            log "Host xkbcommon support detected via pkg-config"
        else
            warn "Host xkbcommon support not found; disabling xkbcommon-dependent native Wine features"
            disable_host_pkg \
                ac_cv_header_xkbcommon_xkbcommon_h \
                ac_cv_lib_xkbcommon_xkb_context_new \
                XKBCOMMON_CFLAGS \
                XKBCOMMON_LIBS \
                ac_cv_lib_soname_xkbcommon
        fi

        if configure_host_pkg \
            xkbregistry \
            XKBREGISTRY_CFLAGS \
            XKBREGISTRY_LIBS \
            ac_cv_header_xkbcommon_xkbregistry_h \
            ac_cv_lib_xkbregistry_rxkb_context_new \
            ac_cv_lib_soname_xkbregistry \
            libxkbregistry.so.0; then
            log "Host xkbregistry support detected via pkg-config"
        else
            warn "Host xkbregistry support not found; disabling xkbregistry-dependent native Wine features"
            disable_host_pkg \
                ac_cv_header_xkbcommon_xkbregistry_h \
                ac_cv_lib_xkbregistry_rxkb_context_new \
                XKBREGISTRY_CFLAGS \
                XKBREGISTRY_LIBS \
                ac_cv_lib_soname_xkbregistry
        fi

        export WAYLAND_SCANNER="$WAYLAND_SCANNER"
        ../configure --enable-win64 --disable-tests \
            --without-x --without-freetype --without-alsa \
            --without-opengl --without-vulkan
    fi
    make -j"$JOBS" tools/winebuild tools/widl tools/winegcc tools/wine/wine
}

build_ohos_unix() {
    log "--- Build OHOS Wine unix side ---"

    mkdir -p "$WINE_SRC/build-ohos"
    cd "$WINE_SRC/build-ohos"

    if [ ! -f "Makefile" ] || ! grep -q '#define SONAME_LIBFREETYPE' include/config.h 2>/dev/null \
       || ! grep -q '#define SONAME_LIBWAYLAND_CLIENT' include/config.h 2>/dev/null \
       || ! grep -Eq 'dlls/wineohos\.drv/wineohos\.so:.*dlls/wineohos\.drv/ohos_audio_client\.o' Makefile 2>/dev/null; then
        export FREETYPE_CFLAGS="-I$SYSROOT_EXT_INC/freetype2"
        export FREETYPE_LIBS="-L$SYSROOT_EXT_LIB -lfreetype"
        export ac_cv_header_ft2build_h=yes
        export ac_cv_lib_soname_freetype="libfreetype.so.6"
        export ac_cv_header_wayland_client_h=yes
        export ac_cv_lib_wayland_client_wl_display_connect=yes
        export ac_cv_lib_soname_wayland_client="libwayland-client.so.0"
        export ac_cv_header_xkbcommon_xkbcommon_h=yes
        export ac_cv_lib_xkbcommon_xkb_context_new=yes
        export ac_cv_lib_soname_xkbcommon="libxkbcommon.so.0"
        export ac_cv_header_xkbcommon_xkbregistry_h=yes
        export ac_cv_lib_soname_xkbregistry="libxkbregistry.so.0"
        export WAYLAND_CLIENT_CFLAGS="-I$SYSROOT_EXT_INC"
        export WAYLAND_CLIENT_LIBS="-L$SYSROOT_EXT_LIB -lwayland-client"
        export XKBCOMMON_CFLAGS="-I$SYSROOT_EXT_INC"
        export XKBCOMMON_LIBS="-L$SYSROOT_EXT_LIB -lxkbcommon"
        export XKBREGISTRY_CFLAGS="-I$SYSROOT_EXT_INC"
        export XKBREGISTRY_LIBS="-L$SYSROOT_EXT_LIB -lxkbregistry"
        export WAYLAND_SCANNER="$WAYLAND_SCANNER"

        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
        CXX="$CLANGXX --target=$TARGET --sysroot=$SYSROOT" \
        CPP="$CLANG --target=$TARGET --sysroot=$SYSROOT -E" \
        LD="$CLANG --target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld" \
        AR="$LLVM_AR" STRIP="$LLVM_STRIP" \
        ../configure \
            --build="$(gcc -dumpmachine)" \
            --host="$TARGET" \
            --prefix=/opt/winehua \
            --libdir='${prefix}' \
            --with-wine-tools=../build-native \
            --with-mingw=gcc \
            --disable-tests \
            --without-x --without-alsa \
            --without-opengl --without-vulkan
        sed -i 's/#define HAVE_LINUX_NTSYNC_H 1/\/\* OHOS \*\/\n#undef HAVE_LINUX_NTSYNC_H/' include/config.h
        sed -i 's/#define HAVE_NETIPX_IPX_H 1/\/\* OHOS \*\/\n#undef HAVE_NETIPX_IPX_H/' include/config.h
    fi

    make -k -j"$JOBS" \
        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
        CFLAGS="$WINE_CFLAGS -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2" \
        LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB" || true
}

build_wineserver() {
    log "--- Build wineserver ---"
    local out="$BUILD_DIR/wine_server"
    local bindir datadir
    local srv_target="$NATIVE_TARGET"
    local target_binary
    local wine_include="-I$WINE_SRC/include -I$WINE_SRC/include/wine -I$WINE_SRC/server -I$WINE_SRC/build-ohos/include"

    if [ "$DEVICE_TYPE" = "pad" ]; then
        bindir="$WINE_DEVICE_ROOT/bin"
        datadir="$WINE_DEVICE_ROOT/share"
    else
        bindir="/opt/winehua/bin"
        datadir="/opt/winehua/share"
    fi

    if [ "$DEVICE_TYPE" = "pad" ] && [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        srv_target="$TARGET"
    fi

    local srv_cflags="--target=$srv_target --sysroot=$SYSROOT -D__MUSL__ -D_GNU_SOURCE \
        -DWINE_UNIX_LIB -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
        -D__ANDROID__ -D__OHOS__ -DBINDIR=\"$bindir\" -DDATADIR=\"$datadir\" \
        -fPIC $wine_include"

    mkdir -p "$out"
    target_binary="$out/wineserver"
    if [ "$DEVICE_TYPE" = "pad" ] && [ "$NATIVE_ARCH" != "arm64-v8a" ]; then
        target_binary="$out/libwineserver.so"
    fi

    local need_rebuild=0
    if [ ! -f "$target_binary" ]; then
        need_rebuild=1
    else
        for f in $WINE_SRC/server/*.c; do
            [ "$f" -nt "$target_binary" ] && { need_rebuild=1; break; }
        done
    fi

    if [ $need_rebuild -eq 0 ]; then
        if [ -f "$out/libwineserver.so" ] && [ ! -f "$NATIVE_LIBS/libwineserver.so" ]; then
            mkdir -p "$NATIVE_LIBS"
            cp "$out/libwineserver.so" "$NATIVE_LIBS/"
        fi
        return
    fi

    for f in $WINE_SRC/server/*.c; do
        $CLANG $srv_cflags -c -o "$out/$(basename "$f" .c).o" "$f"
    done

    if [ "$DEVICE_TYPE" = "pad" ] && [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        $CLANG --target=$TARGET --sysroot=$SYSROOT -fuse-ld=lld -pie \
            -o "$out/wineserver" "$out"/*.o -lm
        log "wineserver: $out/wineserver"
    elif [ "$DEVICE_TYPE" = "pad" ]; then
        $CLANG --target=$NATIVE_TARGET --sysroot=$SYSROOT -fuse-ld=lld \
            -shared -Wl,-soname,libwineserver.so \
            -o "$out/libwineserver.so" "$out"/*.o -lm
        mkdir -p "$NATIVE_LIBS"
        cp "$out/libwineserver.so" "$NATIVE_LIBS/"
        log "wineserver: $NATIVE_LIBS/libwineserver.so"
    else
        $CLANG --target=$NATIVE_TARGET --sysroot=$SYSROOT -fuse-ld=lld \
            -o "$out/wineserver" "$out"/*.o -lm
        log "wineserver: $out/wineserver"
    fi
}

log "=== Build Wine ==="

describe_ohaudio_runtime_profile
normalize_wine_scripts
refresh_wine_configure
build_native_tools
build_ohos_unix
build_wineserver

log "Wine build complete"

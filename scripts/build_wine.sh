#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

WINE_CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -D__ANDROID__ -D__OHOS__ -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables $PAD_CFLAGS"

ENABLE_OPENGL="${ENABLE_OPENGL:-0}"
ENABLE_VULKAN="${ENABLE_VULKAN:-0}"
WINE_BUILD_SCOPE="${WINE_BUILD_SCOPE:-full}"
NATIVE_ENABLE_OPENGL=0
NATIVE_ENABLE_VULKAN=0
WINE_NATIVE_GRAPHICS_ARGS=(--without-opengl --without-vulkan)
WINE_OHOS_GRAPHICS_ARGS=()
if [ "$ENABLE_OPENGL" = "1" ]; then
    WINE_OHOS_GRAPHICS_ARGS+=(--with-opengl)
else
    WINE_OHOS_GRAPHICS_ARGS+=(--without-opengl)
fi

case "$WINE_BUILD_SCOPE" in
    full|graphics-smoke)
        ;;
    *)
        err "unsupported WINE_BUILD_SCOPE=$WINE_BUILD_SCOPE (expected: full | graphics-smoke)"
        ;;
esac
if [ "$ENABLE_VULKAN" = "1" ]; then
    WINE_OHOS_GRAPHICS_ARGS+=(--with-vulkan)
else
    WINE_OHOS_GRAPHICS_ARGS+=(--without-vulkan)
fi

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
    elif ! grep -q 'programs/winehua_graphics_smoke' "$WINE_SRC/configure" 2>/dev/null; then
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

configure_ohos_graphics_env() {
    if [ "$ENABLE_OPENGL" != "1" ]; then
        return 0
    fi

    export WAYLAND_EGL_CFLAGS="-I$SYSROOT_EXT_INC"
    export WAYLAND_EGL_LIBS="-L$SYSROOT_EXT_LIB -lwayland-egl"
    export ac_cv_header_wayland_egl_h=yes
    export ac_cv_lib_wayland_egl_wl_egl_window_create=yes
    export EGL_CFLAGS=""
    export EGL_LIBS="-lEGL"
    export ac_cv_header_EGL_egl_h=yes
    export ac_cv_lib_soname_EGL="libEGL.so"
    log "Experimental OpenGL path enabled for Wine build"
}

build_dir_matches_graphics_args() {
    local build_dir="$1"
    local want_opengl="$2"
    local want_vulkan="$3"
    local cfg="$build_dir/config.log"

    [ -f "$cfg" ] || return 1

    if [ "$want_opengl" = "1" ]; then
        grep -q -- '--with-opengl' "$cfg" || return 1
    else
        grep -q -- '--without-opengl' "$cfg" || return 1
    fi

    if [ "$want_vulkan" = "1" ]; then
        grep -q -- '--with-vulkan' "$cfg" || return 1
    else
        grep -q -- '--without-vulkan' "$cfg" || return 1
    fi

    return 0
}

build_dir_has_wrong_host_path_style() {
    local build_dir="$1"
    local cfg="$build_dir/config.log"

    [ -f "$cfg" ] || return 1

    if [ "$HOST_SHELL" = "msys2" ] && grep -q '/mnt/[A-Za-z]/' "$cfg"; then
        return 0
    fi

    if [ "$HOST_SHELL" = "wsl" ] && grep -qE '(^|[ =])/[A-Za-z]/' "$cfg"; then
        return 0
    fi

    return 1
}

prepare_wine_build_dir() {
    local build_dir="$1"
    local want_opengl="$2"
    local want_vulkan="$3"
    local reason=""

    if [ -f "$build_dir/Makefile" ]; then
        if ! build_dir_matches_graphics_args "$build_dir" "$want_opengl" "$want_vulkan"; then
            reason="graphics configure flags changed"
        elif build_dir_has_wrong_host_path_style "$build_dir"; then
            reason="host path style changed"
        fi
    fi

    if [ -n "$reason" ]; then
        log "Reconfiguring $(basename "$build_dir") because $reason"
        remove_tree "$build_dir"
    fi

    mkdir -p "$build_dir"
}

build_native_tools() {
    log "--- Build native Wine tools ---"
    prepare_wine_build_dir "$WINE_SRC/build-native" "$NATIVE_ENABLE_OPENGL" "$NATIVE_ENABLE_VULKAN"
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

        if [ "$NATIVE_ENABLE_OPENGL" = "1" ]; then
            if configure_host_pkg \
                wayland-egl \
                WAYLAND_EGL_CFLAGS \
                WAYLAND_EGL_LIBS \
                ac_cv_header_wayland_egl_h \
                ac_cv_lib_wayland_egl_wl_egl_window_create; then
                log "Host wayland-egl support detected via pkg-config"
            else
                warn "Host wayland-egl support not found; native Wine OpenGL helpers may stay disabled"
                disable_host_pkg \
                    ac_cv_header_wayland_egl_h \
                    ac_cv_lib_wayland_egl_wl_egl_window_create \
                    WAYLAND_EGL_CFLAGS \
                    WAYLAND_EGL_LIBS
            fi
        fi

        export WAYLAND_SCANNER="$WAYLAND_SCANNER"
        ../configure --enable-win64 --disable-tests \
            --without-x --without-freetype --without-alsa \
            "${WINE_NATIVE_GRAPHICS_ARGS[@]}"
    fi

    make -j"$JOBS" \
        tools/winebuild/winebuild.exe \
        tools/widl/widl.exe \
        tools/winegcc/winegcc.exe \
        tools/wrc/wrc.exe \
        tools/wmc/wmc.exe \
        tools/sfnt2fon/sfnt2fon.exe \
        tools/make_xftmpl.exe

    make -j"$JOBS" \
        loader/wine.inf \
        nls/all \
        include/all

    if [ ! -f tools/wine/wine.exe ]; then
        warn "Creating placeholder native tools/wine/wine.exe for cross-build dependencies"
        mkdir -p tools/wine
        : > tools/wine/wine.exe
    fi
}

build_ohos_unix() {
    log "--- Build OHOS Wine unix side ---"
    local make_env
    local final_targets=()
    local required_pe=()
    local pe_path=""
    local need_reconfigure=0

    prepare_wine_build_dir "$WINE_SRC/build-ohos" "$ENABLE_OPENGL" "$ENABLE_VULKAN"
    cd "$WINE_SRC/build-ohos"

    if [ ! -f "Makefile" ]; then
        need_reconfigure=1
    elif [ "$WINE_BUILD_SCOPE" != "graphics-smoke" ] && {
        ! grep -q '#define SONAME_LIBFREETYPE' include/config.h 2>/dev/null ||
        ! grep -q '#define SONAME_LIBWAYLAND_CLIENT' include/config.h 2>/dev/null ||
        ! grep -Eq 'dlls/wineohos\.drv/wineohos\.so:.*dlls/wineohos\.drv/ohos_audio_client\.o' Makefile 2>/dev/null
    }; then
        need_reconfigure=1
    elif [ "$WINE_BUILD_SCOPE" = "graphics-smoke" ] && {
        ! grep -q '#define SONAME_LIBFREETYPE' include/config.h 2>/dev/null ||
        ! grep -Eq 'dlls/wineohos\.drv/wineohos\.so:.*dlls/wineohos\.drv/ohos_audio_client\.o' Makefile 2>/dev/null
    }; then
        warn "Reusing existing build-ohos for graphics-smoke despite missing full runtime markers"
    fi

    if [ "$need_reconfigure" -eq 1 ]; then
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
        configure_ohos_graphics_env

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
            "${WINE_OHOS_GRAPHICS_ARGS[@]}"
        sed -i 's/#define HAVE_LINUX_NTSYNC_H 1/\/\* OHOS \*\/\n#undef HAVE_LINUX_NTSYNC_H/' include/config.h
        sed -i 's/#define HAVE_NETIPX_IPX_H 1/\/\* OHOS \*\/\n#undef HAVE_NETIPX_IPX_H/' include/config.h
    fi

    make_env=(
        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT"
        CFLAGS="$WINE_CFLAGS -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2"
        LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB"
    )

    if [ "$WINE_BUILD_SCOPE" = "graphics-smoke" ]; then
        log "Fast Wine scope enabled: rebuilding graphics smoke target only"
        final_targets=(
            programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe
        )
        required_pe=(
            "$WINE_SRC/build-ohos/programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe"
        )
    else
        make -k -j"$JOBS" "${make_env[@]}" || true
        final_targets=(
            dlls/kernel32/x86_64-windows/kernel32.dll
            dlls/kernelbase/x86_64-windows/kernelbase.dll
            programs/wineboot/x86_64-windows/wineboot.exe
            programs/explorer/x86_64-windows/explorer.exe
            programs/winehua_audio_smoke/x86_64-windows/winehua_audio_smoke.exe
            programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe
        )
        required_pe=(
            "$WINE_SRC/build-ohos/dlls/kernel32/x86_64-windows/kernel32.dll"
            "$WINE_SRC/build-ohos/dlls/kernelbase/x86_64-windows/kernelbase.dll"
            "$WINE_SRC/build-ohos/programs/wineboot/x86_64-windows/wineboot.exe"
            "$WINE_SRC/build-ohos/programs/explorer/x86_64-windows/explorer.exe"
            "$WINE_SRC/build-ohos/programs/winehua_audio_smoke/x86_64-windows/winehua_audio_smoke.exe"
            "$WINE_SRC/build-ohos/programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe"
        )
    fi

    make -j"$JOBS" "${make_env[@]}" "${final_targets[@]}"

    for pe_path in "${required_pe[@]}"; do
        [ -f "$pe_path" ] || err "missing required Wine PE artifact after build: $pe_path"
    done
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
if [ "$WINE_BUILD_SCOPE" = "graphics-smoke" ]; then
    log "Fast Wine scope enabled: skipping source normalization scan"
else
    normalize_wine_scripts
fi
refresh_wine_configure
build_native_tools
build_ohos_unix
build_wineserver

log "Wine build complete"

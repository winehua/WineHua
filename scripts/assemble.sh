#!/bin/bash
set -euo pipefail
shopt -s nullglob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

count_files() {
    local dir="$1"
    if [ ! -d "$dir" ]; then
        echo 0
        return 0
    fi
    find "$dir" -maxdepth 1 -type f | wc -l
}

path_newer_than_stamp() {
    local stamp="$1"
    local path="$2"

    [ -e "$path" ] || return 1
    [ -f "$stamp" ] || return 0

    if [ -d "$path" ]; then
        find "$path" -type f -newer "$stamp" -print -quit 2>/dev/null | grep -q .
        return $?
    fi

    [ "$path" -nt "$stamp" ]
}

assemble_inputs_changed() {
    local stamp="$1"
    shift
    local path=""

    [ -f "$stamp" ] || return 0

    for path in "$@"; do
        if path_newer_than_stamp "$stamp" "$path"; then
            return 0
        fi
    done

    return 1
}

write_assemble_stamp() {
    local stamp="$1"
    mkdir -p "$(dirname "$stamp")"
    printf '%s\n' "$NATIVE_ARCH $DEVICE_TYPE $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$stamp"
}

copy_find_matches() {
    local src_root="$1"
    local dest="$2"
    shift 2
    local copied=0
    local path

    while IFS= read -r -d '' path; do
        cp "$path" "$dest/"
        copied=$((copied + 1))
    done < <(find "$src_root" -path '*/x86_64-windows/*' -type f \( "$@" \) -print0)

    echo "$copied"
}

pick_runtime_lib() {
    local name="$1"
    local soname="$2"
    local linker="${3:-}"
    local dest="$4"
    local candidate=""

    for candidate in \
        "$NATIVE_LIBS/$soname" \
        "$NATIVE_LIBS/$name" \
        "$SYSROOT_EXT_LIB/$soname" \
        "$SYSROOT_EXT_LIB/$name" \
        "$SYSROOT/usr/lib/x86_64-linux-ohos/$soname" \
        "$SYSROOT/usr/lib/x86_64-linux-ohos/$name"; do
        [ -f "$candidate" ] || continue
        cp "$candidate" "$dest/$soname"
        break
    done

    if [ ! -f "$dest/$soname" ]; then
        warn "$soname not found for runtime staging"
        return 0
    fi

    if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
        cp "$dest/$soname" "$dest/$linker"
    fi
}

bundle_alarm_wav() {
    local dest="$1"
    if [ -f "$ROOT/assets/windows-media/Alarm01.wav" ]; then
        cp "$ROOT/assets/windows-media/Alarm01.wav" "$dest/Alarm01.wav"
    else
        warn "Alarm01.wav not found at $ROOT/assets/windows-media/Alarm01.wav"
    fi
}

assemble_pad() {
    log "=== Assemble Pad layout ($NATIVE_ARCH) ==="

    local wine_data="$STAGING_DIR/wine-data"
    local rawfile_dir="$WINEHUA/entry/src/main/resources/rawfile"
    local zip_name="wine-data.zip"
    local wine_bin="$wine_data/bin"
    local wine_unix="$wine_bin/x86_64-unix"
    local wine_win="$wine_bin/x86_64-windows"
    local aarch64_lib="$SYSROOT_EXT/usr/lib/$NATIVE_TARGET"

    rm -rf "$STAGING_DIR" "$wine_data"
    mkdir -p "$wine_unix" "$wine_win" "$wine_data/share/wine/nls" \
        "$wine_data/share/wine/fonts" "$wine_data/share/wine/winmd" \
        "$wine_data/share/X11" "$NATIVE_LIBS"

    if [ "$NATIVE_ARCH" = "x86_64" ]; then
        for so in "$WINE_SRC/build-ohos/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done

        pick_runtime_lib "libfreetype.so.6.20.2" "libfreetype.so.6" "libfreetype.so" "$NATIVE_LIBS"
        pick_runtime_lib "libz.so" "libz.so" "" "$NATIVE_LIBS"
        pick_runtime_lib "libwayland-client.so.0.22.0" "libwayland-client.so.0" "libwayland-client.so" "$NATIVE_LIBS"
        pick_runtime_lib "libwayland-server.so.0.22.0" "libwayland-server.so.0" "libwayland-server.so" "$NATIVE_LIBS"
        pick_runtime_lib "libxkbcommon.so.0.0.0" "libxkbcommon.so.0" "libxkbcommon.so" "$NATIVE_LIBS"
        pick_runtime_lib "libxkbregistry.so.0.0.0" "libxkbregistry.so.0" "libxkbregistry.so" "$NATIVE_LIBS"
        pick_runtime_lib "libxml2.so.2.12.0" "libxml2.so.2" "libxml2.so" "$NATIVE_LIBS"
        pick_runtime_lib "libffi.so.8.1.4" "libffi.so.8" "libffi.so" "$NATIVE_LIBS"
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$NATIVE_LIBS/"

        if [ -f "$BUILD_DIR/wine_server/libwineserver.so" ]; then
            cp "$BUILD_DIR/wine_server/libwineserver.so" "$NATIVE_LIBS/"
        else
            warn "libwineserver.so not found for x86_64 pad build"
        fi
    else
        pick_runtime_lib "libfreetype.so.6.20.2" "libfreetype.so.6" "libfreetype.so" "$wine_unix"
        pick_runtime_lib "libz.so" "libz.so" "" "$wine_unix"
        pick_runtime_lib "libwayland-client.so.0.22.0" "libwayland-client.so.0" "libwayland-client.so" "$wine_unix"
        pick_runtime_lib "libwayland-server.so.0.22.0" "libwayland-server.so.0" "libwayland-server.so" "$wine_unix"
        pick_runtime_lib "libxkbcommon.so.0.0.0" "libxkbcommon.so.0" "libxkbcommon.so" "$wine_unix"
        pick_runtime_lib "libxkbregistry.so.0.0.0" "libxkbregistry.so.0" "libxkbregistry.so" "$wine_unix"
        pick_runtime_lib "libxml2.so.2.12.0" "libxml2.so.2" "libxml2.so" "$wine_unix"
        pick_runtime_lib "libffi.so.8.1.4" "libffi.so.8" "libffi.so" "$wine_unix"

        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_bin/"
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_unix/"

        for so in "$WINE_SRC/build-ohos/dlls/"*/*.so; do
            if [ "$(basename "$so")" = "ntdll.so" ]; then
                cp "$so" "$wine_bin/"
            else
                cp "$so" "$wine_unix/"
            fi
        done
        cp "$WINE_SRC/build-ohos/loader/wine" "$wine_bin/"

        if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
            cp "$BUILD_DIR/wine_server/wineserver" "$wine_bin/"
        elif [ -f "$WINE_SRC/build-ohos/server/wineserver" ]; then
            cp "$WINE_SRC/build-ohos/server/wineserver" "$wine_bin/"
        fi

        for lib in \
            libfreetype.so.6 libfreetype.so \
            libxkbcommon.so.0 libxkbcommon.so \
            libxkbregistry.so.0 libxkbregistry.so \
            libxml2.so.2 libxml2.so \
            libwayland-client.so.0 libwayland-client.so \
            libwayland-server.so.0 libwayland-server.so \
            libffi.so.8 libffi.so; do
            [ -f "$wine_unix/$lib" ] || continue
            cp "$wine_unix/$lib" "$wine_bin/"
        done

        if [ -d "$aarch64_lib" ]; then
            for lib in \
                libfreetype.so.6 libxkbcommon.so.0 libxkbregistry.so.0 \
                libxml2.so.2 libwayland-client.so.0 libwayland-server.so.0 \
                libffi.so.8; do
                [ -f "$aarch64_lib/$lib" ] || continue
                cp "$aarch64_lib/$lib" "$NATIVE_LIBS/$lib"
            done
            for pair in \
                "libfreetype.so.6:libfreetype.so" \
                "libxkbcommon.so.0:libxkbcommon.so" \
                "libxkbregistry.so.0:libxkbregistry.so" \
                "libxml2.so.2:libxml2.so" \
                "libwayland-client.so.0:libwayland-client.so" \
                "libwayland-server.so.0:libwayland-server.so" \
                "libffi.so.8:libffi.so"; do
                src="${pair%%:*}"
                dst="${pair#*:}"
                [ -f "$NATIVE_LIBS/$src" ] || continue
                cp "$NATIVE_LIBS/$src" "$NATIVE_LIBS/$dst"
            done
        fi
    fi

    copy_find_matches "$WINE_SRC/build-ohos/dlls" "$wine_win" \
        -name '*.dll' -o -name '*.drv' -o -name '*.exe' -o -name '*.sys' \
        -o -name '*.cpl' -o -name '*.ocx' -o -name '*.tlb' -o -name '*.ax' \
        -o -name '*.acm' -o -name '*.com' >/dev/null
    copy_find_matches "$WINE_SRC/build-ohos/programs" "$wine_win" \
        -name '*.exe' -o -name '*.com' >/dev/null
    bundle_alarm_wav "$wine_win"

    cp "$WINE_SRC/fonts/"*.ttf "$wine_data/share/wine/fonts/"
    cp "$WINE_SRC/build-native/nls/"*.nls "$wine_data/share/wine/nls/"
    cp "$WINE_SRC/build-native/include/"*.winmd "$wine_data/share/wine/winmd/"
    cp "$WINE_SRC/build-native/loader/wine.inf" "$wine_data/share/wine/"
    sed -i '/^\[MCI\]$/i\
;; OHOS font substitutes\
HKLM,%FontSubStr%,"System",,"HarmonyOS Sans"\
HKLM,%FontSubStr%,"Fixedsys",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"MS Sans Serif",,"HarmonyOS Sans"\
HKLM,%FontSubStr%,"Courier",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier New",,"Noto Sans Mono"' "$wine_data/share/wine/wine.inf"

    if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ]; then
        cp -rL "$SYSROOT_EXT_SHARE/X11/xkb" "$wine_data/share/X11/"
    fi

    mkdir -p "$rawfile_dir"
    (
        cd "$wine_data"
        rm -f "$STAGING_DIR/$zip_name"
        zip -qr "$STAGING_DIR/$zip_name" .
    )
    cp "$STAGING_DIR/$zip_name" "$rawfile_dir/"
    log "pad rawfile -> $rawfile_dir/$zip_name"
}

log "=== Assemble layout ($NATIVE_ARCH, $DEVICE_TYPE) ==="

BIN="$HNP_LAYOUT/bin"
UNIX_DIR="$BIN/x86_64-unix"
WIN_DIR="$BIN/x86_64-windows"
LIB_DIR="$HNP_LAYOUT/lib/x86_64"
SHARE_WINE="$HNP_LAYOUT/share/wine"

ASSEMBLE_STAMP="$OUT_DIR/.assemble-${DEVICE_TYPE}-${NATIVE_ARCH}.stamp"
ASSEMBLE_PROBE="$HNP_LAYOUT/bin/wine"
if [ "$DEVICE_TYPE" = "pad" ]; then
    ASSEMBLE_PROBE="$STAGING_DIR/wine-data.zip"
fi

ASSEMBLE_INPUTS=(
    "$WINE_SRC/build-ohos/loader/wine"
    "$WINE_SRC/build-ohos/dlls"
    "$WINE_SRC/build-ohos/programs"
    "$WINE_SRC/fonts"
    "$WINE_SRC/build-native/nls"
    "$WINE_SRC/build-native/include"
    "$WINE_SRC/build-native/loader/wine.inf"
    "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so"
    "$SYSROOT_EXT_LIB"
    "$SYSROOT_EXT_SHARE/X11/xkb"
    "$ROOT/assets/windows-media/Alarm01.wav"
    "$ROOT/.temp/mmap_test"
)
if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
    ASSEMBLE_INPUTS+=("$BUILD_DIR/wine_server/wineserver")
elif [ -f "$WINE_SRC/build-ohos/server/wineserver" ]; then
    ASSEMBLE_INPUTS+=("$WINE_SRC/build-ohos/server/wineserver")
fi
if [ -f "$BUILD_DIR/wine_server/libwineserver.so" ]; then
    ASSEMBLE_INPUTS+=("$BUILD_DIR/wine_server/libwineserver.so")
fi
if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
    ASSEMBLE_INPUTS+=(
        "$BUILD_DIR/box64_build/box64"
        "$SYSROOT_EXT/usr/lib/$NATIVE_TARGET"
    )
fi

if [ -f "$ASSEMBLE_STAMP" ] && [ -e "$ASSEMBLE_PROBE" ] && ! assemble_inputs_changed "$ASSEMBLE_STAMP" "${ASSEMBLE_INPUTS[@]}"; then
    log "HNP layout already up to date ($NATIVE_ARCH)"
    echo
    if [ "$DEVICE_TYPE" = "pad" ]; then
        echo "  rawfile: $STAGING_DIR/wine-data.zip"
    else
        echo "  $BIN/"
        echo "  core: wine, wineserver, box64"
        echo "  ntdll.so: wine loader runtime"
        echo "  x86_64-windows/: $(count_files "$WIN_DIR") files"
        echo "  x86_64-unix/: $(count_files "$UNIX_DIR") files"
    fi
    exit 0
fi

if [ "$DEVICE_TYPE" = "pad" ]; then
    assemble_pad
    write_assemble_stamp "$ASSEMBLE_STAMP"
    exit 0
fi

rm -rf "$STAGING_DIR"
mkdir -p "$BIN" "$UNIX_DIR" "$WIN_DIR" "$LIB_DIR" \
    "$SHARE_WINE/nls" "$SHARE_WINE/fonts" "$SHARE_WINE/winmd" "$HNP_LAYOUT/share/X11"

cp "$WINE_SRC/build-ohos/loader/wine" "$BIN/"

if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
    cp "$BUILD_DIR/wine_server/wineserver" "$BIN/"
elif [ -f "$WINE_SRC/build-ohos/server/wineserver" ]; then
    cp "$WINE_SRC/build-ohos/server/wineserver" "$BIN/"
else
    err "wineserver not found; run bash scripts/build_wine.sh first"
fi

if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
    if [ -f "$BUILD_DIR/box64_build/box64" ]; then
        cp "$BUILD_DIR/box64_build/box64" "$BIN/"
    else
        err "box64 not found; run bash scripts/build_box64.sh first"
    fi
else
    cat > "$BIN/box64" <<'BOXWRAP'
#!/bin/sh
DIR="${0%/*}"
[ "$DIR" = "$0" ] && DIR="."
[ $# -lt 1 ] && { echo "Usage: $0 <program> [args...]" >&2; exit 1; }
prog="$1"
shift
exec "$DIR/$prog" "$@"
BOXWRAP
    chmod +x "$BIN/box64"
fi

cp "$WINE_SRC/build-ohos/dlls/ntdll/ntdll.so" "$BIN/"
for so in "$WINE_SRC/build-ohos/dlls/"*/*.so; do
    [ "$(basename "$so")" = "ntdll.so" ] && continue
    cp "$so" "$UNIX_DIR/"
done

copy_find_matches "$WINE_SRC/build-ohos/dlls" "$WIN_DIR" \
    -name '*.dll' -o -name '*.drv' -o -name '*.exe' -o -name '*.sys' \
    -o -name '*.cpl' -o -name '*.ocx' -o -name '*.tlb' -o -name '*.ax' \
    -o -name '*.acm' -o -name '*.com' >/dev/null
copy_find_matches "$WINE_SRC/build-ohos/programs" "$WIN_DIR" \
    -name '*.exe' -o -name '*.com' >/dev/null
bundle_alarm_wav "$WIN_DIR"

cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$LIB_DIR/"

pick_runtime_lib "libfreetype.so.6.20.2" "libfreetype.so.6" "libfreetype.so" "$UNIX_DIR"
pick_runtime_lib "libz.so" "libz.so" "" "$UNIX_DIR"
pick_runtime_lib "libwayland-client.so.0.22.0" "libwayland-client.so.0" "libwayland-client.so" "$UNIX_DIR"
pick_runtime_lib "libwayland-server.so.0.22.0" "libwayland-server.so.0" "libwayland-server.so" "$UNIX_DIR"
pick_runtime_lib "libxkbcommon.so.0.0.0" "libxkbcommon.so.0" "libxkbcommon.so" "$UNIX_DIR"
pick_runtime_lib "libxkbregistry.so.0.0.0" "libxkbregistry.so.0" "libxkbregistry.so" "$UNIX_DIR"
pick_runtime_lib "libxml2.so.2.12.0" "libxml2.so.2" "libxml2.so" "$UNIX_DIR"
pick_runtime_lib "libffi.so.8.1.4" "libffi.so.8" "libffi.so" "$UNIX_DIR"

if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
    mkdir -p "$HNP_LAYOUT/lib/arm64-v8a"
    local_aarch64_lib="$SYSROOT_EXT/usr/lib/$NATIVE_TARGET"
    for pair in \
        "libfreetype.so.6:libfreetype.so" \
        "libxkbcommon.so.0:libxkbcommon.so" \
        "libxkbregistry.so.0:libxkbregistry.so" \
        "libxml2.so.2:libxml2.so" \
        "libwayland-client.so.0:libwayland-client.so" \
        "libffi.so.8:libffi.so"; do
        src="${pair%%:*}"
        dst="${pair#*:}"
        [ -f "$local_aarch64_lib/$src" ] || continue
        cp "$local_aarch64_lib/$src" "$HNP_LAYOUT/lib/arm64-v8a/$src"
        cp "$local_aarch64_lib/$src" "$HNP_LAYOUT/lib/arm64-v8a/$dst"
    done
fi

cp "$WINE_SRC/fonts/"*.ttf "$SHARE_WINE/fonts/"
cp "$WINE_SRC/build-native/nls/"*.nls "$SHARE_WINE/nls/"
cp "$WINE_SRC/build-native/include/"*.winmd "$SHARE_WINE/winmd/"
cp "$WINE_SRC/build-native/loader/wine.inf" "$SHARE_WINE/"
sed -i '/^\[MCI\]$/i\
;; OHOS font substitutes\
HKLM,%FontSubStr%,"System",,"HarmonyOS Sans"\
HKLM,%FontSubStr%,"Fixedsys",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"MS Sans Serif",,"HarmonyOS Sans"\
HKLM,%FontSubStr%,"Courier",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier New",,"Noto Sans Mono"' "$SHARE_WINE/wine.inf"

if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ]; then
    cp -rL "$SYSROOT_EXT_SHARE/X11/xkb" "$HNP_LAYOUT/share/X11/"
fi

if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
    cat > "$BIN/wine.sh" <<'SCRIPT'
#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
export WINEDLLPATH="$DIR"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
export BOX64_LD_LIBRARY_PATH="$DIR:$DIR/x86_64-unix:$DIR/../lib/x86_64"
exec "$DIR/box64" "$DIR/wine" "$@"
SCRIPT
else
    cat > "$BIN/wine.sh" <<'SCRIPT'
#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
export WINEDLLPATH="$DIR"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
export LD_LIBRARY_PATH="$DIR:$DIR/x86_64-unix:$DIR/../lib/x86_64"
exec "$DIR/wine" "$@"
SCRIPT
fi
chmod +x "$BIN/wine.sh"

if [ -f "$ROOT/.temp/mmap_test" ]; then
    cp "$ROOT/.temp/mmap_test" "$BIN/"
fi

log "HNP layout ready ($NATIVE_ARCH)"
echo
echo "  $BIN/"
echo "  core: wine, wineserver, box64"
echo "  ntdll.so: wine loader runtime"
echo "  x86_64-windows/: $(count_files "$WIN_DIR") files"
echo "  x86_64-unix/: $(count_files "$UNIX_DIR") files"
write_assemble_stamp "$ASSEMBLE_STAMP"

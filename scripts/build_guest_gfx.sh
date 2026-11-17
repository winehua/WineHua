#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/env.sh"

remove_tree() {
    local path="$1"
    [ -e "$path" ] || return 0
    chmod -R u+w "$path" 2>/dev/null || true
    rm -rf "$path" 2>/dev/null || true
    [ ! -e "$path" ] || err "failed to remove directory: $path"
}

MODE="${GUEST_GFX_MODE:-virpipe}"
INSTALL_ROOT="${WINEHUA_GUEST_GFX_INSTALL_ROOT:-${GUEST_GFX_INSTALL_ROOT:-}}"
MESA_SOURCE_ROOT="${WINEHUA_OHOS_MESA_SOURCE_ROOT:-${GUEST_GFX_MESA_SOURCE_ROOT:-}}"
LIBDRM_SOURCE_ROOT="${WINEHUA_OHOS_LIBDRM_SOURCE_ROOT:-${GUEST_GFX_LIBDRM_SOURCE_ROOT:-}}"
OUTPUT_ROOT="${WINEHUA_GUEST_GFX_OUTPUT_ROOT:-}"
CLEAN=0

usage() {
    cat <<'EOF'
Usage:
  bash scripts/build_guest_gfx.sh [--install-root <mesa-install>] [--mode virpipe|zink] [--output-root <dir>] [--clean]

Notes:
  - This script packages an already-built OHOS Mesa install tree into
    build/guest_gfx/<arch>/ so assemble.sh can stage it into rawfile.
  - The install root must contain OHOS-target runtime files, not regular Linux
    desktop libraries from WSL/Ubuntu.
  - Requires thirdparty/mesa and thirdparty/libdrm submodules (gitee OpenHarmony mirrors).
  - BUILD_INFO.txt records the Mesa/libdrm source roots and Git HEADs when the
    caller provides those source paths.
  - For Step 1 VirGL/vtest smoke, use --mode virpipe.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --install-root)
            [ $# -ge 2 ] || err "--install-root requires a value"
            INSTALL_ROOT="$2"
            shift
            ;;
        --mode)
            [ $# -ge 2 ] || err "--mode requires a value"
            MODE="$2"
            shift
            ;;
        --output-root)
            [ $# -ge 2 ] || err "--output-root requires a value"
            OUTPUT_ROOT="$2"
            shift
            ;;
        --clean)
            CLEAN=1
            ;;
        -h|--help|help)
            usage
            exit 0
            ;;
        *)
            err "unknown option: $1"
            ;;
    esac
    shift
done

case "$MODE" in
    virpipe) OUTPUT_ROOT="${OUTPUT_ROOT:-$BUILD_DIR/guest_gfx/${GUEST_ARCH:-$WINE_ARCH}}" ;;
    zink) OUTPUT_ROOT="${OUTPUT_ROOT:-$BUILD_DIR/guest_gfx_zink/${GUEST_ARCH:-$WINE_ARCH}}" ;;
    *) err "unsupported guest_gfx mode: $MODE" ;;
esac

if [ -n "${GUEST_ARCH:-}" ] && [ "$GUEST_ARCH" != "$WINE_ARCH" ]; then
    err "GUEST_ARCH=$GUEST_ARCH differs from WINE_ARCH=$WINE_ARCH; Mesa and support libraries must use the same architecture"
fi

if [ "$NATIVE_ARCH" = "all" ]; then
    err "build_guest_gfx.sh requires a concrete NATIVE_ARCH (x86_64 or arm64-v8a)"
fi

find_first_existing_file() {
    local candidate=""

    for candidate in "$@"; do
        [ -n "${candidate:-}" ] || continue
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

find_runtime_lib_dir() {
    local install_root="$1"

    find_first_existing_dir \
        "$install_root/lib" \
        "$install_root/usr/lib" \
        "$install_root/lib64" \
        "$install_root/usr/lib64"
}

git_metadata_value() {
    local repo_root="$1"
    shift
    git -c "safe.directory=$repo_root" -C "$repo_root" "$@" 2>/dev/null | head -n 1 || true
}

append_git_source_info() {
    local prefix="$1"
    local repo_root="$2"
    local head=""
    local branch=""
    local remote=""

    [ -n "$repo_root" ] || return 0
    [ -d "$repo_root" ] || return 0

    printf '%s_source_root=%s\n' "$prefix" "$repo_root"

    head="$(git_metadata_value "$repo_root" rev-parse --verify HEAD)"
    [ -n "$head" ] || return 0
    printf '%s_git_head=%s\n' "$prefix" "$head"

    branch="$(git_metadata_value "$repo_root" rev-parse --abbrev-ref HEAD)"
    if [ -n "$branch" ]; then
        printf '%s_git_branch=%s\n' "$prefix" "$branch"
    fi

    remote="$(git_metadata_value "$repo_root" config --get remote.origin.url)"
    if [ -n "$remote" ]; then
        printf '%s_git_remote=%s\n' "$prefix" "$remote"
    fi
}

copy_tree_if_present() {
    local src="$1"
    local dst="$2"

    [ -d "$src" ] || return 0
    mkdir -p "$dst"
    cp -rL "$src"/. "$dst/"
}

copy_support_lib_if_present() {
    local dest_name="$1"
    shift
    local candidate=""

    [ -f "$OUTPUT_ROOT/lib/$dest_name" ] && return 0

    candidate="$(find_first_existing_file "$@" || true)"
    [ -n "$candidate" ] || return 0
    cp -L "$candidate" "$OUTPUT_ROOT/lib/$dest_name"
}

copy_guest_runtime_lib_if_present() {
    local dest_name="$1"
    shift
    local candidate=""

    if [ "$dest_name" = libz.so ]; then
        local zlib_arch="${GUEST_ARCH:-$WINE_ARCH}"
        local zlib_ext="$SYSROOT_EXT_LIB"
        if [ -f "$BUILD_DIR/zlib-runtime/$zlib_arch/lib/libz.so" ]; then
            zlib_ext="$BUILD_DIR/zlib-runtime/$zlib_arch/lib"
        fi
        python3 "$SCRIPT_DIR/copy_runtime_library.py" \
            --ext "$zlib_ext" --sdk "$SYSROOT/usr/lib/$zlib_arch-linux-ohos" \
            --dest "$OUTPUT_ROOT/lib" libz.so libz.so
        return
    fi
    [ -f "$OUTPUT_ROOT/lib/$dest_name" ] && return 0

    candidate="$(find_first_existing_file "$@" || true)"
    [ -n "$candidate" ] || return 0
    cp -L "$candidate" "$OUTPUT_ROOT/lib/$dest_name"
}

materialize_bundle_alias() {
    local alias_name="$1"
    shift
    local src=""
    local prefix=""

    if [ -f "$OUTPUT_ROOT/lib/$alias_name" ]; then
        return 0
    fi

    for prefix in "$@"; do
        src="$(find "$OUTPUT_ROOT/lib" -maxdepth 1 \( -type f -o -type l \) -name "${prefix}*" | sort | head -n 1)"
        [ -n "$src" ] || continue
        cp -L "$src" "$OUTPUT_ROOT/lib/$alias_name"
        return 0
    done

    return 0
}

materialize_dri_driver_alias() {
    local alias_name="$1"
    local src=""

    [ -f "$OUTPUT_ROOT/lib/dri/$alias_name" ] && return 0
    mkdir -p "$OUTPUT_ROOT/lib/dri"

    for src in \
        "$OUTPUT_ROOT/lib/dri/gallium_dri.so" \
        "$OUTPUT_ROOT/lib/libgallium_dri.so" \
        "$OUTPUT_ROOT/lib"/libgallium-*.so; do
        [ -f "$src" ] || continue
        cp -L "$src" "$OUTPUT_ROOT/lib/dri/$alias_name"
        return 0
    done

    return 0
}

emit_env_file() {
    local env_file="$1"
    local bundle_root="$2"
    local lib_dir="$bundle_root/lib"

    cat > "$env_file" <<EOF
# Auto-generated by scripts/build_guest_gfx.sh
# For virgl/vtest, GraphicsBroker injects VTEST_SOCKET_NAME at launch time.
WINEHUA_GUEST_GFX_MODE=mesa-$MODE
WINEHUA_GUEST_GFX_PLATFORM=${WINEHUA_GUEST_GFX_PLATFORM:-unknown}
EOF

    case "$MODE" in
        virpipe)
            cat >> "$env_file" <<'EOF'
LIBGL_ALWAYS_SOFTWARE=1
MESA_LOADER_DRIVER_OVERRIDE=swrast
GALLIUM_DRIVER=virpipe
EOF
            ;;
        zink)
            cat >> "$env_file" <<'EOF'
MESA_LOADER_DRIVER_OVERRIDE=zink
GALLIUM_DRIVER=zink
EOF
            ;;
        *)
            err "unsupported guest_gfx mode: $MODE"
            ;;
    esac

    if [ -d "$lib_dir/dri" ]; then
        cat >> "$env_file" <<'EOF'
LIBGL_DRIVERS_PATH=$ORIGIN/lib/dri
EOF
    fi

    if [ -d "$lib_dir/egl" ]; then
        cat >> "$env_file" <<'EOF'
EGL_DRIVERS_PATH=$ORIGIN/lib/egl
EOF
    fi

    if [ -d "$lib_dir/egl_vendor.d" ]; then
        cat >> "$env_file" <<'EOF'
__EGL_VENDOR_LIBRARY_DIRS=$ORIGIN/lib/egl_vendor.d
EOF
    fi
}

INSTALL_ROOT="${INSTALL_ROOT:-$(find_first_existing_dir \
    "$ROOT/build/guest_gfx_install/$NATIVE_ARCH" \
    "$ROOT/build/mesa-install/$NATIVE_ARCH" \
    "$ROOT/build/mesa-install/$NATIVE_ARCH/install" \
    || true)}"

[ -n "$INSTALL_ROOT" ] || err "guest_gfx install root not provided. Set WINEHUA_GUEST_GFX_INSTALL_ROOT or pass --install-root with an OHOS Mesa install tree."
[ -d "$INSTALL_ROOT" ] || err "guest_gfx install root does not exist: $INSTALL_ROOT"

RUNTIME_LIB_DIR="$(find_runtime_lib_dir "$INSTALL_ROOT" || true)"
[ -n "$RUNTIME_LIB_DIR" ] || err "unable to locate lib/ under guest_gfx install root: $INSTALL_ROOT"

# Mesa 25 uses one Gallium library and may not install per-driver aliases.
# Verify build options AND the installed library hash before creating aliases;
# a VirGL-only Gallium library cannot become Zink merely by renaming it.
if [ "$MODE" = "zink" ]; then
    python3 "$SCRIPT_DIR/guest_gfx_build_identity.py" verify-zink \
        --install-root "$INSTALL_ROOT" --arch "$WINE_ARCH"
fi

log "=== Package guest_gfx bundle ($NATIVE_ARCH) ==="
log "install root: $INSTALL_ROOT"
log "runtime lib dir: $RUNTIME_LIB_DIR"
log "mode: $MODE"
log "output: $OUTPUT_ROOT"

if [ "$CLEAN" -eq 1 ]; then
    remove_tree "$OUTPUT_ROOT"
fi

remove_tree "$OUTPUT_ROOT"
mkdir -p "$OUTPUT_ROOT/lib"

find "$RUNTIME_LIB_DIR" -maxdepth 1 -type f \( -name 'lib*.so' -o -name 'lib*.so.*' \) -exec cp -L {} "$OUTPUT_ROOT/lib/" \;
find "$RUNTIME_LIB_DIR" -maxdepth 1 -type l \( -name 'lib*.so' -o -name 'lib*.so.*' \) -exec cp -L {} "$OUTPUT_ROOT/lib/" \;

copy_tree_if_present "$RUNTIME_LIB_DIR/dri" "$OUTPUT_ROOT/lib/dri"
copy_tree_if_present "$RUNTIME_LIB_DIR/egl" "$OUTPUT_ROOT/lib/egl"
copy_tree_if_present "$RUNTIME_LIB_DIR/egl_vendor.d" "$OUTPUT_ROOT/lib/egl_vendor.d"
copy_tree_if_present "$RUNTIME_LIB_DIR/gallium" "$OUTPUT_ROOT/lib/gallium"

# 所有支撑库由 build_ohos_guest_gfx.sh provision 到 sysroot-ext, 单一来源
SYSROOT_EXT_LIB="$BUILD_DIR/sysroot-ext/usr/lib/$TARGET"
copy_support_lib_if_present "libdrm.so.2" \
    "$SYSROOT_EXT_LIB/libdrm.so.2" \
    "$SYSROOT_EXT_LIB/libdrm.so.2.4.0"
copy_support_lib_if_present "libdrm.so" \
    "$SYSROOT_EXT_LIB/libdrm.so" \
    "$SYSROOT_EXT_LIB/libdrm.so.2" \
    "$SYSROOT_EXT_LIB/libdrm.so.2.4.0"
copy_support_lib_if_present "libwayland-client.so.0" \
    "$SYSROOT_EXT_LIB/libwayland-client.so.0"
copy_support_lib_if_present "libwayland-client.so" \
    "$SYSROOT_EXT_LIB/libwayland-client.so" \
    "$SYSROOT_EXT_LIB/libwayland-client.so.0"
copy_support_lib_if_present "libwayland-server.so.0" \
    "$SYSROOT_EXT_LIB/libwayland-server.so.0"
copy_support_lib_if_present "libwayland-server.so" \
    "$SYSROOT_EXT_LIB/libwayland-server.so" \
    "$SYSROOT_EXT_LIB/libwayland-server.so.0"
copy_support_lib_if_present "libwayland-egl.so.1" \
    "$SYSROOT_EXT_LIB/libwayland-egl.so.1"
copy_support_lib_if_present "libwayland-egl.so" \
    "$SYSROOT_EXT_LIB/libwayland-egl.so" \
    "$SYSROOT_EXT_LIB/libwayland-egl.so.1"
copy_support_lib_if_present "libffi.so.8" \
    "$SYSROOT_EXT_LIB/libffi.so.8"
copy_support_lib_if_present "libffi.so" \
    "$SYSROOT_EXT_LIB/libffi.so" \
    "$SYSROOT_EXT_LIB/libffi.so.8"
copy_guest_runtime_lib_if_present "libz.so" \
    "$SYSROOT_EXT_LIB/libz.so" \
    "$SYSROOT/usr/lib/$TARGET/libz.so" \
    "$ROOT/build/staging/wine-data/bin/${WINE_ARCH}-unix/libz.so" \
    "$ROOT/build/merged_wine_data/bin/${WINE_ARCH}-unix/libz.so" \
    "$ROOT/build/merged_wine_data/bin/guest_gfx/lib/libz.so"
copy_guest_runtime_lib_if_present "libc++_shared.so" \
    "$SYSROOT_EXT_LIB/libc++_shared.so" \
    "$SYSROOT/usr/lib/$TARGET/libc++_shared.so" \
    "$OHOS_SDK/native/llvm/lib/$TARGET/libc++_shared.so" \
    "$TOOL_HOME/sdk/default/openharmony/native/llvm/lib/$TARGET/libc++_shared.so" \
    "$TOOL_HOME/sdk/default/hms/native/BiSheng/lib/$TARGET/libc++_shared.so" \
    "$ROOT/build/staging/wine-data/bin/${WINE_ARCH}-unix/libc++_shared.so" \
    "$ROOT/build/merged_wine_data/bin/${WINE_ARCH}-unix/libc++_shared.so" \
    "$ROOT/build/merged_wine_data/bin/guest_gfx/lib/libc++_shared.so"
if [ -d "$INSTALL_ROOT/share/glvnd/egl_vendor.d" ]; then
    copy_tree_if_present "$INSTALL_ROOT/share/glvnd/egl_vendor.d" "$OUTPUT_ROOT/lib/egl_vendor.d"
fi

materialize_bundle_alias "libGL.so.1" "libGL.so" "libGL_mesa.so"
materialize_bundle_alias "libGL.so" "libGL.so" "libGL_mesa.so"
materialize_bundle_alias "libEGL.so.1" "libEGL.so" "libEGL_mesa.so"
materialize_bundle_alias "libEGL.so" "libEGL.so" "libEGL_mesa.so"
materialize_bundle_alias "libGLESv2.so.2" "libGLESv2.so" "libGLESv2_mesa.so"
materialize_bundle_alias "libGLESv2.so" "libGLESv2.so" "libGLESv2_mesa.so"
materialize_dri_driver_alias "virtio_gpu_dri.so"
materialize_dri_driver_alias "swrast_dri.so"
materialize_dri_driver_alias "kms_swrast_dri.so"
if [ "$MODE" = "zink" ]; then
    materialize_dri_driver_alias "zink_dri.so"
    cp "$INSTALL_ROOT/share/winehua/guest-gfx-build.json" "$OUTPUT_ROOT/guest-gfx-build.json"
fi

emit_env_file "$OUTPUT_ROOT/winehua-guest-gfx.env" "$OUTPUT_ROOT"

{
cat <<EOF
source=$INSTALL_ROOT
mode=$MODE
platform=${WINEHUA_GUEST_GFX_PLATFORM:-unknown}
arch=$NATIVE_ARCH
built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
append_git_source_info "mesa" "$MESA_SOURCE_ROOT"
append_git_source_info "libdrm" "$LIBDRM_SOURCE_ROOT"
} > "$OUTPUT_ROOT/BUILD_INFO.txt"

[ -f "$OUTPUT_ROOT/winehua-guest-gfx.env" ] || err "failed to generate guest_gfx env file"
[ -d "$OUTPUT_ROOT/lib" ] || err "guest_gfx bundle is missing lib/"
[ -f "$OUTPUT_ROOT/lib/dri/virtio_gpu_dri.so" ] || err "guest_gfx bundle is missing lib/dri/virtio_gpu_dri.so"
if [ "$MODE" = "zink" ]; then
    [ -f "$OUTPUT_ROOT/lib/dri/zink_dri.so" ] || err "guest_gfx bundle is missing lib/dri/zink_dri.so"
fi

log "guest_gfx bundle ready: $OUTPUT_ROOT"

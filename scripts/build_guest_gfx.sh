#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

log()  { echo -e "\033[32m[BUILD]\033[0m $*"; }
warn() { echo -e "\033[33m[WARN]\033[0m $*"; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*"; exit 1; }

is_windows_style_path() {
    [[ "${1:-}" =~ ^[A-Za-z]:[\\/].*$ ]]
}

detect_host_shell() {
    if [ -n "${MSYSTEM:-}" ] && command -v cygpath >/dev/null 2>&1; then
        printf 'msys2\n'
        return 0
    fi

    if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null || grep -qi microsoft /proc/version 2>/dev/null; then
        printf 'wsl\n'
        return 0
    fi

    printf 'posix\n'
}

HOST_SHELL="$(detect_host_shell)"

normalize_host_path_input() {
    local value="${1:-}"
    local drive=""
    local rest=""

    [ -n "$value" ] || return 0
    if ! is_windows_style_path "$value"; then
        printf '%s\n' "$value"
        return 0
    fi

    case "$HOST_SHELL" in
        wsl)
            if [[ "$value" =~ ^([A-Za-z]):[\\/](.*)$ ]]; then
                drive="${BASH_REMATCH[1],,}"
                rest="${BASH_REMATCH[2]//\\//}"
                printf '/mnt/%s/%s\n' "$drive" "$rest"
                return 0
            fi
            ;;
        msys2)
            if command -v cygpath >/dev/null 2>&1; then
                cygpath -u "$value"
                return 0
            fi
            ;;
    esac

    printf '%s\n' "$value"
}

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
OUTPUT_ROOT="${WINEHUA_GUEST_GFX_OUTPUT_ROOT:-$ROOT/prebuilt/guest_gfx/$NATIVE_ARCH}"
CLEAN=0

usage() {
    cat <<'EOF'
Usage:
  bash scripts/build_guest_gfx.sh [--install-root <mesa-install>] [--mode virpipe|zink] [--output-root <dir>] [--clean]

Notes:
  - This script packages an already-built OHOS Mesa install tree into
    prebuilt/guest_gfx/<arch>/ so assemble.sh can stage it into the HNP.
  - The install root must contain OHOS-target runtime files, not regular Linux
    desktop libraries from WSL/Ubuntu.
  - Prefer managed source trees under thirdparty/; fallback fetching is still
    available through bash scripts/fetch_ohos_mesa.sh.
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

if [ "$NATIVE_ARCH" = "all" ]; then
    err "build_guest_gfx.sh requires a concrete NATIVE_ARCH (x86_64 or arm64-v8a)"
fi

if [ -n "$INSTALL_ROOT" ]; then
    INSTALL_ROOT="$(normalize_host_path_input "$INSTALL_ROOT")"
fi
if [ -n "$MESA_SOURCE_ROOT" ]; then
    MESA_SOURCE_ROOT="$(normalize_host_path_input "$MESA_SOURCE_ROOT")"
fi
if [ -n "$LIBDRM_SOURCE_ROOT" ]; then
    LIBDRM_SOURCE_ROOT="$(normalize_host_path_input "$LIBDRM_SOURCE_ROOT")"
fi
OUTPUT_ROOT="$(normalize_host_path_input "$OUTPUT_ROOT")"

find_first_existing_dir() {
    local candidate=""

    for candidate in "$@"; do
        [ -n "${candidate:-}" ] || continue
        if [ -d "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

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
    git -C "$repo_root" "$@" 2>/dev/null | head -n 1 || true
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
    "$ROOT/out/guest_gfx_install/$NATIVE_ARCH" \
    "$ROOT/out/mesa-install/$NATIVE_ARCH" \
    "$ROOT/out/mesa-install/$NATIVE_ARCH/install" \
    || true)}"

[ -n "$INSTALL_ROOT" ] || err "guest_gfx install root not provided. Set WINEHUA_GUEST_GFX_INSTALL_ROOT or pass --install-root with an OHOS Mesa install tree."
[ -d "$INSTALL_ROOT" ] || err "guest_gfx install root does not exist: $INSTALL_ROOT"

RUNTIME_LIB_DIR="$(find_runtime_lib_dir "$INSTALL_ROOT" || true)"
[ -n "$RUNTIME_LIB_DIR" ] || err "unable to locate lib/ under guest_gfx install root: $INSTALL_ROOT"

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

copy_support_lib_if_present "libdrm.so.2" \
    "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libdrm.so.2" \
    "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libdrm.so.2.4.0"
copy_support_lib_if_present "libdrm.so" \
    "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libdrm.so" \
    "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libdrm.so.2" \
    "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libdrm.so.2.4.0"
copy_support_lib_if_present "libz.so.1" \
    "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libz.so.1" \
    "$ROOT/out/sdk-links/openharmony-minimal/native/sysroot/usr/lib/x86_64-linux-ohos/libz.so" \
    "/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/sysroot/usr/lib/x86_64-linux-ohos/libz.so" \
    "/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/sysroot/usr/lib/x86_64-linux-ohos/libz.so"
copy_support_lib_if_present "libz.so" \
    "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libz.so" \
    "$ROOT/out/sdk-links/openharmony-minimal/native/sysroot/usr/lib/x86_64-linux-ohos/libz.so" \
    "/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/sysroot/usr/lib/x86_64-linux-ohos/libz.so" \
    "/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/sysroot/usr/lib/x86_64-linux-ohos/libz.so"
copy_support_lib_if_present "libc++_shared.so" \
    "$ROOT/out/sdk-links/openharmony-minimal/native/llvm/lib/x86_64-linux-ohos/libc++_shared.so" \
    "$ROOT/entry/build/default/intermediates/libs/default/x86_64/libc++_shared.so" \
    "$ROOT/out/hvigor-build/WineHua/entry/build/default/intermediates/libs/default/x86_64/libc++_shared.so" \
    "/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/llvm/lib/x86_64-linux-ohos/libc++_shared.so" \
    "/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/llvm/lib/x86_64-linux-ohos/libc++_shared.so"

if [ -d "$INSTALL_ROOT/share/glvnd/egl_vendor.d" ]; then
    copy_tree_if_present "$INSTALL_ROOT/share/glvnd/egl_vendor.d" "$OUTPUT_ROOT/lib/egl_vendor.d"
fi

materialize_bundle_alias "libGL.so.1" "libGL.so" "libGL_mesa.so"
materialize_bundle_alias "libGL.so" "libGL.so" "libGL_mesa.so"
materialize_bundle_alias "libEGL.so.1" "libEGL.so" "libEGL_mesa.so"
materialize_bundle_alias "libEGL.so" "libEGL.so" "libEGL_mesa.so"
materialize_bundle_alias "libGLESv2.so.2" "libGLESv2.so" "libGLESv2_mesa.so"
materialize_bundle_alias "libGLESv2.so" "libGLESv2.so" "libGLESv2_mesa.so"

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

log "guest_gfx bundle ready: $OUTPUT_ROOT"

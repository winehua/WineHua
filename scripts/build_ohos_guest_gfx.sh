#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT/build"
SDK_LINK_DIR="$ROOT/out/sdk-links"
HOST_TOOLS_DIR="$BUILD_DIR/host-tools"
WRAPPER_DIR="$BUILD_DIR/tool-wrappers"
SYSROOT_EXT="$BUILD_DIR/sysroot-ext"
SYSROOT_EXT_INC="$SYSROOT_EXT/usr/include"
SYSROOT_EXT_LIB="$SYSROOT_EXT/usr/lib/x86_64-linux-ohos"
SYSROOT_EXT_PC="$SYSROOT_EXT/usr/lib/pkgconfig"

MODE="${GUEST_GFX_MODE:-virpipe}"
PLATFORM="${WINEHUA_GUEST_GFX_PLATFORM:-wayland}"
SOURCE_ROOT="${WINEHUA_OHOS_MESA_SOURCE_ROOT:-}"
LIBDRM_SOURCE_ROOT="${WINEHUA_OHOS_LIBDRM_SOURCE_ROOT:-}"
WAYLAND_PROTOCOLS_SOURCE_ROOT="${WINEHUA_WAYLAND_PROTOCOLS_SOURCE_ROOT:-}"
WAYLAND_PROTOCOLS_URL="${WINEHUA_WAYLAND_PROTOCOLS_URL:-https://gitlab.freedesktop.org/wayland/wayland-protocols.git}"
WAYLAND_PROTOCOLS_TAG="${WINEHUA_WAYLAND_PROTOCOLS_TAG:-1.39}"
BUILD_ROOT="${WINEHUA_GUEST_GFX_BUILD_ROOT:-$ROOT/out/guest_gfx_build/${NATIVE_ARCH:-x86_64}/$PLATFORM-$MODE}"
INSTALL_ROOT="${WINEHUA_GUEST_GFX_INSTALL_ROOT:-$ROOT/out/guest_gfx_install/${NATIVE_ARCH:-x86_64}}"
PACKAGE_BUNDLE=1
FETCH_IF_MISSING=1
CLEAN=0

log()  { echo -e "\033[32m[BUILD]\033[0m $*"; }
warn() { echo -e "\033[33m[WARN]\033[0m $*"; }
err()  { echo -e "\033[31m[ERROR]\033[0m $*"; exit 1; }

usage() {
    cat <<'EOF'
Usage:
  bash scripts/build_ohos_guest_gfx.sh [--platform wayland] [--mode virpipe|zink]
                                       [--source-root <mesa-src>] [--libdrm-root <libdrm-src>]
                                       [--wayland-protocols-root <wp-src>]
                                       [--build-root <dir>] [--install-root <dir>] [--clean] [--no-package]
                                       [--no-fetch]

What it does:
  - Builds the guest-side Mesa receiver bundle used by WineHua graphics tests.
  - Defaults to a Wayland-targeted Mesa build because Wine's OpenGL path in
    the current tree goes through winewayland.drv + EGL_WAYLAND_KHR.
  - Auto-provisions OHOS libdrm into sysroot-ext when libdrm is missing,
    because the guest Mesa virgl build needs a target-side libdrm.pc + headers.
  - Auto-provisions a newer wayland-protocols bundle (>= 1.38) into sysroot-ext
    when the repo's pinned copy is too old for the current Mesa tree.
  - Packages the install tree into prebuilt/guest_gfx/<arch>/ unless
    --no-package is passed.

Notes:
  - This path is currently implemented for x86_64 only, which matches the
    active Wine userland on the HarmonyOS PC emulator.
  - The resulting bundle is intended for Step 1 VirGL/vtest smoke:
      MESA_LOADER_DRIVER_OVERRIDE=swrast
      GALLIUM_DRIVER=virpipe
  - Preferred source roots are thirdparty/mesa-ohos and thirdparty/libdrm-ohos.
  - If those managed trees are missing, the script reuses scripts/fetch_ohos_mesa.sh
    and clones into tmp/ohos-mesa-sparse by default.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --platform)
            [ $# -ge 2 ] || err "--platform requires a value"
            PLATFORM="$2"
            shift
            ;;
        --mode)
            [ $# -ge 2 ] || err "--mode requires a value"
            MODE="$2"
            shift
            ;;
        --source-root)
            [ $# -ge 2 ] || err "--source-root requires a value"
            SOURCE_ROOT="$2"
            shift
            ;;
        --libdrm-root)
            [ $# -ge 2 ] || err "--libdrm-root requires a value"
            LIBDRM_SOURCE_ROOT="$2"
            shift
            ;;
        --wayland-protocols-root)
            [ $# -ge 2 ] || err "--wayland-protocols-root requires a value"
            WAYLAND_PROTOCOLS_SOURCE_ROOT="$2"
            shift
            ;;
        --build-root)
            [ $# -ge 2 ] || err "--build-root requires a value"
            BUILD_ROOT="$2"
            shift
            ;;
        --install-root)
            [ $# -ge 2 ] || err "--install-root requires a value"
            INSTALL_ROOT="$2"
            shift
            ;;
        --clean)
            CLEAN=1
            ;;
        --no-package)
            PACKAGE_BUNDLE=0
            ;;
        --no-fetch)
            FETCH_IF_MISSING=0
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

is_windows_style_path() {
    [[ "${1:-}" =~ ^[A-Za-z]:[\\/].*$ ]]
}

normalize_host_path_input() {
    local value="${1:-}"
    local drive=""
    local rest=""

    [ -n "$value" ] || return 0
    if ! is_windows_style_path "$value"; then
        printf '%s\n' "$value"
        return 0
    fi

    if [[ "$value" =~ ^([A-Za-z]):[\\/](.*)$ ]]; then
        drive="${BASH_REMATCH[1],,}"
        rest="${BASH_REMATCH[2]//\\//}"
        if [ -n "$rest" ]; then
            printf '/mnt/%s/%s\n' "$drive" "$rest"
        else
            printf '/mnt/%s\n' "$drive"
        fi
        return 0
    fi

    printf '%s\n' "$value"
}

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

resolve_first_executable() {
    local candidate=""

    for candidate in "$@"; do
        [ -n "${candidate:-}" ] || continue
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

ensure_python_module() {
    local dist_name="$1"
    local module_name="$2"
    local cache_root="$ROOT/out/guest_gfx_pydeps"

    export PYTHONPATH="$cache_root${PYTHONPATH:+:$PYTHONPATH}"
    if python3 -c "import $module_name" >/dev/null 2>&1; then
        return 0
    fi

    mkdir -p "$cache_root"
    python3 - "$dist_name" "$module_name" "$cache_root" <<'PY'
import json
import pathlib
import shutil
import sys
import tarfile
import tempfile
import urllib.request

dist_name, module_name, cache_root = sys.argv[1:]
cache_root = pathlib.Path(cache_root)
download_root = cache_root / ".downloads"
download_root.mkdir(parents=True, exist_ok=True)
dest_dir = cache_root / module_name

if dest_dir.exists():
    raise SystemExit(0)

with urllib.request.urlopen(f"https://pypi.org/pypi/{dist_name}/json") as response:
    meta = json.load(response)

version = meta["info"]["version"]
release_files = meta["releases"].get(version) or meta["urls"]
sdist = None
for item in release_files:
    if item.get("packagetype") == "sdist" and item.get("url", "").endswith((".tar.gz", ".tgz")):
        sdist = item
        break

if not sdist:
    raise SystemExit(f"unable to locate sdist for {dist_name}")

archive_name = pathlib.Path(sdist["url"]).name
archive_path = download_root / archive_name
if not archive_path.exists():
    with urllib.request.urlopen(sdist["url"]) as response, archive_path.open("wb") as output:
        shutil.copyfileobj(response, output)

with tempfile.TemporaryDirectory(prefix=f"{module_name}-extract-") as tmpdir:
    tmpdir_path = pathlib.Path(tmpdir)
    with tarfile.open(archive_path, "r:*") as tar:
        tar.extractall(tmpdir_path)

    candidates = []
    for init_file in tmpdir_path.rglob("__init__.py"):
        if init_file.parent.name.lower() == module_name.lower():
            candidates.append(init_file.parent)

    if not candidates:
        raise SystemExit(f"unable to locate import package for {dist_name} ({module_name})")

    candidates.sort(key=lambda p: (len(p.parts), str(p)))
    source_dir = candidates[0]
    shutil.copytree(source_dir, dest_dir)
PY

    python3 -c "import $module_name" >/dev/null 2>&1 || err "failed to bootstrap Python module: $dist_name ($module_name)"
}

remove_tree() {
    local path="$1"
    [ -e "$path" ] || return 0
    chmod -R u+w "$path" 2>/dev/null || true
    rm -rf "$path" 2>/dev/null || true
    [ ! -e "$path" ] || err "failed to remove directory: $path"
}

version_ge() {
    local lhs="${1:-}"
    local rhs="${2:-}"
    [ -n "$lhs" ] || return 1
    [ -n "$rhs" ] || return 1
    [ "$(printf '%s\n%s\n' "$rhs" "$lhs" | sort -V | head -n 1)" = "$rhs" ]
}

read_pkgconfig_version() {
    local pc_file="$1"
    [ -f "$pc_file" ] || return 1
    sed -n 's/^Version:[[:space:]]*//p' "$pc_file" | head -n 1
}

is_wsl() {
    grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null || grep -qi microsoft /proc/version 2>/dev/null
}

create_exe_wrapper() {
    local target="$1"
    local name="$2"
    local wrapper=""

    mkdir -p "$WRAPPER_DIR"
    wrapper="$WRAPPER_DIR/$name"
    cat > "$wrapper" <<EOF
#!/bin/bash
exec bash '$ROOT/scripts/wsl_exe_wrapper.sh' '$target' "\$@"
EOF
    chmod +x "$wrapper"
    printf '%s\n' "$wrapper"
}

setup_minimal_wsl_env() {
    local sdk_source="${OHOS_SDK:-}"
    local sdk_link=""
    local clang_root=""
    local jobs=""

    is_wsl || err "build_ohos_guest_gfx.sh currently runs inside WSL; use the WSL backend for guest_gfx builds"

    sdk_source="$(normalize_host_path_input "$sdk_source")"
    if [ -z "$sdk_source" ]; then
        sdk_source="$(find_first_existing_dir \
            '/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony' \
            '/mnt/d/Program Files/Huawei/DevEco Studio/sdk/default/openharmony' \
            || true)"
    fi
    [ -n "$sdk_source" ] || err "OHOS SDK not found. Set OHOS_SDK or install the OpenHarmony/DevEco SDK."
    [ -d "$sdk_source" ] || err "OHOS SDK directory does not exist: $sdk_source"

    if [[ "$sdk_source" == *" "* ]]; then
        mkdir -p "$SDK_LINK_DIR"
        sdk_link="$SDK_LINK_DIR/openharmony-minimal"
        rm -f "$sdk_link"
        ln -sfn "$sdk_source" "$sdk_link"
        sdk_source="$sdk_link"
    fi

    export OHOS_SDK="$sdk_source"
    export SYSROOT="$OHOS_SDK/native/sysroot"
    export TARGET="x86_64-linux-ohos"

    [ -d "$SYSROOT" ] || err "OHOS sysroot does not exist: $SYSROOT"
    [ -d "$SYSROOT_EXT_LIB" ] || err "sysroot-ext lib directory is missing: $SYSROOT_EXT_LIB"
    [ -d "$SYSROOT_EXT_INC" ] || err "sysroot-ext include directory is missing: $SYSROOT_EXT_INC"
    [ -d "$SYSROOT_EXT_PC" ] || err "sysroot-ext pkg-config directory is missing: $SYSROOT_EXT_PC"

    clang_root="$OHOS_SDK/native/llvm/bin"
    CLANG_REAL="$(resolve_first_executable "$clang_root/clang.exe" "$clang_root/clang" || true)"
    CLANGXX_REAL="$(resolve_first_executable "$clang_root/clang++.exe" "$clang_root/clang++" || true)"
    LLVM_AR_REAL="$(resolve_first_executable "$clang_root/llvm-ar.exe" "$clang_root/llvm-ar" || true)"
    LLVM_STRIP_REAL="$(resolve_first_executable "$clang_root/llvm-strip.exe" "$clang_root/llvm-strip" || true)"
    [ -n "$CLANG_REAL" ] || err "clang not found under $clang_root"
    [ -n "$CLANGXX_REAL" ] || err "clang++ not found under $clang_root"
    [ -n "$LLVM_AR_REAL" ] || err "llvm-ar not found under $clang_root"
    [ -n "$LLVM_STRIP_REAL" ] || err "llvm-strip not found under $clang_root"

    export CLANG="$(create_exe_wrapper "$CLANG_REAL" clang)"
    export CLANGXX="$(create_exe_wrapper "$CLANGXX_REAL" clangxx)"
    export LLVM_AR="$(create_exe_wrapper "$LLVM_AR_REAL" llvm-ar)"
    export LLVM_STRIP="$(create_exe_wrapper "$LLVM_STRIP_REAL" llvm-strip)"

    export WAYLAND_SCANNER="${WAYLAND_SCANNER:-$HOST_TOOLS_DIR/bin/wayland-scanner}"
    [ -x "$WAYLAND_SCANNER" ] || err "wayland-scanner not found: $WAYLAND_SCANNER"

    export PKG_CONFIG_BIN="${PKG_CONFIG_BIN:-$(command -v pkg-config || true)}"
    [ -n "$PKG_CONFIG_BIN" ] || err "pkg-config not found in PATH"

    if command -v nproc >/dev/null 2>&1; then
        jobs="$(nproc)"
    else
        jobs="4"
    fi
    export JOBS="${JOBS:-$jobs}"
    export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$JOBS}"
    export MESON_NUM_THREADS="${MESON_NUM_THREADS:-$JOBS}"
    if [ -z "${MAKEFLAGS:-}" ]; then
        export MAKEFLAGS="-j$JOBS"
    fi
}

gen_guest_gfx_cross_file() {
    local cross="$BUILD_DIR/guest-gfx-x86_64-cross.txt"
    mkdir -p "$BUILD_DIR"
    cat > "$cross" <<XEOF
[binaries]
c = '$CLANG'
cpp = '$CLANGXX'
ar = '$LLVM_AR'
strip = '$LLVM_STRIP'
pkg-config = '$PKG_CONFIG_BIN'
wayland-scanner = '$WAYLAND_SCANNER'

[built-in options]
c_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-I$SYSROOT_EXT_INC']
cpp_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-I$SYSROOT_EXT_INC']
c_link_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$SYSROOT_EXT_LIB']
cpp_link_args = ['--target=$TARGET', '--sysroot=$SYSROOT', '-fuse-ld=lld', '-L$SYSROOT_EXT_LIB']
pkg_config_path = ['$SYSROOT_EXT_PC', '$SYSROOT/usr/lib/pkgconfig']

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
XEOF
    printf '%s\n' "$cross"
}

fetch_default_source_root() {
    local mesa_root=""
    local fetched_root="$ROOT/tmp/ohos-mesa-sparse"

    mesa_root="$(find_first_existing_dir \
        "$ROOT/thirdparty/mesa-ohos" \
        "$ROOT/tmp/ohos-mesa-sparse" \
        "$ROOT/tmp/ohos-guest-gfx-src/third_party_mesa3d" \
        || true)"
    if [ -n "$mesa_root" ]; then
        printf '%s\n' "$mesa_root"
        return 0
    fi

    [ "$FETCH_IF_MISSING" -eq 1 ] || return 1

    log "OHOS Mesa source not found locally; fetching official source tree" >&2
    WINEHUA_OHOS_MESA_SRC_ROOT="$ROOT/tmp" \
    WINEHUA_OHOS_MESA_DIR_NAME="ohos-mesa-sparse" \
    bash "$SCRIPT_DIR/fetch_ohos_mesa.sh" --mesa-only

    [ -d "$fetched_root" ] || err "Mesa source fetch completed but source root is still missing: $fetched_root"
    printf '%s\n' "$fetched_root"
}

ensure_mesa_source_layout() {
    local repo_root="$1"
    local sparse_list=""

    [ -d "$repo_root" ] || err "Mesa source root does not exist: $repo_root"
    [ -f "$repo_root/meson.build" ] || err "Mesa source root is not valid (meson.build missing): $repo_root"

    if [ -f "$repo_root/include/meson.build" ] && [ -d "$repo_root/bin" ] && [ -d "$repo_root/src" ]; then
        return 0
    fi

    sparse_list="$(git -C "$repo_root" sparse-checkout list 2>/dev/null || true)"
    if [ -n "$sparse_list" ]; then
        log "Expanding Mesa sparse checkout to include the files required by meson"
        git -C "$repo_root" sparse-checkout set \
            android \
            bin \
            build-support \
            docs \
            include \
            licenses \
            ohos \
            src \
            subprojects
    fi

    [ -f "$repo_root/include/meson.build" ] || err "Mesa source checkout is still incomplete (include/meson.build missing): $repo_root"
    [ -d "$repo_root/bin" ] || err "Mesa source checkout is still incomplete (bin/ missing): $repo_root"
}

fetch_default_libdrm_root() {
    local libdrm_root=""
    local fetched_root="$ROOT/tmp/third_party_libdrm"

    libdrm_root="$(find_first_existing_dir \
        "$ROOT/thirdparty/libdrm-ohos" \
        "$ROOT/tmp/third_party_libdrm" \
        "$ROOT/tmp/ohos-guest-gfx-src/third_party_libdrm" \
        || true)"
    if [ -n "$libdrm_root" ]; then
        printf '%s\n' "$libdrm_root"
        return 0
    fi

    [ "$FETCH_IF_MISSING" -eq 1 ] || return 1

    log "OHOS libdrm source not found locally; fetching official source tree" >&2
    WINEHUA_OHOS_MESA_SRC_ROOT="$ROOT/tmp" \
    WINEHUA_OHOS_MESA_DIR_NAME="ohos-mesa-sparse" \
    WINEHUA_OHOS_LIBDRM_DIR_NAME="third_party_libdrm" \
    bash "$SCRIPT_DIR/fetch_ohos_mesa.sh"

    [ -d "$fetched_root" ] || err "libdrm source fetch completed but source root is still missing: $fetched_root"
    printf '%s\n' "$fetched_root"
}

fetch_modern_wayland_protocols_root() {
    local wp_root="$ROOT/tmp/wayland-protocols-$WAYLAND_PROTOCOLS_TAG"

    if [ -d "$wp_root/.git" ]; then
        printf '%s\n' "$wp_root"
        return 0
    fi

    if [ -d "$wp_root" ] && [ ! -d "$wp_root/.git" ]; then
        warn "Removing incomplete wayland-protocols checkout: $wp_root"
        remove_tree "$wp_root"
    fi

    [ "$FETCH_IF_MISSING" -eq 1 ] || return 1

    log "Fetching upstream wayland-protocols tag $WAYLAND_PROTOCOLS_TAG" >&2
    git -c http.version=HTTP/1.1 -c core.compression=0 \
        clone --depth 1 --branch "$WAYLAND_PROTOCOLS_TAG" \
        "$WAYLAND_PROTOCOLS_URL" "$wp_root"

    [ -d "$wp_root/.git" ] || err "wayland-protocols fetch completed but checkout is missing: $wp_root"
    printf '%s\n' "$wp_root"
}

ensure_target_libdrm() {
    local build_root="$ROOT/out/libdrm_build/${NATIVE_ARCH}"
    local arch_pc_dir="$SYSROOT_EXT/usr/lib/x86_64-linux-ohos/pkgconfig"
    local meson_args=()

    if [ -f "$SYSROOT_EXT_LIB/libdrm.so" ] \
       && [ -f "$SYSROOT_EXT_PC/libdrm.pc" ] \
       && [ -f "$SYSROOT_EXT_INC/xf86drm.h" ] \
       && [ -f "$SYSROOT_EXT_INC/libdrm/virtgpu_drm.h" ]; then
        log "libdrm already available in sysroot-ext"
        return 0
    fi

    LIBDRM_SOURCE_ROOT="$(normalize_host_path_input "$LIBDRM_SOURCE_ROOT")"
    if [ -z "$LIBDRM_SOURCE_ROOT" ]; then
        LIBDRM_SOURCE_ROOT="$(find_first_existing_dir \
            "$ROOT/thirdparty/libdrm-ohos" \
            "$ROOT/tmp/third_party_libdrm" \
            "$ROOT/tmp/ohos-guest-gfx-src/third_party_libdrm" \
            || true)"
    fi
    if [ -z "$LIBDRM_SOURCE_ROOT" ]; then
        LIBDRM_SOURCE_ROOT="$(fetch_default_libdrm_root)"
    fi

    [ -d "$LIBDRM_SOURCE_ROOT" ] || err "libdrm source root does not exist: $LIBDRM_SOURCE_ROOT"
    [ -f "$LIBDRM_SOURCE_ROOT/meson.build" ] || err "libdrm source root is not valid (meson.build missing): $LIBDRM_SOURCE_ROOT"

    if [ "$CLEAN" -eq 1 ]; then
        remove_tree "$build_root"
    fi

    mkdir -p "$build_root" "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC"
    meson_args=(
        "--cross-file=$CROSS_FILE"
        "--prefix=$SYSROOT_EXT/usr"
        "--libdir=lib/x86_64-linux-ohos"
        "-Dbuildtype=release"
        "-Dtests=false"
        "-Dinstall-test-programs=false"
        "-Dcairo-tests=disabled"
        "-Dman-pages=disabled"
        "-Dvalgrind=disabled"
        "-Dudev=false"
        "-Dintel=disabled"
        "-Dradeon=disabled"
        "-Damdgpu=disabled"
        "-Dnouveau=disabled"
        "-Dvmwgfx=disabled"
        "-Domap=disabled"
        "-Dexynos=disabled"
        "-Dfreedreno=disabled"
        "-Dtegra=disabled"
        "-Dvc4=disabled"
        "-Detnaviv=disabled"
    )

    log "--- Build OHOS libdrm for sysroot-ext ---"
    log "libdrm source: $LIBDRM_SOURCE_ROOT"
    log "libdrm build: $build_root"

    if [ -f "$build_root/build.ninja" ]; then
        meson setup "$build_root" "$LIBDRM_SOURCE_ROOT" --reconfigure "${meson_args[@]}"
    else
        meson setup "$build_root" "$LIBDRM_SOURCE_ROOT" "${meson_args[@]}"
    fi

    meson compile -C "$build_root"
    meson install -C "$build_root"

    if [ -f "$arch_pc_dir/libdrm.pc" ]; then
        mkdir -p "$SYSROOT_EXT_PC"
        cp "$arch_pc_dir/libdrm.pc" "$SYSROOT_EXT_PC/libdrm.pc"
    fi

    [ -f "$SYSROOT_EXT_LIB/libdrm.so" ] || err "libdrm build finished but libdrm.so is missing from sysroot-ext"
    [ -f "$SYSROOT_EXT_PC/libdrm.pc" ] || err "libdrm build finished but libdrm.pc is missing from sysroot-ext"
    [ -f "$SYSROOT_EXT_INC/libdrm/virtgpu_drm.h" ] || err "libdrm build finished but virtgpu_drm.h is missing from sysroot-ext"
}

ensure_modern_wayland_protocols() {
    local current_pc="$SYSROOT_EXT_PC/wayland-protocols.pc"
    local current_version=""
    local required_version="1.38"
    local required_xml="$SYSROOT_EXT/usr/share/wayland-protocols/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml"
    local build_root="$ROOT/out/wayland_protocols_build/$WAYLAND_PROTOCOLS_TAG"
    local share_pc_dir="$SYSROOT_EXT/usr/share/pkgconfig"

    current_version="$(read_pkgconfig_version "$current_pc" || true)"
    if [ -n "$current_version" ] && version_ge "$current_version" "$required_version" && [ -f "$required_xml" ]; then
        log "wayland-protocols $current_version already satisfies Mesa's Wayland requirements"
        return 0
    fi

    WAYLAND_PROTOCOLS_SOURCE_ROOT="$(normalize_host_path_input "$WAYLAND_PROTOCOLS_SOURCE_ROOT")"
    if [ -z "$WAYLAND_PROTOCOLS_SOURCE_ROOT" ]; then
        WAYLAND_PROTOCOLS_SOURCE_ROOT="$(find_first_existing_dir \
            "$ROOT/tmp/wayland-protocols-$WAYLAND_PROTOCOLS_TAG" \
            "$ROOT/tmp/wayland-protocols" \
            || true)"
    fi
    if [ -z "$WAYLAND_PROTOCOLS_SOURCE_ROOT" ]; then
        WAYLAND_PROTOCOLS_SOURCE_ROOT="$(fetch_modern_wayland_protocols_root)"
    fi

    [ -d "$WAYLAND_PROTOCOLS_SOURCE_ROOT" ] || err "wayland-protocols source root does not exist: $WAYLAND_PROTOCOLS_SOURCE_ROOT"
    [ -f "$WAYLAND_PROTOCOLS_SOURCE_ROOT/meson.build" ] || err "wayland-protocols source root is not valid (meson.build missing): $WAYLAND_PROTOCOLS_SOURCE_ROOT"

    if [ "$CLEAN" -eq 1 ]; then
        remove_tree "$build_root"
    fi

    mkdir -p "$build_root" "$SYSROOT_EXT/usr/share" "$SYSROOT_EXT_PC"
    log "--- Install modern wayland-protocols into sysroot-ext ---"
    log "wayland-protocols source: $WAYLAND_PROTOCOLS_SOURCE_ROOT"
    log "wayland-protocols build: $build_root"

    if [ -f "$build_root/build.ninja" ]; then
        meson setup "$build_root" "$WAYLAND_PROTOCOLS_SOURCE_ROOT" --reconfigure \
            "--prefix=$SYSROOT_EXT/usr" \
            "-Dtests=false"
    else
        meson setup "$build_root" "$WAYLAND_PROTOCOLS_SOURCE_ROOT" \
            "--prefix=$SYSROOT_EXT/usr" \
            "-Dtests=false"
    fi

    meson compile -C "$build_root"
    meson install -C "$build_root"

    if [ -f "$share_pc_dir/wayland-protocols.pc" ]; then
        cp "$share_pc_dir/wayland-protocols.pc" "$SYSROOT_EXT_PC/wayland-protocols.pc"
    fi

    current_version="$(read_pkgconfig_version "$SYSROOT_EXT_PC/wayland-protocols.pc" || true)"
    [ -n "$current_version" ] || err "wayland-protocols install finished but wayland-protocols.pc is missing from sysroot-ext"
    version_ge "$current_version" "$required_version" || err "wayland-protocols install finished with version $current_version, expected >= $required_version"
    [ -f "$required_xml" ] || err "wayland-protocols install finished but linux-drm-syncobj-v1.xml is still missing"
}

ensure_wayland_pkgconfig_metadata() {
    local client_version=""
    local prefix_path="$SYSROOT_EXT/usr"

    client_version="$(read_pkgconfig_version "$SYSROOT_EXT_PC/wayland-client.pc" || true)"
    client_version="${client_version:-1.22.0}"

    if [ ! -f "$SYSROOT_EXT_PC/wayland-server.pc" ]; then
        cat > "$SYSROOT_EXT_PC/wayland-server.pc" <<EOF
prefix=$prefix_path
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland

Name: Wayland Server
Description: Server side implementation of the Wayland protocol
Version: $client_version
Requires.private: libffi
Libs: -L\${libdir} -lwayland-server
Cflags: -I\${includedir}
EOF
    fi

    if [ ! -f "$SYSROOT_EXT_PC/wayland-egl-backend.pc" ]; then
        cat > "$SYSROOT_EXT_PC/wayland-egl-backend.pc" <<EOF
prefix=$prefix_path
includedir=\${prefix}/include

Name: wayland-egl-backend
Description: Backend wayland-egl interface
Version: 3
Cflags: -I\${includedir}
EOF
    fi
}

copy_if_missing() {
    local src="$1"
    local dest="$2"

    if [ ! -f "$dest" ] && [ -f "$src" ]; then
        cp "$src" "$dest"
    fi
}

ensure_wayland_dev_headers() {
    local wl_src="$ROOT/thirdparty/wayland/src"
    local wl_egl="$ROOT/thirdparty/wayland/egl"
    local wl_build="$BUILD_DIR/wayland_build/x86_64/src"

    mkdir -p "$SYSROOT_EXT_INC"

    copy_if_missing "$wl_src/wayland-client.h" "$SYSROOT_EXT_INC/wayland-client.h"
    copy_if_missing "$wl_src/wayland-client-core.h" "$SYSROOT_EXT_INC/wayland-client-core.h"
    copy_if_missing "$wl_src/wayland-server.h" "$SYSROOT_EXT_INC/wayland-server.h"
    copy_if_missing "$wl_src/wayland-server-core.h" "$SYSROOT_EXT_INC/wayland-server-core.h"
    copy_if_missing "$wl_src/wayland-util.h" "$SYSROOT_EXT_INC/wayland-util.h"
    copy_if_missing "$wl_egl/wayland-egl.h" "$SYSROOT_EXT_INC/wayland-egl.h"
    copy_if_missing "$wl_egl/wayland-egl-core.h" "$SYSROOT_EXT_INC/wayland-egl-core.h"
    copy_if_missing "$wl_egl/wayland-egl-backend.h" "$SYSROOT_EXT_INC/wayland-egl-backend.h"
    copy_if_missing "$wl_build/wayland-version.h" "$SYSROOT_EXT_INC/wayland-version.h"
    copy_if_missing "$wl_build/wayland-client-protocol-core.h" "$SYSROOT_EXT_INC/wayland-client-protocol-core.h"
    copy_if_missing "$wl_build/wayland-client-protocol.h" "$SYSROOT_EXT_INC/wayland-client-protocol.h"
    copy_if_missing "$wl_build/wayland-server-protocol-core.h" "$SYSROOT_EXT_INC/wayland-server-protocol-core.h"
    copy_if_missing "$wl_build/wayland-server-protocol.h" "$SYSROOT_EXT_INC/wayland-server-protocol.h"

    [ -f "$SYSROOT_EXT_INC/wayland-server.h" ] || err "wayland-server.h is missing from sysroot-ext; run scripts/build_wayland.sh first"
    [ -f "$SYSROOT_EXT_INC/wayland-server-core.h" ] || err "wayland-server-core.h is missing from sysroot-ext; run scripts/build_wayland.sh first"
    [ -f "$SYSROOT_EXT_INC/wayland-server-protocol.h" ] || err "wayland-server-protocol.h is missing from sysroot-ext; run scripts/build_wayland.sh first"
    [ -f "$SYSROOT_EXT_INC/wayland-server-protocol-core.h" ] || err "wayland-server-protocol-core.h is missing from sysroot-ext; run scripts/build_wayland.sh first"
}

setup_minimal_wsl_env

NATIVE_ARCH="${NATIVE_ARCH:-x86_64}"
if [ "$NATIVE_ARCH" != "x86_64" ]; then
    err "build_ohos_guest_gfx.sh currently supports NATIVE_ARCH=x86_64 only"
fi

case "$PLATFORM" in
    wayland) ;;
    *)
        err "unsupported guest_gfx platform: $PLATFORM (expected: wayland)"
        ;;
esac

case "$MODE" in
    virpipe|zink) ;;
    *)
        err "unsupported guest_gfx mode: $MODE (expected: virpipe or zink)"
        ;;
esac

SOURCE_ROOT="$(normalize_host_path_input "$SOURCE_ROOT")"
LIBDRM_SOURCE_ROOT="$(normalize_host_path_input "$LIBDRM_SOURCE_ROOT")"
WAYLAND_PROTOCOLS_SOURCE_ROOT="$(normalize_host_path_input "$WAYLAND_PROTOCOLS_SOURCE_ROOT")"
BUILD_ROOT="$(normalize_host_path_input "$BUILD_ROOT")"
INSTALL_ROOT="$(normalize_host_path_input "$INSTALL_ROOT")"

if [ -z "$SOURCE_ROOT" ]; then
    SOURCE_ROOT="$(find_first_existing_dir \
        "$ROOT/thirdparty/mesa-ohos" \
        "$ROOT/tmp/ohos-mesa-sparse" \
        "$ROOT/tmp/ohos-guest-gfx-src/third_party_mesa3d" \
        || true)"
fi
if [ -z "$SOURCE_ROOT" ]; then
    SOURCE_ROOT="$(fetch_default_source_root)"
fi

[ -d "$SOURCE_ROOT" ] || err "Mesa source root does not exist: $SOURCE_ROOT"
[ -f "$SOURCE_ROOT/meson.build" ] || err "Mesa source root is not valid (meson.build missing): $SOURCE_ROOT"
ensure_mesa_source_layout "$SOURCE_ROOT"

if [ "$CLEAN" -eq 1 ]; then
    remove_tree "$BUILD_ROOT"
    remove_tree "$INSTALL_ROOT"
fi

mkdir -p "$BUILD_ROOT" "$INSTALL_ROOT"

CROSS_FILE="$(gen_guest_gfx_cross_file)"
ensure_target_libdrm
ensure_modern_wayland_protocols
ensure_wayland_pkgconfig_metadata
ensure_wayland_dev_headers
MESON_ARGS=(
    "--cross-file=$CROSS_FILE"
    "--prefix=$INSTALL_ROOT"
    "--libdir=lib"
    "-Dbuildtype=release"
    "-Dplatforms=wayland"
    "-Degl-native-platform=wayland"
    "-Dgallium-drivers=virgl,softpipe"
    "-Dvulkan-drivers="
    "-Degl=enabled"
    "-Dgles1=enabled"
    "-Dgles2=enabled"
    "-Dopengl=true"
    "-Dgbm=disabled"
    "-Dglx=disabled"
    "-Dtools="
    "-Dglvnd=disabled"
    "-Dshared-glapi=enabled"
    "-Dshader-cache=disabled"
    "-Dllvm=disabled"
    "-Ddraw-use-llvm=false"
    "-Dexpat=disabled"
    "-Dxmlconfig=disabled"
)

log "=== Build OHOS guest_gfx receiver ($NATIVE_ARCH) ==="
log "platform: $PLATFORM"
log "mode: $MODE"
log "source: $SOURCE_ROOT"
log "build: $BUILD_ROOT"
log "install: $INSTALL_ROOT"
log "sdk: $OHOS_SDK"
log "cross: $CROSS_FILE"

ensure_python_module "MarkupSafe" "markupsafe"
ensure_python_module "Mako" "mako"

export PKG_CONFIG_PATH="$SYSROOT_EXT_PC:$SYSROOT/usr/lib/pkgconfig"

if [ -f "$BUILD_ROOT/build.ninja" ]; then
    meson setup "$BUILD_ROOT" "$SOURCE_ROOT" --reconfigure "${MESON_ARGS[@]}"
else
    meson setup "$BUILD_ROOT" "$SOURCE_ROOT" "${MESON_ARGS[@]}"
fi

meson compile -C "$BUILD_ROOT"
meson install -C "$BUILD_ROOT"

[ -d "$INSTALL_ROOT/lib" ] || err "guest_gfx install is missing lib/: $INSTALL_ROOT"

if [ "$PACKAGE_BUNDLE" -eq 1 ]; then
    WINEHUA_OHOS_MESA_SOURCE_ROOT="$SOURCE_ROOT" \
    WINEHUA_OHOS_LIBDRM_SOURCE_ROOT="$LIBDRM_SOURCE_ROOT" \
    WINEHUA_GUEST_GFX_PLATFORM="$PLATFORM" \
    NATIVE_ARCH="$NATIVE_ARCH" \
    bash "$SCRIPT_DIR/build_guest_gfx.sh" --install-root "$INSTALL_ROOT" --mode "$MODE"
fi

log "guest_gfx Mesa build complete"

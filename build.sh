#!/bin/bash
#
# Low-level WineHua build entrypoint.
# Prefer scripts/rebuild_harmony.ps1 or scripts/rebuild_harmony.sh for normal use.
#
# Usage:
#   ./build.sh {command} [device_ip] [arch]
#
# arch: arm64 | x86_64 | all
#
# Commands:
#   full       Full build
#   deps       Build x86_64 OHOS sysroot-ext dependencies
#   native     Build native compositor pieces per arch
#   wine       Build Wine
#   guest-gfx  Build + package the guest-side Mesa/VirGL receiver bundle per arch
#   box64      Build Box64
#   assemble   Assemble HNP layout per arch
#   hnp        Package HNP per arch
#   hap        Build and sign HAP
#   deploy     Install to target device
#   quick      assemble -> hnp -> hap -> deploy
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SCRIPTS="$ROOT/scripts"

DEFAULT_IP="192.168.1.4:38879"

# Argument parsing
cmd="${1:-}"
device_ip="${DEFAULT_IP}"
arch="arm64"

case $# in
    0) ;;
    1) cmd="$1" ;;
    *)
        cmd="$1"
        shift
        device_ip="${DEFAULT_IP}"
        arch="arm64"
        for arg in "$@"; do
            if [[ "$arg" == *":"* ]] || [[ "$arg" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                device_ip="$arg"
            elif [ "$arg" = "arm64" ] || [ "$arg" = "x86_64" ] || [ "$arg" = "all" ]; then
                arch="$arg"
            fi
        done
        ;;
esac

# Validate arch
case "$arch" in
    arm64) NATIVE_ARCH="arm64-v8a" ;;
    x86_64) NATIVE_ARCH="x86_64" ;;
    all) ;;
    *) echo "Error: arch must be arm64 | x86_64 | all"; exit 1 ;;
esac

export NATIVE_ARCH

# Helpers
log()  { echo -e "\033[32m[BUILD]\033[0m $*"; }
warn() { echo -e "\033[33m[WARN]\033[0m $*"; }

run_native() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/build_native.sh"
}

run_assemble() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/assemble.sh"
}

run_deps() {
    bash "$SCRIPTS/build_deps.sh"
}

run_wine() {
    ENABLE_OPENGL="${ENABLE_OPENGL:-1}" bash "$SCRIPTS/build_wine.sh"
}

run_wine_smoke() {
    local want_opengl="${ENABLE_OPENGL:-1}"
    local wine_cfg="$ROOT/thirdparty/wine/build-ohos/config.log"
    local wine_config_h="$ROOT/thirdparty/wine/build-ohos/include/config.h"

    if [ "$want_opengl" = "1" ] && {
        [ ! -f "$wine_cfg" ] ||
        ! grep -q -- '--with-opengl' "$wine_cfg" ||
        [ ! -f "$wine_config_h" ] ||
        ! grep -q '^#define HAVE_LIBWAYLAND_EGL 1' "$wine_config_h"
    }; then
        log "wine-smoke requested with OpenGL, but current Wine build is not OpenGL-ready; rebuilding Wine first"
        ENABLE_OPENGL="$want_opengl" bash "$SCRIPTS/build_wine.sh"
        return
    fi

    WINE_BUILD_SCOPE=graphics-smoke ENABLE_OPENGL="$want_opengl" bash "$SCRIPTS/build_wine.sh"
}

run_guest_gfx() {
    local a="${1:-arm64-v8a}"

    if [ "$a" = "x86_64" ]; then
        NATIVE_ARCH="$a" bash "$SCRIPTS/build_ohos_guest_gfx.sh"
    else
        warn "guest_gfx source build currently targets x86_64 Wine userland only; packaging existing bundle for $a"
        NATIVE_ARCH="$a" bash "$SCRIPTS/build_guest_gfx.sh"
    fi
}

run_box64() {
    bash "$SCRIPTS/build_box64.sh"
}

run_hnp() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/package.sh" hnp
}

run_hap() {
    local a="${1:-arm64-v8a}"
    NATIVE_ARCH="$a" bash "$SCRIPTS/package.sh" hap
}

run_deploy() {
    bash "$SCRIPTS/package.sh" deploy "$device_ip"
}

# Multi-arch helpers
for_each_arch() {
    local fn="$1"
    if [ "$arch" = "all" ]; then
        $fn arm64-v8a
        $fn x86_64
    else
        $fn "$NATIVE_ARCH"
    fi
}

# assemble + hnp must stay paired because assemble resets staging
for_each_arch_assemble_and_hnp() {
    if [ "$arch" = "all" ]; then
        log "=== arch: arm64-v8a (assemble + hnp) ==="
        NATIVE_ARCH=arm64-v8a bash "$SCRIPTS/assemble.sh"
        NATIVE_ARCH=arm64-v8a bash "$SCRIPTS/package.sh" hnp
        log "=== arch: x86_64 (assemble + hnp) ==="
        NATIVE_ARCH=x86_64 bash "$SCRIPTS/assemble.sh"
        NATIVE_ARCH=x86_64 bash "$SCRIPTS/package.sh" hnp
    else
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/assemble.sh"
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hnp
    fi
}

# Pad assemble helper shared by pad / pad-hap
for_each_arch_assemble_pad() {
    if [ "$arch" = "all" ]; then
        NATIVE_ARCH=arm64-v8a bash "$SCRIPTS/assemble.sh"
        NATIVE_ARCH=x86_64 bash "$SCRIPTS/assemble.sh"
    else
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/assemble.sh"
    fi
}

# Command handling
case "$cmd" in
    deps)
        run_deps
        ;;
    native)
        for_each_arch run_native
        ;;
    wine)
        run_wine
        ;;
    wine-smoke)
        run_wine_smoke
        ;;
    guest-gfx)
        for_each_arch run_guest_gfx
        for_each_arch_assemble_and_hnp
        ;;
    box64)
        run_box64
        ;;
    assemble)
        # Keep all-mode assemble paired with hnp to avoid staging overwrite.
        for_each_arch_assemble_and_hnp
        ;;
    hnp)
        # hnp also runs assemble so staging matches the current arch.
        for_each_arch_assemble_and_hnp
        ;;
    hap)
        run_hap "$NATIVE_ARCH"
        ;;
    deploy)
        run_deploy
        ;;
    quick)
        run_deps
        run_wine
        run_box64
        for_each_arch run_native
        for_each_arch_assemble_and_hnp
        if [ "$arch" = "all" ]; then
            NATIVE_ARCH=all bash "$SCRIPTS/package.sh" hap
        else
            NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        fi
        run_deploy
        ;;
    full|all_cmd)
        run_deps
        run_wine
        run_box64
        for_each_arch run_native
        for_each_arch_assemble_and_hnp
        if [ "$arch" = "all" ]; then
            NATIVE_ARCH=all bash "$SCRIPTS/package.sh" hap
        else
            NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        fi
        ;;
    pad)
        # Pad build path: fork-only, no HNP, no execve path.
        export DEVICE_TYPE=pad
        run_deps
        run_wine
        [ "$NATIVE_ARCH" = "arm64-v8a" ] && run_box64 || true
        for_each_arch run_native
        for_each_arch_assemble_pad
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        log "Pad HAP build complete"
        ;;
    pad-hap)
        # Pad HAP-only rebuild for ArkTS or native glue changes.
        export DEVICE_TYPE=pad
        for_each_arch run_native
        for_each_arch_assemble_pad
        NATIVE_ARCH="$NATIVE_ARCH" bash "$SCRIPTS/package.sh" hap
        log "Pad HAP build complete"
        ;;
    pad-deploy)
        export DEVICE_TYPE=pad
        bash "$SCRIPTS/package.sh" deploy "$device_ip"
        ;;
    *)
        echo "Usage: $0 {full|deps|native|wine|wine-smoke|guest-gfx|box64|assemble|hnp|hap|deploy|quick|pad|pad-deploy} [device_ip] [arch]"
        echo ""
        echo "  arch: arm64 | x86_64 | all"
        echo ""
        echo "  PC commands:"
        echo "    full       Full build"
        echo "    deps       Build sysroot-ext deps"
        echo "    native     Build native compositor deps"
        echo "    wine       Build Wine"
        echo "    wine-smoke Rebuild only programs/winehua_graphics_smoke.exe"
        echo "    guest-gfx  Build + repackage guest-side Mesa/VirGL bundle -> hnp"
        echo "    box64      Build Box64"
        echo "    assemble   Assemble HNP layout"
        echo "    hnp        Package HNP"
        echo "    hap        Build and sign HAP"
        echo "    deploy     Install to target"
        echo "    quick      assemble -> hnp -> hap -> deploy"
        echo ""
        echo "  Pad commands:"
        echo "    pad <arch>        Build Pad HAP (arm64|x86_64)"
        echo "    pad-hap <arch>    Rebuild Pad HAP only"
        echo "    pad-deploy <ip>   Install to target"
        exit 1
        ;;
esac

#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-incremental}"
ARCH_INPUT="${2:-x86_64}"
shift $(( $# >= 1 ? 1 : 0 ))
shift $(( $# >= 1 ? 1 : 0 ))
AUTO_HEAL=1

while [ $# -gt 0 ]; do
    case "$1" in
        --no-auto-heal)
            AUTO_HEAL=0
            ;;
        -h|--help|help)
            MODE="help"
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
    shift
done

log() {
    printf '[rebuild] %s\n' "$*"
}

die() {
    printf '[rebuild] ERROR: %s\n' "$*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

normalize_arch() {
    case "$1" in
        x86_64|arm64|all)
            printf '%s\n' "$1"
            ;;
        arm64-v8a)
            printf 'arm64\n'
            ;;
        *)
            die "unsupported arch: $1 (expected: x86_64 | arm64 | all)"
            ;;
    esac
}

artifact_arches() {
    case "$1" in
        x86_64)
            printf '%s\n' x86_64
            ;;
        arm64)
            printf '%s\n' arm64-v8a
            ;;
        all)
            printf '%s\n' arm64-v8a
            printf '%s\n' x86_64
            ;;
        *)
            die "unsupported arch for artifact listing: $1"
            ;;
    esac
}

should_build_box64() {
    [ "$1" = "arm64" ] || [ "$1" = "all" ]
}

has_deps_artifacts() {
    [ -f "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libfreetype.so.6" ] &&
    [ -f "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libwayland-client.so.0" ] &&
    [ -f "$ROOT/build/sysroot-ext/usr/lib/x86_64-linux-ohos/libxkbcommon.so.0" ] &&
    [ -d "$ROOT/build/sysroot-ext/usr/share/X11/xkb" ]
}

has_wine_artifacts() {
    [ -f "$ROOT/thirdparty/wine/build-ohos/loader/wine" ] &&
    [ -f "$ROOT/thirdparty/wine/build-ohos/dlls/ntdll/ntdll.so" ] &&
    [ -f "$ROOT/thirdparty/wine/build-native/loader/wine.inf" ]
}

has_box64_artifacts() {
    if ! should_build_box64 "$1"; then
        return 0
    fi
    [ -f "$ROOT/build/box64_build/box64" ]
}

has_native_artifacts() {
    local arch="$1"
    local entry_arch

    for entry_arch in $(artifact_arches "$arch"); do
        [ -d "$ROOT/entry/libs/$entry_arch" ] || return 1
        find "$ROOT/entry/libs/$entry_arch" -maxdepth 1 -type f | grep -q .
    done
}

check_submodules() {
    local missing
    local nested_missing

    missing="$(cd "$ROOT" && git submodule status | awk '$1 ~ /^-/ { print $2 }')"
    if [ -n "$missing" ]; then
        printf '%s\n' "$missing" >&2
        die "git submodules are not fully initialized. Run: git submodule update --init --recursive"
    fi

    nested_missing="$(cd "$ROOT" && git submodule status --recursive | awk '$1 ~ /^-/ { print $2 }')"
    if [ -n "$nested_missing" ]; then
        log "warning: optional nested submodules are missing:"
        printf '%s\n' "$nested_missing"
    fi
}

run_build_step() {
    local step="$1"
    local arch="$2"

    log "==> build.sh $step $arch"
    bash "$ROOT/build.sh" "$step" "$arch"
}

ensure_deps() {
    local arch="$1"

    if has_deps_artifacts; then
        return 0
    fi

    log "missing sysroot-ext runtime deps; auto-running build.sh deps $arch"
    run_build_step deps "$arch"
}

ensure_wine() {
    local arch="$1"

    if has_wine_artifacts; then
        return 0
    fi

    log "missing Wine build artifacts; auto-running build.sh wine $arch"
    run_build_step wine "$arch"
}

ensure_box64() {
    local arch="$1"

    if has_box64_artifacts "$arch"; then
        return 0
    fi

    log "missing Box64 artifacts for $arch; auto-running build.sh box64 $arch"
    run_build_step box64 "$arch"
}

ensure_native() {
    local arch="$1"

    if has_native_artifacts "$arch"; then
        return 0
    fi

    log "missing entry/libs artifacts for $arch; auto-running build.sh native $arch"
    run_build_step native "$arch"
}

require_deps() {
    local arch="$1"

    has_deps_artifacts && return 0
    die "missing sysroot-ext runtime deps; run full once or retry without --no-auto-heal (arch=$arch)"
}

require_wine() {
    local arch="$1"

    has_wine_artifacts && return 0
    die "missing Wine build artifacts; run full once or retry without --no-auto-heal (arch=$arch)"
}

require_box64() {
    local arch="$1"

    has_box64_artifacts "$arch" && return 0
    die "missing Box64 artifacts for $arch; run full once or retry without --no-auto-heal"
}

require_native() {
    local arch="$1"

    has_native_artifacts "$arch" && return 0
    die "missing entry/libs artifacts for $arch; run full once or retry without --no-auto-heal"
}

prepare_prereq() {
    local kind="$1"
    local arch="$2"

    if [ "$AUTO_HEAL" -eq 1 ]; then
        "ensure_$kind" "$arch"
    else
        "require_$kind" "$arch"
    fi
}

print_artifacts() {
    local arch="$1"
    local entry_arch
    local hap_path="$ROOT/entry/build/default/outputs/default/entry-default-signed.hap"

    [ -f "$hap_path" ] || die "missing signed HAP: $hap_path"

    log "artifacts:"
    for entry_arch in $(artifact_arches "$arch"); do
        local hnp_path="$ROOT/entry/hnp/$entry_arch/winehua.hnp"
        [ -f "$hnp_path" ] || die "missing HNP: $hnp_path"
        ls -lh "$hnp_path"
    done
    ls -lh "$hap_path"
}

doctor() {
    local tool

    for tool in git bash python3 perl cmake ninja meson zip unzip; do
        need_cmd "$tool"
    done
    check_submodules

    # shellcheck disable=SC1091
    source "$ROOT/scripts/env.sh"

    [ -x "$JAVA_BIN" ] || die "configured JAVA_BIN is not executable: $JAVA_BIN"
    [ -x "$NODE_BIN" ] || die "configured NODE_BIN is not executable: $NODE_BIN"
    [ -x "$HVIGORW" ] || die "configured HVIGORW is not executable: $HVIGORW"
    [ -x "$HNPCLI" ] || die "configured HNPCLI is not executable: $HNPCLI"
    [ -x "$HDC" ] || die "configured HDC is not executable: $HDC"

    log "root: $ROOT"
    log "arch: $ARCH"
    printf 'HOST_SHELL=%s\n' "${HOST_SHELL:-unknown}"
    printf 'OHOS_SDK=%s\n' "$OHOS_SDK"
    printf 'OHOS_SDK_SOURCE=%s\n' "${OHOS_SDK_SOURCE:-}"
    printf 'HVIGORW=%s\n' "$HVIGORW"
    printf 'NODE_BIN=%s\n' "$NODE_BIN"
    printf 'JAVA_BIN=%s\n' "$JAVA_BIN"
    printf 'HNPCLI=%s\n' "$HNPCLI"
    printf 'HDC=%s\n' "$HDC"
    printf 'CLANG=%s\n' "${CLANG:-}"

    log "tool versions:"
    printf '  bash: %s\n' "$(bash --version | head -n 1)"
    printf '  python3: %s\n' "$(python3 --version 2>&1)"
    printf '  java: %s\n' "$("$JAVA_BIN" -version 2>&1 | head -n 1)"
    printf '  cmake: %s\n' "$(cmake --version | head -n 1)"
    printf '  ninja: %s\n' "$(ninja --version)"
    printf '  meson: %s\n' "$(meson --version)"
    printf '  zip: %s\n' "$(zip -v 2>&1 | head -n 1)"

    log "git submodule status:"
    (cd "$ROOT" && git submodule status --recursive)
}

usage() {
    cat <<'EOF'
Usage:
  bash scripts/rebuild_harmony.sh doctor [x86_64|arm64|all]
  bash scripts/rebuild_harmony.sh full [x86_64|arm64|all]
  bash scripts/rebuild_harmony.sh incremental [x86_64|arm64|all]
  bash scripts/rebuild_harmony.sh wine [x86_64|arm64|all]
  bash scripts/rebuild_harmony.sh package [x86_64|arm64|all]

Modes:
  doctor       Check submodules and resolved toolchain paths.
  full         deps -> wine -> (box64 if needed) -> native -> hnp -> hap
  incremental  native -> hnp -> hap
  wine         wine -> hnp -> hap
  package      hnp -> hap

Notes:
  - Run the whole build chain inside one shell on the selected host backend.
  - Use full when thirdparty/Wine/sysroot changed.
  - Use incremental when ArkTS/native/package glue changed.
  - Pass --no-auto-heal when you want fail-fast incremental behavior instead of
    silently falling back to deps/wine/native rebuilds.
EOF
}

ARCH="$(normalize_arch "$ARCH_INPUT")"

case "$MODE" in
    doctor)
        doctor
        ;;
    full)
        check_submodules
        run_build_step deps "$ARCH"
        run_build_step wine "$ARCH"
        if should_build_box64 "$ARCH"; then
            run_build_step box64 "$ARCH"
        fi
        run_build_step native "$ARCH"
        run_build_step hnp "$ARCH"
        run_build_step hap "$ARCH"
        print_artifacts "$ARCH"
        ;;
    incremental)
        check_submodules
        prepare_prereq deps "$ARCH"
        prepare_prereq wine "$ARCH"
        prepare_prereq box64 "$ARCH"
        run_build_step native "$ARCH"
        run_build_step hnp "$ARCH"
        run_build_step hap "$ARCH"
        print_artifacts "$ARCH"
        ;;
    wine)
        check_submodules
        prepare_prereq deps "$ARCH"
        prepare_prereq box64 "$ARCH"
        prepare_prereq native "$ARCH"
        run_build_step wine "$ARCH"
        run_build_step hnp "$ARCH"
        run_build_step hap "$ARCH"
        print_artifacts "$ARCH"
        ;;
    package)
        check_submodules
        prepare_prereq deps "$ARCH"
        prepare_prereq wine "$ARCH"
        prepare_prereq box64 "$ARCH"
        prepare_prereq native "$ARCH"
        run_build_step hnp "$ARCH"
        run_build_step hap "$ARCH"
        print_artifacts "$ARCH"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        die "unknown mode: $MODE"
        ;;
esac

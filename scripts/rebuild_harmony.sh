#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-incremental}"
ARCH_INPUT="${2:-x86_64}"
shift $(( $# >= 1 ? 1 : 0 ))
shift $(( $# >= 1 ? 1 : 0 ))
AUTO_HEAL=1
HEARTBEAT_INTERVAL="${REBUILD_HEARTBEAT_INTERVAL:-20}"
LOG_DIR="${REBUILD_LOG_DIR:-$ROOT/tmp/rebuild-logs}"

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

strip_ansi() {
    printf '%s' "$1" | sed -E $'s/\x1B\\[[0-9;]*[[:alpha:]]//g'
}

format_duration() {
    local total="${1:-0}"
    local h=$(( total / 3600 ))
    local m=$(( (total % 3600) / 60 ))
    local s=$(( total % 60 ))

    if [ "$h" -gt 0 ]; then
        printf '%02dh%02dm%02ds\n' "$h" "$m" "$s"
    elif [ "$m" -gt 0 ]; then
        printf '%02dm%02ds\n' "$m" "$s"
    else
        printf '%02ds\n' "$s"
    fi
}

status_prefix() {
    local status_root="$1"
    printf '%s\n' "$status_root"
}

status_set() {
    local status_root="$1"
    local key="$2"
    shift 2
    mkdir -p "$(dirname "$status_root")"
    printf '%s\n' "$*" > "${status_root}.${key}"
}

status_get() {
    local status_root="$1"
    local key="$2"
    local path="${status_root}.${key}"
    [ -f "$path" ] || return 1
    cat "$path"
}

update_step_status_from_line() {
    local status_root="$1"
    local line="$2"
    local clean target

    clean="$(strip_ansi "$line")"
    [ -n "$clean" ] || return 0

    status_set "$status_root" last_ts "$(date +%s)"

    case "$clean" in
        "[BUILD]"*)
            status_set "$status_root" detail "$clean"
            ;;
        "[rebuild]"*)
            status_set "$status_root" detail "$clean"
            ;;
        make:\ ***)
            status_set "$status_root" detail "$clean"
            ;;
    esac

    target="$(printf '%s\n' "$clean" | sed -n 's/.* -c -o \([^ ]*\).*/\1/p' | head -n 1)"
    if [ -z "$target" ]; then
        target="$(printf '%s\n' "$clean" | sed -n 's/.* -o \([^ ]*\.\(so\|dll\|exe\|a\|lib\|fon\|res\|hap\|hnp\)\).*/\1/p' | head -n 1)"
    fi
    if [ -n "$target" ]; then
        status_set "$status_root" target "$target"
    fi
}

heartbeat_monitor() {
    local step="$1"
    local arch="$2"
    local status_root="$3"
    local start_ts="$4"
    local pid="$5"
    local build_log="$6"
    local now last_ts elapsed idle detail target

    [ "$HEARTBEAT_INTERVAL" -gt 0 ] 2>/dev/null || return 0

    while kill -0 "$pid" 2>/dev/null; do
        sleep "$HEARTBEAT_INTERVAL" || break
        kill -0 "$pid" 2>/dev/null || break

        now="$(date +%s)"
        last_ts="$(status_get "$status_root" last_ts 2>/dev/null || printf '%s\n' "$start_ts")"
        detail="$(status_get "$status_root" detail 2>/dev/null || true)"
        target="$(status_get "$status_root" target 2>/dev/null || true)"
        elapsed="$(format_duration $(( now - start_ts )))"
        idle="$(format_duration $(( now - last_ts )))"

        log "still running: step=$step arch=$arch elapsed=$elapsed idle=$idle${detail:+ detail=\"$detail\"}${target:+ target=$target} log=$build_log"
    done
}

stream_build_log() {
    local build_log="$1"
    local status_root="$2"

    tail -n +1 -F "$build_log" 2>/dev/null | while IFS= read -r line; do
        printf '%s\n' "$line"
        update_step_status_from_line "$status_root" "$line"
    done
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
    [ -f "$ROOT/thirdparty/wine/build-native/loader/wine.inf" ] &&
    [ -f "$ROOT/thirdparty/wine/build-ohos/dlls/kernel32/x86_64-windows/kernel32.dll" ] &&
    [ -f "$ROOT/thirdparty/wine/build-ohos/dlls/kernelbase/x86_64-windows/kernelbase.dll" ] &&
    [ -f "$ROOT/thirdparty/wine/build-ohos/programs/wineboot/x86_64-windows/wineboot.exe" ] &&
    [ -f "$ROOT/thirdparty/wine/build-ohos/programs/explorer/x86_64-windows/explorer.exe" ] &&
    [ -f "$ROOT/thirdparty/wine/build-ohos/programs/winehua_audio_smoke/x86_64-windows/winehua_audio_smoke.exe" ] &&
    [ -f "$ROOT/thirdparty/wine/build-ohos/programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe" ]
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
    local stamp build_log status_root start_ts build_pid tail_pid heartbeat_pid rc

    log "==> build.sh $step $arch"
    mkdir -p "$LOG_DIR"
    stamp="$(date +%Y%m%d-%H%M%S)"
    build_log="$LOG_DIR/${MODE}-${step}-${arch}-${stamp}.log"
    status_root="$LOG_DIR/${MODE}-${step}-${arch}-${stamp}.status"
    start_ts="$(date +%s)"

    status_set "$status_root" step "$step"
    status_set "$status_root" arch "$arch"
    status_set "$status_root" start_ts "$start_ts"
    status_set "$status_root" last_ts "$start_ts"
    status_set "$status_root" detail "[rebuild] queued: build.sh $step $arch"
    log "progress log: $build_log"

    (
        stdbuf -oL -eL bash "$ROOT/build.sh" "$step" "$arch" >"$build_log" 2>&1
    ) &
    build_pid=$!

    stream_build_log "$build_log" "$status_root" &
    tail_pid=$!

    heartbeat_monitor "$step" "$arch" "$status_root" "$start_ts" "$build_pid" "$build_log" &
    heartbeat_pid=$!

    set +e
    wait "$build_pid"
    rc=$?
    set -e

    sleep 1
    kill "$tail_pid" "$heartbeat_pid" 2>/dev/null || true
    wait "$tail_pid" 2>/dev/null || true
    wait "$heartbeat_pid" 2>/dev/null || true

    if [ "$rc" -ne 0 ]; then
        log "step failed: step=$step arch=$arch exit=$rc log=$build_log"
        return "$rc"
    fi

    log "step complete: step=$step arch=$arch elapsed=$(format_duration $(( $(date +%s) - start_ts )))"
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
  bash scripts/rebuild_harmony.sh wine-smoke [x86_64|arm64|all]
  bash scripts/rebuild_harmony.sh guest-gfx [x86_64|arm64|all]
  bash scripts/rebuild_harmony.sh package [x86_64|arm64|all]

Modes:
  doctor       Check submodules and resolved toolchain paths.
  full         deps -> wine -> (box64 if needed) -> native -> hnp -> hap
  incremental  native -> hnp -> hap
  wine         wine -> hnp -> hap
  wine-smoke   rebuild only programs/winehua_graphics_smoke.exe -> hnp -> hap
  guest-gfx    rebuild guest-side Mesa/VirGL bundle -> hnp -> hap
  package      hnp -> hap

Notes:
  - Run the whole build chain inside one shell on the selected host backend.
  - Use full when thirdparty/Wine/sysroot changed.
  - Use incremental when ArkTS/native/package glue changed.
  - Pass --no-auto-heal when you want fail-fast incremental behavior instead of
    silently falling back to deps/wine/native rebuilds.
  - Long-running steps emit a heartbeat every 20 seconds by default.
    Override with REBUILD_HEARTBEAT_INTERVAL=<seconds>; set 0 to disable.
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
    wine-smoke)
        check_submodules
        prepare_prereq deps "$ARCH"
        prepare_prereq box64 "$ARCH"
        prepare_prereq native "$ARCH"
        require_wine "$ARCH"
        run_build_step wine-smoke "$ARCH"
        run_build_step hnp "$ARCH"
        run_build_step hap "$ARCH"
        print_artifacts "$ARCH"
        ;;
    guest-gfx)
        check_submodules
        prepare_prereq deps "$ARCH"
        prepare_prereq wine "$ARCH"
        prepare_prereq box64 "$ARCH"
        prepare_prereq native "$ARCH"
        run_build_step guest-gfx "$ARCH"
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

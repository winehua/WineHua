#!/bin/bash
# package.sh - HNP packaging, HAP build/sign and deploy helpers.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

ENTRY_PROFILE="$WINEHUA/entry/build-profile.json5"
HVIGOR_BUILD_CACHE_DIR="$OUT_DIR/hvigor-build"
HVIGOR_STOP_DAEMON_ON_BUILD="${HVIGOR_STOP_DAEMON_ON_BUILD:-0}"
HVIGOR_CLEAN_CACHE_ON_BUILD="${HVIGOR_CLEAN_CACHE_ON_BUILD:-0}"
HVIGOR_ENABLE_PARALLEL="${HVIGOR_ENABLE_PARALLEL:-1}"
HVIGOR_ENABLE_INCREMENTAL="${HVIGOR_ENABLE_INCREMENTAL:-1}"
HVIGOR_ENABLE_DAEMON="${HVIGOR_ENABLE_DAEMON:-1}"
TEMP_ROOT=""
ENTRY_PROFILE_BACKUP=""
declare -a RESTORE_MOVES=()

cleanup() {
    local item from to

    if [ -n "$ENTRY_PROFILE_BACKUP" ] && [ -f "$ENTRY_PROFILE_BACKUP" ]; then
        cp "$ENTRY_PROFILE_BACKUP" "$ENTRY_PROFILE"
        rm -f "$ENTRY_PROFILE_BACKUP"
    fi

    for item in "${RESTORE_MOVES[@]}"; do
        from="${item%%:*}"
        to="${item#*:}"
        if [ -e "$from" ]; then
            mkdir -p "$(dirname "$to")"
            rm -rf "$to"
            mv "$from" "$to"
        fi
    done

    if [ -n "$TEMP_ROOT" ] && [ -d "$TEMP_ROOT" ]; then
        rm -rf "$TEMP_ROOT"
    fi
}

trap cleanup EXIT

ensure_temp_root() {
    if [ -z "$TEMP_ROOT" ]; then
        mkdir -p "$BUILD_DIR"
        TEMP_ROOT="$(mktemp -d "$BUILD_DIR/package.XXXXXX")"
    fi
}

backup_entry_profile() {
    if [ -n "$ENTRY_PROFILE_BACKUP" ]; then
        return 0
    fi

    [ -f "$ENTRY_PROFILE" ] || err "entry/build-profile.json5 not found: $ENTRY_PROFILE"
    ensure_temp_root
    ENTRY_PROFILE_BACKUP="$TEMP_ROOT/entry.build-profile.json5"
    cp "$ENTRY_PROFILE" "$ENTRY_PROFILE_BACKUP"
}

set_abi_filters() {
    local abi_value

    backup_entry_profile

    if [ "$NATIVE_ARCH" = "all" ]; then
        abi_value='"arm64-v8a", "x86_64"'
    else
        abi_value="\"$NATIVE_ARCH\""
    fi

    python3 - <<PY
import re
from pathlib import Path

profile = Path(r"$ENTRY_PROFILE")
content = profile.read_text(encoding="utf-8")
updated = re.sub(r'"abiFilters"\s*:\s*\[[^\]]*\]', f'"abiFilters": [$abi_value]', content, count=1)
if updated != content:
    profile.write_text(updated, encoding="utf-8")
PY
    log "abiFilters: [$abi_value]"
}

stash_path() {
    local original="$1"
    local stash_name="$2"
    local stash_path

    [ -e "$original" ] || return 0
    ensure_temp_root
    stash_path="$TEMP_ROOT/$stash_name"
    rm -rf "$stash_path"
    mv "$original" "$stash_path"
    RESTORE_MOVES+=("$stash_path:$original")
}

hide_non_target_artifacts() {
    if [ "$NATIVE_ARCH" = "all" ]; then
        return 0
    fi

    if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        stash_path "$WINEHUA/entry/libs/x86_64" "libs.x86_64"
        stash_path "$WINEHUA/entry/hnp/x86_64" "hnp.x86_64"
    else
        stash_path "$WINEHUA/entry/libs/arm64-v8a" "libs.arm64-v8a"
        stash_path "$WINEHUA/entry/hnp/arm64-v8a" "hnp.arm64-v8a"
    fi
}

prepare_pad_build() {
    local module_json="$WINEHUA/entry/src/main/module.json5"

    python3 - <<PY
import re
from pathlib import Path

module_json = Path(r"$module_json")
content = module_json.read_text(encoding="utf-8")
content = re.sub(r',?\s*"hnpPackages"\s*:\s*\[[^][]*\]', '', content)
if content != module_json.read_text(encoding="utf-8"):
    module_json.write_text(content, encoding="utf-8")
PY

    python3 - <<PY
import re
from pathlib import Path

profile = Path(r"$ENTRY_PROFILE")
content = profile.read_text(encoding="utf-8")
if "-DPAD_MODE" not in content:
    content = re.sub(r'"cppFlags"\s*:\s*"', '"cppFlags": "-DPAD_MODE ', content, count=1)
    profile.write_text(content, encoding="utf-8")
PY
}

get_hnp_payload_paths() {
    if [ "$NATIVE_ARCH" = "all" ]; then
        printf '%s\n' "hnp/arm64-v8a/winehua.hnp" "hnp/x86_64/winehua.hnp"
    else
        printf '%s\n' "hnp/$NATIVE_ARCH/winehua.hnp"
    fi
}

verify_hnp_payload_in_hap() {
    local hap_path="$1"
    shift

    python3 - "$hap_path" "$@" <<'PY'
import sys
import zipfile

hap_path = sys.argv[1]
required = sys.argv[2:]

with zipfile.ZipFile(hap_path) as archive:
    names = set(archive.namelist())

missing = [name for name in required if name not in names]
if missing:
    raise SystemExit("missing HNP payload entries: " + ", ".join(missing))
PY
}

inject_hnp_payload_into_hap() {
    local hap_path="$1"
    local payloads=()
    local payload

    while IFS= read -r payload; do
        [ -n "$payload" ] || continue
        payloads+=("$payload")
    done < <(get_hnp_payload_paths)

    [ "${#payloads[@]}" -gt 0 ] || err "no HNP payloads resolved for $NATIVE_ARCH"

    for payload in "${payloads[@]}"; do
        [ -f "$WINEHUA/entry/$payload" ] || err "HNP payload missing: $WINEHUA/entry/$payload"
    done

    (
        cd "$WINEHUA/entry"
        zip -q "$hap_path" "${payloads[@]}"
    ) || err "zip hnp into hap failed"

    verify_hnp_payload_in_hap "$hap_path" "${payloads[@]}" || err "HNP payload verification failed: $hap_path"
}

package_hnp() {
    log "=== package HNP ($NATIVE_ARCH) ==="
    if [ "$DEVICE_TYPE" = "pad" ]; then
        log "pad mode skips HNP packaging"
        return 0
    fi

    local assemble_stamp="$OUT_DIR/.assemble-${DEVICE_TYPE}-${NATIVE_ARCH}.stamp"
    local staged_hnp="$OUT_DIR/winehua.hnp"
    local hnp_dir="$WINEHUA/entry/hnp/$NATIVE_ARCH"
    local final_hnp="$hnp_dir/winehua.hnp"

    if [ -f "$assemble_stamp" ] && [ -f "$final_hnp" ] && [ "$final_hnp" -nt "$assemble_stamp" ]; then
        mkdir -p "$OUT_DIR"
        if [ ! -f "$staged_hnp" ] || [ "$final_hnp" -nt "$staged_hnp" ]; then
            cp "$final_hnp" "$staged_hnp"
        fi
        log "HNP package already up to date ($NATIVE_ARCH)"
        ls -lh "$final_hnp"
        return 0
    fi

    mkdir -p "$OUT_DIR"
    "$HNPCLI" pack -i "$STAGING_DIR" -o "$OUT_DIR" -n winehua -v 0.1.0 || err "hnpcli pack failed"

    mkdir -p "$hnp_dir"
    cp "$staged_hnp" "$final_hnp"
    ls -lh "$final_hnp"
}

package_hap() {
    local legacy_hap_dir="$WINEHUA/entry/build/default/outputs/default"
    local hvigor_status=0
    local hvigor_signed_hap=""
    local hvigor_args=()
    local unsigned_hap=""
    local staged_signed_hap=""
    local signed_hap="$legacy_hap_dir/entry-default-signed.hap"

    log "=== package HAP ($NATIVE_ARCH) ==="
    set_abi_filters
    hide_non_target_artifacts
    if [ "$DEVICE_TYPE" = "pad" ]; then
        prepare_pad_build
    fi
    if [ "$HVIGOR_CLEAN_CACHE_ON_BUILD" = "1" ]; then
        rm -rf "$HVIGOR_BUILD_CACHE_DIR"
    fi
    mkdir -p "$HVIGOR_BUILD_CACHE_DIR"
    hvigor_args=(--mode module)
    if [ "$HVIGOR_ENABLE_PARALLEL" = "1" ]; then
        hvigor_args+=(--parallel)
    fi
    if [ "$HVIGOR_ENABLE_INCREMENTAL" = "1" ]; then
        hvigor_args+=(--incremental)
    fi
    if [ "$HVIGOR_ENABLE_DAEMON" = "1" ]; then
        hvigor_args+=(--daemon)
    fi
    hvigor_args+=(
        -p product=default
        -p buildMode=debug
        -p ohos.buildDir="$HVIGOR_BUILD_CACHE_DIR"
        -p build-cache-dir="$HVIGOR_BUILD_CACHE_DIR"
        assembleHap
    )

    (
        cd "$WINEHUA"
        if [ "$HVIGOR_STOP_DAEMON_ON_BUILD" = "1" ]; then
            "$HVIGORW" --stop-daemon >/dev/null 2>&1 || true
        fi
        "$HVIGORW" "${hvigor_args[@]}"
    ) || hvigor_status=$?

    unsigned_hap="$(find "$legacy_hap_dir" "$HVIGOR_BUILD_CACHE_DIR" -type f -name 'entry-default-unsigned.hap' 2>/dev/null | head -n 1 || true)"
    hvigor_signed_hap="$(find "$legacy_hap_dir" "$HVIGOR_BUILD_CACHE_DIR" -type f -name 'entry-default-signed.hap' 2>/dev/null | head -n 1 || true)"
    if [ "$hvigor_status" -ne 0 ]; then
        if [ -n "$unsigned_hap" ]; then
            warn "hvigorw assembleHap exited with status $hvigor_status after producing an unsigned HAP; continuing with manual signing"
        else
            err "hvigorw assembleHap failed"
        fi
    fi

    if [ "$hvigor_status" -eq 0 ] && [ -n "$hvigor_signed_hap" ]; then
        if [ "$DEVICE_TYPE" != "pad" ]; then
            if verify_hnp_payload_in_hap "$hvigor_signed_hap" $(get_hnp_payload_paths); then
                log "hvigor signed HAP already contains HNP payloads"
            else
                warn "hvigor signed HAP lost HNP payloads; falling back to unsigned HAP injection + manual signing"
                hvigor_signed_hap=""
            fi
        fi
        if [ -n "$hvigor_signed_hap" ]; then
            if [ "$hvigor_signed_hap" != "$signed_hap" ]; then
                mkdir -p "$legacy_hap_dir"
                cp "$hvigor_signed_hap" "$signed_hap"
            fi
            ls -lh "$signed_hap"
            log "HAP build complete via hvigor signing ($NATIVE_ARCH)"
            return 0
        fi
    fi

    [ -n "$unsigned_hap" ] || err "unsigned HAP not found under $HVIGOR_BUILD_CACHE_DIR"
    staged_signed_hap="$(dirname "$unsigned_hap")/entry-default-signed.hap"

    if [ "$DEVICE_TYPE" != "pad" ]; then
        if verify_hnp_payload_in_hap "$unsigned_hap" $(get_hnp_payload_paths); then
            log "unsigned HAP already contains HNP payloads"
        else
            inject_hnp_payload_into_hap "$unsigned_hap"
        fi
    fi

    (
        cd "$WINEHUA"
        python3 sign.py "$unsigned_hap" "$staged_signed_hap"
    ) || err "HAP signing failed"

    if [ "$DEVICE_TYPE" != "pad" ]; then
        verify_hnp_payload_in_hap "$staged_signed_hap" $(get_hnp_payload_paths) || err "signed HAP lost HNP payload"
    fi

    mkdir -p "$legacy_hap_dir"
    if [ "$staged_signed_hap" != "$signed_hap" ]; then
        cp "$staged_signed_hap" "$signed_hap"
    fi

    ls -lh "$signed_hap"
    log "HAP build and signing complete ($NATIVE_ARCH)"
}

resolve_deploy_target() {
    local requested="${1:-}"
    local targets=()
    local line

    if [ -n "$requested" ] && [ "$requested" != "emulator" ]; then
        case "$requested" in
            *:*|[0-9]*.[0-9]*.[0-9]*.[0-9]*)
                log "connecting target via hdc tconn: $requested"
                "$HDC" tconn "$requested" || err "hdc tconn failed: $requested"
                printf '%s\n' "$requested"
                return 0
                ;;
            *)
                printf '%s\n' "$requested"
                return 0
                ;;
        esac
    fi

    while IFS= read -r line; do
        [ -n "$line" ] || continue
        [ "$line" = "[Empty]" ] && continue
        targets+=("${line%% *}")
    done < <("$HDC" list targets 2>/dev/null || true)

    if [ "${#targets[@]}" -eq 0 ]; then
        err "no HDC targets found. Start the emulator or pass an explicit target/IP."
    fi

    if [ "${#targets[@]}" -gt 1 ]; then
        err "multiple HDC targets found (${targets[*]}). Pass an explicit target key or ip:port."
    fi

    printf '%s\n' "${targets[0]}"
}

deploy() {
    local requested="${1:-}"
    local hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"
    local target

    [ -f "$hap" ] || err "signed HAP does not exist: $hap"
    target="$(resolve_deploy_target "$requested")"

    log "=== deploy to $target ==="
    "$HDC" -t "$target" uninstall -n app.hackeris.winehua 2>/dev/null || true
    "$HDC" -t "$target" install -r "$hap" || err "hdc install failed"

    log "deploy complete"
}

case "${1:-}" in
    hnp)    package_hnp ;;
    hap)    package_hap ;;
    deploy) deploy "${2:-}" ;;
    all)
        if [ "$DEVICE_TYPE" = "pad" ]; then
            package_hap && deploy "${2:-}"
        else
            package_hnp && package_hap && deploy "${2:-}"
        fi
        ;;
    *)      echo "usage: $0 {hnp|hap|deploy|all} [target|ip:port]" >&2; exit 1 ;;
esac

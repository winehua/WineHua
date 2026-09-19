#!/bin/bash
# Verify that a Proton-OHOS candidate is internally consistent and record provenance.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WINE_SRC="${WINE_SRC:-$ROOT/thirdparty/wine-valve}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"
WINE_BUILD="$BUILD_DIR/wine-ohos-aarch64"
REPORT="$BUILD_DIR/w1-candidate-provenance.txt"
HAP=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --hap)
            [ "$#" -ge 2 ] || { echo "--hap requires a path" >&2; exit 2; }
            HAP="$2"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

hap_git()
{
    if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
        git -C "$ROOT" "$@"
    else
        git --git-dir="${HAP_GIT_DIR:-/data/prod/.git/worktrees/WineHua-proton-ohos}" \
            --work-tree="$ROOT" "$@"
    fi
}

wine_git()
{
    if git -C "$WINE_SRC" rev-parse --git-dir >/dev/null 2>&1; then
        git -C "$WINE_SRC" "$@"
    else
        git --git-dir="${WINE_GIT_DIR:-$ROOT/thirdparty/wine/.git/worktrees/wine-valve}" \
            --work-tree="$WINE_SRC" "$@"
    fi
}

audio_source="$WINE_SRC/dlls/wineohos.drv/ohos.c"
wine_protocol="$WINE_SRC/dlls/wineohos.drv/audio_ipc_protocol.h"
host_protocol="$ROOT/entry/src/main/cpp/protocols/audio_ipc_protocol.h"

required_slots=(
    process_attach process_detach main_loop get_endpoint_ids create_stream
    release_stream start stop reset timer_loop get_render_buffer
    release_render_buffer get_capture_buffer release_capture_buffer
    is_format_supported get_loopback_capture_device get_mix_format
    get_device_period get_buffer_size get_latency get_current_padding
    get_next_packet_size get_frequency get_position set_volumes
    set_event_handle set_sample_rate test_connect is_started get_prop_value
    midi_get_driver midi_init midi_release midi_out_message midi_in_message
    midi_notify_wait aux_message
)

table_impl()
{
    local table="$1" slot="$2"
    awk -v table="$table" -v slot="$slot" '
        index($0, "const unixlib_entry_t " table "[]") { in_table = 1; next }
        in_table && /^};/ { exit }
        in_table && $0 ~ "^[[:space:]]*\\[" slot "\\]" {
            line = $0
            sub(/^[^=]*=[[:space:]]*/, "", line)
            sub(/,[[:space:]]*$/, "", line)
            print line
        }
    ' "$audio_source"
}

for slot in "${required_slots[@]}"; do
    native_impl="ohos_$slot"
    case "$slot" in
        get_loopback_capture_device|set_sample_rate) native_impl=ohos_not_implemented ;;
    esac

    wow64_impl="ohos_wow64_$slot"
    case "$slot" in
        process_detach|start|stop|reset|timer_loop|release_render_buffer|release_capture_buffer|is_started|midi_get_driver|midi_release)
            wow64_impl="ohos_$slot"
            ;;
        get_loopback_capture_device|set_sample_rate)
            wow64_impl=ohos_not_implemented
            ;;
    esac

    actual_native="$(table_impl __wine_unix_call_funcs "$slot")"
    actual_wow64="$(table_impl __wine_unix_call_wow64_funcs "$slot")"
    if [ "$actual_native" != "$native_impl" ]; then
        echo "native audio slot '$slot' maps to '$actual_native', expected '$native_impl'" >&2
        exit 1
    fi
    if [ "$actual_wow64" != "$wow64_impl" ]; then
        echo "WoW64 audio slot '$slot' maps to '$actual_wow64', expected '$wow64_impl'" >&2
        exit 1
    fi
done

if ! grep -A18 -F 'static NTSTATUS ohos_wow64_release_stream' "$audio_source" \
        | grep -qF '.timer_thread = ULongToHandle(params32->timer_thread)'; then
    echo "WoW64 release_stream does not convert timer_thread" >&2
    exit 1
fi

if ! cmp -s "$wine_protocol" "$host_protocol"; then
    echo "audio IPC protocol headers differ" >&2
    exit 1
fi

artifacts=(
    "$WINE_BUILD/loader/wine"
    "$WINE_BUILD/dlls/ntdll/ntdll.so"
    "$WINE_BUILD/dlls/mmdevapi/aarch64-windows/mmdevapi.dll"
    "$WINE_BUILD/dlls/mmdevapi/i386-windows/mmdevapi.dll"
    "$WINE_BUILD/dlls/dwrite/aarch64-windows/dwrite.dll"
    "$WINE_BUILD/dlls/dwrite/i386-windows/dwrite.dll"
    "$WINE_BUILD/dlls/wineohos.drv/wineohos.so"
    "$WINE_BUILD/dlls/win32u/win32u.so"
    "$WINE_BUILD/dlls/winewayland.drv/winewayland.so"
    "$ROOT/entry/libs/$NATIVE_ARCH/libwineserver.so"
)

for artifact in "${artifacts[@]}"; do
    if [ ! -s "$artifact" ]; then
        echo "missing candidate artifact: $artifact" >&2
        exit 1
    fi
done

if ! grep -aqF 'WineHua audio unixlib ABI slots v1' "$WINE_BUILD/dlls/wineohos.drv/wineohos.so"; then
    echo "wineohos.so was not rebuilt with the corrected audio ABI" >&2
    exit 1
fi

if [ -n "$HAP" ]; then
    python3 "$SCRIPT_DIR/check_runtime_components.py" --hap "$HAP"
    python3 "$SCRIPT_DIR/check_candidate_hap.py" \
        --hap "$HAP" --root "$ROOT" --native-arch "$NATIVE_ARCH" \
        --wine-build "$WINE_BUILD"
fi

mkdir -p "$BUILD_DIR"
{
    echo "generated_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "hap_head=$(hap_git rev-parse HEAD)"
    echo "wine_head=$(wine_git rev-parse HEAD)"
    echo "wine_src=$WINE_SRC"
    echo "audio_protocol_sha256=$(sha256sum "$wine_protocol" | awk '{print $1}')"
    if [ -n "$HAP" ]; then
        echo "signed_hap=$HAP"
        echo "signed_hap_sha256=$(sha256sum "$HAP" | awk '{print $1}')"
    fi
    echo "hap_status_begin"
    hap_git status --short --ignore-submodules=all
    echo "hap_status_end"
    echo "wine_status_begin"
    wine_git status --short
    echo "wine_status_end"
    echo "artifact_sha256_begin"
    sha256sum "${artifacts[@]}"
    echo "artifact_sha256_end"
    for manifest in \
        "$ROOT/thirdparty/dxvk/manifest.json" \
        "$ROOT/thirdparty/dxvk-modern/manifest.json" \
        "$BUILD_DIR/guest_vulkan/$NATIVE_ARCH/manifest.json" \
        "$ROOT/entry/src/main/resources/rawfile/wine-runtime-manifest.json"; do
        if [ -f "$manifest" ]; then
            echo "manifest=$manifest"
            sha256sum "$manifest"
        fi
    done
} > "$REPORT"

echo "candidate verification PASS"
echo "provenance: $REPORT"

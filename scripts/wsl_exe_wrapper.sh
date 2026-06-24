#!/bin/bash
set -euo pipefail

tool="$1"
shift

temp_files=()

cleanup() {
    if [ "${#temp_files[@]}" -gt 0 ]; then
        rm -rf "${temp_files[@]}" 2>/dev/null || true
    fi
}

trap cleanup EXIT

detect_host_adapter() {
    if [ -n "${WINDOWS_WRAPPER_HOST:-}" ]; then
        printf '%s\n' "$WINDOWS_WRAPPER_HOST"
        return 0
    fi

    if [ -n "${MSYSTEM:-}" ] && command -v cygpath >/dev/null 2>&1; then
        printf 'msys2\n'
        return 0
    fi

    if command -v wslpath >/dev/null 2>&1; then
        if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null || grep -qi microsoft /proc/version 2>/dev/null; then
            printf 'wsl\n'
            return 0
        fi
    fi

    printf 'posix\n'
}

HOST_ADAPTER="$(detect_host_adapter)"

cleanup_temp_dir() {
    local value="$1"
    temp_files+=("$value")
}

is_windows_style_path() {
    [[ "${1:-}" =~ ^[A-Za-z]:[\\/].*$ ]]
}

normalize_tool_path() {
    local value="$1"
    local drive=""
    local rest=""

    if ! is_windows_style_path "$value"; then
        printf '%s\n' "$value"
        return 0
    fi

    case "$HOST_ADAPTER" in
        wsl)
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
            ;;
        msys2)
            if command -v cygpath >/dev/null 2>&1; then
                cygpath -u "$value"
                return 0
            fi
            if [[ "$value" =~ ^([A-Za-z]):[\\/](.*)$ ]]; then
                drive="${BASH_REMATCH[1],,}"
                rest="${BASH_REMATCH[2]//\\//}"
                if [ -n "$rest" ]; then
                    printf '/%s/%s\n' "$drive" "$rest"
                else
                    printf '/%s\n' "$drive"
                fi
                return 0
            fi
            ;;
    esac

    printf '%s\n' "$value"
}

tool="$(normalize_tool_path "$tool")"

path_exists_or_parent_exists() {
    local value="$1"
    [[ "$value" == /* ]] && { [ -e "$value" ] || [ -e "$(dirname "$value")" ]; }
}

rewrite_sdk_overlay_path() {
    local value="$1"
    local mapped="$value"
    local dll_candidate=""
    local restool_plugin_basename=""

    if [ -n "${RESTOOL_PLUGIN_DIR:-}" ]; then
        restool_plugin_basename="$(basename "$mapped")"
        case "$restool_plugin_basename" in
            libimage_transcoder_shared.so|libimage_transcoder_shared.dylib|libimage_transcoder_shared.dll)
                mapped="$RESTOOL_PLUGIN_DIR/libimage_transcoder_shared.dll"
                ;;
        esac
    fi

    if [ -n "${SDK_MAP_HMS_FROM:-}" ] && [ -n "${SDK_MAP_HMS_TO:-}" ] && [[ "$mapped" == "$SDK_MAP_HMS_FROM/"* ]]; then
        mapped="${SDK_MAP_HMS_TO}${mapped#$SDK_MAP_HMS_FROM}"
    elif [ -n "${SDK_MAP_OHOS_FROM:-}" ] && [ -n "${SDK_MAP_OHOS_TO:-}" ] && [[ "$mapped" == "$SDK_MAP_OHOS_FROM/"* ]]; then
        mapped="${SDK_MAP_OHOS_TO}${mapped#$SDK_MAP_OHOS_FROM}"
    fi

    case "$mapped" in
        *.so)
            dll_candidate="${mapped%.so}.dll"
            [ -e "$mapped" ] || [ ! -e "$dll_candidate" ] || mapped="$dll_candidate"
            ;;
        *.dylib)
            dll_candidate="${mapped%.dylib}.dll"
            [ -e "$mapped" ] || [ ! -e "$dll_candidate" ] || mapped="$dll_candidate"
            ;;
    esac

    printf '%s\n' "$mapped"
}

to_windows_path() {
    local value="$1"

    case "$HOST_ADAPTER" in
        wsl)
            wslpath -w "$value"
            ;;
        msys2)
            cygpath -w "$value"
            ;;
        *)
            printf '%s\n' "$value"
            ;;
    esac
}

convert_path() {
    local value="$1"

    value="$(rewrite_sdk_overlay_path "$value")"

    if [ "$HOST_ADAPTER" = "posix" ]; then
        printf '%s\n' "$value"
        return 0
    fi

    if path_exists_or_parent_exists "$value"; then
        to_windows_path "$value"
        return 0
    fi

    printf '%s\n' "$value"
}

convert_restool_file_list() {
    local value="$1"
    local tmp_dir
    local tmp_file

    if ! command -v python3 >/dev/null 2>&1; then
        convert_path "$value"
        return 0
    fi

    if ! path_exists_or_parent_exists "$value" || [ ! -f "$value" ]; then
        convert_path "$value"
        return 0
    fi

    tmp_dir="$(mktemp -d "$(dirname "$value")/.restool-config.XXXXXX")"
    tmp_file="$tmp_dir/$(basename "$value")"
    cleanup_temp_dir "$tmp_dir"
    python3 - "$value" "$tmp_file" "$tmp_dir" "$HOST_ADAPTER" <<'PY'
import json
import os
import subprocess
import sys

src, dst, tmp_dir, host_adapter = sys.argv[1:5]
cache = {}
counter = 0
ohos_from = os.environ.get("SDK_MAP_OHOS_FROM", "")
ohos_to = os.environ.get("SDK_MAP_OHOS_TO", "")
hms_from = os.environ.get("SDK_MAP_HMS_FROM", "")
hms_to = os.environ.get("SDK_MAP_HMS_TO", "")
restool_plugin_dir = os.environ.get("RESTOOL_PLUGIN_DIR", "")

def rewrite_sdk_overlay_path(value: str) -> str:
    mapped = value
    basename = os.path.basename(mapped)

    if restool_plugin_dir and basename in {
        "libimage_transcoder_shared.so",
        "libimage_transcoder_shared.dylib",
        "libimage_transcoder_shared.dll",
    }:
        mapped = os.path.join(restool_plugin_dir, "libimage_transcoder_shared.dll")

    if hms_from and hms_to and mapped.startswith(f"{hms_from}/"):
        mapped = f"{hms_to}{mapped[len(hms_from):]}"
    elif ohos_from and ohos_to and mapped.startswith(f"{ohos_from}/"):
        mapped = f"{ohos_to}{mapped[len(ohos_from):]}"

    if not os.path.exists(mapped):
        if mapped.endswith(".so"):
            dll_candidate = f"{mapped[:-3]}.dll"
            if os.path.exists(dll_candidate):
                mapped = dll_candidate
        elif mapped.endswith(".dylib"):
            dll_candidate = f"{mapped[:-6]}.dll"
            if os.path.exists(dll_candidate):
                mapped = dll_candidate

    return mapped

def looks_like_path(value: object) -> bool:
    if not isinstance(value, str) or not value.startswith("/"):
        return False

    resolved = rewrite_sdk_overlay_path(value)
    return os.path.exists(resolved) or os.path.exists(os.path.dirname(resolved) or "/")

def to_windows_path(value: str) -> str:
    resolved = rewrite_sdk_overlay_path(value)
    cmd = ["wslpath", "-w", resolved] if host_adapter == "wsl" else ["cygpath", "-w", resolved]
    return subprocess.check_output(cmd, text=True).strip()

def next_temp_json_path(source_path: str) -> str:
    global counter
    counter += 1
    nested_dir = os.path.join(tmp_dir, f"nested-{counter}")
    os.makedirs(nested_dir, exist_ok=True)
    return os.path.join(nested_dir, os.path.basename(source_path))

def rewrite_json_file(source_path: str, target_path: str | None = None) -> str:
    if source_path in cache:
        return cache[source_path]

    output_path = target_path or next_temp_json_path(source_path)
    cache[source_path] = output_path

    with open(source_path, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    if os.path.basename(source_path) == "opt-compression.json":
        compression = data.get("compression", {})
        media = compression.get("media", {})
        filters = compression.get("filters", [])
        if not media.get("enable") and not filters:
            context = data.get("context")
            if isinstance(context, dict):
                context.pop("extensionPath", None)

    with open(output_path, "w", encoding="utf-8") as handle:
        json.dump(convert(data), handle, ensure_ascii=False)

    return output_path

def convert(value: object):
    if isinstance(value, dict):
        return {key: convert(item) for key, item in value.items()}
    if isinstance(value, list):
        return [convert(item) for item in value]
    if looks_like_path(value):
        resolved = rewrite_sdk_overlay_path(value)
        if os.path.isfile(resolved) and os.path.splitext(resolved)[1].lower() in {".json", ".json5"}:
            return to_windows_path(rewrite_json_file(resolved))
        return to_windows_path(value)
    return value

rewrite_json_file(src, dst)
PY

    convert_path "$tmp_file"
}

convert_response_file() {
    local value="$1"
    local source_path="${value#@}"
    local tmp_dir
    local tmp_file

    if ! command -v python3 >/dev/null 2>&1; then
        printf '@%s\n' "$(convert_path "$source_path")"
        return 0
    fi

    if ! path_exists_or_parent_exists "$source_path" || [ ! -f "$source_path" ]; then
        printf '@%s\n' "$(convert_path "$source_path")"
        return 0
    fi

    tmp_dir="$(mktemp -d "$(dirname "$source_path")/.response-file.XXXXXX")"
    tmp_file="$tmp_dir/$(basename "$source_path")"
    cleanup_temp_dir "$tmp_dir"
    python3 - "$source_path" "$tmp_file" "$HOST_ADAPTER" <<'PY'
import os
import re
import subprocess
import sys

src, dst, host_adapter = sys.argv[1:4]
ohos_from = os.environ.get("SDK_MAP_OHOS_FROM", "")
ohos_to = os.environ.get("SDK_MAP_OHOS_TO", "")
hms_from = os.environ.get("SDK_MAP_HMS_FROM", "")
hms_to = os.environ.get("SDK_MAP_HMS_TO", "")
restool_plugin_dir = os.environ.get("RESTOOL_PLUGIN_DIR", "")

def rewrite_sdk_overlay_path(value: str) -> str:
    mapped = value
    basename = os.path.basename(mapped)

    if restool_plugin_dir and basename in {
        "libimage_transcoder_shared.so",
        "libimage_transcoder_shared.dylib",
        "libimage_transcoder_shared.dll",
    }:
        mapped = os.path.join(restool_plugin_dir, "libimage_transcoder_shared.dll")

    if hms_from and hms_to and mapped.startswith(f"{hms_from}/"):
        mapped = f"{hms_to}{mapped[len(hms_from):]}"
    elif ohos_from and ohos_to and mapped.startswith(f"{ohos_from}/"):
        mapped = f"{ohos_to}{mapped[len(ohos_from):]}"

    if not os.path.exists(mapped):
        if mapped.endswith(".so"):
            dll_candidate = f"{mapped[:-3]}.dll"
            if os.path.exists(dll_candidate):
                mapped = dll_candidate
        elif mapped.endswith(".dylib"):
            dll_candidate = f"{mapped[:-6]}.dll"
            if os.path.exists(dll_candidate):
                mapped = dll_candidate

    return mapped

def looks_like_path(value: str) -> bool:
    if not isinstance(value, str) or not value.startswith("/") or "/" not in value[1:]:
        return False

    resolved = rewrite_sdk_overlay_path(value)
    return os.path.exists(resolved) or os.path.exists(os.path.dirname(resolved) or "/")

def to_windows_path(value: str) -> str:
    resolved = rewrite_sdk_overlay_path(value)
    cmd = ["wslpath", "-w", resolved] if host_adapter == "wsl" else ["cygpath", "-w", resolved]
    return subprocess.check_output(cmd, text=True).strip()

pattern = re.compile(r"/[^;\s\r\n\"']+")
text = open(src, "r", encoding="utf-8").read()

def replace(match: re.Match[str]) -> str:
    token = match.group(0)
    if looks_like_path(token):
        return to_windows_path(token)
    return token

with open(dst, "w", encoding="utf-8") as handle:
    handle.write(pattern.sub(replace, text))
PY

    printf '@%s\n' "$(convert_path "$tmp_file")"
}

build_windows_path_env() {
    local current_windows_path=""
    local extra_windows_path=""
    local item=""
    local resolved=""
    local converted=""
    local old_ifs="$IFS"

    if command -v cmd.exe >/dev/null 2>&1; then
        current_windows_path="$(cmd.exe /c echo %PATH% 2>/dev/null | tr -d '\r' | tail -n 1)"
    fi

    if [ -n "${WINDOWS_EXE_EXTRA_PATHS:-}" ] && [ "$HOST_ADAPTER" != "posix" ]; then
        IFS=':'
        for item in $WINDOWS_EXE_EXTRA_PATHS; do
            [ -n "$item" ] || continue
            resolved="$(rewrite_sdk_overlay_path "$item")"
            [ -d "$resolved" ] || continue
            converted="$(to_windows_path "$resolved")"
            if [ -z "$extra_windows_path" ]; then
                extra_windows_path="$converted"
            else
                extra_windows_path="$extra_windows_path;$converted"
            fi
        done
        IFS="$old_ifs"
    fi

    if [ -n "$extra_windows_path" ]; then
        if [ -n "$current_windows_path" ]; then
            printf '%s;%s\n' "$extra_windows_path" "$current_windows_path"
        else
            printf '%s\n' "$extra_windows_path"
        fi
        return 0
    fi

    printf '%s\n' "$current_windows_path"
}

converted=()
expect_path=0
expect_restool_file_list=0

for arg in "$@"; do
    if [ "$expect_restool_file_list" -eq 1 ]; then
        converted+=("$(convert_restool_file_list "$arg")")
        expect_restool_file_list=0
        continue
    fi

    if [ "$expect_path" -eq 1 ]; then
        converted+=("$(convert_path "$arg")")
        expect_path=0
        continue
    fi

    case "$arg" in
        -l|--fileList)
            converted+=("$arg")
            expect_restool_file_list=1
            ;;
        -I|-L|-B|-isystem|-isysroot|-o|-MF|-include|-imacros|-resource-dir|-T|--sysroot|--gcc-toolchain)
            converted+=("$arg")
            expect_path=1
            ;;
        --fileList=*)
            converted+=("${arg%%=*}=$(convert_restool_file_list "${arg#*=}")")
            ;;
        --sysroot=*|--gcc-toolchain=*|--config=*|--resource-dir=*|--version-script=*)
            converted+=("${arg%%=*}=$(convert_path "${arg#*=}")")
            ;;
        -I/*|-L/*|-B/*)
            converted+=("${arg:0:2}$(convert_path "${arg:2}")")
            ;;
        -isystem/*)
            converted+=("-isystem$(convert_path "${arg:8}")")
            ;;
        -isysroot/*)
            converted+=("-isysroot$(convert_path "${arg:9}")")
            ;;
        -o/*)
            converted+=("-o$(convert_path "${arg:2}")")
            ;;
        -MF/*)
            converted+=("-MF$(convert_path "${arg:3}")")
            ;;
        -include/*)
            converted+=("-include$(convert_path "${arg:8}")")
            ;;
        -imacros/*)
            converted+=("-imacros$(convert_path "${arg:9}")")
            ;;
        -resource-dir/*)
            converted+=("-resource-dir$(convert_path "${arg:13}")")
            ;;
        -T/*)
            converted+=("-T$(convert_path "${arg:2}")")
            ;;
        @/*)
            converted+=("$(convert_response_file "$arg")")
            ;;
        /*)
            converted+=("$(convert_path "$arg")")
            ;;
        *)
            converted+=("$arg")
            ;;
    esac
done

if [[ "$tool" == *.exe ]]; then
    windows_path_env="$(build_windows_path_env)"
    if [ -n "$windows_path_env" ]; then
        export PATH="$windows_path_env"
    fi
fi

if [ "$HOST_ADAPTER" = "msys2" ] && [[ "$tool" == *.exe ]]; then
    export MSYS2_ARG_CONV_EXCL='*'
    export MSYS2_ENV_CONV_EXCL='*'
fi

"$tool" "${converted[@]}"

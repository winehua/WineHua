#!/bin/bash
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: wayland-scanner [options] <mode> <input.xml> <output>" >&2
    exit 64
fi

command -v wsl.exe >/dev/null 2>&1 || {
    echo "wsl.exe not found; cannot use WSL wayland-scanner wrapper" >&2
    exit 1
}

run_wsl() {
    if command -v cygpath >/dev/null 2>&1; then
        MSYS2_ARG_CONV_EXCL='*' MSYS2_ENV_CONV_EXCL='*' wsl.exe "$@"
    else
        wsl.exe "$@"
    fi
}

resolve_to_wsl_path() {
    local value="$1"
    local absolute=""
    local drive=""
    local rest=""

    case "$value" in
        /dev/null|-)
            printf '%s\n' "$value"
            return 0
            ;;
    esac

    if [[ "$value" =~ ^([A-Za-z]):[\\/](.*)$ ]]; then
        drive="${BASH_REMATCH[1],,}"
        rest="${BASH_REMATCH[2]//\\//}"
        if [ -n "$rest" ]; then
            printf '/mnt/%s/%s\n' "$drive" "$rest"
        else
            printf '/mnt/%s\n' "$drive"
        fi
        return 0
    elif [[ "$value" =~ ^/([A-Za-z])/(.*)$ ]]; then
        drive="${BASH_REMATCH[1],,}"
        rest="${BASH_REMATCH[2]}"
        if [ -n "$rest" ]; then
            printf '/mnt/%s/%s\n' "$drive" "$rest"
        else
            printf '/mnt/%s\n' "$drive"
        fi
        return 0
    elif [[ "$value" = /mnt/* ]]; then
        printf '%s\n' "$value"
        return 0
    elif [[ "$value" = /* ]]; then
        absolute="$(realpath -m "$value")"
    else
        absolute="$(realpath -m "$PWD/$value")"
    fi

    if [[ "$absolute" =~ ^/([A-Za-z])/(.*)$ ]]; then
        drive="${BASH_REMATCH[1],,}"
        rest="${BASH_REMATCH[2]}"
        if [ -n "$rest" ]; then
            printf '/mnt/%s/%s\n' "$drive" "$rest"
        else
            printf '/mnt/%s\n' "$drive"
        fi
        return 0
    fi

    printf '%s\n' "$absolute"
}

looks_like_path() {
    local value="$1"

    case "$value" in
        /dev/null|-)
            return 0
            ;;
    esac

    [[ "$value" =~ ^[A-Za-z]:[\\/].*$ ]] && return 0
    [[ "$value" = /* ]] && return 0
    [[ "$value" == *"/"* ]] && return 0
    [[ "$value" == *"\\"* ]] && return 0
    return 1
}

detect_wsl_scanner() {
    local scanner_path=""

    scanner_path="$(run_wsl sh -lc 'command -v wayland-scanner 2>/dev/null || true' 2>/dev/null | tr -d '\r')"
    [ -n "$scanner_path" ] || {
        echo "wayland-scanner not found inside WSL" >&2
        exit 1
    }
    printf '%s\n' "$scanner_path"
}

shell_quote() {
    local value="$1"
    printf "'%s'" "${value//\'/\'\\\'\'}"
}

wsl_scanner="$(detect_wsl_scanner)"
converted_args=()

for arg in "$@"; do
    if looks_like_path "$arg"; then
        converted_args+=("$(resolve_to_wsl_path "$arg")")
    else
        converted_args+=("$arg")
    fi
done

quoted_command="exec $(shell_quote "$wsl_scanner")"
for arg in "${converted_args[@]}"; do
    quoted_command="$quoted_command $(shell_quote "$arg")"
done

run_wsl sh -lc "$quoted_command"

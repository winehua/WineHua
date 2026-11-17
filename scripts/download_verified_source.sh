#!/bin/bash
# Download to a temporary file and publish only checksum-verified archives.
# Callers provide log/err and the URL, SHA-256, and final cache path.
download_verified_source() {
    local url="$1" checksum="$2" target="$3" pending mirror candidate ok=0
    if [ -f "$target" ] && echo "$checksum  $target" | sha256sum -c - >/dev/null 2>&1; then
        return 0
    fi
    log "--- 下载 $(basename "$target") ---"
    pending=$(mktemp "${target}.part.XXXXXX")
    mirror=""
    case "$url" in
        https://xorg.freedesktop.org/archive/individual/*)
            mirror="https://www.x.org/archive/individual/${url#https://xorg.freedesktop.org/archive/individual/}" ;;
        https://github.com/dottedmag/libsha1/archive/*.tar.gz)
            mirror="${url#https://github.com/dottedmag/libsha1/archive/}"
            mirror="https://codeload.github.com/dottedmag/libsha1/tar.gz/${mirror%.tar.gz}" ;;
    esac
    for candidate in "$url" "$mirror"; do
        [ -n "$candidate" ] || continue
        if curl -fsSL --retry 3 --retry-all-errors --connect-timeout 30 -o "$pending" "$candidate" &&
           echo "$checksum  $pending" | sha256sum -c - >/dev/null 2>&1; then
            mv -f "$pending" "$target"
            ok=1
            break
        fi
    done
    rm -f "$pending"
    [ "$ok" = 1 ] || err "下载失败或 sha256 不匹配: $target"
}

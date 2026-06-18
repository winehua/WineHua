#!/bin/bash
# package.sh — HNP 打包 + HAP 构建 + 签名 + 部署
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
package_hnp() {
    log "=== 打包 HNP ==="
    mkdir -p "$OUT_DIR"
    "$HNPCLI" pack -i "$STAGING_DIR" -o "$OUT_DIR" -n winehua -v 0.1.0 || { err "hnpcli pack 失败"; return 1; }
    ls -lh "$OUT_DIR/winehua.hnp"
}

package_hap() {
    log "=== 打包 HAP ==="
    local unsigned_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-unsigned.hap"
    local signed_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    mkdir -p "$WINEHUA/entry/hnp/arm64-v8a"
    cp "$OUT_DIR/winehua.hnp" "$WINEHUA/entry/hnp/arm64-v8a/winehua.hnp"

    cd "$WINEHUA"
    hvigorw assembleHap || { err "hvigorw assembleHap 失败"; return 1; }

    cd "$WINEHUA/entry"
    zip -r "$unsigned_hap" hnp

    cd "$WINEHUA"
    python3 sign.py "$unsigned_hap" "$signed_hap"

    ls -lh "$signed_hap"
    log "HAP 构建 + 签名完成"
}

deploy() {
    local device="${1:-192.168.1.4:38879}"
    local hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    if [ ! -f "$hap" ]; then
        err "HAP 文件不存在: $hap"
    fi

    log "=== 部署到 $device ==="
    hdc tconn "$device" || { err "hdc tconn 失败"; }
    hdc shell bm uninstall -n app.hackeris.winehua 2>/dev/null || true
    hdc file send "$hap" /data/local/tmp/ || { err "hdc file send 失败"; }
    hdc shell bm install -p /data/local/tmp/entry-default-signed.hap -r || { err "bm install 失败"; }

    log "部署完成"
}

# ---- main ----
case "${1:-}" in
    hnp)  package_hnp ;;
    hap)  package_hap ;;
    deploy) deploy "${2:-}" ;;
    all)  package_hnp && package_hap && deploy "${2:-}" ;;
    *)    echo "Usage: $0 {hnp|hap|deploy|all} [device_ip]" >&2; exit 1 ;;
esac

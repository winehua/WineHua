#!/bin/bash
# package.sh — HNP 打包 + HAP 构建 + 签名 + 部署
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
package_hnp() {
    log "=== 打包 HNP ==="
    mkdir -p "$OUT_DIR"
    "$HNPCLI" pack -i "$STAGING_DIR" -o "$OUT_DIR" -n winebox -v 0.1.0
    ls -lh "$OUT_DIR/winebox.hnp"
}

package_hap() {
    log "=== 打包 HAP ==="
    local unsigned_hap="$HONWINE/entry/build/default/outputs/default/entry-default-unsigned.hap"
    local signed_hap="$HONWINE/entry/build/default/outputs/default/entry-default-signed.hap"

    mkdir -p "$HONWINE/entry/hnp/arm64-v8a"
    cp "$OUT_DIR/winebox.hnp" "$HONWINE/entry/hnp/arm64-v8a/honwine.hnp"

    cd "$HONWINE"
    hvigorw assembleHap

    cd "$HONWINE/entry"
    zip -r "$unsigned_hap" hnp

    cd "$HONWINE"
    python3 sign.py "$unsigned_hap" "$signed_hap"

    ls -lh "$signed_hap"
    log "HAP 构建 + 签名完成"
}

deploy() {
    local device="${1:-192.168.1.4:38879}"
    local hap="$HONWINE/entry/build/default/outputs/default/entry-default-signed.hap"

    log "=== 部署到 $device ==="
    hdc tconn "$device"
    hdc shell bm uninstall -n app.hackeris.honwine 2>/dev/null || true
    hdc file send "$hap" /data/local/tmp/
    hdc shell bm install -p /data/local/tmp/entry-default-signed.hap -r

    log "部署完成。测试命令:"
    echo ""
    echo "  cd /data/service/hnp/winebox.org/winebox_0.1.0/opt/winebox"
    echo "  rm -rf /data/local/tmp/.wine"
    echo '  WINEPREFIX=/data/local/tmp/.wine ./bin/box64 ./bin/wine ./bin/cmd.exe /c echo hello 2>&1'
}

# ---- main ----
case "${1:-}" in
    hnp)  package_hnp ;;
    hap)  package_hap ;;
    deploy) deploy "${2:-}" ;;
    all)  package_hnp && package_hap && deploy "${2:-}" ;;
    *)    echo "Usage: $0 {hnp|hap|deploy|all} [device_ip]" >&2; exit 1 ;;
esac

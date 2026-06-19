#!/bin/bash
# package.sh — HNP 打包 + HAP 构建 + 签名 + 部署
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
# 工具函数: 动态设置 abiFilters
set_abi_filters() {
    # 根据 NATIVE_ARCH 写 build-profile.json5 的 abiFilters
    local profile="$WINEHUA/entry/build-profile.json5"
    if [ ! -f "$profile" ]; then
        err "build-profile.json5 未找到: $profile"
    fi

    local abi_value
    if [ "$NATIVE_ARCH" = "all" ]; then
        abi_value='"arm64-v8a", "x86_64"'
    else
        abi_value="\"$NATIVE_ARCH\""
    fi

    # 用 python 正则替换, 支持多行 abiFilters
    python3 -c "
import re
with open('$profile', 'r') as f:
    content = f.read()
content = re.sub(r'\"abiFilters\"\s*:\s*\[[^\]]*\]', '\"abiFilters\": [$abi_value]', content)
with open('$profile', 'w') as f:
    f.write(content)
"
    log "abiFilters: [$abi_value]"
}

# ============================================================
package_hnp() {
    log "=== 打包 HNP ($NATIVE_ARCH) ==="
    mkdir -p "$OUT_DIR"
    "$HNPCLI" pack -i "$STAGING_DIR" -o "$OUT_DIR" -n winehua -v 0.1.0 || { err "hnpcli pack 失败"; return 1; }

    # HNP 按架构存放
    local hnp_dir="$WINEHUA/entry/hnp/$NATIVE_ARCH"
    mkdir -p "$hnp_dir"
    cp "$OUT_DIR/winehua.hnp" "$hnp_dir/winehua.hnp"
    ls -lh "$hnp_dir/winehua.hnp"
}

# ============================================================
package_hap() {
    log "=== 打包 HAP ($NATIVE_ARCH) ==="
    local unsigned_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-unsigned.hap"
    local signed_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    set_abi_filters

    # 清理非目标架构的 native libs (hvigorw ProcessLibs 会打包所有 libs/)
    local libs_root="$WINEHUA/entry/libs"
    if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        rm -rf "$libs_root/x86_64"
    elif [ "$NATIVE_ARCH" = "x86_64" ]; then
        rm -rf "$libs_root/arm64-v8a"
    fi
    # NATIVE_ARCH=all 时保留两个架构

    cd "$WINEHUA"
    hvigorw assembleHap || { err "hvigorw assembleHap 失败"; return 1; }

    cd "$WINEHUA/entry"
    # 清理非目标架构的 HNP (hvigorw 不处理 hnp/, 所以这时清理即可)
    local hnp_root="$WINEHUA/entry/hnp"
    if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        rm -rf "$hnp_root/x86_64"
    elif [ "$NATIVE_ARCH" = "x86_64" ]; then
        rm -rf "$hnp_root/arm64-v8a"
    fi
    # NATIVE_ARCH=all 时保留两个架构

    # 将 HNP 目录打包进 HAP (hvigorw 不会自动处理 hnp/)
    zip -r "$unsigned_hap" hnp

    cd "$WINEHUA"
    python3 sign.py "$unsigned_hap" "$signed_hap"

    ls -lh "$signed_hap"
    log "HAP 构建 + 签名完成 ($NATIVE_ARCH)"
}

# ============================================================
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
    *)    echo "用法: $0 {hnp|hap|deploy|all} [device_ip]" >&2; exit 1 ;;
esac

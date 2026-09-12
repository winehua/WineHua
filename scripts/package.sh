#!/bin/bash
# package.sh — HAP 构建 + 签名 + 部署
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
# 工具函数: 从用户挂载目录导入 build-profile.json5 + 签名材料
# 挂载约定 (二者需同时存在, 否则回退到项目自带模板):
#   -v C:\path\to\winehua_project:/mnt/user-profile
#   -v C:\path\to\signature_dir:/mnt/user-signature
# ============================================================
USER_PROFILE_DIR="${USER_PROFILE_DIR:-/mnt/user-profile}"
USER_SIGNATURE_DIR="${USER_SIGNATURE_DIR:-/mnt/user-signature}"

import_user_profile() {
    local src_profile="$USER_PROFILE_DIR/build-profile.json5"
    local dst_profile="$WINEHUA/build-profile.json5"

    if [ ! -f "$src_profile" ] || [ ! -d "$USER_SIGNATURE_DIR" ]; then
        log "未检测到用户挂载 (profile=$src_profile, signature=$USER_SIGNATURE_DIR)"
        log "  → 使用项目内置 build-profile.json5"
        return 0
    fi

    log "=== 检测到用户挂载, 导入 build-profile.json5 + 签名材料 ==="
    log "  profile   : $src_profile"
    log "  signature : $USER_SIGNATURE_DIR"

    cp "$src_profile" "$dst_profile"

    python3 - "$dst_profile" "$USER_SIGNATURE_DIR" <<'PY'
import re, sys

profile_path, sig_dir = sys.argv[1:]
sig_dir = sig_dir.rstrip("/")

with open(profile_path, "r", encoding="utf-8") as f:
    content = f.read()

# 仅重写签名材料三件套的路径, 其他字段 (buildOption 之类) 一律不碰
pattern = re.compile(r'("(?:certpath|profile|storeFile)"\s*:\s*)"([^"]+)"')

def rewrite(m):
    prefix, value = m.group(1), m.group(2)
    # 按正/反斜杠切分, 过滤空段, 取最后一段作为文件名
    parts = [p for p in re.split(r'[\\/]+', value) if p]
    fname = parts[-1] if parts else value
    new_path = f"{sig_dir}/{fname}"
    print(f"  rewrite: {value}  ->  {new_path}", file=sys.stderr)
    return f'{prefix}"{new_path}"'

content = pattern.sub(rewrite, content)

with open(profile_path, "w", encoding="utf-8") as f:
    f.write(content)
PY

    log "build-profile.json5 导入完成"
}

# ============================================================
# 工具函数: 同步根 build-profile 的 SDK 版本
set_sdk_versions() {
    local profile="$WINEHUA/build-profile.json5"
    local target_version="${TARGET_SDK_VERSION:-6.1.0(23)}"
    local compatible_version="${COMPATIBLE_SDK_VERSION:-6.1.0(23)}"

    if [ ! -f "$profile" ]; then
        err "build-profile.json5 未找到: $profile"
    fi

    python3 - "$profile" "$target_version" "$compatible_version" <<'PY'
import re
import sys

profile_path, target_version, compatible_version = sys.argv[1:]
with open(profile_path, "r", encoding="utf-8") as profile_file:
    content = profile_file.read()

for key, value in (
    ("targetSdkVersion", target_version),
    ("compatibleSdkVersion", compatible_version),
):
    pattern = rf'("{key}"\s*:\s*)"[^"]*"'
    content, count = re.subn(
        pattern,
        lambda match, version=value: f'{match.group(1)}"{version}"',
        content,
        count=1,
    )
    if count != 1:
        raise SystemExit(f"unable to update {key} in {profile_path}")

with open(profile_path, "w", encoding="utf-8") as profile_file:
    profile_file.write(content)
PY

    log "SDK versions: target=$target_version, compatible=$compatible_version"
}

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
package_hap() {
    log "=== 打包 HAP ($NATIVE_ARCH) ==="
    local signed_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    import_user_profile     # <-- 优先使用用户挂载的 profile + 签名
    set_sdk_versions
    set_abi_filters

    # 移除 hnpPackages (所有平台统一用 rawfile zip)
    local module_json="$WINEHUA/entry/src/main/module.json5"
    python3 -c "
import re
with open('$module_json', 'r') as f:
    content = f.read()
content = re.sub(r',?\s*\"hnpPackages\"\s*:\s*\[[^][]*\]', '', content)
with open('$module_json', 'w') as f:
    f.write(content)
"
    log "  已移除 hnpPackages 配置"

    # 清理非目标架构的 native libs (hvigorw ProcessLibs 会打包所有 libs/)
    local libs_root="$WINEHUA/entry/libs"
    if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        rm -rf "$libs_root/x86_64"
    elif [ "$NATIVE_ARCH" = "x86_64" ]; then
        rm -rf "$libs_root/arm64-v8a"
    fi

    cd "$WINEHUA"
    # hvigorw assembleHap 自带 SignHap 任务: 按根 build-profile.json5 的
    # signingConfigs 选材料 (调试 = .ohos/default_*.cer, 发布 =
    # .ohos/release/hish_beian.*), 直接产出 entry-default-signed.hap。
    # 2026-09-12 移除独立签名步骤 scripts/sign.py: 它拿 unsigned.hap 再签一遍
    # 只是重复劳动, 且材料选择逻辑与 hvigor 重复, 两边不同步时会签错证书。
    hvigorw assembleHap || { err "hvigorw assembleHap 失败"; return 1; }

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
    hap)  package_hap ;;
    deploy) deploy "${2:-}" ;;
    all)
        package_hap && deploy "${2:-}"
        ;;
    *)    echo "用法: $0 {hap|deploy|all} [device_ip]" >&2; exit 1 ;;
esac

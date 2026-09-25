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

    # NATIVE_ARCH=all 已移除 (env.sh 报错); 单一架构
    local abi_value="\"$NATIVE_ARCH\""

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
    local unsigned_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-unsigned.hap"
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

    # CMake 缓存按 WINE_ARCH 失效: hvigor 的 entry/.cxx CMake 缓存跨方案复用,
    # WINEHUA_WINE_ARCH_IS_X86_64 宏不会重新注入。实测: 方案③ 构建后方案② (WINE_ARCH=x86_64)
    # 复用 arm64-v8a CMake 缓存 → libentry.so 走 aarch64 路径 → 部署后 wineboot 初始化
    # 失败 (c0000135 / 加载 el1 ntdll)。切换 WINE_ARCH 时清 entry/.cxx 强制重新 configure。
    local cmake_arch_marker="$BUILD_DIR/.cmake_wine_arch"
    # marker 不存在 (首次/旧构建缓存) 或 WINE_ARCH 变化 → 清 CMake 缓存强制重新 configure
    if [ ! -f "$cmake_arch_marker" ] || [ "$(cat "$cmake_arch_marker" 2>/dev/null)" != "$WINE_ARCH" ]; then
        log "  WINE_ARCH=$WINE_ARCH (上次: $(cat "$cmake_arch_marker" 2>/dev/null || echo 无)), 清 CMake 缓存 (entry/.cxx)"
        rm -rf "$WINEHUA/entry/.cxx"
    fi
    printf '%s' "$WINE_ARCH" > "$cmake_arch_marker"
    # 把 WINE_ARCH 写入 entry/.wine_arch, 供 CMakeLists.txt 读取注入宏。
    # 不能依赖 ENV: hvigor daemon 常驻, 其 CMake 子进程环境是启动时快照, make 传的
    # WINE_ARCH 环境变量在 daemon 里丢失 (实测 ENV{WINE_ARCH} 读不到, 宏未注入)。
    printf '%s' "$WINE_ARCH" > "$WINEHUA/entry/.wine_arch"

    # rawfile wine-data 与本次 hap 的架构组合一致性校验: 方案切换 (如 方案③→②)
    # 后未重跑 assemble 直接 hap, 会把旧方案 staging 打进新宏的 HAP。
    local data_arch_marker="$WINEHUA/entry/src/main/resources/rawfile/.wine-data-arch"
    local expect_arch="$NATIVE_ARCH:$WINE_ARCH"
    if [ -f "$data_arch_marker" ]; then
        local actual_arch
        actual_arch="$(cat "$data_arch_marker")"
        [ "$actual_arch" = "$expect_arch" ] || \
            err "rawfile wine-data 是 '$actual_arch' 的 assemble 产物, 当前 hap 目标是 '$expect_arch'。请先重跑 assemble (make assemble 或 ./build.sh assemble)。"
    else
        warn "未找到 $data_arch_marker (旧版 assemble 产物?), 无法校验 wine-data 架构一致性"
    fi

    cd "$WINEHUA"
    hvigorw assembleHap || { err "hvigorw assembleHap 失败"; return 1; }

    cd "$WINEHUA"
    python3 sign.py "$unsigned_hap" "$signed_hap"
    python3 "$SCRIPT_DIR/check_runtime_components.py" --hap "$signed_hap"

    ls -lh "$signed_hap"
    log "HAP 构建 + 签名完成 ($NATIVE_ARCH)"
}

# ============================================================
deploy() {
    local device="${1:?用法: package.sh deploy <设备地址>}"
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

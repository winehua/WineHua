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
    return f'{prefix}"{new_path}"'

content = pattern.sub(rewrite, content)

with open(profile_path, "w", encoding="utf-8") as f:
    f.write(content)
PY

    log "build-profile.json5 导入完成"
}

# Fail before Hvigor's expensive native/ArkTS/package phases when the mounted
# signing profile is empty, incomplete, or belongs to another bundle.
validate_signing_profile() {
    local profile="$WINEHUA/build-profile.json5"
    local app_json="$WINEHUA/AppScope/app.json5"
    local signing_config="${WINEHUA_SIGNING_CONFIG:-}"
    local selected_profile expected_bundle

    IFS=$'\t' read -r selected_profile expected_bundle < <(
        python3 - "$profile" "$app_json" "$signing_config" <<'PY'
import json
import re
import sys
from pathlib import Path

profile_path = Path(sys.argv[1]).resolve()
app_path = Path(sys.argv[2]).resolve()
selected_name = sys.argv[3]

def load_json5(path):
    # Use the toolchain's JSON5 parser: comments, unquoted keys, single-quoted
    # strings and URLs must survive signing preflight unchanged.
    import os, subprocess
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        pass
    tool = Path(os.environ.get("TOOL_HOME", "/apps/harmony"))
    data = subprocess.check_output([str(tool / "tool/node/bin/node"), "-e",
        "const fs=require('fs'),j=require(process.argv[1]);"
        "process.stdout.write(JSON.stringify(j.parse(fs.readFileSync(process.argv[2],'utf8'))))",
        str(tool / "hvigor/hvigor-ohos-plugin/node_modules/json5"), str(path)])
    return json.loads(data)

profile = load_json5(profile_path)
configs = profile.get("app", {}).get("signingConfigs", [])
if not configs:
    raise SystemExit("signing preflight: app.signingConfigs is empty")

selected = configs[0]
if selected_name:
    selected = next((item for item in configs
                     if item.get("name") == selected_name), None)
    if selected is None:
        raise SystemExit(
            f"signing preflight: config not found: {selected_name}")

material = selected.get("material", {})
required = ("certpath", "profile", "storeFile", "keyAlias", "keyPassword",
            "storePassword", "signAlg")
missing = [key for key in required if not material.get(key)]
if missing:
    raise SystemExit(
        "signing preflight: missing material keys: " + ", ".join(missing))

def material_path(value):
    path = Path(value)
    return path if path.is_absolute() else (profile_path.parent / path).resolve()

paths = {key: material_path(material[key])
         for key in ("certpath", "profile", "storeFile")}
missing_files = [key for key, path in paths.items() if not path.is_file()]
if missing_files:
    raise SystemExit(
        "signing preflight: mounted material files are missing: " +
        ", ".join(missing_files))

expected_bundle = load_json5(app_path).get("app", {}).get("bundleName", "")
if not expected_bundle:
    raise SystemExit("signing preflight: AppScope bundleName is missing")
print(f"{paths['profile']}\t{expected_bundle}")
PY
    ) || err "签名配置预检失败"

    local result_file actual_bundle
    result_file="$(mktemp "${TMPDIR:-/tmp}/winehua-profile-verify.XXXXXX.json")"
    if ! java -jar "$TOOL_HOME/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar" \
            verify-profile -inFile "$selected_profile" \
            -outFile "$result_file" >/dev/null; then
        rm -f -- "$result_file"
        err "签名 profile 验证失败"
    fi
    actual_bundle="$(python3 - "$result_file" <<'PY'
import json
import sys

result = json.load(open(sys.argv[1], encoding="utf-8"))
print(result.get("content", {}).get("bundle-info", {}).get("bundle-name", ""))
PY
    )"
    rm -f -- "$result_file"
    [ -n "$actual_bundle" ] || err "签名 profile 未包含 bundleName"
    [ "$actual_bundle" = "$expected_bundle" ] || \
        err "签名 profile bundle 与 AppScope bundle 不匹配"
    log "签名配置预检通过 (bundle=$expected_bundle)"
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
    local mode="${1:-signed}"
    log "=== 打包 HAP ($NATIVE_ARCH) ==="
    local unsigned_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-unsigned.hap"
    local signed_hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    if [ "$mode" = unsigned ]; then
        # CI must opt in explicitly and must never import private signing data.
        python3 - "$WINEHUA/build-profile.json5" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0,str(Path(sys.argv[1]).resolve().parent/'scripts'))
from json5_data import load
profile = load(sys.argv[1])
app = profile.get("app", {})
if app.get("signingConfigs") or any(p.get("signingConfig") for p in app.get("products", [])):
    raise SystemExit("unsigned build requires an unsigned profile; private signing configuration is not allowed")
PY
    else
        import_user_profile
        validate_signing_profile
    fi
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
    mkdir -p "$BUILD_DIR"
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

    # Pair runtime hashes with the ELF bytes that release stripping will ship.
    # The normalizer rejects stale identities and any loadable-content change.
    python3 "$SCRIPT_DIR/normalize_runtime_identity.py" \
        --libs "$WINEHUA/entry/libs" --rawfile "$WINEHUA/entry/src/main/resources/rawfile" \
        --strip "$TOOL_HOME/sdk/default/openharmony/native/llvm/bin/llvm-strip"

    # A direct hap build may reuse old assemble outputs. Never silently ship a
    # stale SDK stub; leave unknown/generated inputs intact and require reassembly.
    python3 "$SCRIPT_DIR/runtime_library_guard.py" \
        "$WINEHUA/entry/libs" "$WINEHUA/entry/src/main/resources/rawfile"
    cd "$WINEHUA"
    # Match the validated distribution build; Hvigor otherwise defaults to Debug.
    hvigorw --mode module -p product=default -p buildMode=release assembleHap || { err "hvigorw assembleHap 失败"; return 1; }

    if [ -n "${SOURCE_BUILD_MANIFEST:-}" ]; then
        # All app CMake modules are source-built by Hvigor, after Wine assembly.
        # Bind those actual outputs into the receipt and repack incrementally.
        python3 "$SCRIPT_DIR/source_build_contract.py" --finalize-hap "$unsigned_hap" \
            --rawfile "$WINEHUA/entry/src/main/resources/rawfile" \
            --source-manifest "$SOURCE_BUILD_MANIFEST"
        hvigorw --mode module -p product=default -p buildMode=release assembleHap || \
            { err "hvigorw candidate receipt packaging failed"; return 1; }
    fi

    [ -s "$unsigned_hap" ] || err "unsigned HAP missing after assembleHap"
    python3 "$SCRIPT_DIR/runtime_library_guard.py" "$unsigned_hap"
    if [ "$WINE_ARCH" = aarch64 ] || [ "$NATIVE_ARCH" = x86_64 ]; then
        python3 "$SCRIPT_DIR/verify_x11_runtime.py" --hap "$unsigned_hap" --native-arch "$NATIVE_ARCH"
    fi
    # Unsigned distribution packages need the same final-byte compatibility
    # checks as signed builds; checking only the rawfile misses native stripping.
    local candidate_args=()
    if [ -n "${SOURCE_BUILD_MANIFEST:-}" ]; then
        candidate_args=(--source-build-manifest "$SOURCE_BUILD_MANIFEST")
    fi
    python3 "$SCRIPT_DIR/check_runtime_components.py" --hap "$unsigned_hap" "${candidate_args[@]}"
    if [ "$mode" = unsigned ]; then
        ls -lh "$unsigned_hap"
        log "HAP 构建完成 (unsigned, $NATIVE_ARCH); 安装前必须自行签名"
        return 0
    fi

    cd "$WINEHUA"
    python3 sign.py "$unsigned_hap" "$signed_hap"
    python3 "$SCRIPT_DIR/check_runtime_components.py" --hap "$signed_hap" "${candidate_args[@]}"

    ls -lh "$signed_hap"
    log "HAP 构建 + 签名完成 ($NATIVE_ARCH)"
}

# Store App uses the same offline gate, including its embedded HAP. Keep the
# package output in Hvigor's normal directory rather than copying it to stage.
package_app() {
    local mode="${1:-signed}"
    package_hap "$mode"
    hvigorw --mode project -p product=default -p buildMode=release assembleApp || \
        { err "hvigorw assembleApp 失败"; return 1; }
    local apps=() app
    mapfile -d '' apps < <(find "$WINEHUA/build/outputs/default" -maxdepth 1 -type f -name '*.app' -print0)
    [ "${#apps[@]}" -gt 0 ] || err "App missing after assembleApp"

}

# ============================================================
deploy() {
    local device="${1:-192.168.1.4:38879}"
    local hap="$WINEHUA/entry/build/default/outputs/default/entry-default-signed.hap"

    if [ ! -f "$hap" ]; then
        err "HAP 文件不存在: $hap"
    fi

    log "=== 部署到 $device ==="
    if ! hdc list targets 2>/dev/null | grep -qF "$device"; then
        hdc tconn "$device" >/dev/null 2>&1 || true
        hdc list targets 2>/dev/null | grep -qF "$device" || err "设备不在线: $device"
    fi

    # 每条 hdc 都带 -t: 多台设备同时在线时, 不带 -t 的命令只打印
    # "[Fail]ExecuteCommand need connect-key" 而**退出码仍是 0** —— 只看退出码会
    # 静默假成功 (实测: 两台在线时 deploy 报"部署完成", 设备上却还是旧包)。
    # 所以既校退出码, 也扫输出里的 [Fail] 标记。
    local out
    hdc -t "$device" shell bm uninstall -n app.hackeris.winehua >/dev/null 2>&1 || true

    out=$(hdc -t "$device" file send "$hap" /data/local/tmp/ 2>&1) || true
    echo "$out" | tail -1
    echo "$out" | grep -q '\[Fail\]' && err "推送失败: $(echo "$out" | grep '\[Fail\]' | head -1)"

    out=$(hdc -t "$device" shell bm install -p /data/local/tmp/entry-default-signed.hap -r 2>&1) || true
    echo "$out" | tail -2
    echo "$out" | grep -q '\[Fail\]' && err "安装失败: $(echo "$out" | grep '\[Fail\]' | head -1)"

    log "部署完成"
}

# ---- main ----
case "${1:-}" in
    hap)  package_hap ;;
    hap-unsigned) package_hap unsigned ;;
    app) package_app ;;
    app-unsigned) package_app unsigned ;;
    signing-preflight)
        import_user_profile
        set_sdk_versions
        validate_signing_profile
        ;;
    deploy) deploy "${2:-}" ;;
    all)
        package_hap && deploy "${2:-}"
        ;;
    *)    echo "用法: $0 {hap|hap-unsigned|app|app-unsigned|signing-preflight|deploy|all} [device_ip]" >&2; exit 1 ;;
esac

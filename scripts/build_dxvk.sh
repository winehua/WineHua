#!/bin/bash
# Build the managed WineHua DXVK Legacy fork for both PE architectures.
# This runs inside the canonical Docker build container; the Meson build
# directories are intentionally kept beside the fork so later DXVK changes
# can be rebuilt incrementally without touching the Wine tree.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

[ -d "$DXVK_SRC" ] || err "DXVK fork missing: $DXVK_SRC"

python3 - "$DXVK_SRC" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
for name in ('src/d3d10/d3d10_interfaces.h', 'src/dxvk/dxvk_gpu_query.cpp',
             'src/d3d9/d3d9_device.cpp'):
    path = root / name
    data = path.read_bytes()
    normalized = data.replace(b'\r\n', b'\n')
    if data != normalized:
        path.write_bytes(normalized)
PY
for compat_patch in "$SCRIPT_DIR/patches/dxvk-legacy-mingw-compat.patch" \
                    "$SCRIPT_DIR/patches/dxvk-legacy-d3d9-features.patch"; do
    if ! patch -d "$DXVK_SRC" -p1 --reverse --dry-run --silent < "$compat_patch" >/dev/null 2>&1; then
        patch -d "$DXVK_SRC" -p1 --forward --batch < "$compat_patch"
    fi
done

# 读取 meson 缓存里配置的源码绝对路径 (可能为空)
configured_source_of() {
    local build_dir="$1"
    [ -f "$build_dir/meson-info/meson-info.json" ] || { printf ''; return; }
    python3 - "$build_dir/meson-info/meson-info.json" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    print(d["directories"]["source"])
except Exception:
    print("")
PY
}

setup_if_missing() {
    local build_dir="$1"
    local cross_file="$2"
    local prefix="$3"
    local probe_cxx=x86_64-w64-mingw32-g++
    [ "$cross_file" != build-win32.txt ] || probe_cxx=i686-w64-mingw32-g++
    local probe_dir uuid_probe
    probe_dir="$(mktemp -d "${TMPDIR:-/tmp}/winehua-dxvk-uuid.XXXXXX")"
    uuid_probe="$probe_dir/probe.exe"
    local uuid_cpp_args=""
    # MinGW releases differ in whether d3d10effect.h declares this UUID.
    # Probe the installed headers rather than redeclaring it unconditionally.
    if ! printf '%s\n' '#include <d3d10_1.h>' \
        'int main() { return __uuidof(ID3D10StateBlock).Data1 == 0; }' \
        | "$probe_cxx" -x c++ - -o "$uuid_probe" >/dev/null 2>&1; then
        uuid_cpp_args=-DWINEHUA_NEEDS_D3D10_STATE_BLOCK_UUID
    fi
    # Older MinGW headers use a different name for the same D3D9 structure.
    if ! printf '%s\n' '#include <d3d9.h>' \
        'int main() { return sizeof(D3DDEVINFO_RESOURCEMANAGER) == 0; }' \
        | "$probe_cxx" -x c++ - -o "$uuid_probe" >/dev/null 2>&1; then
        uuid_cpp_args+=" -DD3DDEVINFO_RESOURCEMANAGER=D3DDEVINFO_D3DRESOURCEMANAGER"
    fi
    rm -f "$uuid_probe"
    rmdir "$probe_dir"
    # 仓库被移动/换挂载路径时 (例如另一台机器 clone 到别处, 或经 symlink 挂载),
    # 旧 build 目录会把旧绝对路径缓存在 meson-info.json, 直接 ninja 会报 "Neither
    # source directory '<旧路径>' ... contain a build file meson.build"。检测到路径
    # 不一致就重建, 让构建与源码所在路径无关。
    # 注意 meson 缓存的是 realpath 归一化后的物理路径, 而 $DXVK_SRC 可能是逻辑
    # 路径 (含 symlink 组件), 比较前先 realpath 归一到同基准, 避免 symlink
    # 环境下每次误判重建 (全量重编)。
    if [ -f "$build_dir/build.ninja" ]; then
        local configured_src
        configured_src="$(configured_source_of "$build_dir")"
        if [ -n "$configured_src" ] && [ "$configured_src" != "$(realpath "$DXVK_SRC")" ]; then
            log "DXVK $(basename "$build_dir") 源码路径变化: $configured_src -> $DXVK_SRC, 重建 build 目录"
            rm -rf "$build_dir"
        fi
    fi
    if [ ! -f "$build_dir/build.ninja" ]; then
        log "Configuring DXVK $(basename "$build_dir")"
        meson setup "$build_dir" "$DXVK_SRC" \
            --cross-file "$DXVK_SRC/$cross_file" \
            --prefix "$prefix" -Dbuildtype=release "-Dcpp_args=$uuid_cpp_args"
    else
        meson configure "$build_dir" "-Dcpp_args=$uuid_cpp_args"
    fi
}

setup_if_missing "$DXVK_SRC/build.winehua64" build-win64.txt "$DXVK_BUILD_ROOT/x64"
setup_if_missing "$DXVK_SRC/build.winehua32" build-win32.txt "$DXVK_BUILD_ROOT/x86"

log "--- DXVK Legacy fork ($DXVK_SRC) ---"
ninja -j"$JOBS" -C "$DXVK_SRC/build.winehua64" install
ninja -j"$JOBS" -C "$DXVK_SRC/build.winehua32" install

for dll in d3d9.dll d3d10.dll d3d10_1.dll d3d10core.dll d3d11.dll dxgi.dll; do
    [ -f "$DXVK_BUILD_ROOT/x64/bin/$dll" ] || \
        err "DXVK x64 artifact missing: $DXVK_BUILD_ROOT/x64/bin/$dll"
    [ -f "$DXVK_BUILD_ROOT/x86/bin/$dll" ] || \
        err "DXVK x86 artifact missing: $DXVK_BUILD_ROOT/x86/bin/$dll"
done

log "DXVK Legacy ready: $(git -c safe.directory="$DXVK_SRC" -C "$DXVK_SRC" rev-parse --short HEAD)"

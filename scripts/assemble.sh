#!/bin/bash
# assemble.sh — 组装 HNP 打包临时目录
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 组装 HNP 布局 ==="

# staging 目录每次重建
rm -rf "$STAGING_DIR"
mkdir -p "$HNP_LAYOUT/bin"
mkdir -p "$HNP_LAYOUT/lib/x86_64"
mkdir -p "$HNP_LAYOUT/share/wine/nls"

BIN="$HNP_LAYOUT/bin"

# ---- 主二进制 ----
cp "$WINE_SRC/build-ohos/loader/wine" "$BIN/"

# wineserver: 优先手动编译版 (含 __ANDROID__), 回退 make 版
if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
    cp "$BUILD_DIR/wine_server/wineserver" "$BIN/"
elif [ -f "$WINE_SRC/build-ohos/server/wineserver" ]; then
    cp "$WINE_SRC/build-ohos/server/wineserver" "$BIN/"
else
    err "wineserver 未找到！请先执行: bash scripts/build_wine.sh"
fi

# box64: 保留已有 (/build/box64_build 优于已安装的)
if [ -f "$BUILD_DIR/box64_build/box64" ]; then
    cp "$BUILD_DIR/box64_build/box64" "$BIN/"
elif [ ! -f "$BIN/box64" ]; then
    err "box64 未找到！请先执行: bash scripts/build_box64.sh"
fi

# ---- ntdll.so (必须在 bin/ — wine loader 硬编码加载) ----
cp "$WINE_SRC/build-ohos/dlls/ntdll/ntdll.so" "$BIN/"

# ---- x86_64-unix/ (其他 .so — load_builtin_unixlib 拼接路径) ----
mkdir -p "$BIN/x86_64-unix"
for so in "$WINE_SRC/build-ohos/dlls/"*/*.so; do
    [ "$(basename "$so")" = "ntdll.so" ] && continue
    cp "$so" "$BIN/x86_64-unix/"
done
log "  x86_64-unix: $(ls "$BIN/x86_64-unix" | wc -l) .so files"

# ---- x86_64-windows/ (PE DLL) ----
mkdir -p "$BIN/x86_64-windows"
for dll in "$WINE_SRC/build-native/dlls/"*/x86_64-windows/*.dll; do
    cp "$dll" "$BIN/x86_64-windows/"
done
log "  x86_64-windows: $(ls "$BIN/x86_64-windows" | wc -l) .dll files"

# ---- *.exe stubs ----
for exe in "$WINE_SRC/build-native/programs/"*/x86_64-windows/*.exe; do
    cp "$exe" "$BIN/"
done
log "  *.exe stubs: $(ls "$BIN"/*.exe 2>/dev/null | wc -l) files"

# ---- libc + NLS + wine.inf ----
cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$HNP_LAYOUT/lib/x86_64/"
cp "$WINE_SRC/build-native/nls/"*.nls "$HNP_LAYOUT/share/wine/nls/"
cp "$WINE_SRC/build-native/loader/wine.inf" "$HNP_LAYOUT/share/wine/"

# ---- 启动脚本 ----
cat > "$BIN/wine.sh" << 'SCRIPT'
#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
export BOX64_LD_LIBRARY_PATH="$DIR:$DIR/../lib/x86_64"
exec "$DIR/box64" "$DIR/wine" "$@"
SCRIPT
chmod +x "$BIN/wine.sh"

log "HNP 布局组装完成"
echo ""
echo "  $BIN/"
echo "  ├── wine, wineserver, box64"
echo "  ├── ntdll.so                  ← wine loader 直接加载"
echo "  ├── *.exe                     ← PE stubs ($(ls "$BIN"/*.exe 2>/dev/null | wc -l) files)"
echo "  ├── x86_64-windows/           ← PE DLL ($(ls "$BIN/x86_64-windows" | wc -l) files)"
echo "  └── x86_64-unix/              ← Unix .so ($(ls "$BIN/x86_64-unix" | wc -l) files)"

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

# ---- libc + 交叉编译依赖 + NLS + wine.inf + fonts ----
cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$HNP_LAYOUT/lib/x86_64/"

# 交叉编译依赖 → bin/x86_64-unix/ (文件名 = ELF SONAME)
# 来源: sysroot-ext (标准) 或 SDK sysroot (回退, 旧版)
pick_lib() {
    local name="$1"
    local soname="$2"
    if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
        cp "$SYSROOT_EXT_LIB/$soname" "$HNP_LAYOUT/bin/x86_64-unix/$soname"
    elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$HNP_LAYOUT/bin/x86_64-unix/$soname"
    else
        warn "$soname 未找到"
    fi
}

pick_lib "libfreetype.so.6.20.2"        "libfreetype.so.6"
pick_lib "libz.so"                      "libz.so"
pick_lib "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
pick_lib "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
pick_lib "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
pick_lib "libxml2.so.2.12.0"            "libxml2.so.2"
pick_lib "libffi.so.8.1.4"              "libffi.so.8"
log "  交叉编译依赖 → bin/x86_64-unix/"

# Wine 内置字体 (TrueType)
mkdir -p "$HNP_LAYOUT/share/wine/fonts"
cp "$WINE_SRC/fonts/"*.ttf "$HNP_LAYOUT/share/wine/fonts/"
log "  fonts: $(ls "$HNP_LAYOUT/share/wine/fonts" | wc -l) .ttf files"
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

# ---- mmap 测试工具 (终端版) ----
if [ -f "$ROOT/.temp/mmap_test" ]; then
    cp "$ROOT/.temp/mmap_test" "$BIN/"
    log "  mmap_test (终端版) → bin/"
fi

log "HNP 布局组装完成"
echo ""
echo "  $BIN/"
echo "  ├── wine, wineserver, box64"
echo "  ├── ntdll.so                  ← wine loader 直接加载"
echo "  ├── *.exe                     ← PE stubs ($(ls "$BIN"/*.exe 2>/dev/null | wc -l) files)"
echo "  ├── x86_64-windows/           ← PE DLL ($(ls "$BIN/x86_64-windows" | wc -l) files)"
echo "  └── x86_64-unix/              ← Unix .so ($(ls "$BIN/x86_64-unix" | wc -l) files)"

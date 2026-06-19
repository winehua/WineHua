#!/bin/bash
# assemble.sh — 组装 HNP 打包临时目录
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 组装 HNP 布局 ($NATIVE_ARCH) ==="

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

# box64: arm64 用真实 box64 二进制, x86_64 用 passthrough 包装器
# (C++ 代码硬编码 execve("./box64",...) 调用, 不能简单跳过)
if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
    if [ -f "$BUILD_DIR/box64_build/box64" ]; then
        cp "$BUILD_DIR/box64_build/box64" "$BIN/"
    elif [ ! -f "$BIN/box64" ]; then
        err "box64 未找到！请先执行: bash scripts/build_box64.sh"
    fi
    log "  box64 → bin/ (arm64, 真实二进制)"
else
    # x86_64: box64 是 passthrough 包装器, 直接 exec 原生二进制
    cat > "$BIN/box64" << 'BOXWRAP'
#!/bin/sh
# x86_64 passthrough: box64 调用 → 直接运行原生二进制
# C++ 代码调用格式: ./box64 <program> [args...]
# 注意: 不使用 dirname (Wine 隔离环境可能没有)
DIR="${0%/*}"
[ "$DIR" = "$0" ] && DIR="."
[ $# -lt 1 ] && { echo "Usage: $0 <program> [args...]" >&2; exit 1; }
prog="$1"
shift
exec "$DIR/$prog" "$@"
BOXWRAP
    chmod +x "$BIN/box64"
    log "  box64 → bin/ (x86_64 passthrough wrapper)"
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

# ---- x86_64-windows/ (PE DLL + .drv + .exe + .sys) ----
mkdir -p "$BIN/x86_64-windows"
for dll in "$WINE_SRC/build-native/dlls/"*/x86_64-windows/*.dll; do
    cp "$dll" "$BIN/x86_64-windows/"
done
for drv in "$WINE_SRC/build-native/dlls/"*/x86_64-windows/*.drv; do
    cp "$drv" "$BIN/x86_64-windows/"
done
for exe in "$WINE_SRC/build-native/dlls/"*/x86_64-windows/*.exe; do
    cp "$exe" "$BIN/x86_64-windows/"
done
for sys in "$WINE_SRC/build-native/dlls/"*/x86_64-windows/*.sys; do
    cp "$sys" "$BIN/x86_64-windows/"
done
log "  x86_64-windows: $(ls "$BIN/x86_64-windows" | wc -l) DLL/DRV/EXE/SYS files"

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
    local linker="${3:-}"
    local dest="$HNP_LAYOUT/bin/x86_64-unix"
    if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
        cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
    elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
    else
        warn "$soname 未找到"
        return 0
    fi
    # 创建无版本号别名 (dlopen("libfoo.so") 按名查找需要, 不能用 symlink)
    if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
        cp "$dest/$soname" "$dest/$linker"
    fi
}

pick_lib "libfreetype.so.6.20.2"        "libfreetype.so.6"   "libfreetype.so"
pick_lib "libz.so"                      "libz.so"
pick_lib "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
pick_lib "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
pick_lib "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
pick_lib "libxml2.so.2.12.0"            "libxml2.so.2"
pick_lib "libffi.so.8.1.4"              "libffi.so.8"
log "  交叉编译依赖 → bin/x86_64-unix/"

# libfreetype 需要同时放在 bin/ (Box64 按名 dlopen 搜索路径: .)
cp "$HNP_LAYOUT/bin/x86_64-unix/libfreetype.so.6" "$BIN/"
cp "$HNP_LAYOUT/bin/x86_64-unix/libfreetype.so" "$BIN/"

# Wine 内置字体 (TrueType)
mkdir -p "$HNP_LAYOUT/share/wine/fonts"
cp "$WINE_SRC/fonts/"*.ttf "$HNP_LAYOUT/share/wine/fonts/"
# XKB 键盘布局数据 (Wine 键盘驱动初始化必需, 由 build_xkbconfig.sh 安装到 sysroot-ext)
mkdir -p "$HNP_LAYOUT/share/X11"
if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ]; then
    cp -r "$SYSROOT_EXT_SHARE/X11/xkb" "$HNP_LAYOUT/share/X11/"
    log "  xkb: $(du -sh "$HNP_LAYOUT/share/X11/xkb" | cut -f1)"
else
    warn "xkb 数据未找到 ($SYSROOT_EXT_SHARE/X11/xkb), 请先运行: bash scripts/build_xkbconfig.sh"
fi

log "  fonts: $(ls "$HNP_LAYOUT/share/wine/fonts" | wc -l) .ttf files"
cp "$WINE_SRC/build-native/nls/"*.nls "$HNP_LAYOUT/share/wine/nls/"
mkdir -p "$HNP_LAYOUT/share/wine/winmd"
cp "$WINE_SRC/build-native/include/"*.winmd "$HNP_LAYOUT/share/wine/winmd/"
log "  winmd: $(ls "$HNP_LAYOUT/share/wine/winmd" | wc -l) .winmd files"
cp "$WINE_SRC/build-native/loader/wine.inf" "$HNP_LAYOUT/share/wine/"

# OHOS: 无 fontconfig, Wine 内置字体 glyph metrics 不足.
# 将 Windows 默认字体映射到 HarmonyOS 系统字体.
# 插入到 wine.inf 的 [Fonts] section 末尾.
sed -i '/^\[MCI\]$/i\
;; OHOS font substitutes\
HKLM,%FontSubStr%,"System",,"HarmonyOS Sans"\
HKLM,%FontSubStr%,"Fixedsys",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"MS Sans Serif",,"HarmonyOS Sans"\
HKLM,%FontSubStr%,"Courier",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier New",,"Noto Sans Mono"' "$HNP_LAYOUT/share/wine/wine.inf"

# ---- 启动脚本 ----
# arm64: 用 box64 翻译执行 wine (x86_64 → arm64)
# x86_64: 直接原生执行 wine
if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
    cat > "$BIN/wine.sh" << 'SCRIPT'
#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
export BOX64_LD_LIBRARY_PATH="$DIR:$DIR/x86_64-unix:$DIR/../lib/x86_64"
exec "$DIR/box64" "$DIR/wine" "$@"
SCRIPT
else
    cat > "$BIN/wine.sh" << 'SCRIPT'
#!/bin/sh
DIR="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
export LD_LIBRARY_PATH="$DIR:$DIR/x86_64-unix:$DIR/../lib/x86_64"
exec "$DIR/wine" "$@"
SCRIPT
fi
chmod +x "$BIN/wine.sh"
log "  wine.sh ($NATIVE_ARCH)"

# ---- mmap 测试工具 (终端版) ----
if [ -f "$ROOT/.temp/mmap_test" ]; then
    cp "$ROOT/.temp/mmap_test" "$BIN/"
    log "  mmap_test (终端版) → bin/"
fi

log "HNP 布局组装完成 ($NATIVE_ARCH)"
echo ""
echo "  $BIN/"
echo "  ├── wine, wineserver, box64"
echo "  ├── ntdll.so                  ← wine loader 直接加载"
echo "  ├── *.exe                     ← PE stubs ($(ls "$BIN"/*.exe 2>/dev/null | wc -l) files)"
echo "  ├── x86_64-windows/           ← PE DLL ($(ls "$BIN/x86_64-windows" | wc -l) files)"
echo "  └── x86_64-unix/              ← Unix .so ($(ls "$BIN/x86_64-unix" | wc -l) files)"

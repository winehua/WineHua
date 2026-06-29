#!/bin/bash
# assemble.sh — 组装 HNP 打包临时目录
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# ============================================================
# Pad 模式: 无 HNP — 文件分流到 libs/ + rawfile/
# ============================================================
assemble_pad() {
    log "=== 组装 Pad 布局 ($NATIVE_ARCH, 无 HNP) ==="

    local wine_data="$STAGING_DIR/wine-data"
    rm -rf "$STAGING_DIR"
    rm -rf "$wine_data"
    mkdir -p "$wine_data/bin/x86_64-windows"
    mkdir -p "$wine_data/bin/x86_64-unix"
    mkdir -p "$wine_data/share/wine/nls"
    mkdir -p "$wine_data/share/wine/fonts"
    mkdir -p "$wine_data/share/wine/winmd"
    mkdir -p "$wine_data/share/X11"

    # -- 1. 原生 .so → libs/$NATIVE_ARCH/ (由各 build 脚本完成) --
    mkdir -p "$NATIVE_LIBS"

    if [ "$NATIVE_ARCH" = "x86_64" ]; then
        # x86_64 Pad: Wine .so 是原生架构, 直接放 libs/
        log "  → Wine .so → libs/x86_64/"

        # 所有 Wine Unix .so → libs/x86_64/ (系统 linker 通过文件名搜索)
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            cp "$so" "$NATIVE_LIBS/"
        done
        log "    Wine .so: $(ls "$BUILD_DIR/wine-ohos/dlls/"*/*.so 2>/dev/null | wc -l) files"

        # 交叉编译依赖 → libs/x86_64/
        # (系统 linker 自动搜索此路径, 无需 x86_64-unix 子目录)
        _pick_lib_pad() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$NATIVE_LIBS"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
                cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad "libz.so"                      "libz.so"
        _pick_lib_pad "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"
        _pick_lib_pad "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad "libffi.so.8.1.4"              "libffi.so.8"
        log "    交叉编译依赖 → libs/x86_64/"

        # libc.so → libs/x86_64/
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$NATIVE_LIBS/"

        # libfreetype 已由 _pick_lib_pad 放入 libs/x86_64/，系统 linker 可直接找到

        # libwineserver.so (Pad fork+dlopen 入口)
        if [ -f "$BUILD_DIR/wine_server/libwineserver.so" ]; then
            cp "$BUILD_DIR/wine_server/libwineserver.so" "$NATIVE_LIBS/"
            log "    libwineserver.so → libs/x86_64/"
        else
            warn "libwineserver.so 未找到！请先执行: bash scripts/build_wine.sh"
        fi
    elif [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        # arm64 Pad: Wine .so 是 x86_64, 不放 libs/, 放 rawfile zip
        # box64.so 由 build_box64.sh 放入 NATIVE_LIBS
        log "  → Wine x86_64 .so → rawfile zip"

        # ARM64 原生库 → libs/arm64-v8a/ (Box64 dlopen bridge libraries)
        # Box64 模拟 x86_64 时需要加载 ARM64 原生的 freetype/xkbcommon 等,
        # 系统 linker 搜索 libs/arm64-v8a/
        local aarch64_lib="$SYSROOT_EXT/usr/lib/$NATIVE_TARGET"
        _pick_arm64_native() {
            local soname="$1" linker="${2:-}"
            if [ -f "$aarch64_lib/$soname" ]; then
                cp "$aarch64_lib/$soname" "$NATIVE_LIBS/$soname"
            else
                warn "ARM64 原生库 $soname 未找到, 跳过"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$NATIVE_LIBS/$linker" ]; then
                cp "$aarch64_lib/$soname" "$NATIVE_LIBS/$linker"  # HAP 不支持 symlink, 实体复制
            fi
        }
        # Box64 native bridge libs: soname 文件 + linker 名拷贝
        _pick_arm64_native "libfreetype.so.6"   "libfreetype.so"
        _pick_arm64_native "libxkbcommon.so.0"   "libxkbcommon.so"
        _pick_arm64_native "libxkbregistry.so.0" "libxkbregistry.so"
        _pick_arm64_native "libxml2.so.2"        "libxml2.so"
        _pick_arm64_native "libwayland-client.so.0" "libwayland-client.so"
        _pick_arm64_native "libwayland-server.so.0" "libwayland-server.so"
        _pick_arm64_native "libffi.so.8"         "libffi.so"

        # ntdll.so → rawfile
        cp "$BUILD_DIR/wine-ohos/dlls/ntdll/ntdll.so" "$wine_data/bin/"

        # x86_64-unix/ .so → rawfile
        for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
            [ "$(basename "$so")" = "ntdll.so" ] && continue
            cp "$so" "$wine_data/bin/x86_64-unix/"
        done

        # 交叉编译依赖 → rawfile
        _pick_lib_pad_rf() {
            local name="$1" soname="$2" linker="${3:-}"
            local dest="$wine_data/bin/x86_64-unix"
            if [ -f "$SYSROOT_EXT_LIB/$soname" ]; then
                cp "$SYSROOT_EXT_LIB/$soname" "$dest/$soname"
            elif [ -f "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" ]; then
                cp "$SYSROOT/usr/lib/x86_64-linux-ohos/$name" "$dest/$soname"
            else
                warn "$soname 未找到"
                return 0
            fi
            if [ -n "$linker" ] && [ ! -f "$dest/$linker" ]; then
                cp "$dest/$soname" "$dest/$linker"
            fi
        }
        _pick_lib_pad_rf "libfreetype.so.6.20.2"       "libfreetype.so.6"   "libfreetype.so"
        _pick_lib_pad_rf "libz.so"                      "libz.so"
        _pick_lib_pad_rf "libwayland-client.so.0.22.0"  "libwayland-client.so.0"
        _pick_lib_pad_rf "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"    "libwayland-egl.so"
        _pick_lib_pad_rf "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"
        _pick_lib_pad_rf "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"
        _pick_lib_pad_rf "libxml2.so.2.12.0"            "libxml2.so.2"
        _pick_lib_pad_rf "libffi.so.8.1.4"              "libffi.so.8"

        # libfreetype → bin/ (box64 按名 dlopen 搜索路径: .)
        cp "$wine_data/bin/x86_64-unix/libfreetype.so.6" "$wine_data/bin/"
        cp "$wine_data/bin/x86_64-unix/libfreetype.so" "$wine_data/bin/"

        # libc.so → bin/ (当前目录) + x86_64-unix/ (BOX64_LD_LIBRARY_PATH)
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_data/bin/"
        cp "$SYSROOT/usr/lib/x86_64-linux-ohos/libc.so" "$wine_data/bin/x86_64-unix/"

        # wine + wineserver (x86_64 ELF, 由 box64 加载)
        cp "$BUILD_DIR/wine-ohos/loader/wine" "$wine_data/bin/"
        if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
            cp "$BUILD_DIR/wine_server/wineserver" "$wine_data/bin/"
        elif [ -f "$BUILD_DIR/wine-ohos/server/wineserver" ]; then
            cp "$BUILD_DIR/wine-ohos/server/wineserver" "$wine_data/bin/"
        fi
    fi

    # -- 2. PE DLL + 数据文件 → rawfile (两种架构共用) --
    # x86_64-windows/
    for dll in "$BUILD_DIR/wine-native/dlls/"*/x86_64-windows/*.dll; do
        cp "$dll" "$wine_data/bin/x86_64-windows/"
    done
    for drv in "$BUILD_DIR/wine-native/dlls/"*/x86_64-windows/*.drv; do
        cp "$drv" "$wine_data/bin/x86_64-windows/"
    done
    for exe in "$BUILD_DIR/wine-native/dlls/"*/x86_64-windows/*.exe; do
        cp "$exe" "$wine_data/bin/x86_64-windows/"
    done
    for sys in "$BUILD_DIR/wine-native/dlls/"*/x86_64-windows/*.sys; do
        cp "$sys" "$wine_data/bin/x86_64-windows/"
    done
    log "  x86_64-windows → $(ls "$wine_data/bin/x86_64-windows" | wc -l) files"

    # *.exe stubs → rawfile
    for exe in "$BUILD_DIR/wine-native/programs/"*/x86_64-windows/*.exe; do
        cp "$exe" "$wine_data/bin/"
    done
    # graphics smoke test (OHOS 交叉编译产物, 不在 build-native/)
    if [ -f "$BUILD_DIR/wine-ohos/programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe" ]; then
        cp "$BUILD_DIR/wine-ohos/programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe" "$wine_data/bin/x86_64-windows/"
        log "  winehua_graphics_smoke.exe → x86_64-windows/"
    fi

    # fonts
    cp "$WINE_SRC/fonts/"*.ttf "$wine_data/share/wine/fonts/"
    # NLS
    cp "$BUILD_DIR/wine-native/nls/"*.nls "$wine_data/share/wine/nls/"
    # winmd
    cp "$BUILD_DIR/wine-native/include/"*.winmd "$wine_data/share/wine/winmd/"
    # wine.inf (含 OHOS font substitutes)
    cp "$BUILD_DIR/wine-native/loader/wine.inf" "$wine_data/share/wine/"
    sed -i '/^\[MCI\]$/i\
;; OHOS font substitutes\
HKLM,%FontSubStr%,"System",,"HarmonyOS Sans"\
HKLM,%FontSubStr%,"Fixedsys",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"MS Sans Serif",,"HarmonyOS Sans"\
HKLM,%FontSubStr%,"Courier",,"Noto Sans Mono"\
HKLM,%FontSubStr%,"Courier New",,"Noto Sans Mono"' "$wine_data/share/wine/wine.inf"
    # XKB
    if [ -d "$SYSROOT_EXT_SHARE/X11/xkb" ]; then
        cp -r "$SYSROOT_EXT_SHARE/X11/xkb" "$wine_data/share/X11/"
    fi

    # guest GPU 库 (Mesa/VirGL, 供 GraphicsBroker 注入到 Wine LD_LIBRARY_PATH)
    if [ -d "$BUILD_DIR/guest_gfx/$NATIVE_ARCH/lib" ]; then
        mkdir -p "$wine_data/bin/guest_gfx"
        cp -a "$BUILD_DIR/guest_gfx/$NATIVE_ARCH/"* "$wine_data/bin/guest_gfx/"
        log "  guest_gfx ($NATIVE_ARCH): $(ls "$wine_data/bin/guest_gfx/lib"/*.so* 2>/dev/null | wc -l) .so files"
    else
        log "  guest_gfx: SKIP (build/guest_gfx/$NATIVE_ARCH/lib not found)"
    fi

    # -- 3. 打包 zip → rawfile (不带 wine-data/ 前缀) --
    local rawfile_dir="$WINEHUA/entry/src/main/resources/rawfile"
    mkdir -p "$rawfile_dir"
    local zip_name="wine-data.zip"
    cd "$wine_data"
    rm -f "$STAGING_DIR/$zip_name"
    zip -r "$STAGING_DIR/$zip_name" . -x '*.git*'
    cp "$STAGING_DIR/$zip_name" "$rawfile_dir/"
    log "  $zip_name → rawfile/ ($(du -h "$rawfile_dir/$zip_name" | cut -f1))"

    log "Pad 布局组装完成 ($NATIVE_ARCH)"
    echo ""
    echo "  libs/$NATIVE_ARCH/"
    ls -la "$NATIVE_LIBS/" 2>/dev/null || echo "    (empty)"
    echo "  rawfile/$zip_name"
}

log "=== 组装布局 ($NATIVE_ARCH, $DEVICE_TYPE) ==="

if [ "$DEVICE_TYPE" = "pad" ]; then
    assemble_pad
    exit 0
fi

# ============================================================
# 以下为 PC 模式 HNP 布局 (不变)
# ============================================================

# staging 目录每次重建
rm -rf "$STAGING_DIR"
mkdir -p "$HNP_LAYOUT/bin"
mkdir -p "$HNP_LAYOUT/lib/x86_64"
mkdir -p "$HNP_LAYOUT/share/wine/nls"

BIN="$HNP_LAYOUT/bin"

# ---- 主二进制 ----
cp "$BUILD_DIR/wine-ohos/loader/wine" "$BIN/"

# wineserver: 优先手动编译版 (含 __ANDROID__), 回退 make 版
if [ -f "$BUILD_DIR/wine_server/wineserver" ]; then
    cp "$BUILD_DIR/wine_server/wineserver" "$BIN/"
elif [ -f "$BUILD_DIR/wine-ohos/server/wineserver" ]; then
    cp "$BUILD_DIR/wine-ohos/server/wineserver" "$BIN/"
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
cp "$BUILD_DIR/wine-ohos/dlls/ntdll/ntdll.so" "$BIN/"

# ---- x86_64-unix/ (其他 .so — load_builtin_unixlib 拼接路径) ----
mkdir -p "$BIN/x86_64-unix"
for so in "$BUILD_DIR/wine-ohos/dlls/"*/*.so; do
    [ "$(basename "$so")" = "ntdll.so" ] && continue
    cp "$so" "$BIN/x86_64-unix/"
done
log "  x86_64-unix: $(ls "$BIN/x86_64-unix" | wc -l) .so files"

# ---- x86_64-windows/ (PE DLL + .drv + .exe + .sys) ----
mkdir -p "$BIN/x86_64-windows"
for dll in "$BUILD_DIR/wine-native/dlls/"*/x86_64-windows/*.dll; do
    cp "$dll" "$BIN/x86_64-windows/"
done
for drv in "$BUILD_DIR/wine-native/dlls/"*/x86_64-windows/*.drv; do
    cp "$drv" "$BIN/x86_64-windows/"
done
for exe in "$BUILD_DIR/wine-native/dlls/"*/x86_64-windows/*.exe; do
    cp "$exe" "$BIN/x86_64-windows/"
done
for sys in "$BUILD_DIR/wine-native/dlls/"*/x86_64-windows/*.sys; do
    cp "$sys" "$BIN/x86_64-windows/"
done
log "  x86_64-windows: $(ls "$BIN/x86_64-windows" | wc -l) DLL/DRV/EXE/SYS files"

# ---- *.exe stubs ----
for exe in "$BUILD_DIR/wine-native/programs/"*/x86_64-windows/*.exe; do
    cp "$exe" "$BIN/"
done
log "  *.exe stubs: $(ls "$BIN"/*.exe 2>/dev/null | wc -l) files"

# ---- graphics smoke test (OHOS 交叉编译产物, 不在 build-native/) ----
smoke_src="$BUILD_DIR/wine-ohos/programs/winehua_graphics_smoke/x86_64-windows/winehua_graphics_smoke.exe"
if [ -f "$smoke_src" ]; then
    cp "$smoke_src" "$BIN/x86_64-windows/"
    log "  winehua_graphics_smoke.exe → x86_64-windows/"
fi

# ---- ARM64 原生桥接库 (PC/HNP) + libc + 交叉编译依赖 + NLS + wine.inf + fonts ----
# ARM64 native bridge libs for Box64 in HNP package
if [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
    aarch64_lib="$SYSROOT_EXT/usr/lib/$NATIVE_TARGET"
    mkdir -p "$HNP_LAYOUT/lib/arm64-v8a"
    _pc_pick_arm64_native() {
        local soname="$1" linker="${2:-}"
        [ -f "$aarch64_lib/$soname" ] || { warn "ARM64 native lib $soname not found"; return 0; }
        cp "$aarch64_lib/$soname" "$HNP_LAYOUT/lib/arm64-v8a/$soname"
        [ -n "$linker" ] && [ ! -f "$HNP_LAYOUT/lib/arm64-v8a/$linker" ] && \
            cp "$aarch64_lib/$soname" "$HNP_LAYOUT/lib/arm64-v8a/$linker"
    }
    _pc_pick_arm64_native "libfreetype.so.6"   "libfreetype.so"
    _pc_pick_arm64_native "libxkbcommon.so.0"   "libxkbcommon.so"
    _pc_pick_arm64_native "libxkbregistry.so.0" "libxkbregistry.so"
    _pc_pick_arm64_native "libxml2.so.2"        "libxml2.so"
    _pc_pick_arm64_native "libwayland-client.so.0" "libwayland-client.so"
    _pc_pick_arm64_native "libffi.so.8"         "libffi.so"
    log "  ARM64 native bridge libs -> lib/arm64-v8a/"
fi

# ---- 宿主 VirGL 运行时 (libvirglrenderer.so + virgl_test_server) ----
UNIX_DIR="$BIN/x86_64-unix"
for lib in libepoxy.so.0 libvirglrenderer.so.1; do
    [ -f "$NATIVE_LIBS/$lib" ] && cp "$NATIVE_LIBS/$lib" "$UNIX_DIR/"
done
[ -f "$NATIVE_LIBS/libepoxy.so" ] && cp "$NATIVE_LIBS/libepoxy.so" "$UNIX_DIR/"
[ -f "$NATIVE_LIBS/libvirglrenderer.so" ] && cp "$NATIVE_LIBS/libvirglrenderer.so" "$UNIX_DIR/"
if [ -f "$NATIVE_LIBS/virgl_test_server" ]; then
    cp "$NATIVE_LIBS/virgl_test_server" "$BIN/"
    chmod +x "$BIN/virgl_test_server"
    log "  virgl_test_server → bin/"
else
    warn "virgl_test_server not found in $NATIVE_LIBS; VirGL host transport will stay unavailable"
fi

# ---- guest GPU 库 (Mesa/VirGL, 供 GraphicsBroker 注入到 Wine LD_LIBRARY_PATH) ----
if [ -d "$BUILD_DIR/guest_gfx/$NATIVE_ARCH/lib" ]; then
    mkdir -p "$BIN/guest_gfx"
    cp -a "$BUILD_DIR/guest_gfx/$NATIVE_ARCH/"* "$BIN/guest_gfx/"
    log "  guest_gfx ($NATIVE_ARCH): $(ls "$BIN/guest_gfx/lib"/*.so* 2>/dev/null | wc -l) .so files"
else
    log "  guest_gfx: SKIP (build/guest_gfx/$NATIVE_ARCH/lib not found)"
fi

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
pick_lib "libwayland-client.so.0.22.0"  "libwayland-client.so.0"   "libwayland-client.so"
pick_lib "libwayland-server.so.0.22.0"  "libwayland-server.so.0"   "libwayland-server.so"
pick_lib "libwayland-egl.so.1.22.0"     "libwayland-egl.so.1"      "libwayland-egl.so"
pick_lib "libxkbcommon.so.0.0.0"        "libxkbcommon.so.0"        "libxkbcommon.so"
pick_lib "libxkbregistry.so.0.0.0"      "libxkbregistry.so.0"      "libxkbregistry.so"
pick_lib "libxml2.so.2.12.0"            "libxml2.so.2"             "libxml2.so"
pick_lib "libffi.so.8.1.4"              "libffi.so.8"              "libffi.so"
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
cp "$BUILD_DIR/wine-native/nls/"*.nls "$HNP_LAYOUT/share/wine/nls/"
mkdir -p "$HNP_LAYOUT/share/wine/winmd"
cp "$BUILD_DIR/wine-native/include/"*.winmd "$HNP_LAYOUT/share/wine/winmd/"
log "  winmd: $(ls "$HNP_LAYOUT/share/wine/winmd" | wc -l) .winmd files"
cp "$BUILD_DIR/wine-native/loader/wine.inf" "$HNP_LAYOUT/share/wine/"

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

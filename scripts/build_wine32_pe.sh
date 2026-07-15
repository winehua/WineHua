#!/bin/bash
# build_wine32_pe.sh — Wine 32-bit PE DLL 交叉编译 (i686-mingw32)
#
# 32-bit PE DLL 用于 WoW64: 64-bit Wine 进程加载 32-bit Windows 程序时，
# 需要 32-bit PE DLL (不含 Unix .so, 后者保持 64-bit)。
#
# 不依赖 OHOS 工具链，仅需 mingw-w64 i686 交叉编译器。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

log "=== 构建 Wine 32-bit PE DLL (i686-mingw32) ==="

CROSS_CC="i686-w64-mingw32-gcc"
CROSS_CXX="i686-w64-mingw32-g++"

BUILD_DIR="$ROOT/build/wine-i386-pe"
WINE_TOOLS="$ROOT/build/wine-native"
WINE32_SENTINEL="$BUILD_DIR/dlls/ntdll/i386-windows/ntdll.dll"

# 确保 host tools 存在
if [ ! -f "$WINE_TOOLS/tools/winegcc/winegcc" ]; then
    warn "wine-native tools not found at $WINE_TOOLS, building first..."
    NATIVE_ARCH=x86_64 bash "$SCRIPT_DIR/build_wine.sh"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ "$HOST_OS" = "Darwin" ] && [ -f "Makefile" ]; then
    WAYLAND_SCANNER="$WAYLAND_SCANNER" perl -pi -e \
        's{/usr/local/bin/wayland-scanner}{$ENV{WAYLAND_SCANNER}}g' Makefile config.status
fi

if [ ! -f "Makefile" ]; then
    log "Configuring Wine 32-bit PE..."
    # 照抄 64-bit OHOS 构建 (build_wine.sh:20-30 + 79-83).
    # PE DLL 只需要 stub, 实际协议由 64-bit winewayland.so 处理.
    # 照抄 64-bit OHOS 构建 (build_wine.sh Wayland 交叉编译缓存).
    # PE DLL 只需 stub, 实际协议由 64-bit winewayland.so 处理.
    # 设 cache 变量骗过 configure 的 AC_CHECK_HEADER / AC_CHECK_LIB / WINE_CHECK_SONAME.
    export ac_cv_header_wayland_client_h=yes
    export ac_cv_lib_wayland_client_wl_display_connect=yes
    export ac_cv_lib_soname_wayland_client="libwayland-client.so.0"
    export ac_cv_header_xkbcommon_xkbcommon_h=yes
    export ac_cv_lib_xkbcommon_xkb_context_new=yes
    export ac_cv_lib_soname_xkbcommon="libxkbcommon.so.0"
    export ac_cv_header_xkbcommon_xkbregistry_h=yes
    export ac_cv_lib_xkbregistry_rxkb_context_new=yes
    export ac_cv_lib_soname_xkbregistry="libxkbregistry.so.0"
    export ac_cv_header_ft2build_h=yes
    export ac_cv_lib_soname_freetype="libfreetype.so.6"
    # mingw 无法识别 linux/input.h, 必须设 yes 否则 Wayland 驱动被禁用
    export ac_cv_header_linux_input_h=yes
    # LIBS 仅设简单 -l 字符串 (configure 只测 -z, 不真正链接到 mingw).
    # 不用 PKG_CONFIG_PATH / -L 路径, 避免注入 OHOS clang flags 污染 mingw.
    export FREETYPE_CFLAGS="-I/usr/include/freetype2"
    export FREETYPE_LIBS="-lfreetype"
    export WAYLAND_CLIENT_LIBS="-lwayland-client"
    export XKBCOMMON_LIBS="-lxkbcommon"
    export XKBREGISTRY_LIBS="-lxkbregistry"
    if [ "$HOST_OS" = "Darwin" ]; then
        export ac_cv_path_WAYLAND_SCANNER="$WAYLAND_SCANNER"
    else
        export WAYLAND_SCANNER=/usr/local/bin/wayland-scanner
    fi
    "$WINE_SRC/configure" \
        --host=i686-w64-mingw32 \
        --with-wine-tools="$WINE_TOOLS" \
        --with-mingw=gcc \
        --disable-tests \
        --without-x --without-alsa \
        --without-freetype \
        --without-opengl --without-vulkan \
        CC="$CROSS_CC" \
        CROSSCC="$CROSS_CC" \
        CXX="$CROSS_CXX"
    # configure 可能被 pkg-config 清空, 在 configure 后补回
    sed -i 's/^XKBREGISTRY_LIBS=.$/XKBREGISTRY_LIBS=-lxkbregistry/' "$BUILD_DIR/config.status" 2>/dev/null || true
fi

log "Building 32-bit PE DLLs..."
make -j"$JOBS" \
    CC="$CROSS_CC" \
    CROSSCC="$CROSS_CC"

# 统计产出
PE_COUNT=$(find "$BUILD_DIR/dlls/"*/i386-windows -name '*.dll' 2>/dev/null | wc -l)
DRV_COUNT=$(find "$BUILD_DIR/dlls/"*/i386-windows -name '*.drv' 2>/dev/null | wc -l)
EXE_COUNT=$(find "$BUILD_DIR/programs/"*/i386-windows -name '*.exe' 2>/dev/null | wc -l)
log "32-bit PE 构建完成: dll=${PE_COUNT}, drv=${DRV_COUNT}, exe=${EXE_COUNT}"

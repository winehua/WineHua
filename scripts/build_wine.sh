#!/bin/bash
# build_wine.sh — Wine 交叉编译
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# Wine 编译标志 (Unix .so + wineserver)
WINE_CFLAGS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -D__ANDROID__ -D__OHOS__ -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables"

build_native_tools() {
    log "--- Native 构建 (winegcc 等 host 工具) ---"
    mkdir -p "$BUILD_DIR/wine-native"
    cd "$BUILD_DIR/wine-native"
    if [ ! -f "Makefile" ]; then
        # 用 cache variables 骗过 configure, 避免 host 安装 wayland/xkbcommon/freetype/GL
        # 这些库仅供 winewayland.drv/winex11.drv 等 DLL 编译使用
        # 但我们只编译 tools/ 下的纯 host 工具, 不编任何 DLL, 不需要实际头文件/库
        export ac_cv_header_wayland_client_h=yes
        export ac_cv_lib_wayland_client_wl_display_connect=yes
        export ac_cv_header_xkbcommon_xkbcommon_h=yes
        export ac_cv_lib_xkbcommon_xkb_context_new=yes
        export ac_cv_header_xkbcommon_xkbregistry_h=yes
        export ac_cv_lib_xkbregistry_rxkb_context_new=yes
        export ac_cv_header_ft2build_h=yes
        export ac_cv_lib_soname_freetype="libfreetype.so.6"
        if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
            export FREETYPE_CFLAGS="$("$PKG_CONFIG_BIN" --cflags freetype2)"
            export FREETYPE_LIBS="$("$PKG_CONFIG_BIN" --libs freetype2)"
            "$CONFIGURE_BIN" --srcdir="$WINE_SRC" --enable-archs=x86_64 --disable-tests \
                --without-x --without-alsa --without-opengl --without-vulkan
        else
            export FREETYPE_CFLAGS="-I/usr/include/freetype2"
            export FREETYPE_LIBS="-lfreetype"
            "$CONFIGURE_BIN" --srcdir="$WINE_SRC" --enable-win64 --disable-tests \
                --without-x --without-alsa --without-opengl --without-vulkan
        fi
    fi
    # 只编译 OHOS 交叉构建实际需要的 host 工具 (~44 .o 文件)
    # 不编 DLL (PE/fake-module 和 Unix .so), 砍掉 ~90% 编译时间
    # 也不需要在 host 上安装 wayland/xkbcommon/freetype/GL dev 包
    # 只编译 OHOS 交叉构建实际需要的 host 工具
    # winegcc/winebuild/wrc/widl: 交叉编译 PE DLL
    # wine: 加载器 (locale.nls 等数据文件)
    # makedep/make_xftmpl/wmc: Makefile 依赖/资源生成
    # sfnt2fon: 字体 .fon 生成 (唯一需要 host freetype 的工具)
    make -j$JOBS \
        tools/winegcc/winegcc \
        tools/winebuild/winebuild \
        tools/wrc/wrc \
        tools/widl/widl \
        tools/wine/wine \
        tools/makedep \
        tools/make_xftmpl \
        tools/wmc/wmc \
        tools/sfnt2fon/sfnt2fon

    # 确保 wrc 能加载 locale.nls (翻译资源编译需要)。build_native_tools
    # 只编 host 工具, 不跑生成 nls 数据的 make 规则, 故手动 symlink
    # 源码 nls 到 wine-native/nls/ (wrc 硬编码从 ../nls 找)
    mkdir -p "$BUILD_DIR/wine-native/nls"
    for nlsf in "$WINE_SRC"/nls/*.nls; do
        ln -sf "$nlsf" "$BUILD_DIR/wine-native/nls/$(basename "$nlsf")"
    done
}

build_ohos_unix() {
    log "--- OHOS 交叉编译 (Unix .so, WINE_ARCH=$WINE_ARCH) ---"

    # 构建目录按 WINE_ARCH 隔离: 同工作树切换架构 (方案① x86_64 ↔ 方案③ arm64 原生)
    # 时不复用跨架构 configure 缓存。方案①/② (WINE_ARCH=x86_64) 共享 wine-ohos-x86_64。
    local wine_build_dir="$BUILD_DIR/wine-ohos-$WINE_ARCH"
    mkdir -p "$wine_build_dir"
    cd "$wine_build_dir"

    # 检查是否需要重新 configure。只查 wine 真正生成到 config.h 的 SONAME 宏
    # (freetype/vulkan/gnutls 用 WINE_CHECK_SONAME); wayland/gstreamer 不生成
    # SONAME 宏 (config.h.in 无条目), 旧检查 SONAME_LIBWAYLAND_CLIENT /
    # SONAME_LIBGSTREAMER_1_0 永远为真 → 每次都重配, 已移除。
    # 外加 host 校验 (config.status 的 --host), 防止切换架构后复用旧 host 缓存。
    if [ ! -f "Makefile" ] || ! grep -q '#define SONAME_LIBFREETYPE' include/config.h 2>/dev/null \
       || ! grep -q '#define SONAME_LIBVULKAN "libvulkan.so.1"' include/config.h 2>/dev/null \
       || ! grep -q '#define SONAME_LIBGNUTLS' include/config.h 2>/dev/null \
       || ! grep -q -- "--host=$HOST_TRIPLE" config.status 2>/dev/null; then
        export FREETYPE_CFLAGS="-I$SYSROOT_EXT_INC/freetype2"
        export FREETYPE_LIBS="-L$SYSROOT_EXT_LIB -lfreetype"
        export ac_cv_header_ft2build_h=yes
        export ac_cv_lib_soname_freetype="libfreetype.so.6"
        # Wayland 交叉编译缓存
        export ac_cv_header_wayland_client_h=yes
        export ac_cv_lib_wayland_client_wl_display_connect=yes
        export ac_cv_lib_soname_wayland_client="libwayland-client.so.0"
        export ac_cv_header_xkbcommon_xkbcommon_h=yes
        export ac_cv_lib_xkbcommon_xkb_context_new=yes
        export ac_cv_lib_soname_xkbcommon="libxkbcommon.so.0"
        export ac_cv_header_xkbcommon_xkbregistry_h=yes
        export ac_cv_lib_soname_xkbregistry="libxkbregistry.so.0"
        # The x86_64 Guest Vulkan Loader is assembled separately from the OHOS
        # sysroot. Wine only dlopens it at runtime, so provide the canonical
        # soname explicitly instead of linking the cross build against it.
        export ac_cv_lib_soname_vulkan="libvulkan.so.1"
        # GnuTLS 交叉编译缓存 (schannel TLS 后端, 由 build_gnutls.sh 编入 sysroot-ext)
        export GNUTLS_CFLAGS="-I$SYSROOT_EXT_INC"
        export GNUTLS_LIBS="-L$SYSROOT_EXT_LIB -lgnutls"
        export ac_cv_header_gnutls_gnutls_h=yes
        export ac_cv_lib_soname_gnutls="libgnutls.so.30"
        # GStreamer 交叉编译缓存 (winegstreamer 后端, 由 build_gstreamer.sh 编入 sysroot-ext)
        # configure 探测 gstreamer-1.0/video/audio/tag 4 个 .pc (PKG_CONFIG_PATH 已含)。
        # 注意: 不设 GSTREAMER_CFLAGS/LIBS env — autoconf 惯例 env 优先于 pkg-config
        # 探测, 设了会覆盖 4 包合并的完整链接列表 (winegstreamer 链接缺 glib 符号)
        export ac_cv_header_gst_gst_h=yes
        export ac_cv_lib_gstreamer_1_0_gst_pad_new=yes
        export ac_cv_lib_soname_gstreamer_1_0="libgstreamer-1.0.so.0"
        export WAYLAND_CLIENT_CFLAGS="-I$SYSROOT_EXT_INC"
        export WAYLAND_CLIENT_LIBS="-L$SYSROOT_EXT_LIB -lwayland-client"
        export XKBCOMMON_CFLAGS="-I$SYSROOT_EXT_INC"
        export XKBCOMMON_LIBS="-L$SYSROOT_EXT_LIB -lxkbcommon"
        export XKBREGISTRY_CFLAGS="-I$SYSROOT_EXT_INC"
        export XKBREGISTRY_LIBS="-L$SYSROOT_EXT_LIB -lxkbregistry"
        local pkg_config=/usr/bin/pkg-config
        if [ "$HOST_OS" = "Darwin" ] || [ "$HOST_OS" = "HarmonyOS" ]; then
            local guest_gfx_prefix="$BUILD_DIR/guest_gfx_install/x86_64"
            export EGL_CFLAGS="-I$guest_gfx_prefix/include"
            export EGL_LIBS="-L$guest_gfx_prefix/lib -lEGL"
            export ac_cv_lib_soname_EGL="libEGL.so.1"
            export WAYLAND_EGL_CFLAGS="-I$SYSROOT_EXT_INC"
            export WAYLAND_EGL_LIBS="-L$SYSROOT_EXT_LIB -lwayland-egl"
            pkg_config="$PKG_CONFIG_BIN"
        fi
        if [ "$HOST_OS" = "HarmonyOS" ]; then
            ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
            GUEST_GFX_ROOT="${WINEHUA_GUEST_GFX_INSTALL_ROOT:-$ROOT/build/guest_gfx_install/x86_64}"
            export CROSSCFLAGS="-I$GUEST_GFX_ROOT/include"

            MINGW_CC="$LLVM_MINGW/bin/clang"
        else
            MINGW_CC="gcc"
        fi

        if [ "$WINE_ARCH" = "aarch64" ]; then
            # arm64 原生 wine: aarch64 Unix 层 + arm64ec/aarch64/i386 PE (FEX 转译 x64 应用)
            export PATH="$LLVM_MINGW/bin:$PATH"
            CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
            CXX="$OHOS_SDK/native/llvm/bin/clang++ --target=$TARGET --sysroot=$SYSROOT" \
            CFLAGS="${WINE_CFLAGS:-} -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2" \
            LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB" \
            PKG_CONFIG="$pkg_config" \
            PKG_CONFIG_PATH="$SYSROOT_EXT_PC" \
            "$CONFIGURE_BIN" --srcdir="$WINE_SRC" \
                --host="$HOST_TRIPLE" \
                --enable-archs=arm64ec,aarch64,i386 \
                --prefix=/opt/winehua \
                --libdir='${prefix}' \
                --with-wine-tools="$BUILD_DIR/wine-native" \
                --with-mingw="$LLVM_MINGW/bin/clang" \
                --disable-tests \
                --without-x --without-alsa \
                --with-opengl --with-vulkan
        else
            CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
            CFLAGS="${WINE_CFLAGS:-} -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2" \
            LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB" \
            PKG_CONFIG="$pkg_config" \
            PKG_CONFIG_PATH="$SYSROOT_EXT_PC" \
            "$CONFIGURE_BIN" --srcdir="$WINE_SRC" \
                --host="$HOST_TRIPLE" \
                --enable-archs=i386,x86_64 \
                --prefix=/opt/winehua \
                --libdir='${prefix}' \
                --with-wine-tools="$BUILD_DIR/wine-native" \
                --with-mingw="$MINGW_CC" \
                --disable-tests \
                --without-x --without-alsa \
                --with-opengl --with-vulkan
        fi
    fi

    # arm64 用 llvm-mingw clang: aarch64-windows target 的默认 include 路径不含
    # generic-w64-mingw32 的 GL/gl.h (x86_64 用 GNU mingw gcc 自带 GL 头)。
    # winehua_graphics_smoke 需要 <GL/gl.h> → 从 llvm-mingw 复制到 build 树
    # include/GL/ (PE 编译命令含 -Iinclude)。GL/gl.h 仅依赖 windows.h/stddef.h,
    # 与 wine 的 -Iinclude/-Iinclude/msvcrt 兼容。
    if [ "$WINE_ARCH" = "aarch64" ]; then
        mkdir -p include/GL
        cp -f "$LLVM_MINGW/generic-w64-mingw32/include/GL/gl.h" include/GL/gl.h
    fi

    make -j$JOBS \
        CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" \
        CXX="$OHOS_SDK/native/llvm/bin/clang++ --target=$TARGET --sysroot=$SYSROOT" \
        CFLAGS="$WINE_CFLAGS -I$SYSROOT_EXT_INC -I$SYSROOT_EXT_INC/freetype2" \
        LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET -L$SYSROOT_EXT_LIB"

    # 验证关键 .so 已成功链接（make -k 可能静默跳过链接失败）
    for pair in "winewayland.drv/winewayland.so" "wineohos.drv/wineohos.so" \
                "win32u/win32u.so" "ntdll/ntdll.so"; do
        if [ ! -f "dlls/$pair" ]; then
            warn "关键 .so 缺失: dlls/$pair (链接可能失败，检查 sysroot-ext)"
        fi
    done
}

build_wineserver() {
    log "--- 编译 wineserver (含 OHOS 修复) ---"
    # 目录按 WINE_ARCH 隔离 (方案①/② 共用 wine_server-x86_64, 产物形式不同 → 见 pie_mode)
    local out="$BUILD_DIR/wine_server-$WINE_ARCH"
    # 数据文件在应用 sandbox 内
    local bindir="$WINE_DEVICE_ROOT/bin"
    local datadir="$WINE_DEVICE_ROOT/share"
    local wine_include="-I$WINE_SRC/include -I$WINE_SRC/include/wine -I$WINE_SRC/server -I$BUILD_DIR/wine-ohos-$WINE_ARCH/include"
    # 目标架构 = WINE_ARCH 的 TARGET (wineserver 与 wine 同架构)。
    # 产物形式: box64+wine 方案 (arm64 设备 + x86_64 wine) → x86_64 PIE 可执行 (box64 转译);
    # 其余 (方案① x86_64 原生 / 方案③ arm64 原生) → native libwineserver.so (dlopen)。
    local srv_target="$TARGET"
    local pie_mode=0
    if [ "$WINE_ARCH" = "x86_64" ] && [ "$NATIVE_ARCH" = "arm64-v8a" ]; then
        pie_mode=1
    fi
    local srv_cflags="--target=$srv_target --sysroot=$SYSROOT -D__MUSL__ -D__ANDROID__ -D__OHOS__ -D_GNU_SOURCE \
        -DWINE_UNIX_LIB -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
        -DBINDIR=\"$bindir\" -DDATADIR=\"$datadir\" \
        -fPIC $wine_include"

    mkdir -p "$out"
    local need_rebuild=0
    local target_binary="$out/libwineserver.so"
    [ "$pie_mode" = "1" ] && target_binary="$out/wineserver"
    if [ ! -f "$target_binary" ]; then
        need_rebuild=1
    else
        for f in $WINE_SRC/server/*.c; do
            [ "$f" -nt "$target_binary" ] && { need_rebuild=1; break; }
        done
    fi
    if [ $need_rebuild -eq 0 ]; then
        # 确保 libwineserver.so 已复制到 NATIVE_LIBS
        # (目录可能被其他架构构建清掉, 需重建 — 见 entry/libs/x86_64 缺失 bug)
        if [ -f "$out/libwineserver.so" ] && [ ! -f "$NATIVE_LIBS/libwineserver.so" ]; then
            mkdir -p "$NATIVE_LIBS"
            cp "$out/libwineserver.so" "$NATIVE_LIBS/"
        fi
        return
    fi
    for f in $WINE_SRC/server/*.c; do
        $CLANG $srv_cflags -c -o "$out/$(basename "$f" .c).o" "$f"
    done

    # musl_compat.c 已在 WINE_SRC/server/ 中, 遍历编译时已打包

    if [ "$pie_mode" = "1" ]; then
        # box64+wine: x86_64 PIE 可执行, box64 转译加载
        log "  wineserver → x86_64 PIE ELF (box64 转译, arm64 设备)"
        $CLANG --target=$srv_target --sysroot=$SYSROOT -fuse-ld=lld -pie \
            -o "$out/wineserver" "$out"/*.o -lm
        log "  → $out/wineserver"
    else
        # 原生: 编译为共享库 (dlopen 加载), 目标 = wine 架构
        log "  wineserver → libwineserver.so ($srv_target)"
        $CLANG --target=$srv_target --sysroot=$SYSROOT -fuse-ld=lld \
            -shared -Wl,-soname,libwineserver.so \
            -o "$out/libwineserver.so" "$out"/*.o -lm
        mkdir -p "$NATIVE_LIBS"
        cp "$out/libwineserver.so" "$NATIVE_LIBS/"
        log "  → $NATIVE_LIBS/libwineserver.so"
    fi
}

# ---- main ----
log "=== 构建 Wine ==="

# 检查 gettext 工具 (msgfmt)。缺失时 wine configure 会禁用 po 翻译,
# 产物 PE 资源只有英文 (中文/多语言 UI 依赖 msgfmt 编翻译语言块)
if ! command -v msgfmt >/dev/null 2>&1; then
    log "ERROR: msgfmt (gettext) 未安装, wine 翻译资源不会编译"
    log "  请安装: apt-get install -y gettext  或  brew install gettext"
    exit 1
fi

# 从 configure.ac 重新生成 configure 到构建目录 (不污染源码树)
# 我们的 configure.ac 新增了 wineohos.drv 等模块的 WINE_CONFIG_MAKEFILE
CONFIGURE_BIN="$BUILD_DIR/configure"
if [ ! -x "$CONFIGURE_BIN" ] || [ "$WINE_SRC/configure.ac" -nt "$CONFIGURE_BIN" ]; then
    log "--- 重新生成 configure (autoconf) ---"
    (cd "$BUILD_DIR" && autoconf -I "$WINE_SRC" -o "$CONFIGURE_BIN" "$WINE_SRC/configure.ac")
    chmod +x "$CONFIGURE_BIN"
fi

build_native_tools
build_ohos_unix
build_wineserver

log "Wine 构建完成"

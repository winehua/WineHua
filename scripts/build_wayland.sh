#!/bin/bash
# build_wayland.sh — Wayland + wayland-protocols 交叉编译 → sysroot-ext
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

WL_SRC="$ROOT/thirdparty/wayland"
WP_SRC="$ROOT/thirdparty/wayland-protocols"
WL_BUILD="$BUILD_DIR/wayland_build"

# 确保 native wayland-scanner 可用
SCANNER="$WAYLAND_SCANNER"
build_scanner() {
    if [ -x "$SCANNER" ]; then return 0; fi
    log "--- 编译 wayland-scanner (native) ---"
    # 装到项目内 build/host-tools，与 env.sh 的 WAYLAND_SCANNER 默认值一致；不写 /usr/local，无需 root
    local host_build="$BUILD_DIR/wayland_native"
    local host_prefix="$BUILD_DIR/host-tools"
    mkdir -p "$host_build" "$host_prefix"
    meson setup "$host_build" "$WL_SRC" \
        --prefix "$host_prefix" \
        --libdir lib \
        -Dlibraries=false -Dscanner=true -Ddtd_validation=false \
        -Ddocumentation=false -Dtests=false --buildtype=release
    ninja -C "$host_build"
    ninja -C "$host_build" install

    # 安装时的 strip 操作破坏了 OHOS SDK clang 的自动签名，所以在 HarmonyOS 上需要重新签名才能运行
    if [ "$HOST_OS" = "HarmonyOS" ]; then
        "$SCRIPT_DIR/ohos-sign-elf.py" "$host_prefix"
    fi
    log "wayland-scanner: $SCANNER"
}

log "=== 构建 Wayland (x86_64) ==="

if [ -f "$SYSROOT_EXT_LIB/libwayland-client.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libwayland-server.so.0" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-client.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-server.h" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-client.pc" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-server.pc" ]; then
    log "Wayland 已就绪，跳过"
    exit 0
fi

build_scanner

# 项目内 host-tools 前缀加入构建期 pkg-config 搜索路径（wayland-scanner 的 native 依赖经此解析）
export PKG_CONFIG_PATH="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH_FOR_BUILD="$BUILD_DIR/host-tools/lib/pkgconfig${PKG_CONFIG_PATH_FOR_BUILD:+:$PKG_CONFIG_PATH_FOR_BUILD}"

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$SYSROOT_EXT_SHARE"
mkdir -p "$WL_BUILD"

# 1. 交叉编译 wayland (client + egl)
meson_build "$WL_BUILD/x86_64" "$WL_SRC" \
    -Ddocumentation=false -Dtests=false -Dscanner=false
ninja -C "$WL_BUILD/x86_64"

# 版本号取自源码 meson.build（.pc 的 Version 与产物尾缀随 tag 走，不写死）
WL_VERSION=$(sed -n "s/^ *version *: *'\([^']*\)'.*/\1/p" "$WL_SRC/meson.build" | head -1)
[ -n "$WL_VERSION" ] || err "无法从 $WL_SRC/meson.build 解析 version"

# 安装 .so (文件名 = SONAME; 版本尾缀随 tag 变, glob 取实际产物, 取不到即失败)
install_soname() {  # $1=产物 glob  $2=目标 SONAME 名
    local src
    src=$(compgen -G "$1" | head -1 || true)
    [ -n "$src" ] || err "未找到产物: $1"
    cp "$src" "$SYSROOT_EXT_LIB/$2"
    log "installed $2 ← $(basename "$src")"
}
install_soname "$WL_BUILD/x86_64/src/libwayland-client.so.0.*" "libwayland-client.so.0"
install_soname "$WL_BUILD/x86_64/src/libwayland-server.so.0.*" "libwayland-server.so.0"
install_soname "$WL_BUILD/x86_64/egl/libwayland-egl.so.1.*"    "libwayland-egl.so.1"
ln -sf libwayland-client.so.0 "$SYSROOT_EXT_LIB/libwayland-client.so"
ln -sf libwayland-server.so.0 "$SYSROOT_EXT_LIB/libwayland-server.so"
ln -sf libwayland-egl.so.1    "$SYSROOT_EXT_LIB/libwayland-egl.so"

# 头文件
cp "$WL_SRC/src/wayland-client.h" \
   "$WL_SRC/src/wayland-client-core.h" \
   "$WL_SRC/src/wayland-util.h" \
   "$WL_BUILD/x86_64/src/wayland-client-protocol.h" \
   "$WL_BUILD/x86_64/src/wayland-version.h" \
   "$SYSROOT_EXT_INC/"
# server 头 (wlroots 消费; R-REPRO: 固化于此, 不再依赖 guest-gfx 伪造路径)
cp "$WL_SRC/src/wayland-server.h" \
   "$WL_SRC/src/wayland-server-core.h" \
   "$WL_BUILD/x86_64/src/wayland-server-protocol.h" \
   "$SYSROOT_EXT_INC/"
cp "$WL_SRC/egl/wayland-egl.h" "$SYSROOT_EXT_INC/" 2>/dev/null || true
cp "$WL_SRC/egl/wayland-egl-core.h" "$SYSROOT_EXT_INC/" 2>/dev/null || true

# 2. wayland-protocols
meson_build "$WL_BUILD/protocols" "$WP_SRC" \
    -Dtests=false
ninja -C "$WL_BUILD/protocols"

# 安装协议 XML 到 sysroot-ext
mkdir -p "$SYSROOT_EXT_SHARE/wayland-protocols/stable/xdg-shell" \
         "$SYSROOT_EXT_SHARE/wayland"
cp "$WP_SRC/stable/xdg-shell/xdg-shell.xml" "$SYSROOT_EXT_SHARE/wayland-protocols/stable/xdg-shell/"
cp "$WL_SRC/protocol/wayland.xml" "$SYSROOT_EXT_SHARE/wayland/"

# .pc 文件
cat > "$SYSROOT_EXT_PC/wayland-client.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos

Name: Wayland Client
Description: Wayland client side library
Version: $WL_VERSION
Requires.private: libffi
Libs: -L\${libdir} -lwayland-client
Cflags: -I\${includedir}
EOF

cat > "$SYSROOT_EXT_PC/wayland-server.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos

Name: Wayland Server
Description: Wayland server side library
Version: $WL_VERSION
Requires.private: libffi
Libs: -L\${libdir} -lwayland-server
Cflags: -I\${includedir}
EOF

cat > "$SYSROOT_EXT_PC/wayland-egl.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/x86_64-linux-ohos

Name: Wayland EGL
Description: Wayland EGL platform library
Version: $WL_VERSION
Libs: -L\${libdir} -lwayland-egl
Cflags: -I\${includedir}
EOF

cat > "$SYSROOT_EXT_PC/wayland-protocols.pc" << EOF
prefix=$SYSROOT_EXT/usr
datarootdir=\${prefix}/share
pkgdatadir=\${datarootdir}/wayland-protocols
Name: Wayland Protocols
Description: Wayland protocol files
Version: 1.32
EOF

# ---- 验收断言: 版本门槛（wlroots 0.20.2 要 ≥1.24, xserver 主线要 client ≥1.26）----
# 踩坑记录: 此前 wayland-server.pc 靠 build_ohos_guest_gfx.sh 的伪造路径装入
# (spec R-REPRO), 版本 1.22 时 wlroots 无法构建; 本断言防回退。
WLV=$(PKG_CONFIG_LIBDIR="$SYSROOT_EXT_PC" pkg-config --modversion wayland-server)
[ -n "$WLV" ] || err "wayland-server.pc 缺失或不可解析"
case "$WLV" in
    1.2[6-9]*|1.[3-9]*|2.*) log "wayland-server=$WLV (≥1.26 OK)";;
    *) err "wayland 版本不达标: $WLV (需 ≥1.26)";;
esac

log "Wayland → sysroot-ext (version=$WL_VERSION)"

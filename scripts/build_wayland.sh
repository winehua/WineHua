#!/bin/bash
# build_wayland.sh — Wayland + wayland-protocols 交叉编译 → sysroot-ext
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

WL_SRC="$ROOT/thirdparty/wayland"
WP_SRC="$ROOT/thirdparty/wayland-protocols"

# 版本取自源码 meson.build。构建目录按版本隔离：升级 tag 后旧 build 目录的 meson
# 缓存会用旧 scanner/旧配置 regenerate（实测: 1.22→1.26 时缓存 scanner 1.22 不满足
# 1.26 门槛直接失败），必须换目录，不能复用。
WL_VERSION=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$WL_SRC/meson.build" | head -1)
[ -n "$WL_VERSION" ] || err "无法从 $WL_SRC/meson.build 解析 version"
WL_BUILD="$BUILD_DIR/wayland_build_$WL_VERSION"

# 确保 native wayland-scanner 可用且与源码同版本（1.26 源码要求 scanner ≥1.26）
SCANNER="$WAYLAND_SCANNER"
build_scanner() {
    if [ -x "$SCANNER" ] && "$SCANNER" --version 2>/dev/null | grep -q "$WL_VERSION"; then return 0; fi
    log "--- 编译 wayland-scanner (native, $WL_VERSION) ---"
    # 装到项目内 build/host-tools，与 env.sh 的 WAYLAND_SCANNER 默认值一致；不写 /usr/local，无需 root
    local host_build="$BUILD_DIR/wayland_native_$WL_VERSION"
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

log "=== 构建 Wayland (x86_64, $WL_VERSION) ==="

# wayland-protocols 版本同样取自源码（卫语句校验已装版本与源码一致, 防 bump 后被跳过;
# 版本查询用 env.sh 顶层 PKG_CONFIG_LIBDIR——share/pkgconfig 下的数据包 pc 不在 SYSROOT_EXT_PC）
WP_VERSION=$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([^']*\)'.*/\1/p" "$WP_SRC/meson.build" | head -1)
[ -n "$WP_VERSION" ] || err "无法从 $WP_SRC/meson.build 解析 version"
WP_INSTALLED=$(pkg-config --modversion wayland-protocols 2>/dev/null || echo none)

if [ -f "$SYSROOT_EXT_LIB/libwayland-client.so.0" ] \
   && [ -f "$SYSROOT_EXT_LIB/libwayland-server.so.0" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-client.h" ] \
   && [ -f "$SYSROOT_EXT_INC/wayland-server.h" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-client.pc" ] \
   && [ -f "$SYSROOT_EXT_PC/wayland-server.pc" ] \
   && [ "$WP_INSTALLED" = "$WP_VERSION" ]; then
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

# 版本已在上文从源码解析（.pc 的 Version 与产物尾缀随 tag 走，不写死）

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

# 2. wayland-protocols（整树 XML 安装: wlroots 经 wl_protocol_dir 引用
#    staging/xwayland-shell 等任意协议, 单拷 xdg-shell 不够; .pc 由安装自带, 不手写）
meson_build "$WL_BUILD/protocols_$WP_VERSION" "$WP_SRC" \
    --prefix="$SYSROOT_EXT/usr" --libdir=lib/x86_64-linux-ohos \
    -Dtests=false
ninja -C "$WL_BUILD/protocols_$WP_VERSION" install
# 清除历史手写 pc 残留 (旧位置 usr/lib/pkgconfig 会在搜索序里遮蔽新装的 1.49, 实测踩坑)
rm -f "$SYSROOT_EXT_PC/wayland-protocols.pc"

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

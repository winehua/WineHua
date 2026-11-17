#!/bin/bash
# build_x11_client.sh — libX11 + libXext (按侧参数化)
# 用法: SIDE=host bash scripts/build_x11_client.sh   # M0: host 侧 (mini X client)
#       SIDE=guest bash scripts/build_x11_client.sh  # M1: guest 侧 (winex11.drv 硬依赖面)
# libX11 tarball 自带 xtrans; keysymdef 数据来自 xorgproto (build_xcb_stack.sh 已装)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"
source "$SCRIPT_DIR/download_verified_source.sh"

SIDE="${SIDE:-host}"
case "$SIDE" in
    host)
        T="$NATIVE_TARGET"; PRE="$HOST_EXT_USR"; INC="$HOST_EXT_INC"
        LIB="$HOST_EXT_LIB"; PC="$HOST_EXT_PC"; SPC="$HOST_EXT_SHARE/pkgconfig"
        ;;
    guest)
        T="$GUEST_TARGET"; PRE="$SYSROOT_EXT/usr"; INC="$SYSROOT_EXT_INC"
        # guest 代码库统一装 multiarch (同 build_xcb_stack.sh 注释)
        LIB="$SYSROOT_EXT_LIB"; PC="$SYSROOT_EXT_PC"; SPC="$SYSROOT_EXT_SHARE/pkgconfig"
        ;;
    *) err "SIDE 必须是 host|guest" ;;
esac
# 搜索面含 $LIB/pkgconfig (理由同 build_xcb_stack.sh: .pc 跟 libdir 走)
export PKG_CONFIG_PATH="$PC:$LIB/pkgconfig:$SPC${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_LIBDIR="$PC:$LIB/pkgconfig:$SPC:$SYSROOT/usr/lib/pkgconfig"

# .pc 就位检查 (两个历史落点: $PC = usr/lib/pkgconfig, $LIB/pkgconfig)
have_pc() {
    { [ -f "$PC/$1.pc" ] || [ -f "$LIB/pkgconfig/$1.pc" ]; } || return 1
    [ -z "${2:-}" ] || "$PKG_CONFIG_BIN" --exact-version="$2" "$1"
}

# CC wrapper: 真实目标固定在编译器层 (同 build_xcb_stack.sh 教训: libtool 交叉模式
# 会吞 --target 且塞 -lgcc_s; --host 谎言值 x86_64-linux-gnu 哄 config.sub + 同机 libtool)
CCWRAP="$BUILD_DIR/ccwrap-$T.sh"
cat > "$CCWRAP" << WEOF
#!/bin/sh
exec "$OHOS_SDK/native/llvm/bin/clang" --target=$T --sysroot=$SYSROOT "\$@"
WEOF
chmod +x "$CCWRAP"

DL="$BUILD_DIR/downloads"
mkdir -p "$DL" "$PRE" "$INC" "$LIB" "$PC" "$SPC"

# 解包按目标隔离 ($BUILD_DIR/x11/$SIDE/$T/<pkg>): host 与 guest pass 严禁共享
# configure/build 树 —— 见 build_xcb_stack.sh 同名注释 (M1-T4 实测踩坑:
# 共享目录下 guest 复用 host 的对象缓存, make 重链 arm64 .so 装进 guest 树)。
fetch_and_unpack() {
    local tgz="$DL/$(basename "$1")"
    download_verified_source "$1" "$2" "$tgz"
    echo "$2  $tgz" | sha256sum -c - || { err "sha256 不匹配: $tgz"; }
    mkdir -p "$BUILD_DIR/x11/$SIDE/$T"
    [ -d "$BUILD_DIR/x11/$SIDE/$T/$3" ] || tar -C "$BUILD_DIR/x11/$SIDE/$T" -xf "$tgz"
}

build_autotools() {  # $1=源码目录
    # --enable-malloc0returnsnull=yes: libX11 的 malloc(0) 运行检查交叉编译时无法执行 (实测踩坑);
    # 该值是 AC_ARG_ENABLE 选项, env 前缀会被 option 解析的 auto 默认值覆盖, 必须走命令行
    # --libdir 必须显式 (同 build_xcb_stack.sh: 默认 $prefix/lib 会装错到根目录)
    ( cd "$1" && ./configure --host=x86_64-linux-gnu --prefix="$PRE" --libdir="$LIB" \
        CC="$CCWRAP" \
        CFLAGS="-I$INC" \
        LDFLAGS="-fuse-ld=lld -L$LIB -L$SYSROOT/usr/lib/$T" \
        --enable-malloc0returnsnull=yes \
        --disable-static --enable-shared --disable-silent-rules )
    make -C "$1" -j"$JOBS" && make -C "$1" install
}

# ── 0. xtrans (libX11 configure 硬依赖 xtrans.pc; 纯头文件包) ──
# WineHua 补丁 (scripts/patches/xtrans-abstract-client.patch): xtrans 客户端
# local 传输走 abstract 名 (沙箱无 /tmp)。消费链核实 (2026-10-13): libX11 内
# 的引用点全部在 XIM (src/xlibi18n/xim_trans.c 与 modules/im/ximcp/, 经
# `#include <X11/Xtrans/transport.c>` → 再 include 本包装进 $PRE/include 的
# Xtranssock.c; 装后副本带 "WineHua (M0-T9)" 标记, 下方有断言) —— 补丁经此
# 随 libX11 生效, 服务 XIM 传输层 (缺它 XIM 客户端连接走 fs 路径)。
# 勘误 (2026-10-13 复核): 核心 XOpenDisplay 连接**不走** xtrans —— libX11 1.8
# 连接在 libxcb (src/OpenDis.c 调 _XConnectXCB; libX11.so 直接 NEEDED
# libxcb.so.1), libxcb 1.17 `_xcb_open` 原生先试 abstract 名 (两侧 config.h
# 均 HAVE_ABSTRACT_SOCKETS=1)。原「不打则 XOpenDisplay 永远 ENOENT (M0-T9)」
# 归因不实: 当时「无 /tmp」能连通的实际链条是 wlroots 侧改绑标准 abstract 名
# + 客户端 libxcb 原生先试 abstract; 别按旧归因理解或"修正"本补丁。
XTRANS_SRC="$BUILD_DIR/x11/$SIDE/$T/xtrans-1.5.2"
if [ ! -f "$SPC/xtrans.pc" ] || ! grep -q "WineHua (M0-T9)" "$XTRANS_SRC/Xtranssock.c" 2>/dev/null; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/xtrans-1.5.2.tar.xz" \
        "5c5cbfe34764a9131d048f03c31c19e57fb4c682d67713eab6a65541b4dff86c" "xtrans-1.5.2"
    if ! grep -q "WineHua (M0-T9)" "$XTRANS_SRC/Xtranssock.c"; then
        patch -d "$XTRANS_SRC" -p1 --forward < "$SCRIPT_DIR/patches/xtrans-abstract-client.patch" \
            || err "xtrans abstract-client 补丁应用失败"
    fi
    build_autotools "$XTRANS_SRC"
fi
[ -f "$SPC/xtrans.pc" ] || err "xtrans 未就绪"
grep -q "WineHua (M0-T9)" "$PRE/include/X11/Xtrans/Xtranssock.c" \
    || err "安装后的 Xtranssock.c 缺 abstract-client 补丁"

# ── 1. libX11 (≥1.8; 关 udc/xlocale 收窄) ──
if ! have_pc x11 1.8.10; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libX11-1.8.10.tar.xz" \
        "2b3b3dad9347db41dca56beb7db5878f283bde1142f04d9f8e478af435dfdc53" "libX11-1.8.10"
    build_autotools "$BUILD_DIR/x11/$SIDE/$T/libX11-1.8.10"
fi
have_pc x11 1.8.10 || err "libX11 未就绪"

# ── 2. libXext ──
if ! have_pc xext 1.3.6; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libXext-1.3.6.tar.xz" \
        "edb59fa23994e405fdc5b400afdf5820ae6160b94f35e3dc3da4457a16e89753" "libXext-1.3.6"
    build_autotools "$BUILD_DIR/x11/$SIDE/$T/libXext-1.3.6"
fi
have_pc xext 1.3.6 || err "libXext 未就绪"

# ── 侧别架构断言 ──
want=AArch64
[ "$T" = "x86_64-linux-ohos" ] && want=Advanced
for so in libX11.so.6 libXext.so.6; do
    m=$("$OHOS_SDK/native/llvm/bin/llvm-readelf" -h "$LIB/$so" | awk '/Machine:/{print $2}')
    [ "$m" = "$want" ] || err "架构侧别错误 (期望 $want): $LIB/$so → $m"
done

# Wine loads these optional extensions dynamically. Omitting their headers at
# configure time silently disables raw input, display modes and cursor support.
while read -r package pc soname checksum; do
    [ -n "$package" ] || continue
    if ! have_pc "$pc" "${package##*-}"; then
        fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/$package.tar.xz" "$checksum" "$package"
        build_autotools "$BUILD_DIR/x11/$SIDE/$T/$package"
    fi
    have_pc "$pc" "${package##*-}" || err "wrong $SIDE X11 extension version: $pc"
    [ -f "$LIB/$soname" ] || err "missing $SIDE X11 extension: $soname"
    m=$("$OHOS_SDK/native/llvm/bin/llvm-readelf" -h "$LIB/$soname" | awk '/Machine:/{print $2}')
    [ "$m" = "$want" ] || err "wrong $SIDE X11 extension architecture: $soname ($m)"
done <<'X11_EXTENSIONS'
libXrender-0.9.12 xrender libXrender.so.1 b832128da48b39c8d608224481743403ad1691bf4e554e4be9c174df171d1b97
libXfixes-6.0.2 xfixes libXfixes.so.3 39f115d72d9c5f8111e4684164d3d68cc1fd21f9b27ff2401b08fddfc0f409ba
libXi-1.8.2 xi libXi.so.6 d0e0555e53d6e2114eabfa44226ba162d2708501a25e18d99cfb35c094c6c104
libXrandr-1.5.4 xrandr libXrandr.so.2 1ad5b065375f4a85915aa60611cc6407c060492a214d7f9daf214be752c3b4d3
libXinerama-1.1.5 xinerama libXinerama.so.1 5094d1f0fcc1828cb1696d0d39d9e866ae32520c54d01f618f1a3c1e30c2085c
libXcursor-1.2.3 xcursor libXcursor.so.1 fde9402dd4cfe79da71e2d96bb980afc5e6ff4f8a7d74c159e1966afb2b2c2c0
libXcomposite-0.4.6 xcomposite libXcomposite.so.1 fe40bcf0ae1a09070eba24088a5eb9810efe57453779ec1e20a55080c6dc2c87
libXxf86vm-1.1.6 xxf86vm libXxf86vm.so.1 96af414c73ce1d5449ad04be7f9f27fa8330f844b6dda843ef22e3e1befb3ee3
X11_EXTENSIONS

log "x11 client libs OK ($SIDE, $T): x11=$(pkg-config --modversion x11) xext=$(pkg-config --modversion xext)"

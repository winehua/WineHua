#!/bin/bash
# build_x11_client.sh — libX11 + libXext (按侧参数化)
# 用法: SIDE=host bash scripts/build_x11_client.sh   # M0: host 侧 (mini X client)
#       SIDE=guest bash scripts/build_x11_client.sh  # M1: guest 侧 (winex11.drv 硬依赖面)
# libX11 tarball 自带 xtrans; keysymdef 数据来自 xorgproto (build_xcb_stack.sh 已装)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

SIDE="${SIDE:-host}"
case "$SIDE" in
    host)
        T="$NATIVE_TARGET"; PRE="$HOST_EXT_USR"; INC="$HOST_EXT_INC"
        LIB="$HOST_EXT_LIB"; PC="$HOST_EXT_PC"; SPC="$HOST_EXT_SHARE/pkgconfig"
        ;;
    guest)
        T="$GUEST_TARGET"; PRE="$SYSROOT_EXT/usr"; INC="$SYSROOT_EXT_INC"
        LIB="$SYSROOT_EXT_LIB"; PC="$SYSROOT_EXT_PC"; SPC="$SYSROOT_EXT_SHARE/pkgconfig"
        ;;
    *) err "SIDE 必须是 host|guest" ;;
esac
export PKG_CONFIG_PATH="$PC:$SPC${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_LIBDIR="$PC:$SPC:$SYSROOT/usr/lib/pkgconfig"

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

fetch_and_unpack() {
    local tgz="$DL/$(basename "$1")"
    if [ ! -f "$tgz" ]; then
        log "--- 下载 $(basename "$1") ---"
        curl -fsSL -o "$tgz" "$1"
    fi
    echo "$2  $tgz" | sha256sum -c - || { err "sha256 不匹配: $tgz"; }
    [ -d "$BUILD_DIR/$3" ] || tar -C "$BUILD_DIR" -xf "$tgz"
}

build_autotools() {  # $1=源码目录
    # --enable-malloc0returnsnull=yes: libX11 的 malloc(0) 运行检查交叉编译时无法执行 (实测踩坑);
    # 该值是 AC_ARG_ENABLE 选项, env 前缀会被 option 解析的 auto 默认值覆盖, 必须走命令行
    ( cd "$1" && ./configure --host=x86_64-linux-gnu --prefix="$PRE" \
        CC="$CCWRAP" \
        CFLAGS="-I$INC" \
        LDFLAGS="-fuse-ld=lld -L$LIB -L$SYSROOT/usr/lib/$T" \
        --enable-malloc0returnsnull=yes \
        --disable-static --enable-shared --disable-silent-rules )
    make -C "$1" -j"$JOBS" && make -C "$1" install
}

# ── 0. xtrans (libX11 configure 硬依赖 xtrans.pc; 纯头文件包) ──
if [ ! -f "$SPC/xtrans.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/xtrans-1.5.2.tar.xz" \
        "5c5cbfe34764a9131d048f03c31c19e57fb4c682d67713eab6a65541b4dff86c" "xtrans-1.5.2"
    build_autotools "$BUILD_DIR/xtrans-1.5.2"
fi
[ -f "$SPC/xtrans.pc" ] || err "xtrans 未就绪"

# ── 1. libX11 (≥1.8; 关 udc/xlocale 收窄) ──
if [ ! -f "$PC/x11.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libX11-1.8.10.tar.xz" \
        "2b3b3dad9347db41dca56beb7db5878f283bde1142f04d9f8e478af435dfdc53" "libX11-1.8.10"
    build_autotools "$BUILD_DIR/libX11-1.8.10"
fi
[ -f "$PC/x11.pc" ] || err "libX11 未就绪"

# ── 2. libXext ──
if [ ! -f "$PC/xext.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libXext-1.3.6.tar.xz" \
        "edb59fa23994e405fdc5b400afdf5820ae6160b94f35e3dc3da4457a16e89753" "libXext-1.3.6"
    build_autotools "$BUILD_DIR/libXext-1.3.6"
fi
[ -f "$PC/xext.pc" ] || err "libXext 未就绪"

# ── 侧别架构断言 ──
want=AArch64
[ "$T" = "x86_64-linux-ohos" ] && want=Advanced
for so in libX11.so.6 libXext.so.6; do
    m=$("$OHOS_SDK/native/llvm/bin/llvm-readelf" -h "$LIB/$so" | awk '/Machine:/{print $2}')
    [ "$m" = "$want" ] || err "架构侧别错误 (期望 $want): $LIB/$so → $m"
done

log "x11 client libs OK ($SIDE, $T): x11=$(pkg-config --modversion x11) xext=$(pkg-config --modversion xext)"

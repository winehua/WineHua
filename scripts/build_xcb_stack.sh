#!/bin/bash
# build_xcb_stack.sh — X 协议头 + libxcb + xcb-util-wm (按侧参数化)
# 用法: SIDE=host bash scripts/build_xcb_stack.sh   # M0: host 侧 (Xwayland / mini client 消费)
#       SIDE=guest bash scripts/build_xcb_stack.sh  # M1: guest 侧 (winex11.drv 消费)
# 侧别纪律见 .claude/rules/build-and-log.md; 版本钉死 + sha256 校验; 断言失败即非零退出
# 依赖清单来源: wlroots 0.20.2 xwayland/meson.build xwayland_required 实证
#   (xcb, xcb-composite, xcb-ewmh, xcb-icccm, xcb-render, xcb-res, xcb-xfixes>=1.15)
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
        # guest 代码库统一装 multiarch (wayland/freetype 同款, assemble 的
        # _pick_lib_pad_rf 与 wine 链接的 -L 都从这里取)
        LIB="$SYSROOT_EXT_LIB"; PC="$SYSROOT_EXT_PC"; SPC="$SYSROOT_EXT_SHARE/pkgconfig"
        ;;
    *) err "SIDE 必须是 host|guest" ;;
esac
# 搜索面含 $LIB/pkgconfig: guest 侧 libdir=multiarch, 这批包的 .pc 跟
# libdir 走 (Makefile 硬编码 pkgconfigdir=$(libdir)/pkgconfig, 不支持
# --with-pkgconfigdir; glib/gstreamer 的 .pc 也在此, 既有先例)
export PKG_CONFIG_PATH="$PC:$LIB/pkgconfig:$SPC${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_LIBDIR="$PC:$LIB/pkgconfig:$SPC:$SYSROOT/usr/lib/pkgconfig"

# .pc 就位检查 (两个历史落点: $PC = usr/lib/pkgconfig, $LIB/pkgconfig)
have_pc() { [ -f "$PC/$1.pc" ] || [ -f "$LIB/pkgconfig/$1.pc" ]; }
CC_BIN="$OHOS_SDK/native/llvm/bin/clang"

DL="$BUILD_DIR/downloads"
mkdir -p "$DL" "$PRE" "$INC" "$LIB" "$PC" "$SPC"

# 解包按目标隔离 ($BUILD_DIR/$T/<pkg>): host 与 guest pass 严禁共享
# configure/build 树 —— M1-T4 实测踩坑: 共享目录下 guest 重跑 configure
# 不会作废 host 的 arm64 对象缓存, make 直接重链 arm64 .so 并装进 guest
# 树 (libXau/libXdmcp 中招), 下游按 .la 链到 host-ext 库报
# "incompatible with elf_x86_64"。
fetch_and_unpack() {  # $1=url  $2=sha256  $3=解压后目录名
    local tgz="$DL/$(basename "$1")"
    if [ ! -f "$tgz" ]; then
        log "--- 下载 $(basename "$1") ---"
        curl -fsSL -o "$tgz" "$1"
    fi
    echo "$2  $tgz" | sha256sum -c - || { err "sha256 不匹配: $tgz"; }
    mkdir -p "$BUILD_DIR/$T"
    [ -d "$BUILD_DIR/$T/$3" ] || tar -C "$BUILD_DIR/$T" -xf "$tgz"
}

assert_min_ver() {  # $1=当前  $2=下限  $3=名称
    local cur="$1"
    [ -n "$cur" ] || err "$3 版本不可解析"
    [ "$(printf '%s\n' "$2" "$cur" | sort -V | head -1)" = "$2" ] || err "$3 版本不达标: $cur (需 ≥ $2)"
}

# CC wrapper: 把 --target/--sysroot 固定在编译器层。
# 教训: libtool 交叉模式(--host=gnu 三元组)会吞掉 LDFLAGS 里的 --target 且给链接塞
# -lgcc_s (bionic 无此库 → 链接失败, 实测 libXau); 走 native 模式 libtool + wrapper CC
# 是项目已验证模式 (build_xkbcommon.sh libxml2 同款)。
CCWRAP="$BUILD_DIR/ccwrap-$T.sh"
cat > "$CCWRAP" << WEOF
#!/bin/sh
exec "$CC_BIN" --target=$T --sysroot=$SYSROOT "\$@"
WEOF
chmod +x "$CCWRAP"

conf_ohos() {  # $1=源码目录; autotools 配置 (CC wrapper 携带真实目标)
    # --host=x86_64-linux-gnu 为固定谎言值 (build_xkbcommon.sh libffi/libxml2 同款):
    # 1) 哄过 config.sub (ohos 不被识别); 2) 让 libtool 走"同机"模式——
    # 否则 aarch64 gnu 三元组会塞 -lgcc_s (bionic 无此库, 链接失败); 真实目标在 CC wrapper。
    # --libdir 必须显式: guest 默认 $prefix/lib 会把库装到 sysroot-ext/usr/lib
    # 根, 而 _pick_lib_pad_rf (assemble) / wine -L 都从 multiarch 取 (M1-T4 实测)。
    ( cd "$1" && ./configure --host=x86_64-linux-gnu --prefix="$PRE" --libdir="$LIB" \
        CC="$CCWRAP" \
        CFLAGS="-I$INC" \
        LDFLAGS="-fuse-ld=lld -L$LIB -L$SYSROOT/usr/lib/$T" \
        --disable-static --enable-shared --disable-silent-rules )
}

build_autotools() {  # $1=源码目录
    conf_ohos "$1"
    make -C "$1" -j"$JOBS" && make -C "$1" install
}

# ── 1. xorgproto (X 协议头, 无编译代码) ──
if [ ! -f "$INC/X11/X.h" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/proto/xorgproto-2024.1.tar.xz" \
        "372225fd40815b8423547f5d890c5debc72e88b91088fbfb13158c20495ccb59" "xorgproto-2024.1"
    build_autotools "$BUILD_DIR/$T/xorgproto-2024.1"
fi
[ -f "$INC/X11/X.h" ] || err "xorgproto 未就绪 (缺 $INC/X11/X.h)"

# ── 2. xcb-proto (协议 XML + python 生成器, 数据件) ──
if [ ! -f "$SPC/xcb-proto.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/xcb/xcb-proto-1.17.0.tar.xz" \
        "2c1bacd2110f4799f74de6ebb714b94cf6f80fb112316b1219480fd22562148c" "xcb-proto-1.17.0"
    build_autotools "$BUILD_DIR/$T/xcb-proto-1.17.0"
fi
[ -f "$SPC/xcb-proto.pc" ] || err "xcb-proto 未就绪"

# ── 3. libXau + libXdmcp (libxcb configure 硬依赖 xau; xdmcp 供 Xwayland) ──
if ! have_pc xau; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libXau-1.0.12.tar.xz" \
        "74d0e4dfa3d39ad8939e99bda37f5967aba528211076828464d2777d477fc0fb" "libXau-1.0.12"
    build_autotools "$BUILD_DIR/$T/libXau-1.0.12"
fi
have_pc xau || err "libXau 未就绪"

if ! have_pc xdmcp; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libXdmcp-1.1.5.tar.gz" \
        "31a7abc4f129dcf6f27ae912c3eedcb94d25ad2e8f317f69df6eda0bc4e4f2f3" "libXdmcp-1.1.5"
    build_autotools "$BUILD_DIR/$T/libXdmcp-1.1.5"
fi
have_pc xdmcp || err "libXdmcp 未就绪"

# ── 4. libxcb (xcb/composite/render/res/xfixes 都在其中, ≥1.15) ──
if ! have_pc xcb; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/xcb/libxcb-1.17.0.tar.xz" \
        "599ebf9996710fea71622e6e184f3a8ad5b43d0e5fa8c4e407123c88a59a6d55" "libxcb-1.17.0"
    build_autotools "$BUILD_DIR/$T/libxcb-1.17.0"
fi
assert_min_ver "$(PKG_CONFIG_LIBDIR="$PC:$SPC" pkg-config --modversion xcb 2>/dev/null)" "1.15.0" "$SIDE xcb"

# ── 5. xcb-util-wm (ewmh + icccm) ──
if ! have_pc xcb-icccm; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/xcb/xcb-util-wm-0.4.2.tar.xz" \
        "62c34e21d06264687faea7edbf63632c9f04d55e72114aa4a57bb95e4f888a0b" "xcb-util-wm-0.4.2"
    build_autotools "$BUILD_DIR/$T/xcb-util-wm-0.4.2"
fi
{ have_pc xcb-ewmh && have_pc xcb-icccm; } || err "xcb-util-wm 未就绪"

# ── 侧别架构断言 ──
want=AArch64
[ "$T" = "x86_64-linux-ohos" ] && want=Advanced   # readelf 对 x86-64 输出 "Advanced Micro Devices X86-64"
for so in libxcb.so.1 libxcb-icccm.so.4; do
    m=$("$OHOS_SDK/native/llvm/bin/llvm-readelf" -h "$LIB/$so" | awk '/Machine:/{print $2}')
    [ "$m" = "$want" ] || err "架构侧别错误 (期望 $want): $LIB/$so → $m"
done

log "xcb stack OK ($SIDE, $T): xcb=$(pkg-config --modversion xcb) icccm=$(pkg-config --modversion xcb-icccm)"

#!/bin/bash
# build_xwayland.sh — Xwayland (钉版 xwayland-24.1.13) NCP 共享库形态构建 (host 侧)
# 侧别: Xwayland 是 host 组件 (NCP 子进程) — 纪律见 .claude/rules/build-and-log.md
# 伴生库: libxshmfence / libxcvt / libXfont2 → host-ext/<NATIVE_ARCH>
#         (libXau/libXdmcp 由 build_xcb_stack.sh, xtrans 由 build_x11_client.sh 供给)
# 产物:   build/host-ext/$NATIVE_ARCH/usr/lib/libxwayland_ohos.so (导出 main;
#         T7 NCP shim 调用; 装机拷贝由 assemble.sh 统一负责)
# 用法:   NATIVE_ARCH=arm64-v8a bash scripts/build_xwayland.sh
#
# R-xkb 运行时配置点 (构建期实测记录):
#   - xkb_dir (规则数据)  = $SYSROOT_EXT/usr/share/X11/xkb (设备端需同布局, T7 起用
#     Xwayland 的 -xkbdir 运行时参数指向沙箱内数据树)
#   - xkb_bin_dir         = 构建期选项 -Dxkb_bin_dir; 设备端无 xkbcomp 可执行 (且 NCP
#     内不可 fork/exec), Xwayland 的 XkbBinDirectory 是编译期常量 → keymap 编译会
#     失败, 但 xkb/ddxLoad.c POST_ERROR_MSG2 明示 "Errors from xkbcomp are not
#     fatal to the X server" — M0 以无键盘 keymap 起步, 键盘方案 T7/T9 裁决
#   - xkb_output_dir      = 编译期常量, 构建路径在设备端无效, 后果同上 (非致命)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

XSERVE_SRC="$ROOT/thirdparty/xserver"
XS_VER=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$XSERVE_SRC/meson.build" | head -1)
[ -n "$XS_VER" ] || err "无法从 $XSERVE_SRC/meson.build 解析 version"
[ "$XS_VER" = "24.1.13" ] || err "xserver 钉版漂移: $XS_VER (spec 钉 24.1.13, 裁决证据见 M0 台账)"

T="$NATIVE_TARGET"
DL="$BUILD_DIR/downloads"
mkdir -p "$DL" "$HOST_EXT_USR" "$HOST_EXT_INC" "$HOST_EXT_LIB" "$HOST_EXT_PC" "$HOST_EXT_SHARE/pkgconfig"

# host 侧 pc 搜索链, 与 gen_host_cross wrapper 一致 (share 目录不可省: fontsproto 等)
export PKG_CONFIG_LIBDIR="$HOST_EXT_PC:$HOST_EXT_SHARE/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT_EXT_PC:$SYSROOT_EXT/usr/share/pkgconfig"
export PKG_CONFIG_PATH="$HOST_EXT_PC:$HOST_EXT_SHARE/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

CCWRAP="$BUILD_DIR/ccwrap-$T.sh"
cat > "$CCWRAP" << WEOF
#!/bin/sh
exec "$OHOS_SDK/native/llvm/bin/clang" --target=$T --sysroot=$SYSROOT "\$@"
WEOF
chmod +x "$CCWRAP"

DL="$BUILD_DIR/downloads"
fetch_and_unpack() {  # $1=url  $2=sha256  $3=解压后目录名
    local tgz="$DL/$(basename "$1")"
    if [ ! -f "$tgz" ]; then
        log "--- 下载 $(basename "$1") ---"
        curl -fsSL -o "$tgz" "$1"
    fi
    echo "$2  $tgz" | sha256sum -c - || err "sha256 不匹配: $tgz"
    [ -d "$BUILD_DIR/$3" ] || tar -C "$BUILD_DIR" -xf "$tgz"
}

conf_ohos() {  # $1=源码目录; --host 谎言值原理同 build_xcb_stack.sh (libtool 同机模式)
    ( cd "$1" && ./configure --host=x86_64-linux-gnu --prefix="$HOST_EXT_USR" \
        CC="$CCWRAP" \
        CFLAGS="-I$HOST_EXT_INC" \
        LDFLAGS="-fuse-ld=lld -L$HOST_EXT_LIB -L$NATIVE_LIBS -L$SYSROOT/usr/lib/$T" \
        --disable-static --enable-shared --disable-silent-rules )
}

# ── 1. libxshmfence (xshmfence.pc) ──
if [ ! -f "$HOST_EXT_PC/xshmfence.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libxshmfence-1.3.3.tar.xz" \
        "d4a4df096aba96fea02c029ee3a44e11a47eb7f7213c1a729be83e85ec3fde10" "libxshmfence-1.3.3"
    conf_ohos "$BUILD_DIR/libxshmfence-1.3.3"
    make -C "$BUILD_DIR/libxshmfence-1.3.3" -j"$JOBS" && make -C "$BUILD_DIR/libxshmfence-1.3.3" install
fi
[ -f "$HOST_EXT_PC/xshmfence.pc" ] || err "libxshmfence 未就绪"

# ── 2. libxcvt (纯 meson 工程; pc 实名 libxcvt.pc) ──
if [ ! -f "$HOST_EXT_PC/libxcvt.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libxcvt-0.1.3.tar.xz" \
        "a929998a8767de7dfa36d6da4751cdbeef34ed630714f2f4a767b351f2442e01" "libxcvt-0.1.3"
    meson_host_build "$BUILD_DIR/xcvt_build_0.1.3" "$BUILD_DIR/libxcvt-0.1.3" \
        --prefix="$HOST_EXT_USR" --libdir=lib -Ddefault_library=shared
    ninja -C "$BUILD_DIR/xcvt_build_0.1.3" && ninja -C "$BUILD_DIR/xcvt_build_0.1.3" install
fi
[ -f "$HOST_EXT_PC/libxcvt.pc" ] || err "libxcvt 未就绪"

# ── 3. libfontenc (libXfont2 硬依赖 fontenc) ──
if [ ! -f "$HOST_EXT_PC/fontenc.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libfontenc-1.1.9.tar.xz" \
        "9d8392705cb10803d5fe1d27d236cbab3f664e26841ce01916bbbe430cf273e2" "libfontenc-1.1.9"
    conf_ohos "$BUILD_DIR/libfontenc-1.1.9"
    make -C "$BUILD_DIR/libfontenc-1.1.9" -j"$JOBS" && make -C "$BUILD_DIR/libfontenc-1.1.9" install
fi
[ -f "$HOST_EXT_PC/fontenc.pc" ] || err "libfontenc 未就绪"

# ── 4. libXfont2 (freetype: 头用源码树, 库链 NATIVE_LIBS 的 host 侧 .so) ──
if [ ! -f "$HOST_EXT_PC/xfont2.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libXfont2-2.0.6.tar.xz" \
        "74ca20017eb0fb3f56d8d5e60685f560fc85e5ff3d84c61c4cb891e40c27aef4" "libXfont2-2.0.6"
    ( cd "$BUILD_DIR/libXfont2-2.0.6" && ./configure --host=x86_64-linux-gnu --prefix="$HOST_EXT_USR" \
        CC="$CCWRAP" \
        CFLAGS="-I$HOST_EXT_INC -I$ROOT/thirdparty/freetype/include" \
        LDFLAGS="-fuse-ld=lld -L$HOST_EXT_LIB -L$NATIVE_LIBS -L$SYSROOT/usr/lib/$T" \
        --disable-static --enable-shared --disable-silent-rules \
        --disable-devel-docs --without-xmlto --without-fop )
    make -C "$BUILD_DIR/libXfont2-2.0.6" -j"$JOBS" && make -C "$BUILD_DIR/libXfont2-2.0.6" install
fi
[ -f "$HOST_EXT_PC/xfont2.pc" ] || err "libXfont2 未就绪"

# ── 5. libxkbfile (xserver meson.build:102 硬依赖 xkbfile) ──
if [ ! -f "$HOST_EXT_PC/xkbfile.pc" ]; then
    fetch_and_unpack "https://xorg.freedesktop.org/archive/individual/lib/libxkbfile-1.1.3.tar.xz" \
        "a9b63eea997abb9ee6a8b4fbb515831c841f471af845a09de443b28003874bec" "libxkbfile-1.1.3"
    conf_ohos "$BUILD_DIR/libxkbfile-1.1.3"
    make -C "$BUILD_DIR/libxkbfile-1.1.3" -j"$JOBS" && make -C "$BUILD_DIR/libxkbfile-1.1.3" install
fi
[ -f "$HOST_EXT_PC/xkbfile.pc" ] || err "libxkbfile 未就绪"

# ── 6. libsha1 (xserver 唯一消费方 render/glyph.c 的 SHA-1 提供方) ──
# 来源: github.com/dottedmag/libsha1 (正主镜像; gitlab.com SaaS 反爬 403 不可下)。
# 上游为 autotools 工程, 但仅 sha1.c 一个编译单元 — 跳过 autoreconf 直编成
# 静态库 (-fPIC, 将被链入 libxwayland_ohos.so), pc 按上游 libsha1.pc.in 手写。
# bionic 无内置 SHA1 API; 不钉此项时 xserver auto 探测会撞上 guest 侧 x86_64
# nettle.pc (gnutls_staging) → 链接报 incompatible with aarch64linux (实测)。
LIBSHA1_SRC="$BUILD_DIR/libsha1-master"
if [ ! -f "$HOST_EXT_PC/libsha1.pc" ]; then
    fetch_and_unpack "https://github.com/dottedmag/libsha1/archive/refs/heads/master.tar.gz" \
        "1fcf23b10d9e253f392d5608ce014577bbecd7aa62b1746892c5ad2f98ab7caa" "libsha1-master"
    "$OHOS_SDK/native/llvm/bin/clang" --target=$T --sysroot=$SYSROOT \
        -fPIC -O2 -I"$LIBSHA1_SRC" -c "$LIBSHA1_SRC/sha1.c" \
        -o "$BUILD_DIR/libsha1-master/sha1.o"
    "$OHOS_SDK/native/llvm/bin/llvm-ar" rcs "$BUILD_DIR/libsha1-master/libsha1.a" \
        "$BUILD_DIR/libsha1-master/sha1.o"
    cp "$BUILD_DIR/libsha1-master/libsha1.a" "$HOST_EXT_LIB/"
    cp "$LIBSHA1_SRC/libsha1.h" "$HOST_EXT_INC/"
    cat > "$HOST_EXT_PC/libsha1.pc" << EOF
Name: libsha1
Description: Tiny SHA1 implementation for embedded devices.
Version: 1.0
Cflags: -I$HOST_EXT_INC
Libs: -L$HOST_EXT_LIB -lsha1
EOF
    log "libsha1 (直编) → $HOST_EXT_LIB/libsha1.a"
fi
[ -f "$HOST_EXT_PC/libsha1.pc" ] || err "libsha1 未就绪"
[ -f "$HOST_EXT_LIB/libsha1.a" ] || err "libsha1.a 未产出"

# ── 7. xserver → libxwayland_ohos.so (out-of-tree 补丁, NCP 共享库形态) ──
# 补丁存主仓库 scripts/patches/ (submodule 钉在上游 tag 不带本地提交——补丁
# 提交无法推送时保证指针可解析, glib-format-security.patch 同款惯例)。
# 产物与签名标记落 host-ext (build/ 内), 装机拷贝由 assemble.sh 统一负责。
XS_BUILD="$BUILD_DIR/xwayland_build_$XS_VER"
XS_PATCH="$SCRIPT_DIR/patches/xserver-xwayland-ohos-ncp.patch"
# 守卫 = 产物存在 + 补丁签名一致 (补丁任一变更即重建; 只看文件存在会吃掉
# 补丁改动 — 与 wayland 头文件守卫同款教训)
XS_PATCH_SIG=$(sha256sum "$XS_PATCH" | cut -d' ' -f1)
if [ ! -f "$HOST_EXT_LIB/libxwayland_ohos.so" ] \
   || [ "$(cat "$HOST_EXT_LIB/.xwayland_ohos_sig" 2>/dev/null)" != "$XS_PATCH_SIG" ]; then
    [ -f "$XSERVE_SRC/hw/xwayland/meson.build" ] || err "thirdparty/xserver 缺源码"
    # 幂等应用: sentinel 命中视为已应用 (构建中途重跑不重复 apply)
    if ! grep -q "xwayland_ohos_lib" "$XSERVE_SRC/hw/xwayland/meson.build"; then
        git -C "$XSERVE_SRC" apply --check "$XS_PATCH" \
            || err "xserver 补丁无法应用 (submodule 工作区与补丁基线不符)"
        git -C "$XSERVE_SRC" apply "$XS_PATCH"
    fi
    # 24.1 独立分支无 xorg/xephyr/xnest 选项 (Xwayland-only 项目, 结构性裁剪);
    # xvfb/docs*/glamor 逐项关闭至依赖闭包最小 (sha1/libdecor/ei 走 auto 探测);
    # glx=false 同时让 include/meson.build:9 的 dri_dep 转 optional
    # (required: build_glx), M0 shm-only 无 GLX 需求;
    # bionic 无 libc RPC 且无 libtirpc (os/meson.build:66 实测报错),
    # xdm-auth-1 依赖 DES RPC 一并关闭
    meson_host_build "$XS_BUILD" "$XSERVE_SRC" \
        --prefix="$HOST_EXT_USR" --libdir=lib \
        -Dxvfb=false \
        -Ddocs=false -Ddevel-docs=false -Ddocs-pdf=false \
        -Dglamor=false -Dglx=false \
        -Dsecure-rpc=false -Dxdm-auth-1=false \
        -Dsha1=libsha1 \
        -Dxkb_dir="$SYSROOT_EXT/usr/share/X11/xkb" \
        -Dxkb_output_dir="$BUILD_DIR/xkb_output_$XS_VER" \
        -Dxkb_bin_dir="$HOST_EXT_USR/bin" \
        -Ddefault_library=static
    ninja -C "$XS_BUILD" hw/xwayland/libxwayland_ohos.so
    cp "$XS_BUILD/hw/xwayland/libxwayland_ohos.so" "$HOST_EXT_LIB/libxwayland_ohos.so"
    echo "$XS_PATCH_SIG" > "$HOST_EXT_LIB/.xwayland_ohos_sig"
fi

# ── 验收断言 (产物在 build/ 内; entry/libs 由 assemble.sh 统一装配) ──
[ -f "$HOST_EXT_LIB/libxwayland_ohos.so" ] || err "libxwayland_ohos.so 未产出"
LLVM="$OHOS_SDK/native/llvm/bin"
# grep -c 消费全量输入: pipefail 下 nm|grep -q 命中早退会让 nm 吃 SIGPIPE 假失败
"$LLVM/llvm-nm" -D "$HOST_EXT_LIB/libxwayland_ohos.so" | grep -c " T main\$" | grep -q "^1$" \
    || err "libxwayland_ohos.so 未导出 T main (NCP 入口要求)"
m=$("$LLVM/llvm-readelf" -h "$HOST_EXT_LIB/libxwayland_ohos.so" | awk '/Machine:/{print $2; exit}')
want=AArch64
[ "$T" = "x86_64-linux-ohos" ] && want=Advanced
[ "$m" = "$want" ] || err "架构侧别错误 (期望 $want): libxwayland_ohos.so → $m"

log "xwayland OK ($NATIVE_ARCH, $XS_VER): $HOST_EXT_LIB/libxwayland_ohos.so"

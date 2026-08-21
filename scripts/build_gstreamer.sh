#!/bin/bash
# build_gstreamer.sh — GStreamer 链交叉编译 → sysroot-ext (供 Wine winegstreamer 使用)
#
# 依赖链 (pcre2/glib 用 autotools+meson, gstreamer 系用 meson):
#   pcre2 ─→ glib(2.78) ─→ gstreamer core(1.24.4) ─→ gst-plugins-base
#   zlib(OHOS sysroot)                              (出 gstreamer-video/audio/tag .pc)
#   libffi(已编)
#
# Wine configure 探测 gstreamer-1.0/video/audio/tag 四个 .pc:
#   core 出 gstreamer-1.0 / gstreamer-base-1.0, base 的 gst-libs 出 video/audio/tag。
# 全部直接装 sysroot-ext (prefix=$SYSROOT_EXT/usr, libdir=lib/x86_64-linux-ohos),
# 与 gnutls 链不同 (那套走 staging 中转, 因库间用 pkg-config 互相找)。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

GLIB_SRC="$ROOT/thirdparty/glib"
GST_SRC="$ROOT/thirdparty/gstreamer"
PCRE2_SRC="$ROOT/thirdparty/pcre2"
# gst-plugins-base: fd.o 独立仓库停在 1.12, 1.24 只在 monorepo subproject。
# gstreamer 子模块已直接跟踪 subprojects/gst-plugins-base (1.24.4, 与 core 同源),
# 直接用它, 无需外部 .temp/crossover staging。
BASE_SRC="$ROOT/thirdparty/gstreamer/subprojects/gst-plugins-base"
GOOD_SRC="$ROOT/thirdparty/gstreamer/subprojects/gst-plugins-good"
BAD_SRC="$ROOT/thirdparty/gstreamer/subprojects/gst-plugins-bad"
UGLY_SRC="$ROOT/thirdparty/gstreamer/subprojects/gst-plugins-ugly"
LIBAV_SRC="$ROOT/thirdparty/gstreamer/subprojects/gst-libav"
FFMPEG_SRC="$BUILD_DIR/ffmpeg_src"

GST_PREFIX="$SYSROOT_EXT/usr"
GST_LIBDIR="$GST_PREFIX/lib/$TARGET"

# 幂等跳过: 4 个 .pc + 关键 .so 齐全
idempotent_done() {
    [ -f "$SYSROOT_EXT_PC/gstreamer-1.0.pc" ] \
        && [ -f "$SYSROOT_EXT_PC/gstreamer-video-1.0.pc" ] \
        && [ -f "$SYSROOT_EXT_PC/glib-2.0.pc" ] \
        && [ -f "$SYSROOT_EXT_LIB/libgstreamer-1.0.so.0" ] \
        && [ -f "$SYSROOT_EXT_LIB/libglib-2.0.so.0" ] \
        && [ -f "$SYSROOT_EXT_INC/glib.h" ]
}

log "=== 构建 GStreamer 链 (core + plugins-base/good + libav, $WINE_ARCH) → sysroot-ext ==="

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$BUILD_DIR"

CROSS_CFLAGS="-O2 -fPIC -D__MUSL__"
CROSS_LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET"
# pkgconfigdir = libdir/pkgconfig (x86_64-linux-ohos 子目录) — 两个路径都要
export PKG_CONFIG_PATH="$SYSROOT_EXT_PC:$GST_LIBDIR/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export CFLAGS="-I$SYSROOT_EXT_INC $CROSS_CFLAGS"
export LDFLAGS="-L$SYSROOT_EXT_LIB $CROSS_LDFLAGS"
export CC="$CLANG --target=$TARGET --sysroot=$SYSROOT"

# OHOS sysroot 有 libz 但无 .pc (glib meson dependency('zlib') 需要)
if [ ! -f "$SYSROOT_EXT_PC/zlib.pc" ]; then
    cat > "$SYSROOT_EXT_PC/zlib.pc" << EOF
prefix=$SYSROOT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET
Name: zlib
Description: zlib compression library
Version: 1.2.11
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF
fi
# musl (OHOS) 无 libintl: libc 已裁剪 gettext 符号, 也无 libintl.h/libintl.so。
# glib 的 meson dependency('intl') 强制要求真库 (找不到就走 proxy-libintl wrap,
# nodownload/nofallback 下都报 ERROR), 且其检测走 find_library 不走 pkg-config
# → 构建 stub libintl (gettext 系返回 msgid, 即 musl 的 stub 语义)。
# (方案来自 feature/build-pipeline 的 fix(build) commit 86bd555)
if [ ! -f "$SYSROOT_EXT_LIB/libintl.so" ]; then
    log "--- 构建 stub libintl (gettext 返回 msgid) ---"
    mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB"
    cat > "$SYSROOT_EXT_INC/libintl.h" << 'INTL_H'
#ifndef _LIBINTL_H
#define _LIBINTL_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
char *gettext(const char *msgid);
char *dgettext(const char *domainname, const char *msgid);
char *dcgettext(const char *domainname, const char *msgid, int category);
char *ngettext(const char *msgid, const char *msgid_plural, unsigned long int n);
char *dngettext(const char *domainname, const char *msgid, const char *msgid_plural, unsigned long int n);
char *dcngettext(const char *domainname, const char *msgid, const char *msgid_plural, unsigned long int n, int category);
char *textdomain(const char *domainname);
char *bindtextdomain(const char *domainname, const char *dirname);
char *bind_textdomain_codeset(const char *domainname, const char *codeset);
#ifdef __cplusplus
}
#endif
#endif /* _LIBINTL_H */
INTL_H
    cat > "$BUILD_DIR/libintl.c" << 'INTL_C'
#include "libintl.h"
char *gettext(const char *msgid) { return (char *)msgid; }
char *dgettext(const char *domainname, const char *msgid) { (void)domainname; return (char *)msgid; }
char *dcgettext(const char *domainname, const char *msgid, int category) { (void)domainname; (void)category; return (char *)msgid; }
char *ngettext(const char *msgid, const char *msgid_plural, unsigned long int n) { return (char *)(n == 1 ? msgid : msgid_plural); }
char *dngettext(const char *domainname, const char *msgid, const char *msgid_plural, unsigned long int n) { (void)domainname; return (char *)(n == 1 ? msgid : msgid_plural); }
char *dcngettext(const char *domainname, const char *msgid, const char *msgid_plural, unsigned long int n, int category) { (void)domainname; (void)category; return (char *)(n == 1 ? msgid : msgid_plural); }
char *textdomain(const char *domainname) { return (char *)domainname; }
char *bindtextdomain(const char *domainname, const char *dirname) { (void)dirname; return (char *)domainname; }
char *bind_textdomain_codeset(const char *domainname, const char *codeset) { (void)domainname; return (char *)codeset; }
INTL_C
    "$CLANG" --target="$TARGET" --sysroot="$SYSROOT" -shared -fPIC -O2 \
        -o "$SYSROOT_EXT_LIB/libintl.so" "$BUILD_DIR/libintl.c" || err "stub libintl 编译失败"
    rm -f "$BUILD_DIR/libintl.c"
    # .pc 指向真实 stub (供走 pkg-config 的库链接 -lintl)
    cat > "$SYSROOT_EXT_PC/libintl.pc" << EOF
prefix=$SYSROOT_EXT/usr
includedir=\${prefix}/include
libdir=\${prefix}/lib/$TARGET
Name: libintl
Description: GNU gettext (musl stub)
Version: 0.22
Libs: -L\${libdir} -lintl
Cflags: -I\${includedir}
EOF
fi

# 每库 install 后 .pc 复制到 $SYSROOT_EXT_PC (wine configure 只搜那里)
stage_pcs() {
    cp "$GST_LIBDIR"/pkgconfig/*.pc "$SYSROOT_EXT_PC/" 2>/dev/null || true
}

# ── 1. pcre2 (glib 正则后端, autotools) ──
if [ ! -f "$SYSROOT_EXT_LIB/libpcre2-8.so.0" ]; then
    log "--- 构建 pcre2 ---"
    # git 树无 configure, 首次跑 autogen.sh (autotools 全套); touch 规避 NFS clock skew
    [ -f "$PCRE2_SRC/configure" ] || { find "$PCRE2_SRC" -type f -exec touch {} + 2>/dev/null || true; (cd "$PCRE2_SRC" && ./autogen.sh); find "$PCRE2_SRC" -type f -exec touch {} + 2>/dev/null || true; }
    build="$BUILD_DIR/pcre2_build"
    rm -rf "$build" && mkdir -p "$build" && cd "$build"
    CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
    "$PCRE2_SRC/configure" --host=$GNU_HOST --prefix="$GST_PREFIX" \
        --libdir="$GST_LIBDIR" --disable-static --disable-pcre2grep --disable-pcre2test \
        --enable-jit=no
    make -j$JOBS
    make install
    cd "$SCRIPT_DIR"
    stage_pcs
else
    log "pcre2 已就绪，跳过"
fi

# ── 2. glib 2.78 (meson) ──
if [ ! -f "$SYSROOT_EXT_LIB/libglib-2.0.so.0" ]; then
    log "--- 构建 glib ---"
    # gnulib works 检测: cross file 的 needs_exe_wrapper=true 使
    # meson.can_run_host_binaries() 返回 false, 检测自动走 else 分支
    # (works=true, 不依赖 cc.run), 无需 sed 源码。
    # -Werror=format=2 含 format-security: G_DBUS_ERROR 宏的 format 参数非字面量
    # 触发 (gdebugcontrollerdbus.c) → 以 patch 方式追加 -Wno-error 豁免。
    # 改动仅 2 行, 不值得提交 submodule (开分支/push/指针更新), patch 随构建走。
    PATCH="$SCRIPT_DIR/patches/glib-format-security.patch"
    if ! git -C "$GLIB_SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
        git -C "$GLIB_SRC" apply "$PATCH"
        log "  已应用 patch: $(basename "$PATCH")"
    fi
    # musl 无完整 libintl 语义: 简化 glib intl 检测, 跳过 proxy-libintl internal 断言
    # (stub libintl 已在脚本开头构建, 供 find_library 检测; nls=disabled 走 glib 内建 stub)
    sed -i "s|assert(libintl.type_name() == 'internal')|assert(true) # OHOS: use libintl stub|" "$GLIB_SRC/meson.build" 2>/dev/null || true
    build="$BUILD_DIR/glib_build"
    rm -rf "$build"
    # --wrap-mode=nofallback: 禁用 fallback subproject。musl 无独立 libintl.so
    # (OHOS libc 已裁剪 gettext), meson 的 dependency('intl') 走内置检测
    # (find_library, 不走 pkg-config), 找不到时 fallback 指定的 proxy-libintl wrap
    # 在 nodownload 下 buildable NO → ERROR。nofallback 直接 not-found, 配合
    # -Dnls=disabled 走 glib 内建 stub。pcre2/zlib/libffi 已由 .pc 提供, 不触发 fallback。
    meson setup "$build" "$GLIB_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/$TARGET --wrap-mode=nofallback \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dman=false \
        -Ddtrace=false -Dsystemtap=false -Dgtk_doc=false -Dtests=false \
        -Dinstalled_tests=false -Dlibelf=disabled \
        -Dnls=disabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
else
    log "glib 已就绪，跳过"
fi

# ── 3. gstreamer core 1.24.4 (meson, 只需 core 本身) ──
if [ ! -f "$SYSROOT_EXT_LIB/libgstreamer-1.0.so.0" ]; then
    log "--- 构建 gstreamer core ---"
    build="$BUILD_DIR/gstreamer_build"
    rm -rf "$build"
    meson setup "$build" "$GST_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/$TARGET --wrap-mode=nofallback \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dtests=disabled -Dexamples=disabled -Dtools=disabled \
        -Dintrospection=disabled -Ddoc=disabled -Dgtk_doc=disabled -Dorc=disabled \
        -Dbase=disabled -Dgood=disabled -Dbad=disabled -Dugly=disabled -Dlibav=disabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
else
    log "gstreamer core 已就绪，跳过"
fi

# ── 4. gst-plugins-base 1.24.4 (meson, monorepo subproject) ──
# 既出 gst-libs 的 .pc (wine configure 探测用), 也编基础插件
# (typefind/playback/app/audioconvert 等, winegstreamer 运行时必需)。
if [ ! -f "$SYSROOT_EXT_PC/gstreamer-video-1.0.pc" ] || \
   [ ! -d "$GST_LIBDIR/gstreamer-1.0" ]; then
    log "--- 构建 gst-plugins-base ---"
    build="$BUILD_DIR/gst_base_build"
    rm -rf "$build"
    meson setup "$build" "$BASE_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/$TARGET --wrap-mode=nofallback \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dtests=disabled -Dexamples=disabled -Dintrospection=disabled -Ddoc=disabled \
        -Dorc=disabled -Dnls=disabled \
        -Dcompositor=disabled -Ddebugutils=disabled -Dencoding=disabled \
        -Doverlaycomposition=disabled -Ddrm=disabled -Dgl=disabled -Dalsa=disabled \
        -Dcdparanoia=disabled -Dlibvisual=disabled -Dogg=disabled -Dopus=disabled \
        -Dpango=disabled -Dtheora=disabled -Dtremor=disabled -Dvorbis=disabled \
        -Dxshm=disabled -Dxi=disabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
else
    log "gst-plugins-base 已就绪，跳过"
fi

# include 平铺 symlink: wine configure 的 gint64 检查 (AC_COMPILE_IFELSE) 只用
# 全局 CFLAGS (-I$SYSROOT_EXT_INC), 不含 pkg-config 的 -Iglib-2.0/-Igstreamer-1.0
for f in "$SYSROOT_EXT_INC"/glib-2.0/*; do
    b="$(basename "$f")"
    [ -e "$SYSROOT_EXT_INC/$b" ] || ln -sfn "glib-2.0/$b" "$SYSROOT_EXT_INC/$b"
done
# glibconfig.h 是构建产物头, meson 按 libdir 装 (lib/x86_64-linux-ohos/glib-2.0/include/)
ln -sfn ../lib/$TARGET/glib-2.0/include/glibconfig.h "$SYSROOT_EXT_INC/glibconfig.h"
ln -sfn gstreamer-1.0/gst "$SYSROOT_EXT_INC/gst"

# wine 直接 -l 链接 4 个包 (动态链接不看 Libs.private) → Libs 补全依赖链,
# 否则 winegstreamer.so 链接报 g_*/g_object_* undefined
for pc in gstreamer-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 gstreamer-tag-1.0; do
    f="$SYSROOT_EXT_PC/$pc.pc"
    grep -q "lgstbase-1.0" "$f" || \
        sed -i "/^Libs:/ s/\$/ -lgstbase-1.0 -lgstpbutils-1.0 -lglib-2.0 -lgobject-2.0 -lgmodule-2.0 -lgio-2.0/" "$f"
done

# ── 5. gst-plugins-good (demuxer: qtdemux/matroskademux/typefind 等) ──
# 幂等以 qtdemux 所在库 libgstisomp4.so 为准 (无独立 libgstqtdemux.so)
if [ ! -d "$GST_LIBDIR/gstreamer-1.0" ] || \
   [ ! -f "$GST_LIBDIR/gstreamer-1.0/libgstisomp4.so" ]; then
    log "--- 构建 gst-plugins-good ---"
    build="$BUILD_DIR/gst_good_build"
    rm -rf "$build"
    # zlib 来自 OHOS sysroot; vpx 插件已由 libvpx 提供。禁用外部音频/图像依赖。
    meson setup "$build" "$GOOD_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/$TARGET --wrap-mode=nofallback \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dcairo=disabled -Ddv=disabled -Dflac=disabled -Djack=disabled \
        -Djpeg=disabled -Dlame=disabled -Dlibcaca=disabled -Dmpg123=disabled \
        -Dpulse=disabled -Dshout2=disabled -Dspeex=disabled -Dtaglib=disabled \
        -Dtwolame=disabled -Dvpx=disabled -Dwavpack=disabled -Daalib=disabled \
        -Damrnb=disabled -Damrwbdec=disabled -Ddv1394=disabled -Dgtk3=disabled \
        -Doss=disabled -Doss4=disabled -Dosxaudio=disabled -Dosxvideo=disabled \
        -Dqt5=disabled -Dqt6=disabled -Drpicamsrc=disabled -Dsoup=disabled \
        -Dv4l2=disabled -Dximagesrc=disabled -Ddirectsound=disabled \
        -Dhls-crypto=nettle
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
else
    log "gst-plugins-good 已就绪，跳过"
fi

# ── 5b. gst-plugins-bad (h264parse 等视频解析插件) ──
# H.264 (AVC) in MP4: qtdemux 输出 video/x-h264(stream-format=avc)，需
# h264parse 转 byte-stream 后 avdec_h264 (libav) 才能解码。缺它会导致
# decodebin 协商失败 → caps not fixed → 播放失败。
# 用 -Dauto_features=disabled + 显式 enabled 只编无外部依赖的插件
# (h264parse 是 videoparsers 的元素, 插件文件名为 libgstvideoparsersbad.so)。
if [ -d "$BAD_SRC" ] && \
   [ ! -f "$GST_LIBDIR/gstreamer-1.0/libgstvideoparsersbad.so" ]; then
    log "--- 构建 gst-plugins-bad (h264parse 等) ---"
    build="$BUILD_DIR/gst_bad_build"
    rm -rf "$build"
    # CUDA 库 (gst-libs/gst/cuda) 用 C++ 编译, OHOS musl 交叉链接缺 libstdc++
    # 符号; 无 meson option 可禁用。构建期临时 sed 注释 (幂等, 不污染子模块
    # 提交; 若子模块工作树被还原, 本 sed 下次构建自动重新应用)。
    sed -i "s/^subdir('cuda')/# subdir('cuda') # disabled: musl C++ link/" \
        "$BAD_SRC/gst-libs/gst/meson.build"
    meson setup "$build" "$BAD_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/x86_64-linux-ohos --wrap-mode=nofallback \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dauto_features=disabled -Dgpl=disabled -Dexamples=disabled -Dtests=disabled \
        -Dvideoparsers=enabled -Dasfmux=enabled \
        -Dmpegdemux=enabled -Dmpegtsdemux=enabled -Dmidi=enabled -Daiff=enabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
    log "gst-plugins-bad 构建完成 (h264parse 等)"
else
    log "gst-plugins-bad 已就绪或不可用，跳过"
fi

# ── 5c. gst-plugins-ugly (asfdemux: WMV/ASF 播放) ──
# 吉里吉里等游戏的 MV 常为 WMV (ASF 容器)。ASF demuxer (asfdemux) 在
# gst-plugins-ugly (rank=SECONDARY, decodebin 自动选用), 不在 bad。
# bad 的 asfparse (rank=NONE) 只做 parse 不解 ES, decodebin 不会用它。
if [ -d "$UGLY_SRC" ] && \
   [ ! -f "$GST_LIBDIR/gstreamer-1.0/libgstasf.so" ]; then
    log "--- 构建 gst-plugins-ugly (asfdemux) ---"
    build="$BUILD_DIR/gst_ugly_build"
    rm -rf "$build"
    meson setup "$build" "$UGLY_SRC" --cross-file "$(gen_cross_file)" \
        --prefix="$GST_PREFIX" -Dlibdir=lib/x86_64-linux-ohos --wrap-mode=nofallback \
        -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
        -Dauto_features=disabled -Dgpl=disabled -Dnls=disabled -Dtests=disabled -Ddoc=disabled \
        -Dasfdemux=enabled
    meson compile -C "$build" -j "$JOBS"
    DESTDIR=/ meson install -C "$build"
    stage_pcs
    log "gst-plugins-ugly 构建完成 (asfdemux)"
else
    log "gst-plugins-ugly 已就绪或不可用，跳过"
fi

# ── 6. FFmpeg + gst-libav (通用解码: H.264/VP9/AAC 等) ──
if [ ! -f "$SYSROOT_EXT_LIB/libavcodec.so" ] || \
   [ ! -f "$GST_LIBDIR/gstreamer-1.0/libgstlibav.so" ]; then
    log "--- 构建 FFmpeg (meson-ports) ---"
    if [ ! -d "$FFMPEG_SRC/.git" ]; then
        if [ ! -f "$FFMPEG_SRC/meson.build" ]; then
            rm -rf "$FFMPEG_SRC"
            if [ -f "$ROOT/build/ffmpeg-meson.tar.gz" ]; then
                mkdir -p "$FFMPEG_SRC"
                tar -xzf "$ROOT/build/ffmpeg-meson.tar.gz" -C "$FFMPEG_SRC" --strip-components=1
            else
                git clone --depth 1 --branch meson-6.1 \
                    https://gitlab.freedesktop.org/gstreamer/meson-ports/ffmpeg.git "$FFMPEG_SRC" \
                    || { echo "FFmpeg clone 失败, 跳过 libav"; FFMPEG_SRC=""; }
            fi
        fi
    fi
    if [ -n "${FFMPEG_SRC:-}" ] && [ -f "$FFMPEG_SRC/meson.build" ]; then
        build="$BUILD_DIR/ffmpeg_build"
        rm -rf "$build"
        meson setup "$build" "$FFMPEG_SRC" --cross-file "$(gen_cross_file)" \
            --prefix="$GST_PREFIX" -Dlibdir=lib/$TARGET \
            -Ddefault_library=shared \
            -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
            -Dc_link_args="--target=$TARGET --sysroot=$SYSROOT -L$SYSROOT_EXT_LIB -lz" \
            -Dprograms=disabled -Dtests=disabled \
            -Dzlib=enabled -Diconv=disabled \
            -Dlibx264=disabled -Dlibvpx=disabled -Dlibopus=disabled
        meson compile -C "$build" -j "$JOBS"
        DESTDIR=/ meson install -C "$build"
        stage_pcs
    fi
    if [ -n "${FFMPEG_SRC:-}" ] && [ -f "$FFMPEG_SRC/meson.build" ] && \
       [ -f "$SYSROOT_EXT_LIB/libavcodec.so" ]; then
        log "--- 构建 gst-libav ---"
        build="$BUILD_DIR/gst_libav_build"
        rm -rf "$build"
        meson setup "$build" "$LIBAV_SRC" --cross-file "$(gen_cross_file)" \
            --prefix="$GST_PREFIX" -Dlibdir=lib/$TARGET --wrap-mode=nofallback \
            -Dc_args="--target=$TARGET --sysroot=$SYSROOT -I$SYSROOT_EXT_INC -D__MUSL__" \
            -Dtests=disabled -Ddoc=disabled
        meson compile -C "$build" -j "$JOBS"
        DESTDIR=/ meson install -C "$build"
        stage_pcs
    fi
else
    log "gst-libav 已就绪，跳过"
fi

log "GStreamer 链就绪: $SYSROOT_EXT"

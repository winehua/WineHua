#!/bin/bash
# build_gnutls.sh — GnuTLS 链交叉编译 → sysroot-ext (供 Wine schannel 使用)
#
# 依赖链 (全 autotools, 安装到统一 staging, 最后复制到 sysroot-ext):
#   gmp → nettle(→hogweed) → gnutls
#   libtasn1 ──────────────┘
#   libunistring ──────────┘
#
# 交叉目标 x86_64-linux-ohos (Wine unix 层), 仿 build_libffi.sh 模式:
#   CC="$CLANG --target=$TARGET --sysroot=$SYSROOT" + --host=$GNU_HOST
#   (configure 只看 host 判平台特性, 实际编译 target 由 --target= 控制)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

# STAGING 按 wine 架构隔离: 跨架构切换 (aarch64↔x86_64) 复用共享目录会残留旧架构库
# (build_one 每次 rm -rf 重建但 STAGING 不清空) → gnutls configure 链接到旧架构 nettle
# 报 "Nettle lacks rsa_sec_decrypt"。master 仅 x86_64 无此问题; feature/arm64 引入 aarch64 后必现。
STAGING="$BUILD_DIR/gnutls_staging-$WINE_ARCH"
TARBALL_DIR="$BUILD_DIR/gnutls_tarballs"

# release tarball 源: 自带 configure/Makefile.in, 免 gnulib bootstrap。
# git 树的 bootstrap 依赖 gnulib 且与 libtasn1 等库存在版本错配
# (src/gl/lib/malloc.c.diff 打不上, 且 build-aux 生成不全/软链断链),
# fresh clone 下无法一次构建成功 → 改用同版本官方 release tarball,
# 语义等价且可复现。
fetch_tarball() {
    local name="$1" primary="$2" fallback="$3"
    local out="$TARBALL_DIR/$name"
    local archive="$TARBALL_DIR/$(basename "$primary")"
    # 幂等: configure 就位才算就绪 (防上次解压中断留下不完整目录被误判)
    if [ -f "$out/configure" ]; then return 0; fi
    mkdir -p "$TARBALL_DIR"
    if ! curl -fL --retry 3 -o "$archive" "$primary"; then
        [ -n "$fallback" ] || err "下载 $name 失败: $primary"
        log "--- $name 首选源失败, 回退: $fallback ---"
        curl -fL --retry 3 -o "$archive" "$fallback" || err "下载 $name 失败: $fallback"
    fi
    case "$archive" in
        *.tar.xz) tar -xJf "$archive" -C "$TARBALL_DIR" ;;
        *) tar -xzf "$archive" -C "$TARBALL_DIR" ;;
    esac
    # GNU release tarball 顶层目录即 <name>-<version>, 直接推导;
    # 不再次 tar -t (xz 归档需 -tJ, 避免误用 gzip 报错)
    if [ ! -d "$out" ]; then
        local base="${archive##*/}"
        local top="${base%.tar.*}"
        [ -n "$top" ] && [ -d "$TARBALL_DIR/$top" ] && mv "$TARBALL_DIR/$top" "$out"
    fi
    log "--- $name 就绪: $out ---"
}

# GNU 库首选国内 mirror (Ustc, ~1MB/s), 官方 ftp.gnu.org 作 fallback;
# gnutls 无 GNU mirror, 用 gnupg.org 官方源 (实测速度快)
fetch_tarball gmp "https://mirrors.ustc.edu.cn/gnu/gmp/gmp-6.2.1.tar.xz" "https://ftp.gnu.org/gnu/gmp/gmp-6.2.1.tar.xz"
fetch_tarball nettle "https://mirrors.ustc.edu.cn/gnu/nettle/nettle-3.10.2.tar.gz" "https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz"
fetch_tarball libtasn1 "https://mirrors.ustc.edu.cn/gnu/libtasn1/libtasn1-4.20.0.tar.gz" "https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.20.0.tar.gz"
fetch_tarball libunistring "https://mirrors.ustc.edu.cn/gnu/libunistring/libunistring-1.3.tar.xz" "https://ftp.gnu.org/gnu/libunistring/libunistring-1.3.tar.xz"
fetch_tarball gnutls "https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.3.tar.xz" ""

# 幂等跳过: 5 个库的关键产物全部就位
idempotent_done() {
    [ -f "$SYSROOT_EXT_LIB/libgnutls.so.30" ] \
        && [ -f "$SYSROOT_EXT_LIB/libnettle.so.8" ] \
        && [ -f "$SYSROOT_EXT_LIB/libhogweed.so.6" ] \
        && [ -f "$SYSROOT_EXT_LIB/libgmp.so.10" ] \
        && [ -f "$SYSROOT_EXT_LIB/libtasn1.so.6" ] \
        && [ -f "$SYSROOT_EXT_LIB/libunistring.so.5" ] \
        && [ -f "$SYSROOT_EXT_INC/gnutls/gnutls.h" ]
}

if idempotent_done; then
    log "GnuTLS 链已就绪，跳过"
    # 幂等跳过时补复制 .pc 到架构 pkgconfig 目录: SYSROOT_EXT_PC 按架构隔离后,
    # 跨架构切换 (或共享目录 .pc 被清) 时 stage_pc 不会重跑, 而 gstreamer/wine
    # configure 依赖这些 .pc → 缺 nettle.pc 报 "HLS crypto library not found"。
    if [ -d "$STAGING/lib/pkgconfig" ]; then
        cp "$STAGING"/lib/pkgconfig/*.pc "$SYSROOT_EXT_PC/" 2>/dev/null || true
    fi
    exit 0
fi

log "=== 构建 GnuTLS 链 (gmp/nettle/libtasn1/libunistring/gnutls, x86_64) → sysroot-ext ==="

mkdir -p "$SYSROOT_EXT_INC" "$SYSROOT_EXT_LIB" "$SYSROOT_EXT_PC" "$STAGING/include" "$STAGING/lib" "$STAGING/lib/pkgconfig"

CROSS_CFLAGS="-O2 -fPIC -D__MUSL__"
CROSS_LDFLAGS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET"
export PKG_CONFIG_PATH="$STAGING/lib/pkgconfig"
export CFLAGS="-I$STAGING/include $CROSS_CFLAGS"
export LDFLAGS="-L$STAGING/lib $CROSS_LDFLAGS"
export CC="$CLANG --target=$TARGET --sysroot=$SYSROOT"

# 收集 staging 产物到 sysroot-ext (libtool .so 保留 SONAME 符号链接)
stage_libs() {
    cp -P "$STAGING"/lib/"$1".so* "$SYSROOT_EXT_LIB/"
}
stage_headers() {
    cp -r "$STAGING"/include/"$1" "$SYSROOT_EXT_INC/"
}
stage_pc() {
    cp "$STAGING"/lib/pkgconfig/"$1" "$SYSROOT_EXT_PC/" 2>/dev/null || true
}

build_one() {
    local name="$1" src="$2"; shift 2
    local build="$BUILD_DIR/${name}_build"
    log "--- 构建 $name ---"
    rm -rf "$build"
    mkdir -p "$build"
    cd "$build"
    CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
    "$src/configure" --host=$GNU_HOST --prefix="$STAGING" \
        --libdir="$STAGING/lib" --disable-static \
        "$@"
    # 阻止 automake 因 NFS clock skew (Makefile.in 比 Makefile.am 旧) 重新生成
    # → 缺 build-aux/mdate-sh 等辅助文件 → 构建失败
    find "$src" -name 'Makefile.in' -exec touch {} + 2>/dev/null || true
    touch "$src/Makefile.in" 2>/dev/null || true
    # tests/fuzz 是无条件 SUBDIRS: libtasn1 的 all 目标会运行交叉 asn1Parser
    # 生成头文件 (宿主无法执行 x86_64-ohos 二进制), libunistring 的 tests 编译
    # glibc 扩展宏 (musl 无 PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP)
    # → 按库剔除测试子目录; 再 touch Makefile 防止 automake 自动 regen
    # (源码 mtime 比 build Makefile 新时 config.status 会重新生成覆盖 sed)
    case "$name" in
        libtasn1)
            sed -i 's/^SUBDIRS = .*/SUBDIRS = lib src/' "$build/Makefile"
            touch "$build/Makefile"
            ;;
        libunistring)
            sed -i 's/^SUBDIRS = .*/SUBDIRS = doc gnulib-local lib/' "$build/Makefile"
            touch "$build/Makefile"
            ;;
        gnutls)
            # ASN.1 tab 生成需要 asn1Parser (交叉二进制, 宿主不可运行)。
            # release tarball 自带同版本 (3.8.3) 预生成 tab 文件 (mtime
            # 比 .asn 新) → 复制到 build 树, touch 保证比 .asn 新, 防止
            # make 用交叉 asn1Parser 重新生成。
            mkdir -p "$build/lib"
            for t in gnutls_asn1_tab.c pkix_asn1_tab.c; do
                if [ ! -f "$build/lib/$t" ] && [ -f "$src/lib/$t" ]; then
                    cp "$src/lib/$t" "$build/lib/$t"
                fi
                [ -f "$build/lib/$t" ] && touch "$build/lib/$t"
            done
            # inih (第三方 INI 解析) 无 config.h include, gnulib 替换头
            # (getdelim/getline 被替换声明) 要求 config.h 先行 → 复制到
            # build 树首行插入 (automake VPATH 优先 build 树文件, 不污染 submodule)
            # ini.h 必须同放 (build 树 #include "ini.h" 相对当前文件目录解析)
            if [ -f "$src/lib/inih/ini.c" ]; then
                mkdir -p "$build/lib/inih"
                cp "$src/lib/inih/ini.c" "$build/lib/inih/ini.c"
                cp "$src/lib/inih/ini.h" "$build/lib/inih/ini.h"
                sed -i '1i #include "config.h"' "$build/lib/inih/ini.c"
                touch "$build/lib/inih/ini.c" "$build/lib/inih/ini.h"
            fi
            ;;
    esac
    make -j$JOBS
    make install
    cd "$SCRIPT_DIR"
}

# ── 1. gmp (nettle 的 bignum 后端) ──
build_one gmp "$TARBALL_DIR/gmp" \
    --disable-assembly --enable-cxx=no --disable-dependency-tracking
stage_libs libgmp
stage_headers gmp.h
stage_pc gmp.pc

# ── 2. libtasn1 (gnutls 的 ASN.1 解析) ──
build_one libtasn1 "$TARBALL_DIR/libtasn1" \
    --disable-doc --disable-dependency-tracking --disable-tests
stage_libs libtasn1
stage_headers libtasn1.h
stage_pc libtasn1.pc

# ── 3. libunistring (gnutls 的字符串/IDN 依赖) ──
build_one libunistring "$TARBALL_DIR/libunistring" \
    --disable-dependency-tracking --without-libiconv-prefix
stage_libs libunistring
stage_headers unistring
stage_pc libunistring.pc

# ── 4. nettle (+hogweed, gnutls 的 crypto 后端) ──
build_one nettle "$TARBALL_DIR/nettle" \
    --disable-documentation --disable-openssl --disable-assembler \
    --disable-dependency-tracking
stage_libs libnettle
stage_libs libhogweed
stage_headers nettle
stage_pc nettle.pc
stage_pc hogweed.pc

# ── 5. gnutls (schannel 的 TLS 后端) ──
build_one gnutls "$TARBALL_DIR/gnutls" \
    --disable-doc --disable-tools --disable-tests --disable-full-test-suite --disable-gtk-doc --disable-cxx \
    --disable-guile --disable-valgrind-tests --disable-code-coverage \
    --without-p11-kit --without-tpm --without-brotli --without-zstd \
    --without-libpsl --without-idn --without-libidn2 --without-unbound \
    --without-libxml2 --disable-dependency-tracking
stage_libs libgnutls
stage_headers gnutls
stage_pc gnutls.pc

log "GnuTLS 链就绪: $SYSROOT_EXT"

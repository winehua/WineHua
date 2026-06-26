#!/usr/bin/env bash
# Legacy Wine for HarmonyOS build and package script (Phase 1: x86_64).
#
# Usage:
#   bash scripts/build_wine_ohos.sh [--clean] [--package]
#
# Outputs:
#   out/wine/      Assembled Wine runtime tree
#   out/wine.hnp   HNP package when hnpcli is available
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINE_SRC="$ROOT/thirdparty/wine"
OHOS_SDK="${OHOS_SDK:-/apps/harmony/sdk/default/openharmony}"
OHOS_ARCH="${OHOS_ARCH:-x86_64}"

CLANG="$OHOS_SDK/native/llvm/bin/clang"
SYSROOT="$OHOS_SDK/native/sysroot"
TARGET="${OHOS_ARCH}-linux-ohos"
NATIVE_BUILD="$WINE_SRC/build-native"
OHOS_BUILD="$WINE_SRC/build-ohos"
OUT_DIR="$ROOT/out/wine"

# Parse args
DO_CLEAN=0
DO_PACKAGE=0
for arg in "$@"; do
    case "$arg" in
        --clean) DO_CLEAN=1 ;;
        --package) DO_PACKAGE=1 ;;
        *) echo "Usage: $0 [--clean] [--package]"; exit 1 ;;
    esac
done

# OHOS build flags
CFLAGS_OHOS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables"

CC_OHOS="$CLANG --target=$TARGET --sysroot=$SYSROOT"
LDFLAGS_OHOS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET"

check_prereqs() {
    echo "==> Checking prerequisites..."
    for f in "$CLANG" "$SYSROOT/usr/lib/$TARGET/libc.so"; do
        [ -e "$f" ] || { echo "ERROR: $f not found"; exit 1; }
    done
    [ -d "$WINE_SRC" ] || { echo "ERROR: Wine source not found"; exit 1; }
    echo "    OK"
}

build_native() {
    echo "==> Building native Wine..."
    mkdir -p "$NATIVE_BUILD"
    cd "$NATIVE_BUILD"
    if [ ! -f Makefile ]; then
        "$WINE_SRC/configure" --enable-win64 --disable-tests \
            --without-x --without-freetype --without-alsa \
            --without-opengl --without-vulkan
    fi
    make -j"${JOBS:-$(nproc)}"
    echo "    Native build complete"
    cd "$ROOT"
}

configure_ohos() {
    echo "==> Configuring OHOS build ($TARGET)..."
    rm -rf "$OHOS_BUILD" && mkdir -p "$OHOS_BUILD"
    cd "$OHOS_BUILD"

    CC="gcc" "$WINE_SRC/configure" \
        --host="$TARGET" \
        --with-wine-tools="$NATIVE_BUILD" \
        --with-mingw=gcc \
        --disable-tests \
        --without-x --without-freetype --without-alsa \
        --without-opengl --without-vulkan --without-gstreamer \
        --without-pulse --without-oss --without-cups --without-fontconfig

    # Undef headers that are unavailable in the OHOS sysroot.
    for pair in \
        "HAVE_LINUX_NTSYNC_H" "HAVE_NETIPX_IPX_H" \
        "HAVE_LINUX_IRDA_H" "HAVE_LINUX_UCDROM_H" "HAVE_LINUX_CAPI_H"; do
        sed -i "s/#define $pair 1/\/\* OHOS: not available \*\/\n#undef $pair/" include/config.h 2>/dev/null || true
    done

    # Add a musl stub for epoll_pwait2 when unavailable.
    cat > "$WINE_SRC/server/musl_compat.c" << 'EOF'
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <errno.h>
__attribute__((weak))
int epoll_pwait2(int fd, struct epoll_event *ev, int n, const struct timespec *ts, const sigset_t *s)
{ (void)fd;(void)ev;(void)n;(void)ts;(void)s; errno=ENOSYS; return -1; }
EOF
    $CC_OHOS -c -o "$OHOS_BUILD/server/musl_compat.o" \
        "$WINE_SRC/server/musl_compat.c" \
        -I"$WINE_SRC/include" -I"$WINE_SRC/server"

    echo "    Configure done"
    cd "$ROOT"
}

build_ohos() {
    echo "==> Building OHOS components..."
    cd "$OHOS_BUILD"

    # Build Unix-side pieces. Keep going so PE-only targets do not stop the pass.
    make -k -j"${JOBS:-$(nproc)}" \
        CC="$CC_OHOS" \
        CFLAGS="$CFLAGS_OHOS" \
        LDFLAGS="$LDFLAGS_OHOS" \
        2>&1 | grep -E 'error:|Error' | head -20 || true

    # Manually link wineserver if the build did not produce it.
    if [ ! -f server/wineserver ]; then
        $CC_OHOS -fuse-ld=lld -o server/wineserver \
            server/*.o server/musl_compat.o -lm
    fi

    echo "    OHOS build done"
    cd "$ROOT"
}

assemble() {
    echo "==> Assembling Wine distribution..."
    rm -rf "$OUT_DIR"
    mkdir -p "$OUT_DIR/bin" "$OUT_DIR/lib/wine" "$OUT_DIR/share/wine"

    # OHOS-compiled components
    cp "$OHOS_BUILD/server/wineserver" "$OUT_DIR/bin/"
    cp "$OHOS_BUILD/dlls/ntdll/ntdll.so" "$OUT_DIR/lib/wine/"
    find "$OHOS_BUILD/dlls" -name "*.so" -not -path "*/ntdll.so" \
        -exec cp {} "$OUT_DIR/lib/wine/" \; 2>/dev/null || true

    # PE DLLs and programs from the native build
    find "$NATIVE_BUILD/dlls" -name "*.dll" -type f \
        -exec cp {} "$OUT_DIR/lib/wine/" \; 2>/dev/null || true
    find "$NATIVE_BUILD/programs" -name "*.exe" -type f \
        -exec cp {} "$OUT_DIR/bin/" \; 2>/dev/null || true

    echo "    Assembled: $OUT_DIR"
    echo "    bin: $(ls "$OUT_DIR/bin" | wc -l) files"
    echo "    lib: $(ls "$OUT_DIR/lib/wine" | wc -l) files"
}

package_hnp() {
    echo "==> Creating HNP package..."

    HNP_STAGING="$ROOT/out/hnp_staging"
    rm -rf "$HNP_STAGING"
    mkdir -p "$HNP_STAGING/opt/wine"
    cp -r "$OUT_DIR"/* "$HNP_STAGING/opt/wine/"

    cat > "$HNP_STAGING/hnp.json" << EOFHNP
{
    "type": "hnp-config",
    "name": "wine",
    "version": "10.0",
    "install": {
        "links": [
            { "source": "./opt/wine/bin/wineserver", "target": "wineserver" }
        ]
    }
}
EOFHNP

    HNPCLI=""
    for cand in "$OHOS_SDK/../toolchains/hnpcli" \
                "$OHOS_SDK/native/build-tools/hnpcli/bin/hnpcli" \
                "$(command -v hnpcli 2>/dev/null)" \
                "$(command -v hnp 2>/dev/null)"; do
        [ -n "$cand" ] && [ -x "$cand" ] && { HNPCLI="$cand"; break; }
    done

    if [ -n "$HNPCLI" ]; then
        "$HNPCLI" pack -i "$HNP_STAGING" -o "$ROOT/out" -n wine -v 10.0
        echo "    HNP: $ROOT/out/wine.hnp"
    else
        echo "    hnpcli not found, creating tar.gz instead"
        tar -czf "$ROOT/out/wine.tar.gz" -C "$HNP_STAGING" .
        echo "    Archive: $ROOT/out/wine.tar.gz"
    fi
}

echo "============================================"
echo " Wine for HarmonyOS Build Script"
echo " Target: $TARGET | SDK: $OHOS_SDK"
echo "============================================"
echo ""

check_prereqs

if [ ! -f "$NATIVE_BUILD/tools/winebuild/winebuild" ] || [ "$DO_CLEAN" = "1" ]; then
    build_native
else
    echo "==> Native build exists, skipping (use --clean to rebuild)"
fi

configure_ohos
build_ohos
assemble

if [ "$DO_PACKAGE" = "1" ]; then
    package_hnp
fi

echo ""
echo "============================================"
echo " Build complete!"
echo " Output: $OUT_DIR"
echo " Size:   $(du -sh "$OUT_DIR" | cut -f1)"
echo "============================================"

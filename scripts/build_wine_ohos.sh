#!/usr/bin/env bash
# Wine for HarmonyOS 鈥?Build & Package Script (Phase 1: x86_64)
#
# 鐢ㄦ硶:
#   bash scripts/build_wine_ohos.sh [--clean] [--package]
#
# 杈撳嚭:
#   out/wine/   鈥?瀹屾暣 Wine 鍙戣鐗?(bin + lib)
#   out/wine.hnp 鈥?HNP 瀹夎鍖?(闇€瑕?hnpcli)
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

# ================================================================
# Parse args
# ================================================================
DO_CLEAN=0; DO_PACKAGE=0
for arg in "$@"; do
    case "$arg" in
        --clean) DO_CLEAN=1 ;;
        --package) DO_PACKAGE=1 ;;
        *) echo "Usage: $0 [--clean] [--package]"; exit 1 ;;
    esac
done

# ================================================================
# OHOS Build Flags (verified working)
# ================================================================
CFLAGS_OHOS="-g -O2 -D__MUSL__ -D_GNU_SOURCE -DWINE_UNIX_LIB \
    -D_NTSYSTEM_ -D__WINESRC__ -DFAR= -D_ACRTIMP= -DWINBASEAPI= -DZ_SOLO \
    -fPIC -fasynchronous-unwind-tables"

CC_OHOS="$CLANG --target=$TARGET --sysroot=$SYSROOT"
LDFLAGS_OHOS="-fuse-ld=lld --sysroot=$SYSROOT --target=$TARGET"

# ================================================================
# Step 1: Check prerequisites
# ================================================================
check_prereqs() {
    echo "==> Checking prerequisites..."
    for f in "$CLANG" "$SYSROOT/usr/lib/$TARGET/libc.so"; do
        [ -e "$f" ] || { echo "ERROR: $f not found"; exit 1; }
    done
    [ -d "$WINE_SRC" ] || { echo "ERROR: Wine source not found"; exit 1; }
    echo "    OK"
}

# ================================================================
# Step 2: Build native Wine (tools + PE DLLs)
# ================================================================
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

# ================================================================
# Step 3: Configure OHOS build
# ================================================================
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

    # Fix config.h: undef headers missing from OHOS sysroot
    for pair in \
        "HAVE_LINUX_NTSYNC_H" "HAVE_NETIPX_IPX_H" \
        "HAVE_LINUX_IRDA_H" "HAVE_LINUX_UCDROM_H" "HAVE_LINUX_CAPI_H"; do
        sed -i "s/#define $pair 1/\/\* OHOS: not available \*\/\n#undef $pair/" include/config.h 2>/dev/null || true
    done

    # Create musl_compat.c (epoll_pwait2 stub)
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

# ================================================================
# Step 4: Build OHOS Unix components
# ================================================================
build_ohos() {
    echo "==> Building OHOS components..."
    cd "$OHOS_BUILD"

    # Build all Unix .so files (continue on error for PE-only targets)
    make -k -j"${JOBS:-$(nproc)}" \
        CC="$CC_OHOS" \
        CFLAGS="$CFLAGS_OHOS" \
        LDFLAGS="$LDFLAGS_OHOS" \
        2>&1 | grep -E 'error:|Error' | head -20 || true

    # Manually link wineserver (need musl_compat.o)
    if [ ! -f server/wineserver ]; then
        $CC_OHOS -fuse-ld=lld -o server/wineserver \
            server/*.o server/musl_compat.o -lm
    fi

    echo "    OHOS build done"
    cd "$ROOT"
}

# ================================================================
# Step 5: Assemble Wine directory
# ================================================================
assemble() {
    echo "==> Assembling Wine distribution..."
    rm -rf "$OUT_DIR"
    mkdir -p "$OUT_DIR/bin" "$OUT_DIR/lib/wine" "$OUT_DIR/share/wine"

    # OHOS-compiled components
    cp "$OHOS_BUILD/server/wineserver" "$OUT_DIR/bin/"
    cp "$OHOS_BUILD/dlls/ntdll/ntdll.so" "$OUT_DIR/lib/wine/"
    find "$OHOS_BUILD/dlls" -name "*.so" -not -path "*/ntdll.so" \
        -exec cp {} "$OUT_DIR/lib/wine/" \; 2>/dev/null || true

    # PE DLLs from native build
    find "$NATIVE_BUILD/dlls" -name "*.dll" -type f \
        -exec cp {} "$OUT_DIR/lib/wine/" \; 2>/dev/null || true
    find "$NATIVE_BUILD/programs" -name "*.exe" -type f \
        -exec cp {} "$OUT_DIR/bin/" \; 2>/dev/null || true

    echo "    Assembled: $OUT_DIR"
    echo "    bin: $(ls $OUT_DIR/bin | wc -l) files"
    echo "    lib: $(ls $OUT_DIR/lib/wine | wc -l) files"
}

# ================================================================
# Step 6: HNP packaging
# ================================================================
package_hnp() {
    echo "==> Creating HNP package..."

    HNP_STAGING="$ROOT/out/hnp_staging"
    rm -rf "$HNP_STAGING"
    mkdir -p "$HNP_STAGING/opt/wine"
    cp -r "$OUT_DIR"/* "$HNP_STAGING/opt/wine/"

    # hnp.json
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

    # Pack
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

# ================================================================
# Main
# ================================================================
echo "============================================"
echo " Wine for HarmonyOS 鈥?Build Script"
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
echo " Size:   $(du -sh $OUT_DIR | cut -f1)"
echo "============================================"

#!/bin/bash
# Build only Wine's self-contained ICU PE component, with no OS-private ICU ABI.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"
ICU_SOURCE_PIN=dc5204ecb0c3c1bf6624542ecf42f931fcc1a3b0
ICU_ARCHIVE_SHA=cdb76defc6fb193446dbb09d67f818cd019712dc2ed1f6d5538f9d2e733598a8
component="$BUILD_DIR/icu-compat"
mkdir -p "$component"
archive="${ICU_SOURCE_ARCHIVE:-$component/wine-source.tar}"
if [ ! -f "$archive" ]; then
    [ -z "${ICU_SOURCE_ARCHIVE:-}" ] || err 'Explicit ICU source archive is missing'
    repository="${ICU_SOURCE_REPO:-$ROOT/thirdparty/wine}"
    if ! git -C "$repository" cat-file -e "$ICU_SOURCE_PIN^{commit}" 2>/dev/null; then
        repository="$component/source-cache"
        if [ ! -d "$repository/.git" ]; then git init -q "$repository"; fi
        log 'Fetching the pinned Wine ICU source (offline builds may set ICU_SOURCE_ARCHIVE or ICU_SOURCE_REPO)'
        git -C "$repository" fetch --depth=1 https://github.com/winehua/wine.git "$ICU_SOURCE_PIN" ||
            err 'Cannot obtain pinned ICU source; provide the verified git archive via ICU_SOURCE_ARCHIVE'
    fi
    git -C "$repository" archive --format=tar "$ICU_SOURCE_PIN" -o "$archive.pending"
    test "$(sha256sum "$archive.pending" | cut -d ' ' -f1)" = "$ICU_ARCHIVE_SHA" || err 'ICU source archive identity mismatch'
    mv "$archive.pending" "$archive"
fi
test "$(sha256sum "$archive" | cut -d ' ' -f1)" = "$ICU_ARCHIVE_SHA" || err 'ICU source archive identity mismatch'
# Keep a verified cache so the following incremental/offline build needs no
# local reference checkout or caller-specific workspace_temp path.
if [ "$archive" != "$component/wine-source.tar" ]; then
    cp "$archive" "$component/wine-source.tar.pending"
    mv "$component/wine-source.tar.pending" "$component/wine-source.tar"
fi
src="$component/source"
tools="$component/tools-build"
# Compiler-generated ARM64EC memory libcalls lack an x64 exit thunk. Keep
# their declared import types, so an application's x64 UCRT can be used too.
target="$component/pe-build-crt-safe"
mkdir -p "$component"
if [ ! -f "$src/.source-pin" ]; then
    test ! -e "$src" || err 'Unmarked ICU source exists; use a new component build directory'
    mkdir -p "$src"
    tar -xf "$archive" -C "$src"
    printf '%s\n' "$ICU_SOURCE_PIN" > "$src/.source-pin"
fi
test "$(cat "$src/.source-pin")" = "$ICU_SOURCE_PIN" || err 'ICU source pin mismatch'
mkdir -p "$tools" "$target"
if [ ! -f "$tools/Makefile" ]; then
    (cd "$tools"; CC=gcc CXX=g++ "$src/configure" --enable-win64 --disable-tests \
        --without-x --without-freetype --without-gnutls --without-opengl --without-vulkan --without-alsa)
fi
make -C "$tools" -j"$JOBS" tools/winebuild/winebuild tools/winegcc/winegcc tools/wrc/wrc tools/widl/widl tools/wmc/wmc
if [ ! -f "$target/Makefile" ]; then
    (cd "$target"; CC="$OHOS_SDK/native/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$SYSROOT" \
      CXX="$OHOS_SDK/native/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$SYSROOT" \
      CFLAGS='-O2 -D__MUSL__ -D_GNU_SOURCE -D__ANDROID__ -D__OHOS__ -DWINE_UNIX_LIB -D_NTSYSTEM_ -D__WINESRC__' \
      LDFLAGS="-fuse-ld=lld --target=aarch64-linux-ohos --sysroot=$SYSROOT" \
      CROSSCFLAGS='-O2 -g0 -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-memset' "$src/configure" --host=aarch64-unknown-linux-ohos \
      --enable-archs=arm64ec,aarch64,i386 --with-wine-tools="$tools" --with-mingw="$LLVM_MINGW/bin/clang" \
      --disable-tests --without-x --without-alsa --without-freetype --without-gnutls --without-opengl --without-vulkan)
fi
# A directory name or output marker cannot prove how a reused build was
# configured. Reject stale compiler options before certifying the component.
for arch in arm64ec aarch64 i386; do
    flags="$(sed -n "s/^${arch}_CFLAGS = //p" "$target/Makefile")"
    for flag in -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-memset; do
        [[ " $flags " == *" $flag "* ]] || err "ICU $arch build lacks $flag; use a newly configured component build"
    done
done
make -C "$target" -j"$JOBS" dlls/icu/all dlls/icuuc/all dlls/icuin/all
output="${ICU_COMPAT_OUTPUT:-$component/output}"
mkdir -p "$output/aarch64-windows" "$output/i386-windows" "$output/nls"
for arch in aarch64-windows i386-windows; do
    for module in icu icuuc icuin; do
        cp "$target/dlls/$module/$arch/$module.dll" "$output/$arch/"
        "$LLVM_MINGW/bin/llvm-strip" --strip-debug "$output/$arch/$module.dll"
    done
done
cp "$src/nls/icudtl.dat" "$output/nls/"
printf '%s\n' "$ICU_SOURCE_PIN" > "$output/source-pin"
printf '%s\n' typed-arm64ec-imports-v1 > "$output/memory-call-policy"
python3 "$SCRIPT_DIR/icu_compat_runtime.py" check --output "$output"
log 'Self-contained ICU PE modules built for ARM64X/x64 and i386'

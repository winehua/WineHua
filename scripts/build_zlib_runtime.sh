#!/bin/bash
# Real OHOS libz: a required guest runtime, never an SDK linker stub.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"
guest_arch="${GUEST_ARCH:-$WINE_ARCH}"
case "$guest_arch" in aarch64|x86_64) ;; *) err "unsupported zlib guest arch: $guest_arch" ;; esac
archive="${ZLIB_SOURCE_ARCHIVE:-$BUILD_DIR/downloads/zlib-1.3.1.tar.gz}"
# Same upstream archive pin as thirdparty/mesa/subprojects/zlib.wrap.
expected=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
mkdir -p "$(dirname "$archive")" "$BUILD_DIR/zlib-runtime/$guest_arch"
if [ ! -f "$archive" ]; then
    curl -fL --connect-timeout 20 --max-time 60 \
        https://zlib.net/fossils/zlib-1.3.1.tar.gz -o "$archive.part"
    printf '%s  %s\n' "$expected" "$archive.part" | sha256sum -c -
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
output="$BUILD_DIR/zlib-runtime/$guest_arch/lib"
identity="$(printf '%s\n' "$expected" "$guest_arch" "$CLANG" "$SYSROOT" \
    "$(sha256sum "$SCRIPT_DIR/build_zlib_runtime.sh" "$CLANG")" | sha256sum | cut -d' ' -f1)"
identity_file="$BUILD_DIR/zlib-runtime/$guest_arch/build-identity.sha256"
checksum_file="$BUILD_DIR/zlib-runtime/$guest_arch/output.sha256"
if [ -s "$output/libz.so" ] && [ "$(cat "$identity_file" 2>/dev/null || true)" = "$identity" ] && \
    [ -f "$checksum_file" ] && (cd "$output" && sha256sum -c "$checksum_file") && \
    python3 "$SCRIPT_DIR/runtime_library_guard.py" "$output/libz.so"; then
    log "Real zlib runtime: verified existing $output/libz.so"
    exit 0
fi
work="$BUILD_DIR/zlib-runtime/$guest_arch/source"
mkdir -p "$work"
tar -xzf "$archive" -C "$work" --strip-components=1
sources=(adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil)
inputs=()
for unit in "${sources[@]}"; do inputs+=("$work/$unit.c"); done
mkdir -p "$output"
"$CLANG" --target="$guest_arch-linux-ohos" --sysroot="$SYSROOT" \
    -O2 -fPIC -DHAVE_UNISTD_H -DHAVE_HIDDEN -D_LARGEFILE64_SOURCE=1 \
    -shared -fuse-ld=lld -Wl,--no-undefined -Wl,-soname,libz.so \
    -I "$work" "${inputs[@]}" -o "$work/libz.so"
python3 "$SCRIPT_DIR/runtime_library_guard.py" "$work/libz.so"
mv "$work/libz.so" "$output/libz.so"
cp "$work/README" "$BUILD_DIR/zlib-runtime/$guest_arch/README.upstream"
printf 'zlib=1.3.1\nsource_sha256=%s\ntarget=%s-linux-ohos\n' "$expected" "$guest_arch" \
    > "$BUILD_DIR/zlib-runtime/$guest_arch/build-info.txt"
sha256sum "$output/libz.so" >> "$BUILD_DIR/zlib-runtime/$guest_arch/build-info.txt"
(cd "$output" && sha256sum libz.so) > "$checksum_file"
printf '%s\n' "$identity" > "$identity_file"
log "Real zlib runtime: $output/libz.so (full deflate/inflate/gzip APIs)"

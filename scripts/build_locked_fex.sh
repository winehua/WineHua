#!/bin/bash
# Build the locked ARM64EC/WOW64 pair and their matching OHOS Unix bridge.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"
[ "$WINE_ARCH" = aarch64 ] || { log 'Native x86_64 Wine does not require FEX'; exit 0; }
FEX_SRC="${FEX_SRC:-$ROOT/build/runtime-sources/fex}"
python3 "$SCRIPT_DIR/prepare_runtime_sources.py" --component fex --source "$FEX_SRC" --verify-only
FEX_SOURCE_HASH="$(git -C "$FEX_SRC" rev-parse HEAD)"
export FEX_SOURCE_HASH PATH="$LLVM_MINGW/bin:$PATH"

# Stable production optimization; exact-normal stores on WOW64 only. Strict
# PC24 multiplication and tracing experiments are not production build flags.
for kind in ec pe; do
    build="$BUILD_DIR/fex-$kind"
    triple=arm64ec-w64-mingw32
    target=arm64ecfex
    flags=''
    if [ "$kind" = pe ]; then
        triple=aarch64-w64-mingw32
        target=wow64fex
        flags='-DFEX_WOW64_EXACT_STORE=1'
    fi
    cmake -S "$FEX_SRC" -B "$build" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_TOOLCHAIN_FILE="$FEX_SRC/Data/CMake/toolchain_mingw.cmake" \
        -DCMAKE_CXX_FLAGS="$flags" -DENABLE_LTO=False -DBUILD_TESTING=False \
        -DMINGW_TRIPLE="$triple" -DOVERRIDE_HASH="$FEX_SOURCE_HASH" \
        -DOVERRIDE_VERSION="ohos-upstream-${FEX_SOURCE_HASH:0:10}"
    cmake --build "$build" --parallel "$JOBS" --target "$target"
done
mkdir -p "$NATIVE_LIBS"
"$OHOS_SDK/native/llvm/bin/clang++" --target=aarch64-linux-ohos \
    --sysroot="$SYSROOT" -std=c++20 -O2 -DNDEBUG -fPIC -shared -Wl,--no-undefined \
    "$FEX_SRC/Source/Windows/UnixLib/FEXUnixLib.cpp" -o "$NATIVE_LIBS/libarm64ecfex.so"
cp "$NATIVE_LIBS/libarm64ecfex.so" "$NATIVE_LIBS/libwow64fex.so"
python3 "$SCRIPT_DIR/fex_simd_runtime.py" record --build "$BUILD_DIR" \
    --patch "$SCRIPT_DIR/patches/fex-simd-zero.patch" --source-hash "$FEX_SOURCE_HASH"
log 'Locked FEX ARM64EC/WOW64 and Unix bridges built'

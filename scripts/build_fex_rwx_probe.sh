#!/usr/bin/env bash
# Isolated diagnostic build. Never invokes build_fex.sh's patch staging.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
export PATH="$LLVM_MINGW/bin:$PATH"
probe_root="$ROOT/build/fex-rwx-probe"
probe_out="$ROOT/artifacts/fex-rwx-probe"
mkdir -p "$probe_out"

# The deployed original is the stripped form of this existing build, not a
# pre-ThreadTerm binary. Fail closed if that proven baseline has changed.
cp "$ROOT/build/fex-ec/Bin/libarm64ecfex.dll" "$probe_out/staged-stripped.dll"
"$LLVM_MINGW/bin/llvm-strip" "$probe_out/staged-stripped.dll"
test "$(sha256sum "$probe_out/staged-stripped.dll" | cut -d ' ' -f 1)" = \
  8aba586e7988b01cd1d24685bef778ed71f64da9dc3bf525bd720ef6596f0cfc
cmp "$probe_out/staged-stripped.dll" "$ROOT/artifacts/steam-font-contract/libarm64ecfex-original.dll"

if [ ! -d "$probe_root/source" ]; then
    mkdir -p "$probe_root"
    cp -a --reflink=auto "$ROOT/build/fex-src" "$probe_root/source"
fi
probe_patch="$ROOT/scripts/patches/fex-arm64ec-rwx-probe.patch"
if ! patch -d "$probe_root/source" -p1 -R --dry-run -s < "$probe_patch" >/dev/null 2>&1; then
    patch -d "$probe_root/source" -p1 --forward --batch < "$probe_patch"
fi

cmake -S "$probe_root/source" -B "$probe_root/ec" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$probe_root/source/Data/CMake/toolchain_mingw.cmake" \
  -DENABLE_LTO=False -DMINGW_TRIPLE=arm64ec-w64-mingw32 -DBUILD_TESTING=False
cmake --build "$probe_root/ec" --target arm64ecfex -j "${JOBS:-8}"
cp "$probe_root/ec/Bin/libarm64ecfex.dll" "$probe_out/libarm64ecfex-rwx-probe.dll"
"$LLVM_MINGW/bin/llvm-strip" "$probe_out/libarm64ecfex-rwx-probe.dll"
"$LLVM_MINGW/bin/llvm-readobj" --file-headers "$probe_out/libarm64ecfex-rwx-probe.dll" | head -25
sha256sum "$probe_out/libarm64ecfex-rwx-probe.dll"

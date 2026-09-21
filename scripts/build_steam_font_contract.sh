#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:-/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64}"
OUT="$ROOT/artifacts/steam-font-contract"
mkdir -p "$OUT"
for entry in 'i686-w64-mingw32 I386 i386' 'x86_64-w64-mingw32 AMD64 amd64'; do
    read -r triple machine name <<< "$entry"
    "$LLVM_MINGW/bin/$triple-clang++" -std=c++17 -O2 -Wall -Wextra -static \
        "$ROOT/tools/steam-boundary/font-contract.cpp" -ldwrite -lgdi32 -luser32 -lole32 -luuid \
        -o "$OUT/font-contract-$name.exe"
    "$LLVM_MINGW/bin/llvm-readobj" --file-headers "$OUT/font-contract-$name.exe" | \
        grep -q "Machine: IMAGE_FILE_MACHINE_$machine"
    printf '%s PE Machine=%s\n' "$OUT/font-contract-$name.exe" "$machine"
done

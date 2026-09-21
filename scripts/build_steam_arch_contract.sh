#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:-/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64}"
OUT="$ROOT/artifacts/steam-arch-contract"
mkdir -p "$OUT"

for entry in 'i686-w64-mingw32 I386 i386' 'x86_64-w64-mingw32 AMD64 amd64'; do
    read -r triple machine name <<< "$entry"
    "$LLVM_MINGW/bin/$triple-gcc" -O2 -Wall -Wextra -o "$OUT/contract-$name.exe" \
        "$ROOT/tools/steam-boundary/contract.c"
    "$LLVM_MINGW/bin/llvm-readobj" --file-headers "$OUT/contract-$name.exe" | \
        grep -q "Machine: IMAGE_FILE_MACHINE_$machine"
    printf '%s PE Machine=%s\n' "$OUT/contract-$name.exe" "$machine"
done

#!/bin/bash
# P0-2A: build the x86 and x64 Windows architecture probes (no Wine behaviour change).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

LLVM_MINGW="${LLVM_MINGW:-/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64}"
SRC="$ROOT/tools/arch-probe/arch_probe.c"
OUT="$ROOT/artifacts/arch-probe"

mkdir -p "$OUT"

echo "== build arch_probe_x86.exe =="
"$LLVM_MINGW/bin/i686-w64-mingw32-gcc" -O2 -Wall -o "$OUT/arch_probe_x86.exe" "$SRC"

echo "== build arch_probe_x64.exe =="
"$LLVM_MINGW/bin/x86_64-w64-mingw32-gcc" -O2 -Wall -o "$OUT/arch_probe_x64.exe" "$SRC"

ls -l "$OUT"
file "$OUT"/*.exe 2>/dev/null || true

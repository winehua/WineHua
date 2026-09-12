#!/bin/bash
# 构建带 --bench 的 smoke cube (x86 / amd64 / 原生 ARM64 三份)。
#
# 背景: smoke/winehua_d3d_switch_cube.c 默认每帧 Sleep(1), 把渲染循环节流在
# ~78fps, 无法用于 CPU 后端对比。--bench 跳过该节流并输出帧时间分布
# (avg/p50/p95/min/max)。见 docs/proton-parity/p3-p4-smoke-ab.md §4。
#
# 只需要 llvm-mingw (含 i686/x86_64/aarch64 三个 target), 不需要 Docker。
#   LLVM_MINGW=/path/to/llvm-mingw ./build_bench_cubes.sh [outdir]
set -euo pipefail

LLVM_MINGW="${LLVM_MINGW:-/home/liufeng/src/WineHua-arm64ec/.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64}"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SOURCE="$REPO_ROOT/smoke/winehua_d3d_switch_cube.c"
OUT_DIR="${1:-/tmp/bench-cubes}"
LIBS=(-ld3d9 -ld3d11 -ldxgi -ld3dcompiler -luuid -lshell32 -luser32 -lgdi32 -lm)

test -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" || { echo "missing llvm-mingw: $LLVM_MINGW" >&2; exit 1; }
mkdir -p "$OUT_DIR"

"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" -O2 -s -mwindows \
    -o "$OUT_DIR/winehua_d3d_switch_cube.amd64.exe" "$SOURCE" "${LIBS[@]}"
"$LLVM_MINGW/bin/i686-w64-mingw32-clang" -O2 -s -mwindows \
    -o "$OUT_DIR/winehua_d3d_switch_cube.x86.exe" "$SOURCE" "${LIBS[@]}"
"$LLVM_MINGW/bin/aarch64-w64-mingw32-clang" -O2 -s -mwindows \
    -o "$OUT_DIR/winehua_d3d_switch_cube.aarch64.exe" "$SOURCE" "${LIBS[@]}"

for f in "$OUT_DIR"/winehua_d3d_switch_cube.*.exe; do
    printf '%s: ' "$(basename "$f")"
    "$LLVM_MINGW/bin/llvm-readobj" --file-headers "$f" | grep -m1 'Format:'
done

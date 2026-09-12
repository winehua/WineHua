#!/bin/bash
# W1 / M2：用我们新编的 Proton-Wine-OHOS 候选跑 assemble.sh，产出运行时包 wine-data.zip。
# 日志落 $BUILD_DIR/w1-m2-assemble.log。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export LLVM_MINGW="${LLVM_MINGW:-/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64}"
export WINE_SRC="${WINE_SRC:-$ROOT/thirdparty/wine-valve}"
export BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
export NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"

if [ -z "${WAYLAND_SCANNER:-}" ] && [ -x "$BUILD_DIR/host-tools/bin/wayland-scanner" ]; then
    export WAYLAND_SCANNER="$BUILD_DIR/host-tools/bin/wayland-scanner"
fi

log="$BUILD_DIR/w1-m2-assemble.log"
echo "== assemble.sh -> $log =="
( cd "$ROOT" && NATIVE_ARCH="$NATIVE_ARCH" bash scripts/assemble.sh ) 2>&1 | tee "$log"
echo "assemble rc=${PIPESTATUS[0]}"

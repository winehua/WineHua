#!/bin/bash
# w1-m4-build-merged.sh — 用 Proton 11.0 + OHOS port 树走完整 Wine 交叉编译。
# 默认源码是受支持的 thirdparty/wine-valve；WINE_SRC 仍可显式覆盖。
#
# 用法（容器内，工作目录 = 仓库根）：
#   scripts/w1-m4-build-merged.sh          # 完整构建
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

echo "== Proton-OHOS Wine build =="
echo "WINE_SRC  = $WINE_SRC"
echo "BUILD_DIR = $BUILD_DIR"
echo "WAYLAND_SCANNER = ${WAYLAND_SCANNER:-<unset>}"

log="$BUILD_DIR/w1-m4-merged-build.log"
mkdir -p "$BUILD_DIR"
echo "== build_wine.sh -> $log =="
( cd "$ROOT" && bash scripts/build_wine.sh ) 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
echo "build_wine.sh rc=$rc"
exit "$rc"

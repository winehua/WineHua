#!/bin/bash
# w1-m4-build-merged.sh — 用 Proton-Wine-OHOS 合并树（winehua 11.10 + Proton proton_11.0）
# 走我们自己的 build_wine.sh 全套交叉编译。
#
# 与 w1-m1-probe.sh 的区别：WINE_SRC 指向 thirdparty/wine-proton（合并树），
# 日志落 build/w1-m4-merged-build.log。
#
# 用法（容器内，工作目录 = 仓库根）：
#   scripts/w1-m4-build-merged.sh          # 完整构建
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export LLVM_MINGW="${LLVM_MINGW:-/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64}"
export WINE_SRC="${WINE_SRC:-$ROOT/thirdparty/wine-proton}"
export BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
export NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"

if [ -z "${WAYLAND_SCANNER:-}" ] && [ -x "$BUILD_DIR/host-tools/bin/wayland-scanner" ]; then
    export WAYLAND_SCANNER="$BUILD_DIR/host-tools/bin/wayland-scanner"
fi

echo "== merged build =="
echo "WINE_SRC  = $WINE_SRC"
echo "BUILD_DIR = $BUILD_DIR"
echo "WAYLAND_SCANNER = ${WAYLAND_SCANNER:-<unset>}"

log="$BUILD_DIR/w1-m4-merged-build.log"
mkdir -p "$BUILD_DIR"
echo "== build_wine.sh -> $log =="
( cd "$ROOT" && bash scripts/build_wine.sh ) 2>&1 | tee "$log"
echo "build_wine.sh rc=${PIPESTATUS[0]}"

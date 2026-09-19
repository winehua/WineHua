#!/bin/bash
# W1 / M2：用我们新编的 Proton-Wine-OHOS 候选跑 assemble.sh，产出运行时包 wine-data.zip。
# 日志落 $BUILD_DIR/w1-m2-assemble.log。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
# Docker builds place llvm-mingw under /data/llvm.  Outside that environment,
# leave LLVM_MINGW unset so env.sh can select the checked-in .temp toolchain.
if [ -z "${LLVM_MINGW:-}" ] && \
   [ -x /data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/bin/arm64ec-w64-mingw32-clang ]; then
    export LLVM_MINGW=/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64
fi
export WINE_SRC="${WINE_SRC:-$ROOT/thirdparty/wine-valve}"
export BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
export NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"
# Match the ARM64 direct-game package. Wine Mono's first-launch installer is
# interactive and previously blocked wineboot on device; opt in explicitly.
export BUILD_WINE_MONO="${BUILD_WINE_MONO:-0}"

if [ -z "${WAYLAND_SCANNER:-}" ] && [ -x "$BUILD_DIR/host-tools/bin/wayland-scanner" ]; then
    export WAYLAND_SCANNER="$BUILD_DIR/host-tools/bin/wayland-scanner"
fi

log="$BUILD_DIR/w1-m2-assemble.log"
echo "== assemble.sh -> $log =="
( cd "$ROOT" && NATIVE_ARCH="$NATIVE_ARCH" bash scripts/assemble.sh ) 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
echo "assemble rc=$rc"
exit "$rc"

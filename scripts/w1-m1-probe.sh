#!/bin/bash
# w1-m1-probe.sh — W1 / M1 探针
#
# 目的：用**我们自己的构建环境**（build_wine.sh + OHOS SDK + llvm-mingw + sysroot-ext）
#       去配置/编译 **Valve Proton Wine** 源码，把第一批构建失败点记录下来。
#
# 用法（在容器内，工作目录 = 仓库根）：
#   scripts/w1-m1-probe.sh env        # 只打印解析后的环境
#   scripts/w1-m1-probe.sh autoconf   # 只用 Valve 的 configure.ac 生成 configure
#   scripts/w1-m1-probe.sh full       # 跑完整 scripts/build_wine.sh，日志落 $BUILD_DIR/w1-m1-build.log
#
# 默认把 WINE_SRC 指向 thirdparty/wine-valve（Valve dc26e618 的独立检出），
# 不碰 thirdparty/wine（产品/parity 使用的 winehua 检出）。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export LLVM_MINGW="${LLVM_MINGW:-/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64}"
export WINE_SRC="${WINE_SRC:-$ROOT/thirdparty/wine-valve}"
export BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
export NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"

# wayland-scanner：env.sh 默认指向 /usr/local/bin/wayland-scanner（容器里没有），
# 我们仓库自带一份在 $BUILD_DIR/host-tools/bin/。必须在 configure 之前指定，
# 因为 configure 会把这个路径写进生成的 Makefile。
if [ -z "${WAYLAND_SCANNER:-}" ] && [ -x "$BUILD_DIR/host-tools/bin/wayland-scanner" ]; then
    export WAYLAND_SCANNER="$BUILD_DIR/host-tools/bin/wayland-scanner"
fi

# shellcheck disable=SC1091
source "$SCRIPT_DIR/env.sh"

echo "== resolved environment =="
echo "ROOT        = $ROOT"
echo "WINE_SRC    = $WINE_SRC"
echo "BUILD_DIR   = $BUILD_DIR"
echo "NATIVE_ARCH = $NATIVE_ARCH"
echo "WINE_ARCH   = ${WINE_ARCH:-<unset>}"
echo "HOST_TRIPLE = ${HOST_TRIPLE:-<unset>}"
echo "TARGET      = ${TARGET:-<unset>}"
echo "LLVM_MINGW  = $LLVM_MINGW"
echo "OHOS_SDK    = ${OHOS_SDK:-<unset>}"

step="${1:-env}"

case "$step" in
env)
    ;;

autoconf)
    echo "== autoconf (Valve configure.ac) =="
    test -f "$WINE_SRC/configure.ac" || { echo "FATAL: no $WINE_SRC/configure.ac"; exit 2; }
    ( cd "$BUILD_DIR" && autoconf -I "$WINE_SRC" -o "$BUILD_DIR/configure" "$WINE_SRC/configure.ac" )
    rc=$?
    echo "autoconf rc=$rc"
    [ -x "$BUILD_DIR/configure" ] && ls -la "$BUILD_DIR/configure"
    ;;

full)
    log="$BUILD_DIR/w1-m1-build.log"
    echo "== full build_wine.sh -> $log =="
    ( cd "$ROOT" && bash scripts/build_wine.sh ) 2>&1 | tee "$log"
    echo "build_wine.sh rc=${PIPESTATUS[0]}"
    ;;

*)
    echo "usage: $0 [env|autoconf|full]" >&2
    exit 2
    ;;
esac

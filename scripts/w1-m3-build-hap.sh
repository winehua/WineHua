#!/bin/bash
# W1 / M3：把 Proton-Wine-OHOS 候选打成 HAP（容器内执行）。
#
# 为什么必须走 HAP：app 只在「HAP 内 wine-runtime-manifest.json 文本 ≠ 已解压的
# files/wine/.winehua-runtime-manifest.json 文本」时才从 **HAP rawfile** 重新解压，
# 而设备上没有 unzip，没法手工铺 1GB 的运行时目录。装带新 rawfile 的 HAP 是最干净的路径。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

export TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
export LLVM_MINGW="${LLVM_MINGW:-/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64}"
export NATIVE_ARCH="${NATIVE_ARCH:-arm64-v8a}"

log="$ROOT/build/w1-m3-hap.log"
echo "== package.sh hap -> $log =="
( cd "$ROOT" && NATIVE_ARCH="$NATIVE_ARCH" bash scripts/package.sh hap ) 2>&1 | tee "$log"
echo "package rc=${PIPESTATUS[0]}"
ls -la "$ROOT/entry/build/default/outputs/default/" 2>/dev/null | tail -5

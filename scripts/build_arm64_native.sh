#!/bin/bash
# build_arm64_native.sh — 兼容性包装器, 委托给 build_native.sh
# 已由 build_native.sh 替代, 保留此文件用于向后兼容
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export NATIVE_ARCH=arm64-v8a
exec bash "$SCRIPT_DIR/build_native.sh"

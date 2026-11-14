#!/bin/bash
# build_hgs.sh — hgs 构建上层封装
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"

cd "$SCRIPT_DIR/../hgs"
bash scripts/build_all.sh
cd "$SCRIPT_DIR"

# 后续考虑将 hgs 的 sysroot-ext 复制到外层 winehua 中

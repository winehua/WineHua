#!/bin/bash
# W1 调试用：复现 winebuild 的 --staticlib 失败，并打开 Wine 调试通道看它到底 spawn 了什么。
# 用法（容器内）：bash scripts/w1-debug-winebuild.sh /tmp/wb_cmd.txt
set -uo pipefail
build_dir="${BUILD_DIR:-/data/src/winehua/build}"
cmd_file="${1:-/tmp/wb_cmd.txt}"

if [ ! -f "$cmd_file" ]; then
    echo "usage: $0 <file containing the failing winebuild command line>" >&2
    exit 2
fi

cmd="$(cat "$cmd_file")"
echo "== command =="
echo "$cmd"
echo
echo "== WINEDEBUG=+all, filtered =="
cd "$build_dir" || exit 1
WINEDEBUG=+all $cmd 2>&1 | grep -i -E 'spawn|exec|archive|ranlib|[^a-z]ar[^a-z]|error' | head -30
echo
echo "== raw tail =="
WINEDEBUG=+all $cmd 2>&1 | tail -5

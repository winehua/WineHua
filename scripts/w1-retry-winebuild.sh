#!/bin/bash
# W1 调试：把 llvm-mingw/bin 放进 PATH 之后，重跑那条失败的 winebuild 命令，
# 验证「裸名 clang 找不到」是不是唯一原因。
B=/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64
export PATH="$B/bin:$PATH"

echo "which clang = $(command -v clang)"
cd /data/src/winehua/build || exit 1
bash /tmp/wb_cmd.txt
echo "winebuild rc=$?"
ls -la libs/compiler-rt/aarch64-windows/libcompiler-rt.a 2>&1

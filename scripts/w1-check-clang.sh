#!/bin/bash
# W1 调试：确认 llvm-mingw 里有没有裸名 clang，以及构建 Makefile 里的 CC 是什么。
B=/data/llvm/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64

echo "== 裸名 clang/clang++ 在 llvm-mingw bin 里吗 =="
ls -la "$B/bin/clang" "$B/bin/clang++" 2>&1 | head -4

echo
echo "== 把 llvm-mingw/bin 加进 PATH 后能否找到 clang =="
PATH="$B/bin:$PATH" command -v clang || echo "clang NOT FOUND in PATH"

echo
echo "== 构建目录 Makefile 里的 CC / MINGW 相关定义 =="
grep -n -E '^(CC|CXX|CROSSCC|MINGW|TARGET_CC)' /data/src/winehua/build/wine-ohos/Makefile 2>/dev/null | head -8

echo
echo "== 我们脚本给 make 传的 CC（用于对照） =="
grep -n -E 'CC="|--with-mingw' /data/src/winehua/scripts/build_wine.sh | head -8

#!/bin/bash
# W1: sysroot-ext 里的 .pc / 脚本记录的是**构建当时的挂载点**（/workspace），
# 而我们现在把工作树挂在 /data/src/winehua。不修正会导致 pkg-config 返回不存在的路径，
# configure 探测失败（例如 GSTREAMER_LIBS 变成空 → winegstreamer.so 链接时
# 报 gst_debug_log 之类 undefined symbol）。
#
# 用法（容器内）：bash scripts/w1-fix-sysroot-prefix.sh [新前缀] [旧前缀]
set -uo pipefail

new_prefix="${1:-/data/src/winehua}"
old_prefix="${2:-/workspace}"
root="${BUILD_DIR:-/data/src/winehua/build}/sysroot-ext"

# ⚠️ 只改**文本**配置文件（.pc/.cmake）。
# 第一版脚本用 grep -rl 扫了所有文件，把 ELF 二进制里内嵌的路径字符串也改了——
# 替换前后长度不同（/workspace=10 → /data/src/winehua=18），直接把这些 .so
# **损坏**了，症状是链接时 gst_debug_log 之类符号“未定义”。
# 教训：批量 sed 必须限定扩展名，绝不能扫整棵树。
echo "== 扫描 $root 下引用 $old_prefix 的文本配置 =="
mapfile -t files < <(grep -rl --include='*.pc' --include='*.cmake' -- "$old_prefix" "$root" 2>/dev/null)
printf '%s\n' "${files[@]}" | head -20
echo "共 ${#files[@]} 个文件"

if [ "${#files[@]}" -eq 0 ]; then
    echo "无需修改。"
    exit 0
fi

for f in "${files[@]}"; do
    sed -i "s#$old_prefix#$new_prefix#g" "$f"
done
echo "已把 $old_prefix 替换为 $new_prefix"

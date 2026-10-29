#!/bin/bash
#
# 检查 docs/ 下文档里引用的代码路径是否真实存在。
#
# 正确用法（在项目根目录或任意目录执行均可）：
#   bash scripts/check-docs-paths.sh
#
# 退出码：0 = 引用的路径全部存在；1 = 有失效路径（会逐条列出）
#
# 只检查下面这些前缀开头的路径，其他路径（设备路径、Windows 路径、示例路径等）不检查：
#   entry/src/main/cpp/   entry/src/main/ets/   thirdparty/<名字>/   scripts/   automation/   smoke/   host_tests/
#
# 不检查 docs/archive/ —— 那里是历史材料，引用旧路径是正常的。
#
# 为什么需要这个脚本：2026-08-29 把 entry/src/main/cpp/ 下平铺的文件整理进子目录后，
# 文档里的路径大面积失效，靠人一处处核对不可靠。改动源码目录结构后请跑一次这个脚本。

set -eo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCS_DIR="$REPO_ROOT/docs"

if [ ! -d "$DOCS_DIR" ]; then
    echo "错误：找不到 docs 目录：$DOCS_DIR" >&2
    exit 1
fi

echo "检查目录：$DOCS_DIR"

# 文档中有意提到的历史名称或示例路径（例如"旧的 xxx 设施已删除"），列在允许列表里跳过检查
ALLOW_FILE="$REPO_ROOT/scripts/check-docs-paths.allow"
ALLOW=""
if [ -f "$ALLOW_FILE" ]; then
    ALLOW="$(grep -v '^#' "$ALLOW_FILE" | grep -v '^$' || true)"
fi

# 提取文档里引用的路径（去掉 docs/ 里属于历史材料的 archive/）
# 路径前要求是词边界，避免把 winehua_d3d11_smoke/main.c 这样的名字截尾匹配成 smoke/main.c
# grep 输出格式：<文件>:<行号>:<路径>
matches="$(grep -rnoE '\b(entry/src/main/(cpp|ets)|thirdparty/[a-z0-9_-]+|scripts|automation|smoke|host_tests)/[A-Za-z0-9_/.+-]+\.(c|h|cpp|cc|ets|ts|py|sh|json|json5|txt|xml|patch)' \
    --include='*.md' --exclude-dir=archive "$DOCS_DIR" || true)"

if [ -z "$matches" ]; then
    echo "文档里没有发现可检查的路径引用。"
    exit 0
fi

total=0
missing=0
skipped=0
checked_thirdparty=""

while IFS= read -r line; do
    file="${line%%:*}"
    rest="${line#*:}"
    lineno="${rest%%:*}"
    path="${rest#*:}"

    total=$((total + 1))

    # 允许列表里的路径跳过检查
    if [ -n "$ALLOW" ] && printf '%s\n' "$ALLOW" | grep -qxF "$path"; then
        skipped=$((skipped + 1))
        continue
    fi

    # 子模块还没初始化时跳过第三方检查，避免误报
    case "$path" in
        thirdparty/*)
            sub="${path#thirdparty/}"
            sub="${sub%%/*}"
            if [ ! -d "$REPO_ROOT/thirdparty/$sub" ]; then
                checked_thirdparty="$checked_thirdparty$sub "
                continue
            fi
            ;;
    esac

    if [ ! -e "$REPO_ROOT/$path" ]; then
        echo "失效：${file#"$REPO_ROOT"/}:$lineno 引用的 $path 不存在"
        missing=$((missing + 1))
    fi
done <<< "$matches"

echo
echo "共检查 $total 处路径引用，失效 $missing 处，跳过 $skipped 处（在允许列表中）。"

if [ -n "$checked_thirdparty" ]; then
    echo "（以下子模块未初始化，已跳过其内部路径检查：$checked_thirdparty）"
fi

if [ "$missing" -gt 0 ]; then
    exit 1
fi

echo "全部有效。"

#!/bin/bash
#
# strip-smoke.sh — main-ui 合并 master 后摘除 smoke 设施的自动化脚本
#
# 背景 (docs/SMOKE_REBUILD_20260831.md §11): smoke 是开发测试设施, 不得进入
# main-ui (面向用户的产品分支)。2026-09-01 物理隔离重构后, smoke ArkTS 全在
# entry/src/main/ets/smoke/ 整目录 (可整目录删除), 产品文件只剩带
# "// [[SMOKE]]" 标记的行级钩子 (3 个文件共 8 处)。
#
# 两种起始状态都支持:
#   - master 侧: 8 处标记钩子 + smoke 目录 → 逐个删标记块 + 删目录
#   - main-ui 侧: 钩子此前已被手工摘除, 只剩 smoke 目录残留 → 只删目录
# 不在摘除范围 (别误伤): WineEnvService.seedSmokePayload() 与 wine/smoke
# 载荷是**产品功能** (应用库内建 smoke 程序入口), 与测试设施无关。
#
# 原本靠人工三步: git rm 目录 → grep 定位标记行逐处删除 → grep 校验零引用。
# 本脚本把它变成一条命令 + 断言, 避免漏删 (漏删的后果不是编译错误而是
# 行为差异, 人工目视容易放过)。
#
# 标记块结构 (脚本按此删除):
#     // [[SMOKE]] 说明...            ← 标记行 (缩进为 I)
#     // 续行说明...                  ← 同缩进 I 的注释续行 (可选, 任意行)
#     <代码或 import>                 ← 紧跟的第一行非注释 (缩进可不同)
#
# 用法 (本仓库 core.fileMode=false → 脚本无执行位, 须用 bash 显式调用,
# 与 Makefile 里 `bash $(SCRIPTS)/xxx.sh` 的惯例一致):
#     bash scripts/strip-smoke.sh            # dry-run: 只打印将删除的内容 (默认)
#     bash scripts/strip-smoke.sh --apply    # 实际执行删除 + 校验
#
# 退出码: 0 = 成功 (apply 模式下含校验通过); 非 0 = 出错或校验失败。
# 注意: --apply 会修改工作区, 请在 git 工作区干净时运行, 便于 diff 复查与回退。

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SMOKE_DIR="entry/src/main/ets/smoke"
ETS_ROOT="entry/src/main/ets"
HOOK_FILES=(
    "entry/src/main/ets/entryability/EntryAbility.ets"
    "entry/src/main/ets/service/WineEnvService.ets"
    "entry/src/main/ets/pages/Index.ets"
)

MODE="${1:---dry-run}"
case "$MODE" in
    --dry-run) DRY=1 ;;
    --apply)   DRY=0 ;;
    *) echo "用法: $0 [--dry-run|--apply]" >&2; exit 2 ;;
esac

echo "=== strip-smoke ($([ "$DRY" = 1 ] && echo dry-run || echo apply)) ==="
echo ""

# --- 1. 前置检查 -----------------------------------------------------------
if [ ! -d "$SMOKE_DIR" ]; then
    echo "错误: $SMOKE_DIR 不存在 — 本分支可能已摘除过 smoke, 或不在预期的代码结构上" >&2
    exit 1
fi

# 只看摘除会触碰的路径: submodule 的 dirty 是构建常态 (glib/gstreamer 等被
# 构建脚本改写), 用全局 git diff 会把它们误判成"工作区不干净"而挡下正常流程。
dirty="$(git status --porcelain --ignore-submodules=all -- entry/ scripts/ docs/ 2>/dev/null \
         | grep -v '^??' || true)"
if [ "$DRY" = 0 ] && [ -n "$dirty" ]; then
    echo "错误: entry/scripts/docs 下有未提交改动, --apply 前请先提交或暂存" >&2
    echo "     (避免摘除改动与其它改动混在一起, 无法区分)" >&2
    echo "$dirty" >&2
    exit 1
fi

# 标记行总数: 为 0 说明标记格式已变 (或被摘过), 此时静默跳过是危险的。
# 只统计 3 个产品文件 — smoke/ 目录内的说明文字也含这个字面量, 不能算进去。
# `|| true`: 无匹配时 grep 返回 1, 在 set -o pipefail 下会让整个赋值非零 →
# set -e 静默退出 (本脚本"删干净后反而失败"的根源, 两处都要)。
marker_count="$(grep -ho '// \[\[SMOKE\]\]' "${HOOK_FILES[@]}" | wc -l | tr -d ' ' || true)"
echo "标记行: $marker_count 处 (master 基线为 8)"
if [ "$marker_count" -eq 0 ]; then
    # 两种可能: (a) 此前已手工摘除钩子, 只剩 smoke 目录残留 (main-ui 现状);
    # (b) 标记格式变了 (脚本会漏删). 用"产品文件是否仍有 smoke 符号引用"
    # 区分 — 引用还在就说明钩子没被摘干净, 此时删目录会直接编译失败。
    pre_leftover="$(grep -rn 'SmokeHook\|SmokeDevPanel\|SmokeRunner\|SmokeTypes' "$ETS_ROOT" 2>/dev/null \
                    | grep -v "/smoke/" | grep -vE ':[0-9]+:[[:space:]]*//' || true)"
    if [ -n "$pre_leftover" ]; then
        echo "错误: 无标记行但产品文件仍有 smoke 引用 — 标记格式可能已变, 拒绝执行" >&2
        echo "$pre_leftover" >&2
        exit 1
    fi
    echo "说明: 产品文件已无标记也无引用 (此前已摘除钩子), 本次只删除 smoke 目录"
fi
echo ""

# --- 2. 删除标记块 ---------------------------------------------------------
# awk: 命中标记行 → 记录缩进 → 吞掉同缩进的 // 续行 → 吞掉第一行非注释 → 恢复
strip_file() {
    local file="$1" tmp
    tmp="$(mktemp)"
    awk -v dry="$DRY" -v fname="$file" '
    function report(txt) { if (dry) printf("    DEL %s:%d |%s\n", fname, NR, txt) > "/dev/stderr" }
    BEGIN { skip = 0; indent = ""; blocks = 0 }
    {
        if (skip == 0) {
            if ($0 ~ /^[ \t]*\/\/ \[\[SMOKE\]\]/) {
                match($0, /^[ \t]*/);
                indent = substr($0, 1, RLENGTH);
                skip = 1; blocks++;
                report($0);
                next;
            }
            print; next;
        }
        if (index($0, indent "//") == 1) { report($0); next; }  # 同缩进注释续行
        skip = 0; report($0); next;                            # 紧跟的一行代码
    }
    ' "$file" > "$tmp"

    if [ "$DRY" = 0 ]; then
        mv "$tmp" "$file"
    else
        rm -f "$tmp"
    fi
}

for f in "${HOOK_FILES[@]}"; do
    [ -f "$f" ] || { echo "错误: 缺少文件 $f" >&2; exit 1; }
    strip_file "$f"
done

# --- 3. 删除 smoke 目录 ----------------------------------------------------
if [ "$DRY" = 0 ]; then
    git rm -rq "$SMOKE_DIR"
    echo "已删除目录: $SMOKE_DIR"
else
    echo "将删除目录: $SMOKE_DIR ($(find "$SMOKE_DIR" -name '*.ets' | wc -l | tr -d ' ') 个 .ets)"
fi
echo ""

# --- 4. 校验 (仅 apply) ----------------------------------------------------
if [ "$DRY" = 1 ]; then
    echo "dry-run 结束 — 确认无误后用 --apply 执行"
    exit 0
fi

# 4a. 目录已消失
# 注意: 不能写成 `[ -d X ] && { ...; exit 1; }` — 条件为假时整行返回非零,
# set -e 会当场退出, 校验段后续根本不执行 (演练实测踩到)。
if [ -d "$SMOKE_DIR" ]; then
    echo "错误: $SMOKE_DIR 仍存在" >&2
    exit 1
fi

# 4b. 无残留代码引用 (注释行不计 — 历史提及无害)
leftover="$(grep -rn 'SmokeHook\|SmokeDevPanel\|SmokeRunner\|SmokeTypes' "$ETS_ROOT" 2>/dev/null \
            | grep -vE ':[0-9]+:[[:space:]]*//' || true)"
if [ -n "$leftover" ]; then
    echo "错误: 仍有非注释级 smoke 引用:" >&2
    echo "$leftover" >&2
    exit 1
fi

# 4c. 标记行已清零 (grep 零匹配返回 1, 见上文 `|| true` 说明)
left_markers="$(grep -rc '\[\[SMOKE\]\]' "$ETS_ROOT" --include='*.ets' 2>/dev/null \
                | awk -F: '{s+=$2} END {print s+0}' || true)"
if [ "$left_markers" -ne 0 ]; then
    echo "错误: 仍有 $left_markers 处标记行未清除" >&2
    exit 1
fi

echo "校验通过: smoke 目录已删除、标记行清零、无代码级引用残留"
echo ""
echo "后续: 复查 git diff → 提交 (建议信息: \"chore(main-ui): 摘除 smoke 测试设施\")"

#!/bin/bash
# w1-m5b-replay-retry.sh — 重放第一轮失败/跳过的 WineHua 提交。
#
# 第一轮失败原因基本都是机械性的（autom4te.cache 属主、上一次失败留下的
# 未提交改动/未跟踪文件、以及“补丁已等价应用 → empty commit”），不是语义冲突。
# 本脚本逐条清理现场后重试。
#
# 用法：bash scripts/w1-m5b-replay-retry.sh <commit-list-file>
set -uo pipefail

REPO="${REPO:-/home/liufeng/src/WineHua-proton-ohos/thirdparty/wine-valve}"
OUT="${OUT:-/home/liufeng/src/WineHua-proton-ohos/replay-out}"
BRANCH="${BRANCH:-ohos-port}"
LIST="$1"

cd "$REPO"
[ "$(git branch --show-current)" = "$BRANCH" ] || { echo "FATAL: not on $BRANCH"; exit 2; }

ok=0; skipped=0; conf=0; failed=0
while read -r c; do
    [ -z "$c" ] && continue
    subject=$(git log -1 --format='%h %s' "$c")

    # 清理上一次失败留下的现场（只清工作区，不碰已提交内容）
    git cherry-pick --abort 2>/dev/null
    git checkout -q -- . 2>/dev/null
    git clean -fdq 2>/dev/null

    if git cherry-pick -x -X theirs "$c" > "$OUT/retry-$c.log" 2>&1; then
        ok=$((ok+1)); echo "OK   $subject" >> "$OUT/replay-retry.log"
        continue
    fi

    if grep -q "now empty" "$OUT/retry-$c.log"; then
        git cherry-pick --skip >/dev/null 2>&1
        skipped=$((skipped+1)); echo "SKIP $subject (already applied)" >> "$OUT/replay-retry.log"
        continue
    fi

    files=$(git diff --name-only --diff-filter=U | tr '\n' ' ')
    if [ -n "$files" ]; then
        for f in $files; do echo "$c $f" >> "$OUT/replay-conflicts.txt"; done
        git checkout --theirs -- $files 2>/dev/null
        git add -A -- $files 2>/dev/null
        git add -A
        if git -c core.editor=true cherry-pick --continue >/dev/null 2>&1; then
            conf=$((conf+1)); echo "CONF $subject :: $files" >> "$OUT/replay-retry.log"
        else
            git cherry-pick --abort 2>/dev/null
            failed=$((failed+1)); echo "FAIL $subject" >> "$OUT/replay-retry.log"
        fi
        continue
    fi

    git cherry-pick --abort 2>/dev/null
    failed=$((failed+1)); echo "FAIL $subject :: $(head -3 "$OUT/retry-$c.log" | tr '\n' ' ')" >> "$OUT/replay-retry.log"
done < "$LIST"

echo "== retry finished: ok=$ok skipped=$skipped conflicted=$conf failed=$failed =="

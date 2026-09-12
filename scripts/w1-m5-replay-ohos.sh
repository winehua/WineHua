#!/bin/bash
# w1-m5-replay-ohos.sh — 把 WineHua 在 Wine 上的自有提交（OHOS 平台层）重放到
# Valve Proton Wine 基线上（计划 W1 的“逐个提交重放”）。
#
# 输入：thirdparty/wine-valve（Valve dc26e618 + W1 构建修复），
#       refs/remotes/winehua/feature/arm64-heaven-port（WineHua 完整历史）。
# 输出：分支 ohos-port，每个 WineHua 提交对应一次 cherry-pick。
#
# 用法（容器内或宿主 WSL，工作目录 = wine-valve）：
#   bash /path/to/w1-m5-replay-ohos.sh
#
# 冲突策略：默认用 git 的 -X theirs（即保留 WineHua 侧补丁），
# 剩下的硬冲突（add/add、modify/delete）按“取 WineHua 侧”批量落，
# 全部记录到 $OUT/replay-conflicts.txt，供后续逐个复核。
set -uo pipefail

REPO="${REPO:-/data/src/winehua/thirdparty/wine-valve}"
OUT="${OUT:-/data/src/winehua/build/replay}"
BASE_BRANCH="${BASE_BRANCH:-ohos-port-base}"
PORT_BRANCH="${PORT_BRANCH:-ohos-port}"
COMMITS="${COMMITS:-/data/src/winehua/build/winehua-commits.txt}"

mkdir -p "$OUT"
cd "$REPO"

echo "== repo=$REPO branch=$(git branch --show-current) =="
: > "$OUT/replay.log"
: > "$OUT/replay-conflicts.txt"

git rev-parse --verify -q "refs/heads/$PORT_BRANCH" >/dev/null && {
    echo "FATAL: branch $PORT_BRANCH already exists"; exit 2;
}

git branch -f "$BASE_BRANCH" HEAD
git checkout -q -b "$PORT_BRANCH"
echo "port branch created from $(git rev-parse --short HEAD)"

n=0; ok=0; conflicted=0; failed=0
while read -r c; do
    [ -z "$c" ] && continue
    n=$((n+1))
    subject=$(git log -1 --format='%h %s' "$c")
    if git cherry-pick -x -X theirs "$c" > "$OUT/cp-$c.log" 2>&1; then
        ok=$((ok+1))
        echo "OK   $subject" >> "$OUT/replay.log"
    else
        files=$(git diff --name-only --diff-filter=U | tr '\n' ' ')
        if [ -n "$files" ]; then
            conflicted=$((conflicted+1))
            echo "CONF $subject  :: $files" >> "$OUT/replay.log"
            for f in $files; do echo "$c $f" >> "$OUT/replay-conflicts.txt"; done
            # 硬冲突（add/add、modify/delete）批量取 WineHua 侧
            git checkout --theirs -- $files 2>/dev/null
            git add -A -- $files 2>/dev/null
            if git diff --name-only --diff-filter=U | grep -q .; then
                git add -A
            fi
            git -c core.editor=true cherry-pick --continue >/dev/null 2>&1 \
                || { git add -A; git -c core.editor=true cherry-pick --continue >/dev/null 2>&1; }
            if git rev-parse -q --verify CHERRY_PICK_HEAD >/dev/null; then
                failed=$((failed+1))
                echo "FAIL $subject (left unmerged)" >> "$OUT/replay.log"
                git cherry-pick --abort 2>/dev/null
            fi
        else
            failed=$((failed+1))
            echo "FAIL $subject (no conflict files; aborted)" >> "$OUT/replay.log"
            git cherry-pick --abort 2>/dev/null
        fi
    fi
    echo "--- progress $n: ok=$ok conf=$conflicted fail=$failed"
done < "$COMMITS"

echo "== replay finished: total=$n ok=$ok conflicted=$conflicted failed=$failed =="
echo "== log: $OUT/replay.log =="

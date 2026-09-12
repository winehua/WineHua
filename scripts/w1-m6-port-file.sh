#!/bin/bash
# w1-m6-port-file.sh — 单文件精确移植：把 WineHua 在同名文件上的**自有增量**
# 三方式合并到 Valve Proton 基线上。
#
# 为什么需要它：整提交 cherry-pick 会把主库里"上游 11.10 的改动"一起带过来，
# 在 Valve 11.0 的文件里顶掉 Proton 自己的内容（unixlib 入口表、signal/server
# 的函数签名等）。这里改用：
#     base   = 主库第一个人工提交**之前**的同名文件（= 纯上游版本）
#     ours   = 主库最终版本（上游 11.10 + WineHua 增量）
#     theirs = Valve proton_11.0 基线
# 三方式合并的结果 = Valve 文件 + 仅 WineHua 增量。
#
# 用法：bash scripts/w1-m6-port-file.sh <相对路径> [更多路径...]
# 冲突不会被自动吞掉：留下冲突标记并打印文件，交人工处理。
set -uo pipefail

REPO="${REPO:-/home/liufeng/src/WineHua-proton-ohos/thirdparty/wine-valve}"
VALVE_REF="${VALVE_REF:-50f18dced76}"
FORK_REF="${FORK_REF:-refs/remotes/winehua/feature/arm64-heaven-port}"
TMP="${TMP:-/tmp/w1-port}"
mkdir -p "$TMP"

cd "$REPO"
conflicted=()
done_ok=()
skipped=()

for f in "$@"; do
    # 只看 WineHua 作者署名的提交，找最早触碰该文件的那一个
    first=$(git log --reverse --format=%H --author=hackeris --author=yifengling0 \
                 "$FORK_REF" -- "$f" | head -1)
    if [ -z "$first" ]; then skipped+=("$f (no winehua commit)"); continue; fi
    if ! git cat-file -e "$first^:$f" 2>/dev/null; then
        skipped+=("$f (no pre-winehua version)"); continue
    fi
    git cat-file -e "$FORK_REF:$f" 2>/dev/null || { skipped+=("$f (absent in fork head)"); continue; }
    git cat-file -e "$VALVE_REF:$f" 2>/dev/null || { skipped+=("$f (absent in valve base)"); continue; }

    git show "$first^:$f" > "$TMP/base"
    git show "$FORK_REF:$f" > "$TMP/result"
    git show "$VALVE_REF:$f" > "$TMP/theirs"

    if git merge-file -L "winehua+11.10" -L "upstream-base" -L "valve-proton" \
           "$TMP/result" "$TMP/base" "$TMP/theirs"; then
        cp "$TMP/result" "$f"
        done_ok+=("$f")
    else
        cp "$TMP/result" "$f"
        conflicted+=("$f")
    fi
done

echo "== merged clean: ${#done_ok[@]} =="
printf '   %s\n' "${done_ok[@]}"
[ "${#conflicted[@]}" -gt 0 ] && { echo "== left with conflict markers: ${#conflicted[@]} =="; printf '   %s\n' "${conflicted[@]}"; }
[ "${#skipped[@]}" -gt 0 ] && { echo "== skipped: ${#skipped[@]} =="; printf '   %s\n' "${skipped[@]}"; }

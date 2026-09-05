#!/bin/bash
# 检查 submodule 状态：HEAD vs remote default branch
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
failed=0

echo "=== Submodule 状态检查 ==="
echo ""

while IFS= read -r path; do
    sm="${path##*/}"
    HEAD=$(git ls-tree HEAD "$path" | awk '{print $3}')
    
    # 获取 default branch
    branch=$(git config -f .gitmodules --get "submodule.$path.branch" 2>/dev/null || echo "master")
    
    # 获取 remote HEAD
    cd "$path"
    remote_url=$(git remote get-url origin)
    remote_sha=$(git ls-remote origin "refs/heads/$branch" 2>/dev/null | awk '{print $1}')
    
    echo "--- $sm ($branch) ---"
    echo "  tracked: $HEAD"
    echo "  remote:  $remote_sha"
    
    if git fetch --dry-run --no-tags origin "$HEAD" >/dev/null 2>&1; then
        echo "  CI can fetch the exact tracked gitlink"
    else
        echo "  ERROR: remote cannot fetch the exact tracked gitlink"
        failed=1
    fi

    if [ "$HEAD" = "$remote_sha" ]; then
        echo "  ✅ 与 remote 一致"
    else
        # 不依赖 git branch -r --contains: 那是本地 remote-tracking 缓存,
        # 有陈旧/假阳性问题 (曾对已 fetch 的 gitlink 误报"未推送")。
        # 权威判据 = 上方的 fetch --dry-run (模拟 CI 精确 SHA fetch)。
        # 到这里说明 HEAD 可达 remote 但不在 $branch 尖端。
        echo "  ⚠️  tracked commit 可达 remote (fetch 通过), 但不在 $branch 尖端"
    fi
    echo ""
    cd "$ROOT"
done < <(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}')

exit "$failed"
